/*
 * libsfxexitfix.so
 *
 * THE BUG
 * -------
 * -[World update:accurateDT:pinchScale:dragInProgress:] can create an
 * NSURLConnection with World as delegate, then retain that connection in
 * World.sendPricesConnection. -[NSURLConnectionInternal start] retains its
 * delegate. In the measured exit, World went from retain count 3 to 1 and
 * World.dealloc did not run: the connection still owned it. World.dealloc
 * would cancel/release sendPricesConnection, but that cleanup cannot run
 * while the connection keeps World alive. The request has a 60-second
 * timeout, so this can be a delayed teardown rather than a permanent leak.
 *
 * Weather.dealloc's four setPaused:1 calls therefore did not run on exit.
 * MJSoundManager still held playing rain, wind and MJMultiSound children
 * after the menu appeared. Cricket multisounds are also registered with
 * that manager; Weather.dealloc has no stop for them even if it later runs.
 * Stopping cached Weather MJSounds also leaves _paused=0. A newly joined
 * rainy/snowy world calls setPaused:0 to activate them, which is a no-op
 * at that state; this caused ambience to stay silent after rejoining.
 *
 * THE FIX
 * -------
 * After the game's world exit finishes, stop the sources held in all
 * three manager dictionaries. Use the game's MJSound.stop (which calls
 * alSourceStop and rewinds) and MJMultiSound.stopAll, plus stop the
 * separate loopingSound before MJMultiSound.stopLoopingSound releases it.
 * Keep cached sound objects/buffers and the manager in place so the next
 * world can play them normally. After stopping, setPaused:1 on the four
 * Weather-owned sounds. A new world's setPaused:0 then calls MJSound.play
 * through the existing game code. Do not call MJSoundManager.resignActive:
 * its saved sounds would be resumed by becomeActive on a later app event.
 *
 * This hook handles calls through the normal leave-world action and the
 * observed timeout/background paths. It does not repair audio focus or
 * assume Weather.dealloc runs promptly. On-device checks confirmed that
 * rain, snow, jetpack, crickets and birds stop at the menu; rain,
 * snow and jetpack play after rejoining; and in-world effects resume
 * after backgrounding.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "sfxexitfix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libsfxexitfix.so sfxexitfix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>


#ifndef SFXEXITFIX_VERBOSE
#define SFXEXITFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef SFXEXITFIX_STRICT_ENCODING
#define SFXEXITFIX_STRICT_ENCODING 1
#endif

#define TAG "SFXEXITFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if SFXEXITFIX_VERBOSE
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

#if SFXEXITFIX_STRICT_ENCODING
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

#define ENC_EXIT "v8@0:4"

#define MSG_v_c(o,s,a) (((void (*)(id,SEL,char))g_msgSend)((o),(s),(a)))

/* Every collection belongs to the singleton MJSoundManager and outlives
   World. The game keeps sounds cached, so stop playback without releasing
   their buffers, dictionary entries, or manager-owned multisounds. */
static uint32_t g_loadedOff, g_multiLoadedOff, g_externalOff;
static uint32_t g_multiLoopOff, g_playingOff;
static uint32_t g_worldOff, g_weatherOff, g_ambientOff[4];
static Class g_managerClass, g_soundClass, g_multiClass;
static SEL g_instanceSel, g_enumeratorSel, g_nextSel, g_kindSel;
static SEL g_stopSel, g_stopAllSel, g_stopLoopSel;
static SEL g_retainSel, g_releaseSel, g_setPausedSel;
static IMP g_origExit;

static int isSound(id object) {
    return object && (MSG_c_id(object, g_kindSel, (id)g_soundClass) & 1);
}

static int isMulti(id object) {
    return object && (MSG_c_id(object, g_kindSel, (id)g_multiClass) & 1);
}

#if SFXEXITFIX_VERBOSE
static int playing(id sound) {
    return *(unsigned char *)((char *)sound + g_playingOff) != 0;
}
#endif

static void stopSounds(id collection, const char *name) {
#if SFXEXITFIX_VERBOSE
    unsigned objects = 0, active = 0, remaining = 0;
#endif
    id en = collection ? MSG_id(collection, g_enumeratorSel) : 0;
    id sound;
    while (en && (sound = MSG_id(en, g_nextSel)) != 0) {
        if (!isSound(sound)) { LOGE("%s: unexpected object=%p", name, sound); continue; }
#if SFXEXITFIX_VERBOSE
        ++objects;
        active += playing(sound);
#endif
        MSG_v(sound, g_stopSel);
#if SFXEXITFIX_VERBOSE
        remaining += playing(sound);
#endif
    }
#if SFXEXITFIX_VERBOSE
    LOGV("%s: entries=%u active=%u stillPlaying=%u", name, objects, active, remaining);
#endif
}

static void stopMultis(id collection, const char *name) {
#if SFXEXITFIX_VERBOSE
    unsigned objects = 0, loops = 0;
#endif
    id en = collection ? MSG_id(collection, g_enumeratorSel) : 0;
    id multi;
    while (en && (multi = MSG_id(en, g_nextSel)) != 0) {
        if (!isMulti(multi)) { LOGE("%s: unexpected object=%p", name, multi); continue; }
#if SFXEXITFIX_VERBOSE
        ++objects;
#endif

        /* MJMultiSound.stopAll covers _sounds. loopingSound may be a
           separate MJSound, so stop it before releasing that reference. */
        id loop = *(id *)((char *)multi + g_multiLoopOff);
        if (loop) {
#if SFXEXITFIX_VERBOSE
            ++loops;
#endif
            if (isSound(loop)) MSG_v(loop, g_stopSel);
            else LOGE("%s: unexpected loopingSound=%p", name, loop);
            MSG_v(multi, g_stopLoopSel);
        }
        MSG_v(multi, g_stopAllSel);
    }
#if SFXEXITFIX_VERBOSE
    LOGV("%s: entries=%u loopingSound refs=%u stopped", name, objects, loops);
#endif
}

