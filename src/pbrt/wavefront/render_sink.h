// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0
//
// Track B (RenderSink): consume ParserTarget events and realize GPU resources
// in parallel on a SINGLE tf::Executor using a Taskflow taskgraph.
//
// Architecture (single executor, no second scheduler, no global poll barrier):
//   * Leaf tasks (one per asset, per-event single ownership):
//       - Texture   -> decode + cuda upload of the image (real GPU work)
//       - Shape     -> count + record (placeholder GPU resource; geometric
//                       fidelity is out of scope for this milestone)
//       - Material / NamedMaterial / Light -> count + record
//   * Dependency edges: a referencing task (Material referencing a named
//       material or a texture) calls .succeed(resourceNode) so the referenced
//       asset is realized first. Forward references are naturally safe because
//       edges are wired at EndOfFiles, by which point every defining event has
//       already been processed and its resource node created.
//   * Barrier node ("end"): every task .precede(end); it runs only after all
//       leaf/dependency work completes (OptiX BVH build is stubbed for this
//       milestone and reported as such).
// The graph is built incrementally as events arrive and run ONCE at EndOfFiles
// on the sole tf::Executor. There is no other thread pool and no manual
// "all uploads done" poll.

#ifndef PBRT_WAVEFRONT_RENDER_SINK_H
#define PBRT_WAVEFRONT_RENDER_SINK_H

#include <pbrt/parser.h>
#include <taskflow/taskflow.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pbrt {

class RenderSink : public ParserTarget {
  public:
    RenderSink();
    ~RenderSink() override;

    // --- ParserTarget interface (all events) ---
    void Scale(Float, Float, Float, FileLoc) override;
    void Shape(const std::string &, ParsedParameterVector, FileLoc) override;
    void Option(const std::string &, const std::string &, FileLoc) override;
    void Identity(FileLoc) override;
    void Translate(Float, Float, Float, FileLoc) override;
    void Rotate(Float, Float, Float, Float, FileLoc) override;
    void LookAt(Float, Float, Float, Float, Float, Float, Float, Float, Float, FileLoc) override;
    void ConcatTransform(Float[16], FileLoc) override;
    void Transform(Float[16], FileLoc) override;
    void CoordinateSystem(const std::string &, FileLoc) override;
    void CoordSysTransform(const std::string &, FileLoc) override;
    void ActiveTransformAll(FileLoc) override;
    void ActiveTransformEndTime(FileLoc) override;
    void ActiveTransformStartTime(FileLoc) override;
    void TransformTimes(Float, Float, FileLoc) override;
    void ColorSpace(const std::string &, FileLoc) override;
    void PixelFilter(const std::string &, ParsedParameterVector, FileLoc) override;
    void Film(const std::string &, ParsedParameterVector, FileLoc) override;
    void Accelerator(const std::string &, ParsedParameterVector, FileLoc) override;
    void Integrator(const std::string &, ParsedParameterVector, FileLoc) override;
    void Camera(const std::string &, ParsedParameterVector, FileLoc) override;
    void MakeNamedMedium(const std::string &, ParsedParameterVector, FileLoc) override;
    void MediumInterface(const std::string &, const std::string &, FileLoc) override;
    void Sampler(const std::string &, ParsedParameterVector, FileLoc) override;
    void WorldBegin(FileLoc) override;
    void AttributeBegin(FileLoc) override;
    void AttributeEnd(FileLoc) override;
    void Attribute(const std::string &, ParsedParameterVector, FileLoc) override;
    void Texture(const std::string &name, const std::string &type,
                 const std::string &texname, ParsedParameterVector params, FileLoc) override;
    void Material(const std::string &name, ParsedParameterVector params, FileLoc) override;
    void MakeNamedMaterial(const std::string &name, ParsedParameterVector params,
                           FileLoc) override;
    void NamedMaterial(const std::string &name, FileLoc) override;
    void LightSource(const std::string &, ParsedParameterVector, FileLoc) override;
    void AreaLightSource(const std::string &, ParsedParameterVector, FileLoc) override;
    void ReverseOrientation(FileLoc) override;
    void ObjectBegin(const std::string &, FileLoc) override;
    void ObjectEnd(FileLoc) override;
    void ObjectInstance(const std::string &, FileLoc) override;
    void EndOfFiles() override;

    // --- results ---
    size_t TexturesRealized() const { return nTexturesRealized.load(); }
    size_t ShapeCount() const { return nShapes.load(); }
    size_t MaterialCount() const { return nMaterials.load(); }
    size_t LightCount() const { return nLights.load(); }
    bool BvhBuilt() const { return bvhBuilt.load(); }
    // Number of texture files that could not be uploaded (stubbed/failed).
    size_t TextureStubs() const { return nTextureStubs.load(); }

  private:
    // THE ONLY executor for all parallel work in this process.
    tf::Executor executor;
    tf::Taskflow taskflow;

    tf::Task addLeaf(std::function<void()> work);

    // Wire a referencing task to the resource node for `key` (e.g. "tex:NAME"
    // or "nm:NAME"). Edges are resolved at EndOfFiles so forward references
    // are safe.
    void dependOn(tf::Task dependent, const std::string &key);

    // Realizes one texture image on the GPU (decode + MIP + cuda upload).
    void realizeTexture(const std::string &name, const std::string &resolvedFile);

    void finalize();

    std::atomic<size_t> nTexturesRealized{0};
    std::atomic<size_t> nTextureStubs{0};
    std::atomic<size_t> nShapes{0};
    std::atomic<size_t> nMaterials{0};
    std::atomic<size_t> nLights{0};
    std::atomic<bool> bvhBuilt{false};

    // name/key -> resource-realizing task (created when the defining event
    // arrives; referenced by later events via dependOn()).
    std::map<std::string, tf::Task> resourceNodes;
    // resolved filename -> already-created upload task (coalesce duplicate
    // files referenced by multiple texture names).
    std::map<std::string, tf::Task> filenameToTask;
    // every task created, so the barrier can .succeed() them all.
    std::vector<tf::Task> allTasks;
    // recorded (dependent task, resource key) edges, resolved at EndOfFiles.
    std::vector<std::pair<tf::Task, std::string>> pendingDeps;

    std::mutex realizedMutex;
    std::map<std::string, unsigned long long> realizedTextures;  // name -> texObj
};

}  // namespace pbrt

#endif  // PBRT_WAVEFRONT_RENDER_SINK_H
