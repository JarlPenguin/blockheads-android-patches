/*
 * libsignfix.so
 *
 * THE BUG
 * -------
 * The Android BlockTextPromptAlertView's -text getter wraps the UITextView's
 * text, applies its three-line limit, then calls uppercaseString. Its
 * -textViewDidChange: immediately writes that uppercase result back into the
 * view and puts the cursor at the end. Thus the first edit uppercases even a
 * sign loaded with lowercase text, and each edit restarts IME composition.
 * The confirmation block also calls -text, uppercasing text on save.
 *
 * THE FIX
 * -------
 * Mark only prompts created by -[UIManager displaySignUIForSign:]. For that
 * marked instance, reproduce -text's wrapping, three-line check and callback
 * without uppercaseString. In -textViewDidChange: avoid setText: when text is
 * already equal, retaining the original rewrite and cursor behavior when the
 * text actually changes. Other prompts chain to their original methods.
 * The initial UITextView setText: and sign persistence are not changed.
 * Target method encodings are verified against on-device values. A JNI
 * creation-scope query lets the Java TextView set the sign editor's inset,
 * centered alignment and linear/subpixel font paint before it is shown;
 * other text views stay unchanged.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "signfix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * BUILD
 * -----
 *   armeabi-v7a, matching the rest of the APK:
 *   $CC -shared -fPIC -O2 -o libsignfix.so signfix.c -llog -ldl
 *   The associated TextView.smali declares signfixIsCreatingSign().
 *   then drop it in lib/armeabi-v7a/ before repacking.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>

#ifndef SIGNFIX_VERBOSE
#define SIGNFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef SIGNFIX_STRICT_ENCODING
#define SIGNFIX_STRICT_ENCODING 1
#endif

#define TAG "SIGNFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if SIGNFIX_VERBOSE
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

#if SIGNFIX_STRICT_ENCODING
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

/* Measured on-device from the runtime, not inferred from Hex-Rays prototypes. */
#define ENC_DISPLAY_SIGN   "v12@0:4@8"
#define ENC_PROMPT_TEXT    "@8@0:4"
#define ENC_TEXT_DID_CHANGE "v12@0:4@8"

typedef struct { uint32_t location, length; } BHRange;
typedef struct {
    void *isa;
    uint32_t flags;
    void *reserved;
    void *invoke;  /* ARM block invoke is at +12, as in the IDA listing. */
} BHBlock;

/* The game passes 120. The on-device probes and supplied wrapper's strict
   less-than check establish l = 4 units and a = 10 units: at 130, 32 l's
   (128) and a+29 l's (126) fit, whereas 33 l's (132) and a+30 l's (130)
   break. This matches the two observed iOS line limits. Keep the width
   overridable while checking other glyphs and three-line signs. */
#ifndef SIGNFIX_WRAP_WIDTH
#define SIGNFIX_WRAP_WIDTH 130
#endif

/* Message signatures that the shared MSG_* block does not cover. The
   constrained width is the immediate 120 in the supplied ARM decompilation;
   the NSRange fields are two 32-bit words on armeabi-v7a. */
#define MSG_id_id_u32(o,s,a,n) \
    (((id (*)(id,SEL,id,uint32_t))g_msgSend)((o),(s),(a),(n)))
#define MSG_id_range(o,s,r) \
    (((id (*)(id,SEL,BHRange))g_msgSend)((o),(s),(r)))
#define MSG_v_range(o,s,r) \
    (((void (*)(id,SEL,BHRange))g_msgSend)((o),(s),(r)))
#define MSG_v_id(o,s,a) \
    (((void (*)(id,SEL,id))g_msgSend)((o),(s),(a)))
#define MSG_id_cls_cstr(c,s,p) \
    (((id (*)(Class,SEL,const char *))g_msgSend)((c),(s),(p)))

static void (*g_setAssociated)(id, const void *, id, uintptr_t);
static id   (*g_getAssociated)(id, const void *);
static char g_signMarker;
static volatile uint32_t g_signCreationDepth;
static uint32_t g_signTextUIOffset;
static Class g_bitmapFont;
static id g_newline;

static SEL g_selTextView, g_selText, g_selEmbossedFont, g_selWrap;
static SEL g_selComponents, g_selCount, g_selLength, g_selSubstring;
static SEL g_selCallBack, g_selSetText, g_selIsEqual, g_selSelectedRange;
static SEL g_selUTF8String;

static IMP g_origDisplaySign, g_origText, g_origDidChange;

