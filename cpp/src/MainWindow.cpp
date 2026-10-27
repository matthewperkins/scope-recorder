#include "MainWindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeySequence>
#include <QMessageBox>
#include <QStatusBar>
#include <QStorageInfo>
#include <QThreadPool>
#include <QToolBar>
#include <QUrl>

#include <ic4/ic4.h>

#include "Camera.h"
#include "CameraSettingsDialog.h"
#include "LiveView.h"
#include "Recorder.h"
#include "RecordingSidecar.h"

#include <unistd.h>

namespace {

// Name -> NVENC constant-quality level, matching the Python version.
const QList<QPair<QString, int>> kQualityPresets = {
    {"Very high", 16}, {"High", 19}, {"Standard", 23}, {"Compact", 27}};
const QString kDefaultQuality = "High";

QString timestampedName(const QString& format)
{
    return QDateTime::currentDateTime().toString(format);
}

}  // namespace

MainWindow::MainWindow(Camera* camera, QWidget* parent)
    : QMainWindow(parent), camera_(camera), view_(new LiveView(camera, this))
{
    setWindowTitle(tr("Scope Recorder"));
    resize(1280, 860);
    setCentralWidget(view_);
    outputDir_ = settings_.value("output_dir", QDir::homePath() + "/Videos/ScopeRecordings").toString();

    buildToolbar();
    buildStatusBar();

    connect(camera_, &Camera::deviceLost, this, &MainWindow::onDeviceLost, Qt::QueuedConnection);
    connect(&statusTimer_, &QTimer::timeout, this, &MainWindow::updateStatus);
    statusTimer_.start(500);
    updateActions();
}

// ---------- UI ----------

void MainWindow::buildToolbar()
{
    toolbar_ = addToolBar(tr("Main"));
    toolbar_->setMovable(false);

    selectAction_ = toolbar_->addAction(tr("Select camera…"), this, &MainWindow::selectCamera);
    settingsAction_ = toolbar_->addAction(tr("Camera settings…"), this, &MainWindow::showCameraSettings);
    settingsAction_->setShortcut(QKeySequence("Ctrl+P"));
    toolbar_->addSeparator();

    recordAction_ = toolbar_->addAction(tr("● Record"));
    recordAction_->setCheckable(true);
    recordAction_->setShortcut(QKeySequence("Ctrl+R"));
    connect(recordAction_, &QAction::triggered, this, [this](bool on) { on ? startRecording() : stopRecording(); });
    snapshotAction_ = toolbar_->addAction(tr("Snapshot"), this, [this] { snapshot(); });
    snapshotAction_->setShortcut(QKeySequence("Ctrl+S"));

    toolbar_->addSeparator();
    toolbar_->addWidget(new QLabel(tr(" Quality: ")));
    quality_ = new QComboBox;
    for (const auto& [name, level] : kQualityPresets)
        quality_->addItem(name, level);
    quality_->setCurrentText(settings_.value("quality", kDefaultQuality).toString());
    quality_->setToolTip(tr("Recording quality (Ctrl+E). Higher quality means bigger files."));
    connect(quality_, &QComboBox::currentTextChanged, this, [this](const QString& q) { settings_.setValue("quality", q); });
    toolbar_->addWidget(quality_);
    auto* qualityShortcut = new QAction(this);
    qualityShortcut->setShortcut(QKeySequence("Ctrl+E"));
    connect(qualityShortcut, &QAction::triggered, quality_, &QComboBox::showPopup);
    addAction(qualityShortcut);

    toolbar_->addSeparator();
    outputDirAction_ = toolbar_->addAction(tr("Output folder…"), this, &MainWindow::chooseOutputDir);
    openDirAction_ = toolbar_->addAction(tr("Open folder"), this, &MainWindow::openOutputDir);
}

void MainWindow::buildStatusBar()
{
    cameraLabel_ = new QLabel(tr("No camera"));
    fpsLabel_ = new QLabel;
    recordLabel_ = new QLabel;
    diskLabel_ = new QLabel;
    statusBar()->addWidget(cameraLabel_);
    statusBar()->addWidget(fpsLabel_);
    statusBar()->addWidget(recordLabel_, 1);
    statusBar()->addPermanentWidget(diskLabel_);
}

