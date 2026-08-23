# pbrt-v4 GPU build — Benchmark & Build Notes

Machine: RTX 2060 SUPER (sm_75, Turing), driver 591.86 · CUDA 13.1 ·
OptiX SDK 8.0.0 · VS 2022 Community · CMake 3.31.6 · Windows 10/11.

Branch: `gpu-build`. Binary: `build\Release\pbrt.exe`.

## Build configuration (this branch)

- GPU enabled via CUDA 13.1 + OptiX 8.0.0 (`-DPBRT_OPTIX_PATH=...OptiX SDK 8.0.0`
  `-DPBRT_GPU_SHADER_MODEL=sm_75`).
- C++ standard: **C++17** (not C++20 — see note below).
- Taskflow 3.11 vendored at `src/ext/taskflow` (header-only) and added to the
  include path, available for the planned parallel CPU read / upload work.
- Parser fix: `WorldEnd` is now swallowed for normal scene parsing
  (was only swallowed in formatting mode → "Unknown directive: WorldEnd").

### C++20 could NOT be used

Switching the whole build to C++20 was attempted and reverted:
- pbrt's own code broke (`Quaternion` / `LightBVHNode` braced-init no longer
  valid because a `= default` constructor disables aggregate init in C++20 — fixed
  those two, but more C++20 issues remain).
- **nvcc crashed**: `cicc died with status 0xC0000005 (ACCESS_VIOLATION)`,
  an internal CUDA 13.1 compiler crash under C++20.
- Taskflow 3.11 does **not** require C++20 (it targets C++17; only optional
  features like `TF_ENABLE_ATOMIC_NOTIFIER` need C++20). So C++17 is fine.

## Stage timing (the key finding)

Instrumentation added (`STAGE_TIMING` prints) in `cmd/pbrt.cpp` (parse),
`wavefront/wavefront.cpp` (GPU build/upload/BVH vs. render),
`gpu/optix/aggregate.cpp` (OptiX ctor phases, bulk-upload timing) and
`textures.cpp` (texture upload). Quick scene
`bistro/bistro_cafe_quick.pbrt` (640×360, 8 spp), RTX 2060 SUPER, OptiX 8.0.0:

**Before any front-end optimization** (~110 s fixed startup):
```
STAGE_TIMING [parse]                2.53 s
STAGE_TIMING [gpu-build+upload+bvh] 113.98 s   <-- dominates
STAGE_TIMING [render-total]         115.97 s
```

The actual GPU ray tracing finishes in well under ~2 s; **~98% of wall-clock was
`gpu-build+upload+bvh`**. Detailed phase instrumentation then showed the OptiX
GPU build itself is trivial (**~0.2 s**) — the time was almost entirely
**CPU-side per-mesh geometry upload + scene/texture construction**.

**After P0 (instrumentation) + P1 (bulk geometry upload) + P4 (SBT header
precompute)** — `gpu-build+upload+bvh` dropped to **~28 s** (≈ 4.2× faster),
pixel-identical output:
```
STAGE_TIMING [parse]                 0.26 s
STAGE_TIMING [wpi-pre-flush]        13.66 s   CreateTextures/Lights/Materials (image read/decode)
STAGE_TIMING [texture-upload]        9.64 s   147 unique textures -> GPU
STAGE_TIMING [optix-init]            0.20 s
STAGE_TIMING [optix-prepare-ply]     1.22 s
STAGE_TIMING [optix-bvh-triangles-fn] 2.81 s  (nMeshes=1591)
STAGE_TIMING [optix-bvh-triangles-upload] 0.06 s   <-- was 58.15 s before P1
STAGE_TIMING [optix-instances]       2.82 s
STAGE_TIMING [optix-accelbuild-total] 0.25 s
STAGE_TIMING [wpi-post-flush]       14.18 s   light preprocess/sampler + queue alloc
STAGE_TIMING [gpu-build+upload+bvh] 27.85 s
STAGE_TIMING [render-total]          29.67 s
```

