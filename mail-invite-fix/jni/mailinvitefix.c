/*
 * libmailinvitefix.so
 *
 * THE BUG
 * -------
 * The ShareUI and LoadWorldUI alertView:didDismissWithButtonIndex: methods
 * both format the same mailto string. They escape its subject, but insert the
 * full text-field invite URL as an unescaped final %@ in the body. A mailto
 * parser treats the invite URL's '=' and '&' as header-query delimiters.
 * The fixed body text also says "from your mobile device join my world",
 * omitting "to" before "join".
 *
 * THE FIX
 * -------
 * The exact mailto format ends with the invite URL. Intercept NSURL's
 * +URLWithString: only for that format, and percent-encode the URL suffix as
 * UTF-8, and insert %20to into the fixed body text. This leaves the other
 * already encoded body text, subject, copied invite URL and every unrelated
 * URL alone. The original URL constructor and UIApplication openURL: remain
 * on the original path.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "mailinvitefix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libmailinvitefix.so mailinvitefix.c -llog
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>


#ifndef MAILINVITEFIX_VERBOSE
#define MAILINVITEFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef MAILINVITEFIX_STRICT_ENCODING
#define MAILINVITEFIX_STRICT_ENCODING 1
#endif

#define TAG "MAILINVITEFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if MAILINVITEFIX_VERBOSE
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

#if MAILINVITEFIX_STRICT_ENCODING
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

/* This patch encodes UTF-8 bytes and allocates a replacement URI, so these
   standard C functions are genuinely file-specific. */
#include <stdlib.h>
#include <string.h>

/* Class methods are obtained with class_getClassMethod. The canonical
   hook() helper targets instance methods, so step 7 commits this class-method
   hook directly through its Method; it remains the same +URLWithString: hook. */

static Class g_nsstring;
static SEL g_selUTF8String;
static SEL g_selStringWithUTF8String;
static IMP g_origURLWithString;

/* The single body marker, after NSString stringWithFormat: turns %% into %. */
static const char kMailPrefix[] = "mailto:?to=&subject=";
static const char kBodyMarker[] =
    "&body=Click%20the%20link%20below%20from%20your%20mobile%20device"
    "%20join%20my%20world%20in%20The%20Blockheads!%0D%0A%0D%0A";
static const char kInviteURL[] = "http://theblockheads.net/join.php?";
static const char kMissingTo[] = "device%20join";
static const char kAddTo[] = "%20to";

/* Measured on the target build for +[NSURL URLWithString:]. */
#define ENC_URL_WITH_STRING "@12@0:4@8"

static int is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

/* NULL means that this is not the invite email or allocation failed. Caller
 * then forwards the original argument unchanged. The template's final %@
 * guarantees everything after the marker is the separate invite URL. The
 * insertion changes only the known typo in that template's body text. */
static char *rewrite_invite_mailto(const char *raw) {
    static const char hex[] = "0123456789ABCDEF";
    const char *marker, *url, *typo;
    size_t prefix_len, insertion_at, url_len, capacity, i, j;
    char *out;

    if (!raw || strncmp(raw, kMailPrefix, sizeof(kMailPrefix) - 1) != 0)
        return NULL;
    marker = strstr(raw + sizeof(kMailPrefix) - 1, kBodyMarker);
    if (!marker) return NULL;
    url = marker + sizeof(kBodyMarker) - 1;
    if (strncmp(url, kInviteURL, sizeof(kInviteURL) - 1) != 0)
        return NULL;
    typo = strstr(marker, kMissingTo);
    if (!typo || typo >= url) return NULL;

    prefix_len = (size_t)(url - raw);
    insertion_at = (size_t)(typo + sizeof("device") - 1 - raw);
    url_len = strlen(url);
    if (prefix_len > SIZE_MAX - sizeof(kAddTo)) return NULL;
    if (url_len > (SIZE_MAX - prefix_len - sizeof(kAddTo)) / 3)
        return NULL;
    capacity = prefix_len + sizeof(kAddTo) - 1 + url_len * 3 + 1;
    out = malloc(capacity);
    if (!out) return NULL;

    memcpy(out, raw, insertion_at);
    memcpy(out + insertion_at, kAddTo, sizeof(kAddTo) - 1);
    memcpy(out + insertion_at + sizeof(kAddTo) - 1,
           raw + insertion_at, prefix_len - insertion_at);
    j = prefix_len + sizeof(kAddTo) - 1;
    for (i = 0; i < url_len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (is_unreserved(c)) {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }
    out[j] = '\0';
    return out;
}

/* Type encoding was measured on the target build: @12@0:4@8.
 * NSURL's +URLWithString: is a class method, not an instance method. */
static id my_URLWithString(id cls, SEL cmd, id string) {
    const char *raw;
    char *rewritten;
    id new_string, result;

    if (!string)
        return ((id (*)(id, SEL, id))g_origURLWithString)(cls, cmd, string);
    raw = MSG_str(string, g_selUTF8String);
    rewritten = rewrite_invite_mailto(raw);
    if (!rewritten)
        return ((id (*)(id, SEL, id))g_origURLWithString)(cls, cmd, string);

    new_string = ((id (*)(Class, SEL, const char *))g_msgSend)(
        g_nsstring, g_selStringWithUTF8String, rewritten);
    free(rewritten);
    if (!new_string) {
        LOGE("could not create encoded invite string; forwarding original");
        return ((id (*)(id, SEL, id))g_origURLWithString)(cls, cmd, string);
    }
    result = ((id (*)(id, SEL, id))g_origURLWithString)(cls, cmd, new_string);
    LOGV("encoded invite URL in mailto body");
    return result;
}

static int install(void) {
    Class nsurl;
    Method method;
    Method (*classMethod)(Class, SEL);
    IMP cur;

    /* 1 */
    nsurl = getClass("NSURL");
    g_nsstring = getClass("NSString");
    if (!nsurl || !g_nsstring) {
        LOGE("NSURL/NSString not loaded");
        return 0;
    }

    /* 2 - my_URLWithString uses MSG_str to read the NSString bytes. */
    if (!g_msgSend) {
        LOGE("required Objective-C runtime symbol missing");
        return 0;
    }

    /* 3 - preserve the actual class-method lookup and the old duplicate-hook
       log event. The hook remains installed if this installer runs twice. */
    classMethod = dlsym(g_sys, "class_getClassMethod");
    if (!classMethod) {
        LOGE("required Objective-C runtime symbol missing");
        return 0;
    }
    method = classMethod(nsurl, selReg("URLWithString:"));
    if (!method) { LOGE("+[NSURL URLWithString:] not found"); return 0; }
    cur = getImp(method);
    if (cur == (IMP)my_URLWithString) {
        LOGE("unexpected current +[NSURL URLWithString:] implementation");
        return 1;
    }

    /* No ivars are used; step 4 does not apply. */

    /* 5 - selectors are initialized before the class-method IMP changes. */
    g_selUTF8String = selReg("UTF8String");
    g_selStringWithUTF8String = selReg("stringWithUTF8String:");

    /* A single hook needs no transactional step 6 pre-check. */

    /* 7 - retain the same class Method and implementation. The measured
       encoding is checked before changing the IMP. */
    if (!checkEncoding(method, "+[NSURL URLWithString:]", ENC_URL_WITH_STRING))
        return 0;
    if (!cur) {
        LOGE("unexpected current +[NSURL URLWithString:] implementation");
        return 0;
    }
    g_origURLWithString = cur;
    setImp(method, (IMP)my_URLWithString);
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


