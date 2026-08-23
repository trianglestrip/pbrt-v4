// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0
//
// Track A (editor scene graph).  A neutral, executor-agnostic scene graph
// whose optional fields are exposed through virtual getters that return
// DEFAULT values.  Concrete entities only override the fields they actually
// carry, so a Model with no camera/light still renders via World defaults.

#ifndef PBRT_EDITOR_SCENE_GRAPH_H
#define PBRT_EDITOR_SCENE_GRAPH_H

#include <pbrt/pbrt.h>
#include <pbrt/util/vecmath.h>

#include <memory>
#include <string>
#include <vector>

namespace pbrt {
namespace editor {

// A single typed pbrt parameter.  Stored in a neutral form so the scene graph
// stays self-contained; the ParserTarget adapter converts these into real
// ParsedParameter objects at emit time.
struct Param {
    std::string type;    // "float" | "integer" | "string" | "bool" | "texture"
    std::string name;
    std::vector<Float> floats;
    std::vector<int> ints;
    std::vector<std::string> strings;
    std::vector<uint8_t> bools;
};

using ParamList = std::vector<Param>;

// A texture referenced by a model's material (or directly by a shape).
struct Texture {
    std::string name;     // named reference used by materials/shapes
    std::string texType;  // "float" | "spectrum"
    std::string texname;  // "imagemap" | "constant" | ...
    ParamList params;
};

// SceneNode Base Class
// Optional fields are returned by virtual getters that return DEFAULT values.
// Subclasses override only the fields they actually carry.
class SceneNode {
  public:
    SceneNode() = default;
    explicit SceneNode(std::string name) : name(std::move(name)) {}
    virtual ~SceneNode() = default;

    // Optional field getters -- defaults are neutral / empty.
    virtual std::string GetName() const { return name; }
    virtual std::string GetShapeType() const { return ""; }
    virtual const ParamList &GetShapeParams() const { return emptyParams; }
    virtual std::string GetMaterialType() const { return ""; }
    virtual const ParamList &GetMaterialParams() const { return emptyParams; }
    virtual std::string GetNamedMaterial() const { return ""; }
    virtual const std::vector<Texture> &GetTextures() const { return noTextures; }

  protected:
    std::string name;
    static const ParamList emptyParams;
    static const std::vector<Texture> noTextures;
};

// Model: geometry + material + textures.  It MUST NOT contain camera or light
// members -- those are scene-level and supplied by World defaults when absent.
class Model : public SceneNode {
  public:
    using SceneNode::SceneNode;

    std::string shapeType;
    ParamList shapeParams;
    // Material: either an inline material (materialType + materialParams) or a
    // reference to a named material (namedMaterial).
    std::string materialType;
    ParamList materialParams;
    std::string namedMaterial;
    std::vector<Texture> textures;

    std::string GetShapeType() const override { return shapeType; }
    const ParamList &GetShapeParams() const override { return shapeParams; }
    std::string GetMaterialType() const override { return materialType; }
    const ParamList &GetMaterialParams() const override { return materialParams; }
    std::string GetNamedMaterial() const override { return namedMaterial; }
    const std::vector<Texture> &GetTextures() const override { return textures; }
};

// Light (scene level).  isArea selects AreaLightSource vs LightSource.
class Light : public SceneNode {
  public:
    using SceneNode::SceneNode;
    std::string lightType;
    ParamList lightParams;
    bool isArea = false;

    std::string GetLightType() const { return lightType; }
    const ParamList &GetLightParams() const { return lightParams; }
    bool IsArea() const { return isArea; }
};

// Camera (scene level).  The transform is expressed as eye/look/up so the
// adapter can emit a LookAt event.
class Camera : public SceneNode {
  public:
    using SceneNode::SceneNode;
    std::string cameraType;
    ParamList cameraParams;
    Point3f eye = Point3f(0, 0, 5);
    Point3f look = Point3f(0, 0, 0);
    Vector3f up = Vector3f(0, 1, 0);

    std::string GetCameraType() const { return cameraType; }
    const ParamList &GetCameraParams() const { return cameraParams; }
};

// Film / Sampler / Integrator (scene level).
class Film : public SceneNode {
  public:
    using SceneNode::SceneNode;
    std::string filmType;
    ParamList filmParams;
};

class Sampler : public SceneNode {
  public:
    using SceneNode::SceneNode;
    std::string samplerType;
    ParamList samplerParams;
};

class Integrator : public SceneNode {
  public:
    using SceneNode::SceneNode;
    std::string integratorType;
    ParamList integratorParams;
};

// World: aggregates Models + Lights and supplies default Camera/Light/Film/
// Sampler/Integrator when its children omit them.
class World {
  public:
    std::vector<std::unique_ptr<Model>> models;
    std::vector<std::unique_ptr<Light>> lights;

    // Optional scene-level directives.  When null, defaults are supplied by the
    // accessors below.
    std::unique_ptr<Camera> camera;
    std::unique_ptr<Film> film;
    std::unique_ptr<Sampler> sampler;
    std::unique_ptr<Integrator> integrator;

    const Camera &GetCamera() const;
    const Film &GetFilm() const;
    const Sampler &GetSampler() const;
    const Integrator &GetIntegrator() const;
    const std::vector<std::unique_ptr<Light>> &GetLights() const;

    static Camera MakeDefaultCamera();
    static Film MakeDefaultFilm();
    static Sampler MakeDefaultSampler();
    static Integrator MakeDefaultIntegrator();
    static Light MakeDefaultLight();
};

}  // namespace editor
}  // namespace pbrt

#endif  // PBRT_EDITOR_SCENE_GRAPH_H
