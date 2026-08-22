# Patch The Blockheads APK for modern Android devices

These patches were designed with the help of LLMs for v1.7.5 and were tested on a device running Android 16.

## List of patches and what they do

### `audio-fix.patch`
Fixes music not resuming after backgrounding, music not looping, and music never starting when the game is launched while another app is playing audio. Includes a native library (`libaudiofix.so`) that swizzles Apportable's Objective-C runtime at startup, plus a smali change.

> **Notes from Claude:**
> `-[MJSoundManager restartMusicAfterActiveEvent:]` - the only music call `-[GameView didBecomeActive]` makes on resume - is an empty stub: a compiled prologue and epilogue with no body. It is empty because on iOS the audio-session interruption path handled resume. That path is intact on both sides here and never connects. `-[AVAudioPlayer(Platform) _platform_focus_changed:]` correctly maps Android focus constants onto `_beginInterruption`/`_endInterruption`, and `-[MJSoundManager reinitialize]` (reached from `initWithMasterVolume:`) does register for `AVAudioSessionInterruptionNotification`, but `-[AndroidAudioManager audioFocusChange:]` never fires and its `delegate` is nil, so nothing propagates. Apportable also sends the deprecated `AVAudioSessionDelegate` method `beginInterruption` where the game listens for the modern notification, so the two ends would not have met even with focus working. Meanwhile `-[AVAudioPlayer dealloc]` is the sole sender of `_platform_unload:`, which is what releases the Java `MediaPlayer` slot - so the player dies on background with nothing to restart it.

* Fills in the empty `restartMusicAfterActiveEvent:` with a call to the game's own `playMP3IfSafe:withTimeOffset:`, using the path retained from `lastPlayedMP3Path` and the position captured at teardown. Routing through the game's own loader means native retains ownership of the new `AVAudioPlayer`, so the in-game music volume slider continues to affect it - the defect in the earlier Java-side workaround, where the retained player was invisible to native.
* Captures the playback position in `-[AVAudioPlayer stop]`, which fires ~1 ms before `dealloc` on the same player. This is the only moment it is readable: `-[AVAudioPlayer currentTime]` returns the cached seek target from `setCurrentTime:` whenever `_platform_isPlaying:` is false, and by `dealloc` the Java `MediaPlayer` has already been stopped. The `isPlaying` check at capture time also distinguishes a genuine suspend from a track that ended naturally, in which case no restore is issued.
* Declines to restore when the game is mid-transition, keyed to `MJSoundManager.fadingOut` and to a dealloc occurring inside `loadMP3IfSafe:withTimeOffset:`. A dealloc in either window is the game replacing a track, not a suspend, and the position is meaningless. Any game-initiated load also drops a pending restore outright, since the game has decided what should be playing.
* Nils `MJSoundManager.mp3Player` after the player is deallocated. Nothing else does - the only writer that nils it is `stopMP3Playback`, which has no callers - so `setMusicVolume`, `isPlayingMP3`, `currentMP3time`, and `setLoopMP3s:` all read freed memory after every suspend in the unpatched game.
* Replaces `-[MJSoundManager safeToPlayMP3s]` with an unconditional true. The method is a one-line read of `otherAudioWasPaying`, which `reinitialize` latches once at startup from `isOtherAudioPlaying` and never recomputes, because the only path that would (`attemptToReinitializeAudio`) is unreachable. Launching while another app plays audio therefore muted game music for the entire session. The session category is `Ambient`, so mixing is the expected behaviour; the user's volume slider remains the control.
* Treats any nonzero loop count as infinite in `AudioPlayer.setNumberOfLoops`, which previously mapped only `-1` to `setLooping(true)`. The game requests 999 repetitions for looping tracks, so every looping track silently played once. `loadMP3IfSafe:withTimeOffset:` and `setLoopMP3s:` are the only senders of `setNumberOfLoops:` in the binary and pass only 999 or 0, so the mapping is complete for this app; a faithful finite count is not reachable through `MediaPlayer.setLooping(boolean)` anyway.

