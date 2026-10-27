#include "CameraSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSpinBox>
#include <QVBoxLayout>

CameraSettingsDialog::CameraSettingsDialog(ic4::PropertyMap properties, bool recording, QWidget* parent)
    : QDialog(parent), props_(std::move(properties))
{
    setWindowTitle(tr("Camera settings"));
    auto* outer = new QVBoxLayout(this);
    // If a tiling compositor gives the window less height than the content needs, scroll rather than squash.
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* exposure = addGroup(layout, tr("Exposure"));
    addAutoCheckbox(exposure, tr("Auto exposure"), "ExposureAuto");
    addFloat(exposure, tr("Exposure time"), "ExposureTime", 1e-3, "ms", 3, 0.5);
    addInt(exposure, tr("Auto brightness target"), "ExposureAutoReference");
    addFloat(exposure, tr("Auto max exposure"), "ExposureAutoUpperLimit", 1e-3, "ms", 1, 1);
    addBool(exposure, tr("Highlight reduction"), "ExposureAutoHighlightReduction");

    auto* gain = addGroup(layout, tr("Gain"));
    addAutoCheckbox(gain, tr("Auto gain"), "GainAuto");
    addFloat(gain, tr("Gain"), "Gain", 1.0, "dB", 1, 0.5);
    addFloat(gain, tr("Auto max gain"), "GainAutoUpperLimit", 1.0, "dB", 1, 1);

    auto* wb = addGroup(layout, tr("White balance"));
    addEnum(wb, tr("Mode"), "BalanceWhiteAuto");
    for (const char* channel : {"Red", "Green", "Blue"})
        addBalanceRatio(wb, channel);

    auto* image = addGroup(layout, tr("Image"));
    // The video's timing is set up when recording starts, so the rate can't change mid-recording.
    addFloat(image, tr("Frame rate"), "AcquisitionFrameRate", 1.0, "fps", 1, 1,
             recording ? tr("locked while recording") : QString());
    addFloat(image, tr("Gamma"), "Gamma", 1.0, {}, 2, 0.05);
    addFloat(image, tr("Black level"), "BlackLevel", 1.0, {}, 0, 1);
    addBool(image, tr("Flip horizontal"), "ReverseX");
    addBool(image, tr("Flip vertical"), "ReverseY");

    temperature_ = new QLabel;
    status_ = new QLabel;
    status_->setStyleSheet("color:#c00");
    status_->setWordWrap(true);
    layout->addWidget(temperature_);
    layout->addWidget(status_);
    layout->addStretch();
    scroll->setWidget(content);
    scroll->setMinimumWidth(content->sizeHint().width() + scroll->verticalScrollBar()->sizeHint().width());
    outer->addWidget(scroll);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    // Open at the content's natural size, capped to the screen.
    const int available = screen() ? screen()->availableGeometry().height() - 80 : 900;
    resize(scroll->minimumWidth() + 2 * outer->contentsMargins().left(),
           std::min(available, content->sizeHint().height() + buttons->sizeHint().height() + 3 * outer->spacing() +
                                   outer->contentsMargins().top() + outer->contentsMargins().bottom()));

    refresh();
    // Auto exposure, gain and white balance change values continuously; keep the display live.
    connect(&refreshTimer_, &QTimer::timeout, this, &CameraSettingsDialog::refresh);
    refreshTimer_.start(300);
}

QFormLayout* CameraSettingsDialog::addGroup(QVBoxLayout* layout, const QString& title)
{
    auto* box = new QGroupBox(title);
    auto* form = new QFormLayout(box);
    layout->addWidget(box);
    return form;
}

template <typename T>
void CameraSettingsDialog::write(const char* name, const T& value)
{
    ic4::Error err;
    if (props_.setValue(name, value, err))
        status_->clear();
    else
        status_->setText(QStringLiteral("%1: %2").arg(name, QString::fromStdString(err.message())));
    refresh();
}

void CameraSettingsDialog::addFloat(QFormLayout* form, const QString& label, const char* name, double scale,
                                    const QString& unit, int decimals, double step, const QString& disabledReason)
{
    ic4::Error err;
    auto prop = props_.findFloat(name, err);
    if (err.isError())
        return;
    auto* spin = new QDoubleSpinBox;
    spin->setKeyboardTracking(false);  // typed values apply on Enter, not per keystroke
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    spin->setRange(prop.minimum() * scale, prop.maximum() * scale);
    if (!unit.isEmpty())
        spin->setSuffix(" " + unit);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, name, scale](double v) { write(name, v / scale); });
    QString rowLabel = label;
    if (!disabledReason.isEmpty()) {
        spin->setEnabled(false);
        spin->setToolTip(disabledReason);
        rowLabel += QStringLiteral(" (%1)").arg(disabledReason);
    }
    form->addRow(rowLabel, spin);
    bindings_.push_back({spin,
                         [this, spin, name, scale] {
                             ic4::Error e;
                             const double v = props_.getValueDouble(name, e);
                             if (e.isSuccess())
                                 spin->setValue(v * scale);
                         },
                         disabledReason.isEmpty() ? name : ""});
}

