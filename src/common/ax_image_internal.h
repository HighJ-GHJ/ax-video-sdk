// 文件说明：声明 AxImage 的 SDK 内部访问能力与元数据复制边界。
#pragma once

#include <functional>
#include <memory>

#include "ax_global_type.h"

#include "common/ax_image.h"

namespace axvsdk::common::internal {

struct AxImageAccess {
    using FrameReleaseCallback = std::function<void(const AX_VIDEO_FRAME_INFO_T&)>;

    static AxImage::Ptr WrapVideoFrame(const AX_VIDEO_FRAME_INFO_T& frame_info,
                                       FrameReleaseCallback release_callback = {});

    static const AX_VIDEO_FRAME_INFO_T& GetAxFrameInfo(const AxImage& image) noexcept;
    static AX_VIDEO_FRAME_INFO_T* MutableAxFrameInfo(AxImage* image) noexcept;
    static const AX_VIDEO_FRAME_T& GetAxFrame(const AxImage& image) noexcept;
    static AX_VIDEO_FRAME_T* MutableAxFrame(AxImage* image) noexcept;
    static void AttachLifetime(AxImage* image, std::shared_ptr<void> lifetime) noexcept;
    static void SetFrameTiming(AxImage* image, FrameTiming timing) noexcept;
    // 只复制媒体时间，不覆盖 IVPS 已生成的目标尺寸/crop 元数据。
    static void CopyFrameTiming(const AxImage& source, AxImage* destination) noexcept;
    // 复制同尺寸图像的时间/crop 元数据，但不修改地址、stride 或 block id。
    static void CopyFrameMetadata(const AxImage& source, AxImage* destination) noexcept;
};

}  // namespace axvsdk::common::internal