**Known limitations:** see https://github.com/JarlPenguin/blockheads-android-patches/issues/1.

_Note: Requires `webview-suspend-freeze-fix.patch` to be applied first, currently temporarily conflicts with other native patches._

### `audio-record-keep-saves.patch`:
Sets `allowAudioPlaybackCapture` and `hasFragileUserData` to true.

* `allowAudioPlaybackCapture` allows the game's audio to be captured when screen recording.
* `hasFragileUserData` allows the player to keep the game's data when uninstalling it.

### `dpi-fix.patch`
Fixes UI that renders at phone scale on tablets, and world-select art that overflows the screen on large phones. Includes a native library (`libdpifix.so`) that swizzles Apportable's Objective-C runtime at startup, plus smali changes to export the real display metrics and load it. Also replaces the splash screen's asset selection, which ignored device class, resolution and orientation entirely.

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
Fixes join links (`blockheads://?ip=...` and `theblockheads.net/join.php?...`) not working. Even with the AndroidManifest patched they still wouldn't work on cold boot - the game would launch but stop at the main menu instead of importing the server details and opening the Join Server screen.

> **Notes from Claude:**
> The legacy Apportable bridge hands the launch URL to the native runtime long before the Objective-C application object exists, and never delivers the lifecycle callbacks the game relies on to act on it. This patch repairs the manifest intent filters and rewires the Java-side delivery path so the URL reaches the game's `handleOpenURL:` at a point where it can actually be used.

* Rewrites the broken intent filters. The original declared `android:scheme="theblockheads.net/join"` - not a valid scheme, since schemes cannot contain `.` or `/` - so `blockheads://` links matched nothing, and this filter was duplicated verbatim. Web links instead fell through to a catch-all that claimed *every* `http` URL on `theblockheads.net` and `blockheads.noodlecake.com`, offering the game as a handler for unrelated pages, and which also contained a stray malformed literal (`&quot;\10`) sitting loose inside the element. All of it is replaced with a proper `blockheads` scheme filter and `http`/`https` filters scoped to the `/join.php` path.
* Defers the cold-boot URL instead of dropping it. `VerdeActivity$3` passed the launch URL to `nativeHandleUri` on the Android UI thread roughly half a second before the native application started, and two seconds before the Objective-C runloop began pumping - the call silently no-opped. The URL is now stashed and replayed on the engine's main thread once the runtime is confirmed live, mirroring the working warm-start sequence exactly.
* Repairs a vanilla Apportable lifecycle bug where `startNativeApplication` set its background flag directly instead of routing through `testInBackground()`, meaning `contextDidBecomeValid`, `applicationWillEnterForeground` and `applicationDidBecomeActive` were **never** delivered to the Objective-C app on a cold launch.
* Synthesises a window-focus off/on cycle a few engine ticks after delivery. The game buffers incoming URLs in `delayedOpenURLIfNeeded` and only drains that buffer on a focus-gained event - which never arrives on cold boot, since the window already holds focus. Without this, the Join Server screen only appeared if the user triggered a focus-change event (such as pulling down and releasing the notification shade). Timings are expressed in runloop ticks rather than milliseconds so they self-adjust across hardware.
* Also fixes links firing while the game is already in the foreground, which previously suffered the same stalled-buffer symptom, and supports repeated links without a restart.

_Note: the game no longer registers as a handler for general `theblockheads.net` / `blockheads.noodlecake.com` URLs - only `/join.php` links. The old catch-all behaviour was almost certainly unintentional._

### `privacy-popup-cleanup.patch`
Fixes the spurious "Privacy Setting Changed" dialog that appears during gameplay and menu navigation rather than only after an explicit privacy change. Includes a native library (`libprivacypopupfix.so`) that swizzles Apportable's Objective-C runtime at startup, plus a smali change to load it.

