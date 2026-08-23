// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

// Probe: call pbrt's parser library directly with a custom ParserTarget to
// verify the parser is event-driven (fires per-asset callbacks) and to log
// the event stream (counts + ordering) for a scene.  This is the seam that an
// "assetstream" / "assetpack" rendering binder would hook into.

#include <pbrt/pbrt.h>
#include <pbrt/options.h>
#include <pbrt/parser.h>
#include <pbrt/util/parallel.h>

#include <cstdio>
#include <map>
#include <string>

using namespace pbrt;

namespace {

// Minimal ParserTarget that just logs every parser event.  Confirms the parser
// is directly callable and emits per-asset callbacks (Texture/Shape/Material/
// LightSource/...) in file order, including any forward references.
class LoggingParserTarget : public ParserTarget {
  public:
    void note(const char *name) {
        ++total;
        if (firstSeen.empty())
            firstSeen = name;
        ++counts[name];
    }

    // ParserTarget Interface (all events)
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

    size_t total = 0;
    std::string firstSeen;
    std::map<std::string, size_t> counts;
};

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scene.pbrt>\n", argv[0]);
        return 1;
    }
    PBRTOptions options;
    options.useGPU = false;
    InitPBRT(options);

    LoggingParserTarget target;
    std::vector<std::string> files = {argv[1]};
    auto t0 = std::chrono::high_resolution_clock::now();
    ParseFiles(&target, files);
    auto t1 = std::chrono::high_resolution_clock::now();

    fprintf(stderr, "parse events: %zu  (first=%s)  %.3f s\n", target.total,
            target.firstSeen.c_str(),
            std::chrono::duration<double>(t1 - t0).count());
    // Print counts for the asset-bearing events (the ones a render binder cares
    // about) plus the structural markers.
    const char *want[] = {"WorldBegin", "Texture",   "Shape",        "Material",
                           "MakeNamedMaterial", "NamedMaterial", "LightSource",
                           "AreaLightSource", "ObjectInstance", "EndOfFiles"};
    for (const char *w : want)
        if (target.counts.count(w))
            fprintf(stderr, "  %-18s %zu\n", w, target.counts[w]);

    CleanupPBRT();
    return 0;
}
