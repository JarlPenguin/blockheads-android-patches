# Patch The Blockheads APK for modern Android devices

These patches were designed mostly with the help of LLMs for v1.7.5 and were tested on devices running Android 8.1 and 16.

## List of patches and what they do

### `adaptive-icon.patch`
* Adds support for adaptive icons

### `audio-fix.patch`
* Fixes music not resuming after backgrounding
* Fixes main menu music not looping
* Fixes music never starting when the game is launched while another app is playing audio

### `audio-record-keep-saves.patch`:
* Allows the game's audio to be captured when screen recording
* Allows players to retain game data when uninstalling the game

### `dpi-fix.patch`
* Fixes UI scaling on tablets
* Splash screen is now selected dynamically based on the device's type, resolution and orientation

### `join-link-fix.patch`
* Fixes join links (`blockheads://` and `theblockheads.net/join.php`) not working

### `permissions-fix.patch`
* Fixes the storage-permission request never being shown on Android 7.1 and later

### `privacy-popup-cleanup.patch`
* Prevents the "Privacy Setting Changed" dialog from appearing where uncalled for

### `rotation-fix.patch`
* Allows the game to natively launch in landscape mode
* Fixes crashes on rotation on various devices
* Fixes temporary rotation to right-landscape on startup when auto-rotation is enabled
* Fixes tilt controls when auto-rotation is disabled
* Fixes tilt controls when the device is lying flat at launch

### `webview-rescue.patch`
* Prevents the game from crashing when the WebView crashes

### `webview-suspend-freeze-fix.patch`
* Fixes freezes when suspending and resuming the game, opening WebView pages and the photo picker

### `welcome-fix.patch`
* Fixes newlines and non-ASCII characters being deleted from welcome messages

### `world-selection-fix.patch`
* Fixes the main menu resetting to the most recently played world whenever the game is suspended and resumed

## Technical details

### `adaptive-icon.patch`
* Adds a copy of `res/drawable/icon.png` with transparent padding, located at `res/drawable/icon_adaptive.png`.
* Adds `res/drawable-anydpi-v26/icon.xml` to enable adaptive icon support.

### `audio-fix.patch`
Includes a native library (`libaudiofix.so`) that swizzles Apportable's Objective-C runtime at startup and smali changes.

> **Notes from Claude:**
> `-[MJSoundManager restartMusicAfterActiveEvent:]` - the only music call `-[GameView didBecomeActive]` makes on resume - is an empty stub: a compiled prologue and epilogue with no body. It is empty because on iOS the audio-session interruption path handled resume. That path is intact on both sides here and never connects. `-[AVAudioPlayer(Platform) _platform_focus_changed:]` correctly maps Android focus constants onto `_beginInterruption`/`_endInterruption`, and `-[MJSoundManager reinitialize]` (reached from `initWithMasterVolume:`) does register for `AVAudioSessionInterruptionNotification`, but `-[AndroidAudioManager audioFocusChange:]` never fires and its `delegate` is nil, so nothing propagates. Apportable also sends the deprecated `AVAudioSessionDelegate` method `beginInterruption` where the game listens for the modern notification, so the two ends would not have met even with focus working. Meanwhile `-[AVAudioPlayer dealloc]` is the sole sender of `_platform_unload:`, which is what releases the Java `MediaPlayer` slot - so the player dies on background with nothing to restart it.