> **Notes from Claude:**
> `-[GameView alertView:clickedButtonAtIndex:]` guards its `gdprStatus` write with `if (alertView == self->gdprPrompt)`, but the guard's closing brace falls before the alert construction, so `[[UIAlertView alloc] initWithTitle:@"Privacy Setting Changed" ...]` and `show` run unconditionally on every invocation. `UIAlertView` calls both `alertView:clickedButtonAtIndex:` and `alertView:didDismissWithButtonIndex:` on dismissal, and `GameView` is the delegate for roughly fifteen other alerts - disconnect, join-world, searching, tutorial, death confirmation, Game Center, server rejection - all of which are serviced by `didDismissWithButtonIndex:` and all of which fall through the mis-scoped guard in `clickedButtonAtIndex:`. The sibling handler disambiguates senders correctly for every alert it owns, comparing against a dedicated per-alert ivar and nilling it afterwards, so the GDPR addition breaks a convention the rest of the class follows. `-[EvolutionAppDelegate alertView:clickedButtonAtIndex:]` handles the first-launch consent prompt, writes the same `gdprStatus` key and calls `startThirdPartySDKs:`; it constructs no alert and needs no change.

* Swizzles `-[GameView alertView:clickedButtonAtIndex:]` to compare the incoming `alertView` against the `gdprPrompt` ivar and return early on a mismatch, restoring the guard the original code scopes too narrowly. Matching dismissals are forwarded to the original implementation unchanged, so both `gdprStatus` values and the restart notice behave exactly as intended on the real path.
* Nils `gdprPrompt` after a matched dismissal. `showGDPRAlert` assigns an autoreleased alert and never clears the ivar, so it dangles once the alert deallocates and a later `UIAlertView` allocated at the same address would compare equal and re-trigger the popup. Every branch of `alertView:didDismissWithButtonIndex:` already does this; the GDPR path does not.
* Resolves the `gdprPrompt` offset through `class_getInstanceVariable` and `ivar_getOffset` rather than a constant, reading the post-fixup value in case libobjc2 rewrites the non-fragile ivar offset at load time. Falls back to the statically derived `0x204` only if runtime introspection is unavailable, rejects implausible offsets, and logs an error if the runtime value disagrees with static analysis, which would indicate the binary differs from the one analysed. `OBJC_IVAR_$_GameView.gdprPrompt` is present in `.symtab` but not `.dynsym`, so `dlsym` is not a usable source.

_Note: Currently temporarily conflicts with other native patches._

### `rotation-fix.patch`
Fixes crashes and orientation bugs triggered by device rotation, and restores tilt controls when the system rotation lock is on or the device is held flat at launch. Includes a native library (`librotationfix.so`) that swizzles Apportable's Objective-C runtime at startup, plus manifest and smali changes.

> **Notes from Claude:**
> Apportable creates its view hierarchy on a dedicated `MainThread`, but Android delivers configuration changes on `main`. On rotation, `ActivityThread.handleActivityConfigurationChanged` → `ViewRootImpl.updateConfiguration` → `requestLayout` → `checkThread` compares the two and throws `CalledFromWrongThreadException`. The mechanism is device-independent; One UI appears to force a layout pass where AOSP returns early, so it only manifests on Samsung.
>
> Separately, `-[EvolutionViewController shouldAutorotate]` returns `NO` whenever Android's `accelerometer_rotation` setting is `0`. That gates `-[UIDevice _setOrientation:changed:]`, the only path that calls `_platform_setOrientation:`, the only writer of the global behind `-[UIApplication statusBarOrientation]`. `-[World acceleration:]` reads that global every accelerometer sample and applies its axis transforms only for orientations 2 and 4, so with rotation locked it stays `0`, both transforms are skipped, and tilt is stuck in the LandscapeLeft frame regardless of what the display shows. The value can't simply be corrected: `-[UIScreen _orientationChanged:]` reads `UIDevice.orientation` and the layout path reads `statusBarOrientation`, so setting either correctly reconfigures the render surface for a window that never rotated.
>
> A third failure hinges on `OrientationEventListener` reporting `ORIENTATION_UNKNOWN` when the device is near-horizontal. `SplashScreen$1` filters that out, so launching flat means no orientation is ever dispatched: the engine's hardcoded startup `_setOrientation:4` is never corrected, and the activity is never pinned via `_setRequestedOrientation`. The engine renders landscape in a portrait window, and because the activity stays `unspecified`, Android rotates it directly on sensor input — bypassing `-[World allowsRotation]`, which otherwise vetoes rotation while `requiresMotionEvents` is true. The window follows the device while the game doesn't.