#if SIGNFIX_VERBOSE
static Class (*g_objectClass)(id);
static const char *(*g_className)(Class);
static SEL g_selProxy;

/* Resolve the method actually installed at runtime, rather than assuming
   that xrefs to a particular selector-string copy identify its definition.
   dladdr reports the ELF image and load-relative offset for inspection in
   IDA. A forwarding stub is itself useful evidence of a dynamic bridge. */
static void logPlatformMethod(Class cls, const char *clsName,
                              const char *selector) {
    Method method;
    IMP imp;
    Dl_info info;
    const char *encoding;

    if (!cls) { LOGI("%s unavailable", clsName); return; }
    method = getInstMethod(cls, selReg(selector));
    if (!method) { LOGI("%s %s absent", clsName, selector); return; }
    imp = getImp(method);
    encoding = getTypeEnc ? getTypeEnc(method) : NULL;
    if (dladdr((void *)imp, &info))
        LOGI("%s %s IMP=%p %s+0x%lx symbol=%s encoding=%s", clsName,
             selector, imp, info.dli_fname ? info.dli_fname : "(unknown)",
             (unsigned long)((uintptr_t)imp - (uintptr_t)info.dli_fbase),
             info.dli_sname ? info.dli_sname : "(none)",
             encoding ? encoding : "(unavailable)");
    else
        LOGI("%s %s IMP=%p image unavailable encoding=%s", clsName,
             selector, imp, encoding ? encoding : "(unavailable)");
}
#endif

static int isSignPrompt(id prompt) {
    return prompt && g_getAssociated(prompt, &g_signMarker) == (id)&g_signMarker;
}

/* The Java TextView's _setBackgroundColor: path runs during the original
   sign-prompt initializer, after setFont: and before show. Its private static
   native query resolves to this JNI symbol from libsignfix.so. JNI pointers
   are opaque here: no VM calls are needed, and jboolean is one byte. */
__attribute__((visibility("default")))
unsigned char Java_com_apportable_ui_TextView_signfixIsCreatingSign(
    void *env, void *cls) {
    unsigned char active = __sync_fetch_and_add(&g_signCreationDepth, 0) != 0;
    (void)env;
    (void)cls;
    LOGV("Java TextView sign creation query: %u", (unsigned)active);
    return active;
}

/* -[UIManager displaySignUIForSign:] creates the prompt and stores it in
   signTextUI before returning. Mark that *instance*, not the entire prompt
   class: world names and other prompts retain the game's original behaviour.
   An assign association disappears with the prompt, unlike a stale global
   pointer that might later alias a newly allocated object. */
static id my_displaySign(id self, SEL _cmd, id sign) {
    id result;
    __sync_add_and_fetch(&g_signCreationDepth, 1);
    result = ((id (*)(id, SEL, id))g_origDisplaySign)(self, _cmd, sign);
    __sync_sub_and_fetch(&g_signCreationDepth, 1);
    id prompt = *(id *)((char *)self + g_signTextUIOffset);
    if (prompt) g_setAssociated(prompt, &g_signMarker, (id)&g_signMarker, 0);
#if SIGNFIX_VERBOSE
    /* The selector references in the binary identify callers, not the
       receiver's implementation. Inspect the actual sign UITextView proxy
       after the original display method has created and shown the prompt. */
    if (prompt && g_objectClass && g_className) {
        id view = MSG_id(prompt, g_selTextView);
        id proxy = view ? MSG_id(view, g_selProxy) : NULL;
        Class viewClass = view ? g_objectClass(view) : NULL;
        Class proxyClass = proxy ? g_objectClass(proxy) : NULL;
        const char *proxyName = proxyClass ? g_className(proxyClass) : "(none)";
        LOGI("sign UITextView class=%s proxy class=%s",
             viewClass ? g_className(viewClass) : "(none)", proxyName);
        if (proxyClass) {
            logPlatformMethod(proxyClass, proxyName, "setFont:size:");
            logPlatformMethod(proxyClass, proxyName, "setText:");
        }
    } else if (prompt) {
        LOGI("sign proxy class introspection unavailable");
    }
#endif
    return result;
}

/* The supplied getter obtains [BitmapFont embossedFont], wraps the text at
   the configured width (130 by default), counts newline-separated lines, removes one trailing input
   character and wraps again if there are more than three lines, then invokes
   its stored callback. Its final uppercaseString is the Android sign bug.
   Preserve all earlier steps and the callback for sign prompts. */
