// 文件说明：验证 letterbox 持久画布的布局隔离、LRU、有界释放和并发统计契约。
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <type_traits>

#include "letterbox_workspace_cache.h"

namespace {

using axvsdk::common::CropRect;
using axvsdk::common::ImageDescriptor;
using axvsdk::common::ImageProcessRequest;
using axvsdk::common::ImageProcessorStats;
using axvsdk::common::PixelFormat;
using axvsdk::common::ResizeAlign;
using axvsdk::common::ResizeMode;
using axvsdk::common::internal::AtomicImageProcessorStats;
using axvsdk::common::internal::BoundedLruWorkspaceCache;
using axvsdk::common::internal::MakeLetterboxLayoutKey;

// 新增统计必须保持旧实现只覆盖两个 Process 函数时仍是合法具体类型。
class LegacyCompatibleProcessor final : public axvsdk::common::ImageProcessor {
public:
    axvsdk::common::AxImage::Ptr Process(
        const axvsdk::common::AxImage&,
        const axvsdk::common::ImageProcessRequest&) override {
        return {};
    }
    bool Process(const axvsdk::common::AxImage&,
                 const axvsdk::common::ImageProcessRequest&,
                 axvsdk::common::AxImage&) override {
        return false;
    }
};

static_assert(!std::is_abstract_v<LegacyCompatibleProcessor>);

ImageDescriptor SourceDescriptor(std::uint32_t width, std::uint32_t height) {
    ImageDescriptor result{};
    result.format = PixelFormat::kNv12;
    result.width = width;
    result.height = height;
    result.strides[0] = width;
    result.strides[1] = width;
    return result;
}

ImageDescriptor IntermediateDescriptor() {
    return SourceDescriptor(1088U, 608U);
}

ImageDescriptor OutputDescriptor() {
    ImageDescriptor result{};
    result.format = PixelFormat::kBgr24;
    result.width = 1088U;
    result.height = 608U;
    result.strides[0] = 1088U * 3U;
    return result;
}

ImageProcessRequest Request() {
    ImageProcessRequest result{};
    result.output_image = OutputDescriptor();
    result.resize.mode = ResizeMode::kKeepAspectRatio;
    result.resize.horizontal_align = ResizeAlign::kCenter;
    result.resize.vertical_align = ResizeAlign::kCenter;
    result.resize.background_color = 0x727272U;
    return result;
}

void TestLayoutKeySeparatesEveryRelevantField() {
    const auto source = SourceDescriptor(1280U, 720U);
    const auto intermediate = IntermediateDescriptor();
    const auto output = OutputDescriptor();
    const auto request = Request();
    const auto baseline = MakeLetterboxLayoutKey(source, request, intermediate, output);
    assert(baseline == MakeLetterboxLayoutKey(source, request, intermediate, output));

    auto changed_source = source;
    changed_source.width = 640U;
    assert(!(baseline == MakeLetterboxLayoutKey(changed_source, request, intermediate, output)));

    changed_source = source;
    changed_source.strides[0] += 16U;
    assert(!(baseline == MakeLetterboxLayoutKey(changed_source, request, intermediate, output)));

    auto changed_request = request;
    changed_request.enable_crop = true;
    changed_request.crop = CropRect{2, 4, 640U, 360U};
    assert(!(baseline == MakeLetterboxLayoutKey(source, changed_request, intermediate, output)));

    changed_request = request;
    changed_request.resize.horizontal_align = ResizeAlign::kStart;
    assert(!(baseline == MakeLetterboxLayoutKey(source, changed_request, intermediate, output)));

    changed_request = request;
    changed_request.resize.vertical_align = ResizeAlign::kEnd;
    assert(!(baseline == MakeLetterboxLayoutKey(source, changed_request, intermediate, output)));

    changed_request = request;
    changed_request.resize.background_color = 0x010203U;
    assert(!(baseline == MakeLetterboxLayoutKey(source, changed_request, intermediate, output)));

    auto changed_intermediate = intermediate;
    changed_intermediate.strides[0] += 16U;
    assert(!(baseline == MakeLetterboxLayoutKey(source, request, changed_intermediate, output)));

    auto changed_output = output;
    changed_output.format = PixelFormat::kRgb24;
    assert(!(baseline == MakeLetterboxLayoutKey(source, request, intermediate, changed_output)));
}

void TestSameLayoutHitsAndLruEvictsOldest() {
    using Resource = std::shared_ptr<int>;
    using Cache = BoundedLruWorkspaceCache<decltype(MakeLetterboxLayoutKey(
        SourceDescriptor(1U, 1U), Request(), IntermediateDescriptor(), OutputDescriptor())), Resource>;

    Cache cache(2U);
    const auto request = Request();
    const auto intermediate = IntermediateDescriptor();
    const auto output = OutputDescriptor();
    const auto key_a = MakeLetterboxLayoutKey(SourceDescriptor(1280U, 720U), request,
                                               intermediate, output);
    const auto key_b = MakeLetterboxLayoutKey(SourceDescriptor(640U, 480U), request,
                                               intermediate, output);
    const auto key_c = MakeLetterboxLayoutKey(SourceDescriptor(1920U, 1080U), request,
                                               intermediate, output);

    auto resource_a = std::make_shared<int>(1);
    auto resource_b = std::make_shared<int>(2);
    std::weak_ptr<int> observed_b = resource_b;
    assert(cache.Insert(key_a, resource_a, 100U, true) != nullptr);
    assert(cache.Insert(key_b, resource_b, 200U, true) != nullptr);
    resource_b.reset();
    assert(cache.size() == 2U && cache.bytes() == 300U && cache.high_watermark() == 2U);

    // 命中 A 后，B 成为最久未使用项。
    auto* hit_a = cache.Find(key_a);
    assert(hit_a != nullptr && hit_a->resource == resource_a && hit_a->background_valid);
    auto victim = cache.TakeLeastRecentlyUsed();
    assert(victim.has_value() && victim->key == key_b && *victim->resource == 2);
    victim.reset();
    assert(observed_b.expired());

    auto resource_c = std::make_shared<int>(3);
    assert(cache.Insert(key_c, resource_c, 300U, false) != nullptr);
    assert(cache.Find(key_b) == nullptr);
    assert(cache.Find(key_a) != nullptr);
    auto* hit_c = cache.Find(key_c);
    assert(hit_c != nullptr && !hit_c->background_valid);
    hit_c->background_valid = true;
    assert(cache.size() == 2U && cache.bytes() == 400U);

    // 错误路径可以按 Key 立即删除条目，不会留下无效资源或错误字节账本。
    auto removed_c = cache.Remove(key_c);
    assert(removed_c.has_value() && removed_c->resource == resource_c);
    assert(cache.Find(key_c) == nullptr);
    assert(cache.size() == 1U && cache.bytes() == 100U);
    assert(!cache.Remove(key_c).has_value());

    assert(cache.Insert(key_b, std::make_shared<int>(4), 400U, true) != nullptr);
    assert(cache.Insert(key_c, std::make_shared<int>(5), 500U, true) == nullptr);
    cache.Clear();
    assert(cache.empty() && cache.bytes() == 0U);
}

void TestStatisticsAreSafeDuringConcurrentSnapshots() {
    AtomicImageProcessorStats stats(true, 8U);
    std::atomic<bool> writer_done{false};
    std::thread writer([&] {
        for (std::uint64_t i = 0; i < 100000U; ++i) {
            stats.RecordEligibleRequest();
            stats.RecordCacheHit();
            if ((i % 1000U) == 0U) {
                stats.RecordCacheMiss();
                stats.RecordBackgroundInitialization();
                stats.RecordInvalidation();
            }
        }
        stats.AddWorkspace(992256U);
        stats.RemoveWorkspace(992256U);
        writer_done.store(true, std::memory_order_release);
    });

    while (!writer_done.load(std::memory_order_acquire)) {
        const ImageProcessorStats snapshot = stats.Snapshot();
        assert(snapshot.persistent_workspace_supported);
        assert(snapshot.configured_capacity == 8U);
        assert(snapshot.current_entries <= snapshot.entries_high_watermark);
        assert(snapshot.current_cmm_bytes <= snapshot.cmm_bytes_high_watermark);
    }
    writer.join();

    const auto final = stats.Snapshot();
    assert(final.eligible_requests == 100000U);
    assert(final.cache_hits == 100000U);
    assert(final.cache_misses == 100U);
    assert(final.background_initializations == 100U);
    assert(final.invalidations == 100U);
    assert(final.current_entries == 0U);
    assert(final.entries_high_watermark == 1U);
    assert(final.current_cmm_bytes == 0U);
    assert(final.cmm_bytes_high_watermark == 992256U);
}

void TestLegacyProcessorReportsUnsupportedDefaults() {
    LegacyCompatibleProcessor processor;
    const auto stats = processor.stats();
    assert(!stats.persistent_workspace_supported);
    assert(stats.configured_capacity == 0U);
    assert(stats.eligible_requests == 0U);
}

}  // namespace

int main() {
    TestLayoutKeySeparatesEveryRelevantField();
    TestSameLayoutHitsAndLruEvictsOldest();
    TestStatisticsAreSafeDuringConcurrentSnapshots();
    TestLegacyProcessorReportsUnsupportedDefaults();
    std::cout << "letterbox workspace cache contract: PASS\n";
    return 0;
}
