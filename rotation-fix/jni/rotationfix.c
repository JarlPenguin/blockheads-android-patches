#include <dlfcn.h>
#include <stdint.h>
#include <jni.h>
#include <android/log.h>

/* Set to 1 to re-enable diagnostics. Off by default: the statusBarOrientation
   hook runs at accelerometer rate (~30Hz), so its LOGV would flood logcat. */
#define ROTFIX_VERBOSE 0

#define TAG "ROTFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if ROTFIX_VERBOSE
#define LOGV(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#else
#define LOGV(...) ((void)0)
#endif

typedef void *id;
typedef void *SEL;
typedef void *Method;
typedef void *IMP;

/* --- ObjC runtime, resolved from libSystem.so at load time --- */
static SEL          (*selReg)(const char *);
static id           (*getClass)(const char *);
static Method       (*getInstMethod)(id, SEL);
static IMP          (*setImp)(Method, IMP);
static IMP          (*getImp)(Method);
static const char * (*getTypeEnc)(Method);

static IMP g_origAcceleration = 0;
static IMP g_origSetOrientationChanged = 0;
static IMP g_origStatusBarOri = 0;

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
   So substituting only on 0 is safe. */
static int my_statusBarOrientation(id self, SEL _cmd) {
    int real = ((int (*)(id, SEL))g_origStatusBarOri)(self, _cmd);
    if (real == 0 && g_accelDepth > 0 && g_lockedOrientation != 0) {
        LOGV("substituting %d for statusBarOrientation", g_lockedOrientation);
        return g_lockedOrientation;
    }
    return real;
}

/* Arity verified from the runtime: acceleration: encoding is
   v24@0:4{Vector=[4f]}8 — a 16-byte Vector (four floats) passed by value,
   which AAPCS splits across r2-r3 plus two stack words. The four
   pass-through args cover it exactly. */
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

static int hook(id cls, const char *sel, IMP repl, IMP *orig) {
    Method m = getInstMethod(cls, selReg(sel));
    if (!m) { LOGE("method not found: %s", sel); return 0; }
    if (orig) *orig = getImp(m);
    setImp(m, repl);
    return 1;
}

static int install(void) {
    id world = getClass("World");
    id app   = getClass("UIApplication");
    if (!world || !app) { LOGE("classes not present - load point too early"); return 0; }

    Method m = getInstMethod(world, selReg("acceleration:"));
    if (!m) { LOGE("acceleration: not found"); return 0; }
    if (getImp(m) == (IMP)my_acceleration) { LOGI("already installed"); return 1; }

    if (getTypeEnc) LOGV("acceleration: encoding = %s", getTypeEnc(m));

    if (!hook(world, "acceleration:", (IMP)my_acceleration, &g_origAcceleration)) return 0;
    if (!hook(app, "statusBarOrientation",
              (IMP)my_statusBarOrientation, &g_origStatusBarOri)) return 0;

    id device = getClass("UIDevice");
    if (!device) { LOGE("UIDevice not present - load point too early"); return 0; }
    if (!hook(device, "_setOrientation:changed:",
              (IMP)my_setOrientationChanged, &g_origSetOrientationChanged)) return 0;

    LOGI("installed");
    return 1;
}

__attribute__((constructor))
static void init(void) {
    static int once = 0;
    if (__sync_val_compare_and_swap(&once, 0, 1) != 0) return;

    /* Loaded from BackgroundLibraryLoader$1.run(), after
       LibraryManager.loadLibraries(). RTLD_NOLOAD turns a wrong load point
       into a clean error instead of a second ObjC runtime. */
    void *sys = dlopen("libSystem.so", RTLD_NOW | RTLD_NOLOAD);
    if (!sys) { LOGE("libSystem not loaded - load point too early"); return; }

    selReg        = dlsym(sys, "sel_registerName");
    getClass      = dlsym(sys, "objc_getClass");
    getInstMethod = dlsym(sys, "class_getInstanceMethod");
    setImp        = dlsym(sys, "method_setImplementation");
    getImp        = dlsym(sys, "method_getImplementation");
    getTypeEnc    = dlsym(sys, "method_getTypeEncoding");   /* optional */

    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp) {
        LOGE("runtime symbols missing"); return;
    }

    install();
}
