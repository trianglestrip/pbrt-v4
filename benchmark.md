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

Instrumentation added (`STAGE_TIMING` prints) in `cmd/pbrt.cpp` (parse) and
`wavefront/wavefront.cpp` (GPU build/upload/BVH vs. render). Quick scene
`bistro/bistro_cafe_quick.pbrt` (640×360, 8 spp), RTX 2060 SUPER, OptiX 8.0.0:

```
STAGE_TIMING [parse]                2.53 s
STAGE_TIMING [gpu-build+upload+bvh] 113.98 s   <-- dominates
STAGE_TIMING [render-total]         115.97 s
```

The actual GPU ray tracing finishes in well under ~2 s (tiles done almost
instantly); **~98% of wall-clock is `gpu-build+upload+bvh`** — i.e. CPU-side
scene construction + texture decode/upload + OptiX acceleration-structure build.
This is the exact stage targeted by the planned memory-mapped file reads and
Taskflow-parallel upload.

## End-to-end wall-clock (OptiX 8.0.0)

| Scene file                       | Res / spp        | Time    | Output                        |
|----------------------------------|------------------|---------|-------------------------------|
| `bistro/bistro_cafe.pbrt`       | 1920×1080 / 256 | ~481 s* | `bistro_cafe.exr`             |
| `bistro/bistro_cafe_med.pbrt`   | 720×405 / 32    | ~101 s  | `bistro_cafe_med.exr`         |
| `bistro/bistro_cafe_quick.pbrt` | 640×360 / 8     | ~116 s  | `bistro_cafe_quick.exr`       |

\* The 481 s for the full 1080p/256spp is ~110 s fixed startup + ~370 s of
actual 256-spp sample time. The ~110 s fixed startup is the `gpu-build+upload+bvh`
stage above and is constant regardless of spp.

## How to render

```bat
cd /d D:\models\pbrt-v4-scenes\bistro
F:\project\pbrt-v4\build\Release\pbrt.exe --gpu bistro_cafe_quick.pbrt
```

Always pass `--gpu`; pbrt defaults to CPU. Output lands in the scene folder.
