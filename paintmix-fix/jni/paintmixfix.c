/*
 * libpaintmixfix.so
 *
 * THE BUG
 * -------
 * Fixes black textures in the Easel paint-mixing UI (PaintMixUI).
 * Returning from the photo picker tears down the EGL surface, which
 * unbinds the context (eglMakeCurrent with EGL_NO_SURFACE). The game
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
 * THE FIX
 * -------
 * After construction and before each frame, repair any texture with
 * name == 0 once a context is current:
 *   - file-backed textures  -> -[CPTexture2D updateForChangeToTexturePack]
 *   - painting textures     -> -[PaintMixUI updateMix]
 *
 * This is a workaround: the textures are still created in a contextless
 * window, they are repaired afterwards. Preventing the construction would
 * require changes beyond swizzling.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "paintmixfix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libpaintmixfix.so paintmixfix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 *
 *   Diagnostic variant: -DPAINTMIXFIX_VERBOSE=1.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>
#include <time.h>   /* clock_gettime, for the healing backoff timer */

#ifndef PAINTMIXFIX_VERBOSE
#define PAINTMIXFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef PAINTMIXFIX_STRICT_ENCODING
#define PAINTMIXFIX_STRICT_ENCODING 1
#endif

#define TAG "PAINTMIXFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if PAINTMIXFIX_VERBOSE
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

#if PAINTMIXFIX_STRICT_ENCODING
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

/* TODO: measure -[PaintMixUI render:translation:pinchScale:] on-device and
   fill in. Signature is (float, int, int, float); a wrong float/int split
   on ARM32 corrupts the stack on this per-frame method, so leave
   PAINTMIXFIX_STRICT_ENCODING at its default (1) until this is set. */
#define ENC_RENDER NULL

/* TODO: measure -[PaintMixUI setWorkbench:blockhead:craftableItemObject:]
   on-device and fill in. Signature is three objects (id, id, id). */
#define ENC_SETWORKBENCH NULL

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

/* Resolved in install() (step 5) from libApplication's exported flag. */
static uint8_t *g_hdTextures;

/* Resolved in init(), after the canonical runtime lookups - see the
   dlopen("libEGL.so", ...) note there for why it isn't RTLD_NOLOAD. */
static void *(*g_eglGetCurrentContext)(void);

typedef void (*render_fn)(id, SEL, float, int, int, float);
typedef void (*setwb_fn)(id, SEL, id, id, id);

static render_fn g_origRender;
static setwb_fn  g_origSetWorkbench;

static int     g_mixHealed = 0;
static int64_t g_nextTry   = 0;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static int install(void) {
    Class      cls;
    Method     mRender, mSetWorkbench;
    const char *how;

    /* 1 */
    cls = getClass("PaintMixUI");
    if (!cls) { LOGE("PaintMixUI not present - load point too early"); return 0; }

    /* 2 - heal_file_texture and heal_paintmix call through MSG_v */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    /* 3 */
    mRender = getInstMethod(cls, selReg("render:translation:pinchScale:"));
    if (!mRender) { LOGE("render:translation:pinchScale: not found"); return 0; }
    if (getImp(mRender) == (IMP)my_render) { LOGI("already installed"); return 1; }

    mSetWorkbench = getInstMethod(cls, selReg("setWorkbench:blockhead:craftableItemObject:"));
    if (!mSetWorkbench) {
        LOGE("setWorkbench:blockhead:craftableItemObject: not found");
        return 0;
    }

    /* 4 */
    if (!resolve_ivar("CPTexture2D", "_name",    &TEX_NAME,     0x04, &how)) return 0;
    if (!resolve_ivar("CPTexture2D", "isHD",     &TEX_ISHD,     0x30, &how)) return 0;
    if (!resolve_ivar("CPTexture2D", "basePath", &TEX_BASEPATH, 0x34, &how)) return 0;

    if (!resolve_ivar("PaintMixUI", "backgroundTexture",     &IV_BG,    0x1C, &how)) return 0;
    if (!resolve_ivar("PaintMixUI", "arrowTexture",          &IV_ARROW, 0xB0, &how)) return 0;
    if (!resolve_ivar("PaintMixUI", "paintingRawTexture",    &IV_RAW,   0xBC, &how)) return 0;
    if (!resolve_ivar("PaintMixUI", "paintingOutputTexture", &IV_OUT,   0xC0, &how)) return 0;

    /* 5 - HD_TEXTURES is a global flag, not an ivar, but like the ivars it
       must be resolved before either hook goes live. It is only exported
       from libApplication with no fallback tier, so require the library
       here exactly as the old top-of-function check did. */
    if (!g_app) { LOGE("libApplication not loaded - cannot resolve HD_TEXTURES"); return 0; }
    g_hdTextures = (uint8_t *)dlsym(g_app, "HD_TEXTURES");
    if (!g_hdTextures) LOGE("HD_TEXTURES not found, assuming standard textures");

    /* 6 */
    if (!checkEncoding(mRender, "render:translation:pinchScale:", ENC_RENDER))
        return 0;
    if (!checkEncoding(mSetWorkbench, "setWorkbench:blockhead:craftableItemObject:",
                       ENC_SETWORKBENCH))
        return 0;

    /* 7 */
    if (!hook(cls, "render:translation:pinchScale:", ENC_RENDER,
              (IMP)my_render, (IMP *)&g_origRender))
        return 0;

    if (!hook(cls, "setWorkbench:blockhead:craftableItemObject:", ENC_SETWORKBENCH,
              (IMP)my_setWorkbench, (IMP *)&g_origSetWorkbench)) {
        setImp(mRender, (IMP)g_origRender);
        return 0;
    }

    LOGV("ivars bg=%u arrow=%u raw=%u out=%u | tex name=%u ishd=%u path=%u | hd=%u",
         IV_BG, IV_ARROW, IV_RAW, IV_OUT,
         TEX_NAME, TEX_ISHD, TEX_BASEPATH,
         g_hdTextures ? *g_hdTextures : 255);

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

    /* File-specific: healing needs to know whether an EGL context is
       current before touching any GL object. Unlike libSystem and
       libApplication above, libEGL.so is not guaranteed to already be
       loaded at this point, so this is a real RTLD_NOW dlopen (loading
       it if necessary) rather than the RTLD_NOLOAD used for the
       libraries the game itself is responsible for loading. */
    {
        void *egl = dlopen("libEGL.so", RTLD_NOW);
        if (egl) g_eglGetCurrentContext = dlsym(egl, "eglGetCurrentContext");
        else     LOGE("libEGL dlopen failed: %s", dlerror());
    }

    install();
}
