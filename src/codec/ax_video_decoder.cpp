// 文件说明：实现 AX 视频解码线程、有界回调投递和资源回收。
#include "ax_video_decoder_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>

#include "ax_image_copy.h"
#include "ax_image_internal.h"
#if defined(AXSDK_PLATFORM_AXCL)
#include "axcl_sys.h"
#define AX_POOL_IncreaseRefCnt AXCL_POOL_IncreaseRefCnt
#define AX_POOL_DecreaseRefCnt AXCL_POOL_DecreaseRefCnt
#include "ax_system_internal.h"
#else
#include "ax_sys_api.h"
#endif

#include "common/ax_system.h"

namespace axvsdk::codec {

std::unique_ptr<VideoDecoder> CreateVideoDecoder() {
    return internal::CreatePlatformVideoDecoder();
}

}  // namespace axvsdk::codec

namespace axvsdk::codec::internal {

namespace {

constexpr std::size_t kDecodeInputQueueDepth = 16;
common::ImageDescriptor MakeNativeOutputDescriptor(const Mp4VideoInfo& video_info) noexcept {
    common::ImageDescriptor descriptor{};
    descriptor.format = common::PixelFormat::kNv12;
    descriptor.width = video_info.width;
    descriptor.height = video_info.height;
    return descriptor;
}

Mp4VideoInfo MakeDecoderVideoInfo(const VideoStreamInfo& stream) noexcept {
    Mp4VideoInfo video_info{};
    video_info.codec = stream.codec;
    video_info.width = stream.width;
    video_info.height = stream.height;
    video_info.timescale = 1000000U;
    video_info.fps = stream.frame_rate > 0.0 ? stream.frame_rate : 30.0;
    return video_info;
}

}  // namespace

AxVideoDecoderBase::~AxVideoDecoderBase() = default;

bool AxVideoDecoderBase::Open(const VideoDecoderConfig& config) {
    if (open_) {
        Close();
    }

    if (!common::IsSystemInitialized()) {
        return false;
    }

    config_ = config;
    if ((config.stream.codec != VideoCodecType::kH264 && config.stream.codec != VideoCodecType::kH265) ||
        config.stream.width == 0 || config.stream.height == 0 ||
        config.callback_queue_capacity == 0U || config.callback_queue_capacity > 8U) {
        return false;
    }

    video_info_ = MakeDecoderVideoInfo(config.stream);
    native_output_descriptor_ = MakeNativeOutputDescriptor(video_info_);
    if (!ValidateRequestedOutput(config_.output_image)) {
        return false;
    }

    if (!CreateBackend(video_info_)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_packets_.clear();
        input_eos_requested_ = false;
    }
    send_finished_ = false;
    callback_stop_ = false;
    received_frames_.store(0, std::memory_order_relaxed);
    callback_enqueued_frames_.store(0, std::memory_order_relaxed);
    callback_delivered_frames_.store(0, std::memory_order_relaxed);
    callback_dropped_frames_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback_frames_.Reset();
    }
    open_ = true;
    return true;
}

void AxVideoDecoderBase::Close() noexcept {
    Stop();
    if (open_) {
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            pending_packets_.clear();
            input_eos_requested_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(latest_mutex_);
            latest_frame_.reset();
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback_frames_.Clear();
            frame_callback_ = {};
            callback_mode_ = FrameCallbackMode::kLatest;
        }
        DestroyBackend();
        open_ = false;
    }
}

bool AxVideoDecoderBase::Start() {
    if (!open_ || running_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_packets_.clear();
        input_eos_requested_ = false;
    }
    send_finished_ = false;
    stop_requested_ = false;
    callback_stop_ = false;

    if (!StartBackend()) {
        return false;
    }

    callback_thread_ = std::thread(&AxVideoDecoderBase::CallbackLoop, this);
    receive_thread_ = std::thread(&AxVideoDecoderBase::ReceiveLoop, this);
    send_thread_ = std::thread(&AxVideoDecoderBase::SendLoop, this);
    running_ = true;
    return true;
}

void AxVideoDecoderBase::Stop() noexcept {
    if (!running_) {
        return;
    }

    stop_requested_ = true;
    input_cv_.notify_all();
    callback_cv_.notify_all();

    if (send_thread_.joinable()) {
        send_thread_.join();
    }
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    StopBackend();

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        // Stop 是中止语义：不在关闭阶段继续回调业务，并立即释放 pool 帧引用。
        callback_frames_.Clear();
        callback_stop_ = true;
    }
    callback_cv_.notify_all();
    if (callback_thread_.joinable()) {
        callback_thread_.join();
    }

    running_ = false;
}

