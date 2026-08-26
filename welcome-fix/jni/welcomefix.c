/*
 * libwelcomefix.so
 *
 * THE BUG
 * -------
 * -[GameView viewServerWelcomeMessage:customRules:allowEdit:] composes the
 * HTML page that BlockheadsWebView renders, and does two things to the
 * message on the way:
 *
 *   1. If the message contains no '<' anywhere, it replaces "\n" with
 *      "<br/>". If it contains any '<' - a <b> tag, or prose like
 *      "5 < 10" - the conversion is skipped and raw newlines fall through
 *      to step 2.
 *
 *   2. It builds an NSCharacterSet of printable ASCII (32..126), inverts
 *      it, splits the message on that set and rejoins with @"" - deleting
 *      every character outside 32..126. Surviving newlines die here, and
 *      so does all non-ASCII text.
 *
 * The composed page is then handed to Java, and BlockheadsWebView.onCreate
 * seeds the EditText from it with content.substring(26, length-7). So the
 * editor is populated with the *rendered* form of the message. Pressing
 * DONE transmits whatever the editor holds, which is how "<br/>" literals
 * end up saved to the server and how newlines are lost.
 *
 * Nothing on the send path touches the string: the EditText contents go
 * through JNI (modified UTF-8, transparent), -[World setNewWelcomeMessage:],
 * -[BHClient sendNewWelcomeMessageToServer:], a binary property list
 * (format 100) and gzip. Verified against the server: a message saved with
 * newlines is stored with newlines. The defect is entirely in display
 * composition, not transmission.
 *
 * THE FIX
 * -------
 * This library neutralises both transformations so the composed page
 * carries the message verbatim. The Smali half then converts "\n" to
 * "<br/>" for the WebView only, leaving the EditText seeded with the true
 * stored text.
 *
 * The runtime reports a single shared Method for NSString, __NSCFString
 * and __NSCFConstantString (Method=0x782bc868 for
 * componentsSeparatedByCharactersInSet:), so patching NSString patches the
 * whole cluster - every string in the process. Both hooks are therefore
 * gated on a thread-local depth counter set only while inside
 * viewServerWelcomeMessage:..., and then further gated on their arguments.
 * The guard is checked first so the common case is a thread-local load and
 * a branch.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "welcomefix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libwelcomefix.so welcomefix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>

/* Set to 1 to log every interception. Off by default: the hooks sit on two
   of the hottest methods in Foundation, and even guarded they run often. */
#ifndef WELCOMEFIX_VERBOSE
#define WELCOMEFIX_VERBOSE 0
#endif

/* Wrong arity on viewServerWelcomeMessage:customRules:allowEdit: would
   corrupt the stack on a method that runs every time the welcome-message
   screen opens - see ENC_VIEW_WELCOME below for the measured encoding. */
/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef WELCOMEFIX_STRICT_ENCODING
#define WELCOMEFIX_STRICT_ENCODING 1
#endif

#define TAG "WELCOMEFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if WELCOMEFIX_VERBOSE
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

/* File-specific message-send shapes, needed here but not covered by the
   canonical MSG_* family: characterIsMember: takes an unsigned short
   rather than an id, stringWithUTF8String: takes a const char *, and
   arrayWithObject: below is sent to a Class rather than an instance. */
#define MSG_c_ushort(o,s,a) \
    (((char (*)(id,SEL,unsigned short))g_msgSend)((o),(s),(a)))
#define MSG_id_cls_cstr(c,s,a) \
    (((id (*)(Class,SEL,const char *))g_msgSend)((c),(s),(a)))
#define MSG_id_cls_id(c,s,a) \
    (((id (*)(Class,SEL,id))g_msgSend)((c),(s),(a)))

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

#if WELCOMEFIX_STRICT_ENCODING
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

/* Measured on the target build with method_getTypeEncoding.
   viewServerWelcomeMessage:customRules:allowEdit: returns void and takes two
   objects plus a BOOL passed as char - the decompiler's apparent return value
   is a tail call into objc_msgSend, not a result. */
#define ENC_VIEW_WELCOME  "v20@0:4@8@12c16"
#define ENC_REPLACE_OCCUR "@16@0:4@8@12"
#define ENC_COMPONENTS    "@12@0:4@8"

/* Cached selectors and comparison constants, built once at install time and
   retained. The constants in libApplication.so are __NSCFConstantString and
   are not pointer-identical to runtime-built strings (measured: 0x6d65c040 vs
   0x6e763750), so the comparison has to be by value. */
static SEL   g_selIsEqual        = 0;
static SEL   g_selCharIsMember   = 0;
static SEL   g_selArrayWithObj   = 0;
static Class g_clsNSArray        = 0;
static id    g_strNewline        = 0;   /* @"\n"    */
static id    g_strBr             = 0;   /* @"<br/>" */

static IMP g_origViewWelcome   = 0;
static IMP g_origReplaceOccur  = 0;
static IMP g_origComponentsSep = 0;

/* Depth counter, non-zero only while inside
   -[GameView viewServerWelcomeMessage:customRules:allowEdit:]. Thread-local
   because the shared Method means these hooks are live for every thread in
   the process, and only the thread composing the page must see altered
   behaviour. A counter rather than a flag so a nested call cannot clear it
   early. */
static __thread int g_composeDepth = 0;

