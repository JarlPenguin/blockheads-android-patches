/*
 * libdpifix.so
 *
 * THE BUG
 * -------
 * Apportable emulates a fixed iPhone screen: -[UIScreen preferredMode]
 * asks for +[UIScreenMode emulatedMode:12], which forwards to
 * +[UINativeScreenMode nativeMode:] - and that ignores its argument and
 * computes scale as nativeWidth/600, yielding an iPhone 6 Plus profile
 * (414x736 @3.0) on every device. UIScreen.bounds is mode.size/mode.scale
 * and PIXEL_SCALE is a cached copy of UIScreen.scale taken in
 * -[EAGLView initWithCoder:], so overriding the two mode accessors fixes
 * the whole chain: bounds, framebuffer size, layout and touch mapping.
 *
 * THE FIX
 * -------
 * Overrides -[UIScreenMode scale] and -[UIScreenMode size], as well as
 * the overrides on UINativeScreenMode, with lazily-computed geometry once
 * the GL surface dimensions become available via VerdePluginNativeWidth()
 * and VerdePluginNativeHeight().
 *
 * Scale is computed from getenv("DPIFIX_SWDP") (Android dp mapped directly
 * to iOS points, sharing the 160 dpi baseline). Phones (when getenv("TABLET")
 * != "1") are capped at a 414-point width to prevent crossing the game's
 * internal 415-point tablet layout threshold. The scale is clamped to
 * [1.0, 4.0]. Geometry is reported portrait-canonically (short edge as width).
 * Both getters fall back to the original implementation if geometry is not
 * yet available.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "dpifix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libdpifix.so dpifix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#ifndef DPIFIX_VERBOSE
#define DPIFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef DPIFIX_STRICT_ENCODING
#define DPIFIX_STRICT_ENCODING 1
#endif

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
typedef void *Ivar;
typedef void *Class;

/* --- ObjC runtime, resolved from libSystem.so at load time --- */
static void      *g_sys;      /* libSystem.so      */
static void      *g_app;      /* libApplication.so - may be NULL */

static SEL        (*selReg)(const char *);
static Class      (*getClass)(const char *);
static Method     (*getInstMethod)(Class, SEL);
static IMP        (*setImp)(Method, IMP);
static IMP        (*getImp)(Method);
static void       *g_msgSend;

/* Optional: present in libobjc2, absent in some older Apportable drops.
   Resolved best-effort; the helpers below degrade rather than fail. */
static Ivar        (*getIvar)(Class, const char *);
static ptrdiff_t   (*ivarOff)(Ivar);
static const char *(*getTypeEnc)(Method);

/* objc_msgSend is declared void * and cast at each call site. Never
   declare it varargs: AAPCS passes floats and small structs differently
   to variadic functions, so a varargs declaration is a live trap even
   though the casts currently mask it. */
#define MSG_id(o,s)      (((id           (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_v(o,s)       (((void         (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_c(o,s)       (((char         (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_d(o,s)       (((double       (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_str(o,s)     (((const char *(*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_id_id(o,s,a) (((id           (*)(id,SEL,id))g_msgSend)((o),(s),(a)))
#define MSG_c_id(o,s,a)  (((char         (*)(id,SEL,id))g_msgSend)((o),(s),(a)))

/* Freestanding: avoids pulling <string.h> in for one comparison. */
static int strEq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Logs the runtime's encoding for a method and, when strict, refuses to
   proceed on a mismatch. Pass expected = NULL to log without checking
   (use only while measuring a new encoding). If method_getTypeEncoding
   is unavailable the check is skipped and that fact is logged rather
   than silently passing. */
static int checkEncoding(Method m, const char *sel, const char *expected) {
    const char *enc;

    if (!expected) {
        if (getTypeEnc) LOGI("%s: encoding = %s", sel, getTypeEnc(m));
        return 1;
    }
    if (!getTypeEnc) {
        LOGI("%s: encoding unavailable (method_getTypeEncoding missing)", sel);
        return 1;
    }

    enc = getTypeEnc(m);
    LOGI("%s: encoding = %s (expected %s)", sel, enc ? enc : "(null)", expected);
    if (strEq(enc, expected)) return 1;

#if DPIFIX_STRICT_ENCODING
    LOGE("%s: encoding mismatch - refusing to install", sel);
    return 0;
#else
    LOGE("%s: encoding mismatch - installing anyway (strict check disabled)", sel);
    return 1;
#endif
}

/* Replace one method. Verifies the encoding first, then captures the
   displaced IMP into *orig.

   Pass orig = NULL only for a terminal hook that never chains. Note that
   a terminal hook does not compose: if another library later hooks the
   same selector, whichever installs first is silently discarded, and
   nothing here detects it - the class's pristine IMP is not available at
   install time to compare against. If you need that, log `cur`
   unconditionally at INFO and read it out of logcat. */
