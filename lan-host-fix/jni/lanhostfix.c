/*
 * liblanhostfix.so
 *
 * THE BUG
 * -------
 * LoadWorldUI.optionsButton: and JoinWorldUI.advancedButton: omit native LAN
 * controls. Foundation loses browsable-domain delivery; NSNetService.port
 * has reversed byte order and Bionic does not resolve saved .local names.
 * GameView.didEnterBackground tears down a LAN host during temporary UI;
 * its paused update:accurateDT: path can call server sends off the main thread.
 * TableSelectionAlertView uses incorrect panel caps and a nil titleLabel.
 *
 * THE FIX
 * -------
 * Restore retained native controls, bridge domain callbacks, repair selected
 * endpoints and saved mDNS lookup, reuse all six native client UI exemptions,
 * route host updates to NSThread.mainThread, and match BlockAlertView caps
 * while setting the existing UIButton._titleView font. Preserve native actions,
 * networking, cleanup, cloud behavior, diagnostic forwarding and disassembly
 * notes. Routine trace output is opt-in; failures remain visible.
 * Device-name input is scoped to the bundled daemon; native conflicts remain.
 * The 15-point row inset is accepted. Ad tests are out of scope. Intermittent
 * discovery failures and the separate libSystem+0x930f4 crash are not fixed here.
 *
 * LOAD POINT
 * ----------
 * BackgroundLibraryLoader$1.run(), after LibraryManager.loadLibraries():
 *
 *     const-string vN, "lanhostfix"
 *     invoke-static {vN}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
 *
 * Load after the original ObjC libraries, before first LAN use/class setup.
 *
 * BUILD
 * -----
 *   armeabi-v7a, API 21, C11, matching the APK (with NDK toolchain $CC):
 *   $CC -shared -fPIC -O2 -std=c11 -Wall -Wextra -Werror -o liblanhostfix.so lanhostfix.c -llog -ldl
 *   or: "$ANDROID_NDK_HOME/ndk-build" -C lan-host-fix
 *   then drop it in lib/armeabi-v7a/ before repacking.
 *   Add -DLANHOSTFIX_VERBOSE=1 for per-call traces (default: 0).
 *   Daemon startup before domain enumeration is unconditional.
 *   The host-test copy lives in tests/; tests/sync-production.py generates
 *   the standalone ARM32 production copy from the host-test source.
 */

/* Experimental Blockheads 1.7.5 LAN hosting/join UI restoration.
 * Load after LibraryManager.loadLibraries(), like the other repository fixes.
 * No binary addresses, transport replacements, or cloud-credit changes.
 * Runtime encodings are checked before calling methods through typed IMPs.
 */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <elf.h>
#include <link.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <jni.h>

#ifndef LANHOSTFIX_VERBOSE
#define LANHOSTFIX_VERBOSE 0
#endif

/* Set STRICT_ENCODING to 0 only to diagnose a mismatch. Wrong arity on a
   replaced method corrupts the stack, so the default is to fail closed. */
#ifndef LANHOSTFIX_STRICT_ENCODING
#define LANHOSTFIX_STRICT_ENCODING 1
#endif

#define TAG "LANHOSTFIX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#if LANHOSTFIX_VERBOSE
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

#if LANHOSTFIX_STRICT_ENCODING
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

/* LAN flow: restore the game's existing host/join controls, deliver native
 * domain enumeration on Foundation's main queue, and leave world selection
 * with the game. DNS-SD supplies an IPv4 endpoint for an immediate join;
 * saved .local entries use the bundled daemon when the system resolver fails.
 * Hosting repairs separately scope automatic welcome display, main-thread
 * updates and background server preservation. Dialog fixes reuse the game's
 * images, fonts and controls. Each feature can fail without disabling unrelated
 * features; enable flags below describe the dependencies for endpoint repair.
 */
/* LAN constants and selector encoding expectations. */
/* Measured on the supplied ARM32 target, not inferred from a C prototype. */
#define ENC_FONT_PROXY "@8@0:4"
#define ENC_GAME_UPDATE "v16@0:4f8f12"
#define ENC_WELCOME_DISPLAY "v20@0:4@8@12c16"
#define ENC_WORLD_INIT "@60@0:4^{WindowInfo=ffffffff}8@12@16@20@24@28@32@36@40i44i48@52c56"

/* Measured Android runtime encodings from the 2026-10-10 device log. Keep
 * the compact Java-proxy strings verbatim too: exact checks compare metadata,
 * not reconstructed offsets. Structural ABI checks still precede each hook.
 * Only the two callbacks absent from that log remain unmeasured below.
 */
#define ENC_OPTIONS "v12@0:4@8"
#define ENC_WINDOW_CHANGED "v12@0:4^{WindowInfo=ffffffff}8"
#define ENC_BACK "v12@0:4@8"
#define ENC_HOST_ACTION "v12@0:4@8"
#define ENC_ADVANCED "v12@0:4@8"
#define ENC_LOCAL_JOIN_ACTION "v12@0:4@8"
#define ENC_START_SERVER "c8@0:4"
#define ENC_SEARCH_SERVERS "v8@0:4"
#define ENC_DOMAIN_SEARCH "v8@0:4"
#define ENC_SERVICE_SEARCH "v16@0:4@8@12"
#define ENC_FOUND_DOMAIN "v20@0:4@8@12c16"
#define ENC_FOUND_SERVICE "v20@0:4@8@12c16"
#define ENC_RESOLVED_SERVICE "v12@0:4@8"
/* Used in production. If absent, addMethod supplies v16@0:4@8@12;
 * that declaration does not measure an existing implementation. */
#define ENC_SEARCH_ERROR NULL /* Unmeasured netServiceBrowser:didNotSearch:. */
#define ENC_RESOLVE_ERROR "v16@0:4@8@12"
#define ENC_WORLD_SELECTION "v12@0:4i8"
#define ENC_CLIENT_CONNECT "v8@0:4"
#define ENC_WORLD_OPTIONS "v16@0:4@8@12"
#define ENC_SERVICE_PORT "i8@0:4"
#define ENC_INITIALIZE "v8@0:4"
#define ENC_SHOW_NOW "v8@0:4"
#define ENC_STRETCH_IMAGE "@16@0:4i8i12"
#define ENC_DIALOG_CELL "@16@0:4@8@12"
#define ENC_DIALOG_INIT "@20@0:4@8@12@16"
#define ENC_ASSET_TYPEFACE "@@:@@"
#define ENC_FILE_TYPEFACE "@@:@"
#define ENC_NAMED_TYPEFACE "@@:@i"
#define ENC_SERVER_CLEANUP "v8@0:4"
#define ENC_SERVER_DISCONNECT "v8@0:4"
#define ENC_SERVER_DEALLOC "v8@0:4"
#define ENC_GAME_BACKGROUND "v8@0:4"
/* Used in production. If absent, addMethod supplies v12@0:4@8. */
#define ENC_SERVER_PUBLISH NULL /* Unmeasured netServiceDidPublish:. */
#define ENC_SERVER_PUBLISH_ERROR "v16@0:4@8@12"
#define ENC_GAME_HOSTING "v16@0:4@8@12"
#define ENC_SERVER_INIT "@28@0:4@8@12i16@20i24"
#define ENC_BROWSER_INIT "@8@0:4"
#define ENC_BROWSER_DISPATCH "v20@0:4^v8L12^{?=ii}16"

/* Installation counters belong only to verbose summaries. Always execute
 * the operation in quiet builds: several diagnostic hooks also forward
 * callbacks used by the endpoint and search-error repairs.
 */
#if LANHOSTFIX_VERBOSE
#define COUNT_DIAGNOSTIC(counter, operation) ((counter) += (operation))
#else
#define COUNT_DIAGNOSTIC(counter, operation) ((void)(operation))
#endif

/* Only the bundled daemon thread receives this initialization input. The
 * daemon still owns probing, negotiated suffixes and service record targets.
 * Android's Settings value is already persistent; no installation ID or
 * Android-wide hostname change is introduced.
 */
#define DEVICE_LABEL_SIZE 64
#define LAN_ENDPOINT_COUNT 64
#define LAN_HOSTNAME_SIZE 1024

#define LOCAL_DNS_WAIT_MS 3000

static const char *const welcomeWorldSelector =
    "initWithWindowInfo:cache:delegate:saveID:name:client:server:multiplayerWorldData:serverHostData:saveDelay:worldWidthMacro:customRules:expertMode:";

static const char *const lifecycleFieldNames[6] = {
    "sNativeApplicationInBackground", "sWindowHasFocus", "sActivityPaused",
    "sActivityWasStopped", "sContextHasSurface", "sNativeApplicationStarted"
};

/* Target ABI records and feature-local payloads. */

typedef struct { float x, y, w, h; } Rect;
typedef struct { float width, height; } DialogSize;
typedef struct { float top, left, bottom, right; } DialogInsets;
typedef struct { struct in_addr host; uint16_t port; } ENetProbeAddress;

_Static_assert(sizeof(void *) == 4, "This patch targets ARM32 only");

_Static_assert(sizeof(Rect) == 16, "Expected four float CGRect members");
typedef struct { intptr_t domain; int32_t error; } BrowserError;
typedef struct {
    intptr_t version;
    void *info;
    const void *(*retain)(const void *);
    void (*release)(const void *);
    const void *(*copyDescription)(const void *);
} BrowserContext;
typedef void (*BrowserCallback)(void *, uint32_t, const void *, const BrowserError *, void *);
typedef struct {
    unsigned references;
    BrowserCallback callback;
    void *info;
} WrappedBrowserContext;
typedef struct {
    id owner, delegate;
    const void *domain;
    uint32_t flags;
    BrowserError error;
} QueuedDomain;

_Static_assert(sizeof(BrowserError) == 8, "Expected ARM32 CFStreamError");

_Static_assert(sizeof(BrowserContext) == 20, "Expected ARM32 CFNetServiceClientContext");

typedef struct {
    id match;
    char hostname[LAN_HOSTNAME_SIZE];
    struct in_addr address;
    uint16_t port;
    int valid;
} LANEndpoint;

/* Native DNS-SD client ABI; QueryRecord's A record is four wire-format bytes,
 * avoiding the Apple/Linux sockaddr layout difference in GetAddrInfo replies.
 */
typedef void (*DNSRecordReply)(void *, uint32_t, uint32_t, int32_t, const char *,
                               uint16_t, uint16_t, uint16_t, const void *, uint32_t, void *);

/* Observe CFNetwork's immediate DNS-SD results before Foundation maps errors.
 * Callback pointers, contexts, shared refs and request fields pass unchanged.
 * No extra DNS requests, callbacks, socket reads or ownership changes.
 */
typedef void (*DNSDomainReply)(void *, uint32_t, uint32_t, int32_t, const char *, void *);
typedef void (*DNSBrowseReply)(void *, uint32_t, uint32_t, int32_t,
                                const char *, const char *, const char *, void *);
typedef void (*DNSResolveReply)(void *, uint32_t, uint32_t, int32_t,
                                 const char *, const char *, uint16_t, uint16_t,
                                 const unsigned char *, void *);
typedef void (*DNSRegisterReply)(void *, uint32_t, int32_t,
                                  const char *, const char *, const char *, void *);
typedef struct {
    const char *hostname;
    struct in_addr address;
    int found;
    int32_t error;
} LocalDNSQuery;

typedef struct {
    const char *library, *symbol;
    void *replacement, *g_origStorage;
    int installed;
    int allowData;
    void *expectedTarget;
    Dl_info provider;
} ImportRequest;

typedef struct { id world, delegate; int localHost; } WelcomeWorldContext;
typedef struct { id game, world, server; uint32_t token; double dt, accurate; } WelcomePendingUpdate;

/* Additional runtime, framework and platform dependencies. */

static Class (*objectClass)(id);
static unsigned char (*addMethod)(Class, SEL, IMP, const char *);
static const char *(*ivarEncoding)(Ivar);
static const char *(*runtimeClassName)(Class);
static Class (*runtimeSuperclass)(Class);
static void (*ensureMDNS)(void);
static void (*prepareDeviceHostname)(void);
static int (*enetSetHost)(ENetProbeAddress *, const char *);
static int *(*nativeHostError)(void);
static const void *(*cfRetain)(const void *);
static void (*cfRelease)(const void *);
static uintptr_t (*cfGetTypeID)(const void *), (*cfStringTypeID)(void);
static void (*asyncFunction)(void *, void *, void (*)(void *));
static int32_t (*dnsQueryRecord)(void **, uint32_t, uint32_t, const char *,
                                uint16_t, uint16_t, DNSRecordReply, void *);
static int (*dnsSocketFD)(void *);
static int32_t (*dnsProcessResult)(void *);
static void (*dnsDeallocate)(void *);
static int (*dnsPoll)(struct pollfd *, nfds_t, int) = poll;
static int (*dnsClock)(clockid_t, struct timespec *) = clock_gettime;
static int (*welcomeOnMainThread)(void);
static int (*enqueueWelcomeUpdate)(id game, id world, id server, uint32_t token);

static unsigned char (*hostingSocketValid)(const void *);
static int32_t (*hostingSocketNative)(const void *);

/* Resolved classes, ivar offsets and cached action selectors. */

static Class loadWorldClass;
static uint32_t offState, offHost, offDelete, offBack, offMigrate, offDisabled, offCache, offWindow;
static uint32_t offEnet, offTcp, offPort;
static SEL selHostAction;
static uint32_t offJoinState, offJoinCancel, offJoinLocal, offJoinOnline, offJoinSearch;
static uint32_t offJoinCache, offJoinWindow;
static SEL selJoinLocalAction;
static uint32_t offClientBrowser, offClientDomains;
static uint32_t offClientFoundHosts, offClientHost, offClientPort;
static uint32_t offWelcomeServerMatch;
static Class welcomeServerClass, welcomeMatchClass;

/* Mirror all six guards in the supplied native client shutdown branch.
 * Borrow named runtime metadata and checked native property implementations;
 * never infer protection from Java activity state. Only pickerShowing is
 * consumed here, at the same point as the client's one-shot protection.
 */
static uint32_t offGameServer, offGameWorld, offHostWelcomeFlag;
static uint32_t offHostMovieFlag, offHostIAPFlag, offHostController, offHostClient;

/* The supplied paused-game block calls GameView.update on a dispatch
 * worker. Its client-only poll path is valid for BHNetClientMatch, whose
 * send method has no main-thread assertion. A retained LAN host must run
 * the GameView update on NSThread.mainThread, where server sends are owned.
 * GameView then calls World's longer update selector with camera arguments.
 * The native send method and its thread check are left intact.
 */
static uint32_t offWelcomeWorldServer;
static Class welcomeGameClass;

/* The table dialog requests a zero horizontal cap. Its bundled UIImage
 * implementation instead reserves width-1 points on the right, stretching
 * the first white column. Use fixed white side caps on this panel to match
 * stock Android's thin bitmap borders. Scope creation to its own +initialize.
 */
static Class tableDialogClass;
static uint32_t offDialogView, offDialogTable;

static uint32_t offServerMatch, offServerNodeType, offServerMaxPlayers;
static uint32_t offGameClient;
static uint32_t offServerTimer, offServerService, offServerConnections;
static Class welcomeThreadClass, welcomeDataClass, welcomeArrayClass;

/* Saved implementations and imported entry points, grouped for hook review. */

static IMP g_origOptions, g_origResize, g_origBack, g_origStart;
static IMP g_origAdvanced, g_origJoinResize;
static int (*g_origGetHostname)(char *, size_t);
static void *(*g_origDaemonMain)(void *);
static IMP g_origSearchServers;
static IMP g_origDomainSearch, g_origServiceSearch, g_origFoundDomain;
static IMP g_origFoundService, g_origResolvedService, g_origSearchError, g_origResolveError;
static IMP g_origWorldSelection, g_origClientConnect, g_origWorldOptions;
static struct hostent *(*g_origGetHostByName2)(const char *, int);
static IMP g_origServicePort;
static void *(*g_origBrowserCreate)(const void *, BrowserCallback, BrowserContext *);
static IMP g_origBrowserDispatch, g_origBrowserInit;
static int32_t (*g_origDNSEnumerate)(void **, uint32_t, uint32_t, DNSDomainReply, void *);
static int32_t (*g_origDNSBrowse)(void **, uint32_t, uint32_t, const char *, const char *, DNSBrowseReply, void *);
static int32_t (*g_origDNSResolve)(void **, uint32_t, uint32_t, const char *, const char *, const char *, DNSResolveReply, void *);
static int32_t (*g_origDNSRegister)(void **, uint32_t, uint32_t, const char *, const char *, const char *, const char *,
                                      uint16_t, uint16_t, const void *, DNSRegisterReply, void *);

/* Android opens the welcome activity at the end of World initialization for
 * either a client OR a server. iOS opens it on client connection success.
 * Scope the correction to the synchronous constructor call for a LAN host;
 * manual welcome screens, client joins, and background cleanup still forward.
 */
static IMP g_origWelcomeWorldInit, g_origWelcomeDisplay;
static IMP g_origGameBackground;
static IMP g_origWelcomeUpdate;
static IMP g_origDialogInitialize, g_origDialogShow, g_origDialogStretch;
static IMP g_origDialogCell;
static IMP g_origDialogInit, g_origDialogFontProxy;
static IMP g_origDialogAssetTypeface, g_origDialogFileTypeface, g_origDialogNamedTypeface;

/* Trace the native hosting handoff without altering arguments, return values,
 * ownership, or game mode. All BHServer creations are logged, including later
 * single-player creations that could replace a newly created LAN server.
 */
static IMP g_origServerInit, g_origGameHosting;
static IMP g_origServerCleanup, g_origServerDisconnect, g_origServerDealloc;
static IMP g_origServerPublish, g_origServerPublishError;
static void (*g_origJavaBackground)(JNIEnv *, jclass);

/* Shared feature state; thread-local fields describe only the current call chain. */

static void *cfNetworkHandle;
static char deviceHostname[DEVICE_LABEL_SIZE];
static int deviceHostnameReady, deviceHostnameHooksInstalled;
static pthread_mutex_t deviceHostnameLock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int inMDNSDaemon;
static _Thread_local unsigned daemonHostnameCalls;
static int endpointFixEnabled, runResolverProbe;
static int foundationDomainBridge;
static void *foundationMainQueue;
static _Thread_local id initializingBrowser;
static LANEndpoint endpoints[LAN_ENDPOINT_COUNT];
static size_t nextEndpoint;
static pthread_mutex_t endpointLock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local LANEndpoint activeEndpoint;
static _Thread_local struct hostent fallbackHost;
static _Thread_local struct in_addr fallbackAddress;
static _Thread_local char fallbackName[LAN_HOSTNAME_SIZE];
static _Thread_local char *fallbackAliases[1], *fallbackAddresses[2];
static _Thread_local id activeResolvedService;
static _Thread_local int32_t activeResolvedRawPort, activeResolvedPort;
static _Thread_local char activeConnectHostname[LAN_HOSTNAME_SIZE];
static _Thread_local int resolvingLocalDNS;
static int localDNSFixEnabled;

/* Scope resolver logging to the connection thread. Calls on other threads
 * still pass through unchanged and do not inspect the returned hostent.
 */
static _Thread_local int resolverTracePhase; /* 1: probe, 2: game call */
static _Thread_local WelcomeWorldContext *welcomeWorldContext;
static IMP nativeChatHaxGetter, nativePickerGetter, nativePickerSetter;
static char welcomeUpdateDTType, welcomeUpdateAccurateType;
static WelcomePendingUpdate welcomePendingUpdate;
static uint32_t welcomeUpdateSerial;
static pthread_mutex_t welcomeUpdateLock = PTHREAD_MUTEX_INITIALIZER;
static __thread unsigned int tableDialogFontScope;
static __thread uint32_t dialogTypefaceEvents;
static uint32_t dialogCellTraceCount;
static __thread unsigned int tableDialogImageScope;

static int hostingSnapshotsInstalled;
static JavaVM *lifecycleVM;
static JavaVM *hostnameVM;
static jclass hostnameActivityClass;
static jmethodID hostnameActivityGetter;
static int hostnameJNIReady;
static jclass lifecycleClass;
static jfieldID lifecycleFields[6];
static int lifecycleProbeReady;
static pthread_mutex_t lifecycleRequestLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long lifecycleRequestSequence;
static struct timespec lifecycleLastRequest;
static IMP welcomeMainThreadIMP, welcomeCurrentThreadIMP, welcomeScheduleIMP;
static IMP welcomeDataFactoryIMP, welcomeArrayFactoryIMP;

/* Forward declarations for helpers used before their definitions. */

static int localHostname(const char *name);
static int installDialogTypefaceTrace(void);

/* LAN helpers and replacements, organized by feature. */

/* Map each replacement to its measured encoding from the constants above.
 * Only didNotSearch:/netServiceDidPublish: still use NULL expectations: their
 * existing implementations were not observed in the supplied device log.
 * Structural ABI checks remain required even for those two callbacks. Added
 * callbacks use declared encodings, not measurements of existing methods.
 */
static const char *expectedHookEncoding(const char *selector) {
    if (strEq(selector, "_proxy")) return ENC_FONT_PROXY;
    if (strEq(selector, "update:accurateDT:")) return ENC_GAME_UPDATE;
    if (strEq(selector, "viewServerWelcomeMessage:customRules:allowEdit:")) return ENC_WELCOME_DISPLAY;
    if (strEq(selector, "initWithWindowInfo:cache:delegate:saveID:name:client:server:multiplayerWorldData:serverHostData:saveDelay:worldWidthMacro:customRules:expertMode:"))
        return ENC_WORLD_INIT;
    if (strEq(selector, "optionsButton:")) return ENC_OPTIONS;
    if (strEq(selector, "windowInfoChanged:")) return ENC_WINDOW_CHANGED;
    if (strEq(selector, "backButton:")) return ENC_BACK;
    if (strEq(selector, "hostMultiButton:")) return ENC_HOST_ACTION;
    if (strEq(selector, "advancedButton:")) return ENC_ADVANCED;
    if (strEq(selector, "advancedLocalNetworkButton:")) return ENC_LOCAL_JOIN_ACTION;
    if (strEq(selector, "startServer")) return ENC_START_SERVER;
    if (strEq(selector, "startSearchForServers")) return ENC_SEARCH_SERVERS;
    if (strEq(selector, "searchForBrowsableDomains")) return ENC_DOMAIN_SEARCH;
    if (strEq(selector, "searchForServicesOfType:inDomain:")) return ENC_SERVICE_SEARCH;
    if (strEq(selector, "netServiceBrowser:didFindDomain:moreComing:")) return ENC_FOUND_DOMAIN;
    if (strEq(selector, "netServiceBrowser:didFindService:moreComing:")) return ENC_FOUND_SERVICE;
    if (strEq(selector, "netServiceDidResolveAddress:")) return ENC_RESOLVED_SERVICE;
    if (strEq(selector, "netServiceBrowser:didNotSearch:")) return ENC_SEARCH_ERROR;
    if (strEq(selector, "netService:didNotResolve:")) return ENC_RESOLVE_ERROR;
    if (strEq(selector, "worldSelectedAtIndex:")) return ENC_WORLD_SELECTION;
    if (strEq(selector, "connectToSpecificHostAndPort")) return ENC_CLIENT_CONNECT;
    if (strEq(selector, "multipleWorldsFoundForClientMatch:options:")) return ENC_WORLD_OPTIONS;
    if (strEq(selector, "port")) return ENC_SERVICE_PORT;
    if (strEq(selector, "initialize")) return ENC_INITIALIZE;
    if (strEq(selector, "showNow")) return ENC_SHOW_NOW;
    if (strEq(selector, "stretchableImageWithLeftCapWidth:topCapHeight:")) return ENC_STRETCH_IMAGE;
    if (strEq(selector, "tableView:cellForRowAtIndexPath:")) return ENC_DIALOG_CELL;
    if (strEq(selector, "initWithTitle:message:options:")) return ENC_DIALOG_INIT;
    if (strEq(selector, "loadFromAssetManager:path:")) return ENC_ASSET_TYPEFACE;
    if (strEq(selector, "typeface:")) return ENC_FILE_TYPEFACE;
    if (strEq(selector, "typefaceNamed:style:")) return ENC_NAMED_TYPEFACE;
    if (strEq(selector, "cleanup")) return ENC_SERVER_CLEANUP;
    if (strEq(selector, "disconnect")) return ENC_SERVER_DISCONNECT;
    if (strEq(selector, "dealloc")) return ENC_SERVER_DEALLOC;
    if (strEq(selector, "didEnterBackground")) return ENC_GAME_BACKGROUND;
    if (strEq(selector, "netServiceDidPublish:")) return ENC_SERVER_PUBLISH;
    if (strEq(selector, "netService:didNotPublish:")) return ENC_SERVER_PUBLISH_ERROR;
    if (strEq(selector, "startHostingNetworkGameWithUserName:userPhoto:")) return ENC_GAME_HOSTING;
    if (strEq(selector, "initWithDelegate:match:netNodeType:saveID:maxPlayers:")) return ENC_SERVER_INIT;
    if (strEq(selector, "init")) return ENC_BROWSER_INIT;
    if (strEq(selector, "_dispatchCallBack:flags:error:")) return ENC_BROWSER_DISPATCH;
    return NULL;
}

/* JNI supplies a normalized Android device name once. This second gate accepts
 * only one DNS label (no suffix or dot); the daemon appends .local and performs
 * its own conflict probing. The mutex serializes first publication, and the
 * release/acquire flag makes the completed buffer visible to the daemon.
 */
