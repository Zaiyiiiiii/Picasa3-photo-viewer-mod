// Video through FFmpeg (built without network, encoders, muxers or devices).
//
// The load path only ever sees a small blob naming the file (MakeVideoBlob). Opening it parses the
// container and opens the video decoder; the first NextFrame() decodes the poster frame. StartLive()
// (called by the animator once the picture is on screen) starts a thread that demuxes and decodes into
// a short frame queue and feeds audio to WASAPI. The audio clock (or the system clock for silent
// videos) decides which queued frame RenderLive() shows; swscale scales it to the display buffer.
// At the end of the file playback loops.

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../patch.h"
#include "audio_out.h"
#include "codec.h"

namespace {

constexpr char kMagic[8] = {'P', 'M', 'O', 'D', 'V', 'I', 'D', '1'};
constexpr size_t kMaxQueuedFrames = 12;
constexpr double kMaxQueuedAudioSeconds = 0.5;

struct Frame {
    AVFrame* frame;
    double pts;  // seconds on the playback timeline
};

class VideoSource : public ImageSource {
public:
    ~VideoSource() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        for (Frame& f : queue_) av_frame_free(&f.frame);
        sws_freeContext(sws_);
        swr_free(&swr_);
        avcodec_free_context(&video_);
        avcodec_free_context(&audio_);
        av_buffer_unref(&hwDevice_);
        avformat_close_input(&fmt_);
    }

    bool Open(const std::string& path) {
        int err = avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr);
        if (err < 0) return Fail("open", err);
        if ((err = avformat_find_stream_info(fmt_, nullptr)) < 0) return Fail("stream info", err);
        videoIndex_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIndex_ < 0) return Fail("no video stream", videoIndex_);
        if (!OpenDecoder(videoIndex_, video_)) return false;

        const AVStream* st = fmt_->streams[videoIndex_];
        const AVCodecParameters* par = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(fmt_, const_cast<AVStream*>(st), nullptr);
        displayW_ = par->width;
        displayH_ = par->height;
        if (sar.num > 0 && sar.den > 0 && sar.num != sar.den)
            displayW_ = static_cast<uint32_t>(std::lround(double(par->width) * sar.num / sar.den));
        if (const AVPacketSideData* sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                                                 AV_PKT_DATA_DISPLAYMATRIX)) {
            double ccw = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
            if (!std::isnan(ccw)) turnsCW_ = (static_cast<int>(std::lround(-ccw / 90.0)) % 4 + 4) % 4;
        }
        width = (turnsCW_ & 1) ? displayH_ : displayW_;
        height = (turnsCW_ & 1) ? displayW_ : displayH_;
        // Not a video player: never hand the viewer more pixels than the screen has. A smaller picture
        // means less to convert and less for the viewer to scale on every frame (4K -> screen size).
        double fit = std::min(double(GetSystemMetrics(SM_CXSCREEN)) / width, double(GetSystemMetrics(SM_CYSCREEN)) / height);
        if (fit < 1.0) {
            width = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(width * fit)));
            height = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(height * fit)));
        }
        startPts_ = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        frameCount = 0;  // plays like an endless animation
        return width && height;
    }

    // Poster: the first frame at the display size.
    int NextFrame(uint8_t* out, uint32_t stride) override {
        AVFrame* f = av_frame_alloc();
        bool ok = DecodeOneVideoFrame(f) && Convert(f, out, width, height, stride);
        av_frame_free(&f);
        return ok ? 40 : -1;
    }

    bool Live() const override { return true; }

    bool StartLive() override {
        if (thread_.joinable()) return true;
        avformat_seek_file(fmt_, -1, INT64_MIN, 0, 0, 0);
        avcodec_flush_buffers(video_);
        // The poster decoder is software; playback gets a hardware one when possible.
        if (!GetEnvironmentVariableW(L"PICASA_MOD_NOHW", nullptr, 0)) {
            AVCodecContext* hw = nullptr;
            if (OpenDecoder(videoIndex_, hw, true) && hw->hw_device_ctx) {
                avcodec_free_context(&video_);
                video_ = hw;
            } else {
                avcodec_free_context(&hw);
            }
        }
        clockStart_ = GetTickCount();
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    bool FrameReady() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return !queue_.empty() && queue_.front().pts <= Clock();
    }

    bool RenderLive(uint8_t* dst, uint32_t w, uint32_t h, uint32_t stride) override {
        AVFrame* show = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            double now = Clock();
            // Drop frames whose successor is already due (we are late).
            while (queue_.size() > 1 && queue_[1].pts <= now) {
                av_frame_free(&queue_.front().frame);
                queue_.pop_front();
            }
            if (queue_.empty() || queue_.front().pts > now) return false;
            show = queue_.front().frame;
            queue_.pop_front();
        }
        bool ok = Convert(show, dst, w, h, stride);
        av_frame_free(&show);
        return ok;
    }

    const char* Name() const override { return "ffmpeg"; }

