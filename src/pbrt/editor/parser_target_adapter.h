// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0
//
// Track A: scene graph -> ParserTarget event adapter.  Walks a World and emits
// the same event set the real parser produces.  Executor-agnostic: no threads,
// no GPU.  See parser_target_adapter.cpp.

#ifndef PBRT_EDITOR_PARSER_TARGET_ADAPTER_H
#define PBRT_EDITOR_PARSER_TARGET_ADAPTER_H

#include <pbrt/editor/scene_graph.h>
#include <pbrt/parser.h>

namespace pbrt {
namespace editor {

// Emit the full event stream for `world` into `target`, mirroring the event set
// the real pbrt parser produces (WorldBegin; Texture/MakeNamedMaterial/
// NamedMaterial/Shape per model; Camera/Film/Sampler/Integrator; LightSource/
// AreaLightSource; EndOfFiles).  Missing scene-level directives are taken from
// World defaults so a model with no camera/light still yields a complete,
// renderable stream.
void EmitScene(const World &world, ParserTarget *target);

}  // namespace editor
}  // namespace pbrt

#endif  // PBRT_EDITOR_PARSER_TARGET_ADAPTER_H
