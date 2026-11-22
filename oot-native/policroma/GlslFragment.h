#pragma once

#include "PassSource.h"

namespace ShaderPacks {

// Replaces the fragment source only after a complete translation.
bool TranslateGlslFragment(PassSource& pass, const IncludeFiles& includes, const std::string& preamble,
                           std::string& error);

// A pass with its own vertex shader (manifest v3 "vertex"): pass.source is the fragment GLSL and pass.vertexSource
// the vertex GLSL, already self-contained. Both link as one program; on success they are replaced by the pixel and
// vertex HLSL, whose entry points share one varying struct so the host's linkage check holds.
bool TranslateGlslProgram(PassSource& pass, std::string& error);

// Runs only the glslang preprocessor (no includes) on a fragment source, as GLSL 450. Comments are dropped.
bool PreprocessGlsl(const std::string& source, const std::string& name, std::string& output, std::string& error);

} // namespace ShaderPacks
