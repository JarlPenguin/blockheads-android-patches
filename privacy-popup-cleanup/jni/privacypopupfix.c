#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <android/log.h>

/* Set to 1 to log every dismissal that reaches the GDPR delegate. */
#define PRIVACYPOPUPFIX_VERBOSE 0

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

/* --- ObjC runtime, resolved from libSystem.so at load time --- */
static SEL    (*selReg)(const char *);
static id     (*getClass)(const char *);
static Method (*getInstMethod)(id, SEL);
static IMP    (*setImp)(Method, IMP);
static IMP    (*getImp)(Method);
static void   *g_msgSend;

/* Optional: present in libobjc2, absent in some older Apportable drops.
   Resolved best-effort; install() degrades gracefully if either is missing. */
static Ivar      (*getIvar)(id, const char *);
static ptrdiff_t (*ivarOff)(Ivar);

#define MSG_id(o,s)  (((id          (*)(id,SEL))g_msgSend)((o),(s)))
#define MSG_str(o,s) (((const char *(*)(id,SEL))g_msgSend)((o),(s)))

static uint32_t g_ivarGdprPrompt = 0;
static IMP      g_origAlertClick = 0;
static uint32_t g_suppressed     = 0;
static const char *g_ivarSource  = "none";

/* Offset of -[GameView gdprPrompt] as read from the IDA database. Used only
   if the runtime can't be asked; libobjc2 may rewrite non-fragile ivar offsets
   at load time, so class_getInstanceVariable always wins when available.
   Confirmed to match the runtime value on the target build. */
#define GDPR_PROMPT_OFFSET_FALLBACK 0x204

/* Report the first few suppressions at INFO, then fall silent - enough to
   confirm the fix is live in a normal log capture without flooding it. */
#define SUPPRESS_LOG_LIMIT 8

static const char *alertTitle(id alertView) {
    id t;
    if (!alertView) return "(nil)";
    t = MSG_id(alertView, selReg("title"));
    if (!t) return "(no title)";
    return MSG_str(t, selReg("UTF8String"));
}

/* ---- the fix ----
   -[GameView alertView:clickedButtonAtIndex:] guards its gdprStatus write
   with `if (alertView == self->gdprPrompt)`, but the closing brace lands
   before the alert construction, so "Privacy Setting Changed" is built and
   shown on every invocation. GameView is delegate for ~15 other alerts
   (disconnect, join-world, tutorial, death confirmation, ...), all of which
   reach this method on dismissal because UIAlertView calls both
   clickedButtonAtIndex: and didDismissWithButtonIndex:. Restore the guard by
   dropping dismissals that aren't the GDPR prompt; those are serviced by
   -[GameView alertView:didDismissWithButtonIndex:] instead. */

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

static int hook(id cls, const char *sel, IMP repl, IMP *orig) {
    Method m = getInstMethod(cls, selReg(sel));
    if (!m) { LOGE("method not found: %s", sel); return 0; }
    if (orig) *orig = getImp(m);
    setImp(m, repl);
    return 1;
}

static int install(void) {
    id gv = getClass("GameView");
    Method m;

    if (!gv) { LOGE("GameView missing at install"); return 0; }

    /* Preferred: ask the runtime. This reads the post-fixup value, so it is
       correct even if libobjc2 rewrote the offset global at load time, and it
       does not depend on the ivar being exported in .dynsym. */
    if (getIvar && ivarOff) {
        Ivar iv = getIvar(gv, "gdprPrompt");
        if (iv) {
            ptrdiff_t o = ivarOff(iv);
            if (o > 0 && o < 0x10000) {
                g_ivarGdprPrompt = (uint32_t)o;
                g_ivarSource = "runtime";
            } else {
                LOGE("ivar_getOffset returned implausible %ld", (long)o);
            }
        } else {
            LOGE("class_getInstanceVariable(GameView, gdprPrompt) failed");
        }
    }

    /* Fallback: the value read out of IDA. (An earlier tier that resolved
       OBJC_IVAR_$_GameView.gdprPrompt via dlsym was dropped - the symbol is in
       .symtab but not .dynsym on the shipped build, so it never resolved.) */
    if (!g_ivarGdprPrompt) {
        g_ivarGdprPrompt = GDPR_PROMPT_OFFSET_FALLBACK;
        g_ivarSource = "fallback";
        LOGE("falling back to static-analysis offset 0x%x", g_ivarGdprPrompt);
    }

    if (g_ivarGdprPrompt != GDPR_PROMPT_OFFSET_FALLBACK)
        LOGE("gdprPrompt offset is 0x%x via %s, but static analysis said 0x%x"
             " - the binary differs from the one analysed",
             g_ivarGdprPrompt, g_ivarSource, GDPR_PROMPT_OFFSET_FALLBACK);

    m = getInstMethod(gv, selReg("alertView:clickedButtonAtIndex:"));
    if (m && getImp(m) == (IMP)my_alertClick) { LOGI("already installed"); return 1; }

    if (!hook(gv, "alertView:clickedButtonAtIndex:", (IMP)my_alertClick, &g_origAlertClick))
        return 0;

    LOGI("installed (gdprPrompt=0x%x via %s)", g_ivarGdprPrompt, g_ivarSource);
    return 1;
}

__attribute__((constructor))
static void init(void) {
    static int once = 0;
    void *sys;

    if (__sync_val_compare_and_swap(&once, 0, 1) != 0) return;

    /* Loaded from BackgroundLibraryLoader$1.run(), after
       LibraryManager.loadLibraries(). RTLD_NOLOAD turns a wrong load point
       into a clean error instead of a second ObjC runtime. */
    sys = dlopen("libSystem.so", RTLD_NOW | RTLD_NOLOAD);
    if (!sys) { LOGE("libSystem not loaded - load point too early"); return; }

    selReg        = dlsym(sys, "sel_registerName");
    getClass      = dlsym(sys, "objc_getClass");
    getInstMethod = dlsym(sys, "class_getInstanceMethod");
    setImp        = dlsym(sys, "method_setImplementation");
    getImp        = dlsym(sys, "method_getImplementation");
    g_msgSend     = dlsym(sys, "objc_msgSend");
    getIvar       = dlsym(sys, "class_getInstanceVariable");
    ivarOff       = dlsym(sys, "ivar_getOffset");

    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp || !g_msgSend) {
        LOGE("runtime symbols missing"); return;
    }
    if (!getIvar || !ivarOff)
        LOGE("ivar introspection unavailable - will use static-analysis offset");

    if (!getClass("GameView")) {
        LOGE("GameView not present - load point too early"); return;
    }

    install();
}
