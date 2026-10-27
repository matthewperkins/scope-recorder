#include "Camera.h"

#include <cstring>
#include <stdexcept>

#include <QFile>

#include "GpuFrames.h"

namespace {

constexpr double kDefaultFps = 30.0;

// IC4's default error handler ignores errors; use explicit Error objects where failure is expected.
QString errorText(const ic4::Error& err)
{
    return QString::fromStdString(err.message());
}

}  // namespace

Camera::Camera(QString stateFile, QObject* parent)
    : QObject(parent), stateFile_(std::move(stateFile))
{
}

Camera::~Camera()
{
    close();
}

QString Camera::open(const ic4::DeviceInfo* device)
{
    close();
    ic4::Error err;
    if (QFile::exists(stateFile_) && !grabber_.deviceOpenFromState(stateFile_.toStdString(), err))
        qWarning("Could not restore saved camera state: %s", err.message().c_str());

    // An explicitly chosen camera that isn't the saved one: open it with default settings.
    if (device && grabber_.isDeviceOpen() &&
        grabber_.deviceInfo(err).serial(err) != device->serial(err)) {
        grabber_.deviceClose(err);
    }
    if (device && !grabber_.isDeviceOpen()) {
        if (!grabber_.deviceOpen(*device, err))
            return tr("%1\n\nIs another Scope Recorder already open?").arg(errorText(err));
        applyFirstRunDefaults();
    }

    if (!grabber_.isDeviceOpen()) {
        const auto devices = ic4::DeviceEnum::enumDevices();
        if (devices.empty())
            return tr("No camera found. Check the USB 3 cable.");
        if (!grabber_.deviceOpen(devices.front(), err))
            return tr("%1\n\nIs another Scope Recorder already open?").arg(errorText(err));
        applyFirstRunDefaults();
    }

    // Raw Bayer straight from the sensor: IC4 does no CPU conversion; the GPU debayers.
    if (!grabber_.devicePropertyMap().setValue(ic4::PropId::PixelFormat, ic4::PixelFormat::BayerGB8, err))
        return tr("Camera does not support BayerGB8: %1").arg(errorText(err));

    lostToken_ = grabber_.eventAddDeviceLost([this](ic4::Grabber&) { emit deviceLost(); });

    sink_ = ic4::QueueSink::create(*this, ic4::PixelFormat::BayerGB8, err);
    if (!sink_ || !grabber_.streamSetup(sink_, ic4::StreamSetupOption::AcquisitionStart, err))
        return tr("Could not start the camera stream: %1").arg(errorText(err));
    return {};
}

void Camera::applyFirstRunDefaults()
{
    auto map = grabber_.devicePropertyMap();
    ic4::Error ignored;
    map.setValue(ic4::PropId::AcquisitionFrameRate, kDefaultFps, ignored);
    map.setValue(ic4::PropId::ExposureAuto, "Continuous", ignored);
    map.setValue(ic4::PropId::GainAuto, "Continuous", ignored);
    map.setValue(ic4::PropId::BalanceWhiteAuto, "Continuous", ignored);
}

void Camera::close()
{
    ic4::Error ignored;
    if (grabber_.isStreaming())
        grabber_.streamStop(ignored);
    if (lostToken_) {
        grabber_.eventRemoveDeviceLost(lostToken_, ignored);
        lostToken_ = nullptr;
    }
    if (grabber_.isDeviceOpen())
        grabber_.deviceClose(ignored);
    sink_.reset();
    freeGpu();
}

void Camera::saveState()
{
    ic4::Error err;
    if (grabber_.isDeviceOpen() && !grabber_.deviceSaveState(stateFile_.toStdString(), err))
        qWarning("Could not save camera state: %s", err.message().c_str());
}

QByteArray Camera::stateJson()
{
    ic4::Error err;
    if (!grabber_.isDeviceOpen())
        return {};
    const auto state = grabber_.deviceSaveState(err);
    return err.isSuccess() ? QByteArray(reinterpret_cast<const char*>(state.data()), qsizetype(state.size())) : QByteArray();
}

QString Camera::description() const
{
    if (!grabber_.isDeviceOpen())
        return tr("No camera");
    ic4::Error ignored;
    const auto info = grabber_.deviceInfo(ignored);
    return QStringLiteral("%1  #%2  %3×%4")
        .arg(QString::fromStdString(info.modelName(ignored)), QString::fromStdString(info.serial(ignored)))
        .arg(width_)
        .arg(height_);
}

GpuFrame Camera::latestFrame() const
{
    const uint64_t seq = sequence_.load(std::memory_order_acquire);
    if (seq == 0)
        return {};
    const int slot = int(seq % kSlots);
    return {devRgba_[slot], devRgbaPitch_, width_, height_, seq, slotReady_[slot],
            slotMeta_[slot].device_frame_number, slotMeta_[slot].device_timestamp_ns};
}

