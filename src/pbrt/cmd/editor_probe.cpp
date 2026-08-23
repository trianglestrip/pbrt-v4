// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0
//
// Track A probe: build a SMALL synthetic World (one Model referencing a named
// material + one texture; relying on World defaults for camera/light/film),
// run EmitScene into a counting ParserTarget, and print the event counts.
// Confirms a model with NO camera/light still yields a complete, renderable
// event stream (Camera/Light present via defaults).

#include <pbrt/editor/parser_target_adapter.h>
#include <pbrt/editor/scene_graph.h>
#include <pbrt/parser.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace pbrt;
using namespace pbrt::editor;

namespace {

// Minimal ParserTarget that just counts events.
class CountingParserTarget : public ParserTarget {
  public:
    void note(const char *name) { ++counts[name]; }

    void Scale(Float, Float, Float, FileLoc) override { note("Scale"); }
    void Shape(const std::string &, ParsedParameterVector, FileLoc) override { note("Shape"); }
    void Option(const std::string &, const std::string &, FileLoc) override { note("Option"); }
    void Identity(FileLoc) override { note("Identity"); }
    void Translate(Float, Float, Float, FileLoc) override { note("Translate"); }
    void Rotate(Float, Float, Float, Float, FileLoc) override { note("Rotate"); }
    void LookAt(Float, Float, Float, Float, Float, Float, Float, Float, Float, FileLoc) override {
        note("LookAt");
    }
    void ConcatTransform(Float[16], FileLoc) override { note("ConcatTransform"); }
    void Transform(Float[16], FileLoc) override { note("Transform"); }
    void CoordinateSystem(const std::string &, FileLoc) override { note("CoordinateSystem"); }
    void CoordSysTransform(const std::string &, FileLoc) override { note("CoordSysTransform"); }
    void ActiveTransformAll(FileLoc) override { note("ActiveTransformAll"); }
    void ActiveTransformEndTime(FileLoc) override { note("ActiveTransformEndTime"); }
    void ActiveTransformStartTime(FileLoc) override { note("ActiveTransformStartTime"); }
    void TransformTimes(Float, Float, FileLoc) override { note("TransformTimes"); }
    void ColorSpace(const std::string &, FileLoc) override { note("ColorSpace"); }
    void PixelFilter(const std::string &, ParsedParameterVector, FileLoc) override {
        note("PixelFilter");
    }
    void Film(const std::string &, ParsedParameterVector, FileLoc) override { note("Film"); }
    void Accelerator(const std::string &, ParsedParameterVector, FileLoc) override {
        note("Accelerator");
    }
    void Integrator(const std::string &, ParsedParameterVector, FileLoc) override {
        note("Integrator");
    }
    void Camera(const std::string &, ParsedParameterVector, FileLoc) override { note("Camera"); }
    void MakeNamedMedium(const std::string &, ParsedParameterVector, FileLoc) override {
        note("MakeNamedMedium");
    }
    void MediumInterface(const std::string &, const std::string &, FileLoc) override {
        note("MediumInterface");
    }
    void Sampler(const std::string &, ParsedParameterVector, FileLoc) override { note("Sampler"); }
    void WorldBegin(FileLoc) override { note("WorldBegin"); }
    void AttributeBegin(FileLoc) override { note("AttributeBegin"); }
    void AttributeEnd(FileLoc) override { note("AttributeEnd"); }
    void Attribute(const std::string &, ParsedParameterVector, FileLoc) override {
        note("Attribute");
    }
    void Texture(const std::string &, const std::string &, const std::string &,
                 ParsedParameterVector, FileLoc) override {
        note("Texture");
    }
    void Material(const std::string &, ParsedParameterVector, FileLoc) override {
        note("Material");
    }
    void MakeNamedMaterial(const std::string &, ParsedParameterVector, FileLoc) override {
        note("MakeNamedMaterial");
    }
    void NamedMaterial(const std::string &, FileLoc) override { note("NamedMaterial"); }
    void LightSource(const std::string &, ParsedParameterVector, FileLoc) override {
        note("LightSource");
    }
    void AreaLightSource(const std::string &, ParsedParameterVector, FileLoc) override {
        note("AreaLightSource");
    }
    void ReverseOrientation(FileLoc) override { note("ReverseOrientation"); }
    void ObjectBegin(const std::string &, FileLoc) override { note("ObjectBegin"); }
    void ObjectEnd(FileLoc) override { note("ObjectEnd"); }
    void ObjectInstance(const std::string &, FileLoc) override { note("ObjectInstance"); }
    void EndOfFiles() override { note("EndOfFiles"); }

    std::map<std::string, size_t> counts;
};

World BuildSyntheticWorld() {
    World world;

    // One texture referenced by the model's material.
    Texture tex;
    tex.name = "baseColorTex";
    tex.texType = "spectrum";
    tex.texname = "constant";
    Param texColor;
    texColor.type = "rgb";
    texColor.name = "color";
    texColor.floats = {0.9f, 0.1f, 0.1f};
    tex.params.push_back(texColor);

    // One model: a sphere, referencing a named material + the texture.
    auto model = std::make_unique<Model>("sphere1");
    model->shapeType = "sphere";
    Param radius;
    radius.type = "float";
    radius.name = "radius";
    radius.floats = {1.f};
    model->shapeParams.push_back(radius);

    model->namedMaterial = "red-mat";
    // Define the named material so the stream is self-contained / renderable.
    model->materialType = "diffuse";
    Param reflectance;
    reflectance.type = "texture";
    reflectance.name = "reflectance";
    reflectance.strings = {"baseColorTex"};
    model->materialParams.push_back(reflectance);
    model->textures.push_back(tex);

    world.models.push_back(std::move(model));

    // No camera / light / film supplied -> World defaults fill them in.
    return world;
}

}  // namespace

int main() {
    World world = BuildSyntheticWorld();

    CountingParserTarget target;
    EmitScene(world, &target);

    // Print the asset-bearing event counts the render binder cares about.
    const char *want[] = {"WorldBegin",  "Texture",         "Shape",
                          "Material",    "MakeNamedMaterial", "NamedMaterial",
                          "LightSource", "AreaLightSource", "Camera",
                          "Film",        "Sampler",          "Integrator",
                          "EndOfFiles"};
    fprintf(stderr, "Track A editor_probe: event counts\n");
    for (const char *w : want)
        if (target.counts.count(w))
            fprintf(stderr, "  %-18s %zu\n", w, target.counts[w]);

    // Sanity assertions: a renderable stream must contain a camera, a light,
    // and the model's geometry + material reference.
    bool ok = target.counts.count("WorldBegin") && target.counts["WorldBegin"] == 1 &&
              target.counts.count("Camera") && target.counts["Camera"] == 1 &&
              target.counts.count("LightSource") && target.counts["LightSource"] >= 1 &&
              target.counts.count("Shape") && target.counts["Shape"] == 1 &&
              target.counts.count("MakeNamedMaterial") == 1 &&
              target.counts.count("NamedMaterial") == 1 &&
              target.counts.count("Texture") == 1 &&
              target.counts.count("EndOfFiles") && target.counts["EndOfFiles"] == 1;

    fprintf(stderr, "result: %s\n", ok ? "OK (model w/o camera/light renders via defaults)" : "FAIL");
    return ok ? 0 : 1;
}