* Fills in the empty `restartMusicAfterActiveEvent:` with a call to the game's own `playMP3IfSafe:withTimeOffset:`, using the path retained from `lastPlayedMP3Path` and the position captured at teardown. Routing through the game's own loader means native retains ownership of the new `AVAudioPlayer`, so the in-game music volume slider continues to affect it - the defect in the earlier Java-side workaround, where the retained player was invisible to native.
* Captures the playback position in `-[AVAudioPlayer stop]`, which fires ~1 ms before `dealloc` on the same player. This is the only moment it is readable: `-[AVAudioPlayer currentTime]` returns the cached seek target from `setCurrentTime:` whenever `_platform_isPlaying:` is false, and by `dealloc` the Java `MediaPlayer` has already been stopped. The `isPlaying` check at capture time also distinguishes a genuine suspend from a track that ended naturally, in which case no restore is issued.
* Declines to restore when the game is mid-transition, keyed to `MJSoundManager.fadingOut` and to a dealloc occurring inside `loadMP3IfSafe:withTimeOffset:`. A dealloc in either window is the game replacing a track, not a suspend, and the position is meaningless. Any game-initiated load also drops a pending restore outright, since the game has decided what should be playing.
* Nils `MJSoundManager.mp3Player` after the player is deallocated. Nothing else does - the only writer that nils it is `stopMP3Playback`, which has no callers - so `setMusicVolume`, `isPlayingMP3`, `currentMP3time`, and `setLoopMP3s:` all read freed memory after every suspend in the unpatched game.
* Replaces `-[MJSoundManager safeToPlayMP3s]` with an unconditional true. The method is a one-line read of `otherAudioWasPaying`, which `reinitialize` latches once at startup from `isOtherAudioPlaying` and never recomputes, because the only path that would (`attemptToReinitializeAudio`) is unreachable. Launching while another app plays audio therefore muted game music for the entire session. The session category is `Ambient`, so mixing is the expected behaviour; the user's volume slider remains the control.
* Treats any nonzero loop count as infinite in `AudioPlayer.setNumberOfLoops`, which previously mapped only `-1` to `setLooping(true)`. The game requests 999 repetitions for looping tracks, so every looping track silently played once. `loadMP3IfSafe:withTimeOffset:` and `setLoopMP3s:` are the only senders of `setNumberOfLoops:` in the binary and pass only 999 or 0, so the mapping is complete for this app; a faithful finite count is not reachable through `MediaPlayer.setLooping(boolean)` anyway.

**Known limitations:** see https://github.com/JarlPenguin/blockheads-android-patches/issues/1.

### `audio-record-keep-saves.patch`:
* Sets `allowAudioPlaybackCapture` and `hasFragileUserData` to true.

### `dpi-fix.patch`
Includes a native library (`libdpifix.so`) that swizzles Apportable's Objective-C runtime at startup and smali changes.

> **Notes from Claude:**
> Three independent defects stack here. `-[UIDevice userInterfaceIdiom]` resolves through a `dispatch_once` block to `getenv("TABLET")`, matched case-insensitively against `yes`/`true`/`1` - there is no device detection at all, and nothing in the shipped APK ever sets the variable, so every Android device reports `UIUserInterfaceIdiomPhone`. That single accessor gates both `NSMainNibFile~ipad` and UIKit's whole `~ipad`/`@2x` resource-suffix resolver, so `MainWindow-iPad.nib` and the iPad launch images ship in the APK and are never once consulted. Separately, `-[UIScreen preferredMode]` requests `+[UIScreenMode emulatedMode:12]`, which forwards to `+[UINativeScreenMode nativeMode:]` - and that method discards its argument and computes `scale = VerdePluginNativeWidth() / 600.0`, a magic constant with no relation to display density. `-[UIScreen bounds]` is `currentMode.size / currentMode.scale` and `PIXEL_SCALE` is a copy of `UIScreen.scale` cached in `-[EAGLView initWithCoder:]`, so one bad mode poisons layout, framebuffer sizing and touch coordinate mapping together; on a 2560×1600 tablet the result was a hardcoded 414×736 @3.0 iPhone 6 Plus profile. Finally the game itself branches on a literal `415.0`-point width at every layout site in `MainMenuUI` and the `render:translation:pinchScale:` family to choose phone versus tablet layout, so correcting the geometry is not sufficient on its own - a large phone lands just over the line and gets inventory-on-the-right and a D-pad in portrait.