static int hook(Class cls, const char *sel, const char *expectedEnc,
                IMP repl, IMP *orig) {
    Method m = getInstMethod(cls, selReg(sel));
    IMP    cur;

    if (!m) { LOGE("method not found: %s", sel); return 0; }
    if (!checkEncoding(m, sel, expectedEnc)) return 0;

    cur = getImp(m);
    if (cur == repl) { LOGV("%s already hooked by us", sel); return 1; }

    if (orig) *orig = cur;
    setImp(m, repl);
    return 1;
}

/* Resolve an ivar offset, most authoritative source first.

     1. class_getInstanceVariable + ivar_getOffset. Reads the post-fixup
        value, so it is correct even where libobjc2 rewrote a non-fragile
        offset at load time, and does not depend on .dynsym export.
     2. dlsym(libApplication, "OBJC_IVAR_$_<Class>.<name>"). Works for
        the subset that is exported; GameView.gdprPrompt is in .symtab
        but not .dynsym, and CPTexture2D exports nothing, which is why
        tier 1 has to come first.
     3. The literal read out of IDA. Pass 0 for none.

   *how receives a static string naming the tier that answered, for the
   install() log line. Returns 0 only if every tier failed. */
static int resolve_ivar(const char *clsname, const char *ivname,
                        uint32_t *out, uint32_t fallback,
                        const char **how) {
    uint32_t resolved = 0;
    Class    c;

    *how = "none";

    if (getIvar && ivarOff) {
        c = getClass(clsname);
        if (c) {
            Ivar iv = getIvar(c, ivname);
            if (iv) {
                ptrdiff_t o = ivarOff(iv);
                if (o > 0 && o < 0x10000) { resolved = (uint32_t)o; *how = "runtime"; }
                else LOGE("ivar_getOffset(%s.%s) returned implausible %ld",
                          clsname, ivname, (long)o);
            } else LOGE("class_getInstanceVariable(%s, %s) failed", clsname, ivname);
        } else LOGE("class %s not found for ivar %s", clsname, ivname);
    }

    if (!resolved && g_app) {
        char sym[192];
        int  n = 0;
        const char *p;
        for (p = "OBJC_IVAR_$_"; *p && n < (int)sizeof(sym) - 1; p++) sym[n++] = *p;
        for (p = clsname;        *p && n < (int)sizeof(sym) - 1; p++) sym[n++] = *p;
        if (n < (int)sizeof(sym) - 1) sym[n++] = '.';
        for (p = ivname;         *p && n < (int)sizeof(sym) - 1; p++) sym[n++] = *p;
        sym[n] = '\0';

        {
            uint32_t *off = (uint32_t *)dlsym(g_app, sym);
            if (off) { resolved = *off; *how = "dlsym"; }
            else LOGV("%s not exported", sym);
        }
    }

    if (!resolved && fallback) {
        resolved = fallback;
        *how = "fallback";
        LOGE("%s.%s: falling back to static-analysis offset 0x%x",
             clsname, ivname, fallback);
    }

    if (!resolved) {
        LOGE("%s.%s: unresolvable", clsname, ivname);
        return 0;
    }

    /* A disagreement means the binary is not the one that was analysed.
       Loud, but not fatal: the runtime value is the correct one. */
    if (fallback && resolved != fallback)
        LOGE("%s.%s is 0x%x via %s, but static analysis said 0x%x"
             " - the binary differs from the one analysed",
             clsname, ivname, resolved, *how, fallback);

    *out = resolved;
    return 1;
}

/* =====================================================================
 * per-file content
 * ===================================================================== */

/* TODO: measure -[UIScreenMode scale] with method_getTypeEncoding on-device */
#define ENC_MODE_SCALE NULL

/* TODO: measure -[UIScreenMode size] with method_getTypeEncoding on-device */
#define ENC_MODE_SIZE  NULL

typedef struct { float w, h; } CGSize32;

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
   Both classes' getters are plain ivar reads of the same two ivars, so a
   single pair of replacements serves both and the parent's IMP is a valid
   fallback for either receiver. The subclass originals captured during
   install are used only for rollback, never for chaining. */

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