static id my_text(id self, SEL _cmd) {
    id font, textView, raw, wrapped, lines, callback;
    uint32_t count, length;
    BHRange range;

    if (!isSignPrompt(self))
        return ((id (*)(id, SEL))g_origText)(self, _cmd);

    font = MSG_id((id)g_bitmapFont, g_selEmbossedFont);
    textView = MSG_id(self, g_selTextView);
    raw = MSG_id(textView, g_selText);
    wrapped = MSG_id_id_u32(font, g_selWrap, raw, SIGNFIX_WRAP_WIDTH);
#if SIGNFIX_VERBOSE
    /* The editor can also wrap visually. Capture the actual native wrapper
       output so a displayed line break is not mistaken for an inserted one.
       Probe the view's 138-wide frame value without changing the active wrap.
       Compile with -DSIGNFIX_VERBOSE=1 only while investigating a sign. */
    LOGV("wrap %u: raw=%s wrapped=%s", (unsigned)SIGNFIX_WRAP_WIDTH,
         MSG_str(raw, g_selUTF8String), MSG_str(wrapped, g_selUTF8String));
    LOGV("wrap 138 probe: %s", MSG_str(
         MSG_id_id_u32(font, g_selWrap, raw, 138), g_selUTF8String));
    /* On iOS 32 l's fit, but a followed by 30 l's does not. Probe the
       candidate strict-less-than cutoffs without changing the returned text.
       Only emit these extra logs for a long a+l input in verbose builds. */
    {
        const char *r = MSG_str(raw, g_selUTF8String);
        if (r && r[0] == 'a' && r[1] == 'l'
            && (uint32_t)(uintptr_t)MSG_id(raw, g_selLength) >= 30) {
            uint32_t width;
            for (width = 129; width <= 131; ++width) {
                id trial = MSG_id_id_u32(font, g_selWrap, raw, width);
                id trialLines = MSG_id_id(trial, g_selComponents, g_newline);
                LOGV("a+l width=%u rawLength=%u lineCount=%u", width,
                     (unsigned)(uint32_t)(uintptr_t)MSG_id(raw, g_selLength),
                     (unsigned)(uint32_t)(uintptr_t)MSG_id(trialLines, g_selCount));
            }
        }
    }
#endif
    lines = MSG_id_id(wrapped, g_selComponents, g_newline);
    count = (uint32_t)(uintptr_t)MSG_id(lines, g_selCount);
    if (count > 3) {
        length = (uint32_t)(uintptr_t)MSG_id(raw, g_selLength);
        range.location = 0;
        range.length = length - 1; /* Exactly as in the supplied getter. */
        raw = MSG_id_range(raw, g_selSubstring, range);
        wrapped = MSG_id_id_u32(font, g_selWrap, raw, SIGNFIX_WRAP_WIDTH);
#if SIGNFIX_VERBOSE
        LOGV("line limit %u: shortened=%s wrapped=%s", count,
             MSG_str(raw, g_selUTF8String), MSG_str(wrapped, g_selUTF8String));
#endif
    }

    callback = MSG_id(self, g_selCallBack);
    if (callback) {
        BHBlock *block = (BHBlock *)callback;
        ((int (*)(BHBlock *, id))block->invoke)(block, self);
    }
    LOGV("sign text returned without uppercaseString");
    return wrapped;
}

/* The original textViewDidChange: calls [self text], then unconditionally
   setText: and setSelectedRange: to the end. For a sign, an equal text value
   needs no replacement; avoiding it keeps the IME's composition alive.
   When the width/line limit changes the text, retain the original cursor
   behaviour. Every other prompt chains to its original implementation. */
static void my_didChange(id self, SEL _cmd, id textViewArg) {
    id textView, normalized, current;
    BHRange range;

    if (!isSignPrompt(self)) {
        ((void (*)(id, SEL, id))g_origDidChange)(self, _cmd, textViewArg);
        return;
    }
    textView = MSG_id(self, g_selTextView);
    normalized = MSG_id(self, g_selText);
    current = MSG_id(textView, g_selText);
    if (normalized == current || (normalized && current
        && MSG_c_id(current, g_selIsEqual, normalized))) return;

    MSG_v_id(textView, g_selSetText, normalized);
    range.location = (uint32_t)(uintptr_t)MSG_id(normalized, g_selLength);
    range.length = 0;
    MSG_v_range(textView, g_selSelectedRange, range);
}

