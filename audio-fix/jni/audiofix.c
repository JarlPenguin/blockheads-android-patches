/*
 * libaudiofix.so
 *
 * THE BUG
 * -------
 * Additional, log-confirmed suspend defect: on a server-client suspend,
 * Java's AudioPlayer.lifecycleSuspend completes before GameView's
 * didEnterBackground exits the world. That world-exit sends
 * playMP3IfSafe:withTimeOffset: for mountainKingLoop.mp4 and creates a new
 * AVAudioPlayer after the one-shot Java pause sweep. It then plays while
 * backgrounded. The measured case used one thread and no fade/pending
 * restore. This does not establish the older transition-time defect.
 *
 * Music never comes back after the game is backgrounded, and the volume
 * slider stops affecting it if you work around that from the Java side.
 *
 * -[AVAudioPlayer dealloc] is the sole sender of -_platform_unload:, which
 * is what releases the Java MediaPlayer slot. So the music player dies on
 * background. Nothing restarts it: -[GameView didBecomeActive] calls
 * -[MJSoundManager restartMusicAfterActiveEvent:], and that method is an
 * empty stub - a compiled prologue and epilogue with no body.
 *
 * It is empty because on iOS the audio-session interruption path handled
 * resume. That path exists on both sides here and never connects:
 *   - -[AVAudioPlayer(Platform) _platform_focus_changed:] correctly maps
 *     Android focus constants onto _beginInterruption / _endInterruption.
 *   - -[MJSoundManager reinitialize], reached from -initWithMasterVolume:,
 *     does register for AVAudioSessionInterruptionNotification.
 *   - But -[AndroidAudioManager audioFocusChange:] never fires and its
 *     delegate is nil, so nothing propagates.
 *   - And -[AVAudioSession _beginInterruption] sends the deprecated
 *     AVAudioSessionDelegate method -beginInterruption where the game
 *     listens for the modern notification, so the two ends would not have
 *     met even with focus working.
 *
 * Two further bugs in the same subsystem:
 *   - -[MJSoundManager safeToPlayMP3s] is a single read of the
 *     otherAudioWasPaying ivar. -reinitialize latches that once at startup
 *     from -isOtherAudioPlaying and never recomputes it, because the only
 *     path that would (-attemptToReinitializeAudio) is unreachable.
 *     Launching while another app plays audio therefore muted game music
 *     for the whole session.
 *   - Player teardown outside -stopMP3Playback can leave
 *     MJSoundManager.mp3Player dangling. That normal nil-writing method
 *     was believed to have no callers. The new trace reaches it during
 *     a fade and on suspend, superseding that conclusion. Other teardown
 *     paths can still leave the ivar dangling; the existing guard remains.
 *     The affected readers include -setMusicVolume, -isPlayingMP3, -currentMP3time
 *     and -setLoopMP3s, which can read freed memory after unguarded teardown.
 *
 * THE FIX
 * -------
 * NEW ADDITION (clearly delimited below): set background state on entry
 * to GameView.didEnterBackground; clear it before GameView.didBecomeActive.
 * Defer game-originated playMP3IfSafe:withTimeOffset: and
 * loadMP3IfSafe:withTimeOffset: while backgrounded. Retain only the latest
 * requested path/offset and invalidate the stale position restore.
 * Restart consumes that request before considering the old track snapshot,
 * routing it through the game's own play/loader with g_restoreDepth.
 * A newer foreground game request supersedes the deferred request.
 * No new stop or fade cancellation is introduced: those need evidence for
 * the remaining in-game/transition symptom, which is left open.
 * Client, single-player, host, volume/mute and repeated-cycle checks passed
 * on the user's device with this behavior in the candidate build.
 * Production keeps these decisions and removes observation-only hooks.
 *
 * EXISTING FIX (playback decisions otherwise unchanged):
 * Fill in the empty -restartMusicAfterActiveEvent: with a call to the
 * game's own -playMP3IfSafe:withTimeOffset:, using the path retained from
 * -lastPlayedMP3Path and the position captured at teardown. Routing
 * through the game's own loader means native retains ownership of the new
 * AVAudioPlayer, so the in-game music volume slider still affects it -
 * that was the defect in the earlier Java-side workaround, where the
 * retained player was invisible to native.
 *
 * The position is captured in -[AVAudioPlayer stop], which fires ~1 ms
 * before -dealloc on the same player. That is the only moment it is
 * readable: -[AVAudioPlayer currentTime] returns the cached seek target
 * written by -setCurrentTime: whenever -_platform_isPlaying: is false, and
 * by -dealloc the Java MediaPlayer has already been stopped.
 *
 * Deliberately NOT done:
 *   - No attempt to repair audio-focus delivery. That is the root cause of
 *     the dead interruption path, but it is Java-side work and fixing it
 *     would re-activate -handleInterruption: and -attemptToReinitializeAudio,
 *     changing behaviour this library has been tuned around.
 *   - The OpenAL context is still never suspended on background, since
 *     -interruptionBegan never fires. No audible symptom was found.
 *   - Two hooks are terminal and never chain (see install()).
 *
 * Known limitation: music continues playing if the game is suspended at a
 * specific moment during a track transition. Reproduces on a clean APK on
 * Android 8.1, so it predates this library.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "audiofix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * libSystem.so and libApplication.so must both be up, so MJSoundManager and
 * AVAudioPlayer are registered. It must be no earlier: loading from
 * VerdeApplication.<clinit> initiates libSystem itself, in a different
 * linker namespace, and races class registration - observed as a black
 * screen with the game otherwise alive.
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libaudiofix.so audiofix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>
#include <pthread.h>


#ifndef AUDIOFIX_VERBOSE
#define AUDIOFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef AUDIOFIX_STRICT_ENCODING
#define AUDIOFIX_STRICT_ENCODING 1
#endif

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

#if AUDIOFIX_STRICT_ENCODING
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


/* --- per-file content ------------------------------------------------ */

