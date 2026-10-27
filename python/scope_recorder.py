"""Scope Recorder: live view + H.264 recording for The Imaging Source USB3 Vision cameras.

Run with:  uv run scope_recorder.py
"""

from __future__ import annotations

import csv
import datetime as dt
import gc
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

import imagingcontrol4 as ic4
from imagingcontrol4.pyside6.display import DisplayWidget
from camera_settings import CameraSettingsDialog, choose_device
from PySide6.QtCore import QObject, QSettings, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QAction, QDesktopServices, QKeySequence
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QLabel,
    QMainWindow,
    QMessageBox,
    QToolBar,
)

APP_DIR = Path(__file__).resolve().parent
CAMERA_STATE_FILE = APP_DIR / "camera_state.json"
DEFAULT_OUTPUT_DIR = Path.home() / "Videos" / "ScopeRecordings"
TIS_GENTL_DIR = "/usr/lib/x86_64-linux-gnu/gentl/tis/"
DEFAULT_FPS = 30.0
# Frames buffered between camera and encoder (~9 MB each at 1920x1200 BGRx).
MAX_QUEUED_FRAMES = 60


# Name -> NVENC constant-quality level. Lower = better quality and bigger files.
QUALITY_PRESETS = {"Very high": 16, "High": 19, "Standard": 23, "Compact": 27}
DEFAULT_QUALITY = "High"


def pick_encoder() -> str:
    """Prefer NVIDIA hardware encoding; fall back to CPU x264."""
    probe = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "nullsrc=s=640x480", "-frames:v", "1",
        "-c:v", "h264_nvenc", "-f", "null", "-",
    ]
    try:
        if subprocess.run(probe, capture_output=True, timeout=15).returncode == 0:
            return "h264_nvenc"
    except (OSError, subprocess.TimeoutExpired):
        pass
    return "libx264"


def encoder_args(encoder: str, quality: int) -> list[str]:
    """Constant-quality encoding: bitrate follows scene complexity rather than a fixed target."""
    if encoder == "h264_nvenc":
        # NVENC accepts BGRx directly and converts to 4:2:0 YUV on the GPU, which halves ffmpeg's CPU use.
        return ["-c:v", "h264_nvenc", "-preset", "p5", "-tune", "hq", "-rc", "vbr", "-cq", str(quality), "-b:v", "0"]
    # x264's CRF runs about one step lower than NVENC's CQ for similar quality.
    return ["-c:v", "libx264", "-preset", "veryfast", "-crf", str(quality - 1), "-pix_fmt", "yuv420p"]


class Recorder:
    """Pipes BGRx frames into ffmpeg on a background thread."""

    def __init__(self, path: Path, width: int, height: int, fps: float, encoder_args: list[str]):
        self.path = path
        self.fps = fps
        self.frames_written = 0
        self.frames_dropped = 0
        self.started = time.monotonic()
        self._queue: queue.Queue = queue.Queue(maxsize=MAX_QUEUED_FRAMES)
        self._log_path = path.with_suffix(".ffmpeg.log")
        self._log = open(self._log_path, "w")
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "warning", "-y",
            "-f", "rawvideo", "-pix_fmt", "bgr0", "-s", f"{width}x{height}",
            "-framerate", f"{fps:.3f}", "-i", "-",
            *encoder_args,
            "-g", str(max(1, round(fps * 2))),
            # Fragmented MP4 stays playable if the app or machine crashes mid-recording.
            "-movflags", "+frag_keyframe+empty_moov+default_base_moof",
            str(path),
        ]
        self._proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=self._log)
        self._csv_file = open(path.with_suffix(".frames.csv"), "w", newline="")
        self._csv = csv.writer(self._csv_file)
        self._csv.writerow(["video_frame", "device_frame_number", "device_timestamp_ns"])
        self.error: str | None = None
        self._thread = threading.Thread(target=self._run, name="recorder", daemon=True)
        self._thread.start()

    def submit(self, frame, frame_number: int, timestamp_ns: int) -> None:
        try:
            self._queue.put_nowait((frame, frame_number, timestamp_ns))
        except queue.Full:
            self.frames_dropped += 1

    def _run(self) -> None:
        while (item := self._queue.get()) is not None:
            frame, frame_number, timestamp_ns = item
            try:
                self._proc.stdin.write(frame.data)
            except (BrokenPipeError, OSError) as e:
                self.error = f"ffmpeg stopped unexpectedly ({e}); see {self._log_path.name}"
                break
            self._csv.writerow([self.frames_written, frame_number, timestamp_ns])
            self.frames_written += 1

    def stop(self) -> None:
        self._queue.put(None)
        self._thread.join()
        try:
            self._proc.stdin.close()
        except OSError:
            pass
        rc = self._proc.wait()
        self._csv_file.close()
        self._log.close()
        if rc == 0 and self.error is None and self._log_path.stat().st_size == 0:
            self._log_path.unlink()
        elif self.error is None:
            self.error = f"ffmpeg exited with code {rc}; see {self._log_path.name}"


