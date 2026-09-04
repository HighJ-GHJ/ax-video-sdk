// 文件说明：声明由各芯片后端实现的 ImageProcessor 内部工厂。
#pragma once

#include <memory>

#include "common/ax_image_processor.h"

namespace axvsdk::common::internal {

std::unique_ptr<ImageProcessor> CreatePlatformImageProcessor(const ImageProcessorOptions& options);

}  // namespace axvsdk::common::internal