static int canonicalDeviceLabel(const char *ascii, char output[DEVICE_LABEL_SIZE]) {
    size_t length = ascii ? strnlen(ascii, DEVICE_LABEL_SIZE) : 0, i;
    if (!length || length >= DEVICE_LABEL_SIZE || ascii[0] == '-' || ascii[length - 1] == '-') return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)ascii[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') return 0;
        output[i] = (char)c;
    }
    output[length] = 0;
    return 1;
}
static int publishDeviceHostname(const char *ascii) {
    char label[DEVICE_LABEL_SIZE];
    if (!canonicalDeviceLabel(ascii, label)) return 0;
    pthread_mutex_lock(&deviceHostnameLock);
    if (!__atomic_load_n(&deviceHostnameReady, __ATOMIC_ACQUIRE)) {
        memcpy(deviceHostname, label, strlen(label) + 1);
        __atomic_store_n(&deviceHostnameReady, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&deviceHostnameLock);
    return 1;
}
/* Restrict gethostname substitution to the daemon's call chain. Other callers
 * retain libc behavior. Preserve errno on success and report truncation using
 * the original gethostname contract. The entry wrapper restores prior TLS
 * values even if the native daemon returns.
 */
static int my_daemonGetHostname(char *buffer, size_t size) {
    size_t length, copied;
    int savedErrno = errno;
    if (!inMDNSDaemon || !__atomic_load_n(&deviceHostnameHooksInstalled, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&deviceHostnameReady, __ATOMIC_ACQUIRE) || !buffer)
        return g_origGetHostname(buffer, size);
    length = strlen(deviceHostname);
    copied = length + 1 < size ? length + 1 : size;
    if (copied) memcpy(buffer, deviceHostname, copied);
    if (!daemonHostnameCalls++)
        LOGI("mDNS: supplied Android device hostname base=%s.local.; native collision handling retained", deviceHostname);
    if (size <= length) { errno = ENAMETOOLONG; return -1; }
    errno = savedErrno;
    return 0;
}
static void *my_daemonMain(void *argument) {
    int previous = inMDNSDaemon;
    unsigned previousCalls = daemonHostnameCalls;
    void *result;
    if (!__atomic_load_n(&deviceHostnameReady, __ATOMIC_ACQUIRE) && prepareDeviceHostname)
        prepareDeviceHostname();
    inMDNSDaemon = 1; daemonHostnameCalls = 0;
    LOGV("mDNS: daemon device-name input ready=%d hooks=%d",
         __atomic_load_n(&deviceHostnameReady, __ATOMIC_ACQUIRE),
         __atomic_load_n(&deviceHostnameHooksInstalled, __ATOMIC_ACQUIRE));
    result = g_origDaemonMain(argument);
    {
        int returnedErrno = errno;
        LOGE("mDNS: native daemon exited status=%d errno=%d; startup is once per process",
             (int32_t)(intptr_t)result, returnedErrno);
        errno = returnedErrno;
    }
    inMDNSDaemon = previous; daemonHostnameCalls = previousCalls;
    return result;
}

/* LOGV arguments disappear in quiet builds. Explicit void uses below keep
 * log-only locals/parameters warning-clean without changing shared macros
 * or moving any repair, callback forwarding or installation into LOGV.
 */
static int64_t dnsDiagnosticTime(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void dnsDiagnosticResult(const char *api, void **ref, int32_t result, int64_t started) {
    int returnedErrno = errno;
    int64_t ended = dnsDiagnosticTime();
    /* On failure the out-ref may be uninitialized; never inspect it. */
    if (result)
        LOGE("DNS-SD native: %s returned error=%d errno=%d elapsedMs=%lld ref=%p",
             api, result, returnedErrno, (long long)(started < 0 || ended < 0 ? -1 : ended - started),
             (void *)NULL);
    else
        LOGV("DNS-SD native: %s returned error=%d errno=%d elapsedMs=%lld ref=%p",
             api, result, returnedErrno, (long long)(started < 0 || ended < 0 ? -1 : ended - started),
             ref ? *ref : NULL);
    (void)ref;
    errno = returnedErrno;
}
static int32_t my_dnsEnumerate(void **ref, uint32_t flags, uint32_t index, DNSDomainReply callback, void *context) {
    int savedErrno = errno;
    int64_t started = dnsDiagnosticTime();
    int32_t result;
    LOGV("DNS-SD native: EnumerateDomains entered flags=0x%x interface=%u", flags, index);
    errno = savedErrno;
    result = g_origDNSEnumerate(ref, flags, index, callback, context);
    dnsDiagnosticResult("EnumerateDomains", ref, result, started);
    return result;
}
static int32_t my_dnsBrowse(void **ref, uint32_t flags, uint32_t index, const char *type, const char *domain,
                              DNSBrowseReply callback, void *context) {
    int savedErrno = errno;
    int64_t started = dnsDiagnosticTime();
    int32_t result;
    LOGV("DNS-SD native: Browse entered flags=0x%x interface=%u type=%.200s domain=%.200s",
         flags, index, type ? type : "(null)", domain ? domain : "(default)");
    errno = savedErrno;
    result = g_origDNSBrowse(ref, flags, index, type, domain, callback, context);
    dnsDiagnosticResult("Browse", ref, result, started);
    return result;
}
static int32_t my_dnsResolve(void **ref, uint32_t flags, uint32_t index, const char *name, const char *type,
                               const char *domain, DNSResolveReply callback, void *context) {
    int savedErrno = errno;
    int64_t started = dnsDiagnosticTime();
    int32_t result;
    LOGV("DNS-SD native: Resolve entered flags=0x%x interface=%u name=%.200s type=%.200s domain=%.200s",
         flags, index, name ? name : "(null)", type ? type : "(null)", domain ? domain : "(default)");
    errno = savedErrno;
    result = g_origDNSResolve(ref, flags, index, name, type, domain, callback, context);
    dnsDiagnosticResult("Resolve", ref, result, started);
    return result;
}
static int32_t my_dnsRegister(void **ref, uint32_t flags, uint32_t index, const char *name, const char *type,
                                const char *domain, const char *host, uint16_t port, uint16_t txtLength,
                                const void *txt, DNSRegisterReply callback, void *context) {
    int savedErrno = errno;
    int64_t started = dnsDiagnosticTime();
    int32_t result;
    LOGV("DNS-SD native: Register entered flags=0x%x interface=%u name=%.200s type=%.200s domain=%.200s host=%.200s port=%u txtLength=%u",
         flags, index, name ? name : "(default)", type ? type : "(null)", domain ? domain : "(default)",
         host ? host : "(default)", (unsigned)ntohs(port), (unsigned)txtLength);
    errno = savedErrno;
    result = g_origDNSRegister(ref, flags, index, name, type, domain, host, port, txtLength, txt, callback, context);
    dnsDiagnosticResult("Register", ref, result, started);
    return result;
}
static int sameDNSName(const char *a, const char *b) {
    size_t na = a ? strnlen(a, LAN_HOSTNAME_SIZE) : 0;
    size_t nb = b ? strnlen(b, LAN_HOSTNAME_SIZE) : 0;
    if (!na || !nb || na == LAN_HOSTNAME_SIZE || nb == LAN_HOSTNAME_SIZE) return 0;
    if (a[na - 1] == '.') --na;
    if (b[nb - 1] == '.') --nb;
    return na == nb && !strncasecmp(a, b, na);
}
static void localDNSReply(void *ref, uint32_t flags, uint32_t interfaceIndex,
                          int32_t error, const char *name, uint16_t type, uint16_t rrclass,
                          uint16_t length, const void *bytes, uint32_t ttl, void *context) {
    LocalDNSQuery *query = context;
    struct in_addr address;
    (void)ref; (void)interfaceIndex;
    /* Error replies need not supply valid record arguments. Never inspect them. */
    if (error) { query->error = error; return; }
    if (!(flags & 2) || !ttl || type != 1 || rrclass != 1 || length != 4 || !bytes ||
        !sameDNSName(name, query->hostname)) return;
    memcpy(&address, bytes, sizeof(address));
    if (address.s_addr == INADDR_ANY || address.s_addr == INADDR_BROADCAST) return;
    if (!query->found) { query->address = address; query->found = 1; }
}
static int64_t localDNSMilliseconds(void) {
    struct timespec now;
    if (dnsClock(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
/* Saved entries do not have a current NSNetService address list. Query only an
 * IPv4 A record for the requested .local name, poll the DNS-SD descriptor until
 * the bounded deadline, then deallocate this request. This is a fallback for
 * one connection attempt, rather than a replacement DNS browser or cache.
 */
static int resolveLocalDNS(const char *hostname, struct in_addr *address) {
    LocalDNSQuery query = {.hostname = hostname};
    void *ref = NULL;
    struct pollfd descriptor;
    int success = 0, ready, fd;
    int32_t error;
    int64_t deadline, now;
    if (!localDNSFixEnabled || resolvingLocalDNS || !localHostname(hostname) ||
        !ensureMDNS || !dnsQueryRecord || !dnsSocketFD || !dnsProcessResult || !dnsDeallocate) return 0;
    resolvingLocalDNS = 1;
    ensureMDNS();
    error = dnsQueryRecord(&ref, 0, 0, hostname, 1, 1, localDNSReply, &query);
    if (error || !ref) {
        LOGE("join: native DNS-SD query startup failed host=%s error=%d", hostname, (int)error);
        goto done;
    }
    fd = dnsSocketFD(ref);
    now = localDNSMilliseconds();
    if (fd < 0 || now < 0) goto done;
    deadline = now + LOCAL_DNS_WAIT_MS;
    descriptor = (struct pollfd){.fd = fd, .events = POLLIN};
    while (!query.found && !query.error) {
        now = localDNSMilliseconds();
        if (now < 0 || now >= deadline) break;
        descriptor.revents = 0;
        ready = dnsPoll(&descriptor, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(descriptor.revents & POLLIN)) break;
        /* DNS-SD owns the socket. ProcessResult invokes our callback in this
         * thread; copy the address before it releases its reply buffer.
         */
        error = dnsProcessResult(ref);
        if (error) { query.error = error; break; }
    }
    if (query.found && !query.error) { *address = query.address; success = 1; }
    LOGV("join: native DNS-SD IPv4 lookup host=%s success=%d error=%d",
         hostname, success, (int)query.error);
done:
    /* Cancel only after ProcessResult returns, on the same owning thread. */
    if (ref) dnsDeallocate(ref);
    resolvingLocalDNS = 0;
    return success;
}

/* Endpoint cache entries copy hostname/address/host-order port and use the
 * match pointer as an identity key. The lock protects cache insertion/lookup;
 * activeEndpoint is a per-thread copy used only while forwarding a connect.
 * Starting a new search discards entries for that match, preventing stale
 * browse results from becoming the next connection's endpoint.
 */
static void discardEndpoints(id match) {
    size_t i;
    pthread_mutex_lock(&endpointLock);
    for (i = 0; i < LAN_ENDPOINT_COUNT; ++i)
        if (endpoints[i].match == match) endpoints[i].valid = 0;
    pthread_mutex_unlock(&endpointLock);
}
static void rememberEndpoint(const LANEndpoint *endpoint) {
    size_t i, slot = LAN_ENDPOINT_COUNT;
    pthread_mutex_lock(&endpointLock);
    for (i = 0; i < LAN_ENDPOINT_COUNT; ++i) {
        if (endpoints[i].valid && endpoints[i].match == endpoint->match &&
            endpoints[i].port == endpoint->port && !strcmp(endpoints[i].hostname, endpoint->hostname)) {
            slot = i; break;
        }
    }
    if (slot == LAN_ENDPOINT_COUNT) slot = nextEndpoint++ % LAN_ENDPOINT_COUNT;
    endpoints[slot] = *endpoint;
    pthread_mutex_unlock(&endpointLock);
}
static LANEndpoint selectedEndpoint(id match, const char *hostname, uint16_t port) {
    LANEndpoint result = {0};
    size_t i;
    if (!hostname) return result;
    pthread_mutex_lock(&endpointLock);
    for (i = 0; i < LAN_ENDPOINT_COUNT; ++i) {
        if (endpoints[i].valid && endpoints[i].match == match && endpoints[i].port == port &&
            !strcmp(endpoints[i].hostname, hostname)) { result = endpoints[i]; break; }
    }
    pthread_mutex_unlock(&endpointLock);
    return result;
}

/* Try the system resolver first. A matching active endpoint can supply the
 * address learned from DNS-SD; a saved .local entry can trigger a native A query.
 * The synthetic hostent and all referenced buffers live in TLS, so concurrent
 * connection attempts do not overwrite one another's fallback result.
 */
static struct hostent *my_getHostByName2(const char *name, int family) {
    struct hostent *result;
    int savedErrno, savedHostError = 0, *errorLocation;
    char address[INET_ADDRSTRLEN];
    const char *ipv4 = "(none)";
    if (!resolverTracePhase) return g_origGetHostByName2(name, family);
    savedErrno = errno;
    errorLocation = nativeHostError ? nativeHostError() : NULL;
    errno = savedErrno;
    result = g_origGetHostByName2(name, family);
    savedErrno = errno;
    if (errorLocation) savedHostError = *errorLocation;
    /* Use the selected service address, or query the exact saved .local host
     * inside the original connection call. Preserve successful Bionic lookups
     * and all unrelated resolver requests.
     * Thread-local storage follows the hostent lifetime convention: it remains
     * valid after the connection hook returns, until another fallback lookup.
     */
    if (!result && resolverTracePhase == 2 && family == AF_INET && name && !resolvingLocalDNS) {
        int supplied = 0;
        if (endpointFixEnabled && activeEndpoint.valid && !strcmp(name, activeEndpoint.hostname)) {
            fallbackAddress = activeEndpoint.address;
            supplied = 1;
            LOGV("join: Bionic lookup failed h_errno=%d; supplying selected DNS-SD IPv4 for host=%s port=%u",
                 savedHostError, name, (unsigned)activeEndpoint.port);
        } else if (localDNSFixEnabled && activeConnectHostname[0] &&
                   !strcmp(name, activeConnectHostname)) {
            supplied = resolveLocalDNS(name, &fallbackAddress);
        }
        if (supplied) {
            memcpy(fallbackName, name, strlen(name) + 1);
            fallbackAliases[0] = NULL;
            fallbackAddresses[0] = (char *)&fallbackAddress; fallbackAddresses[1] = NULL;
            fallbackHost = (struct hostent){.h_name = fallbackName, .h_aliases = fallbackAliases,
                .h_addrtype = AF_INET, .h_length = 4, .h_addr_list = fallbackAddresses};
            result = &fallbackHost;
            savedHostError = 0;
        }
    }
    if (result && result->h_addrtype == AF_INET && result->h_length == 4 &&
        result->h_addr_list && result->h_addr_list[0] &&
        inet_ntop(AF_INET, result->h_addr_list[0], address, sizeof(address))) ipv4 = address;
    LOGV("join: gethostbyname2 phase=%s host=%s family=%d result=%p IPv4=%s errno=%d h_errno=%d errorAvailable=%d",
         resolverTracePhase == 1 ? "probe" : "game", name ? name : "(null)",
         family, (void *)result, ipv4, savedErrno, savedHostError, errorLocation != NULL);
    if (errorLocation) *errorLocation = savedHostError;
    (void)ipv4;
    errno = savedErrno;
    return result;
}

/* Inspect only validated ranges in the loaded image. No IDA addresses are
 * embedded: find libSystem's ARM JUMP_SLOT by dynamic symbol name.
 */
static int imageRange(const struct dl_phdr_info *info, uintptr_t address, size_t size) {
    size_t i;
    for (i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        uintptr_t start = info->dlpi_addr + p->p_vaddr;
        if (p->p_type == PT_LOAD && (p->p_flags & PF_R) && address >= start &&
            address - start <= p->p_memsz && size <= p->p_memsz - (address - start)) return 1;
    }
    return 0;
}
static uintptr_t imagePointer(const struct dl_phdr_info *info, uintptr_t value, size_t size) {
    if (value <= UINTPTR_MAX - info->dlpi_addr &&
        imageRange(info, info->dlpi_addr + value, size)) return info->dlpi_addr + value;
    return imageRange(info, value, size) ? value : 0;
}
static void **findImportRelocations(const struct dl_phdr_info *info, const ElfW(Sym) *symbols,
                                   const char *strings, size_t stringSize, uintptr_t relocations,
                                   size_t relocationSize, unsigned wantedType, const char *symbolName) {
    const ElfW(Rel) *rel;
    size_t i;
    if (!relocations || !relocationSize || relocationSize % sizeof(*rel)) return NULL;
    rel = (const ElfW(Rel) *)imagePointer(info, relocations, relocationSize);
    if (!rel) return NULL;
    for (i = 0; i < relocationSize / sizeof(*rel); ++i) {
        size_t index;
        unsigned type;
        uintptr_t symbolAddress, slot;
        const ElfW(Sym) *entry;
        /* Host mocks use native-width synthetic ELF records. The generated
         * ARM32 production copy uses ELF32 decoding without this branch. */
        index = ELF32_R_SYM(rel[i].r_info); type = ELF32_R_TYPE(rel[i].r_info);
        if (type != wantedType || index > (UINTPTR_MAX - (uintptr_t)symbols) / sizeof(*symbols)) continue;
        symbolAddress = (uintptr_t)symbols + index * sizeof(*symbols);
        if (!imageRange(info, symbolAddress, sizeof(*symbols))) continue;
        entry = (const ElfW(Sym) *)symbolAddress;
        if (entry->st_name >= stringSize ||
            !memchr(strings + entry->st_name, 0, stringSize - entry->st_name) ||
            strcmp(strings + entry->st_name, symbolName)) continue;
        if (rel[i].r_offset > UINTPTR_MAX - info->dlpi_addr) return NULL;
        slot = info->dlpi_addr + rel[i].r_offset;
        return slot % sizeof(void *) == 0 && imageRange(info, slot, sizeof(void *)) ? (void **)slot : NULL;
    }
    return NULL;
}
static void **findNamedImportKind(const struct dl_phdr_info *info, const char *symbolName, int allowData) {
    const ElfW(Dyn) *dynamic = NULL;
    const ElfW(Sym) *symbols;
    const char *strings;
    uintptr_t sym = 0, str = 0, jumps = 0, data = 0;
    size_t dynamicCount = 0, stringSize = 0, jumpSize = 0, symbolSize = 0, dataSize = 0, dataEntrySize = 0, i;
    int relocationKind = 0, terminated = 0;
    for (i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_DYNAMIC) {
            dynamic = (const ElfW(Dyn) *)(info->dlpi_addr + p->p_vaddr);
            dynamicCount = p->p_memsz / sizeof(*dynamic);
            if (!imageRange(info, (uintptr_t)dynamic, p->p_memsz)) return NULL;
            break;
        }
    }
    if (!dynamic || dynamicCount > 65536) return NULL;
    for (i = 0; i < dynamicCount; ++i) {
        switch (dynamic[i].d_tag) {
            case DT_NULL: terminated = 1; break;
            case DT_SYMTAB: sym = dynamic[i].d_un.d_ptr; break;
            case DT_STRTAB: str = dynamic[i].d_un.d_ptr; break;
            case DT_STRSZ: stringSize = dynamic[i].d_un.d_val; break;
            case DT_SYMENT: symbolSize = dynamic[i].d_un.d_val; break;
            case DT_JMPREL: jumps = dynamic[i].d_un.d_ptr; break;
            case DT_PLTRELSZ: jumpSize = dynamic[i].d_un.d_val; break;
            case DT_PLTREL: relocationKind = dynamic[i].d_un.d_val; break;
            case DT_REL: data = dynamic[i].d_un.d_ptr; break;
            case DT_RELSZ: dataSize = dynamic[i].d_un.d_val; break;
            case DT_RELENT: dataEntrySize = dynamic[i].d_un.d_val; break;
        }
        if (terminated) break;
    }
    if (!terminated || !sym || !str || !stringSize || symbolSize != sizeof(ElfW(Sym))) return NULL;
    symbols = (const ElfW(Sym) *)imagePointer(info, sym, sizeof(*symbols));
    strings = (const char *)imagePointer(info, str, stringSize);
    if (!symbols || !strings) return NULL;
    /* The CFNetwork pthread entry is taken as a function pointer, which can
     * use GLOB_DAT instead of a PLT call. This extra table is opt-in; existing
     * resolver/browser hooks continue to inspect JUMP_SLOT only. Prefer the
     * address relocation when both forms exist for the same function.
     */
    if (allowData && dataEntrySize == sizeof(ElfW(Rel))) {
        void **slot = findImportRelocations(info, symbols, strings, stringSize, data, dataSize,
                                           R_ARM_GLOB_DAT, symbolName);
        if (slot) return slot;
    }
    if (relocationKind == DT_REL)
        return findImportRelocations(info, symbols, strings, stringSize, jumps, jumpSize,
                                     R_ARM_JUMP_SLOT, symbolName);
    return NULL;
}
static void **findNamedImport(const struct dl_phdr_info *info, const char *symbolName) {
    return findNamedImportKind(info, symbolName, 0);
}
static void **findResolverImport(const struct dl_phdr_info *info) {
    return findNamedImport(info, "gethostbyname2");
}


/* ELF imports are patched in the consumer library, not in every user of the
 * provider. Verify image ranges and the expected provider before exchanging a
 * relocation slot; retain its prior target for forwarding. These C entry points
 * have explicit prototypes and do not use Objective-C encoding metadata.
 */
static int namedImportImage(struct dl_phdr_info *info, size_t size, void *data) {
    ImportRequest *request = data;
    const char *name = strrchr(info->dlpi_name, '/');
    void **slot;
    Dl_info provider;
    long pageSize;
    uintptr_t page;
    void *expected;
    int protection = 0;
    size_t i;
    (void)size;
    name = name ? name + 1 : info->dlpi_name;
    if (strcmp(name, request->library)) return 0;
    slot = request->allowData ? findNamedImportKind(info, request->symbol, 1) :
        (!strcmp(request->symbol, "gethostbyname2") ? findResolverImport(info) : findNamedImport(info, request->symbol));
    if (!slot || !*slot) { LOGE("import hook unavailable: library=%s symbol=%s", name, request->symbol); return 1; }
    if (*slot == request->replacement) { request->installed = 1; return 1; }
    memset(&provider, 0, sizeof(provider));
    if (!dladdr(*slot, &provider) || provider.dli_fbase == (void *)info->dlpi_addr) {
        LOGE("import hook %s: unresolved or internal import; leaving slot unchanged", request->symbol); return 1;
    }
    if (request->expectedTarget && *slot != request->expectedTarget) {
        LOGE("import hook %s: unexpected provider target; leaving slot unchanged", request->symbol); return 1;
    }
    expected = *slot;
    memcpy(request->g_origStorage, &expected, sizeof(expected));
    request->provider = provider;
    pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || ((uintptr_t)pageSize & ((uintptr_t)pageSize - 1))) return 1;
    page = (uintptr_t)slot & ~((uintptr_t)pageSize - 1);
    for (i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        uintptr_t start = info->dlpi_addr + p->p_vaddr;
        if (p->p_type == PT_LOAD && (uintptr_t)slot >= start && (uintptr_t)slot - start < p->p_memsz)
            protection = ((p->p_flags & PF_R) ? PROT_READ : 0) |
                         ((p->p_flags & PF_W) ? PROT_WRITE : 0) |
                         ((p->p_flags & PF_X) ? PROT_EXEC : 0);
    }
    /* Android rounds the GNU_RELRO range outward to page boundaries. */
    for (i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        uintptr_t start = (info->dlpi_addr + p->p_vaddr) & ~((uintptr_t)pageSize - 1);
        uintptr_t end = (info->dlpi_addr + p->p_vaddr + p->p_memsz + (uintptr_t)pageSize - 1) &
                        ~((uintptr_t)pageSize - 1);
        if (p->p_type == PT_GNU_RELRO && page >= start && page < end) protection = PROT_READ;
    }
    if (!protection || mprotect((void *)page, (size_t)pageSize, protection | PROT_WRITE)) {
        LOGE("import hook %s: cannot make GOT page writable errno=%d", request->symbol, errno); return 1;
    }
    if (!__atomic_compare_exchange_n(slot, &expected, request->replacement,
                                     0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        if (mprotect((void *)page, (size_t)pageSize, protection))
            LOGE("resolver import diagnostic: GOT protection restoration failed errno=%d", errno);
        LOGE("import hook %s: import changed during installation; leaving it unchanged", request->symbol);
        return 1;
    }
    if (mprotect((void *)page, (size_t)pageSize, protection))
        LOGE("resolver import diagnostic: GOT protection restoration failed errno=%d", errno);
    request->installed = 1;
    LOGV("import hook installed: library=%s symbol=%s provider=%s target=%s", name, request->symbol,
         provider.dli_fname ? provider.dli_fname : "(unknown)",
         provider.dli_sname ? provider.dli_sname : "(unknown)");
    return 1;
}

static id readObject(id self, uint32_t offset) {
    return *(id *)((char *)self + offset);
}
static int state(id self) { return *(int *)((char *)self + offState); }
static id send0(id self, const char *selector) {
    return MSG_id(self, selReg(selector));
}

/* Parse one Objective-C encoded type, including nested structures and quoted
 * class/field names. Skip stack offsets separately. R means a float CGRect,
 * p means an object or ordinary pointer, and other expectations are literal.
 */
static const char *skipQuoted(const char *p) {
    if (*p != '"') return p;
    for (++p; *p && *p != '"'; ++p) {
        if (*p == '\\' && p[1]) ++p;
    }
    return *p ? p + 1 : NULL;
}
static const char *skipType(const char *p) {
    char end;
    if (!p) return NULL;
    while (*p && strchr("rnNoORV", *p)) ++p;
    if (!*p) return NULL;
    if (*p == '^') return skipType(p + 1);
    if (*p == '@') {
        ++p;
        if (*p == '?') return p + 1;
        return *p == '"' ? skipQuoted(p) : p;
    }
    if (*p == '{' || *p == '(' || *p == '[') {
        end = *p == '{' ? '}' : *p == '(' ? ')' : ']';
        for (++p; *p && *p != end;) {
            if (*p == '"') p = skipQuoted(p);
            else if (*p == '{' || *p == '(' || *p == '[') p = skipType(p);
            else ++p;
            if (!p) return NULL;
        }
        return *p == end ? p + 1 : NULL;
    }
    if (*p == 'b') {
        for (++p; *p >= '0' && *p <= '9'; ++p) {}
        return p;
    }
    return p + 1;
}
static const char *skipOffset(const char *p) {
    if (!p) return NULL;
    if (*p == '+' || *p == '-') ++p;
    while (*p >= '0' && *p <= '9') ++p;
    return p;
}
static int rectFloats(const char **cursor) {
    const char *p = *cursor;
    int count = 0;
    if (*p != '{') return -1;
    for (++p; *p && *p != '=' && *p != '}'; ++p) {}
    if (*p++ != '=') return -1;
    while (*p && *p != '}') {
        if (*p == '"') {
            p = skipQuoted(p);
            if (!p) return -1;
        } else if (*p == '{') {
            int nested = rectFloats(&p);
            if (nested < 0) return -1;
            count += nested;
        } else if (*p == 'f') {
            ++count; ++p;
        } else return -1;
    }
    if (*p != '}') return -1;
    *cursor = p + 1;
    return count;
}
static int matchesType(const char *p, char expected) {
    while (*p && strchr("rnNoORV", *p)) ++p;
    if (expected == 'R') return rectFloats(&p) == 4;
    if (expected == 'Z') return rectFloats(&p) == 2; /* CGSize, not unsigned short 'S'. */
    if (expected == 'p') return *p == '@' || *p == '^';
    return *p == expected && !(expected == '@' && p[1] == '?');
}
static int signature(Method m, const char *expected) {
    const char *enc = m ? getTypeEnc(m) : NULL;
    const char *p = enc;
    if (!enc) return 0;
    for (; *expected; ++expected) {
        if (!p || !matchesType(p, *expected)) return 0;
        p = skipOffset(skipType(p));
    }
    return p && !*p;
}
static Method checkedMethod(Class cls, const char *selector, const char *sig) {
    Method m = getInstMethod(cls, selReg(selector));
    if (!signature(m, sig)) {
        LOGE("unsupported/missing %s: %s", selector,
             m && getTypeEnc(m) ? getTypeEnc(m) : "(missing)");
        return NULL;
    }
    return m;
}

static int resolveOffset(Class cls, const char *className, const char *name,
                         char type, uint32_t *out) {
    Ivar iv = getIvar(cls, name);
    ptrdiff_t offset;
    const char *enc;
    if (!iv) { LOGE("missing ivar %s.%s", className, name); return 0; }
    enc = ivarEncoding(iv);
    offset = ivarOff(iv);
    if (!enc || !matchesType(enc, type) || offset <= 0 || offset >= 0x10000) {
        LOGE("unsupported ivar %s.%s", className, name); return 0;
    }
    *out = (uint32_t)offset;
    return 1;
}

static int usableRect(Rect r) {
    /* These bounds reject NaNs, infinities, and implausible layouts. */
    return r.x > -10000 && r.x < 10000 && r.y > -10000 && r.y < 10000 &&
           r.w >= 200 && r.w < 2000 && r.h >= 20 && r.h < 200;
}
static float absFloat(float f) { return f < 0 ? -f : f; }

/* The exported Android optionsButton: positions Back at y=52 for screenHeight
 * >=415 and y=-6 otherwise. Use that actual frame as the layout anchor, rather
 * than guessing the WindowInfo structure or embedding iOS CGRect doubles.
 * Android's x/width are -120/224; iOS 1.7.4 uses -113/226. All controls retain
 * the Android anchor's dimensions and center.
 */
static int layoutAnchor(id self, Rect *out, int *tall) {
    id back = readObject(self, offBack);
    Method getter;
    if (!back) return 0;
    getter = getInstMethod(objectClass(back), selReg("frame"));
    if (!signature(getter, "R@:")) return 0;
    *out = ((Rect (*)(id, SEL))getImp(getter))(back, selReg("frame"));
    if (!usableRect(*out)) return 0;
    if (absFloat(out->y - 52) < 0.01f) *tall = 1;
    else if (absFloat(out->y + 6) < 0.01f) *tall = 0;
    else return 0;
    return 1;
}
static int canSetFrame(id object) {
    return object && signature(getInstMethod(objectClass(object), selReg("setFrame:")), "v@:R");
}
static void setFrame(id object, Rect frame) {
    Method setter = getInstMethod(objectClass(object), selReg("setFrame:"));
    ((void (*)(id, SEL, Rect))getImp(setter))(object, selReg("setFrame:"), frame);
}
/* Reset our local-world adjustments before the game's method executes. Online
 * options must not inherit a right-column Delete frame from a local world.
 */
static void restoreNativeLayout(id self) {
    id del, migrate;
    Rect anchor, frame;
    int tall;
    if (!readObject(self, offHost) || !layoutAnchor(self, &anchor, &tall)) return;
    del = readObject(self, offDelete);
    migrate = readObject(self, offMigrate);
    if (!canSetFrame(del) || (migrate && !canSetFrame(migrate))) return;
    frame = anchor;
    frame.y -= 48;
    setFrame(del, frame);
    if (migrate) {
        frame = anchor;
        frame.y -= 144; /* Stock Android occupies the omitted host row. */
        setFrame(migrate, frame);
    }
}

static id string(const char *text) {
    return ((id (*)(id, SEL, const char *))g_msgSend)(getClass("NSString"),
               selReg("stringWithUTF8String:"), text);
}

static void restoreHostButton(id self) {
    Class cls;
    Method init, target, backgroundSetter, selectedSetter, textureMethod;
    id del, host, migrate, normal, selected, title, allocated, cache;
    void *window;
    Rect anchor, hostFrame, deleteFrame, migrateFrame;
    int tall;
    if (state(self) != 1) return; /* Local-world Options, not online worlds. */
    del = readObject(self, offDelete);
    if (!del) return;
    cls = objectClass(del);
    init = checkedMethod(cls, "initWithFrame:cache:windowInfo:title:", "@@:R@p@");
    target = checkedMethod(cls, "setTarget:action:", "v@:@:");
    backgroundSetter = checkedMethod(cls, "setBackgroundTexture:", "v@:@");
    selectedSetter = checkedMethod(cls, "setBackgroundSelectedTexture:", "v@:@");
    if (!canSetFrame(del) || !init || !target || !backgroundSetter || !selectedSetter) return;
    cache = readObject(self, offCache);
    window = readObject(self, offWindow);
    if (!cache || !window) return;
    if (!layoutAnchor(self, &anchor, &tall)) {
        LOGE("unrecognized Back frame; leaving UI alone"); return;
    }
    host = readObject(self, offHost);
    if (host && objectClass(host) != cls) { LOGE("existing host button has a different class"); return; }
    if (!host && readObject(self, offDisabled)) return; /* Match iOS's gate. */
    migrate = readObject(self, offMigrate);
    if (migrate && !canSetFrame(migrate)) return;
    hostFrame = deleteFrame = migrateFrame = anchor;
    hostFrame.y -= 144;
    deleteFrame.y -= 48;
    if (tall) migrateFrame.y -= 192;
    else {
        migrateFrame.y -= 48;
        if (migrate) {
            float center = anchor.x + anchor.w * 0.5f;
            migrateFrame.x = center - anchor.w - 4;
            deleteFrame.x = center + 4;
        }
    }
    if (!host) {
        textureMethod = checkedMethod(objectClass(cache), "textureNamed:", "@@:@");
        if (!textureMethod) return;
        normal = ((id (*)(id, SEL, id))getImp(textureMethod))(
            cache, selReg("textureNamed:"), string("InventoryButtonBackground.png"));
        selected = ((id (*)(id, SEL, id))getImp(textureMethod))(
            cache, selReg("textureNamed:"), string("InventoryButtonBackgroundSelected.png"));
        if (!normal || !selected) { LOGE("button textures missing; leaving UI alone"); return; }
        title = string("HOST MULTIPLAYER...");
        if (!title) return;
        allocated = send0(cls, "alloc");
        if (!allocated) return;
        /* A direct typed IMP lets the C compiler implement ARM32's aggregate
         * argument ABI. No varargs dispatch or guessed register assignments. */
        host = ((id (*)(id, SEL, Rect, id, void *, id))getImp(init))(
            allocated, selReg("initWithFrame:cache:windowInfo:title:"), hostFrame,
            cache, window, title);
        if (!host) return; /* init owns the failed allocation's cleanup. */
        ((void (*)(id, SEL, id))getImp(backgroundSetter))(
            host, selReg("setBackgroundTexture:"), normal);
        ((void (*)(id, SEL, id))getImp(selectedSetter))(
            host, selReg("setBackgroundSelectedTexture:"), selected);
        ((void (*)(id, SEL, id, SEL))getImp(target))(
            host, selReg("setTarget:action:"), self, selHostAction);
        /* init returns +1 ownership, matching the game's ivar/dealloc pattern. */
        *(id *)((char *)self + offHost) = host;
        LOGV("created HOST MULTIPLAYER... button; using retained hostMultiButton:");
    } else {
        setFrame(host, hostFrame);
    }
    setFrame(del, deleteFrame);
    if (migrate) setFrame(migrate, migrateFrame);
}

static void my_options(id self, SEL cmd, id sender) {
    restoreNativeLayout(self);
    ((void (*)(id, SEL, id))g_origOptions)(self, cmd, sender);
    restoreHostButton(self);
}
static void my_resize(id self, SEL cmd, void *window) {
    restoreNativeLayout(self);
    ((void (*)(id, SEL, void *))g_origResize)(self, cmd, window);
    restoreHostButton(self);
}
static void my_back(id self, SEL cmd, id sender) {
    restoreNativeLayout(self);
    ((void (*)(id, SEL, id))g_origBack)(self, cmd, sender);
    restoreHostButton(self);
}
static unsigned char my_start(id self, SEL cmd) {
    unsigned char result = ((unsigned char (*)(id, SEL))g_origStart)(self, cmd);
    LOGV("startServer result=%u ENet=%p TCP=%p configuredPort=%u match=%p", result,
         readObject(self, offEnet), readObject(self, offTcp),
         *(uint16_t *)((char *)self + offPort), self);
    return result;
}

/* Android retains advancedLocalNetworkButton:, finalJoinButton:'s state-3
 * branch, and this ivar's render/touch/dealloc paths. Only creation is omitted
 * from advancedButton:. Restore the iOS row arrangement with Android floats,
 * dimensions, and existing action. Discovery/selection remain game-owned.
 */
static void restoreLocalJoinButton(id self) {
    id cancel, online, search, local, cache, normal, selected, title, allocated;
    void *window;
    Class cls;
    Method getter, init, target, backgroundSetter, selectedSetter, textureMethod;
    Rect anchor, cancelFrame, localFrame, onlineFrame, searchFrame;
    int tall;
    if (*(int *)((char *)self + offJoinState) != 2) return;
    cancel = readObject(self, offJoinCancel);
    online = readObject(self, offJoinOnline);
    search = readObject(self, offJoinSearch);
    local = readObject(self, offJoinLocal);
    cache = readObject(self, offJoinCache);
    window = readObject(self, offJoinWindow);
    if (!cancel || !online || !search || !cache || !window) {
        LOGE("join controls unavailable: cancel=%p online=%p search=%p cache=%p window=%p",
             cancel, online, search, cache, window);
        return;
    }
    cls = objectClass(cancel);
    if (!canSetFrame(cancel) || !canSetFrame(online) || !canSetFrame(search)) return;
    if (local && (objectClass(local) != cls || !canSetFrame(local))) return;
    getter = checkedMethod(cls, "frame", "R@:");
    if (!getter) return;
    anchor = ((Rect (*)(id, SEL))getImp(getter))(cancel, selReg("frame"));
    if (!usableRect(anchor)) return;
    /* Original Cancel y=52/-54. Our restored short-screen y=-150 is also
     * accepted because advancedButton: reuses controls on subsequent visits.
     * windowInfoChanged: restores the original anchor before this hook runs.
     */
    if (absFloat(anchor.y - 52) < 0.01f) tall = 1;
    else if (absFloat(anchor.y + 54) < 0.01f || absFloat(anchor.y + 150) < 0.01f) tall = 0;
    else { LOGE("unrecognized Advanced Cancel frame; leaving join UI alone"); return; }
    cancelFrame = localFrame = onlineFrame = searchFrame = anchor;
    cancelFrame.y = tall ? 52 : -150;
    localFrame.y = tall ? 4 : -6;
    onlineFrame.y = tall ? -44 : -54;
    searchFrame.y = tall ? -92 : -102;
    if (!local) {
        init = checkedMethod(cls, "initWithFrame:cache:windowInfo:title:", "@@:R@p@");
        target = checkedMethod(cls, "setTarget:action:", "v@:@:");
        backgroundSetter = checkedMethod(cls, "setBackgroundTexture:", "v@:@");
        selectedSetter = checkedMethod(cls, "setBackgroundSelectedTexture:", "v@:@");
        textureMethod = checkedMethod(objectClass(cache), "textureNamed:", "@@:@");
        if (!init || !target || !backgroundSetter || !selectedSetter || !textureMethod) return;
        normal = ((id (*)(id, SEL, id))getImp(textureMethod))(
            cache, selReg("textureNamed:"), string("InventoryButtonBackground.png"));
        selected = ((id (*)(id, SEL, id))getImp(textureMethod))(
            cache, selReg("textureNamed:"), string("InventoryButtonBackgroundSelected.png"));
        if (!normal || !selected) { LOGE("local-join textures missing; leaving UI alone"); return; }
        /* The reference export omitted this initializer's title argument.
         * Use an explicit label until its original CFString is supplied. */
        title = string("LOCAL NETWORK...");
        if (!title) return;
        allocated = send0(cls, "alloc");
        if (!allocated) return;
        local = ((id (*)(id, SEL, Rect, id, void *, id))getImp(init))(
            allocated, selReg("initWithFrame:cache:windowInfo:title:"), localFrame,
            cache, window, title);
        if (!local) return;
        ((void (*)(id, SEL, id))getImp(backgroundSetter))(
            local, selReg("setBackgroundTexture:"), normal);
        ((void (*)(id, SEL, id))getImp(selectedSetter))(
            local, selReg("setBackgroundSelectedTexture:"), selected);
        ((void (*)(id, SEL, id, SEL))getImp(target))(
            local, selReg("setTarget:action:"), self, selJoinLocalAction);
        *(id *)((char *)self + offJoinLocal) = local; /* Game dealloc owns +1. */
        LOGV("created LOCAL NETWORK... button; using retained advancedLocalNetworkButton:");
    } else setFrame(local, localFrame);
    setFrame(cancel, cancelFrame);
    setFrame(online, onlineFrame);
    setFrame(search, searchFrame);
}

static void my_advanced(id self, SEL cmd, id sender) {
    LOGV("JoinWorldUI advancedButton: hook entered");
    ((void (*)(id, SEL, id))g_origAdvanced)(self, cmd, sender);
    LOGV("Advanced original completed: state=%d local=%p",
         *(int *)((char *)self + offJoinState), readObject(self, offJoinLocal));
    restoreLocalJoinButton(self);
    LOGV("Advanced restoration completed: local=%p", readObject(self, offJoinLocal));
}
static void my_joinResize(id self, SEL cmd, void *window) {
    ((void (*)(id, SEL, void *))g_origJoinResize)(self, cmd, window);
    restoreLocalJoinButton(self);
}

/* The inspected Android CFNetwork starts its embedded mDNS daemon before
 * service browsing/registration, but not before domain enumeration. Local
 * Join enumerates domains first. Its libSystem DNS-SD client connects to the
 * daemon on TCP 127.0.0.1:5354, with four attempts/three one-second sleeps.
 * Request daemon startup before enumeration on every search. Its native
 * dispatch_once still owns the daemon thread lifetime.
 * Domain delivery now comes from the Foundation callback bridge; no domain is
 * inserted manually. Retain native sockets, resolution, and result selection.
 */
static int containsDomain(id self, id domain) {
    id domains;
    Method method;
    if (!offClientDomains || !domain) return -1;
    domains = readObject(self, offClientDomains);
    if (!domains) return -1;
    method = getInstMethod(objectClass(domains), selReg("containsObject:"));
    if (!signature(method, "c@:@") && !signature(method, "B@:@")) return -1;
    return ((unsigned char (*)(id, SEL, id))getImp(method))(
        domains, selReg("containsObject:"), domain) != 0;
}
static void my_searchServers(id self, SEL cmd) {
    discardEndpoints(self);
    ensureMDNS(); /* Requests thread startup; no listener-readiness guarantee. */
    LOGV("requested existing mDNS daemon startup before LAN domain search");
    LOGV("LAN search: Foundation domain bridge=%d manualDomain=0", foundationDomainBridge);
    ((void (*)(id, SEL))g_origSearchServers)(self, cmd);
}

/* Observability for the retained Foundation/DNS-SD path. In particular, the
 * application previously lacked didNotSearch:, so synchronous search errors
 * could be discarded after respondsToSelector:. Add only a logging callback
 * when missing; preserve an existing implementation when present.
 */
static const char *debugText(id object) {
    Method method;
    id description;
    const char *text;
    if (!object) return "(nil)";
    method = getInstMethod(objectClass(object), selReg("UTF8String"));
    if (!signature(method, "*@:")) {
        method = getInstMethod(objectClass(object), selReg("description"));
        if (!signature(method, "@@:")) return "(description unavailable)";
        description = ((id (*)(id, SEL))getImp(method))(object, selReg("description"));
        if (!description) return "(nil description)";
        object = description;
        method = getInstMethod(objectClass(object), selReg("UTF8String"));
        if (!signature(method, "*@:")) return "(UTF8String unavailable)";
    }
    text = ((const char *(*)(id, SEL))getImp(method))(object, selReg("UTF8String"));
    return text ? text : "(null UTF8String)";
}
static const char *serviceName(id service) {
    Method method;
    id name;
    if (!service) return "(nil)";
    method = getInstMethod(objectClass(service), selReg("name"));
    if (!signature(method, "@@:")) return "(name unavailable)";
    name = ((id (*)(id, SEL))getImp(method))(service, selReg("name"));
    return debugText(name);
}
static id diagnosticObject(id object, const char *selector) {
    Method method;
    if (!object) return NULL;
    method = getInstMethod(objectClass(object), selReg(selector));
    if (!signature(method, "@@:")) return NULL;
    return ((id (*)(id, SEL))getImp(method))(object, selReg(selector));
}
static int32_t diagnosticNumber(id object, const char *selector) {
    Method method;
    if (!object) return -1;
    method = getInstMethod(objectClass(object), selReg(selector));
    if (!signature(method, "i@:") && !signature(method, "I@:") &&
        !signature(method, "l@:") && !signature(method, "L@:")) return -1;
    /* All four integer encodings are 32 bits in the target ARM32 runtime. */
    return ((int32_t (*)(id, SEL))getImp(method))(object, selReg(selector));
}
static const char *stringUTF8(id object) {
    Method method;
    if (!object) return NULL;
    method = getInstMethod(objectClass(object), selReg("UTF8String"));
    if (!signature(method, "*@:")) return NULL;
    return ((const char *(*)(id, SEL))getImp(method))(object, selReg("UTF8String"));
}
static id objectAtIndex(id array, uint32_t index) {
    Method method;
    if (!array) return NULL;
    method = getInstMethod(objectClass(array), selReg("objectAtIndex:"));
    if (!signature(method, "@@:I") && !signature(method, "@@:i") &&
        !signature(method, "@@:L") && !signature(method, "@@:l")) return NULL;
    return ((id (*)(id, SEL, uint32_t))getImp(method))(array, selReg("objectAtIndex:"), index);
}
static id retainForDomain(id object) {
    Method retain = object ? getInstMethod(objectClass(object), selReg("retain")) : NULL;
    Method release = object ? getInstMethod(objectClass(object), selReg("release")) : NULL;
    if (!signature(retain, "@@:") || !signature(release, "v@:")) return NULL;
    return ((id (*)(id, SEL))getImp(retain))(object, selReg("retain"));
}
static void releaseForDomain(id object) {
    if (object) ((void (*)(id, SEL))g_msgSend)(object, selReg("release"));
}
/* Foundation's native dispatcher accepts service objects but the domain path
 * needs an NSString callback. Wrap browser creation during init so the domain
 * callback can retain its owner/delegate/domain, copy the stack error, and queue
 * delivery on the main queue. Release those retained values after delivery;
 * non-domain events still call the original browser callback directly.
 */
static void my_browserDispatch(id self, SEL cmd, const void *object,
                                uint32_t flags, const BrowserError *error) {
    id delegate;
    const char *selector;
    Method method;
    if (!(flags & 2)) {
        ((void (*)(id, SEL, const void *, uint32_t, const BrowserError *))g_origBrowserDispatch)(
            self, cmd, object, flags, error);
        return;
    }
    if ((error && error->error) || !object || cfGetTypeID(object) != cfStringTypeID()) {
        LOGE("Foundation domain callback rejected: flags=%u object=%p error=%d",
             (unsigned)flags, object, error ? (int)error->error : 0); return;
    }
    delegate = diagnosticObject(self, "delegate");
    selector = (flags & 8) ? "netServiceBrowser:didRemoveDomain:moreComing:" :
                           "netServiceBrowser:didFindDomain:moreComing:";
    method = delegate ? getInstMethod(objectClass(delegate), selReg(selector)) : NULL;
    if (!signature(method, "v@:@@c") && !signature(method, "v@:@@B")) {
        LOGV("Foundation domain callback: delegate does not implement %s", selector); return;
    }
    LOGV("Foundation domain callback delivered: domain=%s flags=%u removed=%d moreComing=%d",
         debugText((id)object), (unsigned)flags, (flags & 8) != 0, (flags & 1) != 0);
    ((void (*)(id, SEL, id, id, unsigned char))getImp(method))(
        delegate, selReg(selector), self, (id)object, (flags & 1) != 0);
}
static const void *retainBrowserContext(const void *pointer) {
    WrappedBrowserContext *context = (WrappedBrowserContext *)pointer;
    __atomic_add_fetch(&context->references, 1, __ATOMIC_RELAXED);
    return pointer;
}
static void releaseBrowserContext(const void *pointer) {
    WrappedBrowserContext *context = (WrappedBrowserContext *)pointer;
    if (!__atomic_sub_fetch(&context->references, 1, __ATOMIC_ACQ_REL)) free(context);
}
static void deliverDomain(void *pointer) {
    QueuedDomain *event = pointer;
    /* A changed/cleared delegate cancels delivery to the captured target. Both
     * objects are retained for this queued event, not for the CF browser's life.
     */
    if (diagnosticObject(event->owner, "delegate") == event->delegate) {
        my_browserDispatch(event->owner, selReg("_dispatchCallBack:flags:error:"),
                            event->domain, event->flags, &event->error);
    } else LOGV("Foundation domain callback skipped: delegate changed before delivery");
    cfRelease(event->domain);
    releaseForDomain(event->delegate); releaseForDomain(event->owner);
    free(event);
}
static void foundationBrowserCallback(void *browser, uint32_t flags, const void *object,
                                      const BrowserError *error, void *pointer) {
    WrappedBrowserContext *context = pointer;
    QueuedDomain *event;
    if (!(flags & 2)) {
        /* Service discovery keeps its original Foundation callback path. */
        context->callback(browser, flags, object, error, context->info);
        return;
    }
    if (!object || cfGetTypeID(object) != cfStringTypeID()) {
        LOGE("Foundation domain bridge: unexpected domain object=%p flags=%u", object, (unsigned)flags);
        return;
    }
    event = calloc(1, sizeof(*event));
    if (!event) { LOGE("Foundation domain bridge: allocation failed"); return; }
    event->owner = retainForDomain(context->info);
    event->delegate = retainForDomain(diagnosticObject(context->info, "delegate"));
    if (!event->owner || !event->delegate) {
        releaseForDomain(event->owner); releaseForDomain(event->delegate); free(event);
        LOGV("Foundation domain bridge: no retainable browser/delegate"); return;
    }
    event->domain = cfRetain(object);
    event->flags = flags;
    if (error) event->error = *error; /* Original error storage is stack-local. */
    LOGV("Foundation domain callback queued: domain=%s flags=%u", debugText((id)object), (unsigned)flags);
    asyncFunction(foundationMainQueue, event, deliverDomain);
}
static void *my_browserCreate(const void *allocator, BrowserCallback callback, BrowserContext *context) {
    WrappedBrowserContext *wrapped;
    BrowserContext forwarded;
    void *browser;
    /* Match the inspected NSNetServiceBrowser init context. Other CF callers
     * and ownership conventions forward unchanged rather than being guessed.
     */
    if (!callback || !context || context->version || !context->info || context->retain ||
        context->release || context->copyDescription || context->info != initializingBrowser)
        return g_origBrowserCreate(allocator, callback, context);
    wrapped = calloc(1, sizeof(*wrapped));
    if (!wrapped) return g_origBrowserCreate(allocator, callback, context);
    wrapped->references = 1; wrapped->callback = callback; wrapped->info = context->info;
    forwarded = (BrowserContext){.info = wrapped, .retain = retainBrowserContext, .release = releaseBrowserContext};
    browser = g_origBrowserCreate(allocator, foundationBrowserCallback, &forwarded);
    releaseBrowserContext(wrapped);
    LOGV("Foundation domain bridge: browser created owner=%p CFbrowser=%p", context->info, browser);
    return browser;
}
static id my_browserInit(id self, SEL cmd) {
    id previous = initializingBrowser, result;
    initializingBrowser = self;
    result = ((id (*)(id, SEL))g_origBrowserInit)(self, cmd);
    initializingBrowser = previous;
    return result;
}
/* NSNetService exposes Apple's sockaddr byte layout in this port. Read its
 * family, address and network-order port explicitly rather than casting to an
 * Android sockaddr. The endpoint cache stores a host-order port; the service
 * port hook corrects the integer seen by the game within resolution scope.
 */
static int sockaddrIPv4Endpoint(const unsigned char *bytes, uint32_t length,
                                char address[INET_ADDRSTRLEN], uint16_t *port) {
    struct in_addr parsed;
    /* Recognize ARM32 Linux sa_family_t and BSD sin_len/sin_family layouts.
     * Their IPv4 port/address fields occupy the same byte offsets.
     */
    if (!bytes || length < 16 || !port) return 0;
    if (!((bytes[0] == AF_INET && bytes[1] == 0) ||
          ((bytes[0] == 16 || bytes[0] == 0) && bytes[1] == AF_INET))) return 0;
    memcpy(&parsed, bytes + 4, sizeof(parsed));
    if (!inet_ntop(AF_INET, &parsed, address, INET_ADDRSTRLEN)) return 0;
    *port = (uint16_t)(((uint16_t)bytes[2] << 8) | bytes[3]);
    return 1;
}
static int localHostname(const char *name) {
    size_t length = name ? strnlen(name, LAN_HOSTNAME_SIZE) : 0;
    if (!length || length == LAN_HOSTNAME_SIZE) return 0;
    return (length > 7 && !strcasecmp(name + length - 7, ".local.")) ||
           (length > 6 && !strcasecmp(name + length - 6, ".local"));
}
static int decimalPort(const char *text, uint16_t *port) {
    unsigned value = 0;
    size_t i;
    if (!text || !*text) return 0;
    for (i = 0; text[i]; ++i) {
        if (i >= 5 || text[i] < '0' || text[i] > '9') return 0;
        value = value * 10 + (unsigned)(text[i] - '0');
        if (value > UINT16_MAX) return 0;
    }
    if (!value) return 0;
    *port = (uint16_t)value;
    return 1;
}
static int serviceEndpoint(id match, id service, int32_t rawPort, LANEndpoint *endpoint) {
    id addresses = diagnosticObject(service, "addresses");
    const char *hostname = stringUTF8(diagnosticObject(service, "hostName"));
    int32_t count = diagnosticNumber(addresses, "count"), i;
    LANEndpoint result = {0};
    if (!match || !localHostname(hostname) || count <= 0 || count > 1024 ||
        rawPort <= 0 || rawPort > UINT16_MAX) return 0;
    for (i = 0; i < count; ++i) {
        id data = objectAtIndex(addresses, (uint32_t)i);
        int32_t length = diagnosticNumber(data, "length");
        Method method = data ? getInstMethod(objectClass(data), selReg("bytes")) : NULL;
        const unsigned char *bytes;
        char address[INET_ADDRSTRLEN];
        uint16_t port;
        if (length < 16 || !signature(method, "^@:")) continue;
        bytes = ((const unsigned char *(*)(id, SEL))getImp(method))(data, selReg("bytes"));
        if (!sockaddrIPv4Endpoint(bytes, (uint32_t)length, address, &port)) continue;
        if (!port || (result.valid && result.port != port)) return 0;
        if (!result.valid) {
            memcpy(&result.address, bytes + 4, sizeof(result.address));
            if (result.address.s_addr == INADDR_ANY || result.address.s_addr == INADDR_BROADCAST) return 0;
            result.port = port; result.valid = 1;
        }
    }
    if (!result.valid || (rawPort != result.port &&
        rawPort != (int32_t)(((result.port & 0xFF) << 8) | (result.port >> 8)))) return 0;
    result.match = match;
    memcpy(result.hostname, hostname, strlen(hostname) + 1);
    *endpoint = result;
    return 1;
}
static int32_t my_servicePort(id self, SEL cmd) {
    int32_t raw = ((int32_t (*)(id, SEL))g_origServicePort)(self, cmd);
    /* Only the selected game's native resolve callback sees the correction.
     * Publishing/constructed NSNetService objects retain their original getter.
     * Never blindly swap the getter or write private CF object fields.
     */
    if (self == activeResolvedService && raw == activeResolvedRawPort && raw != activeResolvedPort) {
        LOGV("join: corrected resolved service port raw=%d hostOrder=%d", (int)raw, (int)activeResolvedPort);
        return activeResolvedPort;
    }
    return raw;
}
static void logResolvedSocketAddresses(id service) {
    id addresses = diagnosticObject(service, "addresses");
    int32_t count = diagnosticNumber(addresses, "count"), i;
    int32_t servicePort = diagnosticNumber(service, "port");
    (void)servicePort;
    if (count < 0 || count > 1024) return;
    for (i = 0; i < count; ++i) {
        id data = objectAtIndex(addresses, (uint32_t)i);
        int32_t length = diagnosticNumber(data, "length");
        Method bytesMethod = data ? getInstMethod(objectClass(data), selReg("bytes")) : NULL;
        const unsigned char *bytes;
        char address[INET_ADDRSTRLEN];
        uint16_t port;
        if (length < 16 || !signature(bytesMethod, "^@:")) continue;
        bytes = ((const unsigned char *(*)(id, SEL))getImp(bytesMethod))(data, selReg("bytes"));
        if (sockaddrIPv4Endpoint(bytes, (uint32_t)length, address, &port))
            LOGV("DNS-SD: sockaddr[%d] IPv4=%s networkPort=%u service.port=%d match=%d",
                 (int)i, address, (unsigned)port, (int)servicePort, servicePort == port);
    }
}
static void probeClientResolver(id self) {
    id host = offClientHost ? readObject(self, offClientHost) : NULL;
    const char *hostname = stringUTF8(host);
    ENetProbeAddress probe = {{0}, 0};
    int savedErrno = errno, result, previousPhase = resolverTracePhase;
    int *errorLocation;
    int savedHostError;
    char address[INET_ADDRSTRLEN];
    if (!enetSetHost || !hostname) {
        LOGI("join: resolver probe unavailable (ENet export or hostname unavailable)"); return;
    }
    errorLocation = nativeHostError ? nativeHostError() : NULL;
    savedHostError = errorLocation ? *errorLocation : 0;
    /* This is an additional lookup using the application's real ENet setter,
     * not an interception of the game's later call. It can warm resolver caches.
     * Its output is logged and discarded: never copy it into match state.
     */
    resolverTracePhase = 1;
    result = enetSetHost(&probe, hostname);
    resolverTracePhase = previousPhase;
    if (result == 0 && inet_ntop(AF_INET, &probe.host, address, sizeof(address)))
        LOGV("join: ENet resolver probe host=%s result=%d IPv4=%s", hostname, result, address);
    else LOGV("join: ENet resolver probe host=%s result=%d", hostname, result);
    if (errorLocation) *errorLocation = savedHostError;
    errno = savedErrno;
}
static void logClientEndpoint(id self, const char *stage) {
    (void)self; (void)stage;
    LOGV("join: %s host=%s port=%s", stage,
         offClientHost ? debugText(readObject(self, offClientHost)) : "(ivar unavailable)",
         offClientPort ? debugText(readObject(self, offClientPort)) : "(ivar unavailable)");
}
static void my_worldSelection(id self, SEL cmd, int32_t index) {
    LOGV("join: worldSelectedAtIndex: index=%d foundHosts=%s", (int)index,
         offClientFoundHosts ? debugText(readObject(self, offClientFoundHosts)) : "(ivar unavailable)");
    ((void (*)(id, SEL, int32_t))g_origWorldSelection)(self, cmd, index);
    /* Callbacks may release the match, so do not dereference it after return. */
    LOGV("join: original selection callback returned");
}
/* Scope resolver assistance to the game's synchronous connect call. Restore
 * all prior TLS on return so unrelated lookups and nested calls keep their own
 * endpoint. The game continues to choose the world and create its ENet peer.
 */
static void my_clientConnect(id self, SEL cmd) {
    int previousPhase = resolverTracePhase;
    LANEndpoint previousEndpoint = activeEndpoint;
    char previousHostname[LAN_HOSTNAME_SIZE];
    uint16_t port;
    const char *hostname = offClientHost ? stringUTF8(readObject(self, offClientHost)) : NULL;
    const char *portText = offClientPort ? stringUTF8(readObject(self, offClientPort)) : NULL;
    memcpy(previousHostname, activeConnectHostname, sizeof(previousHostname));
    activeConnectHostname[0] = '\0';
    if (localDNSFixEnabled && localHostname(hostname) && decimalPort(portText, &port))
        memcpy(activeConnectHostname, hostname, strlen(hostname) + 1);
    memset(&activeEndpoint, 0, sizeof(activeEndpoint));
    /* Native worldSelectedAtIndex: calls connect before releasing foundHosts.
     * A direct-IP/cloud connection does not have this LAN selection collection.
     */
    if (endpointFixEnabled && offClientFoundHosts && readObject(self, offClientFoundHosts) &&
        decimalPort(portText, &port)) activeEndpoint = selectedEndpoint(self, hostname, port);
    logClientEndpoint(self, "connectToSpecificHostAndPort entered");
    if (runResolverProbe) probeClientResolver(self);
    resolverTracePhase = 2;
    ((void (*)(id, SEL))g_origClientConnect)(self, cmd);
    resolverTracePhase = previousPhase;
    activeEndpoint = previousEndpoint;
    memcpy(activeConnectHostname, previousHostname, sizeof(activeConnectHostname));
    discardEndpoints(self);
    LOGV("join: original connection startup returned (not handshake confirmation)");
}
static void my_worldOptions(id self, SEL cmd, id match, id options) {
    LOGV("join: world options callback match=%p count=%d options=%s", match,
         (int)diagnosticNumber(options, "count"), debugText(options));
    /* Preserve the game's dialog and its OK/Cancel actions exactly. */
    ((void (*)(id, SEL, id, id))g_origWorldOptions)(self, cmd, match, options);
    LOGV("join: original world options callback returned");
}
static void my_domainSearch(id self, SEL cmd) {
    LOGV("Foundation: starting domain enumeration browser=%p", self);
    ((void (*)(id, SEL))g_origDomainSearch)(self, cmd);
    LOGV("Foundation: domain enumeration call returned browser=%p", self);
}
static void my_serviceSearch(id self, SEL cmd, id type, id domain) {
    LOGV("Foundation: starting service browse type=%s domain=%s browser=%p",
         debugText(type), debugText(domain), self);
    ((void (*)(id, SEL, id, id))g_origServiceSearch)(self, cmd, type, domain);
    LOGV("Foundation: service browse call returned browser=%p", self);
}
static void my_foundDomain(id self, SEL cmd, id browser, id domain, unsigned char more) {
    if (containsDomain(self, domain) == 1) {
        LOGV("DNS-SD: ignoring duplicate domain=%s", debugText(domain)); return;
    }
    LOGV("DNS-SD: found domain=%s moreComing=%u", debugText(domain), more);
    ((void (*)(id, SEL, id, id, unsigned char))g_origFoundDomain)(self, cmd, browser, domain, more);
}
static void my_foundService(id self, SEL cmd, id browser, id service, unsigned char more) {
    LOGV("DNS-SD: found service=%s moreComing=%u", serviceName(service), more);
    ((void (*)(id, SEL, id, id, unsigned char))g_origFoundService)(self, cmd, browser, service, more);
}
static void my_resolvedService(id self, SEL cmd, id service) {
    LANEndpoint endpoint;
    id previousService = activeResolvedService;
    int32_t previousRawPort = activeResolvedRawPort, previousPort = activeResolvedPort;
    int32_t rawPort = service && g_origServicePort ?
        ((int32_t (*)(id, SEL))g_origServicePort)(service, selReg("port")) : -1;
    LOGV("DNS-SD: resolved service=%s", serviceName(service));
    LOGV("DNS-SD: resolved endpoint host=%s rawServicePort=%d addresses=%s",
         debugText(diagnosticObject(service, "hostName")),
         (int)diagnosticNumber(service, "port"),
         debugText(diagnosticObject(service, "addresses")));
    logResolvedSocketAddresses(service);
    if (endpointFixEnabled && serviceEndpoint(self, service, rawPort, &endpoint)) {
        char address[INET_ADDRSTRLEN];
        rememberEndpoint(&endpoint);
        activeResolvedService = service;
        activeResolvedRawPort = rawPort; activeResolvedPort = endpoint.port;
        if (inet_ntop(AF_INET, &endpoint.address, address, sizeof(address)))
            LOGV("join: stored DNS-SD endpoint match=%p host=%s IPv4=%s port=%u rawPort=%d",
                 self, endpoint.hostname, address, (unsigned)endpoint.port, (int)rawPort);
    }
    ((void (*)(id, SEL, id))g_origResolvedService)(self, cmd, service);
    activeResolvedService = previousService;
    activeResolvedRawPort = previousRawPort; activeResolvedPort = previousPort;
    LOGV("DNS-SD: original resolve callback returned");
}
static void my_searchError(id self, SEL cmd, id browser, id error) {
    LOGE("DNS-SD: browser search failed browser=%p error=%s", browser, debugText(error));
    if (g_origSearchError) ((void (*)(id, SEL, id, id))g_origSearchError)(self, cmd, browser, error);
}
static void my_resolveError(id self, SEL cmd, id service, id error) {
    LOGE("DNS-SD: resolve failed service=%s error=%s", serviceName(service), debugText(error));
    if (g_origResolveError) ((void (*)(id, SEL, id, id))g_origResolveError)(self, cmd, service, error);
}
static int hookDiagnostic(Class cls, const char *selector, const char *encoding,
                          const char *alternative, IMP replacement, IMP *original) {
    /* 1 - caller supplies the feature class (possibly absent for an optional hook).
     * 2 - g_msgSend was checked by the file-level installer.
     * 3 - lookup; retain the existing ABI refusal before the idempotency check.
     */
    Method method = cls ? getInstMethod(cls, selReg(selector)) : NULL;
    if (!method || (!signature(method, encoding) &&
                    !(alternative && signature(method, alternative)))) {
        LOGE("diagnostic unavailable for %s: %s", selector,
             method && getTypeEnc(method) ? getTypeEnc(method) : "(missing)");
        return 0;
    }
    if (getImp(method) == replacement) return 1;
    /* 4 - no ivars used by this commit helper; callers resolve their feature offsets.
     * 5 - preserve the current target for forwarding and select a named ENC_*.
     * Step 6 is skipped for this independent hook; hook() makes its one exact check.
     */
    *original = getImp(method);
    if (!*original) return 0;
    /* Existing signature/IMP checks above remain authoritative. The shared
     * helper adds exact measured checks, or logs an unmeasured encoding.
     */
    /* 7 - one independent commit; no previously installed sibling is rolled back. */
    return hook(cls, selector, expectedHookEncoding(selector), replacement, original);
}

/* World initialization can invoke automatic welcome display before a host is
 * ready. The constructor trampoline records only its current world/delegate
 * and local-host status in TLS; manual screens and client display still forward.
 */
static int isLANWelcomeServer(id server) {
    id match;
    Method cloud;
    if (!server || objectClass(server) != welcomeServerClass || !offWelcomeServerMatch) return 0;
    match = readObject(server, offWelcomeServerMatch);
    if (!match || objectClass(match) != welcomeMatchClass) return 0;
    cloud = getInstMethod(objectClass(server), selReg("isCloudMatch"));
    if (!signature(cloud, "c@:") && !signature(cloud, "B@:")) return 0;
    if (!getImp(cloud)) return 0;
    return ((unsigned char (*)(id, SEL))getImp(cloud))(server, selReg("isCloudMatch")) == 0;
}
static void my_welcomeDisplay(id self, SEL cmd, id message, id rules, unsigned char allowEdit) {
    WelcomeWorldContext *context = welcomeWorldContext;
    if (context && context->localHost && context->delegate == self && !allowEdit) {
        LOGV("hosting: suppressed automatic LAN-host welcome during World initialization world=%p delegate=%p", context->world, self);
        return;
    }
    ((void (*)(id, SEL, id, id, unsigned char))g_origWelcomeDisplay)(self, cmd, message, rules, allowEdit);
}
#define WELCOME_WORLD_ARGUMENTS(delayType) \
    id self, SEL cmd, void *window, id cache, id delegate, id saveID, id name, \
    id client, id server, id multiplayerData, id hostData, delayType delay, \
    int32_t width, id rules, unsigned char expert
#define WELCOME_WORLD_VALUES \
    self, cmd, window, cache, delegate, saveID, name, client, server, \
    multiplayerData, hostData, delay, width, rules, expert
#define DEFINE_WELCOME_WORLD_HOOK(functionName, delayType) \
    static id functionName(WELCOME_WORLD_ARGUMENTS(delayType)) { \
        WelcomeWorldContext context = {self, delegate, !client && isLANWelcomeServer(server)}; \
        WelcomeWorldContext *previous = welcomeWorldContext; \
        id result; \
        welcomeWorldContext = &context; \
        if (context.localHost) LOGV("hosting: World initialization LAN-host scope entered world=%p delegate=%p", self, delegate); \
        result = ((id (*)(WELCOME_WORLD_ARGUMENTS(delayType)))g_origWelcomeWorldInit)(WELCOME_WORLD_VALUES); \
        welcomeWorldContext = previous; \
        return result; \
    }
DEFINE_WELCOME_WORLD_HOOK(my_welcomeWorldFloat, float)
DEFINE_WELCOME_WORLD_HOOK(my_welcomeWorldDouble, double)
DEFINE_WELCOME_WORLD_HOOK(my_welcomeWorldInteger, int32_t)
#undef DEFINE_WELCOME_WORLD_HOOK

static IMP welcomeWorldHookForEncoding(Method method) {
    const char *delay, *width, *expert;
    char expected[32];
    /* Each variant has its own typed trampoline: ARM32 double arguments need
     * different alignment than floats/integers. Never infer ABI from IDA's
     * pseudocode. WindowInfo may be encoded as an object or struct pointer.
     */
    for (delay = "fdiIlL"; *delay; ++delay)
        for (width = "iIlL"; *width; ++width)
            for (expert = "cB"; *expert; ++expert) {
                memcpy(expected, "@@:p@@@@@@@@fI@c", 17);
                expected[12] = *delay; expected[13] = *width; expected[15] = *expert;
                if (signature(method, expected)) {
                    if (*delay == 'f') return (IMP)my_welcomeWorldFloat;
                    if (*delay == 'd') return (IMP)my_welcomeWorldDouble;
                    return (IMP)my_welcomeWorldInteger;
                }
            }
    return NULL;
}
static uint32_t nativeBooleanOffset(Class cls, const char *name) {
    Ivar flag = cls ? getIvar(cls, name) : NULL;
    const char *encoding = flag ? ivarEncoding(flag) : NULL;
    ptrdiff_t offset = flag ? ivarOff(flag) : 0;
    if (!encoding || (!matchesType(encoding, 'c') && !matchesType(encoding, 'B')) ||
        offset <= 0 || offset >= 0x10000) {
        LOGE("hosting: native client guard unavailable flag=%s encoding=%s", name, encoding ? encoding : "(missing)");
        return 0;
    }
    LOGV("hosting: native client guard bound flag=%s offset=0x%x; game owns flag lifecycle", name, (unsigned)offset);
    return (uint32_t)offset;
}
static IMP nativeBooleanGetter(Class cls, const char *selector) {
    Method getter = cls ? getInstMethod(cls, selReg(selector)) : NULL;
    return (signature(getter, "c@:") || signature(getter, "B@:")) ? getImp(getter) : NULL;
}
static int nativeBooleanFlag(id game, uint32_t offset) {
    return offset ? *((unsigned char *)game + offset) != 0 : -1;
}
static int nativeBooleanValue(id object, IMP getter, const char *selector) {
    return getter ? ((unsigned char (*)(id, SEL))getter)(object, selReg(selector)) != 0 : -1;
}
static int chatHaxOngoing(id game) {
    return nativeBooleanValue(game, nativeChatHaxGetter, "chatHaxOngoing");
}
static int nativeChartboostShowing(id game) {
    if (!offHostController) return -1;
    id controller = readObject(game, offHostController);
    if (!controller) return 0; /* Native messaging nil is false. */
    return nativeBooleanValue(controller, nativeBooleanGetter(objectClass(controller), "chartboostShowing"), "chartboostShowing");
}
static void consumeHostPicker(id game, int consume) {
    if (!consume) return;
    ((void (*)(id, SEL, unsigned char))nativePickerSetter)(game, selReg("setPickerShowing:"), 0);
    LOGV("hosting: consumed native pickerShowing protection after background handler");
}
/* The native client branch protects temporary activities using six game-owned
 * guards. Apply the same guard decision to a LAN host by parking its server only
 * during the original background handler, then restoring it if the handler did
 * not replace it. Flag ownership and ordinary disconnect/cleanup stay native.
 */
static void forwardGameBackground(id self, SEL cmd) {
    id server = offGameServer ? readObject(self, offGameServer) : NULL;
    id world = offGameWorld ? readObject(self, offGameWorld) : NULL;
    id retained;
    int welcome = nativeBooleanFlag(self, offHostWelcomeFlag);
    int chat = chatHaxOngoing(self);
    int movie = nativeBooleanFlag(self, offHostMovieFlag);
    int iap = nativeBooleanFlag(self, offHostIAPFlag);
    int chartboost = nativeChartboostShowing(self);
    int picker = nativeBooleanValue(self, nativePickerGetter, "pickerShowing");
    int lanHost = offHostWelcomeFlag && isLANWelcomeServer(server);
    /* With an actual client present, the original client branch owns its
     * picker reset. For a host alone, consume the same one-shot protection
     * after the original handler, even when another guard is also true.
     */
    int consumePicker = lanHost && nativePickerSetter && offHostClient && !readObject(self, offHostClient);
    Method cleanup = server ? checkedMethod(objectClass(server), "cleanup", "v@:") : NULL;
    if (server) LOGV("hosting: native temporary UI guard state pickerShowing=%d movieViewInProgress=%d chartboostShowing=%d welcomeMessageShowing=%d iapInProgress=%d chatHaxOngoing=%d (-1=unavailable)",
                     picker, movie, chartboost, welcome, iap, chat);
    if (!lanHost || (picker != 1 && movie != 1 && chartboost != 1 && welcome != 1 && iap != 1 && chat != 1) ||
        !world || !cleanup || !getImp(cleanup) ||
        !(retained = retainForDomain(server))) {
        ((void (*)(id, SEL))g_origGameBackground)(self, cmd);
        consumeHostPicker(self, consumePicker);
        return;
    }
    /* Park only GameView's owning reference for the synchronous call, so
     * the native server-cleanup branch sees nil. The rest of the original
     * handler (including its client guards) still runs. World.server and
     * the server's sockets, delegate and timer remain untouched.
     */
    *(id *)((char *)self + offGameServer) = NULL;
    ((void (*)(id, SEL))g_origGameBackground)(self, cmd);
    consumeHostPicker(self, consumePicker);
    if (!readObject(self, offGameServer) && readObject(self, offGameWorld) == world) {
        *(id *)((char *)self + offGameServer) = server;
        if (welcome == 1)
            LOGV("hosting: retained LAN server through welcome activity using native client flag offset=0x%x server=%p",
                 offHostWelcomeFlag, server);
        else LOGV("hosting: retained LAN server using native client guard=%s server=%p",
                  chat == 1 ? "chatHaxOngoing" : picker == 1 ? "pickerShowing" :
                  movie == 1 ? "movieViewInProgress" : chartboost == 1 ? "chartboostShowing" : "iapInProgress", server);
    } else {
        /* Reentrant world replacement must not restore the previous server
         * or overwrite a new one. Dispose of its parked owning reference.
         */
        ((void (*)(id, SEL))getImp(cleanup))(server, selReg("cleanup"));
        releaseForDomain(server);
        LOGV("hosting: temporary UI background handler replaced world/server; previous server not restored");
    }
    releaseForDomain(retained);
}

static int activeWelcomeHostGame(id game, id world, id server) {
    return game && world && server && objectClass(game) == welcomeGameClass && offGameWorld && offGameServer &&
        readObject(game, offGameWorld) == world && readObject(game, offGameServer) == server &&
        readObject(world, offWelcomeWorldServer) == server && isLANWelcomeServer(server);
}
static void callWelcomeUpdate(id game, SEL cmd, double dt, double accurate) {
    if (welcomeUpdateDTType == 'f' && welcomeUpdateAccurateType == 'f')
        ((void (*)(id, SEL, float, float))g_origWelcomeUpdate)(game, cmd, (float)dt, (float)accurate);
    else if (welcomeUpdateDTType == 'f')
        ((void (*)(id, SEL, float, double))g_origWelcomeUpdate)(game, cmd, (float)dt, accurate);
    else if (welcomeUpdateAccurateType == 'f')
        ((void (*)(id, SEL, double, float))g_origWelcomeUpdate)(game, cmd, dt, (float)accurate);
    else ((void (*)(id, SEL, double, double))g_origWelcomeUpdate)(game, cmd, dt, accurate);
}
/* Main-thread requests carry game/world/server identities and a serial token.
 * Check them again at delivery: a queued request from a replaced world must not
 * update the new game. The pending slot coalesces updates while preserving the
 * native update arguments for the latest accepted request.
 */
static void deliverWelcomeUpdate(id game, id world, id server, uint32_t token) {
    WelcomePendingUpdate update;
    pthread_mutex_lock(&welcomeUpdateLock);
    if (!token || welcomePendingUpdate.game != game || welcomePendingUpdate.world != world || welcomePendingUpdate.server != server ||
        welcomePendingUpdate.token != token) {
        pthread_mutex_unlock(&welcomeUpdateLock); return;
    }
    update = welcomePendingUpdate;
    memset(&welcomePendingUpdate, 0, sizeof(welcomePendingUpdate));
    pthread_mutex_unlock(&welcomeUpdateLock);
    if (!welcomeOnMainThread()) {
        LOGE("hosting: queued welcome GameView update did not reach NSThread.mainThread; update skipped"); return;
    }
    if (!activeWelcomeHostGame(game, world, server)) {
        LOGV("hosting: queued welcome GameView update discarded after host/world change"); return;
    }
    LOGV("hosting: welcome GameView update delivered on NSThread.mainThread game=%p world=%p token=%u", game, world, token);
    callWelcomeUpdate(game, selReg("update:accurateDT:"), update.dt, update.accurate);
}
static void forwardWelcomeUpdate(id game, SEL cmd, double dt, double accurate) {
    id world = offGameWorld ? readObject(game, offGameWorld) : NULL;
    id server = offGameServer ? readObject(game, offGameServer) : NULL;
    uint32_t token;
    if (!welcomeOnMainThread || !enqueueWelcomeUpdate ||
        !activeWelcomeHostGame(game, world, server) || welcomeOnMainThread()) {
        callWelcomeUpdate(game, cmd, dt, accurate); return;
    }
    /* Include a final background callback racing with welcome closure. The
     * server requires main-thread ownership even after the native flag clears.
     */
    pthread_mutex_lock(&welcomeUpdateLock);
    if (welcomePendingUpdate.game == game && welcomePendingUpdate.world == world && welcomePendingUpdate.server == server) {
        /* Keep one queued update with the latest frame times, rather than
         * accumulating simulation work if the native main run loop pauses.
         */
        welcomePendingUpdate.dt = dt; welcomePendingUpdate.accurate = accurate;
        pthread_mutex_unlock(&welcomeUpdateLock); return;
    }
    if (++welcomeUpdateSerial == 0) ++welcomeUpdateSerial;
    token = welcomeUpdateSerial;
    welcomePendingUpdate = (WelcomePendingUpdate){game, world, server, token, dt, accurate};
    pthread_mutex_unlock(&welcomeUpdateLock);
    if (!enqueueWelcomeUpdate(game, world, server, token)) {
        pthread_mutex_lock(&welcomeUpdateLock);
        if (welcomePendingUpdate.game == game && welcomePendingUpdate.token == token)
            memset(&welcomePendingUpdate, 0, sizeof(welcomePendingUpdate));
        pthread_mutex_unlock(&welcomeUpdateLock);
        LOGE("hosting: could not queue welcome GameView update on native main thread; update skipped");
    } else LOGV("hosting: off-main welcome GameView update queued game=%p world=%p token=%u", game, world, token);
}
#define DEFINE_WELCOME_UPDATE_HOOK(name, dtType, accurateType) \
    static void name(id self, SEL cmd, dtType dt, accurateType accurate) { \
        forwardWelcomeUpdate(self, cmd, dt, accurate); \
    }
DEFINE_WELCOME_UPDATE_HOOK(my_welcomeUpdateFF, float, float)
DEFINE_WELCOME_UPDATE_HOOK(my_welcomeUpdateFD, float, double)
DEFINE_WELCOME_UPDATE_HOOK(my_welcomeUpdateDF, double, float)
DEFINE_WELCOME_UPDATE_HOOK(my_welcomeUpdateDD, double, double)
#undef DEFINE_WELCOME_UPDATE_HOOK
static IMP welcomeUpdateHookForEncoding(Method method) {
    if (signature(method, "v@:ff")) return (IMP)my_welcomeUpdateFF;
    if (signature(method, "v@:fd")) return (IMP)my_welcomeUpdateFD;
    if (signature(method, "v@:df")) return (IMP)my_welcomeUpdateDF;
    if (signature(method, "v@:dd")) return (IMP)my_welcomeUpdateDD;
    return NULL;
}
static const char *dialogClassName(Class cls) {
    const char *name = cls && runtimeClassName ? runtimeClassName(cls) : NULL;
    return name ? name : "(class name unavailable)";
}
static void dialogMethodInfo(Class cls, const char *selector) {
    Method method = cls ? getInstMethod(cls, selReg(selector)) : NULL;
    IMP imp = method ? getImp(method) : NULL;
    Dl_info info = {0};
    uintptr_t address = (uintptr_t)imp;
    int found = imp && dladdr(imp, &info);
    /* Clear ARM's Thumb tag for offsets usable in IDA, preserving the IMP. */
    uintptr_t code = address & ~(uintptr_t)1;
    (void)code; (void)found;
    LOGV("dialog: method class=%s selector=%s encoding=%s IMP=%p library=%s offset=0x%lx thumb=%u symbol=%s",
         dialogClassName(cls), selector, method ? getTypeEnc(method) : "(missing)", imp,
         found && info.dli_fname ? info.dli_fname : "(unavailable)",
         (unsigned long)(found && info.dli_fbase ? code - (uintptr_t)info.dli_fbase : 0),
         (unsigned int)(address & 1), found && info.dli_sname ? info.dli_sname : "(unnamed)");
}
static void logDialogTypeface(const char *source, id path, id result) {
    (void)source; (void)path; (void)result;
    if (!tableDialogFontScope) return;
    ++dialogTypefaceEvents;
    LOGV("dialog: typeface load source=%s pathOrName=%s result=%p class=%s",
         source, debugText(path), result, dialogClassName(result ? objectClass(result) : NULL));
}
static id my_dialogAssetTypeface(id self, SEL cmd, id manager, id path) {
    id result = ((id (*)(id, SEL, id, id))g_origDialogAssetTypeface)(self, cmd, manager, path);
    logDialogTypeface("asset", path, result);
    return result;
}
static id my_dialogFileTypeface(id self, SEL cmd, id path) {
    id result = ((id (*)(id, SEL, id))g_origDialogFileTypeface)(self, cmd, path);
    logDialogTypeface("file", path, result);
    return result;
}
static id my_dialogNamedTypeface(id self, SEL cmd, id name, int32_t style) {
    id result = ((id (*)(id, SEL, id, int32_t))g_origDialogNamedTypeface)(self, cmd, name, style);
    logDialogTypeface("name fallback", name, result);
    return result;
}
static id my_dialogFontProxy(id self, SEL cmd) {
    uint32_t before = dialogTypefaceEvents;
    (void)before;
    id result;
    if (tableDialogFontScope) installDialogTypefaceTrace();
    result = ((id (*)(id, SEL))g_origDialogFontProxy)(self, cmd);
    if (tableDialogFontScope)
        LOGV("dialog: font proxy font=%p name=%s result=%p class=%s loaderCalls=%u; zero means cached or untraced",
             self, debugText(diagnosticObject(self, "fontName")), result,
             dialogClassName(result ? objectClass(result) : NULL), dialogTypefaceEvents - before);
    return result;
}
static id my_dialogInit(id self, SEL cmd, id title, id message, id options) {
    id result;
    ++tableDialogFontScope;
    result = ((id (*)(id, SEL, id, id, id))g_origDialogInit)(self, cmd, title, message, options);
    --tableDialogFontScope;
    return result;
}
static id dialogSubviewAtIndex(id array, uint32_t index) {
    Method method = array ? getInstMethod(objectClass(array), selReg("objectAtIndex:")) : NULL;
    /* Some bridge collections encode their index as 64 bits even on ARM32.
     * Use that exact prototype here rather than changing the join path.
     */
    if ((signature(method, "@@:Q") || signature(method, "@@:q")) && getImp(method))
        return ((id (*)(id, SEL, uint64_t))getImp(method))(array, selReg("objectAtIndex:"), index);
    return objectAtIndex(array, index);
}
static id dialogTitleLabel(id button) {
    id label = diagnosticObject(button, "titleLabel");
    const char *names[] = {"_titleLabel", "titleLabel"};
    size_t i;
    if (label || !button) return label;
    /* The supplied getter reads an object ivar. If the getter's runtime
     * encoding is missing/unsupported, inspect that same retained field by
     * name and type, never by the supplied fixed 0x60 offset.
     */
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        Ivar iv = getIvar(objectClass(button), names[i]);
        ptrdiff_t offset = iv ? ivarOff(iv) : 0;
        const char *encoding = iv ? ivarEncoding(iv) : NULL;
        if (!encoding || !matchesType(encoding, '@') || offset <= 0 || offset >= 0x10000) continue;
        label = readObject(button, (uint32_t)offset);
        LOGV("dialog: titleLabel field class=%s name=%s offset=0x%lx encoding=%s value=%p",
             dialogClassName(objectClass(button)), names[i], (unsigned long)offset, encoding, label);
        if (label) return label;
    }
    return NULL;
}
static int dialogIsKindOf(id object, Class target) {
    Class cls = object ? objectClass(object) : NULL;
    unsigned int depth;
    if (!target || !runtimeSuperclass) return 0;
    for (depth = 0; cls && depth < 32; ++depth, cls = runtimeSuperclass(cls))
        if (cls == target) return 1;
    return 0;
}
static int dialogIsButton(id object) {
    return dialogIsKindOf(object, getClass("UIButton"));
}
/* This Android UIButton has an existing _titleView that renders its title;
 * titleLabel may be nil. Resolve the actual named UILabel and use its normal
 * font setter, preserving the platform conversion and Java typeface bridge.
 */
static id dialogTitleView(id button) {
    Ivar iv;
    const char *encoding;
    ptrdiff_t offset;
    id label;
    if (!dialogIsButton(button)) return NULL;
    /* The supplied setTitle:forState: uses _titleView (UILabel), while the
     * titleLabel getter reads a different, nil _titleLabel field. Bind the
     * actual title receiver by name and runtime type, never static 0x34.
     */
    iv = getIvar(objectClass(button), "_titleView");
    encoding = iv ? ivarEncoding(iv) : NULL;
    offset = iv ? ivarOff(iv) : 0;
    if (!encoding || strcmp(encoding, "@\"UILabel\"") || offset <= 0 ||
        offset >= 0x10000 || (size_t)offset % sizeof(id)) {
        LOGE("dialog: title-view font unavailable button=%p ivar=_titleView encoding=%s offset=0x%lx",
             button, encoding ? encoding : "(missing)", (unsigned long)offset);
        return NULL;
    }
    label = readObject(button, (uint32_t)offset);
    if (!dialogIsKindOf(label, getClass("UILabel"))) {
        LOGE("dialog: title-view font unavailable button=%p label=%p class=%s",
             button, label, dialogClassName(label ? objectClass(label) : NULL));
        return NULL;
    }
    LOGV("dialog: title-view field button=%p name=_titleView offset=0x%lx encoding=%s label=%p class=%s",
         button, (unsigned long)offset, encoding, label, dialogClassName(objectClass(label)));
    return label;
}
static int requestDialogButtonFont(id button) {
    Class fontClass = getClass("UIFont");
    id label = dialogTitleView(button), font, name, stored;
    Method factory, setter;
    if (!label || !fontClass) return 0;
    factory = getInstMethod(objectClass((id)fontClass), selReg("fontWithName:size:"));
    setter = getInstMethod(objectClass(label), selReg("setFont:"));
    if (!signature(factory, "@@:@f") || !signature(setter, "v@:@") ||
        !getImp(factory) || !getImp(setter)) {
        LOGE("dialog: title-view font unavailable factory=%s setter=%s",
             factory ? getTypeEnc(factory) : "(missing)", setter ? getTypeEnc(setter) : "(missing)");
        return 0;
    }
    name = string("BlockheadsFont-Regular");
    if (!name) return 0;
    font = ((id (*)(id, SEL, id, float))getImp(factory))(
        (id)fontClass, selReg("fontWithName:size:"), name, 16.0f);
    if (!font) return 0;
    /* Use the real label's normal setter, including native font ownership,
     * geometry conversion and its existing platform proxy. Do not create
     * labels, rewrite titles, or route this request to the UIButton proxy.
     */
    ((void (*)(id, SEL, id))getImp(setter))(label, selReg("setFont:"), font);
    stored = diagnosticObject(label, "font");
    (void)stored;
    LOGV("dialog: requested native title-view font button=%p label=%p text=%s fontName=%s size=16 storedFont=%p; rendered result unverified",
         button, label, debugText(diagnosticObject(label, "text")),
         debugText(diagnosticObject(stored, "fontName")), stored);
    return 1;
}
static int repairDialogButtonFont(id button) {
    int result;
    ++tableDialogFontScope;
    result = requestDialogButtonFont(button);
    --tableDialogFontScope;
    return result;
}
static void repairDialogTableBackground(id self) {
    id table = offDialogTable ? readObject(self, offDialogTable) : NULL;
    id background = diagnosticObject(table, "backgroundView");
    Method bounds = table ? getInstMethod(objectClass(table), selReg("bounds")) : NULL;
    Method setter = background ? getInstMethod(objectClass(background), selReg("setFrame:")) : NULL;
    Rect frame;
    if (!background || !signature(bounds, "R@:") || !signature(setter, "v@:R") ||
        !getImp(bounds) || !getImp(setter)) return;
    frame = ((Rect (*)(id, SEL))getImp(bounds))(table, selReg("bounds"));
    if (!(frame.x > -10000 && frame.x < 10000 && frame.y > -10000 && frame.y < 10000 &&
          frame.w > 0 && frame.w < 2000 && frame.h > 0 && frame.h < 2000)) return;
    /* setBackgroundView: inserts this view into the table, but the supplied
     * implementation initializes it using the table's frame in its parent.
     * Use the table's own bounds once the dialog's layout is complete.
     */
    ((void (*)(id, SEL, Rect))getImp(setter))(background, selReg("setFrame:"), frame);
    LOGV("dialog: corrected table background frame table=%p background=%p bounds=%g,%g,%g,%g",
         table, background, frame.x, frame.y, frame.w, frame.h);
}
static void inspectDialogCellObject(const char *role, id object) {
    (void)role;
    Class cls = object ? objectClass(object) : NULL;
    Method getter = cls ? getInstMethod(cls, selReg("_proxy")) : NULL;
    Method frameMethod = cls ? getInstMethod(cls, selReg("frame")) : NULL;
    int proxySupported = signature(getter, "@@:") && getImp(getter);
    id proxy = proxySupported ? ((id (*)(id, SEL))getImp(getter))(object, selReg("_proxy")) : NULL;
    (void)proxy;
    LOGV("dialog: cell factory role=%s object=%p class=%s proxyGetter=%s proxy=%p proxyClass=%s",
         role, object, dialogClassName(cls),
         !getter ? "missing" : proxySupported ? "supported" : "unsupported",
         proxy, dialogClassName(proxy ? objectClass(proxy) : NULL));
    if (!object) return;
    dialogMethodInfo(cls, "_proxy");
    dialogMethodInfo(cls, "setFrame:");
    if (signature(frameMethod, "R@:") && getImp(frameMethod)) {
        Rect frame = ((Rect (*)(id, SEL))getImp(frameMethod))(object, selReg("frame"));
        (void)frame;
        LOGV("dialog: cell factory role=%s frame=%g,%g,%g,%g; before subsequent table layout",
             role, frame.x, frame.y, frame.w, frame.h);
    } else LOGV("dialog: cell factory role=%s frameEncoding=%s; frame unread",
                role, frameMethod ? getTypeEnc(frameMethod) : "(missing)");
}
static id dialogCellObjectGetter(id object, const char *selector) {
    Method getter = object ? getInstMethod(objectClass(object), selReg(selector)) : NULL;
    return signature(getter, "@@:") && getImp(getter) ?
        ((id (*)(id, SEL))getImp(getter))(object, selReg(selector)) : NULL;
}
static void repairDialogCellInset(id cell) {
    id label = dialogCellObjectGetter(cell, "textLabel");
    Method bounds = cell ? getInstMethod(objectClass(cell), selReg("bounds")) : NULL;
    Method setter = label ? getInstMethod(objectClass(label), selReg("setFrame:")) : NULL;
    Method mask = label ? getInstMethod(objectClass(label), selReg("autoresizingMask")) : NULL;
    Rect frame;
    if (!label || !signature(bounds, "R@:") || !signature(setter, "v@:R") ||
        !(signature(mask, "I@:") || signature(mask, "i@:") ||
          signature(mask, "L@:") || signature(mask, "l@:")) ||
        !getImp(bounds) || !getImp(setter) || !getImp(mask)) return;
    /* The supplied constructor creates a full-cell label with mask 18.
     * Keep the fixed side margins through later cell-width changes by
     * relying on its existing flexible-width/flexible-height mask only.
     */
    if (((uint32_t (*)(id, SEL))getImp(mask))(label, selReg("autoresizingMask")) != 18) return;
    frame = ((Rect (*)(id, SEL))getImp(bounds))(cell, selReg("bounds"));
    if (!(frame.x > -10000 && frame.x < 10000 && frame.y > -10000 && frame.y < 10000 &&
          frame.w > 30 && frame.w < 2000 && frame.h > 0 && frame.h < 2000)) return;
    frame.x += 15; frame.w -= 30;
    ((void (*)(id, SEL, Rect))getImp(setter))(label, selReg("setFrame:"), frame);
    LOGV("dialog: inset row label cell=%p label=%p frame=%g,%g,%g,%g; native autoresizing retained",
         cell, label, frame.x, frame.y, frame.w, frame.h);
}
static id my_dialogCell(id self, SEL cmd, id table, id indexPath) {
    id cell;
    ++tableDialogFontScope;
    cell = ((id (*)(id, SEL, id, id))g_origDialogCell)(self, cmd, table, indexPath);
    --tableDialogFontScope;
    /* Bound diagnostic output without dropping or replacing any callbacks.
     * Every cell and all borrowed getters remain synchronous; no raw cell
     * pointer is stored for a later layout pass.
     */
    if (__sync_fetch_and_add(&dialogCellTraceCount, 1) < 8) {
        LOGV("dialog: cell factory returned dialog=%p table=%p indexPath=%p cell=%p", self, table, indexPath, cell);
        inspectDialogCellObject("cell", cell);
        inspectDialogCellObject("textLabel", dialogCellObjectGetter(cell, "textLabel"));
        inspectDialogCellObject("contentView", dialogCellObjectGetter(cell, "contentView"));
    }
    repairDialogCellInset(cell);
    return cell;
}
static void inspectDialogFonts(id self) {
    id view = offDialogView ? readObject(self, offDialogView) : NULL;
    id subviews = diagnosticObject(view, "subviews");
    int32_t count = diagnosticNumber(subviews, "count");
    int32_t i;
    LOGV("dialog: shown class=%s view=%p subviews=%d",
         dialogClassName(objectClass(self)), view, count);
    dialogMethodInfo(subviews ? objectClass(subviews) : NULL, "objectAtIndex:");
    dialogMethodInfo(getClass("UIButton"), "titleLabel");
    dialogMethodInfo(getClass("UILabel"), "_platform_setFont:actualFontSize:");
    /* Resolve unnamed image bridge implementations through runtime metadata.
     * Inspection only: do not invoke platform setters or create image proxies.
     */
    dialogMethodInfo(getClass("UIImageView"), "_platform_setImage:");
    dialogMethodInfo(getClass("UIImage"), "_proxy");
    dialogMethodInfo(getClass("UIStretchableImage"), "_proxy");
    dialogMethodInfo(getClass("UIStretchableImage"), "drawInRect:blendMode:alpha:");
    dialogMethodInfo(getClass("UIStretchableImage"), "_androidBitmap:");
    dialogMethodInfo(getClass("UIStretchableImage"), "_androidBitmap");
    dialogMethodInfo(getClass("UIImage"), "_androidBitmap:");
    dialogMethodInfo(getClass("UIImage"), "_androidBitmap");
    for (i = 0; i < count && i < 64; ++i) {
        id button = dialogSubviewAtIndex(subviews, (uint32_t)i);
        id label = dialogTitleLabel(button);
        Class cls = button ? objectClass(button) : NULL;
        Method getter = cls ? getInstMethod(cls, selReg("titleLabel")) : NULL;
        (void)getter;
        id font, proxy, buttonProxy;
        LOGV("dialog: child index=%d object=%p class=%s titleLabelEncoding=%s label=%p",
             i, button, dialogClassName(cls), getter ? getTypeEnc(getter) : "(missing)", label);
        if (dialogIsButton(button)) repairDialogButtonFont(button);
        if (!label) continue;
        font = diagnosticObject(label, "font");
        (void)font;
        proxy = diagnosticObject(label, "_proxy");
        buttonProxy = diagnosticObject(button, "_proxy");
        LOGV("dialog: button=%p class=%s label=%p labelClass=%s text=%s font=%s fontName=%s labelProxy=%p buttonProxy=%p",
             button, dialogClassName(objectClass(button)), label, dialogClassName(objectClass(label)),
             debugText(diagnosticObject(label, "text")), debugText(font),
             debugText(diagnosticObject(font, "fontName")), proxy, buttonProxy);
        dialogMethodInfo(objectClass(button), "titleLabel");
        dialogMethodInfo(objectClass(button), "setTitle:forState:");
        dialogMethodInfo(objectClass(label), "setFont:");
        dialogMethodInfo(objectClass(label), "_platform_setFont:actualFontSize:");
        dialogMethodInfo(objectClass(label), "_platform_setFont:");
        dialogMethodInfo(objectClass(label), "forwardInvocation:");
        dialogMethodInfo(objectClass(label), "methodSignatureForSelector:");
        if (proxy) {
            dialogMethodInfo(objectClass(proxy), "setFont:size:");
            dialogMethodInfo(objectClass(proxy), "setFont:actualFontSize:");
        }
        if (buttonProxy) {
            dialogMethodInfo(objectClass(buttonProxy), "setFont:size:");
            dialogMethodInfo(objectClass(buttonProxy), "setFont:actualFontSize:");
        }
    }
}
static void my_dialogShowVoid(id self, SEL cmd) {
    ((void (*)(id, SEL))g_origDialogShow)(self, cmd);
    repairDialogTableBackground(self);
    inspectDialogFonts(self);
}
static id my_dialogShowObject(id self, SEL cmd) {
    id result = ((id (*)(id, SEL))g_origDialogShow)(self, cmd);
    repairDialogTableBackground(self);
    inspectDialogFonts(self);
    return result;
}
static void my_dialogInitializeVoid(id self, SEL cmd) {
    unsigned int previous = tableDialogImageScope;
    if (self == (id)tableDialogClass) ++tableDialogImageScope;
    ((void (*)(id, SEL))g_origDialogInitialize)(self, cmd);
    tableDialogImageScope = previous;
}
static id my_dialogInitializeObject(id self, SEL cmd) {
    unsigned int previous = tableDialogImageScope;
    id result;
    if (self == (id)tableDialogClass) ++tableDialogImageScope;
    result = ((id (*)(id, SEL))g_origDialogInitialize)(self, cmd);
    tableDialogImageScope = previous;
    return result;
}
/* Match BlockAlertView's four 2-point caps on the shared alert-window image.
 * The factory wrapper is active only inside this dialog's +initialize, so
 * ordinary image creation and table-row background caps retain native values.
 * Bitmap rendering keeps these point caps separate from image pixel scale.
 */
static id my_dialogStretch(id self, SEL cmd, int32_t left, int32_t top) {
    id native = ((id (*)(id, SEL, int32_t, int32_t))g_origDialogStretch)(self, cmd, left, top);
    Method sizeMethod, imageMethod, scaleMethod, initMethod, allocMethod, autoreleaseMethod;
    Class factory;
    DialogSize size;
    DialogInsets insets;
    float scale;
    void *image;
    id allocated, corrected;
    unsigned int previous;
    if (!tableDialogImageScope || left != 0 || top != 38 || !native || !self) return native;
    factory = objectClass(native);
    sizeMethod = getInstMethod(objectClass(self), selReg("size"));
    imageMethod = getInstMethod(objectClass(self), selReg("CGImage"));
    scaleMethod = getInstMethod(objectClass(self), selReg("scale"));
    initMethod = getInstMethod(factory, selReg("initWithCGImage:insets:scale:"));
    allocMethod = getInstMethod(objectClass((id)factory), selReg("alloc"));
    autoreleaseMethod = getInstMethod(factory, selReg("autorelease"));
    if (!signature(sizeMethod, "Z@:") || !signature(imageMethod, "p@:") ||
        !signature(scaleMethod, "f@:") || !signature(initMethod, "@@:pRf") ||
        !signature(allocMethod, "@@:") || !signature(autoreleaseMethod, "@@:") ||
        !getImp(sizeMethod) || !getImp(imageMethod) || !getImp(scaleMethod) ||
        !getImp(initMethod) || !getImp(allocMethod) || !getImp(autoreleaseMethod)) {
        LOGE("dialog: panel correction unavailable factory=%s size=%s init=%s; native image retained",
             dialogClassName(factory), sizeMethod ? getTypeEnc(sizeMethod) : "(missing)",
             initMethod ? getTypeEnc(initMethod) : "(missing)");
        return native;
    }
    size = ((DialogSize (*)(id, SEL))getImp(sizeMethod))(self, selReg("size"));
    scale = ((float (*)(id, SEL))getImp(scaleMethod))(self, selReg("scale"));
    image = ((void *(*)(id, SEL))getImp(imageMethod))(self, selReg("CGImage"));
    /* Comparisons also reject NaN, infinities and impossible cap geometry. */
    if (!(size.width > 4 && size.width <= 8192 && size.height > 4 && size.height <= 8192 &&
          scale > 0 && scale <= 16) || !image) return native;
    /* The supplied BlockAlertView +initialize uses alert-window.png with
     * resizableImageWithCapInsets:{2,2,2,2}. That UIImage factory calls this
     * same native initializer, preserving CGImage and scale. Match all four
     * caps rather than keeping the table dialog's legacy 38-point top cap.
     * Leave the native Android bitmap renderer responsible for rasterization.
     */
    insets = (DialogInsets){2, 2, 2, 2};
    previous = tableDialogImageScope; tableDialogImageScope = 0;
    allocated = ((id (*)(id, SEL))getImp(allocMethod))((id)factory, selReg("alloc"));
    corrected = allocated ? ((id (*)(id, SEL, void *, DialogInsets, float))getImp(initMethod))(
        allocated, selReg("initWithCGImage:insets:scale:"), image, insets, scale) : NULL;
    if (corrected) corrected = ((id (*)(id, SEL))getImp(autoreleaseMethod))(corrected, selReg("autorelease"));
    tableDialogImageScope = previous;
    if (!corrected) return native;
    LOGV("dialog: matched BlockAlertView panel caps factory=%s size=%gx%g scale=%g insets=%g,%g,%g,%g",
         dialogClassName(factory), size.width, size.height, scale,
         insets.top, insets.left, insets.bottom, insets.right);
    return corrected;
}


static jstring replaceNameText(JNIEnv *env, jmethodID replace, jstring value,
                              const char *pattern, const char *replacement) {
    jstring expression = (*env)->NewStringUTF(env, pattern), substitute;
    if (!expression || (*env)->ExceptionCheck(env)) return NULL;
    substitute = (*env)->NewStringUTF(env, replacement);
    if (!substitute || (*env)->ExceptionCheck(env)) return NULL;
    return (jstring)(*env)->CallObjectMethod(env, value, replace, expression, substitute);
}
static int readDeviceHostnameJNI(JNIEnv *env) {
    jobject activity, resolver, form;
    jclass settings, stringClass, normalizer, formClass, idnClass, build;
    jmethodID getter, replace, normalize, toASCII;
    jfieldID field;
    jstring key, name, normalized, ascii;
    const char *bytes, *stage = "Context";
    int result = 0;
    const char *source = "device_name";
    if ((*env)->ExceptionCheck(env)) return 0;
    if ((*env)->PushLocalFrame(env, 40) != JNI_OK) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        LOGE("mDNS: device-name JNI frame unavailable; original hostname retained"); return 0;
    }
    activity = (*env)->CallStaticObjectMethod(env, hostnameActivityClass, hostnameActivityGetter);
    if (!activity || (*env)->ExceptionCheck(env)) goto failed;
    getter = (*env)->GetMethodID(env, hostnameActivityClass, "getContentResolver", "()Landroid/content/ContentResolver;");
    if (!getter || (*env)->ExceptionCheck(env)) goto failed;
    resolver = (*env)->CallObjectMethod(env, activity, getter);
    if (!resolver || (*env)->ExceptionCheck(env)) goto failed;
    stage = "Settings.Global device_name";
    settings = (*env)->FindClass(env, "android/provider/Settings$Global");
    if (!settings || (*env)->ExceptionCheck(env)) goto failed;
    getter = (*env)->GetStaticMethodID(env, settings, "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
    if (!getter || (*env)->ExceptionCheck(env)) goto failed;
    key = (*env)->NewStringUTF(env, "device_name");
    if (!key || (*env)->ExceptionCheck(env)) goto failed;
    name = (jstring)(*env)->CallStaticObjectMethod(env, settings, getter, resolver, key);
    if ((*env)->ExceptionCheck(env)) goto failed;
    if (!name || !(*env)->GetStringLength(env, name)) {
        /* Match Android Settings' default when no name has been configured. */
        stage = "Build.MODEL"; source = "Build.MODEL";
        build = (*env)->FindClass(env, "android/os/Build");
        if (!build || (*env)->ExceptionCheck(env)) goto failed;
        field = (*env)->GetStaticFieldID(env, build, "MODEL", "Ljava/lang/String;");
        if (!field || (*env)->ExceptionCheck(env)) goto failed;
        name = (jstring)(*env)->GetStaticObjectField(env, build, field);
        if (!name || (*env)->ExceptionCheck(env)) goto failed;
    }
    stage = "device-name normalization";
    stringClass = (*env)->FindClass(env, "java/lang/String");
    if (!stringClass || (*env)->ExceptionCheck(env)) goto failed;
    replace = (*env)->GetMethodID(env, stringClass, "replaceAll", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!replace || (*env)->ExceptionCheck(env)) goto failed;
    normalizer = (*env)->FindClass(env, "java/text/Normalizer");
    if (!normalizer || (*env)->ExceptionCheck(env)) goto failed;
    formClass = (*env)->FindClass(env, "java/text/Normalizer$Form");
    if (!formClass || (*env)->ExceptionCheck(env)) goto failed;
    field = (*env)->GetStaticFieldID(env, formClass, "NFKD", "Ljava/text/Normalizer$Form;");
    if (!field || (*env)->ExceptionCheck(env)) goto failed;
    form = (*env)->GetStaticObjectField(env, formClass, field);
    if (!form || (*env)->ExceptionCheck(env)) goto failed;
    normalize = (*env)->GetStaticMethodID(env, normalizer, "normalize", "(Ljava/lang/CharSequence;Ljava/text/Normalizer$Form;)Ljava/lang/String;");
    if (!normalize || (*env)->ExceptionCheck(env)) goto failed;
    normalized = (jstring)(*env)->CallStaticObjectMethod(env, normalizer, normalize, name, form);
    if (!normalized || (*env)->ExceptionCheck(env)) goto failed;
    /* Fold accents, map separator runs to hyphens and remove edge hyphens.
     * Keep non-Latin letters; IDN supplies a deterministic ASCII encoding
     * instead of dropping the user's entire name or inventing an identity.
     */
    normalized = replaceNameText(env, replace, normalized, "\\p{M}+", "");
    if (!normalized || (*env)->ExceptionCheck(env)) goto failed;
    normalized = replaceNameText(env, replace, normalized, "[^\\p{L}\\p{N}-]+", "-");
    if (!normalized || (*env)->ExceptionCheck(env)) goto failed;
    normalized = replaceNameText(env, replace, normalized, "^-+|-+$", "");
    if (!normalized || (*env)->ExceptionCheck(env) || !(*env)->GetStringLength(env, normalized)) goto failed;
    stage = "device-name IDN conversion";
    idnClass = (*env)->FindClass(env, "java/net/IDN");
    if (!idnClass || (*env)->ExceptionCheck(env)) goto failed;
    toASCII = (*env)->GetStaticMethodID(env, idnClass, "toASCII", "(Ljava/lang/String;I)Ljava/lang/String;");
    if (!toASCII || (*env)->ExceptionCheck(env)) goto failed;
    ascii = (jstring)(*env)->CallStaticObjectMethod(env, idnClass, toASCII, normalized, (jint)2); /* USE_STD3_ASCII_RULES */
    if (!ascii || (*env)->ExceptionCheck(env)) goto failed;
    bytes = (*env)->GetStringUTFChars(env, ascii, NULL);
    if (!bytes || (*env)->ExceptionCheck(env)) goto failed;
    result = publishDeviceHostname(bytes);
    (*env)->ReleaseStringUTFChars(env, ascii, bytes);
    if (!result) goto failed;
    LOGI("mDNS: Android hostname source=%s base=%s.local.; fresh daemon startup required", source, deviceHostname);
    (*env)->PopLocalFrame(env, NULL);
    return 1;
failed:
    /* This optional read never leaves a new Java exception pending. */
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    LOGE("mDNS: Android device name unavailable at %s; original hostname retained", stage);
    return 0;
}
static void prepareDeviceHostnameJNI(void) {
    JNIEnv *env = NULL;
    int attached = 0;
    jint status;
    if (!__atomic_load_n(&hostnameJNIReady, __ATOMIC_ACQUIRE)) return;
    status = (*hostnameVM)->GetEnv(hostnameVM, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if ((*hostnameVM)->AttachCurrentThreadAsDaemon(hostnameVM, (void *)&env, NULL) != JNI_OK) return;
        attached = 1;
    } else if (status != JNI_OK) return;
    readDeviceHostnameJNI(env);
    if (attached) (*hostnameVM)->DetachCurrentThread(hostnameVM);
}
static void initializeDeviceHostnameJNI(JavaVM *vm, JNIEnv *env) {
    jclass local, global;
    jmethodID getter;
    if ((*env)->ExceptionCheck(env)) return;
    local = (*env)->FindClass(env, "com/apportable/activity/VerdeActivity");
    if (!local || (*env)->ExceptionCheck(env)) goto failed;
    getter = (*env)->GetStaticMethodID(env, local, "getActivity", "()Lcom/apportable/activity/VerdeActivity;");
    if (!getter || (*env)->ExceptionCheck(env)) { (*env)->DeleteLocalRef(env, local); goto failed; }
    global = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!global || (*env)->ExceptionCheck(env)) goto failed;
    hostnameVM = vm; hostnameActivityClass = global; hostnameActivityGetter = getter;
    __atomic_store_n(&hostnameJNIReady, 1, __ATOMIC_RELEASE);
    prepareDeviceHostnameJNI();
    return;
failed:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    LOGE("mDNS: verified VerdeActivity Context getter unavailable; original hostname retained");
}

/* Observation only: the original Java request still queues the original
 * notification, and GameView still executes its entire background handler.
 * Last-request timing is not a per-notification identity: the native bridge
 * queues a shared global block, and several requests can be pending.
 */
static void logLifecycleState(const char *stage, JNIEnv *env) {
    (void)stage;
    int flags[6], i;
    unsigned long sequence;
    struct timespec requested, now;
    long long age = -1;
    pthread_mutex_lock(&lifecycleRequestLock);
    sequence = lifecycleRequestSequence;
    requested = lifecycleLastRequest;
    pthread_mutex_unlock(&lifecycleRequestLock);
    if (sequence && clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        age = (long long)(now.tv_sec - requested.tv_sec) * 1000 +
              (now.tv_nsec - requested.tv_nsec) / 1000000;
    if (!__atomic_load_n(&lifecycleProbeReady, __ATOMIC_ACQUIRE)) {
        LOGV("hosting lifecycle: %s Java flags unavailable; JNI probe not initialized", stage); return;
    }
    if (!env && (*lifecycleVM)->GetEnv(lifecycleVM, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        LOGV("hosting lifecycle: %s Java flags unavailable; thread detached", stage); return;
    }
    if ((*env)->ExceptionCheck(env)) {
        LOGV("hosting lifecycle: %s Java flags skipped; existing exception retained", stage); return;
    }
    for (i = 0; i < 6; ++i) {
        flags[i] = (*env)->GetStaticBooleanField(env, lifecycleClass, lifecycleFields[i]) != JNI_FALSE;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            LOGV("hosting lifecycle: %s Java field read unavailable", stage); return;
        }
    }
    (void)flags; (void)age;
    LOGV("hosting lifecycle: %s nativeBackground=%d windowFocus=%d activityPaused=%d activityWasStopped=%d contextSurface=%d nativeStarted=%d lastRequest=%lu ageMs=%lld",
         stage, flags[0], flags[1], flags[2], flags[3], flags[4], flags[5], sequence, age);
}
static void logLifecycleJavaStack(JNIEnv *env) {
    jclass throwable = NULL, logClass = NULL;
    jmethodID initializer, stackMethod;
    jobject exception;
    jstring stack;
    const char *text, *line, *end;
    if ((*env)->ExceptionCheck(env)) return;
    if ((*env)->PushLocalFrame(env, 8) != JNI_OK) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        LOGV("hosting lifecycle: Java request stack unavailable"); return;
    }
    throwable = (*env)->FindClass(env, "java/lang/Throwable");
    if (!throwable || (*env)->ExceptionCheck(env)) goto failed;
    initializer = (*env)->GetMethodID(env, throwable, "<init>", "()V");
    if (!initializer || (*env)->ExceptionCheck(env)) goto failed;
    exception = (*env)->NewObject(env, throwable, initializer);
    if (!exception || (*env)->ExceptionCheck(env)) goto failed;
    logClass = (*env)->FindClass(env, "android/util/Log");
    if (!logClass || (*env)->ExceptionCheck(env)) goto failed;
    stackMethod = (*env)->GetStaticMethodID(env, logClass, "getStackTraceString", "(Ljava/lang/Throwable;)Ljava/lang/String;");
    if (!stackMethod || (*env)->ExceptionCheck(env)) goto failed;
    stack = (jstring)(*env)->CallStaticObjectMethod(env, logClass, stackMethod, exception);
    if (!stack || (*env)->ExceptionCheck(env)) goto failed;
    text = (*env)->GetStringUTFChars(env, stack, NULL);
    if (!text || (*env)->ExceptionCheck(env)) goto failed;
    for (line = text; *line; line = *end ? end + 1 : end) {
        end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        LOGV("hosting lifecycle: Java request stack: %.*s", (int)(end - line), line);
    }
    (*env)->ReleaseStringUTFChars(env, stack, text);
    (*env)->PopLocalFrame(env, NULL);
    return;
failed:
    /* Only exceptions raised by this optional diagnostic are cleared. */
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    LOGV("hosting lifecycle: Java request stack unavailable");
}
static void JNICALL my_javaBackground(JNIEnv *env, jclass cls) {
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&lifecycleRequestLock);
    ++lifecycleRequestSequence;
    lifecycleLastRequest = now;
    pthread_mutex_unlock(&lifecycleRequestLock);
    logLifecycleState("Java background request", env);
    logLifecycleJavaStack(env);
    g_origJavaBackground(env, cls);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;
    jclass localClass, globalClass;
    jfieldID fields[6];
    int i, installed = 0;
    JNINativeMethod method = {
        "applicationDidEnterBackground", "()V", (void *)my_javaBackground
    };
    (void)reserved;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || (*env)->ExceptionCheck(env))
        return JNI_VERSION_1_6;
    initializeDeviceHostnameJNI(vm, env);
    /* JNI_OnLoad uses the loader's class-loader context. Native callback
     * threads reuse this global reference rather than calling FindClass.
     */
    localClass = (*env)->FindClass(env, "com/apportable/Lifecycle");
    if (!localClass || (*env)->ExceptionCheck(env)) goto unavailable;
    for (i = 0; i < 6; ++i) {
        fields[i] = (*env)->GetStaticFieldID(env, localClass, lifecycleFieldNames[i], "Z");
        if (!fields[i] || (*env)->ExceptionCheck(env)) {
            (*env)->DeleteLocalRef(env, localClass); goto unavailable;
        }
    }
    globalClass = (jclass)(*env)->NewGlobalRef(env, localClass);
    (*env)->DeleteLocalRef(env, localClass);
    if (!globalClass || (*env)->ExceptionCheck(env)) goto unavailable;
    lifecycleVM = vm;
    lifecycleClass = globalClass;
    memcpy(lifecycleFields, fields, sizeof(fields));
    __atomic_store_n(&lifecycleProbeReady, 1, __ATOMIC_RELEASE);
    g_origJavaBackground = g_app ? (void (*)(JNIEnv *, jclass))dlsym(
        g_app, "Java_com_apportable_Lifecycle_applicationDidEnterBackground") : NULL;
    if (g_origJavaBackground) {
        jmethodID nativeMethod = (*env)->GetStaticMethodID(env, globalClass, method.name, method.signature);
        if (nativeMethod && !(*env)->ExceptionCheck(env))
            installed = (*env)->RegisterNatives(env, globalClass, &method, 1) == JNI_OK;
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    (void)installed;
    LOGV("hosting lifecycle JNI diagnostic installed: fields=1 requestHook=%d; original native request forwarded", installed);
    return JNI_VERSION_1_6;
unavailable:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    LOGI("hosting lifecycle JNI diagnostic unavailable; original native binding retained");
    return JNI_VERSION_1_6;
}

#if LANHOSTFIX_VERBOSE
static const char *hostingClassName(id object) {
    const char *name = object && runtimeClassName ? runtimeClassName(objectClass(object)) : NULL;
    return name ? name : (object ? "(class unavailable)" : "(nil)");
}
static int hostingBoolean(id object, const char *selector) {
    Method method = object ? getInstMethod(objectClass(object), selReg(selector)) : NULL;
    if (!signature(method, "c@:") && !signature(method, "B@:")) return -1;
    return ((unsigned char (*)(id, SEL))getImp(method))(object, selReg(selector)) != 0;
}
#endif
static int optionalHostingOffset(Class cls, const char *name, char type, uint32_t *offset) {
    Ivar iv = cls ? getIvar(cls, name) : NULL;
    const char *encoding = iv ? ivarEncoding(iv) : NULL;
    ptrdiff_t value = iv ? ivarOff(iv) : 0;
    if (!encoding || value <= 0 || value >= 0x10000 ||
        (!matchesType(encoding, type) && !(type == 'i' && matchesType(encoding, 'I')))) return 0;
    *offset = (uint32_t)value;
    return 1;
}
static void logHostingCaller(const char *stage, void *address) {
    (void)stage;
    Dl_info info;
    uintptr_t caller = (uintptr_t)address & ~(uintptr_t)1;
    if (dladdr((void *)caller, &info) && info.dli_fbase)
        LOGV("hosting: %s caller library=%s offset=0x%lx symbol=%s", stage,
             info.dli_fname ? info.dli_fname : "(unknown)",
             (unsigned long)(caller - (uintptr_t)info.dli_fbase),
             info.dli_sname ? info.dli_sname : "(unknown)");
    else LOGV("hosting: %s caller=%p", stage, address);
}
static void logHostingMatch(const char *stage, id match) {
    (void)stage;
    id timer, service, connections;
    void *enet, *tcp;
    int valid, native;
    if (!match || objectClass(match) != getClass("BHNetServerMatch")) {
        LOGV("hosting: %s match=%p class=%s", stage, match, hostingClassName(match)); return;
    }
    enet = offEnet ? readObject(match, offEnet) : NULL;
    tcp = offTcp ? readObject(match, offTcp) : NULL;
    timer = offServerTimer ? readObject(match, offServerTimer) : NULL;
    service = offServerService ? readObject(match, offServerService) : NULL;
    connections = offServerConnections ? readObject(match, offServerConnections) : NULL;
    valid = tcp && hostingSocketValid ? hostingSocketValid(tcp) != 0 : -1;
    native = tcp && hostingSocketNative && valid == 1 ? hostingSocketNative(tcp) : -1;
    (void)enet; (void)timer; (void)service; (void)connections; (void)native;
    LOGV("hosting: %s match=%p ENet=%p TCP=%p TCPvalid=%d TCPfd=%d pollTimer=%p timerValid=%d connections=%d service=%p servicePort=%d serviceName=%s",
         stage, match, enet, tcp, valid, native, timer, hostingBoolean(timer, "isValid"),
         (int)diagnosticNumber(connections, "count"), service,
         (int)diagnosticNumber(service, "port"), serviceName(service));
}
static void logHostingNode(const char *stage, id node) {
    id match = node && offServerMatch ? readObject(node, offServerMatch) : NULL;
    int32_t type = node && offServerNodeType ? *(int32_t *)((char *)node + offServerNodeType) : -1;
    int32_t players = node && offServerMaxPlayers ? *(int32_t *)((char *)node + offServerMaxPlayers) : -1;
    (void)type; (void)players;
    LOGV("hosting: %s node=%p class=%s match=%p matchClass=%s nodeType=%d maxPlayers=%d",
         stage, node, hostingClassName(node), match, hostingClassName(match), (int)type, (int)players);
    if (node) LOGV("hosting: predicates (-1=unavailable) node.isMultiplayer=%d node.isSinglePlayer=%d node.isCloudMatch=%d match.isLocal=%d match.isLocalMatch=%d",
         hostingBoolean(node, "isMultiplayer"), hostingBoolean(node, "isSinglePlayer"),
         hostingBoolean(node, "isCloudMatch"), hostingBoolean(match, "isLocal"), hostingBoolean(match, "isLocalMatch"));
    logHostingMatch(stage, match);
}
static void my_hostingSnapshot(id self, SEL cmd, id label) {
    (void)label;
    (void)cmd;
    LOGV("hosting: delayed snapshot game=%p time=%s", self, debugText(label));
    logHostingNode("delayed active bhServer", offGameServer ? readObject(self, offGameServer) : NULL);
}
static void my_gameBackground(id self, SEL cmd) {
    Class applicationClass = getClass("UIApplication");
    id application = applicationClass ? diagnosticObject((id)applicationClass, "sharedApplication") : NULL;
    (void)application;
    logHostingCaller("GameView didEnterBackground", __builtin_return_address(0));
    logLifecycleState("GameView background delivery", NULL);
    LOGV("hosting lifecycle: GameView background delivery game=%p applicationState=%d (-1=unavailable)",
         self, (int)diagnosticNumber(application, "applicationState"));
    logHostingNode("before native background handler", offGameServer ? readObject(self, offGameServer) : NULL);
    forwardGameBackground(self, cmd);
    logHostingNode("after native background handler", offGameServer ? readObject(self, offGameServer) : NULL);
}
static void my_serverCleanup(id self, SEL cmd) {
    logHostingCaller("BHServer cleanup", __builtin_return_address(0));
    logHostingNode("BHServer cleanup entered", self);
    ((void (*)(id, SEL))g_origServerCleanup)(self, cmd);
    logHostingNode("BHServer cleanup returned", self);
}
static void my_serverDisconnect(id self, SEL cmd) {
    logHostingCaller("BHNetServerMatch disconnect", __builtin_return_address(0));
    logHostingMatch("disconnect entered", self);
    ((void (*)(id, SEL))g_origServerDisconnect)(self, cmd);
    logHostingMatch("disconnect returned", self);
}
static void my_serverDealloc(id self, SEL cmd) {
    logHostingCaller("BHNetServerMatch dealloc", __builtin_return_address(0));
    logHostingMatch("dealloc entered", self);
    ((void (*)(id, SEL))g_origServerDealloc)(self, cmd);
    /* self is freed: do not inspect after the original dealloc. */
}
static void my_serverPublish(id self, SEL cmd, id service) {
    LOGV("hosting: DNS-SD service published name=%s port=%d", serviceName(service),
         (int)diagnosticNumber(service, "port"));
    if (g_origServerPublish) ((void (*)(id, SEL, id))g_origServerPublish)(self, cmd, service);
}
static void my_serverPublishError(id self, SEL cmd, id service, id error) {
    LOGE("hosting: DNS-SD publish failed name=%s error=%s", serviceName(service), debugText(error));
    if (g_origServerPublishError) ((void (*)(id, SEL, id, id))g_origServerPublishError)(self, cmd, service, error);
}
static id my_serverInit(id self, SEL cmd, id delegate, id match, int32_t type, id saveID, int32_t players) {
    id result;
    LOGV("hosting: BHServer init entered self=%p delegate=%p delegateClass=%s suppliedMatch=%p matchClass=%s nodeType=%d maxPlayers=%d",
         self, delegate, hostingClassName(delegate), match, hostingClassName(match), (int)type, (int)players);
    result = ((id (*)(id, SEL, id, id, int32_t, id, int32_t))g_origServerInit)(
        self, cmd, delegate, match, type, saveID, players);
    logHostingNode("BHServer init returned", result);
    return result;
}
static void my_gameHosting(id self, SEL cmd, id userName, id photo) {
    id node = offGameServer ? readObject(self, offGameServer) : NULL;
    LOGV("hosting: GameView hosting entered game=%p previousServer=%p previousClient=%p",
         self, node, offGameClient ? readObject(self, offGameClient) : NULL);
    logLifecycleState("GameView hosting entry", NULL);
    ((void (*)(id, SEL, id, id))g_origGameHosting)(self, cmd, userName, photo);
    node = offGameServer ? readObject(self, offGameServer) : NULL;
    logHostingNode("GameView hosting returned; active bhServer", node);
    LOGV("hosting: GameView predicates (-1=unavailable) isMultiplayer=%d isSinglePlayer=%d activeClient=%p",
         hostingBoolean(self, "isMultiplayer"), hostingBoolean(self, "isSinglePlayer"),
         offGameClient ? readObject(self, offGameClient) : NULL);
    if (hostingSnapshotsInstalled) {
        Method schedule = getInstMethod(objectClass(self), selReg("performSelector:withObject:afterDelay:"));
        if (signature(schedule, "v@::@d")) {
            ((void (*)(id, SEL, SEL, id, double))getImp(schedule))(
                self, selReg("performSelector:withObject:afterDelay:"),
                selReg("lanhostfixHostingSnapshot:"), string("1s"), 1.0);
            ((void (*)(id, SEL, SEL, id, double))getImp(schedule))(
                self, selReg("performSelector:withObject:afterDelay:"),
                selReg("lanhostfixHostingSnapshot:"), string("3s"), 3.0);
        } else LOGI("hosting: delayed snapshots unavailable; unexpected scheduling encoding");
    }
}
static int hookHostingMethod(Class cls, const char *selector, const char *encoding, IMP hook, IMP *original) {
    Method method = cls ? getInstMethod(cls, selReg(selector)) : NULL;
    Class parent = cls && runtimeSuperclass ? runtimeSuperclass(cls) : NULL;
    if (!signature(method, encoding)) {
        LOGI("hosting: lifecycle diagnostic unavailable for %s", selector); return 0;
    }
    if (parent && getInstMethod(parent, selReg(selector)) == method) {
        /* Shadow an inherited method instead of changing its superclass. */
        *original = getImp(method);
        if (*original && addMethod && addMethod(cls, selReg(selector), hook, getTypeEnc(method))) return 1;
        *original = NULL;
        LOGI("hosting: could not shadow inherited %s; original retained", selector); return 0;
    }
    if (!runtimeSuperclass) {
        LOGI("hosting: class_getSuperclass unavailable; lifecycle method left unchanged"); return 0;
    }
    return hookDiagnostic(cls, selector, encoding, NULL, hook, original);
}
static int nativeWelcomeOnMainThread(void) {
    id main = ((id (*)(id, SEL))welcomeMainThreadIMP)((id)welcomeThreadClass, selReg("mainThread"));
    id current = ((id (*)(id, SEL))welcomeCurrentThreadIMP)((id)welcomeThreadClass, selReg("currentThread"));
    return main && main == current;
}
static void my_nativeWelcomeUpdateCallback(id self, SEL cmd, id request) {
    id data, world, server;
    uint32_t token;
    Method bytes;
    const void *buffer;
    (void)cmd;
    if (diagnosticNumber(request, "count") != 3) return;
    data = objectAtIndex(request, 0); world = objectAtIndex(request, 1); server = objectAtIndex(request, 2);
    if (!data || !world || !server || diagnosticNumber(data, "length") != (int32_t)sizeof(token)) return;
    bytes = getInstMethod(objectClass(data), selReg("bytes"));
    if (!signature(bytes, "^@:") || !getImp(bytes)) return;
    buffer = ((const void *(*)(id, SEL))getImp(bytes))(data, selReg("bytes"));
    if (!buffer) return;
    memcpy(&token, buffer, sizeof(token));
    deliverWelcomeUpdate(self, world, server, token);
}
static int nativeEnqueueWelcomeUpdate(id game, id world, id server, uint32_t token) {
    id objects[3], request;
    objects[0] = ((id (*)(id, SEL, const void *, uint32_t))welcomeDataFactoryIMP)(
        (id)welcomeDataClass, selReg("dataWithBytes:length:"), &token, sizeof(token));
    objects[1] = world; objects[2] = server;
    if (!objects[0]) return 0;
    request = ((id (*)(id, SEL, const id *, uint32_t))welcomeArrayFactoryIMP)(
        (id)welcomeArrayClass, selReg("arrayWithObjects:count:"), objects, 3);
    if (!request) return 0;
    /* NSData copies the token; NSArray retains the token, world and server.
     * The native scheduler retains its target GameView and request until delivery.
     * No borrowed runtime allocation crosses this library's free boundary.
     */
    ((void (*)(id, SEL, SEL, id, unsigned char))welcomeScheduleIMP)(
        game, selReg("performSelectorOnMainThread:withObject:waitUntilDone:"),
        selReg("lanhostfixWelcomeGameUpdate:"), request, 0);
    return 1;
}
static void *symbolEither(void *handle, const char *name) {
    void *result;
    char alternate[128];
    size_t length;
    if (!handle) return NULL;
    result = dlsym(handle, name);
    if (result) return result;
    length = strlen(name);
    if (length + 2 > sizeof(alternate)) return NULL;
    alternate[0] = '_'; memcpy(alternate + 1, name, length + 1);
    return dlsym(handle, alternate);
}


/* Feature installation. Each optional feature has its own commit boundary. */

static int installResolverImportDiagnostic(void) {
    /* 1/2 - target is an ELF consumer, with no ObjC class or message-send gate.
     * 3 - namedImportImage checks relocation identity/idempotency.
     * 4 - no ivar offsets. 5 - symbol/provider lookup; h_errno is optional.
     * 6 - no ObjC encoding exists for this C import. 7 - exchange its GOT slot.
     */
    ImportRequest request = {.library = "libSystem.so", .symbol = "gethostbyname2",
        .replacement = (void *)my_getHostByName2, .g_origStorage = &g_origGetHostByName2};
    dl_iterate_phdr(namedImportImage, &request);
    if (request.installed && request.provider.dli_fname) {
        void *handle = dlopen(request.provider.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        if (handle) nativeHostError = (int *(*)(void))dlsym(handle, "__get_h_errno");
    }
    LOGV("resolver import hook available=%d h_errnoAvailable=%d", request.installed, nativeHostError != NULL);
    return request.installed;
}

static int installJoin(void) {
    /* 1 */
    Class cls = getClass("JoinWorldUI");
    Method advanced, resize, action;
    if (!cls || !getClass("NSString")) {
        LOGE("join class lookup failed: JoinWorldUI=%p NSString=%p", cls, getClass("NSString"));
        return 0;
    }
    /* Step 2 is handled by the file-level install() before feature dispatch. */
    /* 3 */
    advanced = getInstMethod(cls, selReg("advancedButton:"));
    resize = getInstMethod(cls, selReg("windowInfoChanged:"));
    action = getInstMethod(cls, selReg("advancedLocalNetworkButton:"));
    if (advanced && getImp(advanced) == (IMP)my_advanced) return 1;
    /* 4 */
    if (!resolveOffset(cls, "JoinWorldUI", "state", 'i', &offJoinState) ||
        !resolveOffset(cls, "JoinWorldUI", "advancedCancelButton", '@', &offJoinCancel) ||
        !resolveOffset(cls, "JoinWorldUI", "advancedLocalNetworkButton", '@', &offJoinLocal) ||
        !resolveOffset(cls, "JoinWorldUI", "advancedOnlineButton", '@', &offJoinOnline) ||
        !resolveOffset(cls, "JoinWorldUI", "advancedSearchButton", '@', &offJoinSearch) ||
        !resolveOffset(cls, "JoinWorldUI", "cache", '@', &offJoinCache) ||
        !resolveOffset(cls, "JoinWorldUI", "windowInfo", 'p', &offJoinWindow)) return 0;
    /* 5 */
    selJoinLocalAction = selReg("advancedLocalNetworkButton:");
    /* 6 - check all measured encodings before committing coupled hooks. */
    if (!checkedMethod(cls, "advancedButton:", "v@:@") ||
        !checkedMethod(cls, "windowInfoChanged:", "v@:p") ||
        !checkedMethod(cls, "advancedLocalNetworkButton:", "v@:@") ||
        !checkEncoding(action, "advancedLocalNetworkButton:", ENC_LOCAL_JOIN_ACTION) ||
        !getImp(advanced) || !getImp(resize) ||
        !checkEncoding(advanced, "advancedButton:", ENC_ADVANCED) ||
        !checkEncoding(resize, "windowInfoChanged:", ENC_WINDOW_CHANGED)) return 0;
    (void)action; /* Action encoding is checked even though it is not replaced. */
    /* 7 */
    if (!hook(cls, "advancedButton:", ENC_ADVANCED, (IMP)my_advanced, &g_origAdvanced)) return 0;
    if (!hook(cls, "windowInfoChanged:", ENC_WINDOW_CHANGED, (IMP)my_joinResize, &g_origJoinResize)) {
        setImp(advanced, g_origAdvanced);
        return 0;
    }
    LOGI("installed join UI hooks; open Join World > Advanced");
    return 1;
}

static int installDiscovery(void) {
    /* 1 - class lookup. Step 2 is supplied by install(). */
    Class cls = getClass("BHNetClientMatch");
    Method search;
    /* Keep the early-startup availability prerequisite ahead of method lookup
     * so an absent daemon still produces the same diagnostic.
     */
    if (!ensureMDNS) {
        LOGE("mDNS startup symbol unavailable; discovery startup hook not installed");
        return 0;
    }
    if (!cls) return 0;
    /* 3 - method lookup, structural ABI check and idempotency. */
    search = checkedMethod(cls, "startSearchForServers", "v@:");
    if (!search) return 0;
    if (getImp(search) == (IMP)my_searchServers) return 1;
    /* 4 - these offsets support duplicate detection, which is optional. */
    if (!resolveOffset(cls, "BHNetClientMatch", "browser", '@', &offClientBrowser) ||
        !resolveOffset(cls, "BHNetClientMatch", "domains", '@', &offClientDomains)) {
        offClientBrowser = offClientDomains = 0;
        LOGE("domain duplicate check unavailable; retaining native domain handling");
    }
    /* 5 - save the target for forwarding; 6 - hook() checks its measured encoding. */
    g_origSearchServers = getImp(search);
    if (!g_origSearchServers) return 0;
    /* Step 6 is skipped for one hook; hook() verifies its encoding at step 7. */
    /* 7 */
    if (!hook(cls, "startSearchForServers", ENC_SEARCH_SERVERS, (IMP)my_searchServers, &g_origSearchServers)) return 0;
    LOGI("installed LAN search hook; native daemon startup precedes domain enumeration");
    return 1;
}

static int installErrorDiagnostic(Class cls, const char *selector, IMP hook, IMP *original) {
    /* 1/2 - optional class from caller; runtime dependencies checked by install().
     * 3 - existing method uses hookDiagnostic's local stages and saved original.
     * 4/5 - no ivars/constants to resolve. A missing callback is a new method,
     * not a replacement, so its declared encoding is not a measured ENC_*.
     * Step 6 is skipped for one replacement/new method.
     * 7 - validate the existing ABI or add the known C callback directly.
     */
    if (!cls) return 0;
    if (getInstMethod(cls, selReg(selector)))
        return hookDiagnostic(cls, selector, "v@:@@", NULL, hook, original);
    if (addMethod && addMethod(cls, selReg(selector), hook, "v16@0:4@8@12")) return 1;
    LOGE("could not add error diagnostic %s", selector);
    return 0;
}

static int installServicePortFix(void) {
    /* 1-7 - independent getter; hookDiagnostic supplies method/check/commit stages. */
    return hookDiagnostic(getClass("NSNetService"), "port", "i@:", "l@:",
                          (IMP)my_servicePort, &g_origServicePort);
}

static void installDiscoveryDiagnostics(void) {
    /* 1 - independent feature classes. Step 2 is supplied by install(). */
    Class browser = getClass("NSNetServiceBrowser"), client = getClass("BHNetClientMatch");
#if LANHOSTFIX_VERBOSE
    int traces = 0, errors = 0;
#endif
    /* 3-7 - each trace has its own lookup, ABI check and commit. Missing optional
     * traces do not undo siblings; hookDiagnostic documents the local stages.
     */
    COUNT_DIAGNOSTIC(traces, hookDiagnostic(browser, "searchForBrowsableDomains", "v@:", NULL,
                             (IMP)my_domainSearch, &g_origDomainSearch));
    COUNT_DIAGNOSTIC(traces, hookDiagnostic(browser, "searchForServicesOfType:inDomain:", "v@:@@", NULL,
                             (IMP)my_serviceSearch, &g_origServiceSearch));
    COUNT_DIAGNOSTIC(traces, hookDiagnostic(client, "netServiceBrowser:didFindDomain:moreComing:", "v@:@@c", "v@:@@B",
                             (IMP)my_foundDomain, &g_origFoundDomain));
    COUNT_DIAGNOSTIC(traces, hookDiagnostic(client, "netServiceBrowser:didFindService:moreComing:", "v@:@@c", "v@:@@B",
                             (IMP)my_foundService, &g_origFoundService));
    COUNT_DIAGNOSTIC(traces, hookDiagnostic(client, "netServiceDidResolveAddress:", "v@:@", NULL,
                             (IMP)my_resolvedService, &g_origResolvedService));
    COUNT_DIAGNOSTIC(errors, installErrorDiagnostic(client, "netServiceBrowser:didNotSearch:",
                                     (IMP)my_searchError, &g_origSearchError));
    COUNT_DIAGNOSTIC(errors, installErrorDiagnostic(client, "netService:didNotResolve:",
                                     (IMP)my_resolveError, &g_origResolveError));
    LOGV("discovery diagnostics installed: traces=%d/5 errors=%d/2", traces, errors);
}

static void installJoinConnectionDiagnostics(void) {
    /* 1 - independent feature classes. Step 2 is supplied by install(). */
    Class client = getClass("BHNetClientMatch"), game = getClass("GameView");
#if LANHOSTFIX_VERBOSE
    int traces = 0;
#endif
    /* 4/5 - optional connection fields; zero offsets disable their dependent repair. */
    if (client) {
        if (!resolveOffset(client, "BHNetClientMatch", "foundHosts", '@', &offClientFoundHosts))
            offClientFoundHosts = 0;
        if (!resolveOffset(client, "BHNetClientMatch", "host", '@', &offClientHost)) offClientHost = 0;
        if (!resolveOffset(client, "BHNetClientMatch", "port", '@', &offClientPort)) offClientPort = 0;
    }
    /* 3-7 - each trace has its own lookup, ABI check and commit. Missing optional
     * traces do not undo siblings; hookDiagnostic documents the local stages.
     */
    COUNT_DIAGNOSTIC(traces, hookDiagnostic(client, "worldSelectedAtIndex:", "v@:i", "v@:I",
                             (IMP)my_worldSelection, &g_origWorldSelection));
    COUNT_DIAGNOSTIC(traces, hookDiagnostic(client, "connectToSpecificHostAndPort", "v@:", NULL,
                             (IMP)my_clientConnect, &g_origClientConnect));
    COUNT_DIAGNOSTIC(traces, hookDiagnostic(game, "multipleWorldsFoundForClientMatch:options:", "v@:@@", NULL,
                             (IMP)my_worldOptions, &g_origWorldOptions));
    LOGV("join connection diagnostics installed: traces=%d/3", traces);
}

static int installHostingUI(void) {
    /* 1 */
    Method options, resize, back, action, start;
    Class server;
    loadWorldClass = getClass("LoadWorldUI");
    if (!loadWorldClass || !getClass("NSString")) return 0;
    /* Step 2 is handled by the file-level install() before feature dispatch. */
    /* 3 */
    options = getInstMethod(loadWorldClass, selReg("optionsButton:"));
    resize = getInstMethod(loadWorldClass, selReg("windowInfoChanged:"));
    back = getInstMethod(loadWorldClass, selReg("backButton:"));
    action = getInstMethod(loadWorldClass, selReg("hostMultiButton:"));
    if (options && getImp(options) == (IMP)my_options) return 1;
    /* 4 */
    if (!resolveOffset(loadWorldClass, "LoadWorldUI", "state", 'i', &offState) ||
        !resolveOffset(loadWorldClass, "LoadWorldUI", "hostMultiButton", '@', &offHost) ||
        !resolveOffset(loadWorldClass, "LoadWorldUI", "deleteWorldButton", '@', &offDelete) ||
        !resolveOffset(loadWorldClass, "LoadWorldUI", "backButton", '@', &offBack) ||
        !resolveOffset(loadWorldClass, "LoadWorldUI", "migrateToCustomButton", '@', &offMigrate) ||
        !resolveOffset(loadWorldClass, "LoadWorldUI", "disabledTextView", '@', &offDisabled) ||
        !resolveOffset(loadWorldClass, "LoadWorldUI", "cache", '@', &offCache) ||
        !resolveOffset(loadWorldClass, "LoadWorldUI", "windowInfo", 'p', &offWindow)) return 0;
    /* 5 */
    selHostAction = selReg("hostMultiButton:");
    /* 6 - check all measured encodings before committing coupled hooks. */
    if (!checkedMethod(loadWorldClass, "optionsButton:", "v@:@") ||
        !checkedMethod(loadWorldClass, "windowInfoChanged:", "v@:p") ||
        !checkedMethod(loadWorldClass, "backButton:", "v@:@") ||
        !checkedMethod(loadWorldClass, "hostMultiButton:", "v@:@") ||
        !checkEncoding(action, "hostMultiButton:", ENC_HOST_ACTION) ||
        !getImp(options) || !getImp(resize) || !getImp(back) ||
        !checkEncoding(options, "optionsButton:", ENC_OPTIONS) ||
        !checkEncoding(resize, "windowInfoChanged:", ENC_WINDOW_CHANGED) ||
        !checkEncoding(back, "backButton:", ENC_BACK)) return 0;
    (void)action;
    /* 7 */
    if (!hook(loadWorldClass, "optionsButton:", ENC_OPTIONS, (IMP)my_options, &g_origOptions)) return 0;
    if (!hook(loadWorldClass, "windowInfoChanged:", ENC_WINDOW_CHANGED, (IMP)my_resize, &g_origResize)) {
        setImp(options, g_origOptions);
        return 0;
    }
    if (!hook(loadWorldClass, "backButton:", ENC_BACK, (IMP)my_back, &g_origBack)) {
        setImp(resize, g_origResize);
        setImp(options, g_origOptions);
        return 0;
    }
    LOGI("installed UI hooks; open Options for a local world");

    /* A separate optional feature starts here: stages 1/3 locate the server
     * method, 4 resolves its fields, 5 saves its target, and 6/7 validates and
     * commits one trace. Step 2 is inherited. Keep this after the primary UI
     * idempotency guard; independently repeating target capture is unsafe.
     */
    /* Diagnostics are optional and do not change server startup behavior. */
    server = getClass("BHNetServerMatch");
    start = server ? getInstMethod(server, selReg("startServer")) : NULL;
    if ((signature(start, "c@:") || signature(start, "B@:")) &&
        resolveOffset(server, "BHNetServerMatch", "enetServer", '^', &offEnet) &&
        resolveOffset(server, "BHNetServerMatch", "reliableServerListeningSocket", '^', &offTcp) &&
        resolveOffset(server, "BHNetServerMatch", "portToUse", 'S', &offPort)) {
        g_origStart = getImp(start);
        if (g_origStart) hook(server, "startServer", ENC_START_SERVER, (IMP)my_start, &g_origStart);
    } else LOGI("startup diagnostics unavailable; UI hook remains installed");
    return 1;
}

static int installHostWelcomeFix(void) {
    /* 1 - World/GameView classes. Step 2 is supplied by install(). */
    Class world = getClass("World"), game = getClass("GameView");
    /* 3 - method lookup and typed constructor trampoline selection. */
    Method initializer = world ? getInstMethod(world, selReg(welcomeWorldSelector)) : NULL;
    Method display = game ? getInstMethod(game, selReg("viewServerWelcomeMessage:customRules:allowEdit:")) : NULL;
    IMP replacement = welcomeWorldHookForEncoding(initializer);
    welcomeServerClass = getClass("BHServer"); welcomeMatchClass = getClass("BHNetServerMatch");
    /* 4 - validate host identity metadata before the existing idempotency decision.
     * Preliminary ABI checks here keep the original unavailable-feature diagnostic.
     */
    if (!replacement || (!signature(display, "v@:@@c") && !signature(display, "v@:@@B")) ||
        !welcomeServerClass || !welcomeMatchClass ||
        !resolveOffset(welcomeServerClass, "BHServer", "match", '@', &offWelcomeServerMatch)) {
        LOGE("hosting welcome repair unavailable: worldInit=%s display=%s",
             initializer ? getTypeEnc(initializer) : "(missing)", display ? getTypeEnc(display) : "(missing)");
        return 0;
    }
    if (getImp(initializer) == replacement && getImp(display) == (IMP)my_welcomeDisplay) return 1;
    if (getImp(initializer) == replacement || getImp(display) == (IMP)my_welcomeDisplay) {
        LOGE("hosting welcome repair partially wrapped; installation skipped"); return 0;
    }
    /* 5 - both saved targets are needed for the coupled forwarding path. */
    g_origWelcomeWorldInit = getImp(initializer); g_origWelcomeDisplay = getImp(display);
    if (!g_origWelcomeWorldInit || !g_origWelcomeDisplay) return 0;
    /* 6 - both measured encodings before either coupled hook is committed. */
    if (!checkEncoding(display, "viewServerWelcomeMessage:customRules:allowEdit:", ENC_WELCOME_DISPLAY) ||
        !checkEncoding(initializer, welcomeWorldSelector, ENC_WORLD_INIT)) return 0;
    /* 7 */
    if (!hook(game, "viewServerWelcomeMessage:customRules:allowEdit:", ENC_WELCOME_DISPLAY,
              (IMP)my_welcomeDisplay, &g_origWelcomeDisplay)) return 0;
    if (!hook(world, welcomeWorldSelector, ENC_WORLD_INIT, replacement, &g_origWelcomeWorldInit)) {
        setImp(display, g_origWelcomeDisplay);
        return 0;
    }
    LOGI("hosting welcome repair installed: World initializer encoding=%s; automatic LAN-host display scoped to initialization", getTypeEnc(initializer));
    return 1;
}

static int installHostWelcomeClientGuard(void) {
    /* 1 - guard configuration uses GameView and the host classes resolved earlier.
     * 2/3 - runtime and background wrapper were installed by their own feature.
     * 4/5/6 - bind each named field/getter after its ABI check. Reset unavailable
     * guards to zero/NULL, preserving the existing independent fallback decisions.
     * 7 - no method replacement here; publishing these fields enables guards in
     * the already installed background wrapper. A missing chat/movie/IAP/picker
     * guard leaves the other available guards active.
     */
    Class game = getClass("GameView");
    offHostWelcomeFlag = 0;
    offHostMovieFlag = offHostIAPFlag = offHostController = offHostClient = 0;
    nativeChatHaxGetter = nativePickerGetter = nativePickerSetter = NULL;
    if (!game || !g_origGameBackground || !offGameServer ||
        !welcomeServerClass || !welcomeMatchClass || !offWelcomeServerMatch ||
        !resolveOffset(game, "GameView", "world", '@', &offGameWorld) ||
        !(offHostWelcomeFlag = nativeBooleanOffset(game, "welcomeMessageShowing"))) {
        LOGE("hosting: native welcome client guard unavailable; original cleanup retained"); return 0;
    }
    /* class_getInstanceVariable returns borrowed metadata. No runtime-owned
     * array is allocated or passed to this library's allocator/free pair.
     */
    LOGV("hosting: native welcome client guard installed flag=welcomeMessageShowing offset=0x%x; direct ivar lookup; game owns flag lifecycle",
         offHostWelcomeFlag);
    Method chat = getInstMethod(game, selReg("chatHaxOngoing"));
    if ((signature(chat, "c@:") || signature(chat, "B@:")) && getImp(chat)) {
        nativeChatHaxGetter = getImp(chat);
        LOGV("hosting: native chat client guard installed getter=chatHaxOngoing encoding=%s; game owns flag lifecycle", getTypeEnc(chat));
    } else LOGE("hosting: native chat client guard unavailable encoding=%s; welcome guard retained",
                chat ? getTypeEnc(chat) : "(missing)");
    offHostMovieFlag = nativeBooleanOffset(game, "movieViewInProgress");
    offHostIAPFlag = nativeBooleanOffset(game, "iapInProgress");
    resolveOffset(game, "GameView", "viewController", '@', &offHostController);
    Method pickerSetter = getInstMethod(game, selReg("setPickerShowing:"));
    IMP pickerGetter = nativeBooleanGetter(game, "pickerShowing");
    if (pickerGetter && (signature(pickerSetter, "v@:c") || signature(pickerSetter, "v@:B")) &&
        getImp(pickerSetter) && resolveOffset(game, "GameView", "bhClient", '@', &offHostClient)) {
        nativePickerGetter = pickerGetter; nativePickerSetter = getImp(pickerSetter);
        LOGV("hosting: native picker client guard installed getter=pickerShowing setter=setPickerShowing:; one-shot reset follows background handler");
    } else LOGE("hosting: native picker client guard unavailable; getter and reset must both be valid");
    LOGI("hosting: native client guards installed welcome=1 chat=%d movie=%d chartboostController=%d iap=%d picker=%d; checked Chartboost getter resolved at delivery",
         nativeChatHaxGetter != NULL, offHostMovieFlag != 0, offHostController != 0, offHostIAPFlag != 0, nativePickerGetter != NULL);
    return 1;
}

static int installWelcomeUpdateHook(void) {
    /* 1 - classes. Step 2 is supplied by install(). */
    Class world = getClass("World");
    welcomeGameClass = getClass("GameView");
    /* 3 - method and typed update trampoline; metadata refusal still precedes idempotency. */
    Method method = welcomeGameClass ? getInstMethod(welcomeGameClass, selReg("update:accurateDT:")) : NULL;
    IMP replacement = welcomeUpdateHookForEncoding(method);
    /* 4/5 - identity offsets and queue functions must all be available. */
    if (!world || !welcomeGameClass || !replacement || !welcomeOnMainThread || !enqueueWelcomeUpdate ||
        !resolveOffset(welcomeGameClass, "GameView", "world", '@', &offGameWorld) ||
        !resolveOffset(welcomeGameClass, "GameView", "bhServer", '@', &offGameServer) ||
        !resolveOffset(world, "World", "server", '@', &offWelcomeWorldServer)) {
        LOGE("hosting: welcome GameView thread repair unavailable encoding=%s",
             method ? getTypeEnc(method) : "(missing)"); return 0;
    }
    if (getImp(method) == replacement) return 1;
    /* 5 - record the native argument types used by the forwarding trampoline. */
    welcomeUpdateDTType = signature(method, "v@:ff") || signature(method, "v@:fd") ? 'f' : 'd';
    welcomeUpdateAccurateType = signature(method, "v@:ff") || signature(method, "v@:df") ? 'f' : 'd';
    g_origWelcomeUpdate = getImp(method);
    if (!g_origWelcomeUpdate) return 0;
    /* Step 6 is skipped for one hook; ENC_GAME_UPDATE is checked by hook(). */
    /* 7 - one hook: shared helper verifies the measured encoding once. */
    if (!hook(welcomeGameClass, "update:accurateDT:", ENC_GAME_UPDATE, replacement, &g_origWelcomeUpdate)) return 0;
    LOGI("hosting: welcome GameView thread repair installed encoding=%s; native main-thread scheduling", getTypeEnc(method));
    return 1;
}

static int installDialogTypefaceTrace(void) {
    /* 1 - bridge classes may appear lazily when the font proxy is first used.
     * 2 - install() already checked the runtime. 3 - metaclass method lookup.
     * 4/5 - no ivars; each loader has its own named encoding expectation.
     * 6/7 - independent checks/commits via hookDiagnostic, retaining partial
     * availability. Calling this again is safe because the helper checks IMPs.
     */
    Class cls = getClass("AndroidTypeface");
    Class meta = cls ? objectClass((id)cls) : NULL;
    Method named = meta ? getInstMethod(meta, selReg("typefaceNamed:style:")) : NULL;
    int count = 0;
    if (!meta) return 0;
    count += hookDiagnostic(meta, "loadFromAssetManager:path:", "@@:@@", NULL,
                            (IMP)my_dialogAssetTypeface, &g_origDialogAssetTypeface);
    count += hookDiagnostic(meta, "typeface:", "@@:@", NULL,
                            (IMP)my_dialogFileTypeface, &g_origDialogFileTypeface);
    const char *styleSignature = signature(named, "@@:@i") ? "@@:@i" :
        signature(named, "@@:@I") ? "@@:@I" : signature(named, "@@:@l") ? "@@:@l" :
        signature(named, "@@:@L") ? "@@:@L" : NULL;
    if (styleSignature)
        count += hookDiagnostic(meta, "typefaceNamed:style:", styleSignature, NULL,
                                (IMP)my_dialogNamedTypeface, &g_origDialogNamedTypeface);
    return count;
}

static void installTableDialogFix(void *sys) {
    /* 1 - independent dialog/image classes. Step 2 is supplied by install().
     * Missing dialog class skips this feature; no network hooks are rolled back.
     */
    Class imageClass = getClass("UIImage");
    Method initialize, show, stretch;
    IMP initializeHook, showHook;
    int background = 0, fonts = 0, cells = 0, fontTrace = 0, initTrace = 0;
    if (!runtimeClassName && sys)
        runtimeClassName = (const char *(*)(Class))dlsym(sys, "class_getName");
    if (!runtimeSuperclass && sys)
        runtimeSuperclass = (Class (*)(Class))dlsym(sys, "class_getSuperclass");
    tableDialogClass = getClass("TableSelectionAlertView");
    if (!tableDialogClass) { LOGE("dialog: TableSelectionAlertView unavailable"); return; }
    /* 3 - choose ABI-correct wrappers and exclude inherited +initialize. */
    initialize = getInstMethod(objectClass((id)tableDialogClass), selReg("initialize"));
    show = getInstMethod(tableDialogClass, selReg("showNow"));
    stretch = imageClass ? getInstMethod(imageClass, selReg("stretchableImageWithLeftCapWidth:topCapHeight:")) : NULL;
    initializeHook = signature(initialize, "v@:") ? (IMP)my_dialogInitializeVoid :
                     signature(initialize, "@@:") ? (IMP)my_dialogInitializeObject : NULL;
    /* Never replace NSObject's inherited +initialize if this build omits
     * the dialog's own initializer. Method lookup alone includes inheritance.
     */
    if (!runtimeSuperclass || initialize == getInstMethod(
            objectClass((id)runtimeSuperclass(tableDialogClass)), selReg("initialize")))
        initializeHook = NULL;
    showHook = signature(show, "v@:") ? (IMP)my_dialogShowVoid :
               signature(show, "@@:") ? (IMP)my_dialogShowObject : NULL;
    /* 4/5 - image factory needs no ivars. Dialog view/table offsets are resolved
     * only for their respective optional subfeatures below, as in the native path.
     * 6 - validate both panel methods before either coupled replacement.
     */
    if (initializeHook && (signature(stretch, "@@:ii") || signature(stretch, "@@:ll")) &&
        getImp(initialize) && getImp(stretch)) {
        if (!checkEncoding(stretch, "stretchableImageWithLeftCapWidth:topCapHeight:", ENC_STRETCH_IMAGE) ||
            !checkEncoding(initialize, "initialize", ENC_INITIALIZE)) initializeHook = NULL;
        /* 7 - preserve the existing partial-install policy if the second hook fails. */
        background = initializeHook && hookDiagnostic(imageClass, "stretchableImageWithLeftCapWidth:topCapHeight:",
            "@@:ii", "@@:ll", (IMP)my_dialogStretch, &g_origDialogStretch) &&
            hookDiagnostic(objectClass((id)tableDialogClass), "initialize", "v@:", "@@:",
                initializeHook, &g_origDialogInitialize);
    }
    /* Independent font/frame/cell/trace subfeatures repeat stages 3-7 locally.
     * A missing loader or cell method does not disable a working panel/font fix.
     */
    if (showHook && resolveOffset(tableDialogClass, "TableSelectionAlertView", "_view", '@', &offDialogView))
        fonts = hookDiagnostic(tableDialogClass, "showNow", "v@:", "@@:", showHook, &g_origDialogShow);
    int tableFrame = resolveOffset(tableDialogClass, "TableSelectionAlertView", "tableView", '@', &offDialogTable);
    cells = hookDiagnostic(tableDialogClass, "tableView:cellForRowAtIndexPath:", "@@:@@", NULL,
                           (IMP)my_dialogCell, &g_origDialogCell);
    initTrace = hookDiagnostic(tableDialogClass, "initWithTitle:message:options:", "@@:@@@", NULL,
                               (IMP)my_dialogInit, &g_origDialogInit);
    Class fontClass = getClass("UIFont");
    Method fontProxy = fontClass ? getInstMethod(fontClass, selReg("_proxy")) : NULL;
    Class parent = fontClass && runtimeSuperclass ? runtimeSuperclass(fontClass) : NULL;
    if (parent && fontProxy != getInstMethod(parent, selReg("_proxy")))
        fontTrace = hookDiagnostic(fontClass, "_proxy", "@@:", NULL,
                                   (IMP)my_dialogFontProxy, &g_origDialogFontProxy);
    int loaders = installDialogTypefaceTrace();
    LOGI("dialog: installed panelCorrection=%d panelCapPoints=%d titleViewFontRepair=%d tableBackgroundFrame=%d cellInset=%d fontProxyTrace=%d initTrace=%d typefaceLoaders=%d/3; font tracing scoped to dialog; normal UILabel setter on existing _titleView",
         background, background ? 2 : 0, fonts, tableFrame && fonts, cells, fontTrace, initTrace, loaders);
}

static void installHostingLifecycleDiagnostics(void *sys) {
    /* 1 - each lifecycle class is optional. Step 2 is supplied by install().
     * 3-7 - independent methods use hookHostingMethod/hookDiagnostic; snapshot
     * addition has its own declared encoding rather than a measured replacement.
     * Resolve optional fields/socket exports before their dependent traces.
     */
    Class match = getClass("BHNetServerMatch"), server = getClass("BHServer"), game = getClass("GameView");
    void *core = dlopen("libCoreFoundation.so", RTLD_NOW | RTLD_NOLOAD);
    int cleanup, disconnect, dealloc, published = 0, error, background;
    runtimeSuperclass = (Class (*)(Class))dlsym(sys, "class_getSuperclass");
    hostingSocketValid = core ? (unsigned char (*)(const void *))dlsym(core, "CFSocketIsValid") : NULL;
    hostingSocketNative = core ? (int32_t (*)(const void *))dlsym(core, "CFSocketGetNative") : NULL;
    optionalHostingOffset(match, "pollTimer", '@', &offServerTimer);
    optionalHostingOffset(match, "reliableServerService", '@', &offServerService);
    optionalHostingOffset(match, "connections", '@', &offServerConnections);
    cleanup = hookHostingMethod(server, "cleanup", "v@:", (IMP)my_serverCleanup, &g_origServerCleanup);
    disconnect = hookHostingMethod(match, "disconnect", "v@:", (IMP)my_serverDisconnect, &g_origServerDisconnect);
    dealloc = hookHostingMethod(match, "dealloc", "v@:", (IMP)my_serverDealloc, &g_origServerDealloc);
    background = hookHostingMethod(game, "didEnterBackground", "v@:", (IMP)my_gameBackground, &g_origGameBackground);
    if (match && getInstMethod(match, selReg("netServiceDidPublish:")))
        published = hookHostingMethod(match, "netServiceDidPublish:", "v@:@", (IMP)my_serverPublish, &g_origServerPublish);
    else if (match && addMethod)
        published = addMethod(match, selReg("netServiceDidPublish:"), (IMP)my_serverPublish, "v12@0:4@8") != 0;
    error = installErrorDiagnostic(match, "netService:didNotPublish:", (IMP)my_serverPublishError, &g_origServerPublishError);
    if (game && offGameServer && addMethod && !getInstMethod(game, selReg("lanhostfixHostingSnapshot:")))
        hostingSnapshotsInstalled = addMethod(game, selReg("lanhostfixHostingSnapshot:"), (IMP)my_hostingSnapshot, "v12@0:4@8") != 0;
    (void)cleanup; (void)disconnect; (void)dealloc; (void)published; (void)error; (void)background;
    LOGV("hosting lifecycle diagnostics installed: cleanup=%d disconnect=%d dealloc=%d published=%d publishError=%d snapshots=%d socketAPI=%d; -1 means unavailable state",
         cleanup, disconnect, dealloc, published, error, hostingSnapshotsInstalled,
         hostingSocketValid != NULL && hostingSocketNative != NULL);
    LOGV("hosting lifecycle background delivery hook installed=%d; native cleanup retained", background);
}

static void installHostingDiagnostics(void *sys) {
    /* 1 - server/game classes. Step 2 is supplied by install().
     * 3/4/5 - initializer method, typed fields and original target.
     * 6/7 - each independent trace checks its ABI and commits separately.
     * Keep the existing initializer target capture unchanged in this structural
     * pass. Re-running this installer independently can capture its own wrapper;
     * normal bootstrap is once-only. This known repeat-install concern is not fixed here.
     */
    Class server = getClass("BHServer"), game = getClass("GameView");
    Method initializer = server ? getInstMethod(server, selReg("initWithDelegate:match:netNodeType:saveID:maxPlayers:")) : NULL;
    int initInstalled = 0, gameInstalled = 0;
    runtimeClassName = (const char *(*)(Class))dlsym(sys, "class_getName");
    if (optionalHostingOffset(server, "match", '@', &offServerMatch)) {
        optionalHostingOffset(server, "netNodeType", 'i', &offServerNodeType);
        optionalHostingOffset(server, "maxPlayers", 'i', &offServerMaxPlayers);
        if (signature(initializer, "@@:@@i@i") || signature(initializer, "@@:@@I@i") ||
            signature(initializer, "@@:@@i@I") || signature(initializer, "@@:@@I@I")) {
            g_origServerInit = getImp(initializer);
            if (g_origServerInit) initInstalled = hook(server, "initWithDelegate:match:netNodeType:saveID:maxPlayers:", ENC_SERVER_INIT, (IMP)my_serverInit, &g_origServerInit);
        } else LOGE("hosting: unsupported BHServer initializer encoding=%s",
                    initializer ? getTypeEnc(initializer) : "(missing)");
    } else LOGE("hosting: BHServer match ivar unavailable; initialization trace skipped");
    if (optionalHostingOffset(game, "bhServer", '@', &offGameServer)) {
        optionalHostingOffset(game, "bhClient", '@', &offGameClient);
        gameInstalled = hookDiagnostic(game, "startHostingNetworkGameWithUserName:userPhoto:", "v@:@@", NULL,
                                       (IMP)my_gameHosting, &g_origGameHosting);
    } else LOGE("hosting: GameView bhServer ivar unavailable; handoff trace skipped");
    (void)initInstalled; (void)gameInstalled;
    LOGV("hosting initialization diagnostics installed: serverInit=%d gameHosting=%d; native behavior retained",
         initInstalled, gameInstalled);
    installHostingLifecycleDiagnostics(sys);
}

static int installNativeWelcomeThreadRepair(void) {
    /* 1 - Foundation queue payload classes plus GameView. Step 2 is inherited.
     * 3 - native scheduling/factory methods. 4 - no ivars in this queue adapter.
     * 5 - validate methods before exposing them as scheduling dependencies.
     * Step 6 has no coupled replacements in this adapter.
     * 7 - add the declared callback, then install the update feature locally.
     * Retain this order: failed update installation leaves the added callback,
     * but the file installer does not enable host-preservation guards.
     */
    Class game = getClass("GameView");
    Method mainThread, currentThread, schedule, dataFactory, arrayFactory;
    welcomeThreadClass = getClass("NSThread"); welcomeDataClass = getClass("NSData"); welcomeArrayClass = getClass("NSArray");
    if (!game || !welcomeThreadClass || !welcomeDataClass || !welcomeArrayClass || !addMethod) return 0;
    mainThread = checkedMethod(objectClass((id)welcomeThreadClass), "mainThread", "@@:");
    currentThread = checkedMethod(objectClass((id)welcomeThreadClass), "currentThread", "@@:");
    schedule = getInstMethod(game, selReg("performSelectorOnMainThread:withObject:waitUntilDone:"));
    dataFactory = getInstMethod(objectClass((id)welcomeDataClass), selReg("dataWithBytes:length:"));
    arrayFactory = getInstMethod(objectClass((id)welcomeArrayClass), selReg("arrayWithObjects:count:"));
    if (!mainThread || !currentThread || !getImp(mainThread) || !getImp(currentThread) ||
        (!signature(schedule, "v@::@c") && !signature(schedule, "v@::@B")) || !getImp(schedule) ||
        (!signature(dataFactory, "@@:pI") && !signature(dataFactory, "@@:pL")) || !getImp(dataFactory) ||
        (!signature(arrayFactory, "@@:pI") && !signature(arrayFactory, "@@:pL")) || !getImp(arrayFactory)) {
        LOGE("hosting: native main-thread scheduling unavailable schedule=%s data=%s array=%s",
             schedule ? getTypeEnc(schedule) : "(missing)", dataFactory ? getTypeEnc(dataFactory) : "(missing)",
             arrayFactory ? getTypeEnc(arrayFactory) : "(missing)"); return 0;
    }
    if (getInstMethod(game, selReg("lanhostfixWelcomeGameUpdate:")) ||
        !addMethod(game, selReg("lanhostfixWelcomeGameUpdate:"), (IMP)my_nativeWelcomeUpdateCallback, "v12@0:4@8")) return 0;
    welcomeMainThreadIMP = getImp(mainThread); welcomeCurrentThreadIMP = getImp(currentThread);
    welcomeScheduleIMP = getImp(schedule); welcomeDataFactoryIMP = getImp(dataFactory);
    welcomeArrayFactoryIMP = getImp(arrayFactory);
    welcomeOnMainThread = nativeWelcomeOnMainThread; enqueueWelcomeUpdate = nativeEnqueueWelcomeUpdate;
    return installWelcomeUpdateHook();
}

static void installDeviceHostname(void *sys) {
    /* 1/2 - ELF consumers; no ObjC class or message sends.
     * 3 - relocation identity/idempotency is checked by namedImportImage.
     * 4 - no ivars. 5 - resolve symbol names and expected provider targets.
     * 6 - explicit C prototypes replace the ObjC encoding gate.
     * 7 - commit available imports independently; retain the original partial
     * installation behavior rather than undoing a working sibling import.
     */
    void *daemon = symbolEither(sys, "mdns_posix_daemon_main");
    ImportRequest hostRequest = {.library = "libSystem.so", .symbol = "gethostname",
        .replacement = (void *)my_daemonGetHostname, .g_origStorage = &g_origGetHostname};
    ImportRequest daemonRequest = {.library = "libCFNetwork.so", .symbol = "mdns_posix_daemon_main",
        .replacement = (void *)my_daemonMain, .g_origStorage = &g_origDaemonMain,
        .allowData = 1, .expectedTarget = daemon};
    prepareDeviceHostname = prepareDeviceHostnameJNI;
    if (!daemon || !cfNetworkHandle) {
        LOGE("mDNS: device-name hooks unavailable daemon=%d CFNetwork=%d; original hostname retained",
             daemon != NULL, cfNetworkHandle != NULL); return;
    }
    dl_iterate_phdr(namedImportImage, &hostRequest);
    if (hostRequest.installed) {
        /* Publish before the entry pointer: a newly started daemon may run
         * immediately after the GOT exchange. Until that exchange, no
         * native thread can enter this hook's daemon TLS scope.
         */
        __atomic_store_n(&deviceHostnameHooksInstalled, 1, __ATOMIC_RELEASE);
        dl_iterate_phdr(namedImportImage, &daemonRequest);
        if (!daemonRequest.installed) {
            daemonRequest.symbol = "_mdns_posix_daemon_main";
            dl_iterate_phdr(namedImportImage, &daemonRequest);
        }
    }
    __atomic_store_n(&deviceHostnameHooksInstalled,
                     hostRequest.installed && daemonRequest.installed, __ATOMIC_RELEASE);
    LOGI("mDNS: device-name hooks installed=%d gethostname=%d daemonEntry=%d; daemon thread only",
         __atomic_load_n(&deviceHostnameHooksInstalled, __ATOMIC_ACQUIRE),
         hostRequest.installed, daemonRequest.installed);
}

static void installNativeDNSDiagnostics(void *sys) {
    /* 1/2 - ELF consumers; no ObjC class or message sends.
     * 3 - relocation identity/idempotency is checked by namedImportImage.
     * 4 - no ivars. 5 - resolve symbol names and expected provider targets.
     * 6 - explicit C prototypes replace the ObjC encoding gate.
     * 7 - commit available imports independently; retain the original partial
     * installation behavior rather than undoing a working sibling import.
     */
    struct { const char *name; void *hook, *storage; } entries[] = {
        {"DNSServiceEnumerateDomains", (void *)my_dnsEnumerate, &g_origDNSEnumerate},
        {"DNSServiceBrowse", (void *)my_dnsBrowse, &g_origDNSBrowse},
        {"DNSServiceResolve", (void *)my_dnsResolve, &g_origDNSResolve},
        {"DNSServiceRegister", (void *)my_dnsRegister, &g_origDNSRegister}
    };
    size_t i;
#if LANHOSTFIX_VERBOSE
    int installed = 0;
#endif
    for (i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        char alternate[128];
        ImportRequest request = {.library = "libCFNetwork.so", .symbol = entries[i].name,
            .replacement = entries[i].hook, .g_origStorage = entries[i].storage,
            .expectedTarget = symbolEither(sys, entries[i].name)};
        if (request.expectedTarget && cfNetworkHandle) {
            dl_iterate_phdr(namedImportImage, &request);
            if (!request.installed) {
                alternate[0] = '_'; strcpy(alternate + 1, entries[i].name);
                request.symbol = alternate;
                dl_iterate_phdr(namedImportImage, &request);
            }
        }
#if LANHOSTFIX_VERBOSE
        installed += request.installed;
#endif
        LOGV("DNS-SD native: diagnostic %s installed=%d", entries[i].name, request.installed);
    }
    LOGV("DNS-SD native: immediate-result diagnostics installed=%d/4; native callbacks unchanged", installed);
}

static int installFoundationDomainBridge(void *sys) {
    /* 1 - browser class and framework image. Step 2 is supplied by install(). */
    Class cls = getClass("NSNetServiceBrowser");
    /* 3 - existing ObjC methods; ELF idempotency is checked by namedImportImage. */
    Method initMethod = cls ? getInstMethod(cls, selReg("init")) : NULL;
    Method dispatch = cls ? getInstMethod(cls, selReg("_dispatchCallBack:flags:error:")) : NULL;
    void *core = dlopen("libCoreFoundation.so", RTLD_NOW | RTLD_NOLOAD);
    void *(*getMainQueue)(void);
    ImportRequest request = {.library = "libFoundation.so", .symbol = "CFNetServiceBrowserCreate",
        .replacement = (void *)my_browserCreate, .g_origStorage = &g_origBrowserCreate};
    if (!signature(initMethod, "@@:") || !signature(dispatch, "v@:^L^")) {
        LOGE("Foundation domain bridge unavailable: incompatible browser init/dispatch encoding"); return 0;
    }
    /* 4 - no ivars. 5 - ownership/type helpers and the actual Foundation main queue. */
    cfRetain = (const void *(*)(const void *))symbolEither(core, "CFRetain");
    cfRelease = (void (*)(const void *))symbolEither(core, "CFRelease");
    cfGetTypeID = (uintptr_t (*)(const void *))symbolEither(core, "CFGetTypeID");
    cfStringTypeID = (uintptr_t (*)(void))symbolEither(core, "CFStringGetTypeID");
    asyncFunction = (void (*)(void *, void *, void (*)(void *)))symbolEither(sys, "dispatch_async_f");
    getMainQueue = (void *(*)(void))symbolEither(sys, "dispatch_get_main_queue");
    foundationMainQueue = getMainQueue ? getMainQueue() : symbolEither(sys, "dispatch_main_q");
    if (!cfRetain || !cfRelease || !cfGetTypeID || !cfStringTypeID || !asyncFunction || !foundationMainQueue) {
        LOGE("Foundation domain bridge unavailable: retain=%d release=%d type=%d stringType=%d async_f=%d mainQueue=%d",
             cfRetain != NULL, cfRelease != NULL, cfGetTypeID != NULL, cfStringTypeID != NULL,
             asyncFunction != NULL, foundationMainQueue != NULL); return 0;
    }
    if (!getImp(initMethod) || !getImp(dispatch)) return 0;
    /* 6 - check both measured encodings before any coupled ObjC commit,
     * in addition to the earlier structural ABI tests.
     */
    if (!checkEncoding(initMethod, "init", ENC_BROWSER_INIT) ||
        !checkEncoding(dispatch, "_dispatchCallBack:flags:error:", ENC_BROWSER_DISPATCH)) return 0;
    /* 7 - retain the existing import-first sequence and ObjC rollback boundary.
     * The ELF bridge remains installed if a later ObjC hook cannot be committed.
     */
    dl_iterate_phdr(namedImportImage, &request);
    if (!request.installed) return 0;
    if (getImp(initMethod) != (IMP)my_browserInit) {
        if (!hook(cls, "init", ENC_BROWSER_INIT, (IMP)my_browserInit, &g_origBrowserInit)) return 0;
    }
    if (getImp(dispatch) != (IMP)my_browserDispatch) {
        if (!hook(cls, "_dispatchCallBack:flags:error:", ENC_BROWSER_DISPATCH, (IMP)my_browserDispatch, &g_origBrowserDispatch)) {
            if (g_origBrowserInit) setImp(initMethod, g_origBrowserInit);
            return 0;
        }
    }
    LOGI("installed Foundation domain callback bridge; domain objects retained and errors copied for main queue");
    return 1;
}

static void install(void) {
    void *sys = g_sys;
    int hostingInstalled, joinInstalled, discoveryInstalled, resolverInstalled, portInstalled;

    /* 1 - feature classes are checked by their independent installers below.
     * Keep the former bootstrap's required application/introspection gate.
     * A library-wide class gate would disable unrelated available features.
     */
    if (!g_app) { LOGE("load after LibraryManager.loadLibraries()"); return; }
    objectClass = dlsym(sys, "object_getClass");
    addMethod = dlsym(sys, "class_addMethod"); /* Optional, for missing error callbacks. */
    ivarEncoding = dlsym(sys, "ivar_getTypeEncoding");
    if (!selReg || !getClass || !objectClass || !getInstMethod || !getImp ||
        !setImp || !getTypeEnc || !getIvar || !ivarOff ||
        !ivarEncoding) { LOGE("required runtime introspection missing"); return; }
    /* 2 - typed message sends are used throughout the feature code. */
    if (!g_msgSend) { LOGE("required runtime introspection missing"); return; }
    /* The canonical untyped/fallback ivar resolver remains available, but
     * existing typed metadata checks below intentionally retain fail-closed
     * behavior; activating new fallback tiers would change this library.
     */
    (void)resolve_ivar;
    /* 3-7 are feature-local: preserve independent optional components and
     * their existing installation order rather than making all-or-none LAN.
     */
    hostingInstalled = installHostingUI();
    if (!hostingInstalled) LOGE("hosting hooks not installed; original hosting UI preserved");
    joinInstalled = installJoin();
    if (!joinInstalled) LOGE("join hooks not installed; original join UI preserved");
    /* Keep the framework loaded and request startup only when searching.
     * Some dumps expose a leading underscore; try both ELF symbol spellings.
     * The helper's return register is deliberately ignored: dispatch_once /
     * pthread_create artifacts in pseudocode do not establish readiness.
     */
    cfNetworkHandle = dlopen("libCFNetwork.so", RTLD_NOW | RTLD_NOLOAD);
    if (cfNetworkHandle) {
        ensureMDNS = (void (*)(void))dlsym(cfNetworkHandle, "_CreateMDNSDaemonIfNeeded");
        if (!ensureMDNS)
            ensureMDNS = (void (*)(void))dlsym(cfNetworkHandle, "CreateMDNSDaemonIfNeeded");
    }
    installDeviceHostname(sys);
    foundationDomainBridge = installFoundationDomainBridge(sys);
    LOGI("Foundation domain bridge installed=%d; early daemon startup required; manualDomain=0",
         foundationDomainBridge);
    discoveryInstalled = installDiscovery();
    if (!discoveryInstalled) LOGE("original discovery startup retained; UI hooks are independent");
    LOGI("installation summary: hosting=%d join=%d discovery=%d",
         hostingInstalled, joinInstalled, discoveryInstalled);
    /* The resolve/connect traces also capture endpoints. They are dependencies
     * of endpointFixEnabled below, despite their diagnostic installer names.
     */
    installDiscoveryDiagnostics();
    installJoinConnectionDiagnostics();
    installTableDialogFix(sys);
    installHostingDiagnostics(sys);
    installHostWelcomeFix();
    if (installNativeWelcomeThreadRepair()) installHostWelcomeClientGuard();
    else LOGE("hosting: welcome server-preservation guard disabled; native thread routing unavailable");
    enetSetHost = (int (*)(ENetProbeAddress *, const char *))dlsym(g_app, "enet_address_set_host");
    if (!enetSetHost)
        enetSetHost = (int (*)(ENetProbeAddress *, const char *))dlsym(g_app, "_enet_address_set_host");
    LOGV("ENet resolver probe export available=%d; extra lookup disabled", enetSetHost != NULL);
    portInstalled = installServicePortFix();
    resolverInstalled = installResolverImportDiagnostic();
    /* Enable immediate joins only when both port/resolver repairs, resolution
     * callbacks and connection fields are present. UI/discovery remain independent.
     */
    endpointFixEnabled = portInstalled && resolverInstalled &&
        g_origResolvedService && g_origClientConnect && offClientFoundHosts && offClientHost && offClientPort;
    LOGI("join endpoint repair enabled=%d portHook=%d resolverImport=%d; retaining native world selection",
         endpointFixEnabled, portInstalled, resolverInstalled);
    dnsQueryRecord = (int32_t (*)(void **, uint32_t, uint32_t, const char *, uint16_t, uint16_t,
                                  DNSRecordReply, void *))symbolEither(sys, "DNSServiceQueryRecord");
    dnsSocketFD = (int (*)(void *))symbolEither(sys, "DNSServiceRefSockFD");
    dnsProcessResult = (int32_t (*)(void *))symbolEither(sys, "DNSServiceProcessResult");
    dnsDeallocate = (void (*)(void *))symbolEither(sys, "DNSServiceRefDeallocate");
    installNativeDNSDiagnostics(sys);
    /* Saved-entry resolution has its own dependency gate and bounded wait;
     * it does not require a live browse result or the service-port hook.
     */
    localDNSFixEnabled = resolverInstalled && g_origClientConnect && offClientHost && offClientPort &&
        ensureMDNS && dnsQueryRecord && dnsSocketFD && dnsProcessResult && dnsDeallocate;
    LOGI("join: saved .local resolver enabled=%d query=%d socket=%d process=%d cancel=%d daemon=%d; responseWaitMs=%d",
         localDNSFixEnabled, dnsQueryRecord != NULL, dnsSocketFD != NULL, dnsProcessResult != NULL,
         dnsDeallocate != NULL, ensureMDNS != NULL, LOCAL_DNS_WAIT_MS);
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
