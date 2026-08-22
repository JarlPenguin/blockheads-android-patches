/*
 * libpaintmixfix.so
 *
 * Fixes black textures in the Easel paint-mixing UI (PaintMixUI).
 *
 * Root cause: returning from the photo picker tears down the EGL surface,
 * which unbinds the context (eglMakeCurrent with EGL_NO_SURFACE). The game
 * constructs PaintMixUI in response to the picked image, and
 * -[PaintMixUI setWorkbench:blockhead:craftableItemObject:] calls
 * loadResources + updateMix while eglGetCurrentContext() is EGL_NO_CONTEXT.
 * glGenTextures then silently yields 0, and neither CPTexture2D initialiser
 * checks it, so the panel keeps texture objects with dead GL names for the
 * rest of the session. The context recovers ~500ms later on its own.
 *
 * Affected: backgroundTexture (paintMixBackground.png), arrowTexture
 * (convertArrow8.png), paintingRawTexture and paintingOutputTexture (built
 * from the picked photo). itemsTexture survives because Items.png is a cache
 * hit from world load.
 *
 * Fix: after construction and before each frame, repair any texture with
 * name == 0 once a context is current:
 *   - file-backed textures  -> -[CPTexture2D updateForChangeToTexturePack]
 *   - painting textures     -> -[PaintMixUI updateMix]
 *
 * This is a workaround: the textures are still created in a contextless
 * window, they are repaired afterwards. Preventing the construction would
 * require changes beyond swizzling.
 *
 * Build the diagnostic variant with -DPAINTMIXFIX_VERBOSE=1.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <android/log.h>

#define TAG "PAINTMIXFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef PAINTMIXFIX_VERBOSE
#define PAINTMIXFIX_VERBOSE 0
#endif

#if PAINTMIXFIX_VERBOSE
#define LOGV(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#else
#define LOGV(...) ((void)0)
#endif

typedef void *id;
typedef void *SEL;
typedef void *Method;
typedef void *Ivar;
typedef void *IMP;

static SEL       (*selReg)(const char *);
static id        (*getClass)(const char *);
static Method    (*getInstMethod)(id, SEL);
static IMP       (*setImp)(Method, IMP);
static IMP       (*getImp)(Method);
static Ivar      (*getIvar)(id, const char *);
static ptrdiff_t (*ivarOffset)(Ivar);
static void      *g_msgSend;
static void     *(*g_eglGetCurrentContext)(void);
static uint8_t   *g_hdTextures;

#define MSG_v(o,s) (((void (*)(id,SEL))g_msgSend)((o),(s)))

/*
 * Offsets are read from the ObjC metadata at install time. CPTexture2D does
 * not export OBJC_IVAR_$_ symbols, so the runtime API is the only lookup
 * that works for it; these literals are the values observed in v1.7.5 and
 * exist only so a metadata lookup failure degrades instead of disabling the
 * fix. They are not expected to hold for any other build.
 */
static uint32_t TEX_NAME     = 0x04;   /* _name     */
static uint32_t TEX_ISHD     = 0x30;   /* isHD      */
static uint32_t TEX_BASEPATH = 0x34;   /* basePath  */

static uint32_t IV_BG    = 0x1C;       /* backgroundTexture     */
static uint32_t IV_ARROW = 0xB0;       /* arrowTexture          */
static uint32_t IV_RAW   = 0xBC;       /* paintingRawTexture    */
static uint32_t IV_OUT   = 0xC0;       /* paintingOutputTexture */

static int     g_mixHealed = 0;
static int64_t g_nextTry   = 0;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Read an ivar offset from the ObjC metadata, leaving the caller's value
   untouched if the lookup fails. */
static void resolve_ivar(const char *clsname, const char *iv, uint32_t *out)
{
    if (!getIvar || !ivarOffset) return;
    id c = getClass(clsname);
    if (!c) { LOGE("class %s not found for ivar %s", clsname, iv); return; }
    Ivar i = getIvar(c, iv);
    if (!i) { LOGE("ivar %s.%s not found, using %u", clsname, iv, *out); return; }
    *out = (uint32_t)ivarOffset(i);
}

/* nil counts as healthy - nothing to repair */
static uint32_t tex_name(id tex)
{
    return tex ? *(uint32_t *)((char *)tex + TEX_NAME) : 1;
}

/* Reload a file-backed texture from its retained basePath.
   updateForChangeToTexturePack only acts when isHD disagrees with
   HD_TEXTURES, so flip isHD first; the method resets it on the way out. */
static int heal_file_texture(id tex, const char *what)
{
    if (!tex) return 0;
    if (*(uint32_t *)((char *)tex + TEX_NAME) != 0) return 0;
    if (!*(id *)((char *)tex + TEX_BASEPATH)) return 0;

    uint8_t *isHD = (uint8_t *)tex + TEX_ISHD;
    *isHD = (g_hdTextures && *g_hdTextures) ? 0 : 1;
    MSG_v(tex, selReg("updateForChangeToTexturePack"));

    uint32_t now = *(uint32_t *)((char *)tex + TEX_NAME);
    LOGV("file heal %s tex=%p -> name=%u", what, tex, now);
    return now != 0;
}

