#include "Recorder.h"

#include <cmath>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
}

#include "GpuFrames.h"

namespace {

constexpr AVRational kTimeBase{1, 90000};  // standard video clock; plenty for camera timestamps
constexpr int kGpuSurfaces = 32;            // NV12 surfaces shared by the queue and NVENC (~3.5 MB each)
constexpr size_t kMaxQueued = 16;           // frames waiting for the writer before we drop

std::string avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

void check(int code, const char* what)
{
    if (code < 0)
        throw std::runtime_error(std::string(what) + ": " + avError(code));
}

}  // namespace

std::shared_ptr<Recorder> Recorder::start(const Settings& settings, std::string& error)
{
    std::shared_ptr<Recorder> recorder(new Recorder(settings));
    try {
        recorder->open();
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    recorder->writer_ = std::thread(&Recorder::run, recorder.get());
    return recorder;
}

Recorder::Recorder(Settings settings)
    : settings_(std::move(settings))
{
}

Recorder::~Recorder()
{
    stop();
    for (auto& item : queue_)
        av_frame_free(&item.frame);
    av_packet_free(&packet_);
    avcodec_free_context(&encoder_);
    if (muxer_) {
        avio_closep(&muxer_->pb);
        avformat_free_context(muxer_);
    }
    av_buffer_unref(&framesCtx_);
    av_buffer_unref(&device_);
}

void Recorder::open()
{
    const int w = settings_.width, h = settings_.height;

    // Share the app's CUDA context, so the encoder reads our GPU frames directly.
    check(av_hwdevice_ctx_create(&device_, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, AV_CUDA_USE_PRIMARY_CONTEXT),
          "CUDA device for encoder");
    framesCtx_ = av_hwframe_ctx_alloc(device_);
    auto* frames = reinterpret_cast<AVHWFramesContext*>(framesCtx_->data);
    frames->format = AV_PIX_FMT_CUDA;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = w;
    frames->height = h;
    frames->initial_pool_size = kGpuSurfaces;
    check(av_hwframe_ctx_init(framesCtx_), "GPU frame pool");

    check(avformat_alloc_output_context2(&muxer_, nullptr, "mp4", settings_.path.c_str()), "MP4 muxer");

    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec)
        throw std::runtime_error("This FFmpeg build has no h264_nvenc encoder");
    encoder_ = avcodec_alloc_context3(codec);
    encoder_->width = w;
    encoder_->height = h;
    encoder_->time_base = kTimeBase;
    encoder_->framerate = av_d2q(settings_.fps, 100000);
    encoder_->pix_fmt = AV_PIX_FMT_CUDA;
    encoder_->hw_frames_ctx = av_buffer_ref(framesCtx_);
    encoder_->gop_size = std::max(1, int(std::lround(settings_.fps * 2)));  // keyframe (and MP4 fragment) every ~2 s
    encoder_->bit_rate = 0;  // constant quality: bitrate follows the scene
    encoder_->color_primaries = AVCOL_PRI_BT709;
    encoder_->color_trc = AVCOL_TRC_BT709;
    encoder_->colorspace = AVCOL_SPC_BT709;
    encoder_->color_range = AVCOL_RANGE_MPEG;
    if (muxer_->oformat->flags & AVFMT_GLOBALHEADER)
        encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "preset", "p5", 0);
    av_dict_set(&options, "tune", "hq", 0);
    av_dict_set(&options, "rc", "vbr", 0);
    av_dict_set_int(&options, "cq", settings_.quality, 0);
    av_dict_set(&options, "profile", "high", 0);
    const int opened = avcodec_open2(encoder_, codec, &options);
    av_dict_free(&options);
    check(opened, "NVENC encoder");

    stream_ = avformat_new_stream(muxer_, nullptr);
    stream_->time_base = kTimeBase;
    stream_->avg_frame_rate = encoder_->framerate;
    check(avcodec_parameters_from_context(stream_->codecpar, encoder_), "stream parameters");

    check(avio_open(&muxer_->pb, settings_.path.c_str(), AVIO_FLAG_WRITE), "output file");
    AVDictionary* muxOptions = nullptr;
    av_dict_set(&muxOptions, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
    const int header = avformat_write_header(muxer_, &muxOptions);
    av_dict_free(&muxOptions);
    check(header, "MP4 header");
    headerWritten_ = true;

    packet_ = av_packet_alloc();

    std::string csvPath = settings_.path;
    csvPath.replace(csvPath.rfind(".mp4"), 4, ".frames.csv");
    csv_.open(csvPath);
    if (!csv_)
        throw std::runtime_error("Could not create " + csvPath);
    csv_ << "video_frame,device_frame_number,device_timestamp_ns\n";
}