* Overrides `-[UIScreenMode scale]` and `-[UIScreenMode size]`, and the same pair on `UINativeScreenMode`, which declares its own and would otherwise miss the instance actually in use. Both are plain ivar reads, so the parent's implementation remains a valid fallback when geometry is unavailable and the stock profile applies. Everything downstream - `bounds`, `PIXEL_SCALE`, `EAGLView.framebufferWidth`/`framebufferHeight`, and the `World.tapViewport` used for unprojection - derives from these two accessors and therefore stays mutually consistent.
* Computes scale as `shortEdgePixels / smallestScreenWidthDp`, exported from `LibraryManager.initializeLibv` through Apportable's own `LibraryLoader.setenv` bridge. Android dp and iOS points share the same 160-per-inch reference, so the mapping is physically correct rather than a tuned constant: the tablet resolves to 753×1205 points, close to an iPad's 768×1024.
* Reports the size portrait-canonically, short edge as width, regardless of current orientation. iOS fixes `UIScreen` to portrait and rotates the window above it; reporting live orientation instead produced a doubly-rotated, clipped layout in landscape.
* Caps phones at 414 points wide, the iPhone 6 Plus profile and the widest the game's phone layouts were authored against, so a device that would cross the internal `415.0` threshold binds to the largest layout the game can actually render rather than falling through to tablet. Smaller phones keep dp-faithful sizing and are untouched.
* Computes geometry lazily on first accessor call rather than at load. `VerdePluginNativeWidth`/`Height` return zero until the GL surface exists, which is well after `BackgroundLibraryLoader$1` runs; the value latches on first success and is stable across rotation thereafter.
* Sets `TABLET` from the same `smallestScreenWidthDp >= 600` test, restoring the idiom the env-var mechanism was designed to carry. Written before `LibraryLoader.init()` and therefore before the ObjC runtime's `dispatch_once` reads it, so the intended path is used rather than patching the accessor.
* Replaces `SplashScreen.getSplashScreen`, which was a short-circuit chain testing `Default-568h@2x.png` first and stopping there. Since that file ships, no other candidate was ever reachable and both iPad launch images were dead weight. Selection is now by idiom, scale and orientation, ordered aspect-first within each column so a 1.775 asset is preferred over a 1.50 one on a tall display. Landscape borrows the iPad art on every device, phones included: iOS shipped no landscape launch image for iPhone because it launched portrait-only and rotated afterwards, so there is no upstream behaviour to match and cropping real landscape art beats edge-extending a portrait one.
* Factors the letterbox draw out of `show()` into `applyLetterbox` and calls it again from a new `reselect()`, hooked to `VerdeActivity.onConfigurationChanged`. The original selected once during `onCreate` and never revisited, so a device held sideways under rotation lock, or rotated while the splash was up, kept the launch-orientation image for the whole splash. `reselect()` is a no-op when the pick is unchanged and is wrapped so a failure leaves the existing splash rather than aborting launch.

### `join-link-fix.patch`
Includes a native library (`libjoinlinkfix.so`) that hooks Apportable's URL delivery path at startup and smali changes.

> **Notes from Claude:**
> Two independent faults. The manifest declared `android:scheme="theblockheads.net/join"`, which is not a valid scheme - schemes cannot contain `.` or `/` - so `blockheads://` links matched nothing, while web links fell through to a catch-all claiming every `http` URL on `theblockheads.net` and `blockheads.noodlecake.com`. Past that, `Java_..._nativeHandleUri` does not deliver anything: it calls `VerdeHandleURI`, which only stashes a retained copy of the `NSURL` in a native global. The handoff to the app delegate lives in a separate routine, latched by a one-shot flag, first run from inside `-[UIApplication run]`. At that point `UIApplication` has no delegate, so both `application:handleOpenURL:` and `application:openURL:sourceApplication:annotation:` are skipped - but the routine still executes `[stashedURL release]; stashedURL = nil;` unconditionally. The launch URL is destroyed and the latch closes. A resign-active event resets the latch and a become-active event re-runs the handoff, which is why the Join Server screen previously appeared only after the user triggered a focus change, such as pulling down and releasing the notification shade. Nothing else - surface changes, geometry callbacks, lifecycle transitions - had any effect, because nothing else touches that flag.

* Rewrites the broken intent filters. The invalid-scheme filter was also duplicated verbatim, and the catch-all it fell through to contained a stray malformed literal (`&quot;\10`) sitting loose inside the element. All of it is replaced with a proper `blockheads` scheme filter and `http`/`https` filters scoped to the `/join.php` path.
* Replaces `VerdeHandleURI` - an exported pointer-to-function variable, so no swizzle is required - with a handler that keeps its own retained copy of the URL. The engine's global is deliberately left nil, so its latched handoff becomes inert and cannot deliver the same URL a second time.
* Swizzles `-[GameView delayedOpenURLIfNeeded]`, which `-[GameView render:]` calls unconditionally as its first statement on every frame, so `self` is always the live `GameView`. The captured URL is pushed in through `-[GameView handleOpenURL:]`, setting `urlToOpenOnceLoaded`, and the original implementation consumes it on that same call. This bypasses the latch entirely rather than working around it, and uses only exported symbols - no hardcoded offsets - so it survives a rebuild of `libApplication.so`.
* Waits for `GameView.mainMenuUI` to exist before delivering, which lands on the second frame and leaves one frame of visible main menu. Delivering on the first is possible, since `delayedOpenURLIfNeeded` constructs a `MainMenuUI` itself when the ivar is nil, but it skips the `populateGameSaves` call that `render:` makes just before building the menu, and `render:` then sees a non-nil `mainMenuUI` and never populates - leaving an empty saved-worlds list. Calling `populateGameSaves` early to compensate fills the list but leaves the menu in a state that navigates away from the Join Server screen back to the world list. The one-frame delay is the cost of letting the game initialise in the order it was written to.
* Repairs a vanilla Apportable lifecycle bug in `Lifecycle.startNativeApplication`, which set `sNativeApplicationInBackground` directly instead of routing through `testInBackground()`. As a result `contextDidBecomeValid`, `applicationWillEnterForeground` and `applicationDidBecomeActive` were **never** delivered to the Objective-C app on a cold launch. Independent of deep links; surfaced while tracing this one.
* Also fixes links fired while the game is already in the foreground, and supports repeated links without a restart.

