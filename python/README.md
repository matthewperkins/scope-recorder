# Scope Recorder: Python version

The original, simpler version: IC4 (TIS's Python package) with PySide6 for the UI, and recording through an `ffmpeg` subprocess. NVENC is used when available, with x264 as the fallback. It has the same controls and output format as the [C++ version](../cpp/), but uses more CPU while recording (about 78% of one core at 1920×1200, 30 fps), and its video timing assumes the nominal frame rate.

## Requirements

- The TIS GenTL driver (see the [top-level README](../README.md))
- [uv](https://docs.astral.sh/uv/) and `ffmpeg`
- Python 3.12 or newer, which uv installs if needed. Python 3.10 must be avoided: PySide6 6.12 on 3.10 over-releases `None`, and the app aborts after a minute or two of live view.

## Run

```bash
cd python
uv run scope_recorder.py
```

## Controls

| Control | Shortcut | |
|---|---|---|
| Select camera… | | |
| Camera settings… | Ctrl+P | Exposure, gain, white balance, frame rate, gamma, flip |
| ● Record / ■ Stop | Ctrl+R | |
| Snapshot | Ctrl+S | Full-resolution PNG |
| Quality | Ctrl+E | Very high / High (default) / Standard / Compact / Very compact (capped at 3 Mbit/s) |
| Output folder… / Open folder | | |

Camera settings are saved to `camera_state.json` next to the script.

Each recording produces `<name>.mp4`, `<name>.frames.csv` and a `<name>.json` metadata sidecar, in the same format as the C++ version's (see [`cpp/README.md`](../cpp/README.md#output)). The `encoding` section reflects this version's pipeline (ffmpeg, timestamps at the nominal frame rate).

Note: IC4's own PySide6 dialogs are empty stubs in the Linux wheel, so this app ships its own settings dialog (`camera_settings.py`).
