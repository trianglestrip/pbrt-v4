# pbrt-v4 (GPU build) — Project Notes

This repo is a clone of `https://github.com/trianglestrip/pbrt-v4` with a
GPU-capable build maintained on the `gpu-build` branch.

## Build environment (this machine)

- **GPU**: NVIDIA GeForce RTX 2060 SUPER (compute capability **sm_75**, Turing)
  — driver 591.86
- **CUDA**: v13.1 at `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1`
- **OptiX**: SDK 8.0.0 at `C:\ProgramData\NVIDIA Corporation\OptiX SDK 8.0.0`
  (upgraded from 7.5.0; 7.5 predates CUDA 13.1 so 8.0 is the better fit)
- **Visual Studio**: 2022 Community (`vcvarsall.bat x64`)
- **CMake**: 3.31.6

## Building

GPU support is enabled automatically when CUDA is found and `PBRT_OPTIX_PATH`
is set. Configure + build (must run from a VS x64 dev shell):

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
  -DPBRT_OPTIX_PATH="C:\ProgramData\NVIDIA Corporation\OptiX SDK 7.5.0" ^
  -DPBRT_GPU_SHADER_MODEL=sm_75
cmake --build build --config Release --target pbrt_exe -j 8
```

Binary: `build\Release\pbrt.exe`

> The `gpu-build` branch also fixes a parser bug in `src/pbrt/parser.cpp`
> (WorldEnd was only swallowed in formatting mode, breaking normal scene
> parsing). Keep that fix when rebasing.

## Rendering conventions

- **Always render on the GPU**: pbrt defaults to CPU, so pass `--gpu`
  explicitly, e.g. `build\Release\pbrt.exe --gpu <scene.pbrt>`.
- **Default test scenes**: `D:\models\pbrt-v4-scenes\` (one folder per scene,
  e.g. `barcelona-pavilion`, `bistro`, `bmw-m6`, `ganesha`, ...).
- **Output location**: rendered images go into the **scene's own folder**
  (alongside the `.pbrt` file). Easiest way: `cd` into the scene directory
  before running pbrt so `pbrt.exr` (or the film filename) lands there.
  Example:

  ```bat
  cd /d D:\models\pbrt-v4-scenes\bistro
  F:\project\pbrt-v4\build\Release\pbrt.exe --gpu bistro.pbrt
  ```

- For faster GPU previews use `--spp <N>` to raise samples per pixel.

## Render timing notes (RTX 2060 SUPER)

- The GPU ray tracing itself is fast (a 640×360 @ 8spp bistro crop finishes
  its tiles in well under 1s). Wall-clock time is dominated by:
  1. **Startup** — OptiX BVH build + texture upload. After the P0–P5
     optimizations on `gpu-build` (see `benchmark.md`), `bistro_cafe_quick`
     starts in **~22–28 s** (was ~117 s; ~4–5× faster).
  2. **Sample time** — scales with resolution × samples-per-pixel.
- Measured: `bistro/bistro_cafe.pbrt` (1920×1080, 256spp) ≈ **481s**;
  `bistro_cafe_quick.pbrt` (640×360, 8spp): startup ~25s + a few seconds of
  sampling → `bistro_cafe_quick.exr`.
- To get a "few minutes" preview, lower `pixelsamples` (e.g. 16–32) rather than
  resolution.

## ⚠️ Host memory requirement

This build needs several GB of free RAM during scene setup (texture decode
peaks). On the dev machine (16 GB total), close memory-hungry apps first —
with **0 GB free** pbrt can fail-fast (`0xC0000409`) or hang inside CRT heap /
loader locks, and all timings balloon 4–8×. See `benchmark.md` ("Attempted:
texture decode-ahead overlap") for the full incident write-up.

## Local submodule patches (must reapply after `git submodule update`)

`pbrt.exe` must NOT import `deflate.dll`: with no DLL shipped beside the exe,
the Windows loader resolves it via PATH and had been loading an incompatible
copy from another product (`E:\anaconda3\Library\bin`). Two local patches fix
this by linking libdeflate statically:

- `patches/libdeflate-static-link.patch` → apply inside `src/ext/libdeflate`
- `patches/openexr-vendored-deflate.patch` → apply inside `src/ext/openexr`

```bat
git -C src\ext\libdeflate apply ..\..\..\patches\libdeflate-static-link.patch
git -C src\ext\openexr  apply ..\..\..\patches\openexr-vendored-deflate.patch
```

If either submodule shows as modified (`m` in `git status`), that is expected.

## Verifying renders after code changes

`.exr` output bytes are NOT comparable across builds (the OpenEXR deflate
implementation changed; container streams differ). Compare pixels instead:

```bat
build\Release\imgtool.exe diff --metric MAE --reference <old.exr> <new.exr>
```

Silent exit 0 means `error.MaxValue()==0`, i.e. pixel-identical.
A byte-level reference render lives at `bistro_ref.exr` (repo root).