static void heal_paintmix(id self, const char *where)
{
    /* No context yet: the render hook retries on a later frame. Never block
       here - the context is restored by a MainThread tick action, and this
       runs on MainThread, so waiting would deadlock. */
    void *ctx = g_eglGetCurrentContext ? g_eglGetCurrentContext() : 0;
    if (!ctx) {
        LOGV("skip heal at %s: no current context", where);
        return;
    }

    int did = 0;
    did |= heal_file_texture(*(id *)((char *)self + IV_BG),    "background");
    did |= heal_file_texture(*(id *)((char *)self + IV_ARROW), "arrow");

    id raw = *(id *)((char *)self + IV_RAW);
    id out = *(id *)((char *)self + IV_OUT);
    LOGV("heal check at %s ctx=%p raw=%u out=%u", where, ctx,
         tex_name(raw), tex_name(out));

    if (tex_name(raw) != 0 && tex_name(out) != 0) {
        g_mixHealed = 0;                 /* healthy - re-arm for next time */
    } else if (!g_mixHealed && now_ms() >= g_nextTry) {
        /* updateMix rebuilds both painting textures from
           incomingCraftableItemObject. Unconditional, no early out, but it
           decodes and re-encodes a PNG - run it once per breakage. */
        MSG_v(self, selReg("updateMix"));

        id r2 = *(id *)((char *)self + IV_RAW);
        id o2 = *(id *)((char *)self + IV_OUT);
        g_mixHealed = (tex_name(r2) != 0 && tex_name(o2) != 0);
        if (!g_mixHealed) g_nextTry = now_ms() + 500;
        did = 1;
        LOGV("updateMix at %s -> raw=%u out=%u ok=%d",
             where, tex_name(r2), tex_name(o2), g_mixHealed);
    }

    if (did) LOGI("repaired PaintMixUI textures (%s)", where);
}

typedef void (*render_fn)(id, SEL, float, int, int, float);
typedef void (*setwb_fn)(id, SEL, id, id, id);

static render_fn g_origRender;
static setwb_fn  g_origSetWorkbench;

static void my_render(id self, SEL _cmd, float dt, int tx, int ty, float sc)
{
    heal_paintmix(self, "render");
    g_origRender(self, _cmd, dt, tx, ty, sc);
}

static void my_setWorkbench(id self, SEL _cmd, id wb, id bh, id item)
{
    g_origSetWorkbench(self, _cmd, wb, bh, item);
    g_mixHealed = 0;                     /* new item, new textures */
    g_nextTry   = 0;
    heal_paintmix(self, "setWorkbench");
}

static int install(void)
{
    void *h = dlopen("libApplication.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) { LOGE("libApplication not loaded: %s", dlerror()); return 0; }

    g_hdTextures = (uint8_t *)dlsym(h, "HD_TEXTURES");
    if (!g_hdTextures) LOGE("HD_TEXTURES not found, assuming standard textures");

    resolve_ivar("CPTexture2D", "_name",    &TEX_NAME);
    resolve_ivar("CPTexture2D", "isHD",     &TEX_ISHD);
    resolve_ivar("CPTexture2D", "basePath", &TEX_BASEPATH);

    resolve_ivar("PaintMixUI", "backgroundTexture",     &IV_BG);
    resolve_ivar("PaintMixUI", "arrowTexture",          &IV_ARROW);
    resolve_ivar("PaintMixUI", "paintingRawTexture",    &IV_RAW);
    resolve_ivar("PaintMixUI", "paintingOutputTexture", &IV_OUT);

    id cls = getClass("PaintMixUI");
    if (!cls) { LOGE("PaintMixUI class not found"); return 0; }

    Method mr = getInstMethod(cls, selReg("render:translation:pinchScale:"));
    Method ms = getInstMethod(cls, selReg("setWorkbench:blockhead:craftableItemObject:"));
    if (!mr || !ms) {
        LOGE("methods not found: render=%p setWorkbench=%p", mr, ms);
        return 0;
    }

    g_origRender       = (render_fn)getImp(mr);
    g_origSetWorkbench = (setwb_fn)getImp(ms);
    setImp(mr, (IMP)my_render);
    setImp(ms, (IMP)my_setWorkbench);

    LOGV("ivars bg=%u arrow=%u raw=%u out=%u | tex name=%u ishd=%u path=%u | hd=%u",
         IV_BG, IV_ARROW, IV_RAW, IV_OUT,
         TEX_NAME, TEX_ISHD, TEX_BASEPATH,
         g_hdTextures ? *g_hdTextures : 255);

    LOGI("loaded from BackgroundLibraryLoader");
    return 1;
}

__attribute__((constructor))
static void init(void)
{
    static int once = 0;
    if (__sync_val_compare_and_swap(&once, 0, 1) != 0) return;

    void *sys = dlopen("libSystem.so", RTLD_NOW | RTLD_NOLOAD);
    if (!sys) { LOGE("libSystem not loaded - load point too early"); return; }

    selReg        = dlsym(sys, "sel_registerName");
    getClass      = dlsym(sys, "objc_getClass");
    getInstMethod = dlsym(sys, "class_getInstanceMethod");
    setImp        = dlsym(sys, "method_setImplementation");
    getImp        = dlsym(sys, "method_getImplementation");
    getIvar       = dlsym(sys, "class_getInstanceVariable");
    ivarOffset    = dlsym(sys, "ivar_getOffset");
    g_msgSend     = dlsym(sys, "objc_msgSend");

    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp || !g_msgSend) {
        LOGE("ObjC runtime symbols missing"); return;
    }
    if (!getIvar || !ivarOffset)
        LOGE("ivar API missing, using compiled-in offsets");

    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (egl) g_eglGetCurrentContext = dlsym(egl, "eglGetCurrentContext");
    else     LOGE("libEGL dlopen failed: %s", dlerror());

    install();
}