* Catches the `CalledFromWrongThreadException` on a nested `Looper.loop()` and resumes, keyed to the exact exception class, `ViewRootImpl.checkThread` as frame 0, and an `ActivityThread.handleActivityConfigurationChanged*` frame, with a rate limit of 8 per 2000 ms. `checkThread` is the first statement in `requestLayout`, so nothing has mutated when it throws; the aborted work is one layout pass on a hierarchy that is a single GL surface the engine draws into itself. This suppresses one known-benign manifestation of views being owned by the wrong thread, not the ownership problem itself.
* Substitutes the window's own orientation for the engine's hardcoded startup `_setOrientation:4`, intercepted at `-[UIDevice _setOrientation:changed:]` — the only point upstream of `setStatusBarOrientation:`, the Java rotation request, and `[UIScreen _applyMode]`. One-shot, so a later user rotation to LandscapeRight passes through untouched. Normally the engine corrects the `4` itself ~150 ms later from the sensor path; when the device is flat that correction never arrives.
* Limits `SplashScreen$1` to one orientation dispatch while the splash is up. The engine cannot absorb an orientation change mid-initialization and lays out for one orientation in a window sized for another.
* Substitutes the locked orientation for `statusBarOrientation`, but only inside `-[World acceleration:]`, so the layout path still sees the real value. The substitution fires only when the real value is `0`, which is safe because it is absent-or-correct and never stale: the same condition that stops it updating — the rotation lock — also stops the window rotating.
* Derives the locked orientation from `Display.getRotation()` and `Configuration.orientation` at splash and pushes it to the library, covering the flat-launch case where no sensor dispatch occurs. The same value is passed to `_setRequestedOrientation`, pinning the activity so all subsequent rotation goes through the engine's `allowsRotation` veto instead of Android acting unilaterally.
* Removes `screenOrientation="portrait"` from `VerdeActivity` so the game can launch directly in landscape.
* Adds `screenOrientation="behind"` and `screenSize` to the `BlockheadsWebView` activity's manifest entry. `behind` makes it inherit `VerdeActivity`'s current orientation instead of resolving `unspecified` to the device's rotation lock. `screenSize` is defensive: the activity declares `orientation` in `configChanges` but not `screenSize`, which at `targetSdk` 27 should mean rotation destroys and recreates it - but it observably doesn't, and the reason hasn't been established. Declaring `screenSize` makes the intended behaviour explicit rather than relying on whatever is currently absorbing the change.

_Note: Currently temporarily conflicts with other native patches._

### `webview-rescue.patch`
Fixes random WebDialog/WebView crashes taking down the game with themselves.

* When the WebDialog/WebView crashes, only it will, while the game itself will be unaffected.

### `webview-suspend-freeze-fix.patch`
Fixes fatal ANR freezes triggered by Android lifecycle transitions, such as suspending/resuming the game, opening WebView pages (welcome messages, Help/Credits page), or launching the photo picker. Also fixes black textures in the Easel paint-mixing UI after returning from the photo picker.

