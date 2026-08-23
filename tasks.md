# tasks.md — 场景编辑器架构 + 渲染并行绑定（双轨并行）

本文件规划两条**可并行推进**的工作流，它们在一个共享接口（`ParserTarget`
事件缝）上汇合：

- **Track A（解析 / 编辑器架构）**：编辑器侧场景图 → 文件 / 实时事件源。
- **Track B（渲染并行处理）**：消费事件、并行实现 GPU 资源（纹理/几何/BVH），
  即把"解析 + 渲染实现"重叠成流水线。

两条轨相互独立推进；Track B 可用现有 `.pbrt` 解析器
（`ParseFiles`→`ParserTarget`）作为事件源先行验证，Track A 可用一个空
`RenderSink` 验证事件产出。最终由 `ParserTarget` 事件协议对接。

## 当前仓库基线

- 启动优化已在 `gpu-build` 分支完成：meshCreate 的 `cudaMallocManaged` 顽疾
  根治、宿主网格+设备镜像、几何管线后台化、`B` 并行创建。
- 纹理上传重叠**已回退**（`99c5236`，顺序、单发 `FlushGPUTextureUploads`，
  崩溃-free，~33.6s）。原因：重叠逻辑有未解决的固有内存损坏（全局"全部完成"
  屏障竞态）。Track B 用**事件级所有权**从根上规避该竞态。
- 事件缝已验证：`src/pbrt/cmd/parse_probe.cpp`（CMake 目标 `pbrt_parseprobe`）
  直接 `ParseFiles` + 自定义 `ParserTarget`，bistro 场景打出 10645 事件，
  `Texture 147 / Shape 1591 / MakeNamedMaterial 132 / NamedMaterial 1591 /
  AreaLightSource 102`。证明解析器可独立、按资产逐个触发事件。

---

## 共享契约：ParserTarget 事件协议（两轨的对接面）

渲染侧 `RenderSink : public ParserTarget` 必须实现的事件，及绑定时机：

| 事件 | 绑定时机 | 说明 |
|---|---|---|
| `Texture` | **立即**（RunAsync 解码+上传） | assetstream 主体；147 个 |
| `Shape` | **立即**（建 mesh + 推进几何集合） | 1591 个 |
| `Material` / `MakeNamedMaterial` | **延迟队列** | 前向引用：Shape 可能先于 NamedMaterial |
| `NamedMaterial` | 延迟解析（绑定到已实现的 Material） | 1591 个引用 |
| `LightSource` / `AreaLightSource` | 延迟（无限光需世界 bounds） | 103 个 |
| `Camera` / `Film` / `Sampler` / `Integrator` | 收集，缺则取基类默认 | 场景级，可省略 |
| `WorldBegin` / `Attribute* ` / `Object*` / `ObjectInstance` | 变换/实例化状态 | 供 Shape 定位 |
| `EndOfFiles` | **屏障**：建 OptiX BVH + 绑定 bounds 相关灯 | 全部 shape 到齐 |

约定：每个资产由**触发它的那一个事件独占拥有**并立即实现 —— 没有全局
"全部上传完成"屏障，从根本上消除此前重叠崩溃的竞态类别。

---

## Track A — 解析 / 编辑器架构（事件源）

目标：编辑器自有中立场景图，可序列化为文件（路径1）或实时提交（路径2），
两者都翻译成 `ParserTarget` 事件。

- [ ] **A1. 场景图基类与默认值模型**
  - 定义 `SceneNode` 基类：`transform`、`name`、可选字段的 `virtual` getter
    返回默认值；具体实体只覆盖自己携带的字段。
  - `Model`（mesh+material+textures）**不含**灯光/相机；`Camera`/`Light`/
    `Film`/`Sampler`/`Integrator` 缺省由基类/场景级默认补齐
    （Perspective@(0,0,5)、默认 InfiniteLight、默认分辨率等）。
  - 验收：一个"只有几何、无灯光相机"的模型文件能被渲染（用默认相机/灯）。
