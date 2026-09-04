/*
 * librotationfix.so
 *
 * THE BUG
 * -------
 * 1. Startup flat-launch desync: The engine's app delegate hardcodes a call
 *    to -[UIDevice _setOrientation:4 changed:1] (LandscapeRight) at boot and
 *    normally expects to correct it via sensor events ~150ms later. When the
 *    device is held flat at launch, OrientationEventListener reports
 *    ORIENTATION_UNKNOWN, so SplashScreen$1 filters the event and no
 *    correction is ever dispatched. The engine remains stuck rendering
 *    landscape in a portrait window for the entire session.
 *
 * 2. Tilt frame desync: When Android's rotation lock is enabled
 *    (accelerometer_rotation = 0), -[EvolutionViewController shouldAutorotate]
 *    returns NO. This gates -[UIDevice _setOrientation:changed:], the sole caller
 *    of _platform_setOrientation:, which is the only writer of the global
 *    backing -[UIApplication statusBarOrientation]. Consequently,
 *    statusBarOrientation remains 0 for the entire session. -[World acceleration:]
 *    reads this global on every accelerometer sample (~30Hz) and only applies
 *    its coordinate frame transforms for orientations 2 (PortraitUpsideDown)
 *    and 4 (LandscapeRight); with 0, both transforms are skipped, leaving tilt
 *    controls locked in the LandscapeLeft frame regardless of display orientation.
 *
 * 3. Multi-window desync: in a split the pane's shape is set by the divider
 *    rather than by the sensor, so the engine can be driven into an orientation
 *    the display never entered. Leaving the split then strands it there, and
 *    every subsequent rotation is offset by the difference.
 *
 * THE FIX
 * -------
 * 1. Intercepts -[UIDevice _setOrientation:changed:] with a one-shot atomic CAS
 *    to substitute the true locked orientation (pushed from Java via JNI) for
 *    the engine's startup LandscapeRight (4). This single interception point is
 *    upstream of statusBarOrientation, the Java window rotation request, and
 *    [UIScreen _applyMode].
 *
 * 2. Swizzles -[UIApplication statusBarOrientation] to return g_lockedOrientation
 *    while inside -[World acceleration:], whenever the real value is absent (0)
 *    OR disagrees with g_lockedOrientation on axis. Agreement on axis leaves the
 *    real value untouched, so the engine's own handedness - which the tilt
 *    transforms were written against - is preserved wherever it is meaningful.
 *
 * 3. Swizzles -[World acceleration:] with a thread-local depth counter
 *    (g_accelDepth) to isolate the substitution strictly to the tilt physics
 *    path, preventing the layout path from seeing a substituted orientation and
 *    reconfiguring the render surface for an unrotated window.
 *
 * 4. Tracks multi-window state and the rotation-lock setting, pushed from Java.
 *    On leaving a split the engine's orientation is resynchronised via
 *    nativeRunPendingResync, since the pane's shape can have driven it into an
 *    orientation the display never entered. The resync runs on thread 1: the
 *    call chain reaches [UIScreen _applyMode], which is unsafe from the
 *    Android UI thread.
 *
 * KNOWN LIMITATION
 * ----------------
 * In split-screen the in-game tilt-control orientation lock does not hold.
 * The engine expresses its veto by declining to call setRequestedOrientation,
 * which only works while an earlier pin stands; Android does not honour a
 * fixed orientation in multi-window (it rotates the pane and letterboxes
 * instead), so no pin can be kept. The tilt frame re-bases correctly.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "rotationfix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o librotationfix.so rotationfix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>

#include <jni.h>

#ifndef ROTATIONFIX_VERBOSE
#define ROTATIONFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef ROTATIONFIX_STRICT_ENCODING
#define ROTATIONFIX_STRICT_ENCODING 1
#endif

#define TAG "ROTATIONFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if ROTATIONFIX_VERBOSE
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

#if ROTATIONFIX_STRICT_ENCODING
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

/* Measured with method_getTypeEncoding on the target build: a 16-byte
   Vector (four floats) passed by value, which AAPCS splits across r2-r3
   plus two stack words. The four pass-through args cover it exactly. */
#define ENC_ACCELERATION "v24@0:4{Vector=[4f]}8"

/* Measured on the target build. */
#define ENC_STATUS_BAR_ORI "i8@0:4"

/* Measured on the target build: (int, char), the char being a BOOL passed
   in the low byte of its own word. */
#define ENC_SET_ORIENTATION_CHANGED "v16@0:4i8c12"

static IMP g_origAcceleration          = 0;
static IMP g_origStatusBarOri          = 0;
static IMP g_origSetOrientationChanged = 0;

/* Locked orientation, pushed from Java. UIInterfaceOrientation:
   1=Portrait 2=PortraitUpsideDown 3=LandscapeLeft 4=LandscapeRight.
   0 = not yet known. Written repeatedly - see nativeSetLockedOrientation. */
static volatile int g_lockedOrientation = 0;

/* The engine's own last-reported orientation, captured in
   my_setOrientationChanged. Diagnostic only, and retained for the same reason
   as g_autoRotate: the engine's value disagreeing with the display's is the
   single most useful thing to see in a tilt log. Read only by the LOGV in
   my_statusBarOrientation. */