_Note: the game no longer registers as a handler for general `theblockheads.net` / `blockheads.noodlecake.com` URLs - only `/join.php` links. The old catch-all behaviour was almost certainly unintentional._

### `permissions-fix.patch`
Includes smali changes.

> **Notes from Claude:**
> `VerdeActivity.onCreate(Bundle)` guards `ActivityCompat.requestPermissions(..., WRITE_EXTERNAL_STORAGE, ...)` behind `ContextCompat.checkSelfPermission(...) == GRANTED || (!Build.VERSION.RELEASE.startsWith("6") && !Build.VERSION.RELEASE.startsWith("7.0"))`. The second disjunct was written to work around a specific device complaint circa the runtime-permissions rollout and hardcodes the two OS releases current at the time - it was never a check for "does this OS require a runtime request," which is what the condition needed to express. Every release outside `6.x`/exactly `7.0` satisfies the disjunct and takes the `startGame` branch without ever calling `requestPermissions`, so the permission stays in its install-time `not granted` state (confirmed via `dumpsys package`: `granted=false` with no `USER_SET`/`USER_FIXED` flag, i.e. never presented, not denied) and every write under `WRITE_EXTERNAL_STORAGE` fails silently. This includes Android 7.1 - one point release outside the intended window - through the current SDK 27 target and beyond. `onRequestPermissionsResult` already handles both grant and deny correctly and calls `startGame` in either case, so the handler was never the problem; the request simply never reached it on almost any real device.

* Removes the `Build.VERSION.RELEASE` string-prefix branch from the `checkSelfPermission` guard in `onCreate`. Once the permission is confirmed not granted, control now goes unconditionally to the existing `requestPermissions` call (`:cond_3`) instead of falling through to `startGame`; the dead version-check instructions are deleted rather than left unreachable, since unreachable smali has inconsistent verifier behavior across ART versions and a `VerifyError` in `onCreate` would be a boot failure.
* Leaves `onRequestPermissionsResult`, the request code (`133747173`), and the deny-path toast untouched - `startGame` is already called on both outcomes, so a denial does not block boot, it only leaves screenshots unavailable for that session.

_Note: The deny-path `Toast` message is long enough to clip at the two-line limit Android has enforced on `Toast` since API 26._

### `privacy-popup-cleanup.patch`
Includes a native library (`libprivacypopupfix.so`) that swizzles Apportable's Objective-C runtime at startup and smali changes.

> **Notes from Claude:**
> `-[GameView alertView:clickedButtonAtIndex:]` guards its `gdprStatus` write with `if (alertView == self->gdprPrompt)`, but the guard's closing brace falls before the alert construction, so `[[UIAlertView alloc] initWithTitle:@"Privacy Setting Changed" ...]` and `show` run unconditionally on every invocation. `UIAlertView` calls both `alertView:clickedButtonAtIndex:` and `alertView:didDismissWithButtonIndex:` on dismissal, and `GameView` is the delegate for roughly fifteen other alerts - disconnect, join-world, searching, tutorial, death confirmation, Game Center, server rejection - all of which are serviced by `didDismissWithButtonIndex:` and all of which fall through the mis-scoped guard in `clickedButtonAtIndex:`. The sibling handler disambiguates senders correctly for every alert it owns, comparing against a dedicated per-alert ivar and nilling it afterwards, so the GDPR addition breaks a convention the rest of the class follows. `-[EvolutionAppDelegate alertView:clickedButtonAtIndex:]` handles the first-launch consent prompt, writes the same `gdprStatus` key and calls `startThirdPartySDKs:`; it constructs no alert and needs no change.

