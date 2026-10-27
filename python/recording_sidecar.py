"""Per-recording metadata file (`<recording>.json`), matching the C++ version's format.

Holds recording and encoding parameters plus the camera's settings at start and at stop.
Rewritten atomically, so it is always a complete, valid file even if the app crashes mid-recording.
"""

from __future__ import annotations

import datetime as dt
import json
import os
import subprocess
from pathlib import Path

import imagingcontrol4 as ic4


def now_iso() -> str:
    return dt.datetime.now().isoformat(timespec="milliseconds")


def app_version() -> str:
    """`git describe` of the source tree this script runs from."""
    try:
        return subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=Path(__file__).resolve().parent, capture_output=True, text=True, timeout=5,
        ).stdout.strip() or "unknown"
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"


def _summary(pm: ic4.PropertyMap) -> dict:
    """Key settings with their proper types. Properties a camera lacks are left out."""
    out: dict = {}

    def read(key: str, getter, name: str) -> None:
        try:
            out[key] = getter(name)
        except ic4.IC4Exception:
            pass

    read("width", pm.get_value_int, "Width")
    read("height", pm.get_value_int, "Height")
    read("pixel_format", pm.get_value_str, "PixelFormat")
    read("frame_rate_setting_fps", pm.get_value_float, "AcquisitionFrameRate")
    read("exposure_auto", pm.get_value_str, "ExposureAuto")
    read("exposure_time_us", pm.get_value_float, "ExposureTime")
    read("exposure_auto_upper_limit_us", pm.get_value_float, "ExposureAutoUpperLimit")
    read("exposure_auto_reference", pm.get_value_int, "ExposureAutoReference")
    read("gain_auto", pm.get_value_str, "GainAuto")
    read("gain_db", pm.get_value_float, "Gain")
    read("gain_auto_upper_limit_db", pm.get_value_float, "GainAutoUpperLimit")
    read("white_balance_auto", pm.get_value_str, "BalanceWhiteAuto")
    ratios = {}
    for channel in ("Red", "Green", "Blue"):
        try:
            pm.set_value("BalanceRatioSelector", channel)
            ratios[channel.lower()] = pm.get_value_float("BalanceRatio")
        except ic4.IC4Exception:
            pass
    if ratios:
        out["white_balance_ratios"] = ratios
    read("gamma", pm.get_value_float, "Gamma")
    read("black_level", pm.get_value_float, "BlackLevel")
    read("flip_horizontal", pm.get_value_bool, "ReverseX")
    read("flip_vertical", pm.get_value_bool, "ReverseY")
    read("sensor_temperature_c", pm.get_value_float, "DeviceTemperature")
    return out


def camera_snapshot(grabber: ic4.Grabber) -> dict | None:
    """Model, serial, key-settings summary and IC4's complete settings dump; None if no camera."""
    try:
        if not grabber.is_device_open:
            return None
        full = json.loads(bytes(grabber.device_save_state()).decode("utf-8"))
        summary = _summary(grabber.device_property_map)
    except (ic4.IC4Exception, ValueError):
        return None
    return {"captured_at": now_iso(), "device": full.get("device"), "summary": summary, "ic4_state": full}


def write(path: Path, data: dict) -> str | None:
    """Writes atomically. Returns an error message, or None on success."""
    tmp = path.with_name(path.name + ".tmp")
    try:
        tmp.write_text(json.dumps(data, indent=2))
        os.replace(tmp, path)
    except OSError as e:
        return str(e)
    return None
