#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QMainWindow>
#include <QSettings>
#include <QTimer>

class Camera;
class LiveView;
class QToolBar;
class Recorder;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Camera* camera, QWidget* parent = nullptr);

    // Opens the camera and starts the stream; shows an error dialog on failure.
    void startCamera();

    // Measures for `seconds` (after a 3 s warm-up), prints results to stdout, then closes the window.
    // `grabFile`: save the live view's last rendered image there. `record`: record during the
    // measurement and report the file. `snapshot`: take a snapshot at the end and report it.
    void runBenchmark(double seconds, const QString& grabFile = {}, bool record = false, bool snapshot = false);

    // `remember` stores it for future sessions (shared with the Python version).
    void setOutputDir(const QString& dir, bool remember);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildToolbar();
    void buildStatusBar();
    void updateStatus();
    void updateActions();
    void onDeviceLost();

    void selectCamera();
    void showCameraSettings();
    void startRecording();
    void stopRecording();
    QString snapshot();  // returns the file path, or empty if no frame
    void chooseOutputDir();
    void openOutputDir();
    QString sizeAndBitrate(double seconds) const;

    Camera* camera_;
    LiveView* view_;
    // Shared with the Python version, so output folder and quality carry over.
    QSettings settings_{"ScopeRecorder", "ScopeRecorder"};
    QString outputDir_;
    std::shared_ptr<Recorder> recorder_;
    QString lastRecording_;

    QToolBar* toolbar_ = nullptr;
    QAction* selectAction_ = nullptr;
    QAction* settingsAction_ = nullptr;
    QAction* recordAction_ = nullptr;
    QAction* snapshotAction_ = nullptr;
    QAction* outputDirAction_ = nullptr;
    QAction* openDirAction_ = nullptr;
    QComboBox* quality_ = nullptr;

    QLabel* cameraLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* recordLabel_ = nullptr;
    QLabel* diskLabel_ = nullptr;
    QTimer statusTimer_;
    uint64_t lastReceived_ = 0, lastShown_ = 0;
    std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();
};