class FrameListener(ic4.QueueSinkListener):
    """Runs on IC4's worker thread: hands frames to the recorder and snapshot saver."""

    def __init__(self):
        self.recorder: Recorder | None = None
        self.snapshot_path: Path | None = None
        self.frame_count = 0

    def sink_connected(self, sink, image_type, min_buffers_required) -> bool:
        sink.alloc_and_queue_buffers(min_buffers_required + 8)
        return True

    def sink_disconnected(self, sink) -> None:
        pass

    def frames_queued(self, sink) -> None:
        buf = sink.pop_output_buffer()
        self.frame_count += 1

        if (path := self.snapshot_path) is not None:
            self.snapshot_path = None
            # PNG encoding is slow; do it off the acquisition thread. The closure keeps buf alive.
            threading.Thread(target=lambda: buf.save_as_png(path), daemon=True).start()

        if (rec := self.recorder) is not None:
            meta = buf.meta_data
            rec.submit(buf.numpy_copy(), meta.device_frame_number, meta.device_timestamp_ns)


class DeviceEvents(QObject):
    device_lost = Signal()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Scope Recorder")
        self.resize(1280, 860)

        self.settings = QSettings("ScopeRecorder", "ScopeRecorder")
        self.output_dir = Path(str(self.settings.value("output_dir", str(DEFAULT_OUTPUT_DIR))))
        self.encoder = pick_encoder()

        self.grabber = ic4.Grabber()
        self.listener = FrameListener()
        self.sink = ic4.QueueSink(self.listener, [ic4.PixelFormat.BGRa8])
        self.events = DeviceEvents()
        self.events.device_lost.connect(self.on_device_lost)
        self.grabber.event_add_device_lost(lambda g: self.events.device_lost.emit())

        self.display_widget = DisplayWidget()
        self.display = self.display_widget.as_display()
        self.display.set_render_position(ic4.DisplayRenderPosition.STRETCH_CENTER)
        self.setCentralWidget(self.display_widget)

        self._build_toolbar()
        self._build_statusbar()

        self._last_count = 0
        self._last_tick = time.monotonic()
        self.timer = QTimer(self, interval=500, timeout=self.update_status)
        self.timer.start()

        self.open_initial_device()

    # ---------- UI ----------

    def _build_toolbar(self) -> None:
        tb = QToolBar("Main", movable=False)
        tb.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        self.addToolBar(tb)

        self.act_select = QAction("Select camera…", self, triggered=self.select_device)
        self.act_props = QAction("Camera settings…", self, triggered=self.show_properties)
        self.act_props.setShortcut(QKeySequence("Ctrl+P"))
        self.act_record = QAction("● Record", self, checkable=True, triggered=self.toggle_recording)
        self.act_record.setShortcut(QKeySequence("Ctrl+R"))
        self.act_snap = QAction("Snapshot", self, triggered=self.snapshot)
        self.act_snap.setShortcut(QKeySequence("Ctrl+S"))
        self.act_outdir = QAction("Output folder…", self, triggered=self.choose_output_dir)
        self.act_open = QAction("Open folder", self, triggered=self.open_output_dir)

        for a in (self.act_select, self.act_props):
            tb.addAction(a)
        tb.addSeparator()
        tb.addAction(self.act_record)
        tb.addAction(self.act_snap)
        tb.addSeparator()
        tb.addWidget(QLabel(" Quality: "))
        self.quality_combo = QComboBox()
        self.quality_combo.addItems(QUALITY_PRESETS)
        self.quality_combo.setCurrentText(str(self.settings.value("quality", DEFAULT_QUALITY)))
        self.quality_combo.setToolTip("Recording quality (Ctrl+E). Higher quality means bigger files.")
        self.quality_combo.currentTextChanged.connect(lambda q: self.settings.setValue("quality", q))
        tb.addWidget(self.quality_combo)
        act_quality = QAction(self, shortcut=QKeySequence("Ctrl+E"), triggered=self.quality_combo.showPopup)
        self.addAction(act_quality)
        tb.addSeparator()
        tb.addAction(self.act_outdir)
        tb.addAction(self.act_open)
        self.record_button = tb.widgetForAction(self.act_record)

    def _build_statusbar(self) -> None:
        sb = self.statusBar()
        self.lbl_camera = QLabel("No camera")
        self.lbl_fps = QLabel()
        self.lbl_rec = QLabel()
        self.lbl_disk = QLabel()
        sb.addWidget(self.lbl_camera)
        sb.addWidget(self.lbl_fps)
        sb.addWidget(self.lbl_rec, 1)
        sb.addPermanentWidget(self.lbl_disk)

    def update_status(self) -> None:
        now = time.monotonic()
        count = self.listener.frame_count
        fps = (count - self._last_count) / (now - self._last_tick)
        self._last_count, self._last_tick = count, now
        self.lbl_fps.setText(f"{fps:5.1f} fps" if self.grabber.is_streaming else "")

        rec = self.listener.recorder
        if rec is not None:
            if rec.error:
                self.stop_recording()
                return
            elapsed = int(now - rec.started)
            self.lbl_rec.setText(
                f"<b style='color:#d00'>● REC {elapsed // 3600:d}:{elapsed // 60 % 60:02d}:{elapsed % 60:02d}</b>"
                f"  {rec.path.name}  ({rec.frames_written} frames, {rec.frames_dropped} dropped)"
                f"  {self._size_and_bitrate(rec.path, now - rec.started)}"
            )

        try:
            free = shutil.disk_usage(self.output_dir if self.output_dir.exists() else Path.home()).free
            self.lbl_disk.setText(f"Free: {free / 1e9:.0f} GB  →  {self.output_dir}")
        except OSError:
            self.lbl_disk.setText(str(self.output_dir))

    @staticmethod
    def _size_and_bitrate(path: Path, seconds: float) -> str:
        try:
            size = path.stat().st_size
        except OSError:
            return ""
        mbit_s = size * 8 / seconds / 1e6 if seconds > 2 else 0.0
        return f"{size / 1e6:.0f} MB, {mbit_s:.1f} Mbit/s (~{mbit_s / 8 * 3.6:.1f} GB/h)"

    # ---------- device ----------

    def open_initial_device(self) -> None:
        if CAMERA_STATE_FILE.exists():
            try:
                self.grabber.device_open_from_state_file(CAMERA_STATE_FILE)
            except ic4.IC4Exception as e:
                print(f"Could not restore saved camera state: {e.message}", file=sys.stderr)

        if not self.grabber.is_device_open:
            devices = ic4.DeviceEnum.devices()
            if len(devices) == 1:
                try:
                    self.grabber.device_open(devices[0])
                    self._apply_first_run_defaults()
                except ic4.IC4Exception as e:
                    QMessageBox.critical(
                        self, "Camera unavailable",
                        f"{e.message}\n\nIs Scope Recorder already open in another window?",
                    )
            elif devices:
                self.select_device()
                return
        self.start_stream()

    def _apply_first_run_defaults(self) -> None:
        pm = self.grabber.device_property_map
        for prop, value in (
            (ic4.PropId.ACQUISITION_FRAME_RATE, DEFAULT_FPS),
            (ic4.PropId.EXPOSURE_AUTO, "Continuous"),
            (ic4.PropId.GAIN_AUTO, "Continuous"),
            (ic4.PropId.BALANCE_WHITE_AUTO, "Continuous"),
        ):
            try:
                pm.set_value(prop, value)
            except ic4.IC4Exception:
                pass

    def start_stream(self) -> None:
        if not self.grabber.is_device_open:
            self.lbl_camera.setText("No camera found — check the cable, then use “Select camera…”")
            self._update_actions()
            return
        try:
            self.grabber.stream_setup(self.sink, self.display)
        except ic4.IC4Exception as e:
            QMessageBox.critical(self, "Stream error", e.message)
        info = self.grabber.device_info
        w, h, _ = self._frame_geometry()
        self.lbl_camera.setText(f"{info.model_name}  #{info.serial}  {w}×{h}")
        self._update_actions()

    def _frame_geometry(self) -> tuple[int, int, float]:
        pm = self.grabber.device_property_map
        w = pm.get_value_int(ic4.PropId.WIDTH)
        h = pm.get_value_int(ic4.PropId.HEIGHT)
        try:
            fps = pm.get_value_float(ic4.PropId.ACQUISITION_FRAME_RATE)
        except ic4.IC4Exception:
            fps = DEFAULT_FPS
        return w, h, fps

    def select_device(self) -> None:
        if self.listener.recorder is not None:
            return
        if self.grabber.is_streaming:
            self.grabber.stream_stop()
        dev = choose_device(self)
        if dev is not None:
            try:
                if self.grabber.is_device_open:
                    self.grabber.device_close()
                self.grabber.device_open(dev)
                if not CAMERA_STATE_FILE.exists():
                    self._apply_first_run_defaults()
            except ic4.IC4Exception as e:
                QMessageBox.critical(self, "Camera error", e.message)
        self.start_stream()

    def show_properties(self) -> None:
        if not self.grabber.is_device_open:
            return
        CameraSettingsDialog(self.grabber, recording=self.listener.recorder is not None, parent=self).exec()
        self.save_camera_state()
        w, h, _ = self._frame_geometry()
        info = self.grabber.device_info
        self.lbl_camera.setText(f"{info.model_name}  #{info.serial}  {w}×{h}")

    def save_camera_state(self) -> None:
        if self.grabber.is_device_open:
            try:
                # IC4 1.5.3 requires a str here despite its type hints.
                self.grabber.device_save_state_to_file(str(CAMERA_STATE_FILE))
            except Exception as e:
                print(f"Could not save camera state: {e}", file=sys.stderr)

    def on_device_lost(self) -> None:
        self.stop_recording()
        self.lbl_camera.setText("<b style='color:#d00'>Camera disconnected</b> — reconnect, then use “Select camera…”")
        try:
            self.grabber.device_close()
        except ic4.IC4Exception:
            pass
        self._update_actions()
        QMessageBox.warning(self, "Camera disconnected", "The camera was disconnected. Any recording has been saved.")

    # ---------- recording ----------

    def toggle_recording(self, checked: bool) -> None:
        if checked:
            self.start_recording()
        else:
            self.stop_recording()

    def start_recording(self) -> None:
        if not self.grabber.is_streaming:
            self.act_record.setChecked(False)
            return
        self.output_dir.mkdir(parents=True, exist_ok=True)
        w, h, fps = self._frame_geometry()
        path = self.output_dir / f"{dt.datetime.now():%Y-%m-%d_%H-%M-%S}.mp4"
        try:
            quality = QUALITY_PRESETS[self.quality_combo.currentText()]
            self.listener.recorder = Recorder(path, w, h, fps, encoder_args(self.encoder, quality))
        except OSError as e:
            self.act_record.setChecked(False)
            QMessageBox.critical(self, "Recording error", f"Could not start ffmpeg: {e}")
            return
        self.save_camera_state()
        self.act_record.setText("■ Stop")
        self.record_button.setStyleSheet("background:#c00; color:white; font-weight:bold;")
        self._update_actions()

    def stop_recording(self) -> None:
        rec, self.listener.recorder = self.listener.recorder, None
        self.act_record.setChecked(False)
        self.act_record.setText("● Record")
        self.record_button.setStyleSheet("")
        self._update_actions()
        if rec is None:
            return
        rec.stop()
        if rec.error:
            self.lbl_rec.setText(f"<b style='color:#d00'>Recording failed:</b> {rec.error}")
            QMessageBox.critical(self, "Recording error", rec.error)
        else:
            self.lbl_rec.setText(
                f"Saved {rec.path.name}  ({rec.frames_written} frames, {rec.frames_dropped} dropped)  "
                f"{self._size_and_bitrate(rec.path, rec.frames_written / rec.fps)}"
            )

    def snapshot(self) -> None:
        if not self.grabber.is_streaming:
            return
        self.output_dir.mkdir(parents=True, exist_ok=True)
        path = self.output_dir / f"{dt.datetime.now():%Y-%m-%d_%H-%M-%S_%f}"[:-3]
        self.listener.snapshot_path = path.with_suffix(".png")
        self.statusBar().showMessage(f"Snapshot saved: {path.name}.png", 3000)

    def choose_output_dir(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "Recording folder", str(self.output_dir))
        if d:
            self.output_dir = Path(d)
            self.settings.setValue("output_dir", d)
            self.update_status()

    def open_output_dir(self) -> None:
        self.output_dir.mkdir(parents=True, exist_ok=True)
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(self.output_dir)))

    def _update_actions(self) -> None:
        recording = self.listener.recorder is not None
        streaming = self.grabber.is_device_open and self.grabber.is_streaming
        self.act_select.setEnabled(not recording)
        self.act_outdir.setEnabled(not recording)
        self.quality_combo.setEnabled(not recording)
        self.act_props.setEnabled(self.grabber.is_device_open)
        self.act_record.setEnabled(streaming or recording)
        self.act_snap.setEnabled(streaming)

    def closeEvent(self, event) -> None:
        if self.listener.recorder is not None:
            answer = QMessageBox.question(self, "Recording in progress", "Stop recording and quit?")
            if answer != QMessageBox.StandardButton.Yes:
                event.ignore()
                return
            self.stop_recording()
        self.save_camera_state()
        self.shutdown_camera()
        event.accept()

    def shutdown_camera(self) -> None:
        """Must run before IC4 is de-initialized, or in-flight frame callbacks abort the process."""
        try:
            if self.grabber.is_streaming:
                self.grabber.stream_stop()
            if self.grabber.is_device_open:
                self.grabber.device_close()
        except ic4.IC4Exception as e:
            print(f"Error closing camera: {e.message}", file=sys.stderr)


def main() -> None:
    # Use only the TIS driver; other vendors' GenTL producers on this PC (Vimba, pylon) are not needed.
    os.environ["GENICAM_GENTL64_PATH"] = TIS_GENTL_DIR
    with ic4.Library.init_context(api_log_level=ic4.LogLevel.INFO, log_targets=ic4.LogTarget.STDERR):
        app = QApplication(sys.argv)
        app.setApplicationName("Scope Recorder")
        win = MainWindow()
        win.show()
        try:
            app.exec()
        finally:
            if win.listener.recorder is not None:
                win.stop_recording()
            win.shutdown_camera()
            # Free IC4 objects (grabber, sink, display) while the library is still initialized.
            # Dropping them explicitly works even if something else still references the window.
            win.grabber = win.sink = win.display = None
            del win
            gc.collect()


if __name__ == "__main__":
    main()
