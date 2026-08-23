// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

// renderstream_probe: drive the RenderSink (Track B) by feeding pbrt's parser
// a real scene file. Validates the single-executor Taskflow pipeline end to
// end: textures are really uploaded to the GPU, shapes/materials/lights are
// recorded, and the EndOfFiles barrier completes crash-free.

#include <pbrt/options.h>
#include <pbrt/parser.h>
#include <pbrt/util/file.h>

#include <filesystem/path.h>

// ghc::filesystem (and CUDA) pull in <windows.h>, whose RGB macro collides with
// pbrt's RGB type. Undef it before the pbrt headers in render_sink.cpp are
// parsed.
#undef RGB

// RenderSink's implementation is compiled into libpbrt (via
// ${PBRT_WAVEFRONT_SOURCE}); we only need its header here.
#include "pbrt/wavefront/render_sink.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace pbrt;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scene.pbrt>\n", argv[0]);
        return 1;
    }
    std::string sceneFile = argv[1];

    PBRTOptions options;
    options.useGPU = true;  // activate the GPU upload path
    InitPBRT(options);

    // Resolve relative texture/material paths against the scene's directory,
    // mirroring pbrt's own behaviour of working relative to the .pbrt file.
    std::string sceneDir = filesystem::path(sceneFile).parent_path().str();
    if (!sceneDir.empty())
        SetSearchDirectory(sceneDir);

    RenderSink sink;

    auto t0 = std::chrono::high_resolution_clock::now();
    // ParseFiles drives the ParserTarget callbacks; the final EndOfFiles()
    // callback builds the barrier edges and runs the whole Taskflow graph on
    // the single executor (blocking until completion).
    ParseFiles(&sink, {sceneFile});
    auto t1 = std::chrono::high_resolution_clock::now();

    double wall = std::chrono::duration<double>(t1 - t0).count();

    fprintf(stderr, "\n=== RenderSink (Track B) probe results ===\n");
    fprintf(stderr, "  textures realized on GPU : %zu (stubbed/failed: %zu)\n",
            sink.TexturesRealized(), sink.TextureStubs());
    fprintf(stderr, "  shapes recorded         : %zu\n", sink.ShapeCount());
    fprintf(stderr, "  materials recorded       : %zu\n", sink.MaterialCount());
    fprintf(stderr, "  lights recorded          : %zu\n", sink.LightCount());
    fprintf(stderr, "  end barrier / BVH stub   : %s\n", sink.BvhBuilt() ? "done" : "NO");
    fprintf(stderr, "  total wall time          : %.3f s\n", wall);

    CleanupPBRT();
    return 0;
}
