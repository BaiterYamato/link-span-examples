#pragma once

#include "PassSource.h"

#include <string>
#include <string_view>
#include <vector>

namespace ShaderPacks {

// Reduced Iris graph: the fragment programs deferred..deferred15, composite..composite15 and final, run in that
// order (up to 16 with the V3 bridge, 8 with V2). Each program writes the colortex buffers of its
// DRAWBUFFERS/RENDERTARGETS directive; colortex0 starts as the scene color and depthtex0/1/2 are the scene depth.
// gbuffers, shadow and compute programs are outside this subset.
struct IrisProgram {
    std::string name; // "composite", "composite3", "final"
    std::string file; // "shaders/composite3.fsh"
    std::string source;
    // The program's .vsh beside the .fsh, if the pack has one; it runs only with the V3 bridge.
    std::string vertexFile;
    std::string vertexSource;
};

// Iris' cloudHeight in OoT units: clouds above the Great Deku Tree canopy (~2400) in Kokiri Forest. Minecraft's is 192.
constexpr float DefaultIrisCloudHeight = 3000.0f;

// Fog calibration, LinkSpanIris vector 11 (y, z, w). k = blocks per OoT unit: the pack's fog, clouds and cave factor are
// written for blocks (1 block ~ 32 OoT units), so a smaller k pulls the game's distances into the pack's range. The
// defaults (since 0.7.6) bring the game to the pack's range: k = 1/32, no ground offset, open sky. k = 1 reproduces the
// pack unscaled (0.7.3).
constexpr float DefaultIrisFogDivisor = 32.0f; // the menu shows N, k = 1/N
constexpr float DefaultIrisFogScale = 1.0f / DefaultIrisFogDivisor;
constexpr float MinIrisFogScale = 1.0f / 256.0f;
constexpr float MaxIrisFogScale = 4.0f;
// k = 0 (iris.fog_divisor = 0) is automatic since 0.7.7: the generated GLSL derives k from the scene's native fog
// (PolicromaFog.w start, PolicromaIrisInfo.w end, PolicromaCameraInfo.xy zNear/zFar) so the pack's fog reaches 50% where
// the N64 fog does (d50 = 2 f S / (2 S + f - n)). k = clamp(sqrt(Coefficient f / d50^3), Min, Max); with f k below
// Saturation the pack's distance factor saturates and k = Fallback / d50. Any k above 0 is used as given.
constexpr float AutoIrisFogScale = 0.0f;
constexpr float MinAutoIrisFogScale = 1.0f / 64.0f;
constexpr float MaxAutoIrisFogScale = 1.0f / 4.0f;
constexpr float AutoFogCoefficient = 3610.1f;  // ln 2 / 192e-6
constexpr float AutoFogSaturation = 192.0f;
constexpr float AutoFogFallback = 88.51f;
constexpr float AutoFogMinDistance = 1.0f; // keeps d50 > 0 (fog starting at the camera) so the formula never divides by 0
// 0.7.8, bridge feature PolicromaFeatureFogRamp: the host writes the native fog ramp, smoothed, in the scale of
// gSPFogPosition (u, 0..1000) to LinkSpanIris clock.z (start) and clock.w (end). {0, 0}: no valid ramp, the legacy
// formula above stands. {0, 0 < w < FogRampTotalEnd}: total fog (the game's fogNear < 0), d50 collapses to the camera.
// End - start below FogRampMinSpan (fogNear 997..999, or {1000, 1000}): practically no fog, only the far edge, d = far.
// The distance of a u is d = n / max(1 - u (f - n) / (FogRampUnits f), n / f), with u = (start + end) / 2 at 50%.
constexpr float FogRampMinSpan = 3.0f;
constexpr float FogRampTotalEnd = 0.001f;
constexpr float FogRampUnits = 1000.0f;
constexpr float DefaultIrisGroundY = 0.0f;    // OoT height of the floor; 0: heights stay as the game has them
constexpr float DefaultIrisSkyLight = 240.0f; // eyeBrightness.y: 240 open air, 0 pitch dark
constexpr float MaxIrisSkyLight = 240.0f;
// iris.sky_light = -1 is automatic since 0.7.9 (bridge feature PolicromaFeatureSkyExposure): the generated GLSL derives the
// sky light from the host's sky openness, sky = mix(IndoorIrisSkyLight, MaxIrisSkyLight, openness), openness = b10 vector
// 12 ("sky") x, 0..1 (1 open sky, 0 indoors). Any value from 0 is manual and wins; without the feature -1 reads as open air.
constexpr float AutoIrisSkyLight = -1.0f;
constexpr float IndoorIrisSkyLight = 30.0f;

// What the host bridge offers. V3 (RFC 0035): 16 passes, 8 textures, 8 targets, scene.noise, zero, LinkSpanIris and
// Shader Model 5. Without it the graph keeps the V2 limits (8 passes, 4 textures, one target).
struct IrisHost {
    bool v3 = false;
    // Bridge feature PolicromaFeaturePackTextures: the manifest "textures" (the pack's own PNG files) and the repeat
    // sampler s1, for noisetex and the custom textures of shaders.properties.
    bool textures = false;
    // Bridge feature PolicromaFeatureProviderParameters: cloudHeight reads LinkSpanIris vector 11, set live by the
    // provider. Without it, cloudHeight is the constant below (a new value needs the pack reloaded).
    bool parameters = false;
    float cloudHeight = DefaultIrisCloudHeight;
    // Opt-in "reconstructed normals" (GbufferF0.h): a synthetic program before the pack's first one fills colortex4,
    // colortex6 and colortex3 from the depth, only for the ones a pack program reads. V3 only; off keeps the graph
    // exactly as it was without the option.
    bool syntheticGbuffer = false;
    // Bridge feature PolicromaFeatureFogRamp: the automatic k reads the host's fog ramp (LinkSpanIris clock.zw) and keeps
    // the 0.7.7 formula for the frames where the host sends {0, 0}. Without the feature the shader never reads clock.zw.
    bool fogRamp = false;
    // Bridge feature PolicromaFeatureSkyExposure: LinkSpanIris has a 13th vector (sky, x = openness) and sky_light = -1
    // reads it. Without the feature the cbuffer stays at 12 vectors, as the 0.7.8 text.
    bool skyExposure = false;
};

// A custom texture of shaders.properties: "texture.noise=<file>" (noisetex in every program) or
// "texture.<stage>.<buffer>=<file>", which replaces that colortex sampler in the programs of the stage (deferred:
// deferred*; composite: composite* and final). Other stages and resource locations ("minecraft:...") are ignored.
struct IrisTexture {
    std::string stage;  // "noise", "deferred", "composite"
    std::string buffer; // "colortex3", "gaux4"; empty for the noise
    std::string file;   // inside the pack: "shaders/lib/textures/noise.png"
    std::string bytes;  // the file, read by the catalog
    bool clamp = false; // the .mcmeta "clamp": true samples with the clamping sampler, otherwise it repeats
};

// The custom textures declared in shaders.properties (bytes empty), in declaration order. Preprocessor lines are
// skipped: every texture line counts, whatever condition surrounds it.
std::vector<IrisTexture> ParseIrisTextures(std::string_view properties);

// The "clamp" flag of a .mcmeta file (false when absent or unreadable, as Iris defaults it).
bool IrisTextureClamp(std::string_view mcmeta);

// A custom uniform of shaders.properties: "uniform.<type>.<name> = <expression>", or "variable.<type>.<name> = ..."
// (uniform false: only other expressions read it). type is float, int, bool, vec2, vec3 or vec4.
struct IrisCustomUniform {
    std::string type;
    std::string name;
    std::string expression;
    bool uniform = true;
};

// The custom uniforms and variables of shaders.properties, in declaration order (a later one with the same name
// replaces the earlier). Preprocessor lines are skipped like in ParseIrisTextures.
std::vector<IrisCustomUniform> ParseIrisUniforms(std::string_view properties);

// Iris program names this subset runs, in execution order.
std::vector<std::string> IrisProgramOrder();

// Candidate files for a program, in lookup order. OoT plays as the overworld: shaders/world0/ wins over the
// pack root, as Iris does for a dimension folder, and a program missing there falls back to the root.
std::vector<std::string> IrisProgramFiles(const std::string& name);

// Pure part, shared with the tests. Expands the includes, rewrites each program into Vulkan GLSL that
// TranslateGlslFragment accepts (glsl, aligned with the passes of the manifest) and builds the Link-Span manifest
// (version 3 with the V3 bridge, else 2) that wires each colortex to the pass output that last wrote it.
// An opt-in material seed is returned as internal HLSL (manifest file ending in .hlsl).
// vertexGlsl, aligned with glsl, receives the rewritten .vsh of each pass (empty: the host's fullscreen shader);
// the manifest names it in the pass "vertex" field, for TranslateGlslProgram.
bool BuildIrisGraph(const std::vector<IrisProgram>& programs, const IncludeFiles& includes,
                    std::vector<std::string>& glsl, std::string& manifest, std::string& error, IrisHost host = {},
                    std::vector<std::string>* vertexGlsl = nullptr, const std::vector<IrisTexture>& textures = {},
                    const std::vector<IrisCustomUniform>& uniforms = {}, std::string_view properties = {});

// The vertex shader file of a program file: the .vsh beside its .fsh.
std::string IrisVertexFile(const std::string& fragmentFile);

} // namespace ShaderPacks
