# libmpv gpu-next 后端实现方案

## 目标

当前 libmpv 的视频输出路径，本质上等价于 vo=libmpv 配合 gpu render backend。
要实现的目标不是单独发明一套新的 libmpv 渲染器，而是让 vo=libmpv 的行为尽可能等价于 vo=gpu-next。

结论先行：

- 应该参考 video/out/gpu 的分层方式。
- 不应该照搬 video/out/gpu 的具体渲染实现。
- 需要抽取和共享 vo_gpu_next 的 renderer 主体，再为 libmpv 增加一个薄适配层。

## 为什么要参考 gpu

现有 gpu 路径已经证明了一种可合并的模式：

- vo_libmpv 只负责 render API 生命周期和通用同步。
- render backend 只负责参数映射、上下文选择、目标包装、调用共享 renderer。
- API 相关细节通过 libmpv_gpu_context_fns 封装。
- 真正的渲染逻辑集中在 gl_video 这一层，既服务 vo_gpu，也服务 libmpv backend。

对应文件如下：

- video/out/vo_libmpv.c：render API 驱动层。
- video/out/gpu/libmpv_gpu.c：薄 render backend。
- video/out/gpu/libmpv_gpu.h：API context 适配接口。
- video/out/gpu/context.h：ra_ctx、swapchain、FBO 抽象。
- video/out/gpu/video.h：共享 renderer 对外接口。

这条路径的价值不在于 gl_video 本身，而在于它的职责划分是清晰的。

## 为什么不能直接照搬 gpu 的渲染实现

gpu 和 gpu-next 的渲染核心不同：

- gpu 依赖 gl_video 和传统 ra 路径。
- gpu-next 依赖 libplacebo、pl_renderer、pl_queue、以及更深的 hwdec 和 colorspace 集成。

因此不能把 video/out/gpu/video.c 的算法直接复用到 gpu-next。
真正应该复用的是结构：

- 薄 backend
- 共享 renderer core
- API 特定 glue 层
- 与窗口和 swapchain 交互解耦

## 当前实现现状

现有 vo_gpu_next 已经不是一个完全扁平的实现，它已经有自己的分层：

- video/out/gpu_next/context.c：负责 gpu_ctx 创建、销毁，以及把不同 API 下的 libplacebo GPU 和 swapchain 接起来。
- video/out/vo_gpu_next.c：包含 queue、map/unmap、OSD、hwdec、ICC、screenshot、perfdata、控制逻辑等 renderer 主体。
- video/out/placebo/ra_pl.c：已经存在把 pl_gpu 接入现有 RA 体系的桥接层。

这意味着：

- 参考 gpu 是对的。
- 但参考点应该是分层和边界，而不是复制旧 gpu 的具体代码。
- 也不应该新造一套平行的 gpu_next/ra.c，如果现有 placebo/ra_pl 和 gpu_next/context 已经足够承载目标。

## 对早期补丁的判断

早期补丁的参考价值主要在于验证功能需求和接口落点，而不是最终结构。

它说明了三件事：

- vo_libmpv 需要一个明确的 backend 选择机制，例如 gpu 和 gpu-next。
- libmpv 下确实需要 API 专用 context glue，例如 OpenGL FBO 包装和 done_frame 处理。
- 直接在 libmpv 路径里重写一套 gpu-next renderer，会导致大量与 vo_gpu_next 的重复实现和行为分叉。

因此，这个补丁更适合用来确认最小能力集合，而不适合作为最终合并版本的结构模板。

## 推荐的目标架构

### 总体分层

推荐结构如下：

1. vo_libmpv
2. render_backend_gpu_next
3. libmpv_gpu_next_context
4. gpu-next shared renderer core
5. gpu_next/context 和底层 RA 或 placebo bridge

各层职责如下。

### 1. vo_libmpv

职责：

- 选择 render backend。
- 维护 render API 生命周期。
- 处理帧同步、更新回调和通用 VO 框架行为。

要求：

- 与现有 gpu backend 的接入方式保持一致。
- 仅新增 backend 选择，不新增 renderer 逻辑。

### 2. render_backend_gpu_next

职责：