static int strEquals(id a, id b) {
    if (!a || !b) return 0;
    if (a == b) return 1;
    /* BOOL is a char on this runtime: only the low byte of r0 is defined, so
       the return must not be read as an int. */
    return MSG_c_id(a, g_selIsEqual, b) ? 1 : 0;
}

/* Identifies the inverted printable-ASCII set the game builds inline. Three
   probes rather than one: the inverted set must contain '\n' (10) and must not
   contain 'A' (65) or ' ' (32). Any other character set in use during the
   compose window fails at least one. */
static int isPrintableAsciiInverted(id set) {
    if (!set) return 0;
    if (!MSG_c_ushort(set, g_selCharIsMember, (unsigned short)'\n'))
        return 0;
    if (MSG_c_ushort(set, g_selCharIsMember, (unsigned short)'A'))
        return 0;
    if (MSG_c_ushort(set, g_selCharIsMember, (unsigned short)' '))
        return 0;
    return 1;
}

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
        return MSG_id_cls_id(g_clsNSArray, g_selArrayWithObj, self);
    }
    return ((id (*)(id, SEL, id))g_origComponentsSep)(self, _cmd, charset);
}

static int install(void) {
    Class  gameView;
    Class  nsstring;
    Method m, mR, mC;

    /* 1 */
    gameView = getClass("GameView");
    nsstring = getClass("NSString");
    if (!gameView || !nsstring) {
        LOGE("classes not present - load point too early");
        return 0;
    }

    /* 2 - this file calls objc_msgSend directly via the MSG_* macros to
       build and retain the comparison strings and to probe the character
       set, so g_msgSend must be resolved before any of that runs. */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    /* 3 - resolve every Method this file will hook, not just the primary:
       step 6 needs them all in hand before any setImp. */
    m = getInstMethod(gameView, selReg("viewServerWelcomeMessage:customRules:allowEdit:"));
    if (!m) { LOGE("viewServerWelcomeMessage:... not found"); return 0; }
    if (getImp(m) == (IMP)my_viewWelcome) { LOGI("already installed"); return 1; }

    mR = getInstMethod(nsstring, selReg("stringByReplacingOccurrencesOfString:withString:"));
    mC = getInstMethod(nsstring, selReg("componentsSeparatedByCharactersInSet:"));
    if (!mR) { LOGE("stringByReplacingOccurrencesOfString:withString: not found"); return 0; }
    if (!mC) { LOGE("componentsSeparatedByCharactersInSet: not found"); return 0; }

    /* 4 - no ivar offsets: both transformations are neutralised by
       replacing NSString methods outright, and the compose-window guard
       is a thread-local, not a field read off any object. */

    /* 5 - selectors and comparison constants, resolved and built before
       any hook goes live so a hook can never run against a half-built
       state. */
    g_clsNSArray      = getClass("NSArray");
    g_selIsEqual      = selReg("isEqualToString:");
    g_selCharIsMember = selReg("characterIsMember:");
    g_selArrayWithObj = selReg("arrayWithObject:");
    if (!g_clsNSArray) { LOGE("NSArray not present"); return 0; }

    g_strNewline = MSG_id_cls_cstr(nsstring, selReg("stringWithUTF8String:"), "\n");
    g_strBr      = MSG_id_cls_cstr(nsstring, selReg("stringWithUTF8String:"), "<br/>");
    if (!g_strNewline || !g_strBr) { LOGE("could not build comparison strings"); return 0; }
    /* Retained for the process lifetime: these are consulted from inside
       the hooks, which run outside any pool this constructor controls. */
    MSG_id(g_strNewline, selReg("retain"));
    MSG_id(g_strBr, selReg("retain"));

    /* 6 - every encoding, before any setImp. hook() below checks each of
       these again; that duplicate check is deliberate, not redundant to
       remove - passing the real ENC_* to hook() rather than NULL keeps
       verification live even if this pre-check block is ever dropped or
       reordered, at the cost of a duplicate log line per hook. */
    if (!checkEncoding(m,  "viewServerWelcomeMessage:customRules:allowEdit:", ENC_VIEW_WELCOME))
        return 0;
    if (!checkEncoding(mR, "stringByReplacingOccurrencesOfString:withString:", ENC_REPLACE_OCCUR))
        return 0;
    if (!checkEncoding(mC, "componentsSeparatedByCharactersInSet:", ENC_COMPONENTS))
        return 0;

    /* 7 - commit, rolling back already-committed hooks if a later one
       fails. The transactional encoding pre-check above makes a
       mid-sequence failure unlikely - setImp has no documented failure
       mode - but there is no reason to leave the process half-patched if
       one ever occurs. */
    if (!hook(gameView, "viewServerWelcomeMessage:customRules:allowEdit:",
              ENC_VIEW_WELCOME, (IMP)my_viewWelcome, &g_origViewWelcome))
        return 0;

    if (!hook(nsstring, "stringByReplacingOccurrencesOfString:withString:",
              ENC_REPLACE_OCCUR, (IMP)my_replaceOccurrences, &g_origReplaceOccur)) {
        setImp(m, g_origViewWelcome);
        return 0;
    }

    if (!hook(nsstring, "componentsSeparatedByCharactersInSet:",
              ENC_COMPONENTS, (IMP)my_componentsSeparated, &g_origComponentsSep)) {
        setImp(m,  g_origViewWelcome);
        setImp(mR, g_origReplaceOccur);
        return 0;
    }

    LOGI("installed");
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