bool AxVideoDecoderBase::SubmitPacket(EncodedPacket packet) {
    if (!running_ || packet.data.empty()) {
        std::fprintf(stderr,
                     "vdec SubmitPacket reject (early): running=%d bytes=%zu\n",
                     running_ ? 1 : 0, packet.data.size());
        return false;
    }

    std::unique_lock<std::mutex> lock(input_mutex_);
    input_cv_.wait(lock, [this] {
        return stop_requested_ || pending_packets_.size() < kDecodeInputQueueDepth;
    });
    if (stop_requested_ || input_eos_requested_) {
        std::fprintf(stderr,
                     "vdec SubmitPacket reject: stop=%d eos=%d pending=%zu/%zu bytes=%zu\n",
                     stop_requested_ ? 1 : 0, input_eos_requested_ ? 1 : 0,
                     pending_packets_.size(), kDecodeInputQueueDepth, packet.data.size());
        return false;
    }
    pending_packets_.push_back(std::move(packet));
    lock.unlock();
    input_cv_.notify_one();
    return true;
}

bool AxVideoDecoderBase::SubmitEndOfStream() {
    if (!running_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(input_mutex_);
    input_eos_requested_ = true;
    input_cv_.notify_all();
    return true;
}

common::AxImage::Ptr AxVideoDecoderBase::GetLatestFrame() {
    std::lock_guard<std::mutex> lock(latest_mutex_);
    return latest_frame_;
}

bool AxVideoDecoderBase::GetLatestFrame(common::AxImage& output_image) {
    common::AxImage::Ptr latest_frame;
    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_frame = latest_frame_;
    }

    if (!latest_frame) {
        return false;
    }

    return common::internal::CopyImage(*latest_frame, &output_image);
}

void AxVideoDecoderBase::SetFrameCallback(FrameCallback callback) {
    SetFrameCallback(std::move(callback), FrameCallbackMode::kLatest);
}

void AxVideoDecoderBase::SetFrameCallback(FrameCallback callback, FrameCallbackMode mode) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = std::move(callback);
    callback_mode_ = mode;
    callback_frames_.Clear();
    callback_cv_.notify_all();
}

VideoDecoderStats AxVideoDecoderBase::GetStats() const {
    VideoDecoderStats stats{};
    stats.received_frames = received_frames_.load(std::memory_order_relaxed);
    stats.callback_enqueued_frames = callback_enqueued_frames_.load(std::memory_order_relaxed);
    stats.callback_delivered_frames = callback_delivered_frames_.load(std::memory_order_relaxed);
    stats.callback_dropped_frames = callback_dropped_frames_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        stats.callback_queue_depth =
            callback_frames_.Depth(callback_mode_ == FrameCallbackMode::kQueue);
        stats.callback_queue_high_watermark = callback_frames_.high_watermark();
    }
    return stats;
}

const Mp4VideoInfo& AxVideoDecoderBase::video_info() const noexcept {
    return video_info_;
}

const VideoDecoderConfig& AxVideoDecoderBase::config() const noexcept {
    return config_;
}

const common::ImageDescriptor& AxVideoDecoderBase::native_output_descriptor() const noexcept {
    return native_output_descriptor_;
}

bool AxVideoDecoderBase::stop_requested() const noexcept {
    return stop_requested_.load(std::memory_order_relaxed);
}

void AxVideoDecoderBase::SendLoop() {
    while (!stop_requested_) {
        EncodedPacket packet;
        {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait(lock, [this] {
                return stop_requested_ || !pending_packets_.empty() || input_eos_requested_;
            });

            if (stop_requested_) {
                break;
            }

            if (pending_packets_.empty()) {
                if (input_eos_requested_) {
                    break;
                }
                continue;
            }

            packet = std::move(pending_packets_.front());
            pending_packets_.pop_front();
        }
        input_cv_.notify_all();

        if (packet.data.empty()) {
            continue;
        }

        if (!SendEncodedPacket(packet)) {
            std::fprintf(stderr, "vdec SendEncodedPacket failed: pts=%llu bytes=%zu\n",
                         static_cast<unsigned long long>(packet.pts), packet.data.size());
            break;
        }
    }

    if (!stop_requested_) {
        (void)SendEndOfStream();
    }
    send_finished_ = true;
    input_cv_.notify_all();
}