- 结构上对齐 video/out/gpu/libmpv_gpu.c。
- init 阶段创建 API context 和共享 renderer。
- render 阶段包装目标表面后，调用共享 renderer。
- reconfig、resize、update_external、reset、screenshot、perfdata、get_image 仅做转发。

这一层应当尽量薄，避免出现第二套 map_frame、OSD、截图、格式转换实现。

### 3. libmpv_gpu_next_context

职责：

- 结构上对齐 video/out/gpu/libmpv_gpu.h。
- 只处理 render API 和底层图形 API 交界面的事情。
- 例如：OpenGL 初始化参数、FBO 包装、frame 完成通知、必要的 current context 约束。

这一层不应持有 renderer 主逻辑。

### 4. gpu-next shared renderer core

这是整个方案的关键。

应该从现有 vo_gpu_next 中抽取一个共享 renderer core，供两个上层复用：

- vo_gpu_next
- render_backend_gpu_next

这一层应包含以下纯渲染逻辑：

- 帧队列和插帧相关逻辑。
- map_frame、unmap_frame、discard_frame。
- 软件上传和硬件帧映射。
- OSD 和 overlay 组装。
- format check。
- screenshot。
- perfdata。
- DR image 分配和回收。
- 目标色彩空间、tone mapping、libplacebo render params 组装。
- 与 renderer 行为直接相关的 option 应用。

这层可以继续命名为 gpu_next/video.c 或拆成更明确的 core 文件，但原则是必须共享，而不是 libmpv 自己再实现一份。

### 5. gpu_next/context 和 placebo bridge

职责：

- 管理底层 GPU、swapchain、以及 API backend 资源。
- 继续承载 Vulkan、D3D11、OpenGL 等实际图形 API 差异。
- 优先复用现有 video/out/gpu_next/context.c 和 video/out/placebo/ra_pl.c。

设计原则：

- 如果只是缺少少量包装能力，应扩展现有 bridge。
- 不要引入与现有 RA 体系平行的新抽象，除非现有层级无法表达目标能力。

## 建议的代码边界

### 应参考 gpu 的部分

- video/out/gpu/libmpv_gpu.c 的 backend 形状。
- video/out/gpu/libmpv_gpu.h 的 API context vtable 形状。
- video/out/gpu/context.h 的 RA、swapchain、target FBO 边界。
- video/out/gpu/video.h 的共享 renderer 接口设计方式。

### 不应直接复制的部分

- video/out/gpu/video.c 的具体 shader 或渲染实现。
- 任何与 gl_video 强绑定的内部数据结构。

### 应从 vo_gpu_next 抽取共享的部分

- queue push 和 frame 生命周期管理。
- map_frame、unmap_frame、discard_frame。
- plane 和 format 描述转换。
- update_overlays。
- screenshot 流程。
- perfdata 统计。
- get_image 和 DR buffer 管理。
- hwdec acquire 或 release 映射逻辑。
- 与 libplacebo render params 直接相关的逻辑。

### 应保留在 vo_gpu_next 壳层中的部分

- flip_page。
- wait_events、wakeup。
- 与窗口事件直接关联的 control 分支。
- VO 外部 resize 事件接入。
- 平台相关 swapchain 交互。

## 推荐的实施顺序

### 第一阶段：只重构 vo_gpu_next 内部结构

目标：零行为变化。

做法：

- 从 vo_gpu_next.c 中抽取共享 renderer core。
- 让 vo_gpu_next 自己先改为调用共享 core。
- 不引入新的 libmpv backend。

这一阶段完成后，应保证 vo=gpu-next 的行为和当前实现一致。

### 第二阶段：引入 libmpv_gpu_next_context

目标：补齐 API glue，但不复制 renderer。

做法：

- 参照 gpu/libmpv_gpu.h 新建 gpu-next 版本接口。
- 先支持最小可用的 API 类型。
- 处理 wrap_fbo、done_frame、必要的上下文约束。

### 第三阶段：引入 render_backend_gpu_next

目标：让 vo_libmpv 能走 gpu-next 共享 core。

做法：

- backend 形状对齐 gpu/libmpv_gpu.c。
- init 时只创建 context 和共享 core。
- render 时只包装目标并调用共享 core。
- 其他接口尽量转发。