* Swizzles `-[GameView alertView:clickedButtonAtIndex:]` to compare the incoming `alertView` against the `gdprPrompt` ivar and return early on a mismatch, restoring the guard the original code scopes too narrowly. Matching dismissals are forwarded to the original implementation unchanged, so both `gdprStatus` values and the restart notice behave exactly as intended on the real path.
* Nils `gdprPrompt` after a matched dismissal. `showGDPRAlert` assigns an autoreleased alert and never clears the ivar, so it dangles once the alert deallocates and a later `UIAlertView` allocated at the same address would compare equal and re-trigger the popup. Every branch of `alertView:didDismissWithButtonIndex:` already does this; the GDPR path does not.
* Resolves the `gdprPrompt` offset through `class_getInstanceVariable` and `ivar_getOffset` rather than a constant, reading the post-fixup value in case libobjc2 rewrites the non-fragile ivar offset at load time. Falls back to the statically derived `0x204` only if runtime introspection is unavailable, rejects implausible offsets, and logs an error if the runtime value disagrees with static analysis, which would indicate the binary differs from the one analysed. `OBJC_IVAR_$_GameView.gdprPrompt` is present in `.symtab` but not `.dynsym`, so `dlsym` is not a usable source.

_Note: Currently temporarily conflicts with other native patches._

### `rotation-fix.patch`
Includes a native library (`librotationfix.so`) that swizzles Apportable's Objective-C runtime at startup and smali changes.

> **Notes from Claude:**
> Apportable creates its view hierarchy on a dedicated `MainThread`, but Android delivers configuration changes on `main`. On rotation, `ActivityThread.handleActivityConfigurationChanged` → `ViewRootImpl.updateConfiguration` → `requestLayout` → `checkThread` compares the two and throws `CalledFromWrongThreadException`. The mechanism is device-independent; One UI appears to force a layout pass where AOSP returns early, so it only manifests on Samsung.
>
> Separately, `-[EvolutionViewController shouldAutorotate]` returns `NO` whenever Android's `accelerometer_rotation` setting is `0`. That gates `-[UIDevice _setOrientation:changed:]`, the only path that calls `_platform_setOrientation:`, the only writer of the global behind `-[UIApplication statusBarOrientation]`. `-[World acceleration:]` reads that global every accelerometer sample and applies its axis transforms only for orientations 2 and 4, so with rotation locked it stays `0`, both transforms are skipped, and tilt is stuck in the LandscapeLeft frame regardless of what the display shows. The value can't simply be corrected: `-[UIScreen _orientationChanged:]` reads `UIDevice.orientation` and the layout path reads `statusBarOrientation`, so setting either correctly reconfigures the render surface for a window that never rotated.
>
> A third failure hinges on `OrientationEventListener` reporting `ORIENTATION_UNKNOWN` when the device is near-horizontal. `SplashScreen$1` filters that out, so launching flat means no orientation is ever dispatched: the engine's hardcoded startup `_setOrientation:4` is never corrected, and the activity is never pinned via `_setRequestedOrientation`. The engine renders landscape in a portrait window, and because the activity stays `unspecified`, Android rotates it directly on sensor input - bypassing `-[World allowsRotation]`, which otherwise vetoes rotation while `requiresMotionEvents` is true. The window follows the device while the game doesn't.

* Catches the `CalledFromWrongThreadException` on a nested `Looper.loop()` and resumes, keyed to the exact exception class, `ViewRootImpl.checkThread` as frame 0, and an `ActivityThread.handleActivityConfigurationChanged*` frame, with a rate limit of 8 per 2000 ms. `checkThread` is the first statement in `requestLayout`, so nothing has mutated when it throws; the aborted work is one layout pass on a hierarchy that is a single GL surface the engine draws into itself. This suppresses one known-benign manifestation of views being owned by the wrong thread, not the ownership problem itself.
* Substitutes the window's own orientation for the engine's hardcoded startup `_setOrientation:4`, intercepted at `-[UIDevice _setOrientation:changed:]` - the only point upstream of `setStatusBarOrientation:`, the Java rotation request, and `[UIScreen _applyMode]`. One-shot, so a later user rotation to LandscapeRight passes through untouched. Normally the engine corrects the `4` itself ~150 ms later from the sensor path; when the device is flat that correction never arrives.
* Limits `SplashScreen$1` to one orientation dispatch while the splash is up. The engine cannot absorb an orientation change mid-initialization and lays out for one orientation in a window sized for another.
* Substitutes the locked orientation for `statusBarOrientation`, but only inside `-[World acceleration:]`, so the layout path still sees the real value. The substitution fires only when the real value is `0`, which is safe because it is absent-or-correct and never stale: the same condition that stops it updating - the rotation lock - also stops the window rotating.
* Derives the locked orientation from `Display.getRotation()` and `Configuration.orientation` at splash and pushes it to the library, covering the flat-launch case where no sensor dispatch occurs. The same value is passed to `_setRequestedOrientation`, pinning the activity so all subsequent rotation goes through the engine's `allowsRotation` veto instead of Android acting unilaterally.
* Removes `screenOrientation="portrait"` from `VerdeActivity` so the game can launch directly in landscape.
* Adds `screenOrientation="behind"` and `screenSize` to the `BlockheadsWebView` activity's manifest entry. `behind` makes it inherit `VerdeActivity`'s current orientation instead of resolving `unspecified` to the device's rotation lock. `screenSize` is defensive: the activity declares `orientation` in `configChanges` but not `screenSize`, which at `targetSdk` 27 should mean rotation destroys and recreates it - but it observably doesn't, and the reason hasn't been established. Declaring `screenSize` makes the intended behaviour explicit rather than relying on whatever is currently absorbing the change.