/* File-specific: -playMP3IfSafe:withTimeOffset: takes an object and a
   double, which no canonical MSG_* macro covers. Defined here rather than
   in the shared block so the shared block stays byte-identical. */
#define MSG_v_id_d(o,s,a,b) (((void (*)(id,SEL,id,double))g_msgSend)((o),(s),(a),(b)))

/* Encodings measured in the supplied application.objc, not guessed.
   Every hook checks its encoding in step 6 AND in hook(). The four
   AVAudioPlayer encodings are now measured in the device log and filled in.

   The original source warned that wrong arity for object/double/int arguments
   corrupts the stack rather than merely passing a bad pointer. The measured
   restart flag is char, so its wrapper now declares char (flag remains ignored
   by the playback logic). All ten production hooks use measured encodings. */
#define ENC_NOARGS       "v8@0:4"
#define ENC_CHAR         "v12@0:4c8"
#define ENC_PATH_OFFSET  "v20@0:4@8d12"
#define ENC_SAFE         "c8@0:4"
/* Measured in the supplied 2026-10-12 device log. */
#define ENC_AV_VOID      "v8@0:4"
#define ENC_AV_PLAY      "c8@0:4"

static uint32_t g_ivarMp3Player  = 0;
static uint32_t g_ivarFadingOut  = 0;
static uint32_t g_ivarOtherAudio = 0;
static SEL      g_selCurrentTime = 0;
static SEL      g_selIsPlaying   = 0;

/* ---- NEW FIX ADDITION: lifecycle/request hooks ---- */
static IMP g_origGameBackground, g_origGameActive, g_origMusicPlay;
static int g_backgrounded;
/* ---- END NEW FIX HOOK STATE ---- */

/* ---- NEW FIX ADDITION: deferred background request state ----
   runFade was observed on a separate thread. Protect the retained request
   independently of the existing thread-local load/restore depth counters.
   Never hold this mutex while chaining a game method or releasing a path. */
static pthread_mutex_t g_deferredLock = PTHREAD_MUTEX_INITIALIZER;
static id g_deferredPath;
static double g_deferredOffset;
static int g_hasDeferredRequest;
/* ---- END NEW FIX STATE ---- */

static IMP g_origDealloc = 0;
static IMP g_origLoad    = 0;
static IMP g_origPlay    = 0;
static IMP g_origPause   = 0;
static IMP g_origStop    = 0;

static id     g_savedPath = 0;
static double g_savedTime = 0.0;
static double g_lastTime  = 0.0;
static int    g_needRestart = 0;

/* Depth counters, not flags, and thread-local.

   Counters because these methods nest: my_restart calls
   -playMP3IfSafe:withTimeOffset:, which calls
   -loadMP3IfSafe:withTimeOffset:. A set/clear flag would be cleared by
   the inner call while the outer one is still running.

   Thread-local because the dealloc these guard against is synchronous on
   the loading thread: -loadMP3IfSafe: releases the old player inline
   before creating the new one, and GNUstep autorelease pools are
   per-thread, so even a deferred release drains on the same thread.
   Global and thread-local therefore behave identically in practice;
   thread-local is the safer of the two where they diverge, since a
   stale flag left set by another thread would suppress a real capture.

   Neither form helps if the old player is ever released by something
   outside the load path - that would read as a suspend and restore a
   track the game was replacing. Not observed; worth confirming rather
   than assuming. */