void CameraSettingsDialog::addInt(QFormLayout* form, const QString& label, const char* name)
{
    ic4::Error err;
    auto prop = props_.findInteger(name, err);
    if (err.isError())
        return;
    auto* spin = new QSpinBox;
    spin->setKeyboardTracking(false);
    spin->setRange(int(prop.minimum()), int(prop.maximum()));
    connect(spin, &QSpinBox::valueChanged, this, [this, name](int v) { write(name, int64_t(v)); });
    form->addRow(label, spin);
    bindings_.push_back({spin,
                         [this, spin, name] {
                             ic4::Error e;
                             const auto v = props_.getValueInt64(name, e);
                             if (e.isSuccess())
                                 spin->setValue(int(v));
                         },
                         name});
}

void CameraSettingsDialog::addBool(QFormLayout* form, const QString& label, const char* name)
{
    ic4::Error err;
    props_.findBoolean(name, err);
    if (err.isError())
        return;
    auto* box = new QCheckBox;
    connect(box, &QCheckBox::toggled, this, [this, name](bool on) { write(name, on); });
    form->addRow(label, box);
    bindings_.push_back({box,
                         [this, box, name] {
                             ic4::Error e;
                             const bool v = props_.getValueBool(name, e);
                             if (e.isSuccess())
                                 box->setChecked(v);
                         },
                         name});
}

void CameraSettingsDialog::addAutoCheckbox(QFormLayout* form, const QString& label, const char* name)
{
    // An Off/Continuous enumeration shown as a checkbox.
    ic4::Error err;
    props_.findEnumeration(name, err);
    if (err.isError())
        return;
    auto* box = new QCheckBox;
    connect(box, &QCheckBox::toggled, this, [this, name](bool on) { write(name, on ? "Continuous" : "Off"); });
    form->addRow(label, box);
    bindings_.push_back({box,
                         [this, box, name] {
                             ic4::Error e;
                             const auto v = props_.getValueString(name, e);
                             if (e.isSuccess())
                                 box->setChecked(v != "Off");
                         },
                         name});
}

void CameraSettingsDialog::addEnum(QFormLayout* form, const QString& label, const char* name)
{
    ic4::Error err;
    auto prop = props_.findEnumeration(name, err);
    if (err.isError())
        return;
    auto* combo = new QComboBox;
    for (const auto& entry : prop.entries(err))
        if (entry.isAvailable(err))
            combo->addItem(QString::fromStdString(entry.name(err)));
    connect(combo, &QComboBox::textActivated, this,
            [this, name](const QString& text) { write(name, text.toStdString().c_str()); });
    form->addRow(label, combo);
    bindings_.push_back({combo,
                         [this, combo, name] {
                             ic4::Error e;
                             const auto v = props_.getValueString(name, e);
                             if (e.isSuccess())
                                 combo->setCurrentText(QString::fromStdString(v));
                         },
                         name});
}

void CameraSettingsDialog::addBalanceRatio(QFormLayout* form, const char* channel)
{
    // One BalanceRatio property, multiplexed by BalanceRatioSelector.
    ic4::Error err;
    props_.findEnumeration("BalanceRatioSelector", err);
    auto prop = props_.findFloat("BalanceRatio", err);
    if (err.isError())
        return;
    auto* spin = new QDoubleSpinBox;
    spin->setKeyboardTracking(false);
    spin->setDecimals(3);
    spin->setSingleStep(0.01);
    spin->setRange(prop.minimum(), prop.maximum());
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, channel](double v) {
        ic4::Error ignored;
        props_.setValue("BalanceRatioSelector", channel, ignored);
        write("BalanceRatio", v);
    });
    form->addRow(tr("%1 ratio").arg(channel), spin);
    bindings_.push_back({spin,
                         [this, spin, channel] {
                             ic4::Error e;
                             props_.setValue("BalanceRatioSelector", channel, e);
                             const double v = props_.getValueDouble("BalanceRatio", e);
                             if (e.isSuccess())
                                 spin->setValue(v);
                         },
                         "BalanceRatio"});
}

void CameraSettingsDialog::refresh()
{
    for (const auto& b : bindings_) {
        if (auto* spin = qobject_cast<QAbstractSpinBox*>(b.widget);
            spin && spin->hasFocus() && spin->findChild<QLineEdit*>()->isModified())
            continue;  // don't overwrite a value the user is typing
        {
            const QSignalBlocker block(b.widget);
            b.load();
        }
        if (!b.lockProperty.empty()) {
            // Values driven by an auto mode are locked; show them live but greyed out.
            ic4::Error e;
            const auto prop = props_.find(b.lockProperty, e);
            b.widget->setEnabled(e.isSuccess() && !prop.isReadOnly(e) && !prop.isLocked(e));
        }
    }
    ic4::Error e;
    const double temp = props_.getValueDouble("DeviceTemperature", e);
    temperature_->setText(e.isSuccess() ? tr("Sensor temperature: %1 °C").arg(temp, 0, 'f', 1) : QString());
}
