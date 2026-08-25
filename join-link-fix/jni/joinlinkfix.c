/*
 * libjoinlinkfix.so
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
 * Known limitation: delivery lands on the second frame, so one frame of main
 * menu is visible on a cold boot. See the delivery comment below for why
 * moving it to the first frame breaks populateGameSaves() ordering.
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
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>

#ifndef JOINLINKFIX_VERBOSE
#define JOINLINKFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef JOINLINKFIX_STRICT_ENCODING
#define JOINLINKFIX_STRICT_ENCODING 1
#endif

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

#if JOINLINKFIX_STRICT_ENCODING
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

/* TODO: measure -[GameView delayedOpenURLIfNeeded] with
   method_getTypeEncoding on-device and fill this in from the logged value.
   Zero-argument void method, so a mismatch is close to impossible, but the
   check costs nothing here. */
#define ENC_DELAYED_OPEN_URL NULL

static uint32_t g_ivarMainMenuUI = 0;

static SEL g_selCopy          = 0;
static SEL g_selRelease       = 0;
static SEL g_selHandleOpenURL = 0;

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
   and no chance of the URL being applied twice.

   The mutex guards a genuine cross-thread handoff - capture runs on the
   delivering thread, delivery on the ObjC main thread - not a reentrancy
   window, so an atomic or a thread-local flag would not substitute. */
static void *my_VerdeHandleURI(void *url) {
    id copy = url ? MSG_id((id)url, g_selCopy) : 0;

    pthread_mutex_lock(&g_lock);
    if (g_pendingURL) MSG_v(g_pendingURL, g_selRelease);
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
        MSG_c_id(self, g_selHandleOpenURL, url);
        MSG_v(url, g_selRelease);
        LOGI("delivered url to %p", self);
    }

    ((void (*)(id, SEL))g_origDelayedOpen)(self, _cmd);
}

static int install(void) {
    Class        cls;
    Method       m;
    const char  *how;
    void       **pVerdeHandleURI;

    /* 1 - classes */
    cls = getClass("GameView");
    if (!cls) { LOGE("GameView not present - load point too early"); return 0; }

    /* VerdeHandleURI is patched through dlsym below, so libApplication being
       absent is fatal here even though resolve_ivar can manage without it. */
    if (!g_app) { LOGE("libApplication not loaded"); return 0; }

    /* Files that use MSG_* add this here rather than in init(), so the
       requirement is stated where it is actually used. */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    /* 2 - already installed */
    m = getInstMethod(cls, selReg("delayedOpenURLIfNeeded"));
    if (!m) { LOGE("delayedOpenURLIfNeeded not found"); return 0; }
    if (getImp(m) == (IMP)my_delayedOpenURLIfNeeded) { LOGI("already installed"); return 1; }

    /* 3 - ivar offsets */
    if (!resolve_ivar("GameView", "mainMenuUI", &g_ivarMainMenuUI, 0, &how)) return 0;

    /* 4 - selectors, resolved before any hook goes live */
    g_selCopy          = selReg("copy");
    g_selRelease       = selReg("release");
    g_selHandleOpenURL = selReg("handleOpenURL:");

    /* 5 - skipped: only one method hook is committed, so hook() does the
       single encoding check itself. An explicit pre-check here would only
       log the same encoding twice at startup. */

    /* 6 - hooks.
       Order matters: only start swallowing URLs once we can actually deliver
       them, so a failed swizzle leaves vanilla behaviour intact. */
    if (!hook(cls, "delayedOpenURLIfNeeded", ENC_DELAYED_OPEN_URL,
              (IMP)my_delayedOpenURLIfNeeded, &g_origDelayedOpen)) {
        return 0;
    }

    /* 7 - undo the committed hook if the pointer patch cannot be made */
    pVerdeHandleURI = (void **)dlsym(g_app, "VerdeHandleURI");
    if (!pVerdeHandleURI) {
        LOGE("VerdeHandleURI not found: %s", dlerror());
        setImp(getInstMethod(cls, selReg("delayedOpenURLIfNeeded")), g_origDelayedOpen);
        return 0;
    }
    g_origVerdeHandleURI = (void *(*)(void *))*pVerdeHandleURI;
    *pVerdeHandleURI = (void *)my_VerdeHandleURI;

    LOGI("installed (mainMenuUI=0x%x via %s, origVerdeHandleURI=%p)",
         g_ivarMainMenuUI, how, (void *)g_origVerdeHandleURI);
    return 1;
}


/* Region [7] sits AFTER install(), so no forward declaration is needed. */

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