void AxVideoDecoderBase::ReceiveLoop() {
    while (!stop_requested_) {
        AX_VIDEO_FRAME_INFO_T frame_info{};
        bool flow_end = false;
        if (!ReceiveDecodedFrame(&frame_info, &flow_end)) {
            if (flow_end && send_finished_) {
                break;
            }
            continue;
        }
        received_frames_.fetch_add(1, std::memory_order_relaxed);

        // Some firmwares return macroblock-aligned coded dimensions in u32Width/u32Height but
        // don't populate crop fields. Preserve the "real" stream geometry for downstream modules
        // (NPU mapping, VENC attribute match) by filling crop from the known stream info.
        if ((frame_info.stVFrame.s16CropWidth <= 0 || frame_info.stVFrame.s16CropHeight <= 0) &&
            video_info_.width != 0 && video_info_.height != 0 &&
            video_info_.width <= frame_info.stVFrame.u32Width &&
            video_info_.height <= frame_info.stVFrame.u32Height) {
            frame_info.stVFrame.s16CropX = 0;
            frame_info.stVFrame.s16CropY = 0;
            frame_info.stVFrame.s16CropWidth = static_cast<AX_S16>(video_info_.width);
            frame_info.stVFrame.s16CropHeight = static_cast<AX_S16>(video_info_.height);
        }

        PublishFrame(frame_info);
        ReleaseDecodedFrame(frame_info);
    }
}

void AxVideoDecoderBase::CallbackLoop() {
    while (true) {
        FrameCallback callback;
        common::AxImage::Ptr frame;
        {
            std::unique_lock<std::mutex> lock(callback_mutex_);
            callback_cv_.wait(lock, [this] {
                return callback_stop_ || !callback_frames_.empty();
            });

            if (callback_stop_ && callback_frames_.empty()) {
                return;
            }

            callback = frame_callback_;
            frame = callback_frames_.Pop(callback_mode_ == FrameCallbackMode::kQueue);
        }

        if (callback && frame) {
            callback(std::move(frame));
            callback_delivered_frames_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool AxVideoDecoderBase::ValidateRequestedOutput(const common::ImageDescriptor& requested) const noexcept {
    if (requested.format != common::PixelFormat::kUnknown &&
        requested.format != native_output_descriptor_.format) {
        return false;
    }
    if (requested.width != 0 && requested.width != native_output_descriptor_.width) {
        return false;
    }
    if (requested.height != 0 && requested.height != native_output_descriptor_.height) {
        return false;
    }
    return true;
}

void AxVideoDecoderBase::PublishFrame(const AX_VIDEO_FRAME_INFO_T& frame_info) {
    common::AxImage::Ptr published_frame;
    const auto block_id = frame_info.stVFrame.u32BlkId[0];
    if (block_id != AX_INVALID_BLOCKID) {
#if defined(AXSDK_PLATFORM_AXCL)
        if (!common::internal::EnsureAxclThreadContext()) {
            ReleaseDecodedFrame(frame_info);
            return;
        }
#endif
        const auto ref_ret = AX_POOL_IncreaseRefCnt(block_id);
        if (ref_ret == AX_SUCCESS) {
            // Keep the raw frame info unchanged. Some modules (notably VENC) may derive plane offsets internally
            // from width/height/stride, and clamping padded dimensions here can corrupt chroma addressing.
            published_frame = common::internal::AxImageAccess::WrapVideoFrame(
                frame_info, [](const AX_VIDEO_FRAME_INFO_T& retained_frame) {
                    if (retained_frame.stVFrame.u32BlkId[0] != AX_INVALID_BLOCKID) {
#if defined(AXSDK_PLATFORM_AXCL)
                        if (!common::internal::EnsureAxclThreadContext()) {
                            return;
                        }
#endif
                        (void)AX_POOL_DecreaseRefCnt(retained_frame.stVFrame.u32BlkId[0]);
                    }
                });
        }
    }

    if (!published_frame) {
        auto fallback = common::AxImage::Create(native_output_descriptor_);
        if (fallback && common::internal::CopyVideoFrameToImage(frame_info, fallback.get())) {
            published_frame = std::move(fallback);
        }
    }

    if (!published_frame) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(latest_mutex_);
        latest_frame_ = published_frame;
    }

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (frame_callback_) {
            callback_enqueued_frames_.fetch_add(1, std::memory_order_relaxed);
            if (callback_frames_.Push(published_frame,
                                      callback_mode_ == FrameCallbackMode::kQueue,
                                      config_.callback_queue_capacity)) {
                callback_dropped_frames_.fetch_add(1, std::memory_order_relaxed);
            }
            callback_cv_.notify_one();
        }
    }
}

}  // namespace axvsdk::codec::internal
