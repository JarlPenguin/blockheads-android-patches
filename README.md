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