### `webview-rescue.patch`
Includes smali changes.

* Adds `onRenderProcessGone` callbacks to `WebDialog` and `BlockheadsWebView`.

### `webview-suspend-freeze-fix.patch`
Includes a native library (`libpaintmixfix.so`) that swizzles Apportable's Objective-C runtime at startup and smali changes.

> **Notes from Claude:**
> Apportable runs the Objective-C main runloop on a dedicated `MainThread` and creates its views there, so that thread's Looper is the one `ViewRootImpl` posts to. On pause, `MainThread` blocks in `GLSurfaceView.readySurface()` waiting on `mLock` for a surface change only the UI thread can deliver, while the UI thread blocks in `Activity.performStop()` -> `WindowManagerGlobal.setStoppedState()` -> `Handler.runWithScissors()` waiting for `MainThread`'s Looper to dispatch. Neither can proceed and Android raises an ANR after 5 seconds.
>
> The same transition causes a second, unrelated failure. Tearing down the EGL surface unbinds the context (`eglMakeCurrent` with `EGL_NO_SURFACE`), and the game constructs `PaintMixUI` in response to the picked image, so `-[PaintMixUI setWorkbench:blockhead:craftableItemObject:]` runs `loadResources` and `updateMix` while `eglGetCurrentContext()` is `EGL_NO_CONTEXT`. `glGenTextures` silently yields 0, and neither `CPTexture2D` initialiser checks it, so the panel keeps texture objects with dead GL names for the rest of the session. The context recovers on its own about half a second later, but nothing rebuilds the textures. `itemsTexture` is unaffected because `Items.png` is a cache hit from world load.

* Bounds the `mLock` wait in `readySurface()` to 16 ms, breaking the lock cycle; the tick action re-runs on the next tick.
* Bounds the waits in `surfaceCreated()` and `surfaceDestroyed()` to 250 ms, preserving the surface handshake while capping UI-thread exposure.
* Removes the `SDK_INT >= 19` `MainThread.getThread().interrupt()` call in `VerdeActivity.onPause()`. `MessageQueue.next()` retries on `EINTR` and never checks the Java interrupt flag, so it never woke `MainThread` and only left the flag set for later blocking calls to trip over.
* Removes the re-interrupt handlers in the `InterruptedException` catch blocks of `surfaceCreated()` / `surfaceDestroyed()`.
* `libpaintmixfix.so` swizzles `-[PaintMixUI render:translation:pinchScale:]` to repair any texture with a GL name of 0 once a context is current: file-backed textures are reloaded via `-[CPTexture2D updateForChangeToTexturePack]`, and the two painting textures, which have no `basePath`, are rebuilt via `-[PaintMixUI updateMix]`. `-[PaintMixUI setWorkbench:blockhead:craftableItemObject:]` is also swizzled to clear the repair latch when a new photo is picked, so a second pick in the same session is covered.

_Note: The texture repair happens after the fact rather than preventing creation in a contextless window; avoiding that would require changes beyond swizzling. The in-world easel (`Workbench.paintingTexture`) uses the same construction pattern but did not reproduce in testing and is left alone._

### `welcome-fix.patch`
Includes a native library (`libwelcomefix.so`) that swizzles Apportable's Objective-C runtime at startup and smali changes.

