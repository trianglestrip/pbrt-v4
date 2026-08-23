// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0
//
// Track A scene graph implementation (see scene_graph.h).

#include <pbrt/editor/scene_graph.h>

namespace pbrt {
namespace editor {

const ParamList SceneNode::emptyParams{};
const std::vector<Texture> SceneNode::noTextures{};

const Camera &World::GetCamera() const {
    if (camera) return *camera;
    static const Camera def = MakeDefaultCamera();
    return def;
}

const Film &World::GetFilm() const {
    if (film) return *film;
    static const Film def = MakeDefaultFilm();
    return def;
}

const Sampler &World::GetSampler() const {
    if (sampler) return *sampler;
    static const Sampler def = MakeDefaultSampler();
    return def;
}

const Integrator &World::GetIntegrator() const {
    if (integrator) return *integrator;
    static const Integrator def = MakeDefaultIntegrator();
    return def;
}

const std::vector<std::unique_ptr<Light>> &World::GetLights() const {
    if (!lights.empty()) return lights;
    static const std::vector<std::unique_ptr<Light>> def = [] {
        std::vector<std::unique_ptr<Light>> v;
        v.push_back(std::make_unique<Light>(MakeDefaultLight()));
        return v;
    }();
    return def;
}

Camera World::MakeDefaultCamera() {
    Camera c("default-camera");
    c.cameraType = "perspective";
    c.eye = Point3f(0, 0, 5);
    c.look = Point3f(0, 0, 0);
    c.up = Vector3f(0, 1, 0);
    Param fov;
    fov.type = "float";
    fov.name = "fov";
    fov.floats = {40.f};
    c.cameraParams.push_back(fov);
    return c;
}

Film World::MakeDefaultFilm() {
    Film f("default-film");
    f.filmType = "rgb";
    Param res;
    res.type = "integer";
    res.name = "resolution";
    res.ints = {640, 360};
    f.filmParams.push_back(res);
    return f;
}

Sampler World::MakeDefaultSampler() {
    Sampler s("default-sampler");
    s.samplerType = "pcf";
    Param spp;
    spp.type = "integer";
    spp.name = "pixelsamples";
    spp.ints = {16};
    s.samplerParams.push_back(spp);
    return s;
}

Integrator World::MakeDefaultIntegrator() {
    Integrator i("default-integrator");
    i.integratorType = "path";
    return i;
}

Light World::MakeDefaultLight() {
    Light l("default-light");
    l.lightType = "infinite";
    l.isArea = false;
    Param scale;
    scale.type = "float";
    scale.name = "scale";
    scale.floats = {1.f};
    l.lightParams.push_back(scale);
    return l;
}

}  // namespace editor
}  // namespace pbrt