### 第四阶段：补齐行为一致性

目标：确保 libmpv 路径和 vo=gpu-next 在关键行为上不分叉。

重点补齐：

- hwdec。
- ICC profile。
- screenshot。
- perfdata。
- DR image。
- option 更新。
- OSD 和插帧。

## 原理说明

这套方案的核心原理，是把“渲染算法”和“承载环境”拆开。

渲染算法相关：

- 帧怎么入队。
- 软件帧和硬件帧如何映射到 libplacebo。
- OSD 如何上传和合成。
- 最终 render params 如何拼装。

承载环境相关：

- 目标表面从哪里来。
- 谁负责 current context。
- 谁负责 swapchain 和 present。
- 谁提供窗口事件和 resize。

vo=gpu 和 vo=libmpv+gpu 之所以能共存，就是因为 gl_video 被放在中间，成为共享 renderer core。
gpu-next 要可合并，也必须走同样的思路：

- vo_gpu_next 只是一个外壳。
- libmpv backend 也只是一个外壳。
- 中间是唯一一份共享的 gpu-next renderer core。

## 当前基于 gpu 的 libmpv backend 已实现的能力

当前基于 gpu 的 vo=libmpv 路径，能力并不只是能把画面画出来，而是已经继承了 gl_video 的大部分 renderer 能力。

可以确认的能力包括：

- 共享渲染核心。libmpv backend 直接创建 gl_video，并复用其完整渲染逻辑。
- 完整的 HDR 和 tone-mapping 选项链路。tone-mapping、target-peak、HDR peak detect、gamut mapping、interpolation 等都来自 gl_video_conf。
- OSD 和字幕叠加。
- screenshot。
- perfdata。
- get_image 和 DR 路径。
- hwdec 设备创建和加载。
- ICC profile 与 ambient light 相关能力。
- resize、reset、update_external、queue 配置。

其中最关键的一点是：

tone-mapping 并不是通过 render API set_parameter 注入的，而是共享 renderer 自己通过配置缓存读取全局选项。

现有 gpu 路径大致是这样工作的：

- render_backend_gpu 在 init 阶段创建 gl_video。
- gl_video 在内部持有 gl_video_conf 的配置缓存。
- vo_libmpv 收到 VOCTRL_UPDATE_RENDER_OPTS 后，只是设置 need_update_external。
- backend 的 update_external 会调用 gl_video_configure_queue。
- gl_video_configure_queue 会先刷新配置缓存，再应用新选项。

也就是说，现有 gpu backend 的 option 更新能力来自共享 renderer，而不是来自 libmpv backend 自己硬编码处理这些选项。

## 对 patch 当前两个症状的判断

### 1. 无法更改 tone-mapping

这个问题大概率不是前端调用方式导致的，而是 patch 的共享 renderer 链路没有建立起来。

从早期补丁的结构看，问题至少有三层。

第一，补丁里的 pl_video_init 没有建立和现有 vo_gpu_next 等价的 option cache 体系。

- 没有看到对 gl_video_conf 的 m_config_cache_alloc。
- 没有看到对 gl_next_conf 的 m_config_cache_alloc。
- 没有看到 update_options 或 update_render_options 这一整套逻辑。

第二，补丁里的渲染参数是硬编码的简化版。

- upscaler 和 downscaler 被固定为 nearest。
- 只填了 brightness、contrast、hue、saturation、gamma 这类基础 color adjustment。
- 没有把 tone_mapping_function、tone_mapping_param、target_peak、gamut_mapping、peak_detect_params 等参数接入 render path。

第三，libmpv backend 的 update_external 只更新了 OSD，没有建立 renderer option 刷新链路。

因此，现有 patch 出现“改 tone-mapping 没反应”，是符合代码结构的。

更准确地说：

- 当前 gpu backend 之所以能改 tone-mapping，是因为它复用了 gl_video 的完整 options 体系。
- 当前 patch 之所以不能改 tone-mapping，是因为它把 renderer 简化成了一个只会画图的最小实现，没有把 vo_gpu_next 的 options 路径一起搬进去。

### 2. 画面很卡

