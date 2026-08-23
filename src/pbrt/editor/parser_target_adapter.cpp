// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0
//
// Track A: scene graph -> ParserTarget event adapter.

#include <pbrt/editor/parser_target_adapter.h>

#include <pbrt/paramdict.h>

#include <memory>
#include <vector>

namespace pbrt {
namespace editor {

namespace {

// Convert a neutral ParamList into a real ParsedParameterVector, keeping the
// backing ParsedParameter objects alive in `storage`.
ParsedParameterVector BuildParams(const ParamList &params, FileLoc loc,
                                  std::vector<std::unique_ptr<ParsedParameter>> &storage) {
    ParsedParameterVector ppv;
    for (const Param &p : params) {
        auto pp = std::make_unique<ParsedParameter>(loc);
        pp->type = p.type;
        pp->name = p.name;
        if (p.type == "integer") {
            for (int v : p.ints) pp->AddInt(v);
        } else if (p.type == "bool") {
            for (uint8_t v : p.bools) pp->AddBool(v != 0);
        } else if (p.type == "string" || p.type == "texture") {
            for (const std::string &s : p.strings) pp->AddString(s);
        } else {  // "float" | "rgb" | "color" | "point3" | "vector3" | ... -> floats
            for (Float v : p.floats) pp->AddFloat(v);
        }
        ppv.push_back(pp.get());
        storage.push_back(std::move(pp));
    }
    return ppv;
}

}  // namespace

void EmitScene(const World &world, ParserTarget *target) {
    FileLoc loc;  // editor-generated events have no file location
    std::vector<std::unique_ptr<ParsedParameter>> storage;

    // --- Scene-level directives: Camera / Film / Sampler / Integrator ---
    // Emitted before WorldBegin so the render binder collects them.  These come
    // from the World graph or its defaults (never missing -> no ErrorExit).
    const Camera &cam = world.GetCamera();
    target->LookAt(cam.eye.x, cam.eye.y, cam.eye.z, cam.look.x, cam.look.y,
                   cam.look.z, cam.up.x, cam.up.y, cam.up.z, loc);
    target->Camera(cam.cameraType,
                   BuildParams(cam.cameraParams, loc, storage), loc);

    const Film &film = world.GetFilm();
    target->Film(film.filmType, BuildParams(film.filmParams, loc, storage), loc);

    const Sampler &sampler = world.GetSampler();
    target->Sampler(sampler.samplerType,
                    BuildParams(sampler.samplerParams, loc, storage), loc);

    const Integrator &integrator = world.GetIntegrator();
    target->Integrator(integrator.integratorType,
                       BuildParams(integrator.integratorParams, loc, storage), loc);

    // --- World ---
    target->WorldBegin(loc);

    // Lights (default InfiniteLight if the world has none).
    for (const auto &light : world.GetLights()) {
        if (light->IsArea())
            target->AreaLightSource(light->GetLightType(),
                                    BuildParams(light->GetLightParams(), loc, storage), loc);
        else
            target->LightSource(light->GetLightType(),
                                BuildParams(light->GetLightParams(), loc, storage), loc);
    }

    // Models: each model is emitted inside its own attribute scope.  Forward
    // references are allowed (a Shape may reference a NamedMaterial declared
    // later); we emit in a sensible, complete order.
    for (const auto &model : world.models) {
        target->AttributeBegin(loc);

        // Textures referenced by this model's material/shape.
        for (const Texture &tex : model->GetTextures())
            target->Texture(tex.name, tex.texType, tex.texname,
                            BuildParams(tex.params, loc, storage), loc);

        // Material: an inline material is declared as a named material (using
        // the referenced name if provided, otherwise a generated one) and then
        // referenced by the shape via NamedMaterial.  This mirrors how the real
        // parser pairs MakeNamedMaterial + NamedMaterial per shape.  Forward
        // references are allowed (the declaration may arrive after use).
        if (!model->GetMaterialType().empty()) {
            std::string declName = model->GetNamedMaterial();
            if (declName.empty())
                declName = model->GetName().empty()
                               ? std::string("mat") + std::to_string(size_t(model.get()))
                               : model->GetName() + "-mat";
            target->MakeNamedMaterial(
                declName, BuildParams(model->GetMaterialParams(), loc, storage), loc);
            target->NamedMaterial(declName, loc);
        } else if (!model->GetNamedMaterial().empty()) {
            target->NamedMaterial(model->GetNamedMaterial(), loc);
        }

        // Shape.
        const std::string &shapeType = model->GetShapeType();
        if (!shapeType.empty())
            target->Shape(shapeType, BuildParams(model->GetShapeParams(), loc, storage), loc);

        target->AttributeEnd(loc);
    }

    target->EndOfFiles();
}

}  // namespace editor
}  // namespace pbrt
