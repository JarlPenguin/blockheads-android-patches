# Patch The Blockheads APK for modern Android devices

These patches were designed with the help of Google Gemini for 1.7.5 and were tested on a device running Android 16.

List of patches and what they do:

1. `audio-record-keep-saves.patch`: Sets `allowAudioPlaybackCapture` and `hasFragileUserData` to true.

* `allowAudioPlaybackCapture` allows the game's audio to be captured when screen recording.
* `hasFragileUserData` allows the player to keep the game's data when uninstalling it.

2. `webview-suspend-freeze-fix.patch`: Fixes freezes when suspending and resuming the game as well as opening pages through the WebView (welcome messages and the Help/Credits page).

* Removes legacy `Thread.interrupt()` and infinite `wait()` loops in the Apportable engine's Java layer (`VerdeActivity` & `GLSurfaceView`).
* Quarantines the game's webview into a separate `:webview` process, shielding the legacy 32-bit C++ engine from modern Google Chrome memory restrictions.
* Adds `screenSize` to the manifest so rotating the device doesn't destroy and recreate the WebView UI.

Steps on how to apply the patches:
You'll need [apktool](https://apktool.org) and [apksigner](https://developer.android.com/tools/apksigner) or any other utility that signs APKs.

1. Grab the 1.7.5 APK from APKMirror or any other reputable source.
2. Decompile the APK using apktool: `apktool d <path/to/1.7.5.apk> -o patched_apk`
3. Navigate into `patched_apk` and download the `.patch` files there: `cd patched_apk && wget https://raw.githubusercontent.com/JarlPenguin/blockheads-android-patches/refs/heads/main/audio-record-keep-saves.patch && wget https://raw.githubusercontent.com/JarlPenguin/blockheads-android-patches/refs/heads/main/webview-suspend-freeze-fix.patch`
4. Apply the patches: `patch -p1 < audio-record-keep-saves.patch && patch -p1 < webview-suspend-freeze-fix.patch`
5. Re-compile the APK: `cd .. && apktool b patched_apk -o patched-bh.apk`
6. Use apksigner or any other utility to sign the APK.
7. Install the APK on your device and enjoy!

For Windows users I recommend using WSL.