static __thread int g_loadDepth    = 0;
static __thread int g_restoreDepth = 0;

static id sndMgr(void) {
    Class cls = getClass("MJSoundManager");
    return cls ? MSG_id((id)cls, selReg("instance")) : 0;
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

/* ---- NEW FIX ADDITION: background request ownership ---- */
static int musicBackgrounded(void) {
    return __atomic_load_n(&g_backgrounded, __ATOMIC_ACQUIRE);
}

static void discardDeferredRequest(void) {
    id previous;
    pthread_mutex_lock(&g_deferredLock);
    previous = g_deferredPath;
    g_deferredPath = 0;
    g_deferredOffset = 0.0;
    g_hasDeferredRequest = 0;
    pthread_mutex_unlock(&g_deferredLock);
    if (previous) MSG_v(previous, selReg("release"));
}

static int deferBackgroundRequest(id path, double offset) {
    id retained, previous;
    if (g_restoreDepth > 0 || !musicBackgrounded()) return 0;
    retained = path ? MSG_id(path, selReg("retain")) : 0;
    pthread_mutex_lock(&g_deferredLock);
    /* Activity may have changed while retaining the path. */
    if (!musicBackgrounded()) {
        pthread_mutex_unlock(&g_deferredLock);
        if (retained) MSG_v(retained, selReg("release"));
        return 0;
    }
    previous = g_deferredPath;
    g_deferredPath = retained;
    g_deferredOffset = offset;
    g_hasDeferredRequest = 1;  /* distinguishes a nil request from no request */
    pthread_mutex_unlock(&g_deferredLock);
    if (previous) MSG_v(previous, selReg("release"));
    /* A game request supersedes the stale track snapshot even when deferred.
       dealloc can clear that old snapshot later without erasing this request. */
    clearPending();
    return 1;
}

static int takeDeferredRequest(id *path, double *offset) {
    int had;
    pthread_mutex_lock(&g_deferredLock);
    had = g_hasDeferredRequest;
    *path = g_deferredPath;
    *offset = g_deferredOffset;
    g_deferredPath = 0;
    g_deferredOffset = 0.0;
    g_hasDeferredRequest = 0;
    pthread_mutex_unlock(&g_deferredLock);
    return had;  /* transfers our retained ownership to the caller */
}
/* ---- END NEW FIX HELPERS ---- */

/* ---- position capture ----
   The only moment the position is readable: -[AVAudioPlayer currentTime]
   returns a stale seek target once -_platform_isPlaying: goes false, and
   by -dealloc the Java MediaPlayer is already stopped. */

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
    LOGV("stop %p playing=%d t=%.3f loading=%d", self, playing, t, g_loadDepth);
    ((void (*)(id, SEL))g_origStop)(self, _cmd);
}

/* ---- capture on teardown ---- */

static void my_dealloc(id self, SEL _cmd) {
    id mgr = sndMgr();
    id cur = (mgr && g_ivarMp3Player) ? *(id *)((char *)mgr + g_ivarMp3Player) : 0;
    if (self && self == cur) {
        int fading = (mgr && g_ivarFadingOut) ? (*((char *)mgr + g_ivarFadingOut) & 1) : 0;
        id path = MSG_id(mgr, selReg("lastPlayedMP3Path"));

        /* A dealloc inside -loadMP3IfSafe: or during a fade is the game
           changing tracks, not a suspend - the position is meaningless. */
        if (path && g_lastTime > 0.0 && !fading && g_loadDepth <= 0) {
            if (g_savedPath) MSG_v(g_savedPath, selReg("release"));
            g_savedPath   = MSG_id(path, selReg("retain"));
            g_savedTime   = g_lastTime;
            g_needRestart = 1;
            LOGI("captured t=%.3f", g_savedTime);
        } else {
            LOGV("skipping restore (path=%p t=%.3f fading=%d loading=%d)",
                 path, g_lastTime, fading, g_loadDepth);
            clearPending();
        }
        /* Teardown outside -stopMP3Playback otherwise leaves this ivar
           dangling. Preserve the original nil-before-dealloc guard. */
        *(id *)((char *)mgr + g_ivarMp3Player) = 0;
    }
    ((void (*)(id, SEL))g_origDealloc)(self, _cmd);
}

