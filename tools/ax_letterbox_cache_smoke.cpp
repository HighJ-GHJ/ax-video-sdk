// 文件说明：在 AX650 上逐字节比较逐帧背景填充与有界持久 letterbox 画布的 BGR 输出。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ax_cmdline_utils.h"
#include "common/ax_image.h"
#include "common/ax_image_processor.h"
#include "common/ax_system.h"

namespace {

using axvsdk::common::AxImage;
using axvsdk::common::CropRect;
using axvsdk::common::ImageAllocationOptions;
using axvsdk::common::ImageDescriptor;
using axvsdk::common::ImageProcessRequest;
using axvsdk::common::ImageProcessor;
using axvsdk::common::ImageProcessorOptions;
using axvsdk::common::MemoryType;
using axvsdk::common::PixelFormat;
using axvsdk::common::ResizeAlign;
using axvsdk::common::ResizeMode;

struct TestCase {
    const char* name{nullptr};
    std::size_t source_index{0};
    ImageProcessRequest request{};
};

// 保存一组交替 A/B 轮次的每帧耗时，避免单次系统抖动决定结论。
struct BenchmarkSamples {
    std::vector<double> baseline_us_per_frame;
    std::vector<double> cached_us_per_frame;
};

ImageDescriptor Nv12Descriptor(std::uint32_t width, std::uint32_t height) {
    ImageDescriptor result{};
    result.format = PixelFormat::kNv12;
    result.width = width;
    result.height = height;
    result.strides[0] = width;
    result.strides[1] = width;
    return result;
}

ImageDescriptor BgrDescriptor() {
    ImageDescriptor result{};
    result.format = PixelFormat::kBgr24;
    result.width = 1088U;
    result.height = 608U;
    result.strides[0] = 1088U * 3U;
    return result;
}

// 生成包含横纵梯度的确定性 NV12 输入，使残留像素不会被纯色测试掩盖。
bool FillNv12Pattern(AxImage* image, std::uint8_t seed) {
    if (image == nullptr || image->format() != PixelFormat::kNv12) return false;
    auto* y = image->mutable_plane_data(0U);
    auto* uv = image->mutable_plane_data(1U);
    if (y == nullptr || uv == nullptr) return false;
    std::fill_n(y, image->plane_size(0U), static_cast<std::uint8_t>(16U));
    std::fill_n(uv, image->plane_size(1U), static_cast<std::uint8_t>(128U));
    for (std::uint32_t row = 0; row < image->height(); ++row) {
        for (std::uint32_t col = 0; col < image->width(); ++col) {
            y[static_cast<std::size_t>(row) * image->stride(0U) + col] =
                static_cast<std::uint8_t>(16U + ((row * 3U + col * 5U + seed) % 220U));
        }
    }
    for (std::uint32_t row = 0; row < image->height() / 2U; ++row) {
        auto* line = uv + static_cast<std::size_t>(row) * image->stride(1U);
        for (std::uint32_t col = 0; col < image->width(); col += 2U) {
            line[col] = static_cast<std::uint8_t>(64U + ((row + col + seed) % 128U));
            line[col + 1U] = static_cast<std::uint8_t>(64U + ((row * 2U + col + seed) % 128U));
        }
    }
    return image->FlushCache();
}

AxImage::Ptr MakeSource(std::uint32_t width, std::uint32_t height, std::uint8_t seed) {
    ImageAllocationOptions options{};
    options.memory_type = MemoryType::kCmm;
    options.token = "AxLetterboxCacheSmokeSource";
    auto image = AxImage::Create(Nv12Descriptor(width, height), options);
    return image && FillNv12Pattern(image.get(), seed) ? image : nullptr;
}

AxImage::Ptr MakeOutput(const char* token) {
    ImageAllocationOptions options{};
    options.memory_type = MemoryType::kCmm;
    options.token = token;
    return AxImage::Create(BgrDescriptor(), options);
}

ImageProcessRequest MakeRequest(ResizeAlign horizontal,
                                ResizeAlign vertical,
                                std::uint32_t background,
                                bool crop = false,
                                CropRect crop_rect = {}) {
    ImageProcessRequest result{};
    result.output_image = BgrDescriptor();
    result.enable_crop = crop;
    result.crop = crop_rect;
    result.resize.mode = ResizeMode::kKeepAspectRatio;
    result.resize.horizontal_align = horizontal;
    result.resize.vertical_align = vertical;
    result.resize.background_color = background;
    return result;
}

bool EqualBgr(AxImage* baseline, AxImage* cached, std::size_t* first_difference) {
    if (baseline == nullptr || cached == nullptr || baseline->format() != PixelFormat::kBgr24 ||
        cached->format() != PixelFormat::kBgr24 || baseline->descriptor().width != cached->descriptor().width ||
        baseline->descriptor().height != cached->descriptor().height ||
        baseline->stride(0U) != cached->stride(0U) ||
        baseline->plane_size(0U) != cached->plane_size(0U) ||
        !baseline->InvalidateCache() || !cached->InvalidateCache()) {
        return false;
    }
    const auto* left = baseline->plane_data(0U);
    const auto* right = cached->plane_data(0U);
    if (left == nullptr || right == nullptr) return false;
    if (std::memcmp(left, right, baseline->plane_size(0U)) == 0) return true;
    if (first_difference != nullptr) {
        *first_difference = 0U;
        while (*first_difference < baseline->plane_size(0U) &&
               left[*first_difference] == right[*first_difference]) {
            ++*first_difference;
        }
    }
    return false;
}

// 对同一处理器连续执行固定布局序列；计时窗口内不做字节比较或统计输出。
template <std::size_t N>
bool TimePass(ImageProcessor& processor,
              AxImage& output,
              const std::vector<AxImage::Ptr>& sources,
              const std::array<TestCase, N>& cases,
              std::size_t iterations,
              double* us_per_frame) {
    if (iterations == 0U || us_per_frame == nullptr) return false;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        for (const auto& test : cases) {
            if (!processor.Process(*sources[test.source_index], test.request, output)) return false;
        }
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - begin).count();
    *us_per_frame = elapsed / static_cast<double>(iterations * N);
    return true;
}

