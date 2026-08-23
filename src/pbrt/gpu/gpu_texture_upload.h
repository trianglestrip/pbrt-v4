// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#ifndef PBRT_GPU_TEXTURE_UPLOAD_H
#define PBRT_GPU_TEXTURE_UPLOAD_H

#include <pbrt/pbrt.h>

namespace pbrt {

// Runs all deferred GPU image-texture uploads in parallel via a Taskflow
// graph.  Must be called once after scene texture/material creation and
// before rendering (and, on Windows, before OptiX BVH construction).
void FlushGPUTextureUploads();

// Signals that no further texture uploads will be registered (call after
// scene texture creation completes).
void SetGPUTextureCreationDone();

}  // namespace pbrt

#endif  // PBRT_GPU_TEXTURE_UPLOAD_H
