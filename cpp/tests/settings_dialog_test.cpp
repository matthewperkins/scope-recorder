// Hardware test: drives CameraSettingsDialog's widgets and checks the camera took the values.
// Needs the camera (close the app first). Restores the camera's previous settings at the end.
//
//   cmake --build build --target settings_dialog_test && ./build/settings_dialog_test [screenshot.png]

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QLabel>

#include <ic4/ic4.h>

#include "CameraSettingsDialog.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Finds the field whose form label starts with `label`.
template <typename W>
W* field(QWidget* dialog, const QString& label)
{
    for (auto* form : dialog->findChildren<QFormLayout*>())
        for (int row = 0; row < form->rowCount(); ++row) {
            auto* labelItem = form->itemAt(row, QFormLayout::LabelRole);
            auto* fieldItem = form->itemAt(row, QFormLayout::FieldRole);
            auto* l = labelItem ? qobject_cast<QLabel*>(labelItem->widget()) : nullptr;
            if (l && fieldItem && l->text().startsWith(label))
                return qobject_cast<W*>(fieldItem->widget());
        }
    fprintf(stderr, "no field '%s'\n", qPrintable(label));
    std::exit(2);
}

}  // namespace

int main(int argc, char* argv[])
{
    setenv("GENICAM_GENTL64_PATH", "/usr/lib/x86_64-linux-gnu/gentl/tis/", 1);
    QApplication app(argc, argv);
    ic4::initLibrary();
    {
        ic4::Grabber grabber;
        const auto devices = ic4::DeviceEnum::enumDevices();
        if (devices.empty() || !grabber.deviceOpen(devices.front())) {
            fprintf(stderr, "Could not open the camera (is the app running?)\n");
            return 2;
        }
        const auto savedState = grabber.deviceSaveState();
        auto props = grabber.devicePropertyMap();
        props.setValue("ExposureAuto", "Continuous");
        props.setValue("GainAuto", "Continuous");
        props.setValue("BalanceWhiteAuto", "Continuous");

        QWidget mainWindow;  // the app's dialog has a parent; compositors treat that as a floating dialog
        mainWindow.resize(800, 600);
        mainWindow.show();
        {
            CameraSettingsDialog dialog(props, false, &mainWindow);
            dialog.show();
            pump(800);
            expect(!field<QDoubleSpinBox>(&dialog, "Exposure time")->isEnabled(), "exposure time greyed out while auto");
            expect(!field<QDoubleSpinBox>(&dialog, "Gain")->isEnabled(), "gain greyed out while auto");
            expect(field<QDoubleSpinBox>(&dialog, "Frame rate")->isEnabled(), "frame rate editable when not recording");

            field<QCheckBox>(&dialog, "Auto exposure")->setChecked(false);
            pump(400);
            field<QDoubleSpinBox>(&dialog, "Exposure time")->setValue(10.0);
            field<QCheckBox>(&dialog, "Auto gain")->setChecked(false);
            auto* wbMode = field<QComboBox>(&dialog, "Mode");
            wbMode->setCurrentText("Off");
            emit wbMode->textActivated("Off");
            pump(400);
            field<QDoubleSpinBox>(&dialog, "Red ratio")->setValue(1.2);
            pump(800);

            expect(props.getValueString("ExposureAuto") == "Off", "auto exposure switched off on the camera");
            expect(std::abs(props.getValueDouble("ExposureTime") - 10000.0) < 50, "exposure time 10 ms on the camera");
            expect(props.getValueString("GainAuto") == "Off", "auto gain switched off on the camera");
            expect(props.getValueString("BalanceWhiteAuto") == "Off", "white balance set to manual on the camera");
            props.setValue("BalanceRatioSelector", "Red");
            expect(std::abs(props.getValueDouble("BalanceRatio") - 1.2) < 0.02, "red ratio 1.2 on the camera");
            expect(field<QDoubleSpinBox>(&dialog, "Exposure time")->isEnabled(), "exposure time editable in manual mode");
            if (argc > 1)
                dialog.grab().save(argv[1]);
        }
        {
            CameraSettingsDialog dialog(props, true, &mainWindow);
            dialog.show();
            pump(300);
            expect(!field<QDoubleSpinBox>(&dialog, "Frame rate")->isEnabled(), "frame rate locked while recording");
        }

        grabber.deviceClose();
        grabber.deviceOpenFromState(savedState);  // restore the user's settings
        grabber.deviceClose();
    }
    ic4::exitLibrary();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
