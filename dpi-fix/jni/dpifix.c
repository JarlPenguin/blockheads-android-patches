/*
 * libdpifix.so
 *
 * THE BUG
 * -------
 * Apportable emulates a fixed iPhone screen. main() calls
 * +[UIScreenMode emulatedMode:11], which reads the device's native
 * dimensions and hands them to +[UIScreenMode bestEmulatedMode:] - a
 * catalogue of fixed iOS device profiles, bucketed by aspect ratio and then
 * snapped down by pixel thresholds. Every device therefore renders as
 * whichever iPhone it snaps to (750x1334 @2.0 on a 1080x1920 phone),
 * regardless of its actual size or density.
 *
 * UIScreen.bounds is mode.size/mode.scale, computed fresh on every call, and
 * PIXEL_SCALE is a cached copy of UIScreen.scale taken in
 * -[EAGLView initWithCoder:], so overriding the two mode accessors fixes the
 * whole chain: bounds, framebuffer size, layout and touch mapping.
 *
 * A second consequence is the multi-window one. -[UIScreen _applyMode] pushes
 * mode.size through the bridged setContentWidth:height: to
 * SurfaceHolder.setFixedSize, pinning the framebuffer; SurfaceFlinger then
 * stretches that fixed buffer over whatever the window actually is. In a split
 * pane or a resized window the result is warped rather than relaid-out.
 *
 * THE FIX
 * -------
 * Overrides -[UIScreenMode scale] and -[UIScreenMode size], as well as
 * the overrides on UINativeScreenMode, with lazily-computed geometry once
 * the GL surface dimensions become available via VerdePluginNativeWidth()
 * and VerdePluginNativeHeight().
 *
 * Scale is getenv("DPIFIX_SWPX") / getenv("DPIFIX_SWDP") - the display's short
 * edge in pixels over the same edge in dp, both pushed from Java and both read
 * from Display.getRealMetrics(), so the reference is the physical display and
 * not whatever window the app happened to launch into. Android dp and iOS
 * points share the 160 dpi baseline, so mapping one to the other gives
 * physically correct sizing. DPIFIX_SWPX falling back to g_pxW reproduces the
 * older window-derived behaviour on a build where only SWDP is pushed.
 * Phones (when getenv("TABLET") != "1") are capped at a 414-point width to
 * prevent crossing the game's internal 415-point tablet layout threshold. The
 * scale is clamped to [1.0, 4.0]. Geometry is reported portrait-canonically
 * (short edge as width). Both getters fall back to the original implementation
 * if geometry is not yet available.
 *
 * Window resize (multi-window, rotation): window pixels arrive from Java on
 * every layout pass; -[UIScreen _applyMode] re-pins the surface and relayout()
 * fixes the UIWindow frame. -[UIDevice orientation] is overridden because in
 * multi-window the device orientation does not describe the window's shape.
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
#include <android/log.h>

#include <jni.h>
#include <stdlib.h>
#include <string.h>

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

/* All three measured with method_getTypeEncoding on the target build. */
#define ENC_MODE_SCALE  "f8@0:4"
#define ENC_MODE_SIZE   "{CGSize=ff}8@0:4"
#define ENC_DEV_ORIENT  "i8@0:4"

typedef struct { float w, h; }             CGSize32;
typedef struct { float x, y, w, h; }       CGRect32;

/* Neither is covered by the canonical MSG_* set. MSG_rect: a 16-byte struct
   return goes through objc_msgSend_stret on ARM32, a different entry point
   with a hidden sret pointer. MSG_setFrame: four floats by value, which the
   canonical set has no arity for. */
#define MSG_rect(r,o,s) (((void (*)(CGRect32*,id,SEL))g_msgSendStret)((r),(o),(s)))

#define MSG_setFrame(o,s,x,y,w,h) \
    (((void (*)(id,SEL,float,float,float,float))g_msgSend)((o),(s),(x),(y),(w),(h)))

/* objc_msgSend_stret is not part of the canonical runtime set, so it is
   declared here and resolved in install() step 5 rather than in init(). */
static void *g_msgSendStret;

/* Apportable's live framebuffer dimensions; both return 0 until the GL
   surface exists, which is why geometry is computed lazily below. */
static int (*nativeWidth)(void);
static int (*nativeHeight)(void);