这个问题大概率来自时序和队列逻辑被过度简化，而不是单点性能问题。

现有 vo_gpu_next 的 draw_frame 会认真消费 vo_frame 里的时序信息，包括：

- display_synced。
- num_vsyncs。
- num_frames。
- still。
- redraw。
- approx_duration。
- ideal_frame_vsync。
- ideal_frame_vsync_duration。

并据此决定：

- 是否启用 interpolation。
- push 到 pl_queue 的 duration。
- queue update 时的 pts、radius、vsync_duration、interpolation_threshold。
- 是否复用 mixing cache。
- 是否在非单调 PTS 时强制 reset queue。

而早期补丁里的 pl_video_render 明显把这套机制简化掉了：

- 只按 frame_id 推入单帧，没有对齐 vo_gpu_next 的多帧 push 路径。
- push 时没有使用 approx_duration。
- queue_update 时只给了最小参数，没有理想 vsync 持续时间。
- 直接把 vsync_duration 硬编码为 1.0。
- 没有消费 interpolation_threshold。
- 没有基于 display_synced 和 num_vsyncs 建立 redraw 或 cache 策略。
- 没有 vo_gpu_next 那套 queue reset 和非单调 PTS 修正逻辑。

这类差异会直接导致：

- 帧队列欠供给。
- mixing 和 cache 行为失真。
- redraw 节奏异常。
- 插帧与 display-sync 配合错误。
- 最终表现为卡顿、掉帧感、节奏不稳。

所以“画面很卡”更像是时序语义丢失，而不是单纯渲染慢。

## 对这两个问题的直接结论

如果只看现象和当前实现，对应关系基本可以定为：

- tone-mapping 不可变：共享 renderer 没有接入完整 options 更新链路。
- 画面很卡：共享 renderer 没有接住 vo_frame 的完整时序和 queue 语义。

这两个问题都进一步说明，同一个结论是对的：

- 不能在 libmpv 路径里写一个简化版 gpu-next renderer。
- 必须复用 vo_gpu_next 已有的 renderer 语义。
- libmpv backend 应该像 gpu backend 一样，成为共享 renderer 的薄外壳。

## 文件级重构蓝图

这一节给出可直接执行的文件级方案，目标是复刻当前 gpu 路径在 libmpv 下已有的功能覆盖，同时符合仓库现有分层和代码风格。

总体原则：

- 优先抽取共享逻辑，不复制现有逻辑。
- 优先复用已有抽象，不平行再造一套。
- 先让 vo_gpu_next 自己吃到重构结果，再接 libmpv。
- 每个阶段都应保持最小可验证修改。

### 一、建议新增的文件

#### 1. video/out/gpu_next/video.h

职责：

- 对外暴露共享 gpu-next renderer core 的接口。
- 角色上对齐 video/out/gpu/video.h。

建议承载的接口：

- init 和 uninit。
- set_osd_source。
- configure_queue 或 update_external。
- check_format。
- reconfig。
- resize。
- render_frame。
- reset。
- screenshot。
- perfdata。
- get_image。
- init_hwdecs 和 load_hwdecs。

设计要求：

- 接口风格应接近 gl_video，而不是暴露 libmpv 专用语义。
- 参数里可以接收 vo、ra_ctx、gpu_ctx、hwdec_devs 等现有对象，但不应绑死 libmpv。

#### 2. video/out/gpu_next/video.c

职责：

- 承载唯一一份共享的 gpu-next renderer 实现。
- 由当前 vo_gpu_next.c 中抽取纯 renderer 逻辑而来。

建议迁入的逻辑：

- priv 中和 renderer 直接相关的字段。
- option cache 和 update_options。
- update_render_options。
- format_supported 和 plane_data_from_imgfmt。
- map_frame、unmap_frame、discard_frame。
- hwdec acquire 或 release 相关逻辑。
- DR buffer 管理。
- OSD update_overlays。
- queue push、queue update、mix 生成。
- target colorspace 和 tone-mapping params 组装。
- screenshot。
- perfdata。
- get_image。

设计要求：

- 不能退化成早期 patch 里的简化版 pl_video_render。
- 必须完整保留 vo_gpu_next 现有的 queue、timing、option、ICC、hwdec 语义。

