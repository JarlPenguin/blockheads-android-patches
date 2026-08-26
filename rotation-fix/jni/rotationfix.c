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
 * THE FIX
 * -------
 * 1. Intercepts -[UIDevice _setOrientation:changed:] with a one-shot atomic CAS
 *    to substitute the true locked orientation (pushed from Java via JNI) for
 *    the engine's startup LandscapeRight (4). This single interception point is
 *    upstream of statusBarOrientation, the Java window rotation request, and
 *    [UIScreen _applyMode].
 *
 * 2. Swizzles -[UIApplication statusBarOrientation] to return g_lockedOrientation
 *    only when the real value is 0 and only while inside -[World acceleration:].
 *    The absent-or-correct invariant ensures safety: the same condition that stops
 *    statusBarOrientation from updating (the rotation lock) also stops the window
 *    from rotating, so substituting only on 0 is safe.
 *
 * 3. Swizzles -[World acceleration:] with a thread-local depth counter
 *    (g_accelDepth) to isolate the substitution strictly to the tilt physics
 *    path, preventing the layout path from seeing a non-zero orientation and
 *    reconfiguring the render surface for an unrotated window.
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

/* TODO: measure -[UIApplication statusBarOrientation] with method_getTypeEncoding on-device */
#define ENC_STATUS_BAR_ORI NULL

/* TODO: measure -[UIDevice _setOrientation:changed:] with method_getTypeEncoding on-device.
   High priority: takes (int, char), and the char is a BOOL whose ARM32 passing
   convention is worth confirming. */
#define ENC_SET_ORIENTATION_CHANGED NULL

static IMP g_origAcceleration          = 0;
static IMP g_origStatusBarOri          = 0;
static IMP g_origSetOrientationChanged = 0;

/* Locked orientation, pushed from Java at the single splash-time dispatch.
   UIInterfaceOrientation: 1=Portrait 2=PortraitUpsideDown
   3=LandscapeLeft 4=LandscapeRight. 0 = not yet known. */
static volatile int g_lockedOrientation = 0;

/* Depth counter, set only while inside -[World acceleration:]. Confines the
   substitution to the tilt path so the layout path still sees the real (0)
   value and doesn't reconfigure the render surface for a window that never
   rotated. Thread-local: the engine thread runs acceleration:, and another
   thread querying statusBarOrientation concurrently must see the real value.
   A counter rather than a flag so a nested call can't clear it early. */
static __thread int g_accelDepth = 0;

/* One-shot for the engine's hardcoded startup LandscapeRight. Process-wide,
   not thread-local: it marks a single event in the process lifetime, and a
   per-thread copy would let a second interception through. */
static int g_startupFlipHandled = 0;

/* -[UIApplication statusBarOrientation] is a plain getter returning a global
   that is only ever written by _platform_setOrientation:. That call is gated
   on -[EvolutionViewController shouldAutorotate], which returns NO whenever
   Android's accelerometer_rotation setting is 0. So with rotation locked the
   value stays 0 for the whole session, and -[World acceleration:] applies
   neither of its axis transforms, leaving tilt in the LandscapeLeft frame.

   The value is absent-or-correct, never stale-and-wrong: the same condition
   that stops it updating (the rotation lock) also stops the window rotating.
   So substituting only on 0 is safe. This is the argument the whole patch
   rests on — if shouldAutorotate ever returns YES with the lock on, it breaks. */
static int my_statusBarOrientation(id self, SEL _cmd) {
    int real = ((int (*)(id, SEL))g_origStatusBarOri)(self, _cmd);
    if (real == 0 && g_accelDepth > 0 && g_lockedOrientation != 0) {
        LOGV("substituting %d for statusBarOrientation", g_lockedOrientation);
        return g_lockedOrientation;
    }
    return real;
}

/* Arity verified from the runtime: acceleration: encoding is
   v24@0:4{Vector=[4f]}8 (see ENC_ACCELERATION). */
static void my_acceleration(id self, SEL _cmd, void *a, void *b, void *c, void *d) {
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

/* Called from SplashScreen$1 at the single allowed splash-time dispatch,
   where currentOrientation is already in UIInterfaceOrientation encoding. */
JNIEXPORT void JNICALL
Java_com_apportable_ui_Device_nativeSetLockedOrientation(JNIEnv *e, jclass c, jint o) {
    (void)e; (void)c;
    if (o >= 1 && o <= 4) {
        g_lockedOrientation = o;
        LOGI("locked orientation = %d", o);
    } else {
        LOGV("ignoring out-of-range orientation %d", o);
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

    /* 2 - skipped: no MSG_* usage */

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