static int install(void) {
    Class manager, prompt, nsstring;
    Method mDisplay, mText, mDidChange;
    const char *how;

    /* 1 */
    manager = getClass("UIManager");
    prompt = getClass("BlockTextPromptAlertView");
    g_bitmapFont = getClass("BitmapFont");
    nsstring = getClass("NSString");
    if (!manager || !prompt || !g_bitmapFont || !nsstring) {
        LOGE("classes not present - load point too early"); return 0;
    }

    /* 2 */
    if (!g_msgSend) { LOGE("objc_msgSend missing"); return 0; }

    /* 3 */
    mDisplay = getInstMethod(manager, selReg("displaySignUIForSign:"));
    mText = getInstMethod(prompt, selReg("text"));
    mDidChange = getInstMethod(prompt, selReg("textViewDidChange:"));
    if (!mDisplay || !mText || !mDidChange) {
        LOGE("sign prompt methods missing"); return 0;
    }
    if (getImp(mDisplay) == (IMP)my_displaySign) {
        LOGI("already installed"); return 1;
    }

    /* 4 - no static offset was supplied, so fail closed if runtime ivar
       introspection and exported-symbol lookup both fail. */
    if (!resolve_ivar("UIManager", "signTextUI", &g_signTextUIOffset,
                      0, &how)) return 0;
    LOGI("UIManager.signTextUI offset 0x%x via %s", g_signTextUIOffset, how);

    /* 5 - associations are required to mark just the sign prompt. */
    g_setAssociated = dlsym(g_sys, "objc_setAssociatedObject");
    g_getAssociated = dlsym(g_sys, "objc_getAssociatedObject");
    if (!g_setAssociated || !g_getAssociated) {
        LOGE("ObjC associations unavailable - refusing unscoped hook"); return 0;
    }
    g_selTextView = selReg("textView");
    g_selText = selReg("text");
    g_selEmbossedFont = selReg("embossedFont");
    g_selWrap = selReg("wrappedString:constrainedToWidth:");
    g_selComponents = selReg("componentsSeparatedByString:");
    g_selCount = selReg("count");
    g_selLength = selReg("length");
    g_selSubstring = selReg("substringWithRange:");
    g_selCallBack = selReg("callBack");
    g_selSetText = selReg("setText:");
    g_selIsEqual = selReg("isEqualToString:");
    g_selSelectedRange = selReg("setSelectedRange:");
    g_selUTF8String = selReg("UTF8String");
#if SIGNFIX_VERBOSE
    g_selProxy = selReg("_proxy");
    g_objectClass = dlsym(g_sys, "object_getClass");
    g_className = dlsym(g_sys, "class_getName");
#endif
    g_newline = MSG_id_cls_cstr(nsstring, selReg("stringWithUTF8String:"), "\n");
    if (!g_newline) { LOGE("could not construct newline string"); return 0; }
    MSG_id(g_newline, selReg("retain"));
#if SIGNFIX_VERBOSE
    logPlatformMethod(getClass("UITextView"), "UITextView", "_platform_setFont:");
    logPlatformMethod(getClass("UITextView"), "UITextView", "_platform_setText:");
    logPlatformMethod(getClass("VerdeUITextView"), "VerdeUITextView",
                      "_platform_setFont:");
    logPlatformMethod(getClass("VerdeUITextView"), "VerdeUITextView",
                      "_platform_setText:");
#endif

    /* 6 - verify all three measured encodings before installing any hook. */
    if (!checkEncoding(mDisplay, "displaySignUIForSign:", ENC_DISPLAY_SIGN) ||
        !checkEncoding(mText, "text", ENC_PROMPT_TEXT) ||
        !checkEncoding(mDidChange, "textViewDidChange:", ENC_TEXT_DID_CHANGE)) return 0;

    /* 7 - commit after all prerequisites, with rollback on failure. */
    if (!hook(manager, "displaySignUIForSign:", ENC_DISPLAY_SIGN,
              (IMP)my_displaySign, &g_origDisplaySign)) return 0;
    if (!hook(prompt, "text", ENC_PROMPT_TEXT, (IMP)my_text, &g_origText)) {
        setImp(mDisplay, g_origDisplaySign); return 0;
    }
    if (!hook(prompt, "textViewDidChange:", ENC_TEXT_DID_CHANGE,
              (IMP)my_didChange, &g_origDidChange)) {
        setImp(mText, g_origText);
        setImp(mDisplay, g_origDisplaySign);
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
