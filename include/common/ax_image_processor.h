#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "common/ax_image.h"

namespace axvsdk::common {

// resize 模式。
// kStretch: 直接拉伸到目标尺寸。
// kKeepAspectRatio: 保持原始宽高比，多余区域用 background_color 填充。
enum class ResizeMode {
    kStretch = 0,
    kKeepAspectRatio,
};

// 当 KeepAspectRatio 产生留边时，控制图像在目标画面中的对齐方式。
enum class ResizeAlign {
    kCenter = 0,
    kStart,
    kEnd,
};

struct ResizeOptions {
    ResizeMode mode{ResizeMode::kStretch};
    ResizeAlign horizontal_align{ResizeAlign::kCenter};
    ResizeAlign vertical_align{ResizeAlign::kCenter};
    // 留边填充值，当前按 0xRRGGBB 传入。
    // NV12 输出时由底层转换成对应 YUV 背景色。
    std::uint32_t background_color{0};
};

struct CropRect {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

struct ImageProcessRequest {
    // 目标图像描述。
    // 未填写的 format/width/height 可由调用方按具体 API 语义补全。
    ImageDescriptor output_image{};
    bool enable_crop{false};
    CropRect crop{};
    ResizeOptions resize{};
};

// 图像处理器的资源策略。容量为 0 时完全保持逐帧填充中间画布的兼容行为；
// 大于 0 表示允许平台实现按 letterbox 布局持久复用最多指定数量的画布。
struct ImageProcessorOptions {
    std::size_t persistent_letterbox_workspace_capacity{0};
};

// 图像处理器的只读累计统计。统计不保存逐帧数据，也不改变处理结果。
struct ImageProcessorStats {
    bool persistent_workspace_supported{false};
    std::size_t configured_capacity{0};
    std::uint64_t eligible_requests{0};
    std::uint64_t cache_hits{0};
    std::uint64_t cache_misses{0};
    std::uint64_t background_initializations{0};
    std::uint64_t evictions{0};
    std::uint64_t invalidations{0};
    std::size_t current_entries{0};
    std::size_t entries_high_watermark{0};
    std::size_t current_cmm_bytes{0};
    std::size_t cmm_bytes_high_watermark{0};
};

class ImageProcessor {
public:
    virtual ~ImageProcessor() = default;

    // 返回值版本：
    // 由库内部新申请一张输出图像并返回。
    // 默认适合更重视易用性的场景。
    virtual AxImage::Ptr Process(const AxImage& source, const ImageProcessRequest& request) = 0;
    // 出参版本：
    // 调用方自己准备 destination，库只负责写入内容。
    // 更适合需要复用目标缓冲、减少重复分配的场景。
    virtual bool Process(const AxImage& source, const ImageProcessRequest& request, AxImage& destination) = 0;
    // 返回值快照可与 Process 并发读取；不返回内部画布或其他可变对象。
    virtual ImageProcessorStats stats() const noexcept { return {}; }
};

std::unique_ptr<ImageProcessor> CreateImageProcessor();
// 带资源策略版本；不支持持久画布的平台会在 stats() 中明确报告 unsupported。
std::unique_ptr<ImageProcessor> CreateImageProcessor(const ImageProcessorOptions& options);

}  // namespace axvsdk::common