static int install(void) {
    Class  mode, nmode;
    Method mScale, mSize, mNScale, mNSize;
    IMP    origNScale, origNSize;

    /* 1 */
    if (!g_app) { LOGE("libApplication not loaded"); return 0; }

    nativeWidth  = (int (*)(void))dlsym(g_app, "VerdePluginNativeWidth");
    nativeHeight = (int (*)(void))dlsym(g_app, "VerdePluginNativeHeight");
    if (!nativeWidth || !nativeHeight) {
        LOGE("VerdePluginNative* not found - leaving stock profile");
        return 0;
    }

    mode = getClass("UIScreenMode");
    if (!mode) { LOGE("UIScreenMode not found - load point too early"); return 0; }

    /* UINativeScreenMode declares its own size/scale, so hooking the parent
       alone misses the instance preferredMode actually returns. The class is
       always present on this build - preferredMode -> emulatedMode:12 ->
       nativeMode: constructs one unconditionally - so absence means the
       binary is not the one analysed. Log and continue: the parent hooks
       may still cover whatever mode is in use, and failing closed here
       would disable the fix on a binary that might not need it. */
    nmode = getClass("UINativeScreenMode");
    if (!nmode) LOGE("UINativeScreenMode not found");

    /* 2 - skipped: no MSG_* usage */

    /* 3 */
    mScale = getInstMethod(mode, selReg("scale"));
    if (!mScale) { LOGE("UIScreenMode scale not found"); return 0; }
    if (getImp(mScale) == (IMP)my_mode_scale) { LOGI("already installed"); return 1; }

    mSize = getInstMethod(mode, selReg("size"));
    if (!mSize) { LOGE("UIScreenMode size not found"); return 0; }

    mNScale = nmode ? getInstMethod(nmode, selReg("scale")) : NULL;
    mNSize  = nmode ? getInstMethod(nmode, selReg("size"))  : NULL;

    /* 4 - skipped: no ivars resolved */

    /* 5 - skipped: no runtime selectors or constants to cache */

    /* 6 */
    if (!checkEncoding(mScale, "scale", ENC_MODE_SCALE)) return 0;
    if (!checkEncoding(mSize,  "size",  ENC_MODE_SIZE))  return 0;
    if (mNScale && !checkEncoding(mNScale, "scale", ENC_MODE_SCALE)) return 0;
    if (mNSize  && !checkEncoding(mNSize,  "size",  ENC_MODE_SIZE))  return 0;

    /* 7 - commit, rolling back already-committed hooks if a later one fails */
    if (!hook(mode, "scale", ENC_MODE_SCALE, (IMP)my_mode_scale, &g_origModeScale))
        return 0;

    if (!hook(mode, "size", ENC_MODE_SIZE, (IMP)my_mode_size, &g_origModeSize)) {
        setImp(mScale, g_origModeScale);
        return 0;
    }

    if (nmode) {
        origNScale = NULL;
        origNSize  = NULL;

        if (mNScale && !hook(nmode, "scale", ENC_MODE_SCALE, (IMP)my_mode_scale, &origNScale)) {
            setImp(mScale, g_origModeScale);
            setImp(mSize,  g_origModeSize);
            return 0;
        }

        if (mNSize && !hook(nmode, "size", ENC_MODE_SIZE, (IMP)my_mode_size, &origNSize)) {
            setImp(mScale, g_origModeScale);
            setImp(mSize,  g_origModeSize);
            if (mNScale && origNScale) setImp(mNScale, origNScale);
            return 0;
        }
    }

    LOGI("installed");
    return 1;
}

__attribute__((constructor))
static void init(void) {
    static int once = 0;
    if (__sync_val_compare_and_swap(&once, 0, 1) != 0) return;

    /* Loaded from BackgroundLibraryLoader$1.run(), after
       LibraryManager.loadLibraries(). RTLD_NOLOAD turns a wrong load
       point into a clean error instead of a second ObjC runtime. */
    g_sys = dlopen("libSystem.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_sys) { LOGE("libSystem not loaded - load point too early"); return; }

    /* Optional: only the tier-2 ivar lookup and exported-global patches
       need it. Absence is not fatal. */
    g_app = dlopen("libApplication.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_app) LOGV("libApplication not loaded - dlsym ivar tier unavailable");

    selReg        = dlsym(g_sys, "sel_registerName");
    getClass      = dlsym(g_sys, "objc_getClass");
    getInstMethod = dlsym(g_sys, "class_getInstanceMethod");
    setImp        = dlsym(g_sys, "method_setImplementation");
    getImp        = dlsym(g_sys, "method_getImplementation");
    g_msgSend     = dlsym(g_sys, "objc_msgSend");

    getIvar       = dlsym(g_sys, "class_getInstanceVariable");   /* optional */
    ivarOff       = dlsym(g_sys, "ivar_getOffset");              /* optional */
    getTypeEnc    = dlsym(g_sys, "method_getTypeEncoding");      /* optional */

    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp) {
        LOGE("runtime symbols missing"); return;
    }
    if (!getIvar || !ivarOff)
        LOGE("ivar introspection unavailable - will use exported/static offsets");
    if (!getTypeEnc)
        LOGE("method_getTypeEncoding unavailable - encoding checks skipped");

    install();
}
