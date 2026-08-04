# Patch The Blockheads APK for modern Android devices

These patches were designed with the help of Google Gemini for v1.7.5 and were tested on a device running Android 16.

## List of patches and what they do

### `apportable-lifecycle-and-audio-fix.patch`
Fixes fatal Apportable engine freezes and audio bugs triggered by Android lifecycles, such as suspending/resuming the game, opening WebView pages (welcome messages, Help/Credits page), or launching the photo picker.

> **Notes from Gemini:**
> Because modern Android handles background lifecycles, WebView rendering, and hardware acceleration differently than older versions, the legacy Apportable cross-compiled engine frequently crashes, deadlocks, or loses memory pointers on newer devices. This patch natively modifies the Dalvik bytecode and Android Manifest to bypass these incompatibilities, forcing the Java layer to manually correct native JNI memory desyncs.

* Fixes fatal ANR (Application Not Responding) freezes when suspending/resuming the game.
* Removes legacy `Thread.interrupt()` and infinite GPU `wait()` loops in the Apportable engine's Java layer (`VerdeActivity` & `GLSurfaceView`).
* Adds `screenSize` to the manifest `configChanges` so rotating the device doesn't destroy and recreate the WebView UI.
* Implements a "Selective Shield" for the `MediaPlayer` that safely lets main menu music die during a suspend (avoiding native OpenSL ES audio blindspots), while intercepting native JNI kill orders to protect and keep in-game music alive.
* Bypasses broken vanilla engine flags by injecting brute-force Java commands into `lifecycleSuspend` and `lifecycleResume`, guaranteeing that surviving shielded tracks properly pause and wake up without relying on native engine state polling.
* Injects a cross-instance audio garbage collector into Dalvik to hunt down and violently execute orphaned tracks upon scene changes (e.g., exiting a world), preventing shielded in-game music from overlapping with the main menu music.

### `audio-record-keep-saves.patch`:
Sets `allowAudioPlaybackCapture` and `hasFragileUserData` to true.

* `allowAudioPlaybackCapture` allows the game's audio to be captured when screen recording.
* `hasFragileUserData` allows the player to keep the game's data when uninstalling it.

### `main-menu-music-loop-fix.patch`:
Fixes the main menu music not looping.

### `privacy-popup-cleanup.patch`
Fixes the "Privacy Setting Changed" popup appearing where irrelevant.

* Now it only appears when you actually change the privacy settings through the "Privacy Options..." button.

### `webview-rescue.patch`
Fixes random WebView crashes taking down the game with themselves.

* When the WebView crashes, only it will, while the game itself will be unaffected.

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
