"""Keyboard-friendly camera settings dialog for IC4 cameras.

IC4's own PySide6 dialogs are empty stubs in the Linux wheel, so this replaces them.
Arrow keys apply immediately; typed values apply on Enter. Esc closes.
"""

from __future__ import annotations

import imagingcontrol4 as ic4
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QInputDialog,
    QLabel,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

BALANCE_CHANNELS = ("Red", "Green", "Blue")


class CameraSettingsDialog(QDialog):
    def __init__(self, grabber: ic4.Grabber, recording: bool, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Camera settings")
        self.pm = grabber.device_property_map
        self.recording = recording
        # (widget, getter, property name used for enabled/locked state)
        self._bindings: list[tuple[QWidget, callable, str]] = []

        layout = QVBoxLayout(self)

        exposure = self._group(layout, "Exposure")
        self._auto_checkbox(exposure, "Auto exposure", "ExposureAuto")
        self._float(exposure, "Exposure time", "ExposureTime", scale=1e-3, unit="ms", decimals=3, step=0.5)
        self._int(exposure, "Auto brightness target", "ExposureAutoReference")
        self._float(exposure, "Auto max exposure", "ExposureAutoUpperLimit", scale=1e-3, unit="ms", decimals=1, step=1)
        self._bool(exposure, "Highlight reduction", "ExposureAutoHighlightReduction")

        gain = self._group(layout, "Gain")
        self._auto_checkbox(gain, "Auto gain", "GainAuto")
        self._float(gain, "Gain", "Gain", unit="dB", decimals=1, step=0.5)
        self._float(gain, "Auto max gain", "GainAutoUpperLimit", unit="dB", decimals=1, step=1)

        wb = self._group(layout, "White balance")
        self._enum(wb, "Mode", "BalanceWhiteAuto")
        for channel in BALANCE_CHANNELS:
            self._balance_ratio(wb, channel)

        image = self._group(layout, "Image")
        self._float(image, "Frame rate", "AcquisitionFrameRate", unit="fps", decimals=1, step=1,
                    disabled_reason="locked while recording" if recording else None)
        self._float(image, "Gamma", "Gamma", decimals=2, step=0.05)
        self._float(image, "Black level", "BlackLevel", decimals=0, step=1)
        self._bool(image, "Flip horizontal", "ReverseX")
        self._bool(image, "Flip vertical", "ReverseY")

        self.temperature = QLabel()
        self.status = QLabel()
        self.status.setStyleSheet("color:#c00")
        self.status.setWordWrap(True)
        layout.addWidget(self.temperature)
        layout.addWidget(self.status)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        self.refresh()
        # Auto exposure/gain/white balance change values continuously; keep the display live.
        self._timer = QTimer(self, interval=300, timeout=self.refresh)
        self._timer.start()

    # ---------- widget builders ----------

    def _group(self, layout: QVBoxLayout, title: str) -> QFormLayout:
        box = QGroupBox(title)
        form = QFormLayout(box)
        layout.addWidget(box)
        return form

    def _find(self, name: str):
        try:
            return self.pm.find(name)
        except ic4.IC4Exception:
            return None

    def _set(self, name: str, value) -> None:
        try:
            self.pm.set_value(name, value)
            self.status.clear()
        except ic4.IC4Exception as e:
            self.status.setText(f"{name}: {e.message}")
        self.refresh()

    def _float(self, form, label, name, scale=1.0, unit="", decimals=2, step=1.0, disabled_reason=None):
        prop = self._find(name)
        if prop is None:
            return
        spin = QDoubleSpinBox()
        spin.setKeyboardTracking(False)  # typed values apply on Enter, not per keystroke
        spin.setDecimals(decimals)
        spin.setSingleStep(step)
        spin.setRange(prop.minimum * scale, prop.maximum * scale)
        if unit:
            spin.setSuffix(f" {unit}")
        spin.valueChanged.connect(lambda v: self._set(name, v / scale))
        if disabled_reason:
            spin.setEnabled(False)
            spin.setToolTip(disabled_reason)
            label = f"{label} ({disabled_reason})"
        form.addRow(label, spin)
        self._bindings.append((spin, lambda: self.pm.get_value_float(name) * scale, None if disabled_reason else name))

    def _int(self, form, label, name):
        prop = self._find(name)
        if prop is None:
            return
        spin = QSpinBox()
        spin.setKeyboardTracking(False)
        spin.setRange(prop.minimum, prop.maximum)
        spin.valueChanged.connect(lambda v: self._set(name, v))
        form.addRow(label, spin)
        self._bindings.append((spin, lambda: self.pm.get_value_int(name), name))

    def _bool(self, form, label, name):
        if self._find(name) is None:
            return
        box = QCheckBox()
        box.toggled.connect(lambda on: self._set(name, on))
        form.addRow(label, box)
        self._bindings.append((box, lambda: self.pm.get_value_bool(name), name))

    def _auto_checkbox(self, form, label, name):
        """An Off/Continuous enumeration shown as a checkbox."""
        if self._find(name) is None:
            return
        box = QCheckBox()
        box.toggled.connect(lambda on: self._set(name, "Continuous" if on else "Off"))
        form.addRow(label, box)
        self._bindings.append((box, lambda: self.pm.get_value_str(name) != "Off", name))

    def _enum(self, form, label, name):
        prop = self._find(name)
        if prop is None:
            return
        combo = QComboBox()
        combo.addItems([e.name for e in prop.entries if e.is_available])
        combo.textActivated.connect(lambda text: self._set(name, text))
        form.addRow(label, combo)
        self._bindings.append((combo, lambda: self.pm.get_value_str(name), name))

    def _balance_ratio(self, form, channel):
        if self._find("BalanceRatioSelector") is None:
            return
        prop = self._find("BalanceRatio")
        spin = QDoubleSpinBox()
        spin.setKeyboardTracking(False)
        spin.setDecimals(3)
        spin.setSingleStep(0.01)
        spin.setRange(prop.minimum, prop.maximum)

        def write(v, channel=channel):
            try:
                self.pm.set_value("BalanceRatioSelector", channel)
            except ic4.IC4Exception:
                pass
            self._set("BalanceRatio", v)

        def read(channel=channel):
            self.pm.set_value("BalanceRatioSelector", channel)
            return self.pm.get_value_float("BalanceRatio")

        spin.valueChanged.connect(write)
        form.addRow(f"{channel} ratio", spin)
        self._bindings.append((spin, read, "BalanceRatio"))

    # ---------- live refresh ----------

    def refresh(self) -> None:
        for widget, getter, name in self._bindings:
            if widget.hasFocus() and isinstance(widget, (QDoubleSpinBox, QSpinBox)) and widget.lineEdit().isModified():
                continue  # don't overwrite a value the user is typing
            try:
                value = getter()
            except ic4.IC4Exception:
                continue
            widget.blockSignals(True)
            if isinstance(widget, QCheckBox):
                widget.setChecked(value)
            elif isinstance(widget, QComboBox):
                widget.setCurrentText(value)
            else:
                widget.setValue(value)
            widget.blockSignals(False)
            if name is not None:
                prop = self._find(name)
                # Values driven by an auto mode are locked; show them live but greyed out.
                widget.setEnabled(prop is not None and not prop.is_readonly and not prop.is_locked)
        try:
            self.temperature.setText(f"Sensor temperature: {self.pm.get_value_float('DeviceTemperature'):.1f} °C")
        except ic4.IC4Exception:
            self.temperature.clear()


def choose_device(parent: QWidget | None = None) -> ic4.DeviceInfo | None:
    """Replacement for IC4's (stub) DeviceSelectionDialog."""
    devices = ic4.DeviceEnum.devices()
    if not devices:
        return None
    labels = [f"{d.model_name}  #{d.serial}" for d in devices]
    choice, ok = QInputDialog.getItem(parent, "Select camera", "Camera:", labels, 0, False)
    return devices[labels.index(choice)] if ok else None
