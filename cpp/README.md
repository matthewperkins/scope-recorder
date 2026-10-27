# Scope Recorder: C++ / CUDA version

Every pixel operation runs on the NVIDIA GPU: raw Bayer frames are uploaded, debayered (Malvar–He–Cutler), shown through CUDA–OpenGL interop, and encoded by NVENC through FFmpeg's libraries in-process, with no ffmpeg pipe. While recording at 1920×1200, 30 fps, this uses about 18% of one CPU core.

## Requirements

- NVIDIA GPU with NVENC, and the CUDA toolkit (`nvcc`)
- Qt 6 (Widgets, OpenGL, OpenGLWidgets), CMake ≥ 3.28, Ninja
- FFmpeg development libraries (libavcodec, libavformat, libavutil) built with `h264_nvenc`
- The TIS GenTL driver (see the [top-level README](../README.md))
- The IC4 SDK, unpacked into `cpp/third_party/ic4/`:

  ```bash
  scripts/fetch_ic4_sdk.sh path/to/ic4_<version>_amd64.tar.gz
  ```

  The SDK tarball is "IC Imaging Control 4 SDK" for Linux, from [TIS downloads](https://www.theimagingsource.com/en-us/support/download/).

On Arch: `sudo pacman -S cuda qt6-base qt6-wayland cmake ninja ffmpeg`.

## Build and run

```bash
cd cpp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/scope_recorder
```

CUDA code is built for the GPU in the build machine (`native`). Pass `-DCMAKE_CUDA_ARCHITECTURES=75` (for example) to target another GPU.

To add it to your application launcher, with its icon: `scripts/install_desktop_entry.sh`.

## Controls

| Control | Shortcut | |
|---|---|---|
| Select camera… | | Also reconnects after the camera was unplugged |
| Camera settings… | Ctrl+P | Exposure, gain, white balance, frame rate, gamma, flip |
| ● Record / ■ Stop | Ctrl+R | |
| Snapshot | Ctrl+S | Full-resolution PNG |
| Quality | Ctrl+E | Very high / High (default) / Standard / Compact / Very compact (capped at 3 Mbit/s) |
| Output folder… / Open folder | | |

Camera settings are saved to `~/.config/scope_recorder/camera_state.json`. The quality choice and output folder are shared with the Python version.

## Output

- `YYYY-MM-DD_HH-MM-SS.mp4`: H.264 High, 4:2:0, tagged BT.709, fragmented MP4. Frame timing follows the camera's hardware clock, so playback speed is exact even if frames drop.
- `YYYY-MM-DD_HH-MM-SS.frames.csv`: `video_frame, device_frame_number, device_timestamp_ns`. Gaps in `device_frame_number` mean dropped frames. Flushed every second.
- `YYYY-MM-DD_HH-MM-SS.json`: metadata sidecar. It holds `app` (name and git version it was built from); `recording` (start and stop times, status, frames written and dropped, nominal and measured fps, duration); `encoding` (codec, quality preset and cq, colour, container, timestamp source, debayer method); `camera_at_start` / `camera_at_end` (device model and serial, a `summary` of the key settings, and `ic4_state`, IC4's complete settings dump, which can be loaded back into the camera); and `setting_changes`, every setting changed in the settings dialog during the recording. Each entry has `time`, `recording_elapsed_s`, `issued_at_video_frame`, `after_device_timestamp_ns` (to align with the frames CSV), `setting` (e.g. `ExposureTime` in µs, `GainAuto`, `BalanceRatio[Red]`) and `value`. The camera applies a change a little after it is issued: +2 frames, measured on a DFK 33UX174. Values driven by auto modes (exposure, gain) drift without being logged; the start and end snapshots bracket them. The file is written atomically at start, after every logged change, and at stop. After a crash it still parses, with `"status": "recording"` and `camera_at_end` null.
- Snapshots: `YYYY-MM-DD_HH-MM-SS_mmm.png`.

The bitrate follows the scene. Sensor noise is expensive to encode, so a dim scene that drives auto gain up can double the file size at the same preset: more light, or a lower "Auto max gain", gives smaller and cleaner files. "Very compact" also caps the bitrate at 3000 kbit/s (2 s buffer, so brief peaks reach ~3.3 Mbit/s). In a noisy still scene it averaged 1.9 Mbit/s against 20.6 for "High"; edges stayed sharp and mostly the noise grain was lost. Expect visible smearing during fast motion.

## Testing

```bash
./build/scope_recorder --benchmark 10 [--record] [--snapshot] [--output-dir DIR]   # fps + CPU, then quit
cmake --build build --target settings_dialog_test camera_reopen_test
./build/settings_dialog_test      # hardware test: settings dialog drives the real camera
./build/camera_reopen_test        # hardware test: Select camera / reconnect
```

The hardware tests need the camera connected and the app closed.
