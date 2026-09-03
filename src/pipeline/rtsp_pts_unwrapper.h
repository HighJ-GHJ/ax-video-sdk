// 文件说明：将 rtsp-sdk 输出的 32 位 90kHz RTP 毫秒时间轴延展为微秒时间轴。
#pragma once

#include <cstdint>

namespace axvsdk::pipeline::internal {

struct RtspPtsResult {
    std::uint64_t pts_us{0};
    bool wrapped{false};
    bool regressed{false};
};

// rtsp-sdk 当前先将 RTP timestamp 除以 90 转成毫秒，因此这里保留 1ms 精度。
// 只有跨越半个 32 位周期的大幅回退才视为正常回绕；其他回退原样
// 暴露给上层，使 Person 可以结束旧 stream epoch。
class RtspPtsUnwrapper {
public:
    RtspPtsResult Normalize(std::uint64_t pts_ms) noexcept {
        constexpr std::uint64_t kWrapUs =
            (static_cast<std::uint64_t>(1) << 32U) * 1000000ULL / 90000ULL;
        constexpr std::uint64_t kHalfWrapMs = kWrapUs / 2000ULL;

        RtspPtsResult result{};
        if (initialized_ && pts_ms < last_pts_ms_) {
            const auto backwards_ms = last_pts_ms_ - pts_ms;
            if (backwards_ms > kHalfWrapMs) {
                wrap_offset_us_ += kWrapUs;
                result.wrapped = true;
            } else {
                result.regressed = true;
            }
        }
        initialized_ = true;
        last_pts_ms_ = pts_ms;
        result.pts_us = wrap_offset_us_ + pts_ms * 1000ULL;
        return result;
    }

    void Reset() noexcept {
        initialized_ = false;
        last_pts_ms_ = 0;
        wrap_offset_us_ = 0;
    }

private:
    bool initialized_{false};
    std::uint64_t last_pts_ms_{0};
    std::uint64_t wrap_offset_us_{0};
};

}  // namespace axvsdk::pipeline::internal
