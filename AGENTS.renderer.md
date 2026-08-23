# AGENTS.renderer.md — Track B: 渲染并行处理（事件汇 / RenderSink）

Agent 工作指引：**消费 `ParserTarget` 事件、在单一执行器上并行实现 GPU 资源
（纹理/几何/BVH），把"解析 + 渲染实现"重叠成一条流水线。** 本轨与 Track A
（编辑器/解析架构）在 `ParserTarget` 事件协议上对接，详见 `tasks.md`。

> 构建/渲染/验证约定见根目录 `AGENTS.md`（GPU 构建环境、像素一致性验证等）。
> 重叠实验的实证教训见 `benchmark.md` 的 "Attempted and REVERTED" 段。

## ⚠️ 铁律：只有一个执行器（executor）

**所有 CPU 并行工作必须由同一个 `tf::Executor`（或 pbrt 全局线程池）驱动。
绝不另起第二个执行器，也绝不让两个调度器同时抢资源。**

实证依据（见 `benchmark.md`）：
- `RunParallelTasks` 内部 `new tf::Executor` 起了**独立**线程池；当它和 pbrt
  池（也满核）并发时 = 24 线程跑 12 核 → oversubscription → 启动从 ~33s 恶化到
  **42s**。
- 试图把 decode 塞进 pbrt 池（ParallelFor / RunAsync 并发于创建）→ 间歇性
  `STATUS_STACK_BUFFER_OVERRUN`（池在双并发重活下的缺陷）→ 已回退。

正确做法：**单一执行器 + taskgraph 表达依赖**，而不是第二个调度器。

## Taskflow 并行与 taskgraph 用法

把事件流建模成一张 **Taskflow 有向图（taskgraph）**，在唯一执行器上 `run()`：

- **叶节点（立即）**：每个 `Texture` 事件 → `decode + cudaUpload` 任务；
  每个 `Shape` 事件 → `buildMesh + 推进几何集合` 任务。
- **依赖节点（延迟队列）**：`Material` / `LightSource` 任务通过 `.succeed(prereq)`
  连到它引用的 `NamedMaterial` / `Texture` 任务 —— **前向引用天然由图边表达**，
  无需手写的"是否就绪"轮询。
- **屏障节点（barrier）**：一个 `end` 节点，所有 `Shape`/`Light` 任务
  `.precede(end)`；`EndOfFiles` 事件到达后触发 `end` → 在 `end` 的回调里建
  OptiX BVH、绑定依赖世界 bounds 的灯。

```cpp
// THE ONLY executor for this process.
tf::Executor exec;
// ...per event: tf.emplace([]{...}) with .succeed()/.precede() edges...
exec.run(tf).wait();   // single scheduler; ready tasks overlap each other
```

要点：
- 图在事件到达时增量构建，**在 `EndOfFiles` 时一次性 `exec.run(tf)`**（图冻结后
  才 run，符合 Taskflow 约束）。这样 11s 的 decode/upload 在**单一执行器**上
  并行，不再 oversubscribe。
- 若需与解析重叠，分批小图、每批在唯一执行器上 run；**绝不**为此另起执行器。

## 禁止事项（会复活此前崩溃/恶化）

- ❌ 在 pbrt 池活跃时调用 `RunParallelTasks`（它自己 new 执行器）→ oversubscription。
- ❌ 从普通 `std::thread` 调用 `ParallelFor`（破坏池的 thread-index 状态 → 崩溃）。
- ❌ 全局"所有上传完成"轮询/屏障（此前重叠崩溃的根因类别）。改为**每事件单一
  所有权 + `EndOfFiles` 图节点屏障**。
- ❌ 在 `RenderSink` 之外另开线程池跑 decode。

## 验收（每轮改动后必做）

- `render-total` 低于顺序基线（目标 ~25–30s）且**零崩溃**（多跑 ≥3 次）。
- 像素与 `bistro_ref.exr` 一致（`imgtool diff --metric MAE` 静默 exit 0）。
- 事件协议与 `tasks.md` 的契约表一致（Texture/Shape 立即、Material/Light 延迟、
  EndOfFiles 屏障）。
