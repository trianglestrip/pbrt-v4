# AGENTS.editor.md — Track A: 解析 / 编辑器架构（事件源）

Agent 工作指引：**实现编辑器侧中立场景图（含基类默认值），并把它翻译成
`ParserTarget` 事件流**——既序列化到文件（路径1），也实时提交给渲染器（路径2）。
本轨是 Track B（渲染并行处理）的**事件源**，对接面见 `tasks.md` 的
`ParserTarget` 事件协议。

> 构建/渲染/验证约定见根目录 `AGENTS.md`；双轨并行规划与事件契约见 `tasks.md`；
> 渲染侧单执行器铁律见 `AGENTS.renderer.md`（本轨产出的事件最终由那条铁律约束）。

## 场景图模型（"架构"层）

- 定义中立的 `SceneNode` 基类，持有 `transform` / `name` 与**可选字段的
  `virtual` getter（返回默认值）**；具体实体只覆盖自己真正携带的字段。
- 派生实体：`Model`(mesh+material+textures)、`Light`、`Camera`、`Film`、
  `Sampler`、`Integrator`、`World`(聚合 Models+Lights)。
- **关键点**：`Model` 本身**不含**灯光/相机 —— 这些属于 `World`/场景级。未提供时
  由基类/场景级默认补齐：`Camera`→`Perspective`@(0,0,5)；`Light`→默认
  `InfiniteLight`；`Film`→默认分辨率；`Sampler`/`Integrator` 同理。
  验收：一个"只有几何、无灯光相机"的模型文件也能渲（用默认相机/灯）。

## 事件协议（与渲染侧对齐，不可擅自改）

- 适配器把场景图遍历成 `ParserTarget` 事件（`Texture`/`Shape`/`Material`/
  `LightSource`/`Camera`/.../`EndOfFiles`），**不要直接生成 `.pbrt` 文本**。
- 事件计数应与 `src/pbrt/cmd/parse_probe.cpp`（`pbrt_parseprobe` 目标）打出的同构
  （bistro: Texture 147 / Shape 1591 / MakeNamedMaterial 132 / NamedMaterial 1591 /
  AreaLightSource 102）。
- 前向引用：`Shape` 的 `NamedMaterial` 可能先于其 `MakeNamedMaterial` 出现 —— 事件
  顺序如实反映即可，延迟解析由 Track B 的 taskgraph 边处理，本轨不预判。

## ⚠️ 执行器约定（避免与渲染侧抢资源）

- 编辑器侧如有自身重活（如纹理预处理），只可用**自己的单个**执行器，且**绝不**与
  渲染侧（Track B 的 `tf::Executor`）共享或并发抢核。
- 但**默认把重 GPU 工作交给 Track B**：编辑器只产出事件，不自己解码/上传纹理。
- 不要在编辑器里 new 第二个全局线程池去和渲染器竞争 —— 见 `AGENTS.renderer.md`
  的 oversubscription 教训（24 线程跑 12 核 → 启动恶化）。

## 验收（每轮改动后必做）

- A2 适配器产出事件流与 `parse_probe` 同构（事件计数一致）。
- A3 文件保存→加载→渲染，像素与原始 `.pbrt` 一致（`imgtool diff` 静默 exit 0）。
- 基类默认值：缺相机/灯的场景图可被渲染（不 `ErrorExit`）。