#### 3. video/out/gpu_next/libmpv_gpu_next.h

职责：

- 对齐 video/out/gpu/libmpv_gpu.h。
- 定义 libmpv 与 gpu-next API context 之间的 glue interface。

建议接口：

- api_name。
- init。
- wrap_fbo。
- done_frame。
- destroy。

设计要求：

- 只承载 render API 和底层图形 API 的交界逻辑。
- 不承载 renderer options、queue、OSD、hwdec 等主体逻辑。

补充说明：

- 现有 gpu 路径中的 OpenGL libmpv glue 实际落在 video/out/opengl/libmpv_gl.c。
- 因此 gpu-next 的 API glue 也应优先放在对应的 API 层，或者至少保持同样的职责边界。
- 如果短期内为了减少文件数而暂放在 gpu_next/context.c，也应把接口设计成之后可平移到 API 层，而不是把 renderer 逻辑混进去。

#### 4. video/out/gpu_next/libmpv_gpu_next.c

职责：

- 对齐 video/out/gpu/libmpv_gpu.c。
- 作为薄 render backend，调用共享 gpu-next renderer core。

建议流程：

- init：创建 libmpv_gpu_next_context，创建共享 renderer，初始化 hwdec 设备。
- check_format：转发给共享 renderer。
- reconfig：转发给共享 renderer。
- update_external：更新 OSD 源并刷新 queue 和 option。
- resize：转发给共享 renderer。
- render：wrap_fbo 后调用共享 renderer。
- get_image：转发给共享 renderer。
- screenshot：转发给共享 renderer。
- perfdata：转发给共享 renderer。
- reset：转发给共享 renderer。
- destroy：销毁共享 renderer、hwdec 设备和 context。

设计要求：

- 尽量维持和 render_backend_gpu 相似的函数骨架。
- 不在这里写第二份 map_frame、OSD、tone-mapping 或 queue 逻辑。

### 二、建议重点修改的现有文件

#### 5. video/out/vo_gpu_next.c

职责调整：

- 从“既是 VO 外壳，又是 renderer 主体”改成“以 VO 外壳为主，调用共享 renderer”。

建议保留在本文件中的逻辑：

- preinit 中的 VO 级资源准备。
- flip_page。
- wakeup。
- wait_events。
- 与 ra_ctx control 直接耦合的 control 分支。
- resize 事件入口。
- vo_driver 注册表。

建议迁出的逻辑：

- 大多数纯 renderer 逻辑都迁到 gpu_next/video.c。

设计要求：

- 第一阶段修改完后，功能行为应保持不变。
- 如果某块逻辑既依赖 vo 又依赖 renderer，优先把 renderer 语义下沉，只在壳层保留事件入口。

#### 6. video/out/gpu_next/context.h

职责：

- 保持现有 gpu_ctx 抽象，不扩大成 renderer 抽象。

建议修改：

- 如果共享 renderer 确实需要少量通用 helper，可以在这里增加最小接口。
- 不要把 options、queue、OSD、截图等 renderer 能力塞回 context。

#### 7. video/out/gpu_next/context.c

职责：

- 保持图形 API、pl_gpu、swapchain、ra_ctx 生命周期管理。

建议修改：

- 为 libmpv_gpu_next_context 复用现有初始化路径提供 helper，如果确实需要。
- 把 current context、swapchain、done_frame 相关的平台差异留在这里或其下游 API 适配层。

设计要求：

- 仍然只做 context 和底层资源管理。
- 不承接 renderer 功能扩散。

补充说明：

- 参考现有 video/out/opengl/libmpv_gl.c 的目录层次，libmpv 专用的 API glue 最终更适合放在具体 API 层，而不是全部堆进 gpu_next/context.c。
- gpu_next/context.c 更适合作为共享的 gpu_ctx 资源管理层。

#### 8. video/out/vo_libmpv.c

职责：

- 保持 render API 总控角色。

建议修改：

- 增加 backend 选择逻辑。
- 保持现有 need_resize、need_update_external、need_reset、need_reconfig 调度流程不变。

设计要求：

- 除 backend 路由外，不要把 gpu-next 特有逻辑塞进 vo_libmpv.c。

