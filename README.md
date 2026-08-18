# Patch The Blockheads APK for modern Android devices

These patches were designed with the help of LLMs for v1.7.5 and were tested on a device running Android 16.

## List of patches and what they do

### `audio-fix.patch`
Fixes several audio bugs by loading a small native library (`libaudiofix.so`) that swizzles Apportable's Objective-C runtime at startup, plus one smali change:

* Music never resumes after backgrounding. The game's own `-[MJSoundManager restartMusicAfterActiveEvent:]` is an empty stub - on iOS the audio-session interruption path handled resume, and that path never fires on Android. The library fills the stub in, restoring the track at its previous playback position.
* Main menu music doesn't loop. Apportable's `AudioPlayer.setNumberOfLoops` only treats `-1` as "loop forever", so the game's request for 999 repetitions silently meant "play once".
* No music at all when launched while another app is playing audio. The game caches `otherAudioWasPaying` once at startup and never recomputes it, muting music for the rest of the session.
* Use-after-free on the music player. Nothing nils `MJSoundManager.mp3Player` after the player is deallocated, so volume and playback-state queries read freed memory after every suspend.

**Known limitations:** see https://github.com/JarlPenguin/blockheads-android-patches/issues/1.

_Note: Requires `webview-suspend-freeze-fix.patch` to be applied first._

### `audio-record-keep-saves.patch`:
Sets `allowAudioPlaybackCapture` and `hasFragileUserData` to true.

* `allowAudioPlaybackCapture` allows the game's audio to be captured when screen recording.
* `hasFragileUserData` allows the player to keep the game's data when uninstalling it.

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
Fixes the "Privacy Setting Changed" popup appearing where irrelevant.

* Now it only appears when you actually change the privacy settings through the "Privacy Options..." button.

### `rotation-fix.patch`
Fixes auto-rotation issues.

* Fixes crash on rotating the game on Samsung devices.
* Prevents the game from temporarily rotating to right-landscape on launch.
* Allows the game to be launched directly in landscape mode.

### `webview-rescue.patch`
Fixes random WebDialog/WebView crashes taking down the game with themselves.

* When the WebDialog/WebView crashes, only it will, while the game itself will be unaffected.

### `webview-suspend-freeze-fix.patch`
Fixes fatal ANR freezes triggered by Android lifecycle transitions, such as suspending/resuming the game, opening WebView pages (welcome messages, Help/Credits page), or launching the photo picker.

> **Notes from Claude:**
> Apportable runs the Objective-C main runloop on a dedicated `MainThread` and creates its views there, so that thread's Looper is the one `ViewRootImpl` posts to. On pause, `MainThread` blocks in `GLSurfaceView.readySurface()` waiting on `mLock` for a surface change only the UI thread can deliver, while the UI thread blocks in `Activity.performStop()` -> `WindowManagerGlobal.setStoppedState()` -> `Handler.runWithScissors()` waiting for `MainThread`'s Looper to dispatch. Neither can proceed and Android raises an ANR after 5 seconds.

* Bounds the `mLock` wait in `readySurface()` to 16 ms, breaking the lock cycle; the tick action re-runs on the next tick.
* Bounds the waits in `surfaceCreated()` and `surfaceDestroyed()` to 250 ms, preserving the surface handshake while capping UI-thread exposure.
* Removes the `SDK_INT >= 19` `MainThread.getThread().interrupt()` call in `VerdeActivity.onPause()`. `MessageQueue.next()` retries on `EINTR` and never checks the Java interrupt flag, so it never woke `MainThread` and only left the flag set for later blocking calls to trip over.
* Removes the re-interrupt handlers in the `InterruptedException` catch blocks of `surfaceCreated()` / `surfaceDestroyed()`.
* Adds `screenSize` to the manifest `configChanges` so rotating the device doesn't destroy and recreate the WebView UI.

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
