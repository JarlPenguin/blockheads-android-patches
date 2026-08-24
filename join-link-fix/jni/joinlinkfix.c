/*
 * joinlinkfix - native join-link delivery for the Apportable build of
 * The Blockheads.
 *
 * THE BUG
 * -------
 * Java_..._nativeHandleUri() hands the launch URL to VerdeHandleURI(), which
 * only stashes a copy in a native global. The actual handoff to the app
 * delegate happens in a separate routine that is latched by a one-shot flag
 * and runs for the first time from inside -[UIApplication run].
 *
 * At that point UIApplication has no delegate yet, so both
 *   application:handleOpenURL:
 *   application:openURL:sourceApplication:annotation:
 * dispatches are skipped - but the routine still does:
 *
 *     [stashedURL release]; stashedURL = nil;
 *
 * unconditionally. The URL is destroyed and the latch closes, so no later
 * delivery is ever forwarded. Only a background/foreground round trip resets
 * the latch, which is why the join screen used to appear only after the user
 * pulled down the notification shade.
 *
 * THE FIX
 * -------
 * Skip that path entirely.
 *
 *   1. VerdeHandleURI is an exported pointer-to-function variable. We replace
 *      it, keeping our own retained copy of the URL. The engine's global is
 *      left nil, so its latched handoff becomes a no-op and cannot
 *      double-deliver.
 *
 *   2. -[GameView delayedOpenURLIfNeeded] is invoked from -[GameView render:]
 *      on every frame, with the correct GameView as self. We swizzle it: once
 *      mainMenuUI exists, we push the URL in through -[GameView
 *      handleOpenURL:] (which sets urlToOpenOnceLoaded) and then chain to the
 *      original, which consumes it on that same call.
 *
 * Only exported symbols are used - no hardcoded offsets - so this survives a
 * rebuild of libApplication.so.
 *
 * LOAD POINT
 * ----------
 * Must be loaded after LibraryManager.loadLibraries() and before
 * VerdeActivity$3.run() calls nativeHandleUri. Adding
 *
 *     const-string vN, "joinlinkfix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * immediately after the loadLibraries() call in BackgroundLibraryLoader$1.run()
 * satisfies both: that runs on MainThread, while $3.run() is posted to the UI
 * thread only once loadFinished() has been dispatched back.
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libjoinlinkfix.so joinlinkfix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <android/log.h>

/* Set to 1 for per-delivery diagnostics. */
#define JOINLINKFIX_VERBOSE 0

#define TAG "JOINLINKFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if JOINLINKFIX_VERBOSE
#define LOGV(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#else
#define LOGV(...) ((void)0)
#endif

typedef void *id;
typedef void *SEL;
typedef void *Method;
typedef void *IMP;

/* --- ObjC runtime, resolved from libSystem.so at load time --- */
static SEL    (*selReg)(const char *);
static id     (*getClass)(const char *);
static Method (*getInstMethod)(id, SEL);
static IMP    (*setImp)(Method, IMP);
static IMP    (*getImp)(Method);
static void   *g_msgSend;

#define MSG_id(o,s)        (((id   (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_v(o,s)         (((void (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_c_id(o,s,a)    (((char (*)(id,SEL,id))g_msgSend)((o),(s),(a)))

static uint32_t g_ivarMainMenuUI = 0;

static IMP g_origDelayedOpen = 0;
static void *(*g_origVerdeHandleURI)(void *) = 0;

static id              g_pendingURL = 0;
static pthread_mutex_t g_lock       = PTHREAD_MUTEX_INITIALIZER;

/* ---- capture ----
   Replaces the engine's VerdeHandleURI. Called from
   Java_..._nativeHandleUri on whichever thread delivered the intent (the
   Android UI thread on cold boot, MainThread on the warm path).

   We deliberately do NOT chain to the original: leaving the engine's global
   nil keeps its latched handoff inert, so there is exactly one delivery path
   and no chance of the URL being applied twice. */
static void *my_VerdeHandleURI(void *url) {
    id copy = url ? MSG_id((id)url, selReg("copy")) : 0;

    pthread_mutex_lock(&g_lock);
    if (g_pendingURL) MSG_v(g_pendingURL, selReg("release"));
    g_pendingURL = copy;
    pthread_mutex_unlock(&g_lock);

    LOGI("captured url %p", copy);
    return copy;
}

