/*
 * libprivacypopupfix.so
 *
 * THE BUG
 * -------
 * -[GameView alertView:clickedButtonAtIndex:] guards its gdprStatus write
 * with `if (alertView == self->gdprPrompt)`, but the guard's closing brace
 * lands before the alert construction, so [[UIAlertView alloc]
 * initWithTitle:@"Privacy Setting Changed" ...] and -show run unconditionally
 * on every invocation.
 *
 * GameView is delegate for roughly fifteen other alerts (disconnect,
 * join-world, searching, tutorial, death confirmation, Game Center, server
 * rejection, ...), all of which reach this method on dismissal because
 * UIAlertView calls both clickedButtonAtIndex: and didDismissWithButtonIndex:.
 * All non-GDPR alerts fall through the mis-scoped guard in clickedButtonAtIndex:
 * and spuriously trigger the privacy popup.
 *
 * THE FIX
 * -------
 * Swizzles -[GameView alertView:clickedButtonAtIndex:] to compare the incoming
 * alertView against self->gdprPrompt (resolved via runtime ivar introspection
 * or static fallback offset 0x204). Dismissals that do not match the GDPR
 * prompt ivar are dropped; those are serviced by
 * -[GameView alertView:didDismissWithButtonIndex:] instead.
 *
 * Matching dismissals are forwarded to the original implementation unchanged.
 * After the real dismissal, self->gdprPrompt is nilled (*slot = 0) because
 * showGDPRAlert stores an autoreleased alert and never clears it, which would
 * otherwise leave a dangling pointer that could compare equal to a later
 * UIAlertView allocated at the same address.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "privacypopupfix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libprivacypopupfix.so privacypopupfix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>

#ifndef PRIVACYPOPUPFIX_VERBOSE
#define PRIVACYPOPUPFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef PRIVACYPOPUPFIX_STRICT_ENCODING
#define PRIVACYPOPUPFIX_STRICT_ENCODING 1
#endif

#define TAG "PRIVACYPOPUPFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if PRIVACYPOPUPFIX_VERBOSE
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

#if PRIVACYPOPUPFIX_STRICT_ENCODING
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

/* TODO: measure -[GameView alertView:clickedButtonAtIndex:] with
   method_getTypeEncoding on-device */
#define ENC_ALERT_CLICK NULL

/* Offset of -[GameView gdprPrompt] as read from the IDA database. Used only
   if the runtime can't be asked; libobjc2 may rewrite non-fragile ivar offsets
   at load time, so class_getInstanceVariable always wins when available.
   Confirmed to match the runtime value on the target build. */
#define GDPR_PROMPT_OFFSET_FALLBACK 0x204

/* Report the first few suppressions at INFO, then fall silent - enough to
   confirm the fix is live in a normal log capture without flooding it. */
#define SUPPRESS_LOG_LIMIT 8

static uint32_t g_ivarGdprPrompt = 0;
static IMP      g_origAlertClick = 0;
static uint32_t g_suppressed     = 0;

static const char *alertTitle(id alertView) {
    id t;
    if (!alertView) return "(nil)";
    t = MSG_id(alertView, selReg("title"));
    if (!t) return "(no title)";
    return MSG_str(t, selReg("UTF8String"));
}

/* ---- the fix ----
   Interception point for -[GameView alertView:clickedButtonAtIndex:].
   Restores the guard scoping broken by the misplaced brace (see THE BUG in banner). */
static void my_alertClick(id self, SEL _cmd, id alertView, int buttonIndex) {
    id *slot = (id *)((char *)self + g_ivarGdprPrompt);

    if (alertView != *slot) {
        if (g_suppressed < SUPPRESS_LOG_LIMIT)
            LOGI("suppressed fallthrough #%u (alert=%p title=%s idx=%d owned=%p)",
                 g_suppressed + 1, alertView, alertTitle(alertView),
                 buttonIndex, *slot);
        else
            LOGV("suppressed fallthrough (alert=%p idx=%d)", alertView, buttonIndex);
        g_suppressed++;
        return;
    }

    LOGI("gdpr dismissal idx=%d -> gdprStatus=%d", buttonIndex, buttonIndex == 1 ? 2 : 3);
    ((void (*)(id, SEL, id, int))g_origAlertClick)(self, _cmd, alertView, buttonIndex);

    /* showGDPRAlert stores an autoreleased alert here and never clears it, so
       the ivar dangles after dealloc and a later UIAlertView allocated at the
       same address would compare equal. Every branch in
       alertView:didDismissWithButtonIndex: nils its ivar; match that. */
    *slot = 0;
}

static int install(void) {
    Class      gv;
    Method     m;
    const char *how;

    /* 1 */
    gv = getClass("GameView");
    if (!gv) { LOGE("GameView not present - load point too early"); return 0; }

    /* 2 - alertTitle() uses MSG_id and MSG_str */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    /* 3 */
    m = getInstMethod(gv, selReg("alertView:clickedButtonAtIndex:"));
    if (!m) { LOGE("alertView:clickedButtonAtIndex: not found"); return 0; }
    if (getImp(m) == (IMP)my_alertClick) { LOGI("already installed"); return 1; }

    /* 4 - tier 2 (dlsym) is expected to miss for gdprPrompt because the
       symbol is in .symtab but not .dynsym on the shipped build; resolve_ivar
       will log 'not exported' at VERBOSE and fall through to fallback if tier 1
       is unavailable. */
    if (!resolve_ivar("GameView", "gdprPrompt",
                      &g_ivarGdprPrompt, GDPR_PROMPT_OFFSET_FALLBACK, &how))
        return 0;

    /* 5 - skipped: no runtime selectors or constants to cache */

    /* 6 - skipped: single hook, verification handled inside hook() */

    /* 7 - single hook, so no rollback needed on failure */
    if (!hook(gv, "alertView:clickedButtonAtIndex:", ENC_ALERT_CLICK,
              (IMP)my_alertClick, &g_origAlertClick))
        return 0;

    LOGI("installed (gdprPrompt=0x%x via %s)", g_ivarGdprPrompt, how);
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