// ---------- camera thread ----------

void Recorder::onFrame(const GpuFrame& frame, cudaStream_t stream)
{
    if (frame.width != settings_.width || frame.height != settings_.height) {
        fail("Camera resolution changed during recording");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (closing_ || queue_.size() >= kMaxQueued) {
            framesDropped_.fetch_add(1);
            return;
        }
    }

    AVFrame* nv12 = av_frame_alloc();
    if (av_hwframe_get_buffer(framesCtx_, nv12, 0) < 0) {  // all GPU surfaces busy: encoder is behind
        av_frame_free(&nv12);
        framesDropped_.fetch_add(1);
        return;
    }
    gpu::rgbaToNv12Bt709(frame.rgba, frame.pitch, nv12->data[0], nv12->linesize[0], nv12->data[1],
                         nv12->linesize[1], frame.width, frame.height, stream);
    CUDA_CHECK(cudaStreamSynchronize(stream));  // surface must be complete before NVENC reads it

    // Presentation time from the camera's hardware clock, so dropped frames don't speed up playback.
    int64_t pts;
    if (frame.deviceTimestampNs != 0) {
        if (firstTimestampNs_ == 0)
            firstTimestampNs_ = frame.deviceTimestampNs;
        pts = av_rescale(int64_t(frame.deviceTimestampNs - firstTimestampNs_), kTimeBase.den, 1'000'000'000);
        lastTimestampNs_.store(frame.deviceTimestampNs);
    } else {
        pts = av_rescale(int64_t(submitted_), kTimeBase.den * 1000, std::lround(settings_.fps * 1000));
    }
    nv12->pts = std::max(pts, lastPts_ + 1);
    lastPts_ = nv12->pts;
    ++submitted_;

    {
        std::lock_guard lock(mutex_);
        queue_.push_back({nv12, frame.deviceFrameNumber, frame.deviceTimestampNs});
    }
    wake_.notify_one();
}

// ---------- writer thread ----------

void Recorder::run()
{
    for (;;) {
        Item item;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return closing_ || !queue_.empty(); });
            if (queue_.empty())
                break;  // closing and drained
            item = queue_.front();
            queue_.pop_front();
        }
        if (error().empty()) {
            encode(item.frame);
            csv_ << framesWritten_.load() << ',' << item.deviceFrameNumber << ',' << item.deviceTimestampNs << '\n';
            if (framesWritten_.fetch_add(1) % 30 == 29)
                csv_.flush();  // ~once a second, so a crash loses at most ~1 s of rows
        }
        av_frame_free(&item.frame);
    }
    if (error().empty())
        encode(nullptr);  // drain the encoder
}

void Recorder::encode(AVFrame* frame)
{
    int ret = avcodec_send_frame(encoder_, frame);
    if (ret < 0 && ret != AVERROR_EOF) {
        fail("NVENC: " + avError(ret));
        return;
    }
    while ((ret = avcodec_receive_packet(encoder_, packet_)) >= 0) {
        av_packet_rescale_ts(packet_, encoder_->time_base, stream_->time_base);
        packet_->stream_index = stream_->index;
        ret = av_interleaved_write_frame(muxer_, packet_);  // takes ownership of the packet's data
        if (ret < 0) {
            fail("Writing video: " + avError(ret));
            return;
        }
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        fail("NVENC: " + avError(ret));
}

void Recorder::stop()
{
    {
        std::lock_guard lock(mutex_);
        if (closing_)
            return;
        closing_ = true;
    }
    wake_.notify_one();
    if (writer_.joinable())
        writer_.join();
    if (headerWritten_) {
        const int ret = av_write_trailer(muxer_);
        if (ret < 0)
            fail("Finalising video: " + avError(ret));
        headerWritten_ = false;
    }
    if (muxer_)
        avio_closep(&muxer_->pb);
    csv_.close();
}

double Recorder::measuredFps() const
{
    const uint64_t last = lastTimestampNs_.load();
    const uint64_t frames = framesWritten_.load();
    if (frames < 2 || last <= firstTimestampNs_)
        return 0.0;
    return (frames - 1) / ((last - firstTimestampNs_) / 1e9);
}

void Recorder::fail(const std::string& message)
{
    std::lock_guard lock(errorMutex_);
    if (error_.empty())
        error_ = message;
}

std::string Recorder::error() const
{
    std::lock_guard lock(errorMutex_);
    return error_;
}
