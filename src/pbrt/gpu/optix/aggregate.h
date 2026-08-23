// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#ifndef PBRT_GPU_OPTIX_AGGREGATE_H
#define PBRT_GPU_OPTIX_AGGREGATE_H

#include <pbrt/pbrt.h>

#include <pbrt/gpu/memory.h>
#include <pbrt/gpu/optix/optix.h>
#include <pbrt/scene.h>
#include <pbrt/util/containers.h>
#include <pbrt/util/pstd.h>
#include <pbrt/util/soa.h>
#include <pbrt/util/vecmath.h>
#include <pbrt/wavefront/integrator.h>
#include <pbrt/wavefront/workitems.h>

#include <map>
#include <string>
#include <vector>

#include <cuda.h>
#include <cuda_runtime.h>
#include <optix.h>

namespace pbrt {

// All OptiX state that must exist before any scene geometry is built.  It has
// no dependency on textures or materials, so it can be created on a background
// thread and overlapped with texture creation.  The SBT records store GPU
// pointers and remain valid after the bundle is moved into the aggregate.
struct OptiXInitBundle {
    OptixDeviceContext optixContext = nullptr;
    OptixModule optixModule = nullptr;
    OptixProgramGroup raygenPGClosest = nullptr, missPGNoOp = nullptr,
                     hitPGTriangle = nullptr, hitPGBilinearPatch = nullptr,
                     hitPGQuadric = nullptr;
    OptixProgramGroup raygenPGShadow = nullptr, missPGShadow = nullptr,
                     anyhitPGShadowTriangle = nullptr;
    OptixProgramGroup raygenPGShadowTr = nullptr, missPGShadowTr = nullptr;
    OptixProgramGroup anyhitPGShadowBilinearPatch = nullptr,
                     anyhitPGShadowQuadric = nullptr;
    OptixProgramGroup raygenPGRandomHit = nullptr, hitPGRandomHitTriangle = nullptr,
                     hitPGRandomHitBilinearPatch = nullptr,
                     hitPGRandomHitQuadric = nullptr;
    OptixPipeline optixPipeline = nullptr;
};

class OptiXAggregate : public WavefrontAggregate {
  public:
    // If initBundle is provided it is consumed (moved from); otherwise OptiX
    // is initialized here.  Passing a bundle lets the caller overlap OptiX
    // initialization with texture creation on a background thread.
    OptiXAggregate(const BasicScene &scene, CUDATrackedMemoryResource *memoryResource,
                   NamedTextures &textures,
                   const std::map<int, pstd::vector<Light> *> &shapeIndexToAreaLights,
                   const std::map<std::string, Medium> &media,
                   const std::map<std::string, pbrt::Material> &namedMaterials,
                   const std::vector<pbrt::Material> &materials,
                   std::map<int, TriQuadMesh> preloadedPlyMeshes = {},
                   OptiXInitBundle *initBundle = nullptr);

    // Creates everything in OptiXInitBundle.  optixInit() + device context +
    // module + program groups + pipeline + raygen/miss SBT records.  Safe to
    // call from a background thread (the caller must have pushed the CUDA
    // context with cuCtxSetCurrent first).
    static OptiXInitBundle CreateOptiXBundle(CUcontext cudaContext);

    Bounds3f Bounds() const { return bounds; }

    void IntersectClosest(int maxRays, const RayQueue *rayQueue,
                          EscapedRayQueue *escapedRayQueue,
                          HitAreaLightQueue *hitAreaLightQueue,
                          MaterialEvalQueue *basicEvalMaterialQueue,
                          MaterialEvalQueue *universalEvalMaterialQueue,
                          MediumSampleQueue *mediumSampleQueue,
                          RayQueue *nextRayQueue) const;

    void IntersectShadow(int maxRays, ShadowRayQueue *shadowRayQueue,
                         SOA<PixelSampleState> *pixelSampleState) const;

    void IntersectShadowTr(int maxRays, ShadowRayQueue *shadowRayQueue,
                           SOA<PixelSampleState> *pixelSampleState) const;

    void IntersectOneRandom(int maxRays,
                            SubsurfaceScatterQueue *subsurfaceScatterQueue) const;

    // WAR: The enclosing parent function ("PreparePLYMeshes") for an
    // extended __device__ lambda cannot have private or protected access
    // within its class, so it's public...
    static std::map<int, TriQuadMesh> PreparePLYMeshes(
        const std::vector<ShapeSceneEntity> &shapes,
        const std::map<std::string, FloatTexture> &floatTextures);

    // Load (and quad->tri convert) PLY meshes without evaluating displacement
    // textures.  Pure CPU work with no dependency on NamedTextures, so it can
    // run on a background thread concurrently with texture creation.  Returns
    // the loaded meshes plus the indices of shapes whose "displacement"
    // parameter must be resolved later via ApplyPLYDisplacements().
    struct LoadedPlyMeshes {
        std::map<int, TriQuadMesh> meshes;
        std::vector<int> displacedIndices;
    };
    static LoadedPlyMeshes PreparePLYMeshesLoadOnly(
        const std::vector<ShapeSceneEntity> &shapes);