// 计算轮次中位数；偶数样本取中间两值平均，便于六轮 A/B 直接比较。
double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2U;
    return values.size() % 2U == 0U ? (values[middle - 1U] + values[middle]) / 2.0
                                    : values[middle];
}

// 按轮次交换 baseline/cached 先后顺序，降低温度、频率和板端其他负载的单向偏差。
template <std::size_t N>
bool RunAlternatingBenchmark(const char* label,
                             ImageProcessor& baseline,
                             ImageProcessor& cached,
                             AxImage& baseline_output,
                             AxImage& cached_output,
                             const std::vector<AxImage::Ptr>& sources,
                             const std::array<TestCase, N>& cases,
                             std::size_t iterations,
                             std::size_t rounds) {
    BenchmarkSamples samples;
    samples.baseline_us_per_frame.reserve(rounds);
    samples.cached_us_per_frame.reserve(rounds);
    for (std::size_t round = 0; round < rounds; ++round) {
        double baseline_us = 0.0;
        double cached_us = 0.0;
        const bool baseline_first = (round % 2U) == 0U;
        const bool ok = baseline_first
            ? TimePass(baseline, baseline_output, sources, cases, iterations, &baseline_us) &&
                  TimePass(cached, cached_output, sources, cases, iterations, &cached_us)
            : TimePass(cached, cached_output, sources, cases, iterations, &cached_us) &&
                  TimePass(baseline, baseline_output, sources, cases, iterations, &baseline_us);
        if (!ok) return false;
        samples.baseline_us_per_frame.push_back(baseline_us);
        samples.cached_us_per_frame.push_back(cached_us);
        std::cout << std::fixed << std::setprecision(3)
                  << "LETTERBOX_CACHE_BENCH_ROUND label=" << label
                  << " round=" << round
                  << " order=" << (baseline_first ? "baseline_cached" : "cached_baseline")
                  << " baseline_us_per_frame=" << baseline_us
                  << " cached_us_per_frame=" << cached_us << '\n';
    }

    const double baseline_median = Median(samples.baseline_us_per_frame);
    const double cached_median = Median(samples.cached_us_per_frame);
    const double improvement_percent = baseline_median == 0.0
        ? 0.0
        : (baseline_median - cached_median) * 100.0 / baseline_median;
    std::cout << std::fixed << std::setprecision(3)
              << "LETTERBOX_CACHE_BENCH_SUMMARY label=" << label
              << " rounds=" << rounds
              << " iterations=" << iterations
              << " frames_per_iteration=" << N
              << " baseline_median_us_per_frame=" << baseline_median
              << " cached_median_us_per_frame=" << cached_median
              << " improvement_percent=" << improvement_percent << '\n';
    return true;
}