void MainWindow::updateActions()
{
    const bool recording = recorder_ != nullptr;
    const bool streaming = camera_->isStreaming();
    selectAction_->setEnabled(!recording);
    settingsAction_->setEnabled(streaming);
    recordAction_->setEnabled(streaming || recording);
    snapshotAction_->setEnabled(streaming);
    quality_->setEnabled(!recording);
    outputDirAction_->setEnabled(!recording);
}

void MainWindow::updateStatus()
{
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - lastTick_).count();
    const uint64_t received = camera_->framesReceived();
    const uint64_t shown = view_->framesShown();
    if (camera_->isStreaming()) {
        fpsLabel_->setText(tr("%1 fps (shown %2)")
                               .arg((received - lastReceived_) / dt, 5, 'f', 1)
                               .arg((shown - lastShown_) / dt, 5, 'f', 1));
        cameraLabel_->setText(camera_->description());
    } else {
        fpsLabel_->clear();
    }
    lastReceived_ = received;
    lastShown_ = shown;
    lastTick_ = now;

    if (recorder_) {
        if (!recorder_->error().empty()) {
            stopRecording();
            return;
        }
        const double elapsed = std::chrono::duration<double>(now - recorder_->started()).count();
        const int s = int(elapsed);
        recordLabel_->setText(tr("<b style='color:#d00'>● REC %1:%2:%3</b>  %4  (%5 frames, %6 dropped)  %7")
                                  .arg(s / 3600)
                                  .arg(s / 60 % 60, 2, 10, QChar('0'))
                                  .arg(s % 60, 2, 10, QChar('0'))
                                  .arg(QFileInfo(QString::fromStdString(recorder_->path())).fileName())
                                  .arg(recorder_->framesWritten())
                                  .arg(recorder_->framesDropped())
                                  .arg(sizeAndBitrate(elapsed)));
    }

    const QStorageInfo storage(QDir(outputDir_).exists() ? outputDir_ : QDir::homePath());
    diskLabel_->setText(tr("Free: %1 GB  →  %2").arg(storage.bytesAvailable() / 1e9, 0, 'f', 0).arg(outputDir_));
    updateActions();
}

// ---------- camera ----------

void MainWindow::startCamera()
{
    const QString error = camera_->open();
    if (!error.isEmpty()) {
        cameraLabel_->setText(tr("No camera — check the cable, then use “Select camera…”"));
        QMessageBox::critical(this, tr("Camera unavailable"), error);
    }
    updateActions();
}

void MainWindow::selectCamera()
{
    if (recorder_)
        return;
    ic4::Error err;
    const auto devices = ic4::DeviceEnum::enumDevices(err);
    if (devices.empty()) {
        QMessageBox::warning(this, tr("Select camera"), tr("No camera found. Check the USB 3 cable."));
        return;
    }
    QStringList labels;
    for (const auto& d : devices)
        labels << QStringLiteral("%1  #%2").arg(QString::fromStdString(d.modelName(err)), QString::fromStdString(d.serial(err)));
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, tr("Select camera"), tr("Camera:"), labels, 0, false, &ok);
    if (!ok)
        return;
    camera_->saveState();
    const QString error = camera_->open(&devices[labels.indexOf(choice)]);
    if (!error.isEmpty())
        QMessageBox::critical(this, tr("Camera unavailable"), error);
    updateActions();
}

void MainWindow::showCameraSettings()
{
    if (!camera_->isStreaming())
        return;
    CameraSettingsDialog dialog(camera_->properties(), recorder_ != nullptr, this);
    dialog.exec();
    camera_->saveState();
}

void MainWindow::onDeviceLost()
{
    stopRecording();
    camera_->close();
    cameraLabel_->setText(tr("<b style='color:#d00'>Camera disconnected</b> — reconnect, then use “Select camera…”"));
    updateActions();
    QMessageBox::warning(this, tr("Camera disconnected"),
                         tr("The camera was disconnected. Any recording has been saved.\n\n"
                            "Reconnect it, then use “Select camera…”."));
}

// ---------- recording and snapshots ----------

