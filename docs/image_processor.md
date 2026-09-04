# 图像处理 (`ax_image_processor.h`)

头文件:

- `include/common/ax_image_processor.h`

`ImageProcessor` 提供统一的 crop/resize/csc 等图像处理能力，并尽量使用硬件 IVPS 加速。

## 关键类型

- `ImageProcessRequest`
- `CropRect`
- `ResizeOptions`
  - `ResizeMode::kStretch`: 拉伸到目标尺寸
  - `ResizeMode::kKeepAspectRatio`: 保持宽高比(留边填充)
  - `ResizeAlign`: 留边对齐方式

## Process 两种形态

### 1) 返回值版本(库分配输出)

适合更重视易用性、输出不需要复用的场景:

```cpp
auto proc = axvsdk::common::CreateImageProcessor();

axvsdk::common::ImageProcessRequest req{};
req.output_image.format = axvsdk::common::PixelFormat::kBgr24;
req.output_image.width = 640;
req.output_image.height = 640;
req.resize.mode = axvsdk::common::ResizeMode::kKeepAspectRatio;

auto out = proc->Process(*in, req);
```

### 2) 出参版本(调用方复用输出缓冲)

适合强调性能、减少反复申请释放的场景:

```cpp
auto dst = axvsdk::common::AxImage::Create(axvsdk::common::PixelFormat::kBgr24, 640, 640);
bool ok = proc->Process(*in, req, *dst);
```

## 背景色

`ResizeOptions::background_color` 按 `0xRRGGBB` 传入:

- RGB/BGR 输出: 直接使用该颜色
- NV12 输出: 底层会转换成对应 YUV 背景色

## AX650 持久 letterbox 画布

默认工厂及默认选项保持逐帧填充背景的兼容行为。对固定布局的高频模型预处理，
调用方可以显式请求有界持久画布：

```cpp
axvsdk::common::ImageProcessorOptions options{};
options.persistent_letterbox_workspace_capacity = camera_count;
auto proc = axvsdk::common::CreateImageProcessor(options);
```

AX650 只在“保持宽高比、需要缩放且需要颜色转换”的两阶段路径中使用该缓存。
缓存按输入尺寸、crop、输出尺寸、对齐方式和背景色等完整布局区分，容量满时按
LRU 淘汰。布局命中后，NV12 中间画布的 padding 保持首次初始化的背景，当前帧
有效区域仍由 IVPS 完整覆盖。任何 IVPS 失败都会令对应画布失效，下次使用前重新
填充完整背景。

`ImageProcessor::stats()` 返回命中、填充、淘汰、失效和 CMM 高水位等只读累计值。
其他平台当前报告不支持持久画布；调用方不得把选项请求本身当成已经启用的证据。

一个 `ImageProcessor` 实例的 `Process()` 仍要求由调用方串行使用。需要多个 IVPS
worker 时，每个并发 worker 必须拥有独立 processor，不能并发写同一缓存画布。
