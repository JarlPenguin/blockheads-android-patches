#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <android/log.h>

/* Set to 1 to re-enable per-event diagnostics. */
#define AUDIOFIX_VERBOSE 0

#define TAG "AUDIOFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if AUDIOFIX_VERBOSE
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

#define MSG_id(o,s)         (((id     (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_d(o,s)          (((double (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_c(o,s)          (((char   (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_v(o,s)          (((void   (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_v_id_d(o,s,a,b) (((void   (*)(id,SEL,id,double))g_msgSend)((o),(s),(a),(b)))

static uint32_t g_ivarMp3Player  = 0;
static uint32_t g_ivarFadingOut  = 0;
static uint32_t g_ivarOtherAudio = 0;
static SEL      g_selCurrentTime = 0;
static SEL      g_selIsPlaying   = 0;

static IMP g_origDealloc = 0;
static IMP g_origLoad    = 0;
static IMP g_origPlay    = 0;
static IMP g_origPause   = 0;
static IMP g_origStop    = 0;

static id     g_savedPath   = 0;
static double g_savedTime   = 0.0;
static double g_lastTime    = 0.0;
static int    g_gameLoading = 0;
static int    g_needRestart = 0;
static int    g_restoring   = 0;

/* Positions below this aren't worth restoring; restarting from the top is
   indistinguishable and avoids degenerate rapid-cycle behaviour. */
#define MIN_RESTORE_SECONDS 0.0

static id sndMgr(void) {
    id cls = getClass("MJSoundManager");
    return cls ? MSG_id(cls, selReg("instance")) : 0;
}

static id mp3Player(void) {
    id mgr = sndMgr();
    if (!mgr || !g_ivarMp3Player) return 0;
    return *(id *)((char *)mgr + g_ivarMp3Player);
}

static void clearPending(void) {
    if (g_savedPath) { MSG_v(g_savedPath, selReg("release")); g_savedPath = 0; }
    g_savedTime   = 0.0;
    g_needRestart = 0;
}

/* ---- position capture ----
   The only moment the position is readable: -[AVAudioPlayer currentTime]
   returns a stale seek target once _platform_isPlaying: goes false, and by
   dealloc the Java MediaPlayer is already stopped. */

static char my_play(id self, SEL _cmd) {
    g_lastTime = 0.0;          /* new playback starts fresh (g_savedTime is separate) */
    LOGV("play %p", self);
    return ((char (*)(id, SEL))g_origPlay)(self, _cmd);
}

static void my_pause(id self, SEL _cmd) {
    int playing = MSG_c(self, g_selIsPlaying) & 1;
    double t = MSG_d(self, g_selCurrentTime);
    if (self == mp3Player() && playing && t > 0.0) g_lastTime = t;
    LOGV("pause %p playing=%d t=%.3f", self, playing, t);
    ((void (*)(id, SEL))g_origPause)(self, _cmd);
}

static void my_stop(id self, SEL _cmd) {
    int playing = MSG_c(self, g_selIsPlaying) & 1;
    double t = MSG_d(self, g_selCurrentTime);
    if (self == mp3Player() && playing && t > 0.0) g_lastTime = t;
    LOGV("stop %p playing=%d t=%.3f loading=%d", self, playing, t, g_gameLoading);
    ((void (*)(id, SEL))g_origStop)(self, _cmd);
}

/* ---- capture on teardown ---- */

static void my_dealloc(id self, SEL _cmd) {
    id mgr = sndMgr();
    id cur = (mgr && g_ivarMp3Player) ? *(id *)((char *)mgr + g_ivarMp3Player) : 0;
    if (self && self == cur) {
        int fading = (mgr && g_ivarFadingOut) ? (*((char *)mgr + g_ivarFadingOut) & 1) : 0;
        id path = MSG_id(mgr, selReg("lastPlayedMP3Path"));

        /* A dealloc inside loadMP3IfSafe: or during a fade is the game
           changing tracks, not a suspend - the position is meaningless. */
        if (path && g_lastTime > MIN_RESTORE_SECONDS && !fading && !g_gameLoading) {
            if (g_savedPath) MSG_v(g_savedPath, selReg("release"));
            g_savedPath   = MSG_id(path, selReg("retain"));
            g_savedTime   = g_lastTime;
            g_needRestart = 1;
            LOGI("captured t=%.3f", g_savedTime);
        } else {
            LOGV("skipping restore (path=%p t=%.3f fading=%d loading=%d)",
                 path, g_lastTime, fading, g_gameLoading);
            clearPending();
        }
        /* nothing else nils this ivar; readers would otherwise see freed memory */
        *(id *)((char *)mgr + g_ivarMp3Player) = 0;
    }
    ((void (*)(id, SEL))g_origDealloc)(self, _cmd);
}