- [ ] **A2. 场景图 → `ParserTarget` 事件适配器**
  - 遍历场景图，发出对应 `ParserTarget` 事件（复用 pbrt 现有接口，
    不直接生成 `.pbrt` 文本）。
  - 验收：适配器输出事件流与 `parse_probe` 同构（事件计数一致）。
- [ ] **A3. 文件序列化（路径1）**
  - 场景图 ↔ 文件（JSON 或二进制）；加载器重放成 `ParserTarget` 事件。
  - 验收：保存→加载→渲染，像素与原始 `.pbrt` 一致。
- [ ] **A4. LiveLink 传输层（路径2）**
  - 本地 socket / 共享内存，把同一套事件（或增量事件）发给渲染器。
  - 验收：编辑器改一个材质 → 渲染器收到增量事件。
- [ ] **A5. 增量更新协议（v2）**
  - `UpdateTexture/UpdateTransform/AddModel/RemoveModel`；渲染侧局部重绑 /
    BVH refit。

## Track B — 渲染并行处理（事件汇 / RenderSink）

目标：消费 `ParserTarget` 事件，在解析进行的同时并行实现 GPU 资源，
渲染实现与解析重叠成流水线；崩溃-free。

- [ ] **B1. `StreamingRenderTarget` 骨架**
  - 实现 `ParserTarget` 全部虚函数；`Texture`/`Shape` 事件入队 `RunAsync`
    实现，`EndOfFiles` 触发 BVH 屏障。先用空绑定跑通 `parse_probe` 事件。
  - 验收：用现有 `.pbrt` 解析器喂入，能走到 `EndOfFiles` 且不崩。
- [ ] **B2. 纹理/几何立即并行实现**
  - `Texture`→解码+上传（复用现有 `DoGPUTextureUpload` 逻辑）；
    `Shape`→建 mesh + 推进几何集合。多事件并发 `RunAsync`。
  - 验收：纹理/几何在解析未结束时已开始实现（重叠）。
- [ ] **B3. 延迟队列（前向引用）**
  - `Material`/`NamedMaterial`/`LightSource` 进延迟队列，`EndOfFiles` 前
    绑定依赖；复用 pbrt 已有 `async*Jobs` 机制。
  - 验收：前向引用的命名材质/纹理正确解析（与顺序基线像素一致）。
- [ ] **B4. assetpack 缓存**
  - 事件先查按 name/hash 索引的预构建 realized-asset 缓存，命中即绑、
    未中流式实现；跨会话复用、秒重载。
- [ ] **B5. 与 integrator 对接 + 崩溃-free 验证**
  - `RenderSink` 产出 `WavefrontPathIntegrator` 可消费的就绪场景；
    多轮渲染 + 像素一致性 + 无 `STATUS_STACK_BUFFER_OVERRUN`。
  - 验收：`render-total` 低于顺序基线（目标 ~25–30s，且零崩溃），
    像素与 `bistro_ref.exr` 一致。

---

## 并行执行说明

- **可立即并行**：A1–A2（事件源）与 B1–B2（事件汇）无相互阻塞 ——
  A 用空 `RenderSink` 验证产出，B 用现有 `.pbrt` 解析器验证消费。
- **对接点**：A2 适配器产出 `ParserTarget` 事件 == B1 `RenderSink` 消费事件。
  接口契约（上表）先冻结，两轨各自向内实现。
- **汇合里程碑**：A3 + B5 完成后，编辑器文件可经事件缝直接渲染；
  A4 + B5 完成后支持实时提交。
- **不在本期**：A5 增量更新（v2）；OptiX 局部 refit 的实时几何编辑。

## 主要风险

- pbrt 解析要求场景**必须有** Camera/Film/Integrator（缺则 `ErrorExit`）
  → 由 A1 基类默认在发事件前补齐，渲染侧不感知。
- 实时几何编辑的 BVH refit 延迟（v1 可接受"几何改→重建 BVH"）。
- Track B 必须保持崩溃-free：坚持"每事件单一所有权 + `EndOfFiles` 屏障"，
  不复活全局"全部完成"屏障（即此前重叠崩溃的根因）。