static void stopWorldEffects(void) {
    id manager = MSG_id((id)g_managerClass, g_instanceSel);
    if (!manager) { LOGE("MJSoundManager.instance missing at world exit"); return; }

    stopSounds(*(id *)((char *)manager + g_loadedOff), "loadedSounds");
    stopMultis(*(id *)((char *)manager + g_multiLoadedOff), "loadedMultiSounds");
    stopMultis(*(id *)((char *)manager + g_externalOff), "externalMultiSounds");
}

static void captureAmbient(id game, id ambient[4]) {
    id world = *(id *)((char *)game + g_worldOff);
    id weather = world ? *(id *)((char *)world + g_weatherOff) : 0;
    if (!weather) return;
    for (unsigned i = 0; i < 4; ++i) {
        id sound = *(id *)((char *)weather + g_ambientOff[i]);
        if (isSound(sound)) ambient[i] = MSG_id(sound, g_retainSel);
    }
}

static void my_exit(id self, SEL cmd) {
    id ambient[4] = {0};
#if SFXEXITFIX_VERBOSE
    unsigned armed = 0;
#endif
    /* The original tears down World and builds the main menu. Even if
       World is retained and Weather.dealloc is delayed, no world SFX
       should continue after the transition has completed. */
    captureAmbient(self, ambient);
    ((void (*)(id, SEL))g_origExit)(self, cmd);
    stopWorldEffects();
    for (unsigned i = 0; i < 4; ++i) {
        if (!ambient[i]) continue;
        /* stop sets playing=0 but does not set _paused. This arms the
           cached source for Weather's next setPaused:0 -> play path. */
        MSG_v_c(ambient[i], g_setPausedSel, 1);
        MSG_v(ambient[i], g_releaseSel);
#if SFXEXITFIX_VERBOSE
        ++armed;
#endif
    }
#if SFXEXITFIX_VERBOSE
    LOGV("world exit SFX cleanup complete; ambient sources armed=%u", armed);
#endif
}

static int install(void) {
    static const struct {
        const char *cls, *ivar;
        uint32_t   *out;
    } ivars[] = {
        { "MJSoundManager", "loadedSounds",        &g_loadedOff      },
        { "MJSoundManager", "loadedMultiSounds",   &g_multiLoadedOff },
        { "MJSoundManager", "externalMultiSounds", &g_externalOff    },
        { "MJMultiSound",   "loopingSound",        &g_multiLoopOff   },
        { "MJSound",        "playing",             &g_playingOff     },
        { "GameView",       "world",               &g_worldOff       },
        { "World",          "weather",             &g_weatherOff     },
        { "Weather",        "lightRainSound",      &g_ambientOff[0]  },
        { "Weather",        "heavyRainSound",      &g_ambientOff[1]  },
        { "Weather",        "windSound",           &g_ambientOff[2]  },
        { "Weather",        "undergroundSound",    &g_ambientOff[3]  },
    };
    Class game, world, weather;
    Method m;
    const char *how, *tier = "runtime";
    unsigned i;

    /* 1 */
    game = getClass("GameView");
    world = getClass("World");
    weather = getClass("Weather");
    g_managerClass = getClass("MJSoundManager");
    g_soundClass = getClass("MJSound");
    g_multiClass = getClass("MJMultiSound");
    if (!game || !world || !weather || !g_managerClass || !g_soundClass || !g_multiClass) {
        LOGE("required class missing - load point too early"); return 0;
    }

    /* 2 */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    /* 3 */
    m = getInstMethod(game, selReg("actuallyDoExitWorldRightNowReallyNow"));
    if (!m) { LOGE("GameView exit method missing"); return 0; }
    if (getImp(m) == (IMP)my_exit) { LOGI("already installed"); return 1; }

    /* 4 - no static fallbacks, so each resolves via "runtime" or "dlsym".
       tier reports "runtime" only if every ivar did; resolve_ivar has
       already logged which one did not. */
    for (i = 0; i < sizeof(ivars) / sizeof(ivars[0]); ++i) {
        if (!resolve_ivar(ivars[i].cls, ivars[i].ivar, ivars[i].out, 0, &how))
            return 0;
        if (!strEq(how, "runtime")) tier = how;
    }

    /* 5 */
    g_instanceSel = selReg("instance");
    g_enumeratorSel = selReg("objectEnumerator");
    g_nextSel = selReg("nextObject");
    g_kindSel = selReg("isKindOfClass:");
    g_stopSel = selReg("stop");
    g_stopAllSel = selReg("stopAll");
    g_stopLoopSel = selReg("stopLoopingSound");
    g_retainSel = selReg("retain");
    g_releaseSel = selReg("release");
    g_setPausedSel = selReg("setPaused:");

    /* 6 skipped: only one hook, so hook() performs the encoding check. */

    /* 7 */
    if (!hook(game, "actuallyDoExitWorldRightNowReallyNow", ENC_EXIT,
              (IMP)my_exit, &g_origExit)) return 0;

    LOGI("installed (%u ivars via %s)",
         (unsigned)(sizeof(ivars) / sizeof(ivars[0])), tier);
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
