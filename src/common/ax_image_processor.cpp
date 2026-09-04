// 文件说明：包装平台图像处理器，确保 IVPS 输出继承输入帧时间元数据。
#include "common/ax_image_processor.h"

#include <utility>

#include "ax_image_internal.h"
#include "ax_image_processor_internal.h"

namespace axvsdk::common {
namespace {

// 在统一入口复制时间元数据，避免各芯片 IVPS 实现遗漏该契约。
class MetadataPreservingImageProcessor final : public ImageProcessor {
public:
    explicit MetadataPreservingImageProcessor(std::unique_ptr<ImageProcessor> backend)
        : backend_(std::move(backend)) {}

    AxImage::Ptr Process(const AxImage& source, const ImageProcessRequest& request) override {
        auto output = backend_ ? backend_->Process(source, request) : nullptr;
        if (output) {
            internal::AxImageAccess::CopyFrameTiming(source, output.get());
        }
        return output;
    }

    bool Process(const AxImage& source,
                 const ImageProcessRequest& request,
                 AxImage& destination) override {
        if (!backend_ || !backend_->Process(source, request, destination)) {
            return false;
        }
        internal::AxImageAccess::CopyFrameTiming(source, &destination);
        return true;
    }

    ImageProcessorStats stats() const noexcept override {
        return backend_ ? backend_->stats() : ImageProcessorStats{};
    }

private:
    std::unique_ptr<ImageProcessor> backend_;
};

}  // namespace

std::unique_ptr<ImageProcessor> CreateImageProcessor() {
    return CreateImageProcessor(ImageProcessorOptions{});
}

std::unique_ptr<ImageProcessor> CreateImageProcessor(const ImageProcessorOptions& options) {
    auto backend = internal::CreatePlatformImageProcessor(options);
    return backend ? std::make_unique<MetadataPreservingImageProcessor>(std::move(backend)) : nullptr;
}

}  // namespace axvsdk::common