void MainWindow::startRecording()
{
    const GpuFrame frame = camera_->latestFrame();
    if (!camera_->isStreaming() || frame.sequence == 0) {
        recordAction_->setChecked(false);
        return;
    }
    QDir().mkpath(outputDir_);
    Recorder::Settings s;
    s.path = (outputDir_ + "/" + timestampedName("yyyy-MM-dd_HH-mm-ss") + ".mp4").toStdString();
    s.width = frame.width;
    s.height = frame.height;
    s.fps = camera_->frameRate();
    s.quality = quality_->currentData().toInt();

    std::string error;
    recorder_ = Recorder::start(s, error);
    if (!recorder_) {
        recordAction_->setChecked(false);
        QMessageBox::critical(this, tr("Recording error"), QString::fromStdString(error));
        return;
    }
    camera_->setConsumer(recorder_);
    camera_->saveState();

    sidecarPath_ = QString::fromStdString(s.path).chopped(4) + ".json";
    sidecar_ = QJsonObject{
        {"app", QJsonObject{{"name", "Scope Recorder (C++/CUDA)"}, {"version", sidecar::appVersion()}}},
        {"recording", QJsonObject{
             {"video_file", QFileInfo(QString::fromStdString(s.path)).fileName()},
             {"frames_csv", QFileInfo(QString::fromStdString(s.path)).completeBaseName() + ".frames.csv"},
             {"started", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
             {"status", "recording"},
             {"nominal_fps", s.fps},
             {"width", s.width},
             {"height", s.height}}},
        {"encoding", QJsonObject{
             {"codec", "H.264 (h264_nvenc), High profile"},
             {"rate_control", "constant quality (VBR, cq)"},
             {"quality_preset", quality_->currentText()},
             {"cq", s.quality},
             {"keyframe_interval_s", 2},
             {"pixel_format", "yuv420p (NV12 from GPU)"},
             {"color", "BT.709 primaries/transfer/matrix, limited (TV) range"},
             {"container", "fragmented MP4"},
             {"timestamps", "camera hardware clock (device_timestamp_ns)"},
             {"debayer", "Malvar-He-Cutler 5x5, GPU; camera applies white balance and gamma"}}},
        {"camera_at_start", sidecar::cameraSnapshot(*camera_)},
        {"camera_at_end", QJsonValue::Null}};
    writeSidecar();

    recordAction_->setText(tr("■ Stop"));
    toolbar_->widgetForAction(recordAction_)->setStyleSheet("background:#c00; color:white; font-weight:bold;");
    updateActions();
}

void MainWindow::stopRecording()
{
    recordAction_->setChecked(false);
    recordAction_->setText(tr("● Record"));
    toolbar_->widgetForAction(recordAction_)->setStyleSheet({});
    if (!recorder_) {
        updateActions();
        return;
    }

    camera_->setConsumer(nullptr);
    recorder_->stop();
    const auto rec = std::move(recorder_);
    updateActions();
    lastRecording_ = QString::fromStdString(rec->path());
    const QString error = QString::fromStdString(rec->error());

    QJsonObject recording = sidecar_.value("recording").toObject();
    recording["stopped"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    recording["status"] = error.isEmpty() ? QStringLiteral("complete") : QStringLiteral("failed: ") + error;
    recording["frames_written"] = qint64(rec->framesWritten());
    recording["frames_dropped"] = qint64(rec->framesDropped());
    recording["measured_fps"] = rec->measuredFps();
    recording["duration_s"] = rec->measuredFps() > 0 ? (rec->framesWritten() - 1) / rec->measuredFps() : 0.0;
    sidecar_["recording"] = recording;
    sidecar_["camera_at_end"] = sidecar::cameraSnapshot(*camera_);  // null if the camera was unplugged
    writeSidecar();
    if (!error.isEmpty()) {
        recordLabel_->setText(tr("<b style='color:#d00'>Recording failed:</b> %1").arg(error));
        QMessageBox::critical(this, tr("Recording error"), error);
        return;
    }
    recordLabel_->setText(tr("Saved %1  (%2 frames, %3 dropped)  %4")
                              .arg(QFileInfo(lastRecording_).fileName())
                              .arg(rec->framesWritten())
                              .arg(rec->framesDropped())
                              .arg(sizeAndBitrate(rec->framesWritten() / rec->fps())));
}

void MainWindow::writeSidecar()
{
    // A metadata problem must never stop a recording; just make it visible.
    const QString error = sidecar::write(sidecarPath_, sidecar_);
    if (!error.isEmpty())
        statusBar()->showMessage(tr("Could not write %1: %2").arg(QFileInfo(sidecarPath_).fileName(), error), 10000);
}

QString MainWindow::sizeAndBitrate(double seconds) const
{
    const QString path = recorder_ ? QString::fromStdString(recorder_->path()) : lastRecording_;
    const qint64 size = QFileInfo(path).size();
    const double mbitPerS = seconds > 2 ? size * 8 / seconds / 1e6 : 0.0;
    return tr("%1 MB, %2 Mbit/s (~%3 GB/h)")
        .arg(size / 1e6, 0, 'f', 0)
        .arg(mbitPerS, 0, 'f', 1)
        .arg(mbitPerS / 8 * 3.6, 0, 'f', 1);
}

QString MainWindow::snapshot()
{
    const QImage frame = camera_->grabLatestFrame();
    if (frame.isNull())
        return {};
    QDir().mkpath(outputDir_);
    const QString path = outputDir_ + "/" + timestampedName("yyyy-MM-dd_HH-mm-ss_zzz") + ".png";
    // PNG encoding takes a few hundred ms; keep it off the GUI thread. Opaque RGB, like the Python version.
    QThreadPool::globalInstance()->start([frame, path] {
        if (!frame.convertToFormat(QImage::Format_RGB888).save(path))
            qWarning("Could not save snapshot %s", qPrintable(path));
    });
    statusBar()->showMessage(tr("Snapshot saved: %1").arg(QFileInfo(path).fileName()), 3000);
    return path;
}

void MainWindow::setOutputDir(const QString& dir, bool remember)
{
    outputDir_ = dir;
    if (remember)
        settings_.setValue("output_dir", dir);
}

void MainWindow::chooseOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Recording folder"), outputDir_);
    if (!dir.isEmpty()) {
        setOutputDir(dir, true);
        updateStatus();
    }
}

void MainWindow::openOutputDir()
{
    QDir().mkpath(outputDir_);
    QDesktopServices::openUrl(QUrl::fromLocalFile(outputDir_));
}

// ---------- shutdown ----------

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (recorder_ &&
        QMessageBox::question(this, tr("Recording in progress"), tr("Stop recording and quit?")) != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    stopRecording();
    QThreadPool::globalInstance()->waitForDone();  // finish writing any snapshots
    camera_->saveState();
    camera_->close();
    event->accept();
}

// ---------- benchmark ----------

namespace {

double processCpuSeconds()
{
    QFile stat("/proc/self/stat");
    if (!stat.open(QIODevice::ReadOnly))
        return 0;
    const auto fields = stat.readAll().split(')').last().split(' ');
    // After the ")" the fields start at index 3 (state); utime and stime are fields 14 and 15.
    return (fields.value(12).toDouble() + fields.value(13).toDouble()) / sysconf(_SC_CLK_TCK);
}

}  // namespace

void MainWindow::runBenchmark(double seconds, const QString& grabFile, bool record, bool takeSnapshot)
{
    QTimer::singleShot(3000, this, [=, this] {
        if (record) {
            recordAction_->setChecked(true);
            startRecording();
        }
        const auto t0 = std::chrono::steady_clock::now();
        const double cpu0 = processCpuSeconds();
        const uint64_t received0 = camera_->framesReceived(), shown0 = view_->framesShown();
        QTimer::singleShot(int(seconds * 1000), this, [=, this] {
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            printf("received %.1f fps, shown %.1f fps, CPU %.1f%% of one core\n",
                   (camera_->framesReceived() - received0) / dt, (view_->framesShown() - shown0) / dt,
                   100 * (processCpuSeconds() - cpu0) / dt);
            if (record) {
                stopRecording();
                printf("recording: %s\n", qPrintable(recordLabel_->text()));
            }
            if (takeSnapshot)
                printf("snapshot: %s\n", qPrintable(snapshot()));
            fflush(stdout);
            if (!grabFile.isEmpty())
                view_->grabFramebuffer().save(grabFile);
            close();
        });
    });
}