private:
    bool Fail(const char* what, int err) {
        char msg[128] = "";
        if (err < 0) av_strerror(err, msg, sizeof(msg));
        Log("video: %s failed: %s", what, msg);
        return false;
    }

    static AVPixelFormat PickFormat(AVCodecContext*, const AVPixelFormat* formats) {
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_D3D11) return *p;
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)  // hwaccel refused: software
            if (!(av_pix_fmt_desc_get(*p)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        return AV_PIX_FMT_NONE;
    }

    // D3D11VA if the decoder supports it and a device can be created (frames are copied back to memory).
    bool SetupHardware(const AVCodec* codec, AVCodecContext* ctx) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) return false;
            if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                break;
        }
        if (!hwDevice_) {
            int err = av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
            if (err < 0) return Fail("d3d11va device", err);
        }
        ctx->hw_device_ctx = av_buffer_ref(hwDevice_);
        ctx->get_format = &VideoSource::PickFormat;
        return true;
    }

    bool OpenDecoder(int index, AVCodecContext*& ctx, bool hardware = false) {
        const AVCodecParameters* par = fmt_->streams[index]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) return Fail(avcodec_get_name(par->codec_id), AVERROR_DECODER_NOT_FOUND);
        ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, par);
        ctx->pkt_timebase = fmt_->streams[index]->time_base;
        hardware = hardware && SetupHardware(codec, ctx);
        ctx->thread_count = hardware ? 1 : 0;  // 0 = auto
        int err = avcodec_open2(ctx, codec, nullptr);
        if (err < 0) return Fail("decoder open", err);
        if (index == videoIndex_)
            Log("video: %s decoder%s, %dx%d", codec->name, hardware ? " with d3d11va" : "", par->width, par->height);
        return true;
    }

    bool DecodeOneVideoFrame(AVFrame* out) {
        AVPacket* pkt = av_packet_alloc();
        bool got = false;
        while (!got) {
            int err = avcodec_receive_frame(video_, out);
            if (err == 0) {
                got = true;
                break;
            }
            if (err != AVERROR(EAGAIN)) break;
            if (av_read_frame(fmt_, pkt) < 0) {
                avcodec_send_packet(video_, nullptr);  // drain
                continue;
            }
            if (pkt->stream_index == videoIndex_) avcodec_send_packet(video_, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        return got;
    }

    // Scale (and rotate) a decoded frame into a w x h BGRA destination.
    // swscale's SIMD paths may write past the end of a row whose width is not a multiple of their block
    // size, so it only ever writes into our own padded buffer; the destination gets exact rows.
    bool Convert(const AVFrame* f, uint8_t* dst, uint32_t w, uint32_t h, uint32_t stride) {
        uint32_t sw = (turnsCW_ & 1) ? h : w, sh = (turnsCW_ & 1) ? w : h;  // before rotation
        sws_ = sws_getCachedContext(sws_, f->width, f->height, static_cast<AVPixelFormat>(f->format), sw, sh,
                                    AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return false;
        uint32_t padded = (sw * 4 + 63) & ~63u;
        scratch_.resize(size_t(padded) * (sh + 1) + 64);
        auto* target = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(scratch_.data()) + 63) & ~uintptr_t(63));
        uint8_t* planes[4] = {target};
        int strides[4] = {static_cast<int>(padded)};
        sws_scale(sws_, f->data, f->linesize, 0, f->height, planes, strides);
        if (turnsCW_)
            codec::Rotate(target, padded, sw, sh, turnsCW_, dst, stride);
        else
            for (uint32_t y = 0; y < sh; ++y) memcpy(dst + size_t(y) * stride, target + size_t(y) * padded, size_t(sw) * 4);
        return true;
    }

    double Clock() const {
        if (const AudioOut* audio = clockAudio_.load(std::memory_order_acquire)) return audio->PlayedSeconds();
        return (GetTickCount() - clockStart_) / 1000.0;
    }

    void Run() {
        bool mute = GetEnvironmentVariableW(L"PICASA_MOD_MUTE", nullptr, 0) != 0;
        int audioIndex = mute ? -1 : av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, videoIndex_, nullptr, 0);
        if (audioIndex >= 0 && OpenDecoder(audioIndex, audio_)) {
            auto out = std::make_unique<AudioOut>();
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (out->Open() &&
                swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, out->Rate(), &audio_->ch_layout,
                                    audio_->sample_fmt, audio_->sample_rate, 0, nullptr) >= 0 &&
                swr_init(swr_) >= 0) {
                audioOut_ = std::move(out);
                clockAudio_.store(audioOut_.get(), std::memory_order_release);
            }
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* f = av_frame_alloc();
        double loopOffset = 0, lastPts = 0, frameDur = 1.0 / 30;
        std::vector<float> pending;  // interleaved stereo waiting for the device
        size_t pendingPos = 0;
        while (!stop_) {
            if (audioOut_ && pendingPos < pending.size()) {
                pendingPos += audioOut_->Write(pending.data() + pendingPos, (pending.size() - pendingPos) / 2) * 2;
                if (pendingPos == pending.size()) pending.clear(), pendingPos = 0;
            }
            size_t queued;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queued = queue_.size();
            }
            bool audioFull = !audioOut_ || (pending.size() - pendingPos) / 2 > audioOut_->Rate() * kMaxQueuedAudioSeconds;
            if (queued >= kMaxQueuedFrames && audioFull) {
                Sleep(5);
                continue;
            }
            if (av_read_frame(fmt_, pkt) < 0) {  // end of file: drain, then loop
                avcodec_send_packet(video_, nullptr);
                while (avcodec_receive_frame(video_, f) == 0) Queue(f, loopOffset, lastPts, frameDur);
                loopOffset = lastPts + frameDur;
                avformat_seek_file(fmt_, -1, INT64_MIN, 0, 0, 0);
                avcodec_flush_buffers(video_);
                if (audio_) avcodec_flush_buffers(audio_);
                continue;
            }
            if (pkt->stream_index == videoIndex_) {
                if (avcodec_send_packet(video_, pkt) >= 0)
                    while (avcodec_receive_frame(video_, f) == 0) Queue(f, loopOffset, lastPts, frameDur);
            } else if (audio_ && pkt->stream_index == audioIndex) {
                if (avcodec_send_packet(audio_, pkt) >= 0)
                    while (avcodec_receive_frame(audio_, f) == 0) {
                        int maxOut = swr_get_out_samples(swr_, f->nb_samples);
                        size_t base = pending.size();
                        pending.resize(base + size_t(maxOut) * 2);
                        uint8_t* outp = reinterpret_cast<uint8_t*>(pending.data() + base);
                        int n = swr_convert(swr_, &outp, maxOut, const_cast<const uint8_t**>(f->extended_data),
                                            f->nb_samples);
                        pending.resize(base + size_t(n > 0 ? n : 0) * 2);
                        av_frame_unref(f);
                    }
            }
            av_packet_unref(pkt);
        }
        av_frame_free(&f);
        av_packet_free(&pkt);
    }

    void Queue(AVFrame* f, double loopOffset, double& lastPts, double& frameDur) {
        const AVStream* st = fmt_->streams[videoIndex_];
        int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        double pts = ts == AV_NOPTS_VALUE ? lastPts + frameDur : (ts - startPts_) * av_q2d(st->time_base) + loopOffset;
        if (pts > lastPts && pts - lastPts < 1.0) frameDur = pts - lastPts;
        lastPts = pts;
        AVFrame* copy;
        if (f->format == AV_PIX_FMT_D3D11) {  // GPU surface: bring it back to memory for swscale
            copy = av_frame_alloc();
            if (av_hwframe_transfer_data(copy, f, 0) < 0) {
                av_frame_free(&copy);
                av_frame_unref(f);
                return;
            }
        } else {
            copy = av_frame_clone(f);
        }
        av_frame_unref(f);
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({copy, pts});
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* video_ = nullptr;
    AVCodecContext* audio_ = nullptr;
    SwsContext* sws_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    int videoIndex_ = -1;
    int64_t startPts_ = 0;
    uint32_t displayW_ = 0, displayH_ = 0;
    int turnsCW_ = 0;
    std::vector<uint8_t> scratch_;

    std::unique_ptr<AudioOut> audioOut_;  // owned by the player thread
    std::atomic<const AudioOut*> clockAudio_{nullptr};
    DWORD clockStart_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::deque<Frame> queue_;
};

}  // namespace

std::vector<uint8_t> codec::MakeVideoBlob(const char* utf8Path) {
    std::vector<uint8_t> blob(kMagic, kMagic + sizeof(kMagic));
    blob.insert(blob.end(), utf8Path, utf8Path + strlen(utf8Path) + 1);
    while (blob.size() < 12) blob.push_back(0);  // OpenImageSource wants at least 12 bytes
    return blob;
}

bool codec::IsVideoBlob(const uint8_t* data, size_t size) {
    return size > sizeof(kMagic) && memcmp(data, kMagic, sizeof(kMagic)) == 0 && data[size - 1] == 0;
}

std::unique_ptr<ImageSource> codec::OpenVideo(const uint8_t* data, size_t size) {
    if (!IsVideoBlob(data, size)) return nullptr;
    std::string path(reinterpret_cast<const char*>(data) + sizeof(kMagic));
    auto src = std::make_unique<VideoSource>();
    if (src->Open(path)) return src;
    Log("video: cannot open %s", path.c_str());
    return nullptr;
}