/* ---- delivery ----
   -[GameView render:] calls this unconditionally every frame, before it does
   anything else, so self is always the live GameView.

   We wait for mainMenuUI to exist, which means delivery lands on the second
   frame. Delivering on the first is possible - the original
   delayedOpenURLIfNeeded will construct a MainMenuUI itself when the ivar is
   nil - but it skips the populateGameSaves() call that render: makes just
   before building the menu, and render: then sees a non-nil mainMenuUI and
   never populates. Calling populateGameSaves() ourselves to compensate does
   fill the list, but running it that early leaves the menu in a state that
   navigates away from the Join Server screen to the world list.

   So: one frame of visible main menu, in exchange for the game initialising
   in the order it was written to. */
static void my_delayedOpenURLIfNeeded(id self, SEL _cmd) {
    id url = 0;

    if (g_pendingURL && g_ivarMainMenuUI) {
        id menu = *(id *)((char *)self + g_ivarMainMenuUI);
        if (menu) {
            pthread_mutex_lock(&g_lock);
            url = g_pendingURL;
            g_pendingURL = 0;
            pthread_mutex_unlock(&g_lock);
        }
    }

    if (url) {
        /* sets GameView.urlToOpenOnceLoaded; the original call below
           consumes it on this same frame */
        MSG_c_id(self, selReg("handleOpenURL:"), url);
        MSG_v(url, selReg("release"));
        LOGI("delivered url to %p", self);
    }

    ((void (*)(id, SEL))g_origDelayedOpen)(self, _cmd);
}

static int hook(id cls, const char *sel, IMP repl, IMP *orig) {
    Method m = getInstMethod(cls, selReg(sel));
    if (!m) { LOGE("method not found: %s", sel); return 0; }
    if (orig) *orig = getImp(m);
    setImp(m, repl);
    return 1;
}

static int install(void) {
    id gameView = getClass("GameView");
    if (!gameView) { LOGE("GameView missing at install"); return 0; }

    void *h = dlopen("libApplication.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) { LOGE("libApplication not loaded: %s", dlerror()); return 0; }

    uint32_t *mm = (uint32_t *)dlsym(h, "OBJC_IVAR_$_GameView.mainMenuUI");
    if (!mm) { LOGE("mainMenuUI ivar not found: %s", dlerror()); return 0; }
    g_ivarMainMenuUI = *mm;


    Method d = getInstMethod(gameView, selReg("delayedOpenURLIfNeeded"));
    if (d && getImp(d) == (IMP)my_delayedOpenURLIfNeeded) {
        LOGI("already installed");
        return 1;
    }

    /* Order matters: only start swallowing URLs once we can actually deliver
       them, so a failed swizzle leaves vanilla behaviour intact. */
    if (!hook(gameView, "delayedOpenURLIfNeeded",
              (IMP)my_delayedOpenURLIfNeeded, &g_origDelayedOpen)) {
        return 0;
    }

    void **pVerdeHandleURI = (void **)dlsym(h, "VerdeHandleURI");
    if (!pVerdeHandleURI) {
        LOGE("VerdeHandleURI not found: %s", dlerror());
        setImp(getInstMethod(gameView, selReg("delayedOpenURLIfNeeded")),
               g_origDelayedOpen);
        return 0;
    }
    g_origVerdeHandleURI = (void *(*)(void *))*pVerdeHandleURI;
    *pVerdeHandleURI = (void *)my_VerdeHandleURI;

    LOGI("installed (mainMenuUI=%u origVerdeHandleURI=%p)",
         g_ivarMainMenuUI, (void *)g_origVerdeHandleURI);
    return 1;
}

__attribute__((constructor))
static void init(void) {
    static int once = 0;
    if (__sync_val_compare_and_swap(&once, 0, 1) != 0) return;

    /* RTLD_NOLOAD turns a wrong load point into a clean error instead of a
       second ObjC runtime. */
    void *sys = dlopen("libSystem.so", RTLD_NOW | RTLD_NOLOAD);
    if (!sys) { LOGE("libSystem not loaded - load point too early"); return; }

    selReg        = dlsym(sys, "sel_registerName");
    getClass      = dlsym(sys, "objc_getClass");
    getInstMethod = dlsym(sys, "class_getInstanceMethod");
    setImp        = dlsym(sys, "method_setImplementation");
    getImp        = dlsym(sys, "method_getImplementation");
    g_msgSend     = dlsym(sys, "objc_msgSend");

    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp || !g_msgSend) {
        LOGE("runtime symbols missing");
        return;
    }
    if (!getClass("GameView")) {
        LOGE("GameView not registered - load point too early");
        return;
    }

    install();
}