static IMP g_origModeScale = 0, g_origModeSize = 0;
static IMP g_origDevOrient = 0;

/* Emulated-screen geometry, portrait-canonical (short edge as width).
   g_scale is fixed once and never recomputed: PIXEL_SCALE is a cached copy
   of UIScreen.scale taken in -[EAGLView initWithCoder:], so a mid-session
   scale change would desync touch mapping from rendering. */
static float g_scale = 0.0f;
static int   g_pxW = 0, g_pxH = 0;

/* The window's own pixels, as measured in Java and pushed on every layout.
   Raw, not normalized: the aspect is needed to decide the transpose. */
static int g_winW = 0, g_winH = 0;
static int g_winSet = 0;

/* Display rotation pushed from Java, in UIInterfaceOrientation encoding.
   0 = not yet known. Sourced from Display.getRotation() rather than derived
   from window dimensions: the window shape is downstream of the device
   orientation, so deriving from it creates a feedback loop that oscillates
   whenever the window is near-square or mid-transition. */
static volatile int g_displayOrient = 0;

/* Multi-window state, pushed from Java on every configuration change. */
static volatile int g_multiWindow = 0;

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

    /* Display short edge in pixels, the numerator that makes the scale a
       property of the hardware rather than of the launch window. See the
       banner. Falls back to the framebuffer's short edge on a build where
       only SWDP is pushed. */
    const char *spx = getenv("DPIFIX_SWPX");
    int swpx = spx ? atoi(spx) : 0;
    if (swpx <= 0) swpx = g_pxW;

    g_scale = (float)swpx / (float)swdp;

    /* The game branches on a hardcoded 415-point width to choose phone vs
       tablet layout (inventory placement, D-pad, carousel spacing). dp runs
       a little wider than iOS points, so a large phone can cross the line
       and get tablet layout in portrait. Pin those to the widest phone
       profile the game was authored for. */
    const char *tab = getenv("TABLET");
    int isTablet = tab && *tab == '1';
    if (!isTablet) {
        const float PHONE_MAX_PT = 414.0f;
        if ((float)swpx / g_scale > PHONE_MAX_PT)
            g_scale = (float)swpx / PHONE_MAX_PT;
    }

    if (g_scale < 1.0f) g_scale = 1.0f;
    if (g_scale > 4.0f) g_scale = 4.0f;

    /* swpx and swdp are the scale's inputs; g_pxW/g_pxH are the window's,
       and can differ from the display when the app launched into a split. */
    LOGI("native %dx%d swpx=%d swdp=%d tablet=%s -> scale %.3f (points %.1fx%.1f)",
         w, h, swpx, swdp, isTablet ? "yes" : "no",
         g_scale, g_pxW / g_scale, g_pxH / g_scale);
    return 1;
}

static void apply_mode(void) {
    Class sc = getClass("UIScreen");
    id    screen;

    if (!sc) { LOGE("UIScreen not found - cannot re-pin"); return; }
    screen = MSG_id((id)sc, selReg("mainScreen"));
    if (!screen) { LOGE("mainScreen nil - cannot re-pin"); return; }

    MSG_v(screen, selReg("_applyMode"));
}