/* ---- fills in the game's empty -[MJSoundManager restartMusicAfterActiveEvent:] ----
   Noodlecake left this a no-op because on iOS the audio-session interruption
   path handled resume; that path never fires on Android. */

static void my_restart(id self, SEL _cmd, int flag) {
    id cur = g_ivarMp3Player ? *(id *)((char *)self + g_ivarMp3Player) : 0;
    (void)_cmd; (void)flag;

    if (!g_needRestart || !g_savedPath) return;
    if (cur) { LOGV("player already live, skipping"); clearPending(); return; }

    double t = g_savedTime;
    g_restoring = 1;
    MSG_v_id_d(self, selReg("playMP3IfSafe:withTimeOffset:"), g_savedPath, t);
    g_restoring = 0;
    LOGI("restarted t=%.3f", t);

    clearPending();
}

/* The game latches otherAudioWasPaying once at init and never recomputes it
   (attemptToReinitializeAudio is unreachable on Android), so launching with
   other audio playing muted music for the whole session. Category is Ambient,
   so mixing is expected; let the user's volume slider decide instead. */
static char my_safeToPlayMP3s(id self, SEL _cmd) {
    (void)_cmd;
    if (g_ivarOtherAudio) *((char *)self + g_ivarOtherAudio) = 0;
    return 1;
}

/* A game-initiated load supersedes any pending restore, and marks the window
   in which a dealloc means "track change" rather than "suspend". */
static void my_load(id self, SEL _cmd, id path, double offset) {
    if (g_needRestart && !g_restoring) { LOGV("game load - dropping pending"); clearPending(); }
    g_gameLoading = 1;
    ((void (*)(id, SEL, id, double))g_origLoad)(self, _cmd, path, offset);
    g_gameLoading = 0;
}

static int hook(id cls, const char *sel, IMP repl, IMP *orig) {
    Method m = getInstMethod(cls, selReg(sel));
    if (!m) { LOGE("method not found: %s", sel); return 0; }
    if (orig) *orig = getImp(m);
    setImp(m, repl);
    return 1;
}

static int install(void) {
    id avp = getClass("AVAudioPlayer");
    id snd = getClass("MJSoundManager");
    if (!avp || !snd) { LOGE("classes missing at install"); return 0; }

    void *h = dlopen("libApplication.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) { LOGE("libApplication not loaded: %s", dlerror()); return 0; }

    uint32_t *off = (uint32_t *)dlsym(h, "OBJC_IVAR_$_MJSoundManager.mp3Player");
    if (!off) { LOGE("mp3Player ivar not found: %s", dlerror()); return 0; }
    g_ivarMp3Player = *off;

    uint32_t *fo = (uint32_t *)dlsym(h, "OBJC_IVAR_$_MJSoundManager.fadingOut");
    if (fo) g_ivarFadingOut = *fo;
    else    LOGE("fadingOut ivar not found - fade guard disabled");

    uint32_t *oa = (uint32_t *)dlsym(h, "OBJC_IVAR_$_MJSoundManager.otherAudioWasPaying");
    if (oa) g_ivarOtherAudio = *oa;
    else    LOGE("otherAudioWasPaying ivar not found");

    g_selCurrentTime = selReg("currentTime");
    g_selIsPlaying   = selReg("isPlaying");

    Method d = getInstMethod(avp, selReg("dealloc"));
    if (d && getImp(d) == (IMP)my_dealloc) { LOGI("already installed"); return 1; }

    if (!hook(avp, "dealloc", (IMP)my_dealloc, &g_origDealloc)) return 0;
    if (!hook(snd, "restartMusicAfterActiveEvent:", (IMP)my_restart, 0)) return 0;

    hook(avp, "play",  (IMP)my_play,  &g_origPlay);
    hook(avp, "pause", (IMP)my_pause, &g_origPause);
    hook(avp, "stop",  (IMP)my_stop,  &g_origStop);
    hook(snd, "loadMP3IfSafe:withTimeOffset:", (IMP)my_load, &g_origLoad);
    hook(snd, "safeToPlayMP3s", (IMP)my_safeToPlayMP3s, 0);

    LOGI("installed (mp3Player=%u fadingOut=%u otherAudio=%u)",
         g_ivarMp3Player, g_ivarFadingOut, g_ivarOtherAudio);
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
    g_msgSend     = dlsym(sys, "objc_msgSend");

    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp || !g_msgSend) {
        LOGE("runtime symbols missing"); return;
    }
    if (!getClass("MJSoundManager") || !getClass("AVAudioPlayer")) {
        LOGE("classes not present - load point too early"); return;
    }

    install();
}
