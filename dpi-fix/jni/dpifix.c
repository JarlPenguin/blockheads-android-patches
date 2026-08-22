#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <android/log.h>

/* Set to 1 to re-enable per-call diagnostics. */
#define DPIFIX_VERBOSE 0

#define TAG "DPIFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if DPIFIX_VERBOSE
#define LOGV(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#else
#define LOGV(...) ((void)0)
#endif

typedef void *id;
typedef void *SEL;
typedef void *Method;
typedef void *IMP;

typedef struct { float w, h; } CGSize32;

/* --- ObjC runtime, resolved from libSystem.so at load time --- */
static void   *g_sys, *g_app;
static SEL    (*selReg)(const char *);
static id     (*getClass)(const char *);
static Method (*getInstMethod)(id, SEL);
static IMP    (*setImp)(Method, IMP);
static IMP    (*getImp)(Method);

/* Apportable's live framebuffer dimensions; both return 0 until the GL
   surface exists, which is why geometry is computed lazily below. */
static int (*nativeWidth)(void);
static int (*nativeHeight)(void);

/* ---- geometry ----
   Apportable emulates a fixed iPhone screen: -[UIScreen preferredMode]
   asks for +[UIScreenMode emulatedMode:12], which forwards to
   +[UINativeScreenMode nativeMode:] - and that ignores its argument and
   computes scale as nativeWidth/600, yielding an iPhone 6 Plus profile
   (414x736 @3.0) on every device. UIScreen.bounds is mode.size/mode.scale
   and PIXEL_SCALE is a cached copy of UIScreen.scale taken in
   -[EAGLView initWithCoder:], so overriding the two mode accessors fixes
   the whole chain: bounds, framebuffer size, layout and touch mapping. */

static float g_scale = 0.0f;
static int   g_pxW = 0, g_pxH = 0;   /* portrait-canonical pixels */

static int ensure_geometry(void) {
    if (g_scale > 0.0f) return 1;
    if (!nativeWidth || !nativeHeight) return 0;

    int w = nativeWidth(), h = nativeHeight();
    if (w <= 0 || h <= 0) return 0;      /* surface not up yet; retry later */

    /* UIScreen is portrait-fixed on iOS - UIKit rotates the window on top
       of it - so the short edge is always reported as the width. */
    g_pxW = (w < h) ? w : h;
    g_pxH = (w < h) ? h : w;

    const char *sw = getenv("DPIFIX_SWDP");
    int swdp = sw ? atoi(sw) : 0;
    if (swdp <= 0) { LOGE("DPIFIX_SWDP unset/bad - leaving stock profile"); return 0; }

    /* Android dp and iOS points share the same 160-per-inch reference, so
       mapping dp straight to points gives physically correct sizing. */
    g_scale = (float)g_pxW / (float)swdp;

    /* The game branches on a hardcoded 415-point width to choose phone vs
       tablet layout (inventory placement, D-pad, carousel spacing). dp runs
       a little wider than iOS points, so a large phone can cross the line
       and get tablet layout in portrait. Pin those to the widest phone
       profile the game was authored for. */
    const char *tab = getenv("TABLET");
    int isTablet = tab && *tab == '1';
    if (!isTablet) {
        const float PHONE_MAX_PT = 414.0f;      /* iPhone 6 Plus */
        if ((float)g_pxW / g_scale > PHONE_MAX_PT)
            g_scale = (float)g_pxW / PHONE_MAX_PT;
    }

    if (g_scale < 1.0f) g_scale = 1.0f;
    if (g_scale > 4.0f) g_scale = 4.0f;

    LOGI("native %dx%d swdp=%d tablet=%s -> scale %.3f (points %.1fx%.1f)",
         w, h, swdp, isTablet ? "yes" : "no",
         g_scale, g_pxW / g_scale, g_pxH / g_scale);
    return 1;
}

/* ---- UIScreenMode / UINativeScreenMode leaf overrides ----
   Both classes' getters are plain ivar reads, so the parent's IMP is a
   valid fallback for either receiver. It is only reached when
   ensure_geometry() fails, in which case the stock profile applies. */

static IMP g_origModeScale = 0, g_origModeSize = 0;

static uint32_t my_mode_scale(id self, SEL _cmd) {
    if (ensure_geometry()) { uint32_t b; memcpy(&b, &g_scale, 4); return b; }
    return ((uint32_t (*)(id, SEL))g_origModeScale)(self, _cmd);
}

static void my_mode_size(CGSize32 *ret, id self, SEL _cmd) {
    ((void (*)(CGSize32 *, id, SEL))g_origModeSize)(ret, self, _cmd);
    if (ensure_geometry()) {
        LOGV("mode size %.0fx%.0f -> %dx%d", ret->w, ret->h, g_pxW, g_pxH);
        ret->w = (float)g_pxW;
        ret->h = (float)g_pxH;
    }
}

static int hook(id cls, const char *sel, IMP repl, IMP *orig) {
    Method m = getInstMethod(cls, selReg(sel));
    if (!m) { LOGE("method not found: %s", sel); return 0; }
    if (getImp(m) == repl) return 1;                 /* already installed */
    if (orig) *orig = getImp(m);
    setImp(m, repl);
    return 1;
}

__attribute__((constructor))
static void init(void) {
    static int once = 0;
    if (__sync_val_compare_and_swap(&once, 0, 1) != 0) return;

    /* Loaded from BackgroundLibraryLoader$1.run(), after
       LibraryManager.loadLibraries(). RTLD_NOLOAD turns a wrong load point
       into a clean error instead of a second ObjC runtime. */
    g_sys = dlopen("libSystem.so", RTLD_NOW | RTLD_NOLOAD);
    g_app = dlopen("libApplication.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_sys) { LOGE("libSystem not loaded - load point too early"); return; }
    if (!g_app) { LOGE("libApplication not loaded"); return; }

    selReg        = dlsym(g_sys, "sel_registerName");
    getClass      = dlsym(g_sys, "objc_getClass");
    getInstMethod = dlsym(g_sys, "class_getInstanceMethod");
    setImp        = dlsym(g_sys, "method_setImplementation");
    getImp        = dlsym(g_sys, "method_getImplementation");
    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp) {
        LOGE("runtime symbols missing"); return;
    }

    /* Not exported from the global scope; they live in libApplication. */
    nativeWidth  = dlsym(g_app, "VerdePluginNativeWidth");
    nativeHeight = dlsym(g_app, "VerdePluginNativeHeight");
    if (!nativeWidth || !nativeHeight) {
        LOGE("VerdePluginNative* not found - leaving stock profile"); return;
    }

    id mode = getClass("UIScreenMode");
    if (!mode) { LOGE("UIScreenMode not found - load point too early"); return; }
    if (!hook(mode, "scale", (IMP)my_mode_scale, &g_origModeScale)) return;
    if (!hook(mode, "size",  (IMP)my_mode_size,  &g_origModeSize))  return;

    /* UINativeScreenMode overrides both getters, so hooking the parent
       alone misses the instance actually in use. */
    id nmode = getClass("UINativeScreenMode");
    if (nmode) {
        Method ms = getInstMethod(nmode, selReg("size"));
        Method mc = getInstMethod(nmode, selReg("scale"));
        if (ms && getImp(ms) != (IMP)my_mode_size)  hook(nmode, "size",  (IMP)my_mode_size,  0);
        if (mc && getImp(mc) != (IMP)my_mode_scale) hook(nmode, "scale", (IMP)my_mode_scale, 0);
    } else LOGE("UINativeScreenMode not found");

    LOGI("installed");
}