> **Notes from Claude:**
> Apportable runs the Objective-C main runloop on a dedicated `MainThread` and creates its views there, so that thread's Looper is the one `ViewRootImpl` posts to. On pause, `MainThread` blocks in `GLSurfaceView.readySurface()` waiting on `mLock` for a surface change only the UI thread can deliver, while the UI thread blocks in `Activity.performStop()` -> `WindowManagerGlobal.setStoppedState()` -> `Handler.runWithScissors()` waiting for `MainThread`'s Looper to dispatch. Neither can proceed and Android raises an ANR after 5 seconds.
>
> The same transition causes a second, unrelated failure. Tearing down the EGL surface unbinds the context (`eglMakeCurrent` with `EGL_NO_SURFACE`), and the game constructs `PaintMixUI` in response to the picked image, so `-[PaintMixUI setWorkbench:blockhead:craftableItemObject:]` runs `loadResources` and `updateMix` while `eglGetCurrentContext()` is `EGL_NO_CONTEXT`. `glGenTextures` silently yields 0, and neither `CPTexture2D` initialiser checks it, so the panel keeps texture objects with dead GL names for the rest of the session. The context recovers on its own about half a second later, but nothing rebuilds the textures. `itemsTexture` is unaffected because `Items.png` is a cache hit from world load.

* Bounds the `mLock` wait in `readySurface()` to 16 ms, breaking the lock cycle; the tick action re-runs on the next tick.
* Bounds the waits in `surfaceCreated()` and `surfaceDestroyed()` to 250 ms, preserving the surface handshake while capping UI-thread exposure.
* Removes the `SDK_INT >= 19` `MainThread.getThread().interrupt()` call in `VerdeActivity.onPause()`. `MessageQueue.next()` retries on `EINTR` and never checks the Java interrupt flag, so it never woke `MainThread` and only left the flag set for later blocking calls to trip over.
* Removes the re-interrupt handlers in the `InterruptedException` catch blocks of `surfaceCreated()` / `surfaceDestroyed()`.
* Adds `libpaintmixfix.so`, loaded from `BackgroundLibraryLoader` after `LibraryManager.loadLibraries()`. It swizzles `-[PaintMixUI render:translation:pinchScale:]` to repair any texture with a GL name of 0 once a context is current: file-backed textures are reloaded via `-[CPTexture2D updateForChangeToTexturePack]`, and the two painting textures, which have no `basePath`, are rebuilt via `-[PaintMixUI updateMix]`. `-[PaintMixUI setWorkbench:blockhead:craftableItemObject:]` is also swizzled to clear the repair latch when a new photo is picked, so a second pick in the same session is covered.

_Note: The texture repair happens after the fact rather than preventing creation in a contextless window; avoiding that would require changes beyond swizzling. The in-world easel (`Workbench.paintingTexture`) uses the same construction pattern but did not reproduce in testing and is left alone._

---

## How to apply the patches

### Prerequisites
You'll need [apktool](https://apktool.org) and [apksigner](https://developer.android.com/tools/apksigner) or any other utility that signs APKs.  
*(For Windows users, using **WSL** is recommended).*

### Steps

1. Grab the 1.7.5 APK from APKMirror or any other reputable source.
2. Decompile the APK using apktool: `apktool d <path/to/1.7.5.apk> -o patched_apk`
3. Navigate into `patched_apk` and download the `.patch` files there: `cd patched_apk && wget https://raw.githubusercontent.com/JarlPenguin/blockheads-android-patches/refs/heads/main/<patch1.patch> && wget https://raw.githubusercontent.com/JarlPenguin/blockheads-android-patches/refs/heads/main/<patch2.patch> && ...`
4. Apply the patches: `patch -p1 < <patch1.patch> && patch -p1 < <patch2.patch> && ...`
5. Re-compile the APK: `cd .. && apktool b patched_apk -o patched-bh.apk`
6. Use `apksigner` or any other utility to sign the APK.
7. Install the APK on your device and enjoy!

---

## Disclaimer

Because applying these patches requires recompiling and signing the APK with a custom key, Google Play Games login will no longer work.