/* ---- fills in the game's empty -[MJSoundManager restartMusicAfterActiveEvent:] ----
   Noodlecake left this a no-op because on iOS the audio-session interruption
   path handled resume; that path never fires on Android.

   Terminal hook: the displaced IMP is an empty stub, so there is nothing
   worth chaining to and orig is deliberately NULL. */

static void my_restart(id self, SEL _cmd, char flag) {
    /* ---- NEW FIX ADDITION: latest deferred request takes precedence ---- */
    id requestedPath;
    double requestedOffset;
    if (musicBackgrounded()) {
        return;
    }
    if (takeDeferredRequest(&requestedPath, &requestedOffset)) {
        g_restoreDepth++;
        MSG_v_id_d(self, selReg("playMP3IfSafe:withTimeOffset:"), requestedPath, requestedOffset);
        g_restoreDepth--;
        if (requestedPath) MSG_v(requestedPath, selReg("release"));
        clearPending();
        return;
    }
    /* ---- END NEW FIX RESTART ADDITION ---- */
    id cur = g_ivarMp3Player ? *(id *)((char *)self + g_ivarMp3Player) : 0;
    (void)_cmd; (void)flag;

    if (!g_needRestart || !g_savedPath) {
        return;
    }
    if (cur) {
        LOGV("player already live, skipping"); clearPending();
        return;
    }

    double t = g_savedTime;
    g_restoreDepth++;
    MSG_v_id_d(self, selReg("playMP3IfSafe:withTimeOffset:"), g_savedPath, t);
    g_restoreDepth--;
    LOGI("restarted t=%.3f", t);

    clearPending();
}

/* The game latches otherAudioWasPaying once at init and never recomputes it
   (-attemptToReinitializeAudio is unreachable on Android), so launching with
   other audio playing muted music for the whole session. Category is Ambient,
   so mixing is expected; let the user's volume slider decide instead.

   Terminal hook: -[MJSoundManager safeToPlayMP3s] is a single read of
   otherAudioWasPaying compared against zero - verified in the disassembly -
   so the unconditional return 1 is exact, not an assumption. orig is
   deliberately NULL. */
static char my_safeToPlayMP3s(id self, SEL _cmd) {
    (void)_cmd;
    if (g_ivarOtherAudio) *((char *)self + g_ivarOtherAudio) = 0;
    return 1;
}

/* A game-initiated load supersedes any pending restore, and marks the window
   in which a dealloc means "track change" rather than "suspend". */
static void my_load(id self, SEL _cmd, id path, double offset) {
    /* ---- NEW FIX ADDITION: prevent both loading AND the ensuing play ---- */
    if (deferBackgroundRequest(path, offset)) {
        return;
    }
    if (g_restoreDepth <= 0) discardDeferredRequest();
    /* ---- END NEW FIX REQUEST GATE ---- */
    if (g_needRestart && g_restoreDepth <= 0) {
        LOGV("game load - dropping pending");
        clearPending();
    }
    g_loadDepth++;
    ((void (*)(id, SEL, id, double))g_origLoad)(self, _cmd, path, offset);
    g_loadDepth--;
}

/* ---- NEW FIX ADDITION: lifecycle state and upstream play gate ---- */
static void my_gameBackground(id self, SEL cmd) {
    /* NEW FIX: set before the original callback can exit the world. */
    __atomic_store_n(&g_backgrounded, 1, __ATOMIC_RELEASE);
    ((void (*)(id,SEL))g_origGameBackground)(self, cmd);
}

static void my_gameActive(id self, SEL cmd) {
    /* NEW FIX: clear before the original callback calls restart. */
    __atomic_store_n(&g_backgrounded, 0, __ATOMIC_RELEASE);
    ((void (*)(id,SEL))g_origGameActive)(self, cmd);
}


static void my_musicPlay(id self, SEL cmd, id path, double offset) {
    /* ---- NEW FIX ADDITION: prevent both loading AND the ensuing play ---- */
    if (deferBackgroundRequest(path, offset)) {
        return;
    }
    if (g_restoreDepth <= 0) discardDeferredRequest();
    /* ---- END NEW FIX REQUEST GATE ---- */
    ((void (*)(id,SEL,id,double))g_origMusicPlay)(self, cmd, path, offset);
}


/* ---- END NEW FIX HOOKS ---- */

