#include <cstdlib>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QStandardPaths>

#include <ic4/ic4.h>

#include "Camera.h"
#include "GpuFrames.h"
#include "MainWindow.h"

namespace {

constexpr const char* kTisGentlDir = "/usr/lib/x86_64-linux-gnu/gentl/tis/";

QString cameraStateFile()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/camera_state.json";
}

}  // namespace

int main(int argc, char* argv[])
{
    // Use only the TIS driver; other vendors' GenTL producers on this PC (Vimba, pylon) are not needed.
    setenv("GENICAM_GENTL64_PATH", kTisGentlDir, 1);

    // Sleep instead of busy-spinning a CPU core while waiting for the GPU.
    CUDA_CHECK(cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync));

    QApplication app(argc, argv);
    QApplication::setApplicationName("scope_recorder");
    // Wayland app_id; matches ~/.local/share/applications/scope-recorder.desktop (icon, window rules).
    QGuiApplication::setDesktopFileName("scope-recorder");

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption benchmark("benchmark", "Run for <seconds>, print frame rates and CPU use, then quit.", "seconds");
    parser.addOption(benchmark);
    QCommandLineOption grab("grab", "With --benchmark: save the live view's last image to <file>.", "file");
    parser.addOption(grab);
    QCommandLineOption record("record", "With --benchmark: record during the measurement.");
    parser.addOption(record);
    QCommandLineOption snapshot("snapshot", "With --benchmark: take a snapshot at the end.");
    parser.addOption(snapshot);
    QCommandLineOption changeSettings("change-settings",
                                      "With --benchmark --record: change exposure via the settings dialog mid-recording.");
    parser.addOption(changeSettings);
    QCommandLineOption outputDir("output-dir", "Save recordings to <dir> for this session only.", "dir");
    parser.addOption(outputDir);
    parser.process(app);

    ic4::initLibrary();
    int result = 0;
    {
        // Everything holding IC4 objects must be destroyed before exitLibrary().
        Camera camera(cameraStateFile());
        MainWindow window(&camera);
        if (parser.isSet(outputDir))
            window.setOutputDir(parser.value(outputDir), false);
        window.show();
        window.startCamera();
        if (parser.isSet(benchmark))
            window.runBenchmark(parser.value(benchmark).toDouble(), parser.value(grab), parser.isSet(record),
                                parser.isSet(snapshot), parser.isSet(changeSettings));
        result = app.exec();
        camera.close();
    }
    ic4::exitLibrary();
    return result;
}