#### 9. video/out/libmpv.h

职责：

- 继续作为 render backend 公共接口头。

建议修改：

- 新增 render_backend_gpu_next 声明。
- 如无必要，不扩 render_backend_fns。

设计要求：

- 优先通过共享 renderer 吸收差异，避免增加新的 backend entrypoint。

### 三、建议尽量不动或只做小改的文件

#### 10. video/out/placebo/ra_pl.c 与 video/out/placebo/ra_pl.h

建议：

- 优先复用现有 pl_gpu 到 RA 的桥接。
- 如果共享 renderer 或 libmpv glue 缺少少量 helper，在这里补最小能力。

不建议：

- 新造一套与之平行的 gpu_next/ra.c 抽象，并长期共存。

#### 11. video/out/gpu/context.h 与 video/out/gpu/libmpv_gpu.h

建议：

- 作为结构模板参考。
- 接口命名、职责划分和生命周期模式尽量保持同风格。

不建议：

- 把 gpu 的 gl_video 实现细节复制到 gpu-next。

### 四、公开 API 和构建文件

#### 12. include/mpv/render.h

建议：

- 如果要公开 backend 选择能力，可以保留一个最小的新参数，例如 backend string。
- 但这部分是 public API，需要单独审视兼容性和命名。

设计要求：

- API 面尽量小。
- 不要把实现阶段性的内部概念暴露成长期 API。

#### 13. meson.build

建议：

- 只在共享 renderer 和 libmpv backend 文件真正稳定后再接入构建。
- 第一阶段如果只是抽取 vo_gpu_next 内部逻辑，也可以先只新增 shared core 文件。

### 五、按阶段的文件修改顺序

#### 阶段 1：抽共享 core，不改 libmpv 行为

修改文件：

- video/out/vo_gpu_next.c
- video/out/gpu_next/video.h
- video/out/gpu_next/video.c
- 如有必要，少量修改 video/out/gpu_next/context.h
- 如有必要，少量修改 video/out/gpu_next/context.c

验收目标：

- vo=gpu-next 行为不变。
- tone-mapping、ICC、screenshot、OSD、hwdec、插帧、perfdata 都继续工作。

#### 阶段 2：接入 libmpv 专用 context glue

修改文件：

- video/out/gpu_next/libmpv_gpu_next.h
- video/out/gpu_next/libmpv_gpu_next.c
- 可能少量修改 video/out/gpu_next/context.c

验收目标：

- backend 变成薄封装。
- context 负责 API 交界面。
- 共享 renderer 不知道自己是被 VO 还是 libmpv 调用。

#### 阶段 3：接入 vo_libmpv backend 路由

修改文件：

- video/out/vo_libmpv.c
- video/out/libmpv.h
- include/mpv/render.h
- meson.build

验收目标：

- vo=libmpv 可以稳定选择 gpu-next backend。
- render API 生命周期和现有 gpu backend 风格一致。

#### 阶段 4：补齐对标 gpu backend 的功能覆盖

逐项确认：

- tone-mapping 动态生效。
- target-peak、生色域、HDR peak detect 生效。
- screenshot 正常。
- get_image 和 DR 正常。
- perfdata 正常。
- hwdec 正常。
- OSD 和字幕正常。
- display-sync 或 interpolation 下节奏稳定。

### 六、明确不建议的实现方式

以下实现方式不符合当前仓库的开发方向：

- 在 libmpv backend 下复制一份 vo_gpu_next 的 map_frame、OSD、screenshot、tone-mapping 逻辑。
- 为了尽快跑通，写一个只支持最小渲染的简化版 gpu-next core，然后长期保留。
- 新引入一套平行于现有 RA 和 placebo bridge 的中间层，并让两套抽象长期共存。
- 把 gpu-next 特有的 renderer 细节塞进 vo_libmpv.c。
- 先扩 public API，再倒逼内部实现去适配。

### 七、与仓库开发规范的一致性

这套文件级方案符合当前仓库代码风格的原因如下：