> **Notes from Claude:**
> There is no sanitizer anywhere on the send path. `nativeSetWelcomeMessage` takes the `EditText` contents through `GetStringUTFChars` (modified UTF-8, newline-transparent) into `+[NSString stringWithUTF8String:]`, and `-[World setNewWelcomeMessage:]` forwards to `-[BHClient sendNewWelcomeMessageToServer:]`, which packs a dictionary into a binary property list (`dataWithPropertyList:format:` with format 100) and gzips it. None of those stages touch the string, and reading the stored value back with server-side tools confirms a message saved from Android arrives with its newlines intact. The corruption is entirely on the display side: `-[GameView viewServerWelcomeMessage:customRules:allowEdit:]` composes an HTML page from `GameResources/instructions/server.html`, converting `"\n"` to `"<br/>"` (gated on the message containing no `"<"`) and then splitting the result on the inverse of an inline printable-ASCII 32..126 character set and rejoining with `@""` - which deletes every surviving newline along with all non-ASCII. `BlockheadsWebView.onCreate` then seeds the `EditText` from that composed page with `content.substring(26, length - 7)`, so the editor is populated with the *rendered* message rather than the stored one. Pressing DONE transmits whatever the editor holds, which is how `<br/>` literals reach the server and how newlines are destroyed there permanently. The bug is a round-trip degradation through the editor, not a transmission filter; iOS is unaffected because its editor is never seeded from rendered HTML.

* Suppresses the `"\n"` → `"<br/>"` conversion inside the compose window by hooking `-[NSString stringByReplacingOccurrencesOfString:withString:]` and returning the receiver when the arguments match those two constants. The game's `NSConstantString` literals are not pointer-identical to runtime-built strings, so the match is by `isEqualToString:`. Returning the receiver makes both sides of the game's own `rangeOfString:@"<"` gate behave identically, which moves the gate decision to the Java layer where it can be applied to the message alone.
* Suppresses the printable-ASCII filter by hooking `-[NSString componentsSeparatedByCharactersInSet:]` and returning `[NSArray arrayWithObject:self]` when the set is the inverted 32..126 one. The game sends `componentsJoinedByString:@""` to the result immediately, so a single-element array reconstructs the receiver unchanged. The set is identified by three probes - it must contain `'\n'` and must not contain `'A'` or `' '` - rather than by identity, since it is rebuilt inline on every call.
* Gates both hooks on a thread-local depth counter set by a wrapper on `viewServerWelcomeMessage:customRules:allowEdit:`. The runtime reports a single shared `Method` for `NSString`, `__NSCFString` and `__NSCFConstantString`, so `method_setImplementation` on `NSString` patches every string in the process; the counter confines the substitution to the composing thread inside the compose call, and the argument checks confine it further within that window.
* Verifies the type encoding of all three hooked methods before replacing any implementation, and refuses to install on a mismatch. `viewServerWelcomeMessage:customRules:allowEdit:` is `v20@0:4@8@12c16` - void return, two objects and a `BOOL` passed as `char`; the decompiler's apparent return value is a tail call into `objc_msgSend`. Wrong arity there would corrupt the stack on a method that runs every time the screen opens, and checking all three up front avoids leaving the process half-patched.
* Seeds the `EditText` from the verbatim message in `onCreate` and converts a separate copy for the `WebView`, replacing the original block that did both from the same string. The `substring(26, length - 7)` bounds are unchanged and still correct, because the native fix leaves the `<div id="messageContent">` wrapper intact; what changes is that the extracted text is now the stored message rather than its rendering. The original seeding block is removed rather than left in place, since it would otherwise re-seed the editor from the converted copy and undo the fix.
* Applies the same wrapping in the DONE handler, whose second assignment to `content` previously rebuilt the `<div>` by hand without conversion. The raw editor text still reaches `nativeSetWelcomeMessage` unchanged - that is the send path and was never at fault - so only the post-save re-render is affected. The handler never re-seeds the `EditText`, so a second save transmits the same bytes as the first.
* Keeps the game's own `"<"` gate on the conversion, applied in `wmWrapForDisplay` to the message alone. A message containing any tag is treated as hand-authored HTML where newlines are insignificant whitespace and `<br>` is the author's responsibility; without the gate, a message written with explicit `<br>` tags and newlines for source readability renders double-spaced on Android and single-spaced everywhere else. Rendering therefore matches vanilla; what the patch changes is what gets stored.
* Restores non-ASCII to welcome messages as a consequence of removing the filter. Every character outside 32..126 was previously deleted from any Android-viewed message, so accented text could not survive a view, let alone an edit. Non-BMP characters (emoji) still appear as `\UD83D`-style escapes, but that is the server escaping them before any client sees them, and is identical on iOS.

### `world-selection-fix.patch`
Includes a native library (`libworldselectionfix.so`) that swizzles Apportable's Objective-C runtime at startup.