static volatile int g_engineOrientation = 0;

/* Android's accelerometer_rotation setting, pushed from Java on every layout
   pass and on every focus change. Nothing branches on it: the substitution
   below reasons from the axis test alone. Retained because the rotation lock
   being on or off is the first thing worth knowing when a tilt bug is
   reported, and re-adding the Java plumbing to get it back would cost more
   than leaving it wired. Read only by the LOGV in my_statusBarOrientation. */
static volatile int g_autoRotate = 0;

/* Multi-window state, pushed from Java on every configuration change. */
static volatile int g_multiWindow = 0;

/* Set when g_multiWindow falls from 1 to 0; consumed on thread 1. */
static volatile int g_resyncPending = 0;

/* Depth counter, set only while inside -[World acceleration:]. Confines the
   substitution to the tilt path so the layout path still sees the real
   value and doesn't reconfigure the render surface for a window that never
   rotated. Thread-local: the engine thread runs acceleration:, and another
   thread querying statusBarOrientation concurrently must see the real value.
   A counter rather than a flag so a nested call can't clear it early. */
static __thread int g_accelDepth = 0;

/* One-shot for the engine's hardcoded startup LandscapeRight. Process-wide,
   not thread-local: it marks a single event in the process lifetime, and a
   per-thread copy would let a second interception through. */
static int g_startupFlipHandled = 0;

static int my_statusBarOrientation(id self, SEL _cmd) {
    int real = ((int (*)(id, SEL))g_origStatusBarOri)(self, _cmd);
    int out = real;

    /* -[UIApplication statusBarOrientation] returns a global written only by
       _platform_setOrientation:, which is gated on -[EvolutionViewController
       shouldAutorotate] - false whenever Android's rotation lock is on. So the
       value is 0 for a locked session, and -[World acceleration:] applies
       neither of its axis transforms, leaving tilt in the LandscapeLeft frame.

       It can also be STALE AND WRONG, which the original form of this patch
       assumed impossible: a forced rotation in split-screen moves the window
       while the lock keeps the global frozen at its old value. So substituting
       only on 0 is not sufficient.

       The load-bearing assumption is now the axis test, not `real == 0`.
       g_lockedOrientation is derived from Display.getRotation() and describes
       the window, so a disagreement on axis means `real` cannot be describing
       the same window and is discarded. Agreement on axis leaves `real` alone,
       preserving the engine's own handedness - the two conventions disagree on
       which landscape is which, and the tilt transforms were written against
       the engine's. If getRotation() ever stops describing the window the
       engine renders into, this breaks. */
    if (g_accelDepth > 0 && g_lockedOrientation != 0) {
        int realLand = (real == 3 || real == 4);
        int lockLand = (g_lockedOrientation == 3 || g_lockedOrientation == 4);
        if (real == 0 || realLand != lockLand)
            out = g_lockedOrientation;
    }

    if (g_accelDepth > 0) {
        LOGV("sbo real=%d engine=%d locked=%d auto=%d mw=%d -> out=%d",
             real, g_engineOrientation, g_lockedOrientation,
             g_autoRotate, g_multiWindow, out);
    }

    return out;
}

/* Arity verified from the runtime: acceleration: encoding is
   v24@0:4{Vector=[4f]}8 (see ENC_ACCELERATION). */
static void my_acceleration(id self, SEL _cmd, void *a, void *b, void *c, void *d) {
    static int s_logged = 0;
    if (!s_logged) { s_logged = 1; LOGV("acceleration: hook is live"); }
    g_accelDepth++;
    ((void (*)(id, SEL, void *, void *, void *, void *))g_origAcceleration)
        (self, _cmd, a, b, c, d);
    g_accelDepth--;
}

/* -[UIDevice _setOrientation:changed:] is the sole caller of
   _platform_setOrientation:, which sets statusBarOrientation, asks Android to
   rotate, and then runs [UIScreen _applyMode]. The engine's app delegate calls
   this with a hardcoded LandscapeRight at startup and normally corrects it from
   the sensor path ~150ms later. With the device held flat that path never fires
   (ORIENTATION_UNKNOWN), so the correction never arrives and the engine renders
   landscape in a portrait window for the whole session. Substituting here — the
   only point upstream of all three effects — keeps them consistent.

   One-shot: only the engine's startup request is intercepted, never a later
   user rotation. Fires unconditionally at startup; when Android's rotation lock
   is on the call is discarded downstream by shouldAutorotate, so the
   substitution is inert there. The one-shot is consumed either way, so a later
   user rotation to LandscapeRight always passes through untouched. */
static void my_setOrientationChanged(id self, SEL _cmd, int orientation, char changed) {
    if (orientation >= 1 && orientation <= 4) g_engineOrientation = orientation;

    if (orientation == 4 && __sync_val_compare_and_swap(&g_startupFlipHandled, 0, 1) == 0) {
        if (g_lockedOrientation != 0 && g_lockedOrientation != 4) {
            LOGI("substituting %d for startup LandscapeRight", g_lockedOrientation);
            orientation = g_lockedOrientation;
            changed = 1;
        }
    }
    ((void (*)(id, SEL, int, char))g_origSetOrientationChanged)
        (self, _cmd, orientation, changed);
}

