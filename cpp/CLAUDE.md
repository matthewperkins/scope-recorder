# Scope Recorder (C++ / CUDA)

C++ port of the Python app in `../python/`, which defines the behaviour: live view, H.264 recording (quality presets, fragmented MP4, per-frame CSV), keyboard-driven camera settings, snapshot, output folder and status bar. Everything must be usable without a mouse. Reference hardware: TIS DFK 33UX174 (USB3 Vision, 1920×1200, BayerGB8), NVIDIA T400 (sm_75), Arch Linux on Wayland (Hyprland).

## Build and run

```bash
../scripts/fetch_ic4_sdk.sh path/to/ic4_<version>_amd64.tar.gz   # once: IC4 SDK into third_party/ic4
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/scope_recorder
./build/scope_recorder --benchmark 10 [--grab view.png] [--record] [--snapshot] [--output-dir DIR]   # fps + CPU, then quit
cmake --build build --target settings_dialog_test && ./build/settings_dialog_test [shot.png]   # hardware test: settings dialog
cmake --build build --target camera_reopen_test && ./build/camera_reopen_test                  # hardware test: Select camera / reconnect
```

Testing etiquette:
- Point test recordings at a fresh scratch directory per run (`mktemp -d`) with `--output-dir` (not persisted), so they never land in `~/Videos/ScopeRecordings` and nothing needs deleting. If anything must be deleted, guard variables: `rm -- "${DIR:?}"/...`.
- Stop a test instance by its own PID (`$!`), never `pkill`/`killall` by name: the user may have their own copy running, possibly mid-recording. Check with `pgrep -a -x scope_recorder` before a camera test.
- Only one program can open the camera at a time.

## Pipeline

IC4 QueueSink (raw BayerGB8, no CPU conversion) → pinned staging → GPU: Malvar-He-Cutler debayer (`GpuFrames.cu`) into a 3-slot RGBA ring (`Camera`) → live view copies GPU-to-GPU into a GL texture via CUDA-GL interop (`LiveView`). Recording (`Recorder`): on the camera thread, RGBA → NV12 (BT.709, video range) into an encoder-owned CUDA surface; a writer thread feeds h264_nvenc via libavcodec and muxes a fragmented MP4 with libavformat. No ffmpeg process, no CPU pixel copies. Video timestamps come from the camera's hardware clock (`device_timestamp_ns`), so playback timing stays true when frames drop.

Measured (30 fps, 1920×1200, i5-6600 + T400): live view 15.5% of one core; recording 18%. The Python version uses 27% and 78%.

## Lessons (verified on the reference hardware)

- The camera applies white balance and gamma to the raw Bayer data itself. The GPU path only debayers. All camera settings keep working.
- Do not set a global `QSurfaceFormat` (e.g. 3.3 core): on NVIDIA EGL + Wayland it fails with EGL_BAD_MATCH (3009), and the window never maps. The default context supports `#version 330 core`.
- Arch's Qt logs to journald when stderr isn't a tty. Use `QT_FORCE_STDERR_LOGGING=1` when running from scripts, or errors are invisible.
- `cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync)` before any CUDA call. Busy-spin sync otherwise costs ~5% of a core.
- All IC4 objects must be destroyed before `ic4::exitLibrary()`. Otherwise in-flight callbacks crash at exit.
- IC4's ready-made dialogs aren't usable on Linux. `CameraSettingsDialog` is our own port of the Python `camera_settings.py`.
- Tiling compositors (e.g. Hyprland) tile parentless dialogs and can force a size smaller than the layout needs; Qt then squashes rows flat. The settings dialog therefore scrolls, and dialogs should always get a parent.
- Auto gain may climb to 48 dB when the scene is dim (seen: 23 dB). That means more noise and roughly double the bitrate.
- Warm (halogen, ~3300 K) illumination drives auto white balance's blue gain near its maximum (~3.98), so blue is noisier.
- The 3-slot GPU ring assumes ~30 fps and consumers that copy within ~60 ms. Raise `kSlots` for high frame rates.
- The camera actually delivers ~29.77 fps at a 30 fps setting when auto exposure may use the full frame time. Never assume the nominal rate.
- Crash test (`kill -9` mid-recording): video plays up to the last completed fragment (keyframe every 2 s, so ~2-3 s lost). The CSV is flushed every 30 frames.
- When comparing live-view grabs with video frames, crop the letterbox geometrically (aspect ratio), not by brightness, and compare frames from the same moment. A sloppy comparison once produced a false "green tint".
- The debayer assumes the GBRG pattern of the IMX174 (DFK 33UX174). Other sensors need the phase in `GpuFrames.cu` adjusted.
- NVIDIA's Video Codec SDK (NVENC docs, `NvEncoderCuda` sample) is useful reference and a fallback if libavcodec proves limiting. Don't commit it; it's licensed by NVIDIA.

## Third-party

`third_party/ic4/` (gitignored): IC4 headers and `libic4core.so`, unpacked from TIS's .deb packages by `../scripts/fetch_ic4_sdk.sh`. They're proprietary and must not be committed. The TIS GenTL driver must be installed system-wide (see the top-level README).