    // Applies displacement to the meshes recorded by PreparePLYMeshesLoadOnly.
    // Must be called after texture creation (needs floatTextures) and before
    // OptiXAggregate construction.  GPU-synchronous.
    static void ApplyPLYDisplacements(
        const std::vector<ShapeSceneEntity> &shapes,
        const std::vector<int> &displacedIndices,
        const std::map<std::string, FloatTexture> &floatTextures,
        std::map<int, TriQuadMesh> *meshes);

  private:
    struct HitgroupRecord;

    struct BVH {
        BVH() = default;
        BVH(size_t size);

        BVH(const BVH&) = delete;
        BVH& operator=(const BVH&) = delete;
        BVH(BVH&&);
        BVH& operator=(BVH&&);
        ~BVH();

        OptixTraversableHandle traversableHandle = {};
        std::vector<HitgroupRecord> intersectHGRecords;
        std::vector<HitgroupRecord> shadowHGRecords;
        std::vector<HitgroupRecord> randomHitHGRecords;
        Bounds3f bounds;
    };

    static BVH buildBVHForTriangles(
        const std::vector<ShapeSceneEntity> &shapes,
        const std::map<int, TriQuadMesh> &plyMeshes, OptixDeviceContext optixContext,
        const OptixProgramGroup &intersectPG, const OptixProgramGroup &shadowPG,
        const OptixProgramGroup &randomHitPG,
        const std::map<std::string, FloatTexture> &floatTextures,
        const std::map<std::string, Material> &namedMaterials,
        const std::vector<Material> &materials,
        const std::map<std::string, Medium> &media,
        const std::map<int, pstd::vector<Light> *> &shapeIndexToAreaLights,
        ThreadLocal<Allocator> &threadAllocators,
        ThreadLocal<cudaStream_t> &threadCUDAStreams);

    static BilinearPatchMesh *diceCurveToBLP(const ShapeSceneEntity &shape, int nDiceU,
                                             int nDiceV, Allocator alloc);

    static BVH buildBVHForBLPs(
        const std::vector<ShapeSceneEntity> &shapes, OptixDeviceContext optixContext,
        const OptixProgramGroup &intersectPG, const OptixProgramGroup &shadowPG,
        const OptixProgramGroup &randomHitPG,
        const std::map<std::string, FloatTexture> &floatTextures,
        const std::map<std::string, Material> &namedMaterials,
        const std::vector<Material> &materials,
        const std::map<std::string, Medium> &media,
        const std::map<int, pstd::vector<Light> *> &shapeIndexToAreaLights,
        ThreadLocal<Allocator> &threadAllocators,
        ThreadLocal<cudaStream_t> &threadCUDAStreams);

    static BVH buildBVHForQuadrics(
        const std::vector<ShapeSceneEntity> &shapes, OptixDeviceContext optixContext,
        const OptixProgramGroup &intersectPG, const OptixProgramGroup &shadowPG,
        const OptixProgramGroup &randomHitPG,
        const std::map<std::string, FloatTexture> &floatTextures,
        const std::map<std::string, Material> &namedMaterials,
        const std::vector<Material> &materials,
        const std::map<std::string, Medium> &media,
        const std::map<int, pstd::vector<Light> *> &shapeIndexToAreaLights,
        ThreadLocal<Allocator> &threadAllocators,
        ThreadLocal<cudaStream_t> &threadCUDAStreams);

    int addHGRecords(const BVH &bvh);

    static OptixModule createOptiXModule(OptixDeviceContext optixContext,
                                          const char *ptx);
    static OptixPipelineCompileOptions getPipelineCompileOptions();
    static OptixProgramGroup createRaygenPG(OptixDeviceContext optixContext,
                                            OptixModule optixModule,
                                            const char *entrypoint);
    static OptixProgramGroup createMissPG(OptixDeviceContext optixContext,
                                          OptixModule optixModule,
                                          const char *entrypoint);
    static OptixProgramGroup createIntersectionPG(OptixDeviceContext optixContext,
                                                  OptixModule optixModule,
                                                  const char *closest,
                                                  const char *any,
                                                  const char *intersect);

    static OptixTraversableHandle buildOptixBVH(
        OptixDeviceContext optixContext, const std::vector<OptixBuildInput> &buildInputs,
        ThreadLocal<cudaStream_t> &threadCUDAStreams);

    CUDATrackedMemoryResource *memoryResource;
    std::mutex boundsMutex;
    Bounds3f bounds;
    CUstream cudaStream;
    OptixDeviceContext optixContext;
    OptixModule optixModule;
    OptixPipeline optixPipeline;

    struct ParamBufferState {
        bool used = false;
        cudaEvent_t finishedEvent;
        CUdeviceptr ptr = 0;
        void *hostPtr = nullptr;
    };
    mutable std::vector<ParamBufferState> paramsPool;
    mutable size_t nextParamOffset = 0;

    ParamBufferState &getParamBuffer(const RayIntersectParameters &) const;

    pstd::vector<HitgroupRecord> intersectHGRecords;
    pstd::vector<HitgroupRecord> shadowHGRecords;
    pstd::vector<HitgroupRecord> randomHitHGRecords;
    OptixShaderBindingTable intersectSBT = {}, shadowSBT = {}, shadowTrSBT = {};
    OptixShaderBindingTable randomHitSBT = {};
    OptixTraversableHandle rootTraversable = {};
};

}  // namespace pbrt

#endif  // PBRT_GPU_AGGREGATE_H
