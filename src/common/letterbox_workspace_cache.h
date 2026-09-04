// 文件说明：提供与 AX API 无关的 letterbox 布局键、有界 LRU 画布账本和线程安全统计。
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "common/ax_image_processor.h"

namespace axvsdk::common::internal {

// 完整描述会改变 letterbox 有效区域或背景内容的输入；宁可降低命中率，
// 也不能让不同布局错误共享仍含上一帧像素的画布。
struct LetterboxLayoutKey {
    ImageDescriptor source{};
    bool crop_enabled{false};
    CropRect crop{};
    ImageDescriptor intermediate{};
    PixelFormat output_format{PixelFormat::kUnknown};
    std::uint32_t output_width{0};
    std::uint32_t output_height{0};
    ResizeMode resize_mode{ResizeMode::kStretch};
    ResizeAlign horizontal_align{ResizeAlign::kCenter};
    ResizeAlign vertical_align{ResizeAlign::kCenter};
    std::uint32_t background_color{0};

    bool operator==(const LetterboxLayoutKey& other) const noexcept {
        return source.format == other.source.format && source.width == other.source.width &&
               source.height == other.source.height && source.strides == other.source.strides &&
               crop_enabled == other.crop_enabled && crop.x == other.crop.x &&
               crop.y == other.crop.y && crop.width == other.crop.width &&
               crop.height == other.crop.height &&
               intermediate.format == other.intermediate.format &&
               intermediate.width == other.intermediate.width &&
               intermediate.height == other.intermediate.height &&
               intermediate.strides == other.intermediate.strides &&
               output_format == other.output_format && output_width == other.output_width &&
               output_height == other.output_height && resize_mode == other.resize_mode &&
               horizontal_align == other.horizontal_align &&
               vertical_align == other.vertical_align && background_color == other.background_color;
    }
};

// 根据已经解析完成的图像描述构造布局键；不得使用尚未补齐宽高/stride 的请求描述。
inline LetterboxLayoutKey MakeLetterboxLayoutKey(const ImageDescriptor& source,
                                                  const ImageProcessRequest& request,
                                                  const ImageDescriptor& intermediate,
                                                  const ImageDescriptor& output) noexcept {
    LetterboxLayoutKey result{};
    result.source = source;
    result.crop_enabled = request.enable_crop;
    result.crop = request.crop;
    result.intermediate = intermediate;
    result.output_format = output.format;
    result.output_width = output.width;
    result.output_height = output.height;
    result.resize_mode = request.resize.mode;
    result.horizontal_align = request.resize.horizontal_align;
    result.vertical_align = request.resize.vertical_align;
    result.background_color = request.resize.background_color;
    return result;
}

// 本账本不自行加锁；Process owner 必须串行访问。entries_ 按 LRU 到 MRU 排列，
// 构造时一次 reserve，稳态命中不产生动态分配。
template <typename Key, typename Resource>
class BoundedLruWorkspaceCache {
public:
    struct Entry {
        Key key{};
        Resource resource{};
        std::size_t bytes{0};
        bool background_valid{false};
    };

    explicit BoundedLruWorkspaceCache(std::size_t capacity) : capacity_(capacity) {
        entries_.reserve(capacity_);
    }

    Entry* Find(const Key& key) {
        const auto found = std::find_if(entries_.begin(), entries_.end(),
                                        [&](const Entry& entry) { return entry.key == key; });
        if (found == entries_.end()) return nullptr;
        if (std::next(found) != entries_.end()) {
            Entry value = std::move(*found);
            entries_.erase(found);
            entries_.push_back(std::move(value));
        }
        return &entries_.back();
    }

    Entry* Insert(Key key, Resource resource, std::size_t bytes, bool background_valid) {
        if (capacity_ == 0U || entries_.size() >= capacity_) return nullptr;
        entries_.push_back(Entry{std::move(key), std::move(resource), bytes, background_valid});
        total_bytes_ += bytes;
        high_watermark_ = std::max(high_watermark_, entries_.size());
        return &entries_.back();
    }

