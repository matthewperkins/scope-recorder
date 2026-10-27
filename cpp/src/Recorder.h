#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Camera.h"

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

// Encodes camera frames with NVENC straight from GPU memory and writes a fragmented MP4
// (playable up to the last ~2 s if the app or PC crashes) plus a per-frame CSV.
//
// Camera thread: onFrame() converts RGBA -> NV12 into an encoder-owned GPU surface (no CPU copy).
// Writer thread: feeds NVENC, muxes packets, writes the CSV.
class Recorder : public FrameConsumer {
public:
    struct Settings {
        std::string path;   // .mp4; the CSV goes next to it as .frames.csv
        int width = 0;
        int height = 0;
        double fps = 30.0;  // nominal rate (container hint); timestamps come from the camera clock
        int quality = 19;   // NVENC constant-quality level; lower = better, bigger files
    };

    // Returns nullptr and sets `error` on failure.
    static std::shared_ptr<Recorder> start(const Settings& settings, std::string& error);
    ~Recorder() override;

    void onFrame(const GpuFrame& frame, cudaStream_t stream) override;

    // Flushes the encoder and finalises the file. Call after detaching from the camera.
    void stop();

    const std::string& path() const { return settings_.path; }
    double fps() const { return settings_.fps; }
    uint64_t framesWritten() const { return framesWritten_.load(); }
    uint64_t framesDropped() const { return framesDropped_.load(); }
    // Frame rate measured from the camera clock over the frames submitted so far (0 if < 2 frames).
    // Call from the GUI thread after stop(), or accept a slightly stale value while recording.
    double measuredFps() const;
    std::chrono::steady_clock::time_point started() const { return started_; }
    std::string error() const;

private:
    explicit Recorder(Settings settings);
    void open();  // throws std::runtime_error
    void run();
    void encode(AVFrame* frame);  // nullptr flushes
    void fail(const std::string& message);

    struct Item {
        AVFrame* frame;
        uint64_t deviceFrameNumber;
        uint64_t deviceTimestampNs;
    };

    Settings settings_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    AVBufferRef* device_ = nullptr;
    AVBufferRef* framesCtx_ = nullptr;
    AVCodecContext* encoder_ = nullptr;
    AVFormatContext* muxer_ = nullptr;
    AVStream* stream_ = nullptr;
    AVPacket* packet_ = nullptr;
    bool headerWritten_ = false;
    std::ofstream csv_;

    // Timestamps (camera thread only).
    uint64_t firstTimestampNs_ = 0;
    std::atomic<uint64_t> lastTimestampNs_{0};
    int64_t lastPts_ = -1;
    uint64_t submitted_ = 0;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> queue_;
    bool closing_ = false;
    std::thread writer_;

    std::atomic<uint64_t> framesWritten_{0};
    std::atomic<uint64_t> framesDropped_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
};