QImage Camera::grabLatestFrame() const
{
    const GpuFrame frame = latestFrame();
    if (frame.sequence == 0)
        return {};
    QImage image(frame.width, frame.height, QImage::Format_RGBA8888);
    cudaStream_t stream = nullptr;
    try {
        CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        CUDA_CHECK(cudaStreamWaitEvent(stream, frame.ready));
        CUDA_CHECK(cudaMemcpy2DAsync(image.bits(), image.bytesPerLine(), frame.rgba, frame.pitch,
                                     size_t(frame.width) * sizeof(uchar4), frame.height, cudaMemcpyDeviceToHost, stream));
        CUDA_CHECK(cudaStreamSynchronize(stream));  // well within the ~60 ms before this ring slot is reused
    } catch (const std::exception& e) {
        qCritical("Snapshot failed: %s", e.what());
        image = {};
    }
    if (stream)
        cudaStreamDestroy(stream);
    return image;
}

double Camera::frameRate() const
{
    ic4::Error err;
    const double fps = grabber_.devicePropertyMap().getValueDouble(ic4::PropId::AcquisitionFrameRate, err);
    return err.isError() || fps <= 0 ? kDefaultFps : fps;
}

void Camera::setConsumer(std::shared_ptr<FrameConsumer> consumer)
{
    std::lock_guard lock(consumerMutex_);
    consumer_ = std::move(consumer);
}

// ---------- IC4 sink callbacks (IC4 worker thread) ----------

bool Camera::sinkConnected(ic4::QueueSink& sink, const ic4::ImageType& type, size_t minBuffers)
{
    try {
        allocateGpu(int(type.width()), int(type.height()));
    } catch (const std::exception& e) {
        qCritical("GPU setup failed: %s", e.what());
        return false;
    }
    return sink.allocAndQueueBuffers(minBuffers + 4);
}

void Camera::sinkDisconnected(ic4::QueueSink&)
{
}

void Camera::framesQueued(ic4::QueueSink& sink)
{
    ic4::Error err;
    auto buffer = sink.popOutputBuffer(err);
    if (!buffer)
        return;
    framesReceived_.fetch_add(1, std::memory_order_relaxed);
    const auto meta = buffer->metaData(err);

    // Copy into pinned memory (~2.3 MB, a fraction of a millisecond) so the upload can use DMA.
    const auto* src = static_cast<const uint8_t*>(buffer->ptr());
    const ptrdiff_t srcPitch = buffer->pitch();
    if (srcPitch == width_) {
        std::memcpy(pinnedBayer_, src, size_t(width_) * height_);
    } else {
        for (int y = 0; y < height_; ++y)
            std::memcpy(pinnedBayer_ + size_t(y) * width_, src + y * srcPitch, width_);
    }
    buffer.reset();  // hand the IC4 buffer back to the camera immediately

    const uint64_t seq = sequence_.load(std::memory_order_relaxed) + 1;
    const int slot = int(seq % kSlots);
    try {
        CUDA_CHECK(cudaMemcpy2DAsync(devBayer_, devBayerPitch_, pinnedBayer_, width_, width_, height_,
                                     cudaMemcpyHostToDevice, stream_));
        gpu::debayerGbrgToRgba(devBayer_, devBayerPitch_, devRgba_[slot], devRgbaPitch_, width_, height_, stream_);
        CUDA_CHECK(cudaEventRecord(slotReady_[slot], stream_));
        slotMeta_[slot] = meta;

        std::shared_ptr<FrameConsumer> consumer;
        {
            std::lock_guard lock(consumerMutex_);
            consumer = consumer_;
        }
        if (consumer)
            consumer->onFrame({devRgba_[slot], devRgbaPitch_, width_, height_, seq, slotReady_[slot],
                               meta.device_frame_number, meta.device_timestamp_ns},
                              stream_);

        // The pinned staging buffer is reused next frame, so wait for the GPU (about 1 ms).
        CUDA_CHECK(cudaStreamSynchronize(stream_));
    } catch (const std::exception& e) {
        qCritical("GPU frame processing failed: %s", e.what());
        return;
    }
    sequence_.store(seq, std::memory_order_release);
    emit frameReady();
}

// ---------- GPU buffers ----------

void Camera::allocateGpu(int width, int height)
{
    if (width == width_ && height == height_ && stream_)
        return;
    freeGpu();
    width_ = width;
    height_ = height;
    CUDA_CHECK(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));
    CUDA_CHECK(cudaMallocHost(&pinnedBayer_, size_t(width) * height));
    CUDA_CHECK(cudaMallocPitch(&devBayer_, &devBayerPitch_, width, height));
    for (int i = 0; i < kSlots; ++i) {
        CUDA_CHECK(cudaMallocPitch(&devRgba_[i], &devRgbaPitch_, size_t(width) * sizeof(uchar4), height));
        CUDA_CHECK(cudaEventCreateWithFlags(&slotReady_[i], cudaEventDisableTiming));
    }
}

void Camera::freeGpu()
{
    sequence_.store(0);
    if (stream_)
        cudaStreamSynchronize(stream_);
    for (int i = 0; i < kSlots; ++i) {
        cudaFree(devRgba_[i]);
        devRgba_[i] = nullptr;
        if (slotReady_[i])
            cudaEventDestroy(slotReady_[i]);
        slotReady_[i] = nullptr;
    }
    cudaFree(devBayer_);
    devBayer_ = nullptr;
    cudaFreeHost(pinnedBayer_);
    pinnedBayer_ = nullptr;
    if (stream_)
        cudaStreamDestroy(stream_);
    stream_ = nullptr;
    width_ = height_ = 0;
}
