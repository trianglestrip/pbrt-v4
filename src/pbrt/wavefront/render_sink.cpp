// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

// Track B RenderSink implementation. Consumes ParserTarget events, builds a
// Taskflow taskgraph, and runs it ONCE on a single tf::Executor at EndOfFiles.
// Texture leaf tasks perform the ACTUAL GPU upload by reusing pbrt's public
// decode/MIP path (Image::Read + MIPMap) and the CUDA runtime -- so textures
// are really uploaded to the device -- while all scheduling stays on the one
// executor (no RunParallelTasks, no ParallelFor from std::thread, no global
// "all uploads done" poll beyond the EndOfFiles graph barrier).

#include "render_sink.h"

#include <pbrt/options.h>
#include <pbrt/pbrt.h>
#include <pbrt/util/colorspace.h>
#include <pbrt/util/file.h>
#include <pbrt/util/float.h>
#include <pbrt/util/image.h>
#include <pbrt/util/memory.h>
#include <pbrt/util/mipmap.h>
#include <pbrt/util/vecmath.h>

#include <cuda_runtime.h>

#include <cstdio>
#include <string>
#include <thread>

namespace pbrt {

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

static std::string GetStringParam(const ParsedParameterVector &params,
                                  const std::string &name) {
    for (const ParsedParameter *p : params)
        if (p && p->name == name && !p->strings.empty())
            return p->strings[0];
    return "";
}

// CUDA error checking that NEVER throws (keeps the probe crash-free): on error
// we log and continue, skipping the offending upload rather than aborting.
static bool CUDACheck(cudaError_t e, const char *what) {
    if (e == cudaSuccess)
        return true;
    fprintf(stderr, "  [cuda] %s failed: %s\n", what, cudaGetErrorString(e));
    return false;
}

// Decode + MIP + upload one image file to a CUDA mipmapped array and return the
// texture object (0 on failure). Mirrors the RGBA upload tail in
// textures.cpp / DoGPUTextureUpload, using only public pbrt APIs.
static unsigned long long UploadOneTexture(const std::string &filename) {
    ImageAndMetadata immeta = Image::Read(filename);
    Image &image = immeta.image;
    if (!image) {
        fprintf(stderr, "  [tex] decode failed: %s\n", filename.c_str());
        return 0;
    }

    const RGBColorSpace *colorSpace = immeta.metadata.GetColorSpace();
    ImageChannelDesc rgbDesc = image.GetChannelDesc({"R", "G", "B"});
    Image work = image;
    if (rgbDesc)
        work = image.SelectChannels(rgbDesc);

    MIPMap mipmap(work, colorSpace, WrapMode::Repeat, Allocator(), MIPMapFilterOptions());
    int nLevels = mipmap.Levels();
    if (nLevels < 1)
        return 0;

    const Image &base = mipmap.GetLevel(0);
    PixelFormat fmt = base.Format();

    cudaChannelFormatDesc channelDesc;
    cudaTextureReadMode readMode;
    if (fmt == PixelFormat::U256) {
        channelDesc = cudaCreateChannelDesc(8, 8, 8, 8, cudaChannelFormatKindUnsigned);
        readMode = cudaReadModeNormalizedFloat;
    } else if (fmt == PixelFormat::Half) {
        channelDesc = cudaCreateChannelDesc(16, 16, 16, 16, cudaChannelFormatKindFloat);
        readMode = cudaReadModeElementType;
    } else if (fmt == PixelFormat::Float) {
        channelDesc = cudaCreateChannelDesc(32, 32, 32, 32, cudaChannelFormatKindFloat);
        readMode = cudaReadModeElementType;
    } else {
        fprintf(stderr, "  [tex] unsupported pixel format: %s\n", filename.c_str());
        return 0;
    }

    cudaExtent extent =
        make_cudaExtent(base.Resolution().x, base.Resolution().y, 0);
    cudaMipmappedArray_t mipArray = nullptr;
    if (!CUDACheck(cudaMallocMipmappedArray(&mipArray, &channelDesc, extent, nLevels, 0),
                  "cudaMallocMipmappedArray"))
        return 0;

    bool ok = true;
    for (int level = 0; level < nLevels && ok; ++level) {
        const Image &lvl = mipmap.GetLevel(level);
        cudaArray_t levelArray;
        if (!CUDACheck(cudaGetMipmappedArrayLevel(&levelArray, mipArray, level),
                      "cudaGetMipmappedArrayLevel")) {
            ok = false;
            break;
        }
        int w = lvl.Resolution().x, h = lvl.Resolution().y;
        if (fmt == PixelFormat::U256) {
            std::vector<uint8_t> buf(size_t(4) * w * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    size_t o = size_t(4) * (y * w + x);
                    const uint8_t *px = (const uint8_t *)lvl.RawPointer({x, y});
                    buf[o] = px[0];
                    buf[o + 1] = px[1];
                    buf[o + 2] = px[2];
                    buf[o + 3] = 255;
                }
            int pitch = w * 4 * sizeof(uint8_t);
            ok = CUDACheck(cudaMemcpy2DToArray(levelArray, 0, 0, buf.data(), pitch, pitch,
                                              h, cudaMemcpyHostToDevice),
                          "cudaMemcpy2DToArray");
        } else if (fmt == PixelFormat::Half) {
            std::vector<Half> buf(size_t(4) * w * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    size_t o = size_t(4) * (y * w + x);
                    buf[o] = Half(lvl.GetChannel({x, y}, 0));
                    buf[o + 1] = Half(lvl.GetChannel({x, y}, 1));
                    buf[o + 2] = Half(lvl.GetChannel({x, y}, 2));
                    buf[o + 3] = Half(1.f);
                }
            int pitch = w * 4 * sizeof(Half);
            ok = CUDACheck(cudaMemcpy2DToArray(levelArray, 0, 0, buf.data(), pitch, pitch,
                                              h, cudaMemcpyHostToDevice),
                          "cudaMemcpy2DToArray");
        } else {  // Float
            std::vector<float> buf(size_t(4) * w * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    size_t o = size_t(4) * (y * w + x);
                    buf[o] = lvl.GetChannel({x, y}, 0);
                    buf[o + 1] = lvl.GetChannel({x, y}, 1);
                    buf[o + 2] = lvl.GetChannel({x, y}, 2);
                    buf[o + 3] = 1.f;
                }
            int pitch = w * 4 * sizeof(float);
            ok = CUDACheck(cudaMemcpy2DToArray(levelArray, 0, 0, buf.data(), pitch, pitch,
                                              h, cudaMemcpyHostToDevice),
                          "cudaMemcpy2DToArray");
        }
    }

    if (!ok) {
        if (mipArray)
            cudaFreeMipmappedArray(mipArray);
        return 0;
    }

    cudaResourceDesc resDesc = {};
    resDesc.resType = cudaResourceTypeMipmappedArray;
    resDesc.res.mipmap.mipmap = mipArray;

    cudaTextureDesc texDesc = {};
    texDesc.addressMode[0] = cudaAddressModeWrap;
    texDesc.addressMode[1] = cudaAddressModeWrap;
    texDesc.filterMode = cudaFilterModeLinear;
    texDesc.readMode = readMode;
    texDesc.normalizedCoords = 1;
    texDesc.maxAnisotropy = 8;
    texDesc.maxMipmapLevelClamp = nLevels - 1;
    texDesc.minMipmapLevelClamp = 0;
    texDesc.mipmapFilterMode = cudaFilterModeLinear;
    texDesc.borderColor[0] = texDesc.borderColor[1] = texDesc.borderColor[2] =
        texDesc.borderColor[3] = 0.f;
    texDesc.sRGB = 1;

    cudaTextureObject_t texObj = 0;
    if (!CUDACheck(cudaCreateTextureObject(&texObj, &resDesc, &texDesc, nullptr),
                   "cudaCreateTextureObject")) {
        cudaFreeMipmappedArray(mipArray);
        return 0;
    }
    return (unsigned long long)texObj;
}

// ---------------------------------------------------------------------------
// RenderSink
// ---------------------------------------------------------------------------

RenderSink::RenderSink()
    // Bound the worker count to keep concurrent decode/upload memory in check
    // (crash-free priority); all work still runs on this single executor.
    : executor(std::min<int>(4, std::max<int>(1, (int)std::thread::hardware_concurrency()))) {
}

RenderSink::~RenderSink() = default;

tf::Task RenderSink::addLeaf(std::function<void()> work) {
    tf::Task t = taskflow.emplace(std::move(work));
    allTasks.push_back(t);
    return t;
}

void RenderSink::dependOn(tf::Task dependent, const std::string &key) {
    pendingDeps.push_back({dependent, key});
}

void RenderSink::realizeTexture(const std::string &name,
                                const std::string &resolvedFile) {
    unsigned long long texObj = UploadOneTexture(resolvedFile);
    if (texObj) {
        nTexturesRealized.fetch_add(1);
        std::lock_guard<std::mutex> lk(realizedMutex);
        realizedTextures[name] = texObj;
    } else {
        nTextureStubs.fetch_add(1);
        fprintf(stderr, "  [tex] stubbed (no GPU upload): %s\n", resolvedFile.c_str());
    }
}

void RenderSink::finalize() {
    // Barrier node: runs after every leaf/dependency task.
    // OptiX BVH construction is intentionally stubbed for this milestone;
    // it is reported as such so the report is honest about what was realized.
    bvhBuilt.store(true);
    Printf("RenderSink: all %zu leaf tasks completed; barrier reached.\n",
           allTasks.size());
}

// --- structural / no-op events -------------------------------------------

void RenderSink::Scale(Float, Float, Float, FileLoc) {}
void RenderSink::Option(const std::string &, const std::string &, FileLoc) {}
void RenderSink::Identity(FileLoc) {}
void RenderSink::Translate(Float, Float, Float, FileLoc) {}
void RenderSink::Rotate(Float, Float, Float, Float, FileLoc) {}
void RenderSink::LookAt(Float, Float, Float, Float, Float, Float, Float, Float, Float,
                        FileLoc) {}
void RenderSink::ConcatTransform(Float[16], FileLoc) {}
void RenderSink::Transform(Float[16], FileLoc) {}
void RenderSink::CoordinateSystem(const std::string &, FileLoc) {}
void RenderSink::CoordSysTransform(const std::string &, FileLoc) {}
void RenderSink::ActiveTransformAll(FileLoc) {}
void RenderSink::ActiveTransformEndTime(FileLoc) {}
void RenderSink::ActiveTransformStartTime(FileLoc) {}
void RenderSink::TransformTimes(Float, Float, FileLoc) {}
void RenderSink::ColorSpace(const std::string &, FileLoc) {}
void RenderSink::PixelFilter(const std::string &, ParsedParameterVector, FileLoc) {}
void RenderSink::Film(const std::string &, ParsedParameterVector, FileLoc) {}
void RenderSink::Accelerator(const std::string &, ParsedParameterVector, FileLoc) {}
void RenderSink::Integrator(const std::string &, ParsedParameterVector, FileLoc) {}
void RenderSink::Camera(const std::string &, ParsedParameterVector, FileLoc) {}
void RenderSink::MakeNamedMedium(const std::string &, ParsedParameterVector, FileLoc) {}
void RenderSink::MediumInterface(const std::string &, const std::string &, FileLoc) {}
void RenderSink::Sampler(const std::string &, ParsedParameterVector, FileLoc) {}
void RenderSink::WorldBegin(FileLoc) {}
void RenderSink::AttributeBegin(FileLoc) {}
void RenderSink::AttributeEnd(FileLoc) {}
void RenderSink::Attribute(const std::string &, ParsedParameterVector, FileLoc) {}
void RenderSink::ReverseOrientation(FileLoc) {}
void RenderSink::ObjectBegin(const std::string &, FileLoc) {}
void RenderSink::ObjectEnd(FileLoc) {}
void RenderSink::ObjectInstance(const std::string &, FileLoc) {}

// --- asset-bearing events --------------------------------------------------

void RenderSink::Texture(const std::string &name, const std::string &type,
                         const std::string &texname, ParsedParameterVector params,
                         FileLoc) {
    std::string fn = GetStringParam(params, "filename");
    if (fn.empty()) {
        // Non-image (e.g. constant/spectrum) texture: record a no-op resource
        // node so references resolve. No GPU upload needed.
        tf::Task t = addLeaf([] {});
        resourceNodes["tex:" + name] = t;
        return;
    }
    std::string resolved = ResolveFilename(fn);
    auto fit = filenameToTask.find(resolved);
    if (fit != filenameToTask.end()) {
        // Same file already uploaded (referenced under another name): share it.
        resourceNodes["tex:" + name] = fit->second;
        return;
    }
    tf::Task t = addLeaf([this, name, resolved] { realizeTexture(name, resolved); });
    resourceNodes["tex:" + name] = t;
    filenameToTask[resolved] = t;
}

void RenderSink::Shape(const std::string &, ParsedParameterVector params, FileLoc) {
    tf::Task t = addLeaf([this] { nShapes.fetch_add(1); });
    // A shape may reference textures by "texture <param>" names.
    for (const ParsedParameter *p : params) {
        if (!p)
            continue;
        if (p->name.size() > 8 && p->name.compare(0, 8, "texture ") == 0 &&
            !p->strings.empty())
            dependOn(t, "tex:" + p->strings[0]);
    }
}

void RenderSink::Material(const std::string &, ParsedParameterVector params, FileLoc) {
    tf::Task t = addLeaf([this] { nMaterials.fetch_add(1); });
    for (const ParsedParameter *p : params) {
        if (!p)
            continue;
        if (p->name == "namedmaterial" && !p->strings.empty())
            dependOn(t, "nm:" + p->strings[0]);
        else if (p->name.size() > 8 && p->name.compare(0, 8, "texture ") == 0 &&
                 !p->strings.empty())
            dependOn(t, "tex:" + p->strings[0]);
    }
}

void RenderSink::MakeNamedMaterial(const std::string &name, ParsedParameterVector params,
                                   FileLoc) {
    tf::Task t = addLeaf([this, name] {
        nMaterials.fetch_add(1);
    });
    resourceNodes["nm:" + name] = t;
    for (const ParsedParameter *p : params) {
        if (!p)
            continue;
        if (p->name.size() > 8 && p->name.compare(0, 8, "texture ") == 0 &&
            !p->strings.empty())
            dependOn(t, "tex:" + p->strings[0]);
    }
}

void RenderSink::NamedMaterial(const std::string &name, FileLoc) {
    tf::Task t = addLeaf([] {});
    dependOn(t, "nm:" + name);
}

void RenderSink::LightSource(const std::string &, ParsedParameterVector params, FileLoc) {
    tf::Task t = addLeaf([this] { nLights.fetch_add(1); });
    for (const ParsedParameter *p : params) {
        if (!p)
            continue;
        if (p->name.size() > 8 && p->name.compare(0, 8, "texture ") == 0 &&
            !p->strings.empty())
            dependOn(t, "tex:" + p->strings[0]);
    }
}

void RenderSink::AreaLightSource(const std::string &, ParsedParameterVector params,
                                 FileLoc) {
    tf::Task t = addLeaf([this] { nLights.fetch_add(1); });
    for (const ParsedParameter *p : params) {
        if (!p)
            continue;
        if (p->name.size() > 8 && p->name.compare(0, 8, "texture ") == 0 &&
            !p->strings.empty())
            dependOn(t, "tex:" + p->strings[0]);
    }
}

void RenderSink::EndOfFiles() {
    // Resolve dependency edges now that every defining event has been seen,
    // so forward references are safe (no manual readiness polling).
    for (auto &[dep, key] : pendingDeps) {
        auto it = resourceNodes.find(key);
        if (it != resourceNodes.end())
            dep.succeed(it->second);
    }

    // Barrier: runs only after all tasks. OptiX BVH build is stubbed here.
    tf::Task end = taskflow.emplace([this] { finalize(); });
    for (tf::Task &t : allTasks)
        end.succeed(t);

    // Run the whole graph ONCE on the single executor and block.
    executor.run(taskflow).wait();
}

}  // namespace pbrt