/* ---- JNI exports ---------------------------------------------------- */

/* Pushed from VerdeActivity.onWindowFocusChanged and from
   WindowState.check(), so a rotation-lock toggle made while the game is
   backgrounded is picked up on return. */
JNIEXPORT void JNICALL
Java_com_apportable_ui_Device_nativeSetAutoRotate(JNIEnv *e, jclass c, jint v) {
    (void)e; (void)c;
    if (g_autoRotate != (v ? 1 : 0)) LOGV("autoRotate = %d", v ? 1 : 0);
    g_autoRotate = v ? 1 : 0;
}

/* Called from SplashScreen$1 at the splash-time dispatch, and thereafter
   from WindowState.check() on every layout pass with the value derived from
   Display.getRotation(). It is therefore written repeatedly and concurrently
   with reads on the engine thread - hence volatile, and hence no invariant
   here may assume a single settled value. */
JNIEXPORT void JNICALL
Java_com_apportable_ui_Device_nativeSetLockedOrientation(JNIEnv *e, jclass c, jint o) {
    (void)e; (void)c;
    if (o >= 1 && o <= 4) {
        g_lockedOrientation = o;
        LOGV("locked orientation = %d", o);
    } else {
        LOGV("ignoring out-of-range orientation %d", o);
    }
}

JNIEXPORT void JNICALL
Java_com_apportable_ui_Device_nativeSetMultiWindow(JNIEnv *e, jclass c, jboolean mw) {
    int was = g_multiWindow;
    (void)e; (void)c;
    g_multiWindow = mw ? 1 : 0;
    LOGV("multiWindow = %d", g_multiWindow);
    if (was && !g_multiWindow) g_resyncPending = 1;
}

/* Called on thread 1. The call chain reaches _applyMode, which is unsafe
   from the Android UI thread - calling it there does not return. */
JNIEXPORT void JNICALL
Java_com_apportable_ui_Device_nativeRunPendingResync(JNIEnv *e, jclass c) {
    (void)e; (void)c;
    if (!g_resyncPending || g_lockedOrientation == 0 || !g_origSetOrientationChanged) return;
    g_resyncPending = 0;
    {
        Class dev = getClass("UIDevice");
        id d = dev ? MSG_id((id)dev, selReg("currentDevice")) : NULL;
        if (!d) return;
        LOGV("resync orientation to %d", g_lockedOrientation);
        ((void (*)(id, SEL, int, char))g_origSetOrientationChanged)
            (d, selReg("_setOrientation:changed:"), g_lockedOrientation, 1);
        LOGV("resync returned");
    }
}

static int install(void) {
    Class  world, app, device;
    Method mAccel, mStatus, mSetOri;

    /* 1 */
    world  = getClass("World");
    app    = getClass("UIApplication");
    device = getClass("UIDevice");
    if (!world || !app || !device) {
        LOGE("classes not present - load point too early");
        return 0;
    }

    /* 2 - nativeRunPendingResync uses MSG_id */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    /* 3 - resolve every Method this file will hook: step 6 needs them all in hand */
    mAccel = getInstMethod(world, selReg("acceleration:"));
    if (!mAccel) { LOGE("acceleration: not found"); return 0; }
    if (getImp(mAccel) == (IMP)my_acceleration) { LOGI("already installed"); return 1; }

    mStatus = getInstMethod(app, selReg("statusBarOrientation"));
    if (!mStatus) { LOGE("statusBarOrientation not found"); return 0; }

    mSetOri = getInstMethod(device, selReg("_setOrientation:changed:"));
    if (!mSetOri) { LOGE("_setOrientation:changed: not found"); return 0; }

    /* 4 - skipped: no ivars resolved */

    /* 5 - skipped: no runtime selectors or constants to cache */

    /* 6 - all encodings verified before any setImp */
    if (!checkEncoding(mAccel,  "acceleration:",           ENC_ACCELERATION))            return 0;
    if (!checkEncoding(mStatus, "statusBarOrientation",     ENC_STATUS_BAR_ORI))          return 0;
    if (!checkEncoding(mSetOri, "_setOrientation:changed:", ENC_SET_ORIENTATION_CHANGED)) return 0;

    /* 7 - commit, rolling back already-committed hooks if a later one fails */
    if (!hook(world, "acceleration:", ENC_ACCELERATION,
              (IMP)my_acceleration, &g_origAcceleration))
        return 0;

    if (!hook(app, "statusBarOrientation", ENC_STATUS_BAR_ORI,
              (IMP)my_statusBarOrientation, &g_origStatusBarOri)) {
        setImp(mAccel, g_origAcceleration);
        return 0;
    }

    if (!hook(device, "_setOrientation:changed:", ENC_SET_ORIENTATION_CHANGED,
              (IMP)my_setOrientationChanged, &g_origSetOrientationChanged)) {
        setImp(mAccel,  g_origAcceleration);
        setImp(mStatus, g_origStatusBarOri);
        return 0;
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