static void relayout(void) {
    Class scls, acls;
    id screen, app, win, rootvc, rootview;
    CGRect32 sb = {0,0,0,0};
    float fw, fh;

    if (!g_msgSendStret) { LOGE("objc_msgSend_stret unavailable - no relayout"); return; }

    scls = getClass("UIScreen");
    acls = getClass("UIApplication");
    if (!scls || !acls) return;

    screen = MSG_id((id)scls, selReg("mainScreen"));
    if (!screen) return;
    /* bounds is fetched only to confirm the screen has usable geometry before
       touching the hierarchy; the frame itself comes from g_winW/g_winH for the
       reason below. */
    MSG_rect(&sb, screen, selReg("bounds"));
    if (sb.w <= 0.0f || sb.h <= 0.0f) return;

    /* The window frame takes the window-shaped rect, not the portrait-canonical
       bounds: on iOS UIKit rotates the window above an orientation-invariant
       UIScreen, and nothing here performs that rotation. Using bounds directly
       leaves an unpainted strip wherever the window is not portrait. */
    fw = (float)g_winW / g_scale;
    fh = (float)g_winH / g_scale;

    app = MSG_id((id)acls, selReg("sharedApplication"));
    win = app ? MSG_id(app, selReg("keyWindow")) : NULL;
    if (!win) { LOGE("keyWindow nil - no relayout"); return; }

    LOGV("relayout: keyWindow -> %.1fx%.1f", fw, fh);

    /* setNeedsLayout only: forcing the pass with layoutIfNeeded reaches
       -[EAGLView layoutSubviews] -> deleteFramebuffer, whose rebuild is lazy,
       and tearing the framebuffer down off the render thread freezes the frame. */
    MSG_setFrame(win, selReg("setFrame:"), 0.0f, 0.0f, fw, fh);
    MSG_v(win, selReg("setNeedsLayout"));

    /* The root view was the same size as the window, so it should follow
       from the window's layout - but Apportable's UIWindow may not
       propagate, so set it directly and let its own layoutSubviews run. */
    rootvc   = MSG_id(win, selReg("rootViewController"));
    rootview = rootvc ? MSG_id(rootvc, selReg("view")) : NULL;
    if (rootview) {
        MSG_setFrame(rootview, selReg("setFrame:"), 0.0f, 0.0f, fw, fh);
        MSG_v(rootview, selReg("setNeedsLayout"));
    }
}

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

/* -[UIScreen _applyMode] transposes currentMode.size when this getter returns
   3 or 4, which is how UIKit's window rotation is expressed here. The device's
   own orientation is the wrong input in multi-window, where the pane's shape is
   set by the divider and can differ from the device's pose entirely. */
static int my_device_orientation(id self, SEL _cmd) {
    /* Change-detection for the LOGV below; nothing else reads these. At
       VERBOSE 0 they are maintained for nothing, which is the price of the
       log being readable when it is turned on. */
    static int s_lastIn = -99, s_lastOut = -99;
    static int s_lastW = -1, s_lastH = -1;
    int o = ((int (*)(id, SEL))g_origDevOrient)(self, _cmd);
    int out;

    if (g_multiWindow && g_winSet) {
        /* In a split the pane's shape is set by the divider, not the sensor
           or the display, so the pane is authoritative. Deriving from window
           shape is only unstable in fullscreen, where the window follows the
           display and the two form a feedback loop - hence the split. */
        int winLandscape = (g_winW > g_winH);
        int isLand = (o == 3 || o == 4);
        int isPort = (o == 1 || o == 2);
        if ((winLandscape && isLand) || (!winLandscape && isPort))
            out = o;
        else
            out = winLandscape ? 3 : 1;
    } else if (g_displayOrient != 0) {
        int dispLand = (g_displayOrient == 3 || g_displayOrient == 4);
        int isLand   = (o == 3 || o == 4);
        int isPort   = (o == 1 || o == 2);

        if ((dispLand && isLand) || (!dispLand && isPort))
            out = o;                 /* device agrees with the display */
        else
            out = g_displayOrient;   /* display wins; it is what the window follows */
    } else {
        out = o;                     /* nothing authoritative yet */
    }

    if (o != s_lastIn || out != s_lastOut || g_winW != s_lastW || g_winH != s_lastH) {
        LOGV("orient in=%d out=%d disp=%d win=%dx%d px=%dx%d",
             o, out, g_displayOrient, g_winW, g_winH, g_pxW, g_pxH);
        s_lastIn = o; s_lastOut = out; s_lastW = g_winW; s_lastH = g_winH;
    }

    return out;
}

/* ---- JNI exports ---------------------------------------------------- */

JNIEXPORT void JNICALL
Java_com_apportable_gl_GLSurfaceView_dpifixSetDisplayOrient(JNIEnv *e, jclass c, jint o) {
    (void)e; (void)c;
    if (o >= 1 && o <= 4) g_displayOrient = o;
}

JNIEXPORT void JNICALL
Java_com_apportable_gl_GLSurfaceView_dpifixMultiWindow(JNIEnv *e, jclass c, jboolean mw) {
    (void)e; (void)c;
    g_multiWindow = mw ? 1 : 0;
}

/* Called on thread 1, from WindowState.run(). apply_mode() reaches the
   bridged setContentSize:, and relayout() touches the view hierarchy; both
   are unsafe from the Android UI thread. */