- 与现有 gpu 路径的 backend 分层一致。
- 优先在已有抽象边界内复用，而不是引入平行结构。
- 通过共享 renderer 修复根因，而不是在 libmpv 路径里做表面补丁。
- 每个阶段都可以单独验证，降低回归面。
- 保持 public API 增量最小，减少后续维护成本。

## 主要风险

### 1. OpenGL current context 约束

libmpv 路径通常没有窗口系统帮它兜底，需要在 API glue 层严格定义：

- 何时要求调用者已绑定 context。
- wrap_fbo 和 render 之间的上下文有效期。
- done_frame 是否需要承担释放或收尾职责。

这个风险应放在 libmpv_gpu_next_context 中解决，而不是放进共享 renderer。

### 2. libplacebo 和 pl_gpu 生命周期

共享 core 不能私自拥有和重建底层 GPU 世界。
底层对象的 owner 应保持清晰：

- context 层负责 pl_gpu 或 swapchain 来源。
- renderer core 只消费这些对象。

### 3. hwdec 路径

这是最容易在 libmpv 里失真的部分。

必须保证：

- RA 和 placebo bridge 的 hwdec 映射能力可复用。
- libmpv backend 不要简化成只支持软件上传。
- 与 vo_gpu_next 走同一套 mapper 语义。

### 4. OSD 和 screenshot

这两块在早期补丁里最容易被复制一份，但它们本质上属于 renderer core。
如果这里分叉，后续维护成本会持续上升。

### 5. option 更新和 update_external

共享 core 必须有明确的外部状态入口：

- OSD 来源更新。
- queue 配置更新。
- renderer option 更新。
- profile 或 LUT 相关变更。

否则 vo 和 libmpv 会出现一边更新、一边遗忘的行为分叉。

## 验证策略

实现完成后，至少应从以下维度验证。

### 行为对齐

- vo=gpu-next 与 vo=libmpv + gpu-next 的输出一致性。
- resize、pause、redraw、still frame 行为一致。
- screenshot 输出一致。
- OSD、subtitle、blend 行为一致。

### 功能覆盖

- 软件上传。
- hwdec。
- ICC profile。
- interpolation。
- DR image。
- 性能统计。

### API 约束

- OpenGL FBO 包装。
- flip 和 depth 参数。
- current context 要求是否被正确满足。

## 最终建议

如果只保留一句实现建议，那就是：

先把 vo_gpu_next 重构成像 gpu 一样的共享 renderer 结构，再让 libmpv 去调用这份共享 renderer。

也就是说：

- 参考 gpu 的结构。
- 复用 gpu-next 的算法。
- 避免 libmpv 再实现第二份 gpu-next。

## 当前实现结论

基于目前的代码状态，可以给出一个明确判断：libmpv 的 gpu-next 后端已经进入“可用且接近 vo=gpu-next”的阶段，但还不能直接视为与 vo=gpu-next 完全等价。

### 与 vo=gpu-next 的一致性

- 核心渲染链路已经对齐到共享的 gpu-next renderer。
- OSD、screenshot、perfdata、hwdec、queue、ICC、LUT 和 tone-mapping 的主路径都已经接入。
- vo 路径和 libmpv 路径已经不再是两套完全独立的 renderer。

### 仍然存在的差异

- libmpv 需要额外处理 render API 的目标包装和 context 生命周期，这部分天然不会和 vo 完全一样。
- 部分外部参数协商仍然依赖具体 API backend 的能力，例如 target color hint、FBO 包装、depth、flip 和 current context 约束。
- libmpv backend 目前更偏“共享 renderer 的驱动层”，而不是独立的完整 VO。

### 相比 libmpv gpu 的优势

- 能进入 gpu-next 的 libplacebo 渲染链路，而不是停留在旧 gpu 路径。
- 更容易获得与 vo=gpu-next 一致的 tone-mapping、色彩管理和 OSD 合成行为。
- 后续扩展和修复可以直接沿用 vo=gpu-next 的共享 core，不需要再维护第二套逻辑。

### 结论

如果目标是“在 libmpv 中实现 gpu-next 后端”，当前方案已经满足方向要求；
如果目标是“让 libmpv gpu-next 与 vo=gpu-next 在所有细节上完全一致”，还需要继续收紧 render API 参数协商和 backend glue 的边界。