    std::optional<Entry> TakeLeastRecentlyUsed() {
        if (entries_.empty()) return std::nullopt;
        Entry result = std::move(entries_.front());
        total_bytes_ -= result.bytes;
        entries_.erase(entries_.begin());
        return result;
    }

    void Clear() noexcept {
        entries_.clear();
        total_bytes_ = 0U;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size() const noexcept { return entries_.size(); }
    std::size_t bytes() const noexcept { return total_bytes_; }
    std::size_t high_watermark() const noexcept { return high_watermark_; }
    bool empty() const noexcept { return entries_.empty(); }

private:
    std::size_t capacity_{0};
    std::vector<Entry> entries_;
    std::size_t total_bytes_{0};
    std::size_t high_watermark_{0};
};

// 热路径累计量使用 relaxed 原子；资源当前值用互斥锁形成自洽快照，
// 仅在首次分配、淘汰、销毁和查询时进入该锁。
class AtomicImageProcessorStats {
public:
    AtomicImageProcessorStats(bool supported, std::size_t configured_capacity) noexcept
        : supported_(supported), configured_capacity_(configured_capacity) {}

    void RecordEligibleRequest() noexcept { eligible_requests_.fetch_add(1U, std::memory_order_relaxed); }
    void RecordCacheHit() noexcept { cache_hits_.fetch_add(1U, std::memory_order_relaxed); }
    void RecordCacheMiss() noexcept { cache_misses_.fetch_add(1U, std::memory_order_relaxed); }
    void RecordBackgroundInitialization() noexcept {
        background_initializations_.fetch_add(1U, std::memory_order_relaxed);
    }
    void RecordEviction() noexcept { evictions_.fetch_add(1U, std::memory_order_relaxed); }
    void RecordInvalidation() noexcept { invalidations_.fetch_add(1U, std::memory_order_relaxed); }

    void AddWorkspace(std::size_t bytes) noexcept {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        ++current_entries_;
        current_cmm_bytes_ += bytes;
        entries_high_watermark_ = std::max(entries_high_watermark_, current_entries_);
        cmm_bytes_high_watermark_ = std::max(cmm_bytes_high_watermark_, current_cmm_bytes_);
    }

    void RemoveWorkspace(std::size_t bytes) noexcept {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        if (current_entries_ != 0U) --current_entries_;
        current_cmm_bytes_ = bytes <= current_cmm_bytes_ ? current_cmm_bytes_ - bytes : 0U;
    }

    ImageProcessorStats Snapshot() const noexcept {
        ImageProcessorStats result{};
        result.persistent_workspace_supported = supported_;
        result.configured_capacity = configured_capacity_;
        result.eligible_requests = eligible_requests_.load(std::memory_order_relaxed);
        result.cache_hits = cache_hits_.load(std::memory_order_relaxed);
        result.cache_misses = cache_misses_.load(std::memory_order_relaxed);
        result.background_initializations =
            background_initializations_.load(std::memory_order_relaxed);
        result.evictions = evictions_.load(std::memory_order_relaxed);
        result.invalidations = invalidations_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(resource_mutex_);
            result.current_entries = current_entries_;
            result.entries_high_watermark = entries_high_watermark_;
            result.current_cmm_bytes = current_cmm_bytes_;
            result.cmm_bytes_high_watermark = cmm_bytes_high_watermark_;
        }
        return result;
    }

private:
    const bool supported_{false};
    const std::size_t configured_capacity_{0};
    std::atomic<std::uint64_t> eligible_requests_{0};
    std::atomic<std::uint64_t> cache_hits_{0};
    std::atomic<std::uint64_t> cache_misses_{0};
    std::atomic<std::uint64_t> background_initializations_{0};
    std::atomic<std::uint64_t> evictions_{0};
    std::atomic<std::uint64_t> invalidations_{0};
    mutable std::mutex resource_mutex_;
    std::size_t current_entries_{0};
    std::size_t entries_high_watermark_{0};
    std::size_t current_cmm_bytes_{0};
    std::size_t cmm_bytes_high_watermark_{0};
};

}  // namespace axvsdk::common::internal