JNIEXPORT void JNICALL
Java_com_apportable_gl_GLSurfaceView_dpifixWindowPx(JNIEnv *env, jclass cls,
                                                    jint w, jint h) {
    int nw, nh;
    (void)env; (void)cls;

    if (w <= 0 || h <= 0) return;
    if (!ensure_geometry()) { LOGV("window update before geometry - deferred"); return; }

    nw = (w < h) ? w : h;
    nh = (w < h) ? h : w;

    if (nw == g_pxW && nh == g_pxH && g_winSet && ((g_winW > g_winH) == (w > h))) {
        LOGV("window unchanged: %dx%dpx", w, h);
        return;
    }

    LOGV("window %dx%dpx (canonical %dx%d, points %.1fx%.1f)",
         w, h, nw, nh, nw / g_scale, nh / g_scale);

    g_winW = w; g_winH = h; g_winSet = 1;
    g_pxW  = nw; g_pxH  = nh;

    apply_mode();
    relayout();
}

static int install(void) {
    Class  mode, nmode, dev;
    Method mScale, mSize, mNScale, mNSize, mDevOrient;
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
       always present on this build - preferredMode constructs one
       unconditionally - so absence means the binary is not the one analysed.
       Log and continue: the parent hooks may still cover whatever mode is in
       use, and failing closed here would disable the fix on a binary that
       might not need it. */
    nmode = getClass("UINativeScreenMode");
    if (!nmode) LOGE("UINativeScreenMode not found");

    /* Same tolerance for UIDevice: without the orientation override the
       transpose stays device-driven, which is wrong only in multi-window. */
    dev = getClass("UIDevice");
    if (!dev) LOGE("UIDevice not found - orientation override will be skipped");

    /* 2 - apply_mode() and relayout() use MSG_id/MSG_v */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    /* 3 */
    mScale = getInstMethod(mode, selReg("scale"));
    if (!mScale) { LOGE("UIScreenMode scale not found"); return 0; }
    if (getImp(mScale) == (IMP)my_mode_scale) { LOGI("already installed"); return 1; }

    mSize = getInstMethod(mode, selReg("size"));
    if (!mSize) { LOGE("UIScreenMode size not found"); return 0; }

    mNScale = nmode ? getInstMethod(nmode, selReg("scale")) : NULL;
    mNSize  = nmode ? getInstMethod(nmode, selReg("size"))  : NULL;

    mDevOrient = dev ? getInstMethod(dev, selReg("orientation")) : NULL;

    /* 4 - skipped: no ivars resolved */

    /* 5 - objc_msgSend_stret, for the struct-returning bounds/frame calls in
       relayout(). Not in the canonical runtime set, so it is resolved here
       rather than in init(). Non-fatal: relayout() guards on it and the rest
       of the fix works without it. */
    g_msgSendStret = dlsym(g_sys, "objc_msgSend_stret");
    if (!g_msgSendStret) LOGE("objc_msgSend_stret unavailable - no relayout");

    /* 6 - every encoding, before any setImp */
    if (!checkEncoding(mScale, "scale", ENC_MODE_SCALE)) return 0;
    if (!checkEncoding(mSize,  "size",  ENC_MODE_SIZE))  return 0;
    if (mNScale && !checkEncoding(mNScale, "scale", ENC_MODE_SCALE)) return 0;
    if (mNSize  && !checkEncoding(mNSize,  "size",  ENC_MODE_SIZE))  return 0;
    if (mDevOrient && !checkEncoding(mDevOrient, "orientation", ENC_DEV_ORIENT)) {
        LOGE("UIDevice orientation encoding mismatch - override will be skipped");
        mDevOrient = NULL;
    }

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

    /* Deliberately non-fatal, unlike every hook above: the mode overrides are
       the fix, and the orientation override only corrects the transpose in
       multi-window. Losing it degrades split-screen geometry rather than
       leaving the DPI fix half-applied, so it logs and clears the captured IMP
       instead of rolling the whole install back. */
    if (mDevOrient && !hook(dev, "orientation", ENC_DEV_ORIENT,
                            (IMP)my_device_orientation, &g_origDevOrient)) {
        LOGE("UIDevice orientation hook failed - transpose stays device-driven");
        g_origDevOrient = 0;
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
