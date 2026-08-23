/* welcomefix.c - stops the welcome message being mangled on display.
 *
 * -[GameView viewServerWelcomeMessage:customRules:allowEdit:] composes the
 * HTML page that BlockheadsWebView renders, and does two things to the message
 * on the way:
 *
 *   1. If the message contains no '<' anywhere, it replaces "\n" with "<br/>".
 *      If it contains any '<' - a <b> tag, or prose like "5 < 10" - the
 *      conversion is skipped and raw newlines fall through to step 2.
 *
 *   2. It builds an NSCharacterSet of printable ASCII (32..126), inverts it,
 *      splits the message on that set and rejoins with @"" - deleting every
 *      character outside 32..126.  Surviving newlines die here, and so does
 *      all non-ASCII text.
 *
 * The composed page is then handed to Java, and BlockheadsWebView.onCreate
 * seeds the EditText from it with content.substring(26, length-7).  So the
 * editor is populated with the *rendered* form of the message.  Pressing DONE
 * transmits whatever the editor holds, which is how "<br/>" literals end up
 * saved to the server and how newlines are lost.
 *
 * Nothing on the send path touches the string: the EditText contents go
 * through JNI (modified UTF-8, transparent), -[World setNewWelcomeMessage:],
 * -[BHClient sendNewWelcomeMessageToServer:], a binary property list
 * (format 100) and gzip.  Verified against the server: a message saved with
 * newlines is stored with newlines.
 *
 * This library neutralises both transformations so the page carries the
 * message verbatim.  The Smali half then converts "\n" to "<br/>" for the
 * WebView only, leaving the EditText seeded with the true stored text.
 *
 * Implementation note: the runtime reports a single shared Method for
 * NSString, __NSCFString and __NSCFConstantString
 * (Method=0x782bc868 for componentsSeparatedByCharactersInSet:), so patching
 * NSString patches the whole cluster - every string in the process.  Both
 * hooks are therefore gated on a thread-local depth counter set only while
 * inside viewServerWelcomeMessage:..., and then further gated on their
 * arguments.  The guard is checked first so the common case is a
 * thread-local load and a branch.
 *
 * Load point: BackgroundLibraryLoader$1.run(), after
 * LibraryManager.loadLibraries().
 */

#include <dlfcn.h>
#include <stdint.h>
#include <android/log.h>

/* Set to 1 to log every interception. Off by default: the hooks sit on two of
   the hottest methods in Foundation, and even guarded they run often. */
#define WMFIX_VERBOSE 0

/* Refuse to install if a hooked method's type encoding is not what the hook
   signature assumes. Wrong arity on viewServerWelcomeMessage:... corrupts the
   stack on a method that runs every time the screen opens, so the default is
   to fail closed. Set to 0 to install anyway and rely on the logged encoding. */
#define WMFIX_STRICT_ENCODING 1

/* Measured on the target build with method_getTypeEncoding.
   viewServerWelcomeMessage:customRules:allowEdit: returns void and takes two
   objects plus a BOOL passed as char - the decompiler's apparent return value
   is a tail call into objc_msgSend, not a result. */
#define ENC_VIEW_WELCOME  "v20@0:4@8@12c16"
#define ENC_REPLACE_OCCUR "@16@0:4@8@12"
#define ENC_COMPONENTS    "@12@0:4@8"

#define TAG "WMFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if WMFIX_VERBOSE
#define LOGV(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#else
#define LOGV(...) ((void)0)
#endif

typedef void *id;
typedef void *SEL;
typedef void *Method;
typedef void *IMP;
typedef void *Class;

/* --- ObjC runtime, resolved from libSystem.so at load time --- */
static SEL    (*selReg)(const char *);
static Class  (*getClass)(const char *);
static Method (*getInstMethod)(Class, SEL);
static IMP    (*setImp)(Method, IMP);
static IMP    (*getImp)(Method);
static id     (*msgSend)(id, SEL, ...);
static const char * (*getTypeEnc)(Method);   /* optional */

static IMP g_origViewWelcome    = 0;
static IMP g_origReplaceOccur   = 0;
static IMP g_origComponentsSep  = 0;

/* Cached selectors and comparison constants, built once at install time and
   retained. The constants in libApplication.so are __NSCFConstantString and
   are not pointer-identical to runtime-built strings (measured: 0x6d65c040 vs
   0x6e763750), so the comparison has to be by value. */
static SEL g_selIsEqual        = 0;
static SEL g_selCharIsMember   = 0;
static SEL g_selArrayWithObj   = 0;
static Class g_clsNSArray      = 0;
static id  g_strNewline        = 0;   /* @"\n"    */
static id  g_strBr             = 0;   /* @"<br/>" */

