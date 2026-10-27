#pragma once

#include <functional>
#include <string>
#include <vector>

#include <QDialog>
#include <QJsonValue>
#include <QVariant>
#include <QTimer>

#include <ic4/ic4.h>

class QFormLayout;
class QLabel;
class QVBoxLayout;

// Keyboard-friendly camera settings (port of the Python version's camera_settings.py).
// Tab moves between fields; arrow keys apply immediately; typed values apply on Enter; Esc closes.
// Values driven by an auto mode are shown live but greyed out.
class CameraSettingsDialog : public QDialog {
    Q_OBJECT
public:
    CameraSettingsDialog(ic4::PropertyMap properties, bool recording, QWidget* parent = nullptr);

    // Sets the field for camera property `setting` (as named in settingChanged, e.g. "ExposureTime"
    // in the field's display unit, or "BalanceRatio[Red]") exactly as a user's key press would.
    // Used by automated tests. Returns false if there is no such field.
    bool setField(const QString& setting, const QVariant& value);

signals:
    // A setting was changed by the user and accepted by the camera.
    void settingChanged(const QString& setting, const QJsonValue& value);

private:
    QFormLayout* addGroup(QVBoxLayout* layout, const QString& title);
    void addFloat(QFormLayout* form, const QString& label, const char* name, double scale = 1.0,
                  const QString& unit = {}, int decimals = 2, double step = 1.0, const QString& disabledReason = {});
    void addInt(QFormLayout* form, const QString& label, const char* name);
    void addBool(QFormLayout* form, const QString& label, const char* name);
    void addAutoCheckbox(QFormLayout* form, const QString& label, const char* name);
    void addEnum(QFormLayout* form, const QString& label, const char* name);
    void addBalanceRatio(QFormLayout* form, const char* channel);

    template <typename T>
    void write(const char* name, const T& value, const QString& setting = {});
    void refresh();

    struct Binding {
        QWidget* widget;
        std::function<void()> load;  // reads the camera value into the widget (signals blocked)
        std::string lockProperty;    // empty = enabled state managed elsewhere
        QString setting = {};        // name used in settingChanged / setField
    };

    ic4::PropertyMap props_;
    std::vector<Binding> bindings_;
    QLabel* temperature_;
    QLabel* status_;
    QTimer refreshTimer_;
};
