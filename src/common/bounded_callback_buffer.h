// 文件说明：提供由调用方加锁的 Latest/有界 FIFO 回调帧缓冲及精确深度统计。
#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>

namespace axvsdk::common::internal {

// 本类型不自行加锁；调用方必须用同一互斥锁保护 Push/Pop/Clear/统计查询。
// Push 返回 true 表示为接纳新值而丢弃了一个旧值，生产者永不阻塞。
template <typename T>
class BoundedCallbackBuffer {
public:
    bool Push(T value, bool fifo, std::size_t capacity) {
        bool dropped = false;
        if (!fifo) {
            dropped = static_cast<bool>(latest_);
            latest_ = std::move(value);
        } else {
            fifo_.push_back(std::move(value));
            if (fifo_.size() > capacity) {
                fifo_.pop_front();
                dropped = true;
            }
        }
        high_watermark_ = std::max(high_watermark_, Depth(fifo));
        return dropped;
    }

    T Pop(bool fifo) {
        if (!fifo) return std::exchange(latest_, T{});
        if (fifo_.empty()) return {};
        T value = std::move(fifo_.front());
        fifo_.pop_front();
        return value;
    }

    void Clear() noexcept {
        latest_ = {};
        fifo_.clear();
    }

    void Reset() noexcept {
        Clear();
        high_watermark_ = 0;
    }

    bool empty() const noexcept { return !latest_ && fifo_.empty(); }
    std::size_t Depth(bool fifo) const noexcept {
        return fifo ? fifo_.size() : (latest_ ? 1U : 0U);
    }
    std::size_t high_watermark() const noexcept { return high_watermark_; }

private:
    T latest_{};
    std::deque<T> fifo_;
    std::size_t high_watermark_{0};
};

}  // namespace axvsdk::common::internal
