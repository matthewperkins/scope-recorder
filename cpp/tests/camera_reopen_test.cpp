// Hardware test of the "Select camera…" / reconnect path: Camera::open(&device) on an already
// streaming camera must cleanly close it, re-open it, rebuild GPU buffers and stream again.
// Needs the camera (close the app first).
//
//   cmake --build build --target camera_reopen_test && ./build/camera_reopen_test

#include <cstdio>
#include <cstdlib>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>

#include <ic4/ic4.h>

#include "Camera.h"
#include "GpuFrames.h"

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
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

}  // namespace

int main(int argc, char* argv[])
{
    setenv("GENICAM_GENTL64_PATH", "/usr/lib/x86_64-linux-gnu/gentl/tis/", 1);
    CUDA_CHECK(cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync));
    QCoreApplication app(argc, argv);
    ic4::initLibrary();
    {
        // Use the app's saved settings, read-only: the test never writes them.
        const QString state = QDir::homePath() + "/.config/scope_recorder/camera_state.json";
        Camera camera(state);
        expect(camera.open().isEmpty(), "initial open");
        pump(1500);
        const uint64_t first = camera.framesReceived();
        expect(first > 20, "frames arriving after initial open");
        expect(camera.latestFrame().sequence > 0, "GPU frame available");

        const auto devices = ic4::DeviceEnum::enumDevices();
        expect(!devices.empty(), "camera listed for selection");
        expect(camera.open(&devices.front()).isEmpty(), "re-open selected camera while streaming");
        pump(1500);
        const auto frame = camera.latestFrame();
        expect(camera.isStreaming(), "streaming after re-open");
        expect(camera.framesReceived() > first + 20, "frames arriving after re-open");
        expect(frame.sequence > 0 && frame.width == 1920 && frame.height == 1200, "GPU buffers rebuilt (1920x1200)");
        expect(!camera.grabLatestFrame().isNull(), "snapshot grab works after re-open");

        camera.close();
        expect(!camera.isStreaming() && camera.latestFrame().sequence == 0, "clean close");
    }
    ic4::exitLibrary();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