int Run(std::size_t capacity,
        std::size_t iterations,
        std::size_t benchmark_iterations,
        std::size_t benchmark_rounds) {
    if (capacity == 0U || iterations == 0U || benchmark_rounds == 0U) return 2;

    // 两个处理器必须走同一硬件单元，否则不同 engine 的舍入差异会污染等价性结论。
    (void)setenv("AXVSDK_IVPS_CROPRESIZE_ENGINE", "vpp", 1);
    (void)setenv("AXVSDK_IVPS_CSC_ENGINE", "tdp", 1);

    axvsdk::common::SystemOptions system_options{};
    system_options.enable_ivps = true;
    if (!axvsdk::common::InitializeSystem(system_options)) {
        std::cerr << "InitializeSystem failed\n";
        return 3;
    }
    struct SystemGuard {
        ~SystemGuard() { axvsdk::common::ShutdownSystem(); }
    } system_guard;

    auto baseline = axvsdk::common::CreateImageProcessor();
    ImageProcessorOptions cached_options{};
    cached_options.persistent_letterbox_workspace_capacity = capacity;
    auto cached = axvsdk::common::CreateImageProcessor(cached_options);
    auto baseline_output = MakeOutput("AxLetterboxCacheSmokeBaseline");
    auto cached_output = MakeOutput("AxLetterboxCacheSmokeCached");
    std::vector<AxImage::Ptr> sources;
    sources.push_back(MakeSource(1280U, 720U, 11U));
    sources.push_back(MakeSource(640U, 480U, 37U));
    sources.push_back(MakeSource(1920U, 1080U, 73U));
    if (!baseline || !cached || !baseline_output || !cached_output ||
        std::any_of(sources.begin(), sources.end(), [](const auto& source) { return !source; })) {
        std::cerr << "create processor/image failed\n";
        return 4;
    }
    const auto initial_stats = cached->stats();
    if (!initial_stats.persistent_workspace_supported ||
        initial_stats.configured_capacity != capacity) {
        std::cerr << "persistent workspace unsupported or capacity mismatch\n";
        return 5;
    }

    const std::array<TestCase, 8> cases{{
        {"A-center-1", 0U, MakeRequest(ResizeAlign::kCenter, ResizeAlign::kCenter, 0x727272U)},
        {"A-center-2", 0U, MakeRequest(ResizeAlign::kCenter, ResizeAlign::kCenter, 0x727272U)},
        {"B-center", 1U, MakeRequest(ResizeAlign::kCenter, ResizeAlign::kCenter, 0x727272U)},
        {"A-center-3", 0U, MakeRequest(ResizeAlign::kCenter, ResizeAlign::kCenter, 0x727272U)},
        {"C-start", 2U, MakeRequest(ResizeAlign::kStart, ResizeAlign::kStart, 0x727272U)},
        {"A-end-color", 0U, MakeRequest(ResizeAlign::kEnd, ResizeAlign::kEnd, 0x102030U)},
        {"A-crop", 0U, MakeRequest(ResizeAlign::kCenter, ResizeAlign::kCenter, 0x727272U,
                                    true, CropRect{100, 50, 640U, 360U})},
        {"B-center-repeat", 1U, MakeRequest(ResizeAlign::kCenter, ResizeAlign::kCenter, 0x727272U)},
    }};

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        for (const auto& test : cases) {
            if (!baseline->Process(*sources[test.source_index], test.request, *baseline_output) ||
                !cached->Process(*sources[test.source_index], test.request, *cached_output)) {
                std::cerr << "Process failed case=" << test.name << " iteration=" << iteration << "\n";
                return 6;
            }
            std::size_t difference = 0U;
            if (!EqualBgr(baseline_output.get(), cached_output.get(), &difference)) {
                std::cerr << "BGR mismatch case=" << test.name << " iteration=" << iteration
                          << " first_difference=" << difference << "\n";
                return 7;
            }
        }
    }

    const auto baseline_stats = baseline->stats();
    const auto cached_stats = cached->stats();
    if (cached_stats.current_entries > capacity || cached_stats.entries_high_watermark > capacity ||
        cached_stats.cache_hits == 0U || cached_stats.cache_misses == 0U ||
        baseline_stats.cache_hits != 0U || baseline_stats.cache_misses != 0U) {
        std::cerr << "workspace statistics contract failed\n";
        return 8;
    }
    std::cout << "LETTERBOX_CACHE_SMOKE PASS capacity=" << capacity
              << " iterations=" << iterations
              << " baseline_background_initializations="
              << baseline_stats.background_initializations
              << " cache_hits=" << cached_stats.cache_hits
              << " cache_misses=" << cached_stats.cache_misses
              << " background_initializations=" << cached_stats.background_initializations
              << " evictions=" << cached_stats.evictions
              << " invalidations=" << cached_stats.invalidations
              << " current_entries=" << cached_stats.current_entries
              << " entries_high_watermark=" << cached_stats.entries_high_watermark
              << " current_cmm_bytes=" << cached_stats.current_cmm_bytes
              << " cmm_bytes_high_watermark=" << cached_stats.cmm_bytes_high_watermark << '\n';

    if (benchmark_iterations != 0U) {
        const std::array<TestCase, 1> stable_layout{{cases[0]}};
        if (!RunAlternatingBenchmark("stable_layout", *baseline, *cached,
                                     *baseline_output, *cached_output, sources,
                                     stable_layout, benchmark_iterations, benchmark_rounds) ||
            !RunAlternatingBenchmark("mixed_layouts", *baseline, *cached,
                                     *baseline_output, *cached_output, sources,
                                     cases, benchmark_iterations, benchmark_rounds)) {
            std::cerr << "letterbox A/B benchmark failed\n";
            return 9;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cmdline::parser parser;
    parser.set_program_name("ax_letterbox_cache_smoke");
    parser.add<int>("capacity", 'c', "persistent workspace capacity", false, 8);
    parser.add<int>("iterations", 'n', "test sequence repetitions", false, 10);
    parser.add<int>("benchmark-iterations", 'b', "timed repetitions per A/B round; 0 disables", false, 0);
    parser.add<int>("benchmark-rounds", 'r', "alternating A/B rounds", false, 6);
    const auto cli_result = axvsdk::tooling::ParseCommandLine(parser, argc, argv);
    if (cli_result != axvsdk::tooling::CliParseResult::kOk) {
        return axvsdk::tooling::CliParseExitCode(cli_result);
    }
    int capacity = 8;
    int iterations = 10;
    int benchmark_iterations = 0;
    int benchmark_rounds = 6;
    if (!axvsdk::tooling::GetOptionalArgument(parser, "capacity", 0, 8, &capacity, std::cerr) ||
        !axvsdk::tooling::GetOptionalArgument(parser, "iterations", 1, 10, &iterations, std::cerr) ||
        !axvsdk::tooling::GetOptionalArgument(parser, "benchmark-iterations", 2, 0,
                                              &benchmark_iterations, std::cerr) ||
        !axvsdk::tooling::GetOptionalArgument(parser, "benchmark-rounds", 3, 6,
                                              &benchmark_rounds, std::cerr) ||
        capacity <= 0 || iterations <= 0 || benchmark_iterations < 0 || benchmark_rounds <= 0) {
        std::cerr << parser.usage();
        return 2;
    }
    return Run(static_cast<std::size_t>(capacity),
               static_cast<std::size_t>(iterations),
               static_cast<std::size_t>(benchmark_iterations),
               static_cast<std::size_t>(benchmark_rounds));
}
