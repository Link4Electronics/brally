# Top Gear Rally Remastered on Android

One APK: Top Gear Rally's menus with Boss Rally's engine running the races
(the same two games and Remaster Options as the Mac and Windows builds),
landscape, Vulkan, touch as the N64 pad.

- `app/` is the Android project: a plain `Activity` (`GameActivity.java`) with
  a `SurfaceView` Vulkan draws into, and the touch overlay (`TouchOverlay.java`)
  that turns fingers into the pad, the menus' taps and swipes, exactly as
  `../ios/Sources/TouchPad.swift` does on the iPhone. A controller is the
  second player's pad (`Gamepad.java`).
- `host_android.c` is the host under the two games (`host.h`): the surface,
  the event queue, AAudio out, MediaExtractor/MediaCodec for the CD's FLAC
  tracks (resampled to the mixer's rate), the pad, the app's folders, and the
  JNI the activity calls (`Native.java`).
- `build.sh` builds `libremastered.so` with the NDK's clang
  (`link.sh HOST=android RENDER=vulkan`, `-fPIC`, arm64-v8a, armeabi-v7a and
  x86_64), stages Boss Rally's data track and CD audio in the APK's assets,
  and runs Gradle.

## What it needs

- The Android NDK (r26 or later), `ANDROID_NDK_HOME` pointing at it; the SDK
  with API 34 and build-tools for Gradle. `minSdk` is 28 (AAudio and
  posix_spawn); Vulkan 1.0 is required (Android has no GL 3.3 core, so the GL
  renderer is not built here).
- Gradle 8.7+ and a JDK 17 (`JAVA_HOME`): `build.sh` uses a `gradlew` in this
  folder if there is one, else `gradle` from the PATH.
- Top Gear Rally (USA)'s ROM (the game's data is built in, as on the desktop)
  and Boss Rally's disc image (its data track and its CD audio, staged into
  the APK; none of the original files are in this repository).

```sh
TGR_ROM="/path/Top Gear Rally (USA).z64" \
ANDROID_NDK_HOME=/path/ndk/26.3.11579264 \
ports/remastered/android/build.sh
# build/remastered/android-vulkan/TopGearRallyRemastered.apk
```

`--bin FILE` names Boss Rally's disc image if it is not at
`reference/brally/BossRally.BIN`; `--release` builds Gradle's release variant
(sign it with `RM_KEYSTORE` and friends, or it is unsigned); `--no-build` uses
the libraries and assets already staged. `ABIS` limits the ABIs (default
`arm64-v8a armeabi-v7a x86_64`).

## Controls

- In a race, a finger on the right is the accelerator and the wheel (sideways
  turns; full lock at 90 dp), the left 30% brakes, and once the car has
  stopped, reverses. Elsewhere a tap chooses, a swipe moves.
- Two-finger tap: START/pause. Three-finger tap: the sound's low-pass filter
  (5, 6, 7, 8 kHz, off). The pause button at the top centre pauses a race.
- A game controller (Xbox/DirectInput layout) is player two's pad.
- Back closes the game (the process ends, as closing the window does on the
  desktop). Turning the screen off destroys the surface: the session ends.

## Files

The APK carries `assets/data`: Boss Rally's data track (`disc/`, less the
installer and the video), its CD audio (`music/cd/track02.flac` ...), and a
version stamp (`version.txt`). On first launch the activity copies them to its
files folder (`data/`); saves go to `saves/` beside them, outside the copy, so
a new build replaces the data without the saves. The game reads them through
`host_android.c`'s `host_cd_dir`, `host_music_dir` and `host_resource_dir`.
