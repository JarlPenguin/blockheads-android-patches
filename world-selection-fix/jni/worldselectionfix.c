#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <android/log.h>

/* Set to 1 to re-enable per-event diagnostics. */
#define WORLDSELECTIONFIX_VERBOSE 0

#define TAG "WORLDSELECTIONFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if WORLDSELECTIONFIX_VERBOSE
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

static uint32_t g_ivarCurrentWorldIndex = 0;

/* Kept resolved but deliberately never called - flip the early return in
   my_menuReset() to restore stock behaviour without rebuilding the hook. */
static IMP g_origMenuReset = 0;

static int worldIndex(id self) {
    return *(int *)((char *)self + g_ivarCurrentWorldIndex);
}

/* ---- the fix ----
   -[GameView didBecomeActive] calls -[MainMenuUI selectMostRecentlyPlayedWorld]
   whenever totalGamePlayTimePassed >= 1.0 && mainMenuUI != nil. That ivar is a
   persisted lifetime counter loaded in -[GameView init], so the only effective
   gate is "the menu object exists" - true on every foreground transition.

   Despite the name, the method selects nothing. It writes exactly the values
   -[MainMenuUI initWithDelegate:windowInfo:cache:cloudInterface:] already
   writes at construction:

       currentWorldIndex         = -2
       currentMainMenuSelection  =  1
       currentScroll             = +/-const * (0.75|1.0), by gameSaves.count
       scrollTargetIndex         = -3
       activePreviewTextureIndex = -1

   It is a reset-to-initial-menu-state routine; the name reflects only that the
   default scroll position happens to land on the most recent world given the
   list ordering. So on resume it discards whatever the user navigated to, and
   bounces them out of the Join Server UI, because re-selecting drives
   currentWorldChanged: down into LoadWorldUI.

   Cold start does not need it - the initializer produces the same state - so
   this suppresses the call outright rather than guarding it. Note that
   -[MainMenuUI gameSavesChanged] runs immediately before it on the same path
   and already performs the preview-texture release and the
   activePreviewTextureIndex = -1 that this method would otherwise do, so
   nothing necessary is lost. */
static void my_menuReset(id self, SEL _cmd) {
    LOGV("suppressed menu reset (idx=%d)", worldIndex(self));
    (void)self; (void)_cmd;
}

static int hook(id cls, const char *sel, IMP repl, IMP *orig) {
    Method m = getInstMethod(cls, selReg(sel));
    if (!m) { LOGE("method not found: %s", sel); return 0; }
    if (orig) *orig = getImp(m);
    setImp(m, repl);
    return 1;
}

static int install(void) {
    id menu = getClass("MainMenuUI");
    if (!menu) { LOGE("MainMenuUI missing at install"); return 0; }

    void *h = dlopen("libApplication.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) { LOGE("libApplication not loaded: %s", dlerror()); return 0; }

    uint32_t *off = (uint32_t *)dlsym(h, "OBJC_IVAR_$_MainMenuUI.currentWorldIndex");
    if (!off) { LOGE("currentWorldIndex ivar not found: %s", dlerror()); return 0; }
    g_ivarCurrentWorldIndex = *off;

    Method m = getInstMethod(menu, selReg("selectMostRecentlyPlayedWorld"));
    if (m && getImp(m) == (IMP)my_menuReset) { LOGI("already installed"); return 1; }

    if (!hook(menu, "selectMostRecentlyPlayedWorld",
              (IMP)my_menuReset, &g_origMenuReset)) return 0;

    LOGI("installed (currentWorldIndex=%u)", g_ivarCurrentWorldIndex);
    return 1;
}

__attribute__((constructor))
static void init(void) {
    static int once = 0;
    if (__sync_val_compare_and_swap(&once, 0, 1) != 0) return;

    /* Same load point as audiofix: after LibraryManager.loadLibraries().
       RTLD_NOLOAD turns a wrong load point into a clean error instead of a
       second ObjC runtime. */
    void *sys = dlopen("libSystem.so", RTLD_NOW | RTLD_NOLOAD);
    if (!sys) { LOGE("libSystem not loaded - load point too early"); return; }

    selReg        = dlsym(sys, "sel_registerName");
    getClass      = dlsym(sys, "objc_getClass");
    getInstMethod = dlsym(sys, "class_getInstanceMethod");
    setImp        = dlsym(sys, "method_setImplementation");
    getImp        = dlsym(sys, "method_getImplementation");

    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp) {
        LOGE("runtime symbols missing"); return;
    }
    if (!getClass("MainMenuUI")) {
        LOGE("MainMenuUI not present - load point too early"); return;
    }

    install();
}