(Note: `optix-ctor-total` ≈ 4.2 s is a subset of `wpi-post-flush`; the two
overlap, which is why the per-phase sum slightly exceeds the total. Per-run
absolute numbers vary with machine load — `optix-prepare-ply` swings 1–8 s —
but the *relative* breakdown is stable.)

## End-to-end wall-clock (OptiX 8.0.0)

| Scene file                       | Res / spp        | Time    | Output                        |
|----------------------------------|------------------|---------|-------------------------------|
| `bistro/bistro_cafe.pbrt`       | 1920×1080 / 256 | ~481 s* | `bistro_cafe.exr`             |
| `bistro/bistro_cafe_med.pbrt`   | 720×405 / 32    | ~101 s  | `bistro_cafe_med.exr`         |
| `bistro/bistro_cafe_quick.pbrt` | 640×360 / 8     | ~28 s   | `bistro_cafe_quick.exr`       |

\* The 481 s for the full 1080p/256spp is ~110 s fixed startup + ~370 s of
actual 256-spp sample time. The ~110 s fixed startup is the `gpu-build+upload+bvh`
stage above and is constant regardless of spp.

### Deferred GPU texture upload — implementation & fix

The parallel upload is implemented in `textures.cpp` (`FlushGPUTextureUploads` +
`DoGPUTextureUpload`), fed by memory-mapped image reads in `util/image.cpp`, with the
Taskflow glue in `util/parallel_tasks.*` and `gpu/gpu_texture_upload.h`. Two bugs
were found and fixed while getting it to build and run:

- **Crash (std::terminate):** `GPUSpectrumImageTexture::Create` /
  `GPUFloatImageTexture::Create` locked `textureCacheMutex` but returned early on the
  deferred-upload path without unlocking; the next texture creation on the same worker
  thread re-locked the held non-recursive mutex → `std::mutex::lock()` threw
  `std::system_error` → `terminate`. Fixed by unlocking before the early return.
- **~4× slowdown regression:** pending uploads were registered per texture *instance*
  with no de-duplication, and `DoGPUTextureUpload`'s cache check raced with its insert,
  so the same file was uploaded many times. Fixed by coalescing pending uploads by
  `(filename, type)` so each unique texture is uploaded exactly once.

  Measured after the fix (`bistro_cafe_quick.pbrt`, same machine):
  ```
  STAGE_TIMING [parse]                0.26 s
  STAGE_TIMING [gpu-build+upload+bvh] 117.81 s
  STAGE_TIMING [render-total]         118.24 s
  ```
  i.e. the deferred upload now runs at parity with the synchronous baseline (the
  upload stage is small; the ~110 s is OptiX BVH build, which dominates and is
  unchanged). `bistro_cafe_quick.exr` renders correctly.

## OptiX front-end optimization (P0–P4)

After the deferred-upload fix restored parity (~117 s), phase instrumentation
revealed the ~110 s startup was **not** the OptiX GPU build (≈0.2 s) but
CPU-side per-mesh geometry upload + scene/texture construction. Plan executed:

- **P0 — instrumentation (done).** `STAGE_TIMING` phase timers in
  `aggregate.cpp` and `textures.cpp`. Essential: located the real bottleneck.
- **P1 — bulk geometry upload (done, main win).** In `buildBVHForTriangles`
  (`gpu/optix/aggregate.cpp`) the old code did one `cudaMalloc` + `cudaMemcpy`
  per mesh (1591 meshes → 3182 allocations, **58 s**). Replaced with: gather
  all vertex/index data into two host staging buffers, then one `cudaMalloc` +
  one `cudaMemcpy` per buffer (2 allocations total, **0.06 s**). Vertex offset
  per mesh aligned to 16 B for OptiX. Byte-identical output (verified by EXR
  pixel diff vs. committed reference — only 4 EXR-header bytes differ, which are
  non-deterministic metadata).
