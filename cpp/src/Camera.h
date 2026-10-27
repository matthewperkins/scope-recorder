#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <QImage>
#include <QObject>
#include <QString>

#include <cuda_runtime.h>
#include <ic4/ic4.h>

// A debayered frame in GPU memory. Valid until the camera writes the same ring slot
// again (two frames later), so consumers must copy it out promptly.
struct GpuFrame {
    const uchar4* rgba = nullptr;
    size_t pitch = 0;
    int width = 0;
    int height = 0;
    uint64_t sequence = 0;      // increments per frame; 0 = no frame yet
    cudaEvent_t ready = nullptr;  // consumers wait on this before reading
    uint64_t deviceFrameNumber = 0;
    uint64_t deviceTimestampNs = 0;  // camera hardware clock
};

// Receives every frame on IC4's worker thread, right after debayering. `stream` is the
// camera's CUDA stream: work enqueued on it is ordered after the debayer.
class FrameConsumer {
public:
    virtual ~FrameConsumer() = default;
    virtual void onFrame(const GpuFrame& frame, cudaStream_t stream) = 0;
};

// Owns the IC4 grabber. Frames arrive as raw Bayer on IC4's worker thread, are uploaded
// to the GPU and debayered there, so the CPU never touches colour data.
class Camera : public QObject, private ic4::QueueSinkListener {
    Q_OBJECT
public:
    explicit Camera(QString stateFile, QObject* parent = nullptr);
    ~Camera() override;

    // Opens `device`, or else the saved camera, or else the first connected one, and starts streaming.
    // Returns an error message, or empty on success.
    QString open(const ic4::DeviceInfo* device = nullptr);
    void close();
    bool isStreaming() const { return grabber_.isStreaming(); }
    QString description() const;

    GpuFrame latestFrame() const;
    // Full-resolution copy of the latest frame (RGBA8888), or a null image if there is none.
    QImage grabLatestFrame() const;
    double frameRate() const;

    // At most one consumer (the recorder). Pass nullptr to detach; the camera thread may still be
    // finishing a call into the old consumer, which the shared_ptr keeps alive.
    void setConsumer(std::shared_ptr<FrameConsumer> consumer);
    uint64_t framesReceived() const { return framesReceived_.load(); }
    ic4::PropertyMap properties() const { return grabber_.devicePropertyMap(); }
    void saveState();
    // IC4's complete device state (model, serial, every property) as JSON text; empty if no camera.
    QByteArray stateJson();

signals:
    void frameReady();
    void deviceLost();

private:
    bool sinkConnected(ic4::QueueSink& sink, const ic4::ImageType& type, size_t minBuffers) override;
    void sinkDisconnected(ic4::QueueSink& sink) override;
    void framesQueued(ic4::QueueSink& sink) override;

    void allocateGpu(int width, int height);
    void freeGpu();
    void applyFirstRunDefaults();

    QString stateFile_;
    ic4::Grabber grabber_;
    std::shared_ptr<ic4::QueueSink> sink_;
    ic4::Grabber::NotificationToken lostToken_{};

    static constexpr int kSlots = 3;
    int width_ = 0, height_ = 0;
    cudaStream_t stream_ = nullptr;
    uint8_t* pinnedBayer_ = nullptr;   // host staging buffer for fast async upload
    uint8_t* devBayer_ = nullptr;
    size_t devBayerPitch_ = 0;
    std::array<uchar4*, kSlots> devRgba_{};
    size_t devRgbaPitch_ = 0;
    std::array<cudaEvent_t, kSlots> slotReady_{};
    std::array<ic4::ImageBuffer::MetaData, kSlots> slotMeta_{};

    mutable std::mutex consumerMutex_;
    std::shared_ptr<FrameConsumer> consumer_;

    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> framesReceived_{0};
};