static int install(void) {
    Class avp, snd, game;
    const char *howMp3, *howFade, *howOther;
    unsigned i, committed = 0;
    struct HookSpec {
        Class cls;
        const char *selector, *encoding;
        IMP replacement;
        IMP *original;
        Method method;
        IMP rollback;
    };

    /* 1 */
    avp = getClass("AVAudioPlayer");
    snd = getClass("MJSoundManager");
    game = getClass("GameView");
    if (!avp || !snd || !game) {
        LOGE("required classes absent: AV=%p sound=%p GameView=%p",
             avp, snd, game);
        return 0;
    }

    /* 2 - only in files that use MSG_*; the requirement is stated where it
       is actually used rather than in init(). */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    struct HookSpec specs[] = {
        { avp, "dealloc", ENC_AV_VOID, (IMP)my_dealloc, &g_origDealloc, 0, 0 },
        { snd, "restartMusicAfterActiveEvent:", ENC_CHAR, (IMP)my_restart, 0, 0, 0 },
        { avp, "play", ENC_AV_PLAY, (IMP)my_play, &g_origPlay, 0, 0 },
        { avp, "pause", ENC_AV_VOID, (IMP)my_pause, &g_origPause, 0, 0 },
        { avp, "stop", ENC_AV_VOID, (IMP)my_stop, &g_origStop, 0, 0 },
        { snd, "loadMP3IfSafe:withTimeOffset:", ENC_PATH_OFFSET, (IMP)my_load, &g_origLoad, 0, 0 },
        { snd, "safeToPlayMP3s", ENC_SAFE, (IMP)my_safeToPlayMP3s, 0, 0, 0 },
        { game, "didEnterBackground", ENC_NOARGS, (IMP)my_gameBackground, &g_origGameBackground, 0, 0 },
        { game, "didBecomeActive", ENC_NOARGS, (IMP)my_gameActive, &g_origGameActive, 0, 0 },
        { snd, "playMP3IfSafe:withTimeOffset:", ENC_PATH_OFFSET, (IMP)my_musicPlay, &g_origMusicPlay, 0, 0 },
    };
    const unsigned count = sizeof(specs) / sizeof(specs[0]);

    /* 3 - resolve every Method the file will hook before any setImp */
    for (i = 0; i < count; i++) {
        specs[i].method = getInstMethod(specs[i].cls, selReg(specs[i].selector));
        if (!specs[i].method) {
            LOGE("method not found: %s", specs[i].selector);
            return 0;
        }
        specs[i].rollback = getImp(specs[i].method);
        if (specs[i].rollback == specs[i].replacement) {
            if (i == 0) { LOGI("already installed"); return 1; }
            LOGE("partial previous installation: %s", specs[i].selector);
            return 0;
        }
    }

    /* 4 - require the tested fade guard; no static offsets are hardcoded. */
    if (!resolve_ivar("MJSoundManager", "mp3Player", &g_ivarMp3Player, 0, &howMp3)) return 0;
    if (!resolve_ivar("MJSoundManager", "fadingOut", &g_ivarFadingOut, 0, &howFade)) return 0;
    if (!resolve_ivar("MJSoundManager", "otherAudioWasPaying", &g_ivarOtherAudio, 0, &howOther))
        LOGE("otherAudioWasPaying unresolvable - ivar will not be cleared");

    /* 5 */
    g_selCurrentTime = selReg("currentTime");
    g_selIsPlaying = selReg("isPlaying");

    /* 6 - all encoding checks before any setImp; same ENC_* in step 7 */
    if (!getTypeEnc) {
        LOGE("audiofix requires method_getTypeEncoding; no hooks committed");
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (!checkEncoding(specs[i].method, specs[i].selector, specs[i].encoding)) return 0;
    }

    /* 7 - commit, undoing committed hooks in reverse order on failure.
       The two original terminal hooks still never chain and pass orig=NULL.
       Their displaced IMPs are retained only in this install-time rollback
       table; successful playback/restore behavior is unchanged. */
    for (i = 0; i < count; i++) {
        if (!hook(specs[i].cls, specs[i].selector, specs[i].encoding,
                  specs[i].replacement, specs[i].original)) {
            while (committed) {
                committed--;
                setImp(specs[committed].method, specs[committed].rollback);
            }
            LOGE("audiofix install failed; committed hooks rolled back");
            return 0;
        }
        committed++;
    }

    LOGI("installed (hooks=%u, mp3Player=0x%x via %s, fadingOut=0x%x via %s,"
         " otherAudio=0x%x via %s)",
         count, g_ivarMp3Player, howMp3, g_ivarFadingOut, howFade,
         g_ivarOtherAudio, howOther);
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