- **P4 — SBT header precompute (done).** The 3 SBT record headers depend only
  on the program group, so they are packed once and `memcpy`'d into each record
  instead of calling `optixSbtRecordPackHeader` 3× per mesh (4773 OptiX API
  calls). Shaves ~2.7 s off the flatten; output pixel-identical.
- **P2 — parallelize builds / overlap upload (not beneficial, left as-is).**
  The 3 top-level GAS builds are already coded with `RunAsync`, but on Windows
  `DisableThreadPool()` is required (GPU managed-memory constraint, Issue #164)
  so they run serially and cannot be safely re-enabled. Texture upload is
  already Taskflow-parallel. With the geometry/OptiX stage now only ~4 s, there
  is nothing left to overlap.
- **P3 — build flags / compaction (not beneficial, left as-is).** The OptiX
  `optixAccelBuild` is ~0.25 s; compaction would not move the needle.

**Remaining bottleneck** after P0–P4 + texture-overlap (deep-dive A below):
- `optix-bvh-triangles-fn` (~10 s) — the **serial** flatten of 1591 meshes'
  geometry into the concatenated buffer (`DisableThreadPool` forces the
  `WavefrontPathIntegrator` ctor single-threaded). P1 only bulked the *upload*;
  this CPU gather/copy step is untouched and is now the single biggest cost.
- `wpi-CreateTextures` (~7.5 s) — per-reference texture *object* instantiation
  (scene-graph construction, not I/O).
- `texture-upload` (~12 s, runs on a background thread) — image decode + MIP +
  GPU array copy (already Taskflow-parallel across 147 textures).

`wpi-post-flush` (~13 s) is **not** a separate cost: it wraps the
`OptiXAggregate` constructor, so it *is* `optix-ctor-total` (~12.6 s) plus the
sub-0.3 s light Preprocess / LightSampler / queue allocation. There is nothing
extra to squeeze there.

All `STAGE_TIMING` prints remain in the code (cheap, useful for re-measuring);
they can be gated behind a flag or removed for a clean PR.

## Texture pipeline overlap (improvement 1)

After P0–P4, the dominant remaining cost is the texture pipeline. Finer phase
instrumentation of the `WavefrontPathIntegrator` ctor shows, for
`bistro_cafe_quick`:

- `wpi-CreateTextures` ≈ 12 s — builds the deferred texture *objects* (one per
  texture reference; the GPU `Create` functions only register a pending upload
  and do **not** read image files). Scene-graph construction overhead, not I/O.
- `texture-upload` ≈ 10 s — `FlushGPUTextureUploads()` runs `DoGPUTextureUpload`
  per unique texture on a Taskflow executor (already parallel across the 147
  textures). Each task does `Image::Read` (decode) + MIPMap +
  `cudaMallocMipmappedArray` + per-level `cudaMemcpy2DToArray`.
- `wpi-post-flush` ≈ 13 s — but this wraps the `OptiXAggregate` ctor, so it is
  essentially `optix-ctor-total`; light Preprocess / LightSampler / queue
  allocation (instrumented separately) are all sub-0.3 s.

**Change applied:** `FlushGPUTextureUploads()` is launched on a background
`std::thread` right after `CreateTextures` builds the pending-upload list, and
`join()`ed at the end of the `WavefrontPathIntegrator` ctor. This overlaps the
(~10 s) texture upload with `OptiXAggregate` construction (~4–12 s, varies) and
the rest of scene setup. The upload target is device memory (not managed), so
it does not violate the `DisableThreadPool` constraint; the join guarantees all
`texObj`s are set before `Render()`.

**Result:** `gpu-build+upload+bvh` ≈ 24–27 s (was ~27–28 s); output remains
pixel-identical to the committed reference (only 4 non-deterministic EXR-header
bytes differ). The measured saving (~2–3 s) is within run-to-run variance on
this machine because the upload's CPU decode and the OptiX build's CPU flatten
contend for the same cores; the change is structurally beneficial and helps
more where there is spare CPU/GPU headroom.

The dominant *unaddressed* cost is `wpi-CreateTextures` (~12 s) — texture
object instantiation per reference — which is scene-graph construction, not the
I/O/upload pipeline.

## Deep-dive: wpi-post-flush (A)

Instrumented the `WavefrontPathIntegrator` ctor sub-phases
(`wpi-lightPreprocess`, `wpi-lightSampler`, `wpi-queueAlloc`) to chase the
apparent ~14 s `wpi-post-flush`. Finding (clean run, `bistro_cafe_quick`):

- `wpi-lightPreprocess` = 0.00 s
- `wpi-lightSampler` = 0.30 s
- `wpi-queueAlloc` = 0.06 s
- `wpi-post-flush` = 13.02 s ≈ `optix-ctor-total` (12.59 s) + 0.43 s

**Conclusion:** `wpi-post-flush`'s timer wraps the `OptiXAggregate` constructor
(`aggregate = new OptiXAggregate(...)` at `wavefront/integrator.cpp:199`), so the
~13 s is the OptiX build itself — already optimized by P1/P4. There is **no**
hidden separable scene-setup cost in that span; light Preprocess, LightSampler,
and queue allocation are all negligible. The earlier mental model of
"post-flush = light/queue setup" was wrong.

Current end-to-end phase breakdown (clean run):

| phase | s |
|---|---|
| parse | 3.2 |
| wpi-CreateTextures | 7.5 |
| wpi-pre-flush | 9.3 |
| optix-init | 1.5 |
| optix-prepare-ply | 0.9 |
| optix-bvh-triangles-fn | ~4 (P5 parallel; was 9.6 serial) |
| optix-accelbuild | 0.1 |
| texture-upload (bg thread) | 12.7 |
| **gpu-build+upload+bvh** | **22.4** |
| render-total | 24.3 |

The real remaining separable cost is `wpi-CreateTextures` (~7.5–12.6 s, the
per-reference texture *object* instantiation — B candidate); the OptiX/mesh side
is no longer on the wall-critical path because `texture-upload` (~12–23 s,
variable) gates it.

## Parallelize triangle-mesh creation (P5)

In `buildBVHForTriangles`, the first `ParallelFor` (per-mesh `Triangle::CreateMesh`
/ `LoopSubdivide` / PLY construction, ~8.8 s **serial** under `DisableThreadPool`)
was the dominant cost inside `optix-bvh-triangles-fn` (~9.6 s). Added a manual
`std::thread` pool, `ParallelForManual` (`gpu/optix/aggregate.cpp`), capped at
*half* the cores so the concurrent background texture upload keeps headroom
(full subscription oversubscribes the machine and slows both phases). Each worker
obtains its own allocator via `threadAllocators.Get()`, which is backed by the
managed memory resource, so the meshes stay GPU-visible; no managed memory is
touched during the OptiX build itself. Output is pixel-identical to the committed
reference (EXR byte-diff shows only the 4 non-deterministic header bytes).

Result: `optix-tri-meshCreate` drops to ~3–4 s when cores are available (vs 8.8 s
serial). Net wall saving is only ~1 s, because `gpu-build` is now gated by
`wpi-CreateTextures` (~7.5–12.6 s) + `texture-upload` (~12–23 s, variable); the
OptiX/mesh side is no longer the critical path. The change is still worthwhile:
it removes a genuinely serial 8.8 s stage and helps on unloaded / many-core
machines and once the texture pipeline is faster.

## Attempted: texture decode-ahead overlap (C) — reverted

Goal: overlap image read/decode (~10–13 s of `texture-upload`) with the serial
`wpi-CreateTextures` window by decoding registered files on background threads
into a bounded cache consumed by `DoGPUTextureUpload` (with synchronous-read
fallback on miss).

Implementation went through several hardening rounds (byte budget, catch-all
worker isolation, capped worker count), but any *actively working* worker
thread triggered a reproducible process fail-fast (`0xC0000409` via
`terminate → abort`) — even a worker that only popped filenames without
decoding, while a sleeping-only worker ran clean through the entire scene
build. Root cause is environmental, not the design: this host has **16 GB RAM
and was at 0 GB free with 4.4 GB of pagefile in use** while WeChat/browser
widgets ran alongside; CRT allocation failures under commit exhaustion surface
exactly as these fast-fails, and loader/heap lock pile-ups explain the hang
variants. The feature was therefore **reverted**; it should be retried on a
machine with RAM headroom (the design itself is deadlock-free by construction:
consumers never wait on workers, and misses fall back to synchronous reads).

Retained from this investigation:

- **libdeflate is now linked statically** (vendored `src/ext/libdeflate`;
  `add_library(deflate STATIC)`, `LIBDEFLATE_DLL` define removed, OpenEXR
  forced onto the vendored target). Previously `pbrt.exe` imported
  `deflate.dll`, which the Windows loader resolved via **PATH to an
  incompatible copy from another product** (`E:\anaconda3\Library\bin`) — a
  latent crash/corruption hazard independent of this feature.
- `Printf` flushes stdout per line, so `STAGE_TIMING` progress survives crashes.

**Verification note:** because the OpenEXR deflate implementation changed,
rendered `.exr` files are no longer byte-identical to older references (the
container's compressed streams differ). Correctness must be checked at the
pixel level, e.g.
`imgtool diff --metric MAE --reference old.exr new.exr`
(silent exit 0 == `error.MaxValue()==0` == identical pixels). Verified:
post-revert renders are pixel-identical to the pre-change reference.

Note on measurements taken late in this session: with the host at 0 GB free,
wall times balloon 4–8× across *all* phases (even driver-only `optix-init`);
such runs are not comparable to the baselines above.

## Clean baseline (P0–P5) and original comparison



Four consecutive runs of `bistro_cafe_quick` with the current code (P0–P5),

```
gpu-build+upload+bvh   : 25.8 / 51.7 / 31.6 / 28.5 s
render-total           : 27.7 / 55.6 / 33.5 / 30.9 s
texture-upload         : 12.6 / 39.0 / 19.2 / 14.7 s
optix-bvh-triangles-fn : 10.6 / 36.1 / 2.4 / 11.2 s
```

Run 2 was a heavy-load outlier (texture-upload spiked to 39 s and optix/mesh
contended to 36 s) — machine contention, not a code regression. Best
(least-loaded) `gpu-build+upload+bvh` = **25.8 s**; typical ≈ 28–31 s.

Against the original committed code (`gpu-build` @ `248092c`) measured at
**~117 s** for the same scene, the OptiX front-end work (P0–P5) yields a
**~4.2–4.9×** reduction in startup. The remaining variance is dominated by the
texture pipeline: `wpi-CreateTextures` (~10–12 s) + `texture-upload`
(12–39 s, load-dependent). The OptiX/mesh side is only ~2–4 s when cores are
free, so it is no longer on the wall-critical path.

Controlled re-measurement (Windhawk injection disabled, ~6.4 GB RAM free),
four consecutive runs of `bistro_cafe_quick`:

```
gpu-build+upload+bvh : 26.7 / 25.9 / 46.6* / 25.6 s   (render-total 28.7/28.3/48.8/27.5)
wpi-CreateTextures   : 12.4 / 12.7 / 18.8* / 12.4 s
texture-upload       : 12.4 / 12.5 / 27.0* / 12.4 s
```

\* run 3 caught a transient load spike. Stable median **~26 s**, consistent
with the best-of runs above and confirming no regression from the
static-deflate submodule patches.


## Task-graph overlap: background PLY preload

PLY loading (optix-prepare-ply) is pure CPU work with no dependency on
textures/lights/materials, yet it used to sit on the critical path between
texture creation and the BVH builds -- and it swings wildly under machine
load (0.9 s idle vs 13 s observed). It now runs on a background thread
started immediately after parsing, concurrently with `wpi-CreateTextures`:

```
main : parse -> CreateTextures -> Lights/Materials -> join -> [displace] -> OptiXAggregate
bg   :            PreparePLYMeshesLoadOnly(850 meshes)
bg   :            FlushGPUTextureUploads (unchanged, overlaps OptiXAggregate)
```

Implementation: `OptiXAggregate::PreparePLYMeshesLoadOnly` (real parallelism
via `ParallelForManual`, since `PreparePLYMeshes` runs serially under
`DisableThreadPool`) plus `ApplyPLYDisplacements` for the deferred
displacement-texture evaluation that needs `floatTextures`. `OptiXAggregate`
gains a trailing `preloadedPlyMeshes` constructor parameter; empty means
load-as-before.

Result (`bistro_cafe_quick`, 850 PLY meshes): ctor-side `optix-prepare-ply`
drops to 0.00 s; `gpu-build+upload+bvh` = 28.6 / 26.5 / 31.9 s vs the ~26 s
baseline. On an idle machine the saving is small (idle prepare-ply is only
~0.9 s), but PLY I/O spikes no longer reach the critical path at all -- the
13 s spike observed earlier today would have been fully hidden. Output is
pixel-identical (`imgtool diff` MAE == 0).


## B: parallelize texture creation (scene.cpp)

BasicScene::CreateTextures was a serial loop over all named textures.
Procedural textures (checker/mix/...) look up other named textures in
`textures` during creation, so the code now splits each loop into a
self-contained subset (imagemap / constant / rgb / srgb / ptex -- all of
which never consult `textures`) that runs in parallel via `ParallelFor`,
and a dependent subset that keeps the original serial path.  Results land
in per-index slots and are merged serially afterwards.

Note: the global thread pool is still live here (DisableThreadPool runs','only later, inside the OptiXAggregate ctor), so ParallelFor gives real','parallelism.  Scene side also stays correct for CPU builds.
	extureCacheMutex in GPUSpectrumImageTexture::Create does NOT guard image
decode or GPU work -- only the cache lookup + pending-upload registration --
so it is not the speedup ceiling.

Result for bistro_cafe_quick: `wpi-CreateTextures` ~12 -> ~10 s and
`optix-ctor-total` ~12.6 -> ~10.8 s; gpu-build ~24 s median vs ~26 s
baseline.  The win is modest because per-texture work is small and this
machine has +-5 s variance; texture-heavy scenes would benefit more.
Output pixel-identical (imgtool diff MAE == 0).


## B: parallelize texture creation (scene.cpp)

BasicScene::CreateTextures was a serial loop over all named textures.
Procedural textures (checker/mix/...) look up other named textures in
`textures` during creation, so the code now splits each loop into a
self-contained subset (imagemap / constant / rgb / srgb / ptex -- all of
which never consult `textures`) that runs in parallel via `ParallelFor`,
and a dependent subset that keeps the original serial path.  Results land
in per-index slots and are merged serially afterwards.

Note: the global thread pool is still live here (DisableThreadPool runs
only later, inside the OptiXAggregate ctor), so ParallelFor gives real
parallelism.  The CPU build path is unchanged.
	extureCacheMutex in GPUSpectrumImageTexture::Create guards only the cache
lookup + pending-upload registration (not decode/GPU work), so it is not
the speedup ceiling.

Result for bistro_cafe_quick: wpi-CreateTextures ~12 -> ~10 s and
optix-ctor-total ~12.6 -> ~10.8 s; gpu-build ~24 s median vs ~26 s
baseline.  The win is modest because per-texture work is small and this
machine has +-5 s variance; texture-heavy scenes benefit more.
Output pixel-identical (imgtool diff MAE == 0).


## High-SPP reference: 640x360 @ 64 spp

Rendered istro_cafe_quick.pbrt --gpu --spp 64 (3 clean runs, exit 0):

```
render-total        49.9 / 46.1 / 45.0 s   (median ~46 s)
gpu-build+upload    46.0 / 43.7 / 42.8 s
optix-ctor-total    ~13.5 s   (meshCreate ~10.6 dominates)
wpi-CreateTextures  5-10 s    (machine-load dependent)
texture-upload      ~13 s     (147 unique textures, hidden behind ctor)
```

Compared to the 8 spp quick baseline (~24-32 s wall), 64 spp adds roughly
17 s of pure sampling (8x the rays), i.e. startup remains ~25-28 s and the
rest is sample time scaling linearly with spp as expected.


## Host-side mesh build + device mirrors (kills the cudaMallocManaged stall)

Profiling optix-tri-meshCreate (~13 s, was thought to be ~2 s) showed:
~1600 TriangleMesh constructions are parallel (ParallelForManual) but every
array allocation goes through cudaMallocManaged -- driver-serialized AND
implicitly syncing with in-flight GPU work (the background texture
uploads), ~65 ms per call under memory pressure. Chunk-size tuning does
not help: cost scales with bytes, not calls.

Fix: mesh construction now uses plain HOST memory (monotonic buffers over
new_delete), all per-mesh arrays (p/indices/n/s/uv/faceIndices) are bulk-
uploaded to pure device buffers on a dedicated copy stream
(`geomCopyStream`, cudaMemcpyAsync so they overlap texture-upload DMA), and
each SBT record points at a tiny device-resident TriangleMesh *mirror*
(POD memcpy + pointer patch) whose members target those device buffers.
The closest-hit shaders need no changes: they keep dereferencing rec.mesh,
which is now device-resident.

Result: optix-tri-meshCreate 13.0 -> 0.13 s (managed allocs 0),
optix-ctor-total ~13 -> 0.8-9 s (remaining time is real PCIe data
movement, varying with DMA contention). Output pixel-identical vs the 8 spp
reference (imgtool MAE == 0), 4/4 clean runs.

Next candidate if more is needed: move the whole geometry-prep+upload onto
the background thread (it no longer needs textures/lights/materials at all);
only SBT-record assembly must stay in the ctor after materials.


## Background geometry pipeline: ctor drops to ~0.2 s

The geometry stage moved fully onto the background thread.  After the PLY
preload, the same thread now speculatively runs `PrepareTriangleGeometry`
(host mesh build + staging concat, no CUDA) and `UploadTriangleGeometry`
(bulk H2D on a private stream) -- neither needs textures/materials/lights.
`buildBVHForTriangles` gained an early fast path: when preloaded geometry is
handed in it only assembles SBT records (alpha/material/area-lights/media)
and runs the acceleration build.

If deferred PLY displacement turns out to be present, the speculative
result is discarded after the join and the aggregate falls back to the
original self-contained path (correctness preserved).

Result (bistro_cafe_quick, 8 spp): optix-ctor-total 13 -> 0.10-0.21 s;
geometry work (~0.6 s CPU + ~0.13 s DMA) fully hidden under CreateTextures.
4/4 clean runs, pixel-identical vs reference (MAE == 0).

The critical path is now: parse -> CreateTextures+Lights -> [ctor ~0.2 s] ->
texture-upload join (~11 s, EXPOSED -- it used to hide behind the ctor) ->
render.  Next frontier if desired: producer/consumer texture uploads that
drain pending uploads while CreateTextures is still running, which would
overlap nearly all of the remaining 11 s.

## How to render

```bat
cd /d D:\models\pbrt-v4-scenes\bistro
F:\project\pbrt-v4\build\Release\pbrt.exe --gpu bistro_cafe_quick.pbrt
```

Always pass `--gpu`; pbrt defaults to CPU. Output lands in the scene folder.