> **Notes from Claude:**
> The resume path into game code is a linear chain with no branches: `Java_com_apportable_Lifecycle_applicationDidBecomeActive` schedules a global block on the main run loop, which posts `VerdeLifecycleApplicationDidBecomeActive`; `-[UIApplication _lifecycleEvent:]` is the sole observer and forwards to `-[EvolutionAppDelegate applicationDidBecomeActive:]`, which calls `[[self viewController] didBecomeActive]` on `GameView`. Nothing in the game observes either that notification or `UIApplicationDidBecomeActiveNotification` - every registration for the latter belongs to Apportable's own `CADisplayLink`, `UIAccelerometer`, and `GKLocalPlayer` code - so the delegate chain is the only entry. `-[GameView didBecomeActive]` then calls `-[MainMenuUI selectMostRecentlyPlayedWorld]` behind `totalGamePlayTimePassed >= 1.0 && mainMenuUI != nil`. The first term is a persisted lifetime counter loaded from `NSUserDefaults` in `-[GameView init]`, so for any returning player the only effective condition is that the menu object exists - true on every foreground transition. Whether iOS 1.7.5 shows the same reset is unverified; `applicationDidBecomeActive:` fires on every resume there too and the guard contains no platform check, so this is plausibly an original bug rather than an Apportable artifact.

* Suppresses `-[MainMenuUI selectMostRecentlyPlayedWorld]` outright. Despite the name it selects nothing: it writes `currentWorldIndex = -2`, `currentMainMenuSelection = 1`, `scrollTargetIndex = -3`, `activePreviewTextureIndex = -1`, and a `currentScroll` derived only from whether `gameSaves` is empty - byte for byte the same values `-[MainMenuUI initWithDelegate:windowInfo:cache:cloudInterface:]` already writes at construction. It is a reset-to-initial-state routine whose name reflects only that the default scroll position happens to land on the most recent world given the list ordering. Cold start therefore loses nothing, and `-[GameView didBecomeActive]` is its only sender in the binary.
* Leaves `-[MainMenuUI gameSavesChanged]`, which runs immediately before it on the same path, untouched. That method performs the genuinely necessary resume work - `updateWorldTitles`, releasing `activePreviewTexture` and resetting `activePreviewTextureIndex` to `-1`, and raising the `gameListChanged` flag - and only reads `currentWorldIndex`, never writes it. Suppressing the reset costs no cleanup.
* Guards nothing on `currentWorldIndex`, deliberately. The ivar is not stored state: `-[MainMenuUI render:projectionMatrix:]` recomputes it from `currentScroll` every frame, clamps it to `[-2, count-1]`, and drives `currentMainMenuSelection` from it, where `-1` and `-2` are the virtual Join and Create World slots below index 0. `-2` is thus both "freshly constructed" and "Create World is selected", so an index-based guard cannot tell a cold start from a resume on that screen - an earlier revision of this patch broke exactly there. The durable state is `currentScroll`; protecting it lets the next frame re-derive the same index and selection.
* Keeps `startSearchForCloudInfoFromWorldIndex:` unmodified. `gameSavesChanged` calls it as `currentWorldIndex - 10`, which under stock was always `-12` because the reset had just run, and the callee clamps negatives to 0. With the reset suppressed the argument becomes the user's real position - but the two call sites in `render:projectionMatrix:` already pass `currentWorldIndex - 10` and `currentWorldIndex + i - 10`, so position-relative cloud discovery is the existing mechanism, not something this patch introduces. Pinning the argument to 0 would have made one caller behave unlike the other three.

---

## How to apply the patches

### Prerequisites
You'll need [apktool](https://apktool.org) and [apksigner](https://developer.android.com/tools/apksigner) or any other utility that signs APKs.  
*(For Windows users, using **WSL** is recommended).*

### Steps

1. Grab the 1.7.5 APK from APKMirror or any other reputable source.
2. Decompile the APK using apktool: `apktool d <path/to/1.7.5.apk> -o patched_apk`
3. Navigate into `patched_apk` and download the `.patch` files there: `cd patched_apk && wget https://raw.githubusercontent.com/JarlPenguin/blockheads-android-patches/refs/heads/main/<patch.patch>`
4. Apply each of the patches: `patch -p1 < <patch.patch>`
5. Copy any required native libraries: `cp <patch>/libs/armeabi-v7a/<library.so> patched_apk/lib/armeabi-v7a`
6. Re-compile the APK: `cd .. && apktool b patched_apk -o patched-bh.apk`
7. Use `apksigner` or any other utility to sign the APK.
8. Install the APK on your device and enjoy!

---

## Disclaimer

Because applying these patches requires recompiling and signing the APK with a custom key, Google Play Games login will no longer work.