/* Depth counter, non-zero only while inside
   -[GameView viewServerWelcomeMessage:customRules:allowEdit:]. Thread-local
   because the shared Method means these hooks are live for every thread in
   the process, and only the thread composing the page must see altered
   behaviour. A counter rather than a flag so a nested call cannot clear it
   early. */
static __thread int g_composeDepth = 0;

/* ------------------------------------------------------------------ */

static int strEquals(id a, id b) {
    if (!a || !b) return 0;
    if (a == b) return 1;
    /* BOOL is a char on this runtime: only the low byte of r0 is defined, so
       the return must not be read as an int. */
    return ((char (*)(id, SEL, id))msgSend)(a, g_selIsEqual, b) ? 1 : 0;
}

/* Identifies the inverted printable-ASCII set the game builds inline. Three
   probes rather than one: the inverted set must contain '\n' (10) and must not
   contain 'A' (65) or ' ' (32). Any other character set in use during the
   compose window fails at least one. */
static int isPrintableAsciiInverted(id set) {
    if (!set) return 0;
    if (!((char (*)(id, SEL, unsigned short))msgSend)(set, g_selCharIsMember, (unsigned short)'\n'))
        return 0;
    if (((char (*)(id, SEL, unsigned short))msgSend)(set, g_selCharIsMember, (unsigned short)'A'))
        return 0;
    if (((char (*)(id, SEL, unsigned short))msgSend)(set, g_selCharIsMember, (unsigned short)' '))
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* hooks                                                              */
/* ------------------------------------------------------------------ */

/* Arity from the runtime encoding: v20@0:4@8@12c16 - void return, three
   arguments, the last a BOOL passed as char. allowEdit is not consulted: the
   message must be verbatim in the page for both the view and the edit case,
   because the Smali half renders it for display and seeds the editor from the
   same region. */
static void my_viewWelcome(id self, SEL _cmd, id message, id customRules, char allowEdit) {
    g_composeDepth++;
    LOGV("compose enter (allowEdit=%d)", (int)allowEdit);
    ((void (*)(id, SEL, id, id, char))g_origViewWelcome)
        (self, _cmd, message, customRules, allowEdit);
    g_composeDepth--;
    LOGV("compose leave");
}

/* Neutralises the "\n" -> "<br/>" conversion. Returning the receiver makes
   both sides of the game's rangeOfString:@"<" gate behave identically, so a
   message containing a tag is no longer treated differently from one without.
   The receiver is owned by the caller's scope and is used immediately, so
   returning it unretained matches the autoreleased result it replaces. */
static id my_replaceOccurrences(id self, SEL _cmd, id target, id replacement) {
    if (g_composeDepth > 0
        && strEquals(target, g_strNewline)
        && strEquals(replacement, g_strBr)) {
        LOGV("suppressed newline -> <br/> conversion");
        return self;
    }
    return ((id (*)(id, SEL, id, id))g_origReplaceOccur)(self, _cmd, target, replacement);
}

/* Neutralises the printable-ASCII filter. The game immediately sends
   componentsJoinedByString:@"" to this result, so returning a single-element
   array reconstructs the receiver unchanged. */
static id my_componentsSeparated(id self, SEL _cmd, id charset) {
    if (g_composeDepth > 0 && isPrintableAsciiInverted(charset)) {
        LOGV("suppressed printable-ASCII filter");
        return ((id (*)(Class, SEL, id))msgSend)(g_clsNSArray, g_selArrayWithObj, self);
    }
    return ((id (*)(id, SEL, id))g_origComponentsSep)(self, _cmd, charset);
}

/* ------------------------------------------------------------------ */

static int strEq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Logs the runtime's encoding for every hooked method and, when strict,
   refuses to proceed on a mismatch. If method_getTypeEncoding is unavailable
   the check is skipped and that fact is logged rather than silently passing. */
static int checkEncoding(Method m, const char *sel, const char *expected) {
    const char *enc;
    if (!getTypeEnc) {
        LOGI("%s: encoding unavailable (method_getTypeEncoding missing)", sel);
        return 1;
    }
    enc = getTypeEnc(m);
    LOGI("%s: encoding = %s (expected %s)", sel, enc ? enc : "(null)", expected);
    if (strEq(enc, expected)) return 1;

#if WMFIX_STRICT_ENCODING
    LOGE("%s: encoding mismatch - refusing to install", sel);
    return 0;
#else
    LOGE("%s: encoding mismatch - installing anyway (strict check disabled)", sel);
    return 1;
#endif
}

static int hook(Class cls, const char *sel, const char *expectedEnc, IMP repl, IMP *orig) {
    Method m = getInstMethod(cls, selReg(sel));
    if (!m) { LOGE("method not found: %s", sel); return 0; }
    if (!checkEncoding(m, sel, expectedEnc)) return 0;
    if (orig) *orig = getImp(m);
    setImp(m, repl);
    return 1;
}

static int install(void) {
    Class gameView = getClass("GameView");
    Class nsstring = getClass("NSString");
    Method m;

    if (!gameView || !nsstring) {
        LOGE("classes not present - load point too early");
        return 0;
    }

    m = getInstMethod(gameView, selReg("viewServerWelcomeMessage:customRules:allowEdit:"));
    if (!m) { LOGE("viewServerWelcomeMessage:... not found"); return 0; }
    if (getImp(m) == (IMP)my_viewWelcome) { LOGI("already installed"); return 1; }

    /* Comparison constants and selectors, resolved before any hook goes live
       so a hook can never run against a half-built state. */
    g_clsNSArray     = getClass("NSArray");
    g_selIsEqual     = selReg("isEqualToString:");
    g_selCharIsMember= selReg("characterIsMember:");
    g_selArrayWithObj= selReg("arrayWithObject:");

    if (!g_clsNSArray) { LOGE("NSArray not present"); return 0; }

    g_strNewline = ((id (*)(Class, SEL, const char *))msgSend)
                       (nsstring, selReg("stringWithUTF8String:"), "\n");
    g_strBr      = ((id (*)(Class, SEL, const char *))msgSend)
                       (nsstring, selReg("stringWithUTF8String:"), "<br/>");
    if (!g_strNewline || !g_strBr) { LOGE("could not build comparison strings"); return 0; }
    /* Retained for the process lifetime: these are consulted from inside the
       hooks, which run outside any pool this constructor controls. */
    ((id (*)(id, SEL))msgSend)(g_strNewline, selReg("retain"));
    ((id (*)(id, SEL))msgSend)(g_strBr, selReg("retain"));

    /* All three encodings are verified before any IMP is replaced: hooking is
       not transactional, and a mismatch discovered on the second hook would
       otherwise leave the process patched with the first. */
    {
        Method mR = getInstMethod(nsstring, selReg("stringByReplacingOccurrencesOfString:withString:"));
        Method mC = getInstMethod(nsstring, selReg("componentsSeparatedByCharactersInSet:"));
        if (!mR) { LOGE("stringByReplacingOccurrencesOfString:withString: not found"); return 0; }
        if (!mC) { LOGE("componentsSeparatedByCharactersInSet: not found"); return 0; }

        if (!checkEncoding(m,  "viewServerWelcomeMessage:customRules:allowEdit:", ENC_VIEW_WELCOME))
            return 0;
        if (!checkEncoding(mR, "stringByReplacingOccurrencesOfString:withString:", ENC_REPLACE_OCCUR))
            return 0;
        if (!checkEncoding(mC, "componentsSeparatedByCharactersInSet:", ENC_COMPONENTS))
            return 0;
    }

    if (!hook(gameView, "viewServerWelcomeMessage:customRules:allowEdit:",
              ENC_VIEW_WELCOME, (IMP)my_viewWelcome, &g_origViewWelcome)) return 0;
    if (!hook(nsstring, "stringByReplacingOccurrencesOfString:withString:",
              ENC_REPLACE_OCCUR, (IMP)my_replaceOccurrences, &g_origReplaceOccur)) return 0;
    if (!hook(nsstring, "componentsSeparatedByCharactersInSet:",
              ENC_COMPONENTS, (IMP)my_componentsSeparated, &g_origComponentsSep)) return 0;

    LOGI("installed");
    return 1;
}

__attribute__((constructor))
static void init(void) {
    static int once = 0;
    void *sys;

    if (__sync_val_compare_and_swap(&once, 0, 1) != 0) return;

    /* RTLD_NOLOAD turns a wrong load point into a clean error instead of a
       second ObjC runtime. */
    sys = dlopen("libSystem.so", RTLD_NOW | RTLD_NOLOAD);
    if (!sys) { LOGE("libSystem not loaded - load point too early"); return; }

    selReg        = dlsym(sys, "sel_registerName");
    getClass      = dlsym(sys, "objc_getClass");
    getInstMethod = dlsym(sys, "class_getInstanceMethod");
    setImp        = dlsym(sys, "method_setImplementation");
    getImp        = dlsym(sys, "method_getImplementation");
    msgSend       = dlsym(sys, "objc_msgSend");
    getTypeEnc    = dlsym(sys, "method_getTypeEncoding");   /* optional */

    if (!selReg || !getClass || !getInstMethod || !setImp || !getImp || !msgSend) {
        LOGE("runtime symbols missing");
        return;
    }

    install();
}
