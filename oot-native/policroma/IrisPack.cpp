#include "IrisPack.h"
#include "GbufferF0.h"
#include "GlslFragment.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace ShaderPacks {
namespace {

// Iris expands includes textually before preprocessing, so packs with include guards still inline the same file many
// times; Complementary programs reach a few hundred KB.
constexpr size_t MaxExpandedBytes = 4 * 1024 * 1024;
constexpr int DepthTexture = -1;
constexpr int NoiseTexture = -2;
// Iris shadow maps are not produced yet: they read the constant "one" input, that is, everything lit.
constexpr int ShadowTexture = -3;
constexpr std::string_view MaterialProperty = "linkspan.materialGbuffer";
constexpr std::string_view MaterialSeedName = "linkspan-material";
// The F0 gbuffer program (GbufferF0.h): a name and file of its own, absent from any ZIP like the material seed.
constexpr std::string_view GbufferF0Name = "linkspan-gbuffer-f0";
constexpr std::string_view GbufferF0File = "shaders/linkspan-gbuffer-f0.fsh";
// colortex it can fill, in the order of its outputs; a buffer is filled only when a pack program reads it.
constexpr int GbufferF0Normal = 4;      // world normal
constexpr int GbufferF0Material = 6;    // smoothness, material mask, sky light
constexpr int GbufferF0Translucent = 3; // translucentMult
constexpr auto MaterialSeedHlsl = R"(
Texture2D<float4> MaterialNormal : register(t0);
Texture2D<float4> MaterialSpecular : register(t1);
Texture2D<float> SceneDepth : register(t2);

struct MaterialOutputs {
    float4 normal : SV_Target0;
    float4 specular : SV_Target1;
};

MaterialOutputs main(float4 pixel : SV_Position) {
    const int3 address = int3(int2(pixel.xy), 0);
    const float4 normal = MaterialNormal.Load(address);
    const float depth = SceneDepth.Load(address);
    MaterialOutputs output = (MaterialOutputs)0;
    if (normal.a >= 1.0 && abs((normal.a - 1.0) - depth) <= 1e-6) {
        output.normal = float4(normal.rgb, 1.0);
        output.specular = MaterialSpecular.Load(address);
    }
    return output;
}
)";
// GLSL bindings. Textures use the pass input slot (0..7); the shared sampler sits at 16 so it never shares a
// binding with a texture. The host constant buffers have bindings of their own (8, 9 and 10).
constexpr int SamplerBinding = 16;
constexpr int WrapSamplerBinding = 17; // host s1 (PolicromaFeaturePackTextures)
constexpr int CameraBinding = 8;
constexpr int SceneBinding = 9;
constexpr int IrisBinding = 10;

struct SamplerAlias {
    std::string_view name;
    int colortex;
};

constexpr SamplerAlias Samplers[] = {
    { "colortex0", 0 },   { "colortex1", 1 },   { "colortex2", 2 },   { "colortex3", 3 },
    { "colortex4", 4 },   { "colortex5", 5 },   { "colortex6", 6 },   { "colortex7", 7 },
    { "colortex8", 8 },   { "colortex9", 9 },   { "colortex10", 10 }, { "colortex11", 11 },
    { "colortex12", 12 }, { "colortex13", 13 }, { "colortex14", 14 }, { "colortex15", 15 },
    { "gcolor", 0 },      { "gdepth", 1 },      { "gnormal", 2 },     { "composite", 3 },
    { "gaux1", 4 },       { "gaux2", 5 },       { "gaux3", 6 },       { "gaux4", 7 },
    { "depthtex0", DepthTexture }, { "depthtex1", DepthTexture }, { "depthtex2", DepthTexture },
    { "gdepthtex", DepthTexture }, { "noisetex", NoiseTexture },
    { "shadowtex0", ShadowTexture }, { "shadowtex1", ShadowTexture }, { "shadowcolor0", ShadowTexture },
    { "shadowcolor1", ShadowTexture }, { "shadowcolor", ShadowTexture }, { "watershadow", ShadowTexture },
    { "shadow", ShadowTexture },
};

enum Needs : unsigned {
    NeedSize = 1,
    NeedScene = 2,
    NeedCamera = 4,
    NeedIris = 8,       // LinkSpanIris (b10): V3 only.
    NeedProjection = 16, // the projection with the host aspect fix (from LinkSpanIris on V3)
    NeedWater = 32,
    NeedFog = 64, // gl_Fog of the compatibility profile
    NeedParameters = 128, // LinkSpanIris vector 11 (bridge feature PolicromaFeatureProviderParameters)
    NeedScale = 256,      // OoT units -> Minecraft blocks (policroma_scale, policroma_ground); k = 1 without the parameters
    NeedSky = 512,        // policroma_sky (bridge feature PolicromaFeatureSkyExposure; sky_light = -1 is automatic)
};

// The pack was written for blocks: its fog, clouds and cave factor assume the ground near y = 64. With a ground offset
// the camera and the clouds are moved so the OoT floor lands on this height; 0 leaves the heights as the game has them.
constexpr int GroundAnchorBlocks = 64;

struct UniformAlias {
    std::string_view name;
    std::string_view expression;
    unsigned needs;
    std::string_view type = {}; // empty: any scalar (float, int, uint)
};

// gl_ProjectionMatrix of an Iris composite pass: maps the [0, 1] quad to clip space, z = 0 to -1 (the vertex
// translation moves clip z to D3D's [0, w]).
#define PolicromaOrtho "mat4(2.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, -2.0, 0.0, -1.0, -1.0, -1.0, 1.0)"

// OoT runs its game logic at 20 Hz; PolicromaSceneData[0] is (scene, room, gameplay frames, day fraction) with the day
// fraction 0 at midnight, while Minecraft's worldTime is 0 at sunrise (06:00) and 18000 at midnight. Iris' sunAngle
// is 0 at sunrise and 0.25 at noon.
constexpr UniformAlias Uniforms[] = {
    { "viewWidth", "policroma_view_size.x", NeedSize },
    { "viewHeight", "policroma_view_size.y", NeedSize },
    { "aspectRatio", "(policroma_view_size.x / policroma_view_size.y)", NeedSize },
    // V3: the render clock of LinkSpanIris (one step per drawn frame, as Iris). Without it, gameplay frames (20 Hz).
    { "frameTimeCounter", "PolicromaIrisClock.y", NeedIris },
    { "frameCounter", "PolicromaIrisClock.x", NeedIris },
    { "frameTimeCounter", "(PolicromaSceneData[0].z * 0.05)", NeedScene },
    { "frameCounter", "PolicromaSceneData[0].z", NeedScene },
    { "worldTime", "mod(PolicromaSceneData[0].w * 24000.0 + 18000.0, 24000.0)", NeedScene },
    { "sunAngle", "fract(PolicromaSceneData[0].w - 0.25)", NeedScene },
    { "shadowAngle", "mod(fract(PolicromaSceneData[0].w - 0.25), 0.5)", NeedScene },
    { "near", "(PolicromaCameraInfo.x * policroma_scale)", NeedCamera | NeedScale },
    { "far", "(PolicromaCameraInfo.y * policroma_scale)", NeedCamera | NeedScale },
    // The host matrices are the game's GL-style ones (view space looks down -Z, NDC z in [-1, 1], depth =
    // ndc * 0.5 + 0.5), stored row-vector; read as GLSL column-major mat4 they act as M * v, like Iris expects.
    // Minecraft's gbufferModelView has no translation (player space is relative to the camera), so the view
    // rotation goes there and the eye goes to cameraPosition.
    // With the scale k the pack's view space is in blocks: the inverse projection scales its result by k and the
    // projection takes the blocks back to OoT units, so reconstruction and depth linearisation stay consistent.
    { "gbufferProjection", "(policroma_projection * policroma_unscale_matrix)", NeedCamera | NeedProjection | NeedScale,
      "mat4" },
    { "gbufferProjectionInverse", "(policroma_scale_matrix * inverse(policroma_projection))",
      NeedCamera | NeedProjection | NeedScale, "mat4" },
    { "gbufferModelView", "mat4(mat3(PolicromaView))", NeedCamera, "mat4" },
    { "gbufferModelViewInverse", "inverse(mat4(mat3(PolicromaView)))", NeedCamera, "mat4" },
    { "cameraPosition", "policroma_virtual_position(policroma_camera_position)", NeedCamera | NeedScale, "vec3" },
    { "eyeAltitude", "policroma_virtual_y(policroma_camera_position.y)", NeedCamera | NeedScale },
    // Iris' view-space light positions are 100 units long.
    { "upPosition", "(mat3(PolicromaView) * vec3(0.0, 100.0, 0.0))", NeedCamera, "vec3" },
    { "sunPosition", "(mat3(PolicromaView) * policroma_sun_direction * 100.0)", NeedCamera | NeedScene, "vec3" },
    { "moonPosition", "(mat3(PolicromaView) * policroma_sun_direction * -100.0)", NeedCamera | NeedScene, "vec3" },
    { "shadowLightPosition",
      "(mat3(PolicromaView) * policroma_sun_direction * (PolicromaSceneData[1].y >= 0.0 ? 100.0 : -100.0))",
      NeedCamera | NeedScene, "vec3" },
    { "isEyeInWater", "policroma_eye_in_water()", NeedCamera | NeedScene | NeedWater },
    { "gbufferPreviousProjection", "(PolicromaPreviousProjection * policroma_unscale_matrix)", NeedIris | NeedScale,
      "mat4" },
    { "gbufferPreviousModelView", "mat4(mat3(PolicromaPreviousView))", NeedIris, "mat4" },
    { "previousCameraPosition", "policroma_virtual_position(policroma_previous_camera_position)", NeedIris | NeedScale,
      "vec3" },
    { "frameTime", "PolicromaIrisInfo.y", NeedIris },
    { "fogColor", "PolicromaFog.rgb", NeedIris, "vec3" },
    { "skyColor", "PolicromaFog.rgb", NeedIris, "vec3" },
    { "fogStart", "(PolicromaFog.w * policroma_scale)", NeedIris | NeedScale },
    { "fogEnd", "(PolicromaIrisInfo.w * policroma_scale)", NeedIris | NeedScale },
    // Minecraft's cloud height (world y): live from the provider parameters, else a constant of the activation.
    { "cloudHeight", "policroma_virtual_y(PolicromaIrisParameters.x)", NeedIris | NeedParameters | NeedScale },
    { "cloudHeight", "policroma_cloud_height", 0 },
    // Sky light of the F0 gbuffer program (0..1): the provider's eyeBrightness sky (vector 11 .w, 0..240) or open sky.
    { "linkspanSkyLight", "(policroma_sky / 240.0)", NeedIris | NeedParameters | NeedSky },
    { "linkspanSkyLight", "(PolicromaIrisParameters.w / 240.0)", NeedIris | NeedParameters },
    { "linkspanSkyLight", "1.0", 0 },
    // Iris 1.7 split camera positions and the player's eye: OoT has no separate player camera.
    { "cameraPositionFract", "fract(policroma_virtual_position(policroma_camera_position))", NeedCamera | NeedScale,
      "vec3" },
    { "cameraPositionInt", "ivec3(floor(policroma_virtual_position(policroma_camera_position)))",
      NeedCamera | NeedScale, "ivec3" },
    { "previousCameraPositionFract", "fract(policroma_virtual_position(policroma_previous_camera_position))",
      NeedIris | NeedScale, "vec3" },
    { "previousCameraPositionInt", "ivec3(floor(policroma_virtual_position(policroma_previous_camera_position)))",
      NeedIris | NeedScale, "ivec3" },
    { "eyePosition", "policroma_virtual_position(policroma_camera_position)", NeedCamera | NeedScale, "vec3" },
    { "relativeEyePosition", "vec3(0.0)", 0, "vec3" },
    { "playerLookVector", "normalize(transpose(mat3(PolicromaView)) * vec3(0.0, 0.0, -1.0))", NeedCamera, "vec3" },
    // Iris 1.17 core matrices of a composite program: the fullscreen quad in [0, 1] with an orthographic projection.
    { "modelViewMatrix", "mat4(1.0)", 0, "mat4" },
    { "modelViewMatrixInverse", "mat4(1.0)", 0, "mat4" },
    { "projectionMatrix", PolicromaOrtho, 0, "mat4" },
    { "projectionMatrixInverse", "inverse(" PolicromaOrtho ")", 0, "mat4" },
    { "textureMatrix", "mat4(1.0)", 0, "mat4" },
    { "normalMatrix", "mat3(1.0)", 0, "mat3" },
    // Concepts OoT does not have: fixed neutral values (clear weather, full sky light, empty hands).
    { "rainStrength", "0.0", 0 },
    { "wetness", "0.0", 0 },
    { "nightVision", "0.0", 0 },
    { "blindness", "0.0", 0 },
    { "darknessFactor", "0.0", 0 },
    { "darknessLightFactor", "0.0", 0 },
    { "playerMood", "0.0", 0 },
    { "screenBrightness", "0.5", 0 },
    { "moonPhase", "0", 0 },
    { "worldDay", "0", 0 },
    { "heldItemId", "0", 0 },
    { "heldItemId2", "0", 0 },
    { "heldBlockLightValue", "0", 0 },
    { "heldBlockLightValue2", "0", 0 },
    { "hideGUI", "0", 0 },
    // The sky light under the eye (0..240) from the provider parameters, else open air; the pack smooths it itself.
    // With the sky exposure feature the sky light is policroma_sky (manual from the parameters, or automatic by the scene).
    { "eyeBrightness", "ivec2(0, int(policroma_sky))", NeedIris | NeedParameters | NeedSky, "ivec2" },
    { "eyeBrightnessSmooth", "ivec2(0, int(policroma_sky))", NeedIris | NeedParameters | NeedSky, "ivec2" },
    { "eyeBrightness", "ivec2(0, int(PolicromaIrisParameters.w))", NeedIris | NeedParameters, "ivec2" },
    { "eyeBrightnessSmooth", "ivec2(0, int(PolicromaIrisParameters.w))", NeedIris | NeedParameters, "ivec2" },
    { "eyeBrightness", "ivec2(0, 240)", 0, "ivec2" },
    { "eyeBrightnessSmooth", "ivec2(0, 240)", 0, "ivec2" },
};

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

bool IdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool Identifier(std::string_view text) {
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front()))) return false;
    return std::all_of(text.begin(), text.end(), IdentifierChar);
}

// Positions of the whole-word occurrences of token.
std::vector<size_t> FindTokens(std::string_view source, std::string_view token) {
    std::vector<size_t> found;
    for (size_t at = source.find(token); at != source.npos; at = source.find(token, at + 1)) {
        const auto end = at + token.size();
        if ((at == 0 || !IdentifierChar(source[at - 1])) && (end >= source.size() || !IdentifierChar(source[end])))
            found.push_back(at);
    }
    return found;
}

size_t CountToken(std::string_view source, std::string_view token) {
    return FindTokens(source, token).size();
}

bool HasToken(std::string_view source, std::string_view token) {
    return !FindTokens(source, token).empty();
}

// Iris includes are textual and resolved from shaders/ ("/lib/x.glsl") or from the including file.
bool Expand(std::string_view source, std::string_view file, const IncludeFiles& includes, int depth,
            std::string& output, std::string& error) {
    if (depth > 16) {
        error = "Includes aninhados demais em " + std::string(file);
        return false;
    }
    size_t start = 0;
    while (start < source.size()) {
        auto end = source.find('\n', start);
        if (end == source.npos) end = source.size();
        const auto line = source.substr(start, end - start);
        const auto trimmed = Trim(line);
        if (trimmed.starts_with("#include")) {
            const auto open = trimmed.find('"');
            const auto close = open == trimmed.npos ? trimmed.npos : trimmed.find('"', open + 1);
            if (close == trimmed.npos) {
                error = "Include inválido em " + std::string(file) + ": " + std::string(trimmed);
                return false;
            }
            const auto name = trimmed.substr(open + 1, close - open - 1);
            std::string path;
            if (name.starts_with('/')) {
                path = "shaders" + std::string(name);
            } else {
                const auto slash = file.rfind('/');
                path = std::string(file.substr(0, slash + 1)) + std::string(name);
            }
            const auto found = includes.find(path);
            if (found == includes.end()) {
                error = "Include ausente no ZIP: " + path;
                return false;
            }
            if (!Expand(found->second, found->first, includes, depth + 1, output, error)) return false;
        } else {
            output.append(line);
        }
        output.push_back('\n');
        if (output.size() > MaxExpandedBytes) {
            error = "O programa Iris excede o limite com os includes expandidos: " + std::string(file);
            return false;
        }
        start = end + 1;
    }
    return true;
}

// Render targets of the program: "/* DRAWBUFFERS:31 */" (one digit per target) or "/* RENDERTARGETS: 3,1 */". The
// last directive in the program wins, as in Iris; without one the program writes colortex0.
bool Targets(std::string_view source, std::string_view program, size_t maxOutputs, std::vector<int>& targets,
             std::string& error) {
    targets.clear();
    const auto drawBuffers = source.rfind("DRAWBUFFERS:");
    const auto renderTargets = source.rfind("RENDERTARGETS:");
    if (drawBuffers == source.npos && renderTargets == source.npos) {
        targets.push_back(0);
        return true;
    }
    if (renderTargets == source.npos || (drawBuffers != source.npos && drawBuffers > renderTargets)) {
        const auto rest = source.substr(drawBuffers + 12);
        size_t length = 0;
        while (length < rest.size() && std::isdigit(static_cast<unsigned char>(rest[length]))) ++length;
        if (length == 0) {
            error = std::string(program) + ": DRAWBUFFERS sem alvo.";
            return false;
        }
        for (size_t i = 0; i < length; ++i) targets.push_back(rest[i] - '0');
    } else {
        auto rest = source.substr(renderTargets + 14);
        const auto close = rest.find("*/");
        auto list = Trim(rest.substr(0, close));
        while (true) {
            const auto comma = list.find(',');
            const auto item = Trim(list.substr(0, comma));
            if (item.empty() || item.size() > 2 ||
                !std::all_of(item.begin(), item.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
                error = std::string(program) + ": RENDERTARGETS inválido (" + std::string(Trim(rest.substr(0, close))) + ").";
                return false;
            }
            targets.push_back(std::stoi(std::string(item)));
            if (comma == list.npos) break;
            list = list.substr(comma + 1);
        }
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i] > 15) {
            error = std::string(program) + ": alvo fora de colortex0..15.";
            return false;
        }
        if (std::find(targets.begin(), targets.begin() + i, targets[i]) != targets.begin() + i) {
            error = std::string(program) + ": colortex" + std::to_string(targets[i]) + " repetido nos alvos.";
            return false;
        }
    }
    if (targets.size() > maxOutputs) {
        error = std::string(program) + (maxOutputs == 1
                                            ? ": este host escreve um alvo por programa; atualize-o para a ponte Policroma V3."
                                            : ": mais de " + std::to_string(maxOutputs) + " alvos por programa.");
        return false;
    }
    return true;
}

// Iris reads DRAWBUFFERS/RENDERTARGETS from the preprocessed source, so a directive inside a disabled #if branch does
// not count (Complementary picks its targets that way). Each comment holding one becomes an identifier that survives
// the preprocessor; the line count is kept.
constexpr std::string_view DrawBuffersMarker = "policroma_drawbuffers_";
constexpr std::string_view RenderTargetsMarker = "policroma_rendertargets_";

std::string Marker(std::string_view comment) {
    if (const auto at = comment.find("DRAWBUFFERS:"); at != comment.npos) {
        auto rest = comment.substr(at + 12);
        size_t length = 0;
        while (length < rest.size() && std::isdigit(static_cast<unsigned char>(rest[length]))) ++length;
        return std::string(DrawBuffersMarker) + (length ? std::string(rest.substr(0, length)) : "x");
    }
    const auto at = comment.find("RENDERTARGETS:");
    if (at == comment.npos) return {};
    std::string marker(RenderTargetsMarker);
    bool valid = true;
    for (char c : comment.substr(at + 14)) {
        if (c == '*' || c == '\n') break;
        if (std::isdigit(static_cast<unsigned char>(c))) marker.push_back(c);
        else if (c == ',') marker.push_back('_');
        else if (!std::isspace(static_cast<unsigned char>(c))) valid = false;
    }
    return valid ? marker : std::string(RenderTargetsMarker) + "x";
}

std::string MarkDirectives(std::string_view source) {
    std::string output;
    output.reserve(source.size());
    for (size_t at = 0; at < source.size();) {
        const bool block = source.compare(at, 2, "/*") == 0;
        const bool line = source.compare(at, 2, "//") == 0;
        if (!block && !line) {
            output.push_back(source[at++]);
            continue;
        }
        auto end = block ? source.find("*/", at + 2) : source.find('\n', at);
        end = end == source.npos ? source.size() : end + (block ? 2 : 0);
        const auto comment = source.substr(at, end - at);
        output += ' ';
        output += Marker(comment);
        output += ' ';
        output.append(static_cast<size_t>(std::count(comment.begin(), comment.end(), '\n')), '\n');
        at = end;
    }
    return output;
}

// The directive of the active branch (the last one left by the preprocessor), as "DRAWBUFFERS:054" or
// "RENDERTARGETS: 0,5"; empty when no active directive is left.
std::string ActiveDirective(std::string_view preprocessed) {
    size_t best = preprocessed.npos;
    std::string_view marker;
    for (const auto prefix : { DrawBuffersMarker, RenderTargetsMarker }) {
        const auto at = preprocessed.rfind(prefix);
        if (at != preprocessed.npos && (best == preprocessed.npos || at > best)) {
            best = at;
            marker = prefix;
        }
    }
    if (best == preprocessed.npos) return {};
    auto end = best + marker.size();
    while (end < preprocessed.size() && IdentifierChar(preprocessed[end])) ++end;
    std::string value(preprocessed.substr(best + marker.size(), end - best - marker.size()));
    if (marker == DrawBuffersMarker) return "DRAWBUFFERS:" + value;
    std::replace(value.begin(), value.end(), '_', ',');
    return "RENDERTARGETS: " + value + " */";
}

// Varyings of a program with its own vertex shader: one location per name, shared by both stages, so the vertex
// outputs and the fragment inputs meet whatever order and #if branch each stage declares them in.
struct Varyings {
    std::map<std::string, int> locations;
    int next = 0;
    // Outputs the vertex shader declares once preprocessed. A fragment input outside this set would fail to link;
    // it becomes a global that reads zero.
    std::set<std::string> written;

    int Location(std::string_view name, std::string_view type) {
        const auto found = locations.find(std::string(name));
        if (found != locations.end()) return found->second;
        const int location = next;
        // A matN takes N locations; every other varying, one.
        next += type.starts_with("mat") && type.size() >= 4 ? type[3] - '0' : 1;
        locations.emplace(name, location);
        return location;
    }
};

enum class Stage { Fragment, Vertex };

struct Rewrite {
    std::string body;
    std::vector<std::string> samplers; // texture slot -> Iris name, in order of first declaration
    unsigned needs = 0;
    std::string uv;       // first vec2 input: the fullscreen UV, location 0
    int nextLocation = 1; // every other input; the translator refuses them if they are used
    Stage stage = Stage::Fragment;
    Varyings* varyings = nullptr; // set when the program brings its .vsh (V3)
    // Custom uniforms of shaders.properties by name.
    const std::map<std::string, const IrisCustomUniform*, std::less<>>* customs = nullptr;
};

// Vertex attributes of Iris (the compatibility built-ins and the 1.17 core names) for the fullscreen triangle the
// host draws. policroma_gl_uv is the OpenGL texture coordinate of the vertex.
struct AttributeAlias {
    std::string_view name;
    std::string_view type;
    std::string_view expression;
};
constexpr AttributeAlias Attributes[] = {
    { "vaPosition", "vec3", "vec3(policroma_gl_uv, 0.0)" },
    { "vaUV0", "vec2", "policroma_gl_uv" },
    { "vaColor", "vec4", "vec4(1.0)" },
    { "vaNormal", "vec3", "vec3(0.0, 0.0, 1.0)" },
};

// Drops a leading layout(location = N): the translator assigns the locations itself.
std::string_view WithoutLocation(std::string_view statement) {
    if (!statement.starts_with("layout")) return statement;
    const auto open = statement.find('(');
    const auto close = statement.find(')');
    if (open == statement.npos || close == statement.npos || open > close ||
        !Trim(statement.substr(open + 1, close - open - 1)).starts_with("location") ||
        statement.substr(open + 1, close - open - 1).find(',') != statement.npos)
        return statement;
    for (auto c : Trim(statement.substr(6, open - 6)))
        if (!std::isspace(static_cast<unsigned char>(c))) return statement;
    return Trim(statement.substr(close + 1));
}

// Gives each fragment input an explicit location: the first vec2 (and redeclarations of the same name in other
// #if branches) is the UV at location 0. Without this, automatic mapping numbers inputs in declaration order and an
// unused varying declared before texcoord would push it to location 1. With the program's own vertex shader, the
// inputs (and the vertex outputs) take the shared locations instead, and vertex attributes become expressions.
bool RewriteInput(std::string_view statement, Rewrite& rewrite, std::string& replacement) {
    if (!statement.ends_with(';')) return false;
    if (rewrite.varyings) statement = WithoutLocation(statement);
    std::vector<std::string_view> tokens;
    auto rest = Trim(statement.substr(0, statement.size() - 1));
    while (!rest.empty()) {
        const auto space = rest.find_first_of(" \t");
        tokens.push_back(rest.substr(0, space));
        if (space == rest.npos) break;
        rest = Trim(rest.substr(space));
    }
    size_t keyword = 0;
    while (keyword < tokens.size() && (tokens[keyword] == "flat" || tokens[keyword] == "smooth" ||
                                       tokens[keyword] == "noperspective" || tokens[keyword] == "centroid"))
        ++keyword;
    if (keyword + 2 >= tokens.size()) return false;
    const auto storage = tokens[keyword];
    const bool vertex = rewrite.stage == Stage::Vertex;
    const bool attribute = vertex && (storage == "in" || storage == "attribute");
    const bool output = vertex && (storage == "out" || storage == "varying");
    if (!attribute && !output && (vertex || (storage != "in" && storage != "varying"))) return false;
    const auto type = tokens[keyword + 1];
    std::string names;
    for (size_t i = keyword + 2; i < tokens.size(); ++i) names += tokens[i];
    std::string_view list = names;
    std::string prefix;
    for (size_t i = 0; i < keyword; ++i) prefix += std::string(tokens[i]) + ' ';
    if (rewrite.varyings) {
        // Every name must be a plain identifier (no arrays) for the line to be replaced.
        for (std::string_view entry = list; !entry.empty();) {
            const auto comma = entry.find(',');
            if (!Identifier(entry.substr(0, comma))) return false;
            if (comma == entry.npos) break;
            entry.remove_prefix(comma + 1);
        }
        while (!list.empty()) {
            const auto comma = list.find(',');
            const auto name = list.substr(0, comma);
            if (attribute) {
                const auto* alias = std::find_if(std::begin(Attributes), std::end(Attributes),
                                                 [&](const AttributeAlias& a) { return a.name == name; });
                const bool known = alias != std::end(Attributes) && alias->type == type;
                replacement += "#define " + std::string(name) + " " +
                               (known ? std::string(alias->expression) : std::string(type) + "(0)") + "\n";
            } else if (!vertex && !rewrite.varyings->written.contains(std::string(name))) {
                replacement += "#ifndef POLICROMA_VARYING_" + std::string(name) + "\n#define POLICROMA_VARYING_" +
                               std::string(name) + "\n" + std::string(type) + ' ' + std::string(name) + " = " +
                               std::string(type) + "(0);\n#endif\n";
            } else {
                replacement += "layout(location = " + std::to_string(rewrite.varyings->Location(name, type)) + ") " +
                               prefix + (vertex ? "out " : "in ") + std::string(type) + ' ' + std::string(name) + ";\n";
            }
            if (comma == list.npos) break;
            list.remove_prefix(comma + 1);
        }
        if (!replacement.empty()) replacement.pop_back();
        return !replacement.empty();
    }
    while (!list.empty()) {
        const auto comma = list.find(',');
        const auto name = list.substr(0, comma);
        if (!Identifier(name)) return false;
        int location;
        if (type == "vec2" && (rewrite.uv.empty() || rewrite.uv == name)) {
            // The host UV has its origin at the top (D3D); Iris packs expect OpenGL's, at the bottom. The pack
            // sees the flipped UV and the sampling helpers flip it back.
            rewrite.uv = name;
            replacement += "layout(location = 0) " + prefix + "in vec2 policroma_uv;\n#define " + std::string(name) +
                           " (vec2(policroma_uv.x, 1.0 - policroma_uv.y))\n";
            if (comma == list.npos) break;
            list.remove_prefix(comma + 1);
            continue;
        }
        location = rewrite.nextLocation++;
        replacement += "layout(location = " + std::to_string(location) + ") " + prefix + "in " + std::string(type) +
                       ' ' + std::string(name) + "; ";
        if (comma == list.npos) break;
        list.remove_prefix(comma + 1);
    }
    return !replacement.empty();
}

// Plain GLSL value types a loose uniform can be replaced with (no samplers, images, structs or arrays).
bool NumericType(std::string_view type) {
    static constexpr std::string_view scalars[] = { "float", "int", "uint", "bool" };
    for (auto scalar : scalars)
        if (type == scalar) return true;
    for (std::string_view prefix : { "vec", "ivec", "uvec", "bvec", "mat" }) {
        if (!type.starts_with(prefix)) continue;
        const auto size = type.substr(prefix.size());
        if (size.size() == 1 && size[0] >= '2' && size[0] <= '4') return true;
        if (prefix == "mat" && size.size() == 3 && size[1] == 'x' && size[0] >= '2' && size[0] <= '4' &&
            size[2] >= '2' && size[2] <= '4')
            return true;
    }
    return false;
}

const SamplerAlias* FindSampler(std::string_view name) {
    for (const auto& alias : Samplers)
        if (alias.name == name) return &alias;
    return nullptr;
}

const UniformAlias* FindUniform(std::string_view name, const IrisHost& host) {
    // The first entry the bridge can serve; V3-only entries come before their fallback.
    for (const auto& alias : Uniforms)
        if (alias.name == name && (host.v3 || !(alias.needs & NeedIris)) &&
            (host.parameters || !(alias.needs & NeedParameters)) &&
            (host.skyExposure || !(alias.needs & NeedSky)))
            return &alias;
    return nullptr;
}

// OptiFine/Iris custom uniforms of shaders.properties ("uniform.<type>.<name> = <expression>", and "variable.…"
// usable only inside other expressions), translated into one GLSL expression over the uniforms the bridge serves.
// The result is the same every frame: smooth() returns its target value, and biome and weather queries read "none"
// (no biome, dry, clear).
class CustomExpression {
  public:
    using Customs = std::map<std::string, const IrisCustomUniform*, std::less<>>;

    CustomExpression(std::string_view text, const Customs& customs, const IrisHost& host, int depth)
        : text(text), customs(customs), host(host), depth(depth) {}

    // GLSL of the expression converted to type (float, int, bool, vec2, vec3, vec4); false if it uses something
    // the translation does not know.
    bool Translate(std::string_view type, std::string& out, unsigned& usedNeeds) {
        Next();
        Value value;
        if (!Or(value) || token.kind != Token::End) return false;
        if (type == "bool") out = "bool(" + AsBool(value) + ")";
        else out = std::string(type) + "(" + AsNumber(value) + ")";
        usedNeeds |= needs;
        return out.size() <= 16384;
    }

  private:
    struct Token {
        enum Kind { End, Number, Name, Operator, Bad } kind = End;
        std::string text;
    };
    struct Value {
        std::string code;
        bool boolean = false;
        bool integerLiteral = false;
    };

    void Next() {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
        token = {};
        if (at >= text.size()) return;
        const char c = text[at];
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && at + 1 < text.size() &&
                                                            std::isdigit(static_cast<unsigned char>(text[at + 1])))) {
            const size_t begin = at;
            while (at < text.size() && (std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '.')) ++at;
            if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
                ++at;
                if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
                while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
            }
            token = { Token::Number, std::string(text.substr(begin, at - begin)) };
            return;
        }
        if (IdentifierChar(c)) {
            const size_t begin = at;
            while (at < text.size() && IdentifierChar(text[at])) ++at;
            token = { Token::Name, std::string(text.substr(begin, at - begin)) };
            return;
        }
        for (const std::string_view op : { "||", "&&", "==", "!=", "<=", ">=" }) {
            if (text.substr(at).starts_with(op)) {
                at += op.size();
                token = { Token::Operator, std::string(op) };
                return;
            }
        }
        if (std::string_view("+-*/%<>!(),.").find(c) != std::string_view::npos) {
            ++at;
            token = { Token::Operator, std::string(1, c) };
            return;
        }
        token = { Token::Bad, std::string(1, c) };
    }
    bool Is(std::string_view op) const {
        return token.kind == Token::Operator && token.text == op;
    }
    static std::string AsNumber(const Value& value) {
        return value.boolean ? "(" + value.code + " ? 1.0 : 0.0)" : value.code;
    }
    static std::string AsBool(const Value& value) {
        return value.boolean ? value.code : "(" + value.code + " != 0.0)";
    }

    bool Or(Value& out) {
        if (!And(out)) return false;
        while (Is("||")) {
            Next();
            Value right;
            if (!And(right)) return false;
            out = { "(" + AsBool(out) + " || " + AsBool(right) + ")", true };
        }
        return true;
    }
    bool And(Value& out) {
        if (!Equality(out)) return false;
        while (Is("&&")) {
            Next();
            Value right;
            if (!Equality(right)) return false;
            out = { "(" + AsBool(out) + " && " + AsBool(right) + ")", true };
        }
        return true;
    }
    bool Equality(Value& out) {
        if (!Relation(out)) return false;
        while (Is("==") || Is("!=")) {
            const auto op = token.text;
            Next();
            Value right;
            if (!Relation(right)) return false;
            out = { "(" + AsNumber(out) + " " + op + " " + AsNumber(right) + ")", true };
        }
        return true;
    }
    bool Relation(Value& out) {
        if (!Sum(out)) return false;
        while (Is("<") || Is("<=") || Is(">") || Is(">=")) {
            const auto op = token.text;
            Next();
            Value right;
            if (!Sum(right)) return false;
            out = { "(" + AsNumber(out) + " " + op + " " + AsNumber(right) + ")", true };
        }
        return true;
    }
    bool Sum(Value& out) {
        if (!Product(out)) return false;
        while (Is("+") || Is("-")) {
            const auto op = token.text;
            Next();
            Value right;
            if (!Product(right)) return false;
            out = { "(" + AsNumber(out) + " " + op + " " + AsNumber(right) + ")" };
        }
        return true;
    }
    bool Product(Value& out) {
        if (!Unary(out)) return false;
        while (Is("*") || Is("/") || Is("%")) {
            const auto op = token.text;
            Next();
            Value right;
            if (!Unary(right)) return false;
            out = { op == "%" ? "mod(" + AsNumber(out) + ", " + AsNumber(right) + ")"
                              : "(" + AsNumber(out) + " " + op + " " + AsNumber(right) + ")" };
        }
        return true;
    }
    bool Unary(Value& out) {
        if (Is("-")) {
            Next();
            if (!Unary(out)) return false;
            out = { "(-" + AsNumber(out) + ")" };
            return true;
        }
        if (Is("!")) {
            Next();
            if (!Unary(out)) return false;
            out = { "(!" + AsBool(out) + ")", true };
            return true;
        }
        return Primary(out);
    }
    bool Primary(Value& out) {
        if (token.kind == Token::Number) {
            auto number = token.text;
            const bool integer = number.find_first_of(".eE") == std::string::npos;
            if (integer) number += ".0";
            else if (number.front() == '.') number.insert(0, "0");
            else if (number.back() == '.') number += "0";
            out = { number, false, integer };
            Next();
            return true;
        }
        if (Is("(")) {
            Next();
            if (!Or(out) || !Is(")")) return false;
            Next();
            out.integerLiteral = false;
            return true;
        }
        if (token.kind != Token::Name) return false;
        const auto name = token.text;
        Next();
        if (Is("(")) return Call(name, out);
        if (!Resolve(name, out)) return false;
        if (Is(".")) {
            Next();
            if (token.kind != Token::Name ||
                token.text.find_first_not_of("xyzwrgba") != std::string::npos || token.text.size() > 4)
                return false;
            out = { "(" + out.code + ")." + token.text };
            Next();
        }
        return true;
    }
    bool Resolve(const std::string& name, Value& out) {
        if (const auto found = customs.find(name); found != customs.end()) {
            if (depth >= 16) return false;
            CustomExpression inner(found->second->expression, customs, host, depth + 1);
            std::string code;
            unsigned innerNeeds = 0;
            if (!inner.Translate(found->second->type, code, innerNeeds)) return false;
            needs |= innerNeeds;
            out = { code, found->second->type == "bool" };
            return true;
        }
        if (name == "pi") out = { "3.14159265" };
        else if (name == "true" || name == "false") out = { name, true };
        else if (const auto* alias = FindUniform(name, host)) {
            static constexpr std::pair<std::string_view, std::string_view> casts[] = {
                { "", "float" }, { "vec2", "vec2" }, { "ivec2", "vec2" }, { "vec3", "vec3" },
                { "ivec3", "vec3" }, { "vec4", "vec4" } };
            const auto cast = std::find_if(std::begin(casts), std::end(casts),
                                           [&](const auto& entry) { return entry.first == alias->type; });
            if (cast == std::end(casts)) return false;
            needs |= alias->needs;
            out = { std::string(cast->second) + "(" + std::string(alias->expression) + ")" };
        } else {
            // No biome (-1 matches no BIOME_ constant); every other unknown name (precipitation, weather, state) is 0.
            out = { name == "biome" || name == "biome_category" ? "-1.0" : "0.0" };
        }
        return true;
    }
    bool Call(const std::string& name, Value& out) {
        Next(); // (
        std::vector<Value> args;
        if (!Is(")")) {
            while (true) {
                Value arg;
                if (!Or(arg)) return false;
                args.push_back(std::move(arg));
                if (Is(")")) break;
                if (!Is(",")) return false;
                Next();
            }
        }
        Next(); // )
        const auto count = args.size();
        const auto number = [&](size_t i) { return AsNumber(args[i]); };
        static const std::map<std::string, std::string, std::less<>> same{
            { "sin", "sin" }, { "cos", "cos" }, { "tan", "tan" }, { "asin", "asin" }, { "acos", "acos" },
            { "atan", "atan" }, { "exp", "exp" }, { "exp2", "exp2" }, { "log", "log" }, { "log2", "log2" },
            { "sqrt", "sqrt" }, { "abs", "abs" }, { "floor", "floor" }, { "ceil", "ceil" }, { "frac", "fract" },
            { "signum", "sign" }, { "torad", "radians" }, { "todeg", "degrees" }, { "min", "min" }, { "max", "max" },
            { "clamp", "clamp" }, { "pow", "pow" }, { "fmod", "mod" }, { "atan2", "atan" }, { "vec2", "vec2" },
            { "vec3", "vec3" }, { "vec4", "vec4" } };
        if (name == "if") {
            if (count < 3 || count % 2 == 0) return false;
            std::string code = number(count - 1);
            for (size_t i = count - 1; i >= 2; i -= 2)
                code = "(" + AsBool(args[i - 2]) + " ? " + number(i - 1) + " : " + code + ")";
            out = { code };
        } else if (name == "smooth") {
            // smooth([id,] value[, fadeUp[, fadeDown]]): the value it fades to.
            if (count == 0 || count > 4) return false;
            const bool id = count == 4 || (count >= 2 && args[0].integerLiteral);
            out = { number(id ? 1 : 0) };
        } else if (name == "in") {
            if (count < 2) return false;
            std::string code;
            for (size_t i = 1; i < count; ++i)
                code += (i > 1 ? " || " : "") + std::string("(") + number(0) + " == " + number(i) + ")";
            out = { "(" + code + ")", true };
        } else if (name == "between") {
            if (count != 3) return false;
            out = { "(" + number(0) + " >= " + number(1) + " && " + number(0) + " <= " + number(2) + ")", true };
        } else if (name == "equals") {
            if (count != 3) return false;
            out = { "(abs(" + number(0) + " - " + number(1) + ") <= " + number(2) + ")", true };
        } else if (name == "round") {
            if (count != 1) return false;
            out = { "floor(" + number(0) + " + 0.5)" };
        } else if (name == "random") {
            out = { "0.5" };
        } else if (const auto found = same.find(name); found != same.end()) {
            if (count == 0) return false;
            std::string code = found->second + "(";
            for (size_t i = 0; i < count; ++i) code += (i ? ", " : "") + number(i);
            out = { code + ")" };
        } else {
            return false;
        }
        return true;
    }

    std::string_view text;
    const Customs& customs;
    const IrisHost& host;
    int depth;
    size_t at = 0;
    Token token;
    unsigned needs = 0;
};

// How many times each sampler name appears in its own declarations, to tell declared-only samplers apart. usage is
// the preprocessed program when available, so code in disabled #if branches does not hold a texture slot.
std::map<std::string, size_t> DeclaredSamplers(std::string_view usage) {
    std::map<std::string, size_t> declared;
    for (size_t at = 0; at < usage.size();) {
        auto end = usage.find('\n', at);
        if (end == usage.npos) end = usage.size();
        auto statement = Trim(usage.substr(at, end - at));
        if (statement.starts_with("uniform sampler2D ") || statement.starts_with("uniform sampler2DShadow ")) {
            statement.remove_prefix(statement.starts_with("uniform sampler2D ") ? 18 : 24);
            for (size_t name = 0; name < statement.size();) {
                const auto stop = statement.find_first_of(",;", name);
                const auto token = Trim(statement.substr(name, stop - name));
                if (Identifier(token)) ++declared[std::string(token)];
                if (stop == statement.npos || statement[stop] == ';') break;
                name = stop + 1;
            }
        }
        at = end + 1;
    }
    return declared;
}

// Whether the program samples colortex `index` through any of its names (colortex4, gaux1, ...), beyond declaring it.
bool ReadsColortex(std::string_view usage, int index) {
    const auto declared = DeclaredSamplers(usage);
    return std::any_of(std::begin(Samplers), std::end(Samplers), [&](const SamplerAlias& alias) {
        if (alias.colortex != index) return false;
        const auto count = declared.find(std::string(alias.name));
        return CountToken(usage, alias.name) > (count == declared.end() ? 0 : count->second);
    });
}

// Replaces each known uniform declaration, in place and on one line, by a macro. A declaration inside a disabled
// #if block stays inert because the macro is only seen where the declaration was. Unknown uniforms are left alone:
// glslang refuses them only if the code that uses them is active.
bool RewriteUniforms(std::string_view source, std::string_view usage, std::string_view program, const IrisHost& host,
                     Rewrite& rewrite, std::string& error) {
    const size_t maxSamplers = host.v3 ? MaxPassInputs : LegacyPassInputs;
    const auto declared = DeclaredSamplers(usage);
    size_t start = 0;
    while (start < source.size()) {
        auto end = source.find('\n', start);
        if (end == source.npos) end = source.size();
        const auto line = source.substr(start, end - start);
        auto statement = Trim(line);
        if (const auto comment = statement.find("//"); comment != statement.npos) statement = Trim(statement.substr(0, comment));
        std::string replacement;
        bool replaced = RewriteInput(statement, rewrite, replacement);
        if (!replaced && statement.starts_with("uniform ") && statement.ends_with(';')) {
            auto rest = Trim(statement.substr(8, statement.size() - 9));
            const auto space = rest.find_first_of(" \t");
            const auto type = rest.substr(0, space);
            auto names = space == rest.npos ? std::string_view{} : Trim(rest.substr(space));
            std::vector<std::string_view> list;
            while (!names.empty()) {
                const auto comma = names.find(',');
                list.push_back(Trim(names.substr(0, comma)));
                if (comma == names.npos) break;
                names.remove_prefix(comma + 1);
            }
            const auto unused = [&](std::string_view name) {
                const auto count = declared.find(std::string(name));
                return CountToken(usage, name) <= (count == declared.end() ? 0 : count->second);
            };
            const bool scalar = type == "float" || type == "int" || type == "uint";
            const bool sampler = type == "sampler2D" || type == "sampler2DShadow";
            // Every name must be handled for the line to be replaced; otherwise it stays for glslang to judge.
            std::string lines;
            bool handled = !list.empty();
            for (auto entry : list) {
                if (!handled) break;
                const auto equals = entry.find('=');
                const auto name = Trim(entry.substr(0, equals));
                const auto initializer = equals == entry.npos ? std::string_view{} : Trim(entry.substr(equals + 1));
                if (!Identifier(name)) {
                    handled = false;
                } else if (sampler) {
                    if (unused(name)) {
                        // Declared but never read: no texture slot.
                        lines += "// " + std::string(name) + " sem uso\n";
                        continue;
                    }
                    const auto* alias = FindSampler(name);
                    if (!alias) {
                        error = std::string(program) + ": lê " + std::string(name) +
                                ", que o Iris reduzido não fornece (só colortex0..15, depthtex0..2, noisetex e shadow*).";
                        return false;
                    }
                    // Aliases of one source (gcolor/colortex0, depthtex0/depthtex1) share a slot.
                    const auto origin = alias->colortex;
                    auto slot = std::find_if(rewrite.samplers.begin(), rewrite.samplers.end(),
                                             [&](const std::string& other) { return FindSampler(other)->colortex == origin; });
                    if (slot == rewrite.samplers.end()) {
                        if (rewrite.samplers.size() >= maxSamplers) {
                            error = std::string(program) + ": mais de " + std::to_string(maxSamplers) +
                                    " texturas por programa no Iris reduzido (";
                            for (const auto& used : rewrite.samplers) error += used + ", ";
                            error += std::string(name) + ")" +
                                     (host.v3 ? "." : "; a ponte Policroma V3 do host aceita 16.");
                            return false;
                        }
                        rewrite.samplers.emplace_back(name);
                        slot = rewrite.samplers.end() - 1;
                    }
                    // A texture2D, so the pack can pass it to its own functions; the sampling helpers in the
                    // header build the combined sampler at the point of use, as Vulkan GLSL requires.
                    lines += "#define " + std::string(name) + " policroma_t" +
                             std::to_string(slot - rewrite.samplers.begin()) + "\n";
                } else if (const auto* alias = FindUniform(name, host);
                           alias && (alias->type.empty() ? scalar : alias->type == type)) {
                    rewrite.needs |= alias->needs;
                    // A global, not a macro: packs #define these names themselves (Complementary redefines
                    // nightVision). glslang runs global initializers at the start of main, in declaration order. GLSL
                    // allows a uniform to be declared again; the guard keeps the first declaration that runs.
                    lines += "#ifndef POLICROMA_UNIFORM_" + std::string(name) + "\n#define POLICROMA_UNIFORM_" +
                             std::string(name) + "\n" + std::string(type) + ' ' + std::string(name) + " = " +
                             std::string(type) + "(" + std::string(alias->expression) + ");\n#endif\n";
                } else if (const auto found = rewrite.customs ? rewrite.customs->find(name) : decltype(rewrite.customs->end()){};
                           rewrite.customs && found != rewrite.customs->end() && found->second->uniform &&
                           (type == "bool" || type == "vec2" || type == "vec3" || type == "vec4" ? type == found->second->type
                                                                                                 : scalar && found->second->type != "bool" &&
                                                                                                       found->second->type.find("vec") == std::string::npos)) {
                    // A custom uniform of shaders.properties: its expression, computed in the shader.
                    std::string code;
                    unsigned customNeeds = 0;
                    CustomExpression expression(found->second->expression, *rewrite.customs, host, 0);
                    const bool translated = expression.Translate(found->second->type, code, customNeeds);
                    if (translated) rewrite.needs |= customNeeds;
                    lines += "#ifndef POLICROMA_UNIFORM_" + std::string(name) + "\n#define POLICROMA_UNIFORM_" +
                             std::string(name) + "\n" + std::string(type) + ' ' + std::string(name) + " = " +
                             (translated ? std::string(type) + "(" + code + ")" : std::string(type) + "(0)") + ";\n#endif\n";
                } else if (NumericType(type)) {
                    // Iris fills uniforms the host does not have from shaders.properties or leaves them at their
                    // default; here they read their GLSL initializer, or zero. Vulkan GLSL refuses a loose uniform.
                    lines += "#ifndef POLICROMA_UNIFORM_" + std::string(name) + "\n#define POLICROMA_UNIFORM_" +
                             std::string(name) + "\n" + std::string(type) + ' ' + std::string(name) + " = " +
                             (initializer.empty() ? std::string(type) + "(0)" : std::string(initializer)) +
                             ";\n#endif\n";
                } else {
                    handled = false;
                }
            }
            if (handled) {
                lines.pop_back();
                replacement = std::move(lines);
                replaced = true;
            }
        }
        rewrite.body.append(replaced ? std::string_view(replacement) : line);
        rewrite.body.push_back('\n');
        start = end + 1;
    }
    return true;
}

// Iris samples noisetex and the custom textures without "clamp" with repeat; the host sampler s0 clamps. Calls whose
// texture argument is one of them go through helpers that repeat: the host sampler s1 when the bridge has it, else
// a wrapped coordinate (the texture has one mip level, so fract only disturbs the filtering at the seam).
void WrapTextures(std::string& body, const std::set<std::string>& names) {
    std::vector<size_t> positions;
    for (const auto& name : names) {
        const auto found = FindTokens(body, name);
        positions.insert(positions.end(), found.begin(), found.end());
    }
    std::sort(positions.begin(), positions.end());
    for (auto it = positions.rbegin(); it != positions.rend(); ++it) {
        size_t at = *it;
        while (at > 0 && std::isspace(static_cast<unsigned char>(body[at - 1]))) --at;
        if (at == 0 || body[at - 1] != '(') continue;
        size_t end = at - 1;
        while (end > 0 && std::isspace(static_cast<unsigned char>(body[end - 1]))) --end;
        size_t begin = end;
        while (begin > 0 && IdentifierChar(body[begin - 1])) --begin;
        const auto function = std::string_view(body).substr(begin, end - begin);
        std::string_view wrapped;
        if (function == "texture2D" || function == "texture") wrapped = "policroma_wrap_texture";
        else if (function == "texture2DLod" || function == "textureLod") wrapped = "policroma_wrap_textureLod";
        if (!wrapped.empty()) body.replace(begin, end - begin, wrapped);
    }
}

// Buffers declared "const bool colortexNClear = false;" (or with a gaux/gcolor name): Iris keeps their contents
// between frames instead of clearing them.
void KeptBuffers(std::string_view text, std::set<int>& kept) {
    for (size_t at = text.find("Clear"); at != text.npos; at = text.find("Clear", at + 5)) {
        size_t begin = at;
        while (begin > 0 && IdentifierChar(text[begin - 1])) --begin;
        const auto* alias = FindSampler(text.substr(begin, at - begin));
        if (!alias || alias->colortex < 0) continue;
        auto before = Trim(text.substr(0, begin));
        if (!before.ends_with("bool") || !Trim(before.substr(0, before.size() - 4)).ends_with("const")) continue;
        auto after = Trim(text.substr(at + 5));
        if (!after.starts_with('=')) continue;
        after = Trim(after.substr(1));
        if (after.starts_with("false")) kept.insert(alias->colortex);
    }
}

// A float constant as GLSL text (always with a decimal point or exponent).
std::string GlslFloat(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
    return std::string(text).find_first_of(".en") == std::string::npos ? std::string(text) + ".0" : text;
}

std::string Header(const Rewrite& rewrite, bool fragData, bool gather, const IrisHost& host) {
    const bool vertex = rewrite.stage == Stage::Vertex;
    std::string header = "#version 450\n";
    header += "layout(binding = " + std::to_string(SamplerBinding) + ") uniform sampler policroma_sampler;\n";
    if (host.textures)
        header += "layout(binding = " + std::to_string(WrapSamplerBinding) + ") uniform sampler policroma_wrap_sampler;\n";
    for (size_t slot = 0; slot < std::max<size_t>(rewrite.samplers.size(), (rewrite.needs & NeedSize) ? 1 : 0); ++slot)
        header += "layout(binding = " + std::to_string(slot) + ") uniform texture2D policroma_t" +
                  std::to_string(slot) + ";\n";
    // The scale k, the ground offset and the sky light come from LinkSpanIris vector 11 when the host serves it.
    const bool live = host.v3 && host.parameters;
    const unsigned needs = rewrite.needs | (((rewrite.needs & (NeedProjection | NeedFog)) && host.v3) ? NeedIris : 0u) |
                           (((rewrite.needs & (NeedScale | NeedFog)) && live) ? (NeedIris | NeedCamera) : 0u);
    if (needs & NeedCamera) {
        header += "layout(std140, binding = " + std::to_string(CameraBinding) +
                  ") uniform LinkSpanCamera { mat4 PolicromaProjection; mat4 PolicromaView; vec4 PolicromaCameraInfo; };\n"
                  "#define policroma_camera_position ((inverse(PolicromaView) * vec4(0.0, 0.0, 0.0, 1.0)).xyz)\n";
    }
    if (needs & NeedScene) {
        header += "layout(std140, binding = " + std::to_string(SceneBinding) +
                  ") uniform LinkSpanScene { vec4 PolicromaSceneData[36]; };\n"
                  "#define policroma_sun_direction (PolicromaSceneData[1].xyz / max(length(PolicromaSceneData[1].xyz), 0.0001))\n";
    }
    if (needs & NeedIris) {
        header += "layout(std140, binding = " + std::to_string(IrisBinding) +
                  ") uniform LinkSpanIris { mat4 PolicromaPreviousProjection; mat4 PolicromaPreviousView; "
                  "vec4 PolicromaFog; vec4 PolicromaIrisInfo; vec4 PolicromaIrisClock;" +
                  std::string(host.parameters ? " vec4 PolicromaIrisParameters;" : "") +
                  // The 13th vector only with the bridge feature: a 12-vector host keeps the 0.7.8 declaration.
                  std::string(host.parameters && host.skyExposure ? " vec4 PolicromaIrisSky;" : "") + " };\n"
                  "#define policroma_previous_camera_position ((inverse(PolicromaPreviousView) * vec4(0.0, 0.0, 0.0, 1.0)).xyz)\n";
    }
    if (needs & NeedSky) {
        // The sky light under the eye, once per shader invocation (like policroma_k): w >= 0 is manual (the menu value, as
        // 0.7.8); w < 0 (AutoIrisSkyLight) is automatic, mixing the indoor and open air values by the host's openness.
        header += "float policroma_sky_light() {\n"
                  "    float manual = PolicromaIrisParameters.w;\n"
                  "    return manual >= 0.0 ? manual : mix(" + GlslFloat(IndoorIrisSkyLight) + ", " +
                  GlslFloat(MaxIrisSkyLight) + ", clamp(PolicromaIrisSky.x, 0.0, 1.0));\n"
                  "}\n"
                  "float policroma_sky = policroma_sky_light();\n";
    }
    if (needs & (NeedScale | NeedFog)) {
        // y = blocks per OoT unit (k), z = OoT height of the floor (0: no offset); k = 1 reproduces the unscaled pack.
        if (live) {
            // k = 0 is automatic: the d50 of the scene's native fog sets k. Phase 1 (0.7.7) derives d50 from the legacy
            // start S, the end, zNear n and zFar f; with the host's fog ramp (0.7.8) clock.zw is the ramp in u, which
            // also covers n != 10 and worldFog. {0, 0} is "no ramp this frame" and falls back to phase 1.
            header += "float policroma_auto_scale() {\n"
                      "    float n = PolicromaCameraInfo.x, f = PolicromaCameraInfo.y, s = PolicromaFog.w, e = PolicromaIrisInfo.w;\n"
                      "    float d = (e - s > 0.0) ? 2.0 * f * s / (2.0 * s + f - n) : f;\n";
            if (host.fogRamp)
                header += "    float z = PolicromaIrisClock.z, w = PolicromaIrisClock.w;\n"
                          "    if (w > 0.0 && f > n)\n"
                          "        d = (w - z < " + GlslFloat(FogRampMinSpan) + " && w >= " + GlslFloat(FogRampTotalEnd) +
                          ") ? f : n / max(1.0 - 0.5 * (z + w) * (f - n) / (" + GlslFloat(FogRampUnits) + " * f), n / f);\n";
            header += "    d = max(d, " + GlslFloat(AutoFogMinDistance) + ");\n"
                      "    float k = sqrt(" + GlslFloat(AutoFogCoefficient) + " * f / (d * d * d));\n"
                      "    return clamp(f * k >= " + GlslFloat(AutoFogSaturation) + " ? k : " + GlslFloat(AutoFogFallback) +
                      " / d, " + GlslFloat(MinAutoIrisFogScale) + ", " + GlslFloat(MaxAutoIrisFogScale) + ");\n"
                      "}\n"
                      // k once per shader invocation: glslang runs global initializers at the start of main, before the
                      // pack's own globals. A fixed k > 0 skips the automatic path (a uniform branch, no sqrt).
                      "float policroma_compute_scale() {\n"
                      "    if (PolicromaIrisParameters.y > " + GlslFloat(AutoIrisFogScale) + ") return PolicromaIrisParameters.y;\n"
                      "    return policroma_auto_scale();\n"
                      "}\n"
                      "float policroma_k = policroma_compute_scale();\n"
                      "#define policroma_scale policroma_k\n";
        } else {
            header += "#define policroma_scale 1.0\n";
        }
    }
    if (needs & NeedScale) {
        header += live ? "#define policroma_ground PolicromaIrisParameters.z\n" : "#define policroma_ground 0.0\n";
        header += "float policroma_virtual_y(float y) {\n"
                  "    return (y - policroma_ground) * policroma_scale + (policroma_ground != 0.0 ? " +
                  std::to_string(GroundAnchorBlocks) + ".0 : 0.0);\n"
                  "}\n"
                  "vec3 policroma_virtual_position(vec3 p) {\n"
                  "    return vec3(p.x * policroma_scale, policroma_virtual_y(p.y), p.z * policroma_scale);\n"
                  "}\n"
                  "#define policroma_scale_matrix mat4(vec4(policroma_scale, 0.0, 0.0, 0.0), "
                  "vec4(0.0, policroma_scale, 0.0, 0.0), vec4(0.0, 0.0, policroma_scale, 0.0), vec4(0.0, 0.0, 0.0, 1.0))\n"
                  "#define policroma_unscale_matrix mat4(vec4(1.0 / policroma_scale, 0.0, 0.0, 0.0), "
                  "vec4(0.0, 1.0 / policroma_scale, 0.0, 0.0), vec4(0.0, 0.0, 1.0 / policroma_scale, 0.0), "
                  "vec4(0.0, 0.0, 0.0, 1.0))\n";
    }
    {
        // cloudHeight without the live parameters: the value of this activation.
        char number[32];
        std::snprintf(number, sizeof(number), "%.1f", static_cast<double>(host.cloudHeight));
        header += std::string("#define policroma_cloud_height ") + number + "\n";
    }
    if (needs & NeedProjection) {
        // The Fast3D interpreter scales clip x for the window aspect after the game's 4:3 projection; the depth
        // buffer was drawn with that, so reconstruction needs it too. LinkSpanIris carries the factor (V3 only).
        header += host.v3 ? "#define policroma_projection (mat4(vec4(PolicromaIrisInfo.z, 0.0, 0.0, 0.0), "
                            "vec4(0.0, 1.0, 0.0, 0.0), vec4(0.0, 0.0, 1.0, 0.0), vec4(0.0, 0.0, 0.0, 1.0)) * "
                            "PolicromaProjection)\n"
                          : "#define policroma_projection PolicromaProjection\n";
    }
    if (needs & NeedFog) {
        // gl_Fog from the scene fog (LinkSpanIris); without the V3 bridge, no fog.
        header += "struct policroma_FogParameters { vec4 color; float density; float start; float end; float scale; };\n";
        header += host.v3 ? "policroma_FogParameters policroma_fog() {\n"
                            "    float range = max(PolicromaIrisInfo.w - PolicromaFog.w, 0.0001) * policroma_scale;\n"
                            "    return policroma_FogParameters(vec4(PolicromaFog.rgb, 1.0), 1.0, "
                            "PolicromaFog.w * policroma_scale, PolicromaIrisInfo.w * policroma_scale, 1.0 / range);\n"
                            "}\n"
                          : "policroma_FogParameters policroma_fog() {\n"
                            "    return policroma_FogParameters(vec4(0.0), 0.0, 0.0, 1.0, 1.0);\n"
                            "}\n";
        header += "#define gl_Fog policroma_fog()\n";
    }
    if (needs & NeedWater) {
        // Water boxes of the scene (bounds xz in [20..27], surface height in [28..35].x).
        header += "int policroma_eye_in_water() {\n"
                  "    vec3 eye = policroma_camera_position;\n"
                  "    int count = int(PolicromaSceneData[2].y);\n"
                  "    for (int i = 0; i < 8; ++i) {\n"
                  "        if (i >= count) break;\n"
                  "        vec4 box = PolicromaSceneData[20 + i];\n"
                  "        if (eye.x >= box.x && eye.x <= box.z && eye.z >= box.y && eye.z <= box.w &&\n"
                  "            eye.y < PolicromaSceneData[28 + i].x) return 1;\n"
                  "    }\n"
                  "    return 0;\n"
                  "}\n";
    }
    // Sampling helpers: the pack sees OpenGL orientation (origin at the bottom), the textures are D3D (origin at
    // the top). Defined before the macros below, so their bodies call the real built-ins. Every input of a pass has
    // a single mip level (the host targets, the scene, scene.noise), so implicit level of detail is level 0; reading
    // it explicitly keeps derivatives out of the pack's dynamic loops, which FXC would otherwise have to unroll
    // (Complementary has loops of 74 and 155 iterations), and works in the vertex stage.
    header +=
        "vec2 policroma_flip(vec2 c) { return vec2(c.x, 1.0 - c.y); }\n"
        "ivec2 policroma_textureSize(texture2D t, int lod) { return textureSize(sampler2D(t, policroma_sampler), lod); }\n"
        "vec4 policroma_texture(texture2D t, vec2 c) {\n"
        "    return textureLod(sampler2D(t, policroma_sampler), policroma_flip(c), 0.0);\n"
        "}\n"
        "vec4 policroma_texture(texture2D t, vec2 c, float bias) { return policroma_texture(t, c); }\n";
    header +=
        "vec4 policroma_textureLod(texture2D t, vec2 c, float lod) {\n"
        "    return textureLod(sampler2D(t, policroma_sampler), policroma_flip(c), lod);\n"
        "}\n"
        "vec4 policroma_textureGrad(texture2D t, vec2 c, vec2 dx, vec2 dy) {\n"
        "    return textureGrad(sampler2D(t, policroma_sampler), policroma_flip(c), vec2(dx.x, -dx.y), vec2(dy.x, -dy.y));\n"
        "}\n"
        // Offsets are constant expressions in GLSL; through a function they are not, so the helper adds them to the UV.
        "vec4 policroma_textureOffset(texture2D t, vec2 c, ivec2 o) {\n"
        "    return policroma_texture(t, c + vec2(o) / vec2(policroma_textureSize(t, 0)));\n"
        "}\n"
        "vec4 policroma_texelFetch(texture2D t, ivec2 p, int lod) {\n"
        "    ivec2 size = policroma_textureSize(t, lod);\n"
        "    return texelFetch(sampler2D(t, policroma_sampler), ivec2(p.x, size.y - 1 - p.y), lod);\n"
        "}\n"
        // sampler2DShadow: a vec3 coordinate compares its z with the stored depth (LEQUAL, as Iris sets it up).
        "float policroma_texture(texture2D t, vec3 c) { return step(c.z, policroma_texture(t, c.xy).r); }\n"
        "float policroma_texture(texture2D t, vec3 c, float bias) { return policroma_texture(t, c); }\n"
        "float policroma_textureLod(texture2D t, vec3 c, float lod) { return policroma_texture(t, c); }\n"
        "vec4 policroma_shadow2D(texture2D t, vec3 c) { return vec4(policroma_texture(t, c)); }\n";
    if (host.textures) {
        // Repeat in the sampler (s1): the filter blends across the seam as in Iris.
        header +=
            "vec4 policroma_wrap_textureLod(texture2D t, vec2 c, float lod) {\n"
            "    return textureLod(sampler2D(t, policroma_wrap_sampler), policroma_flip(c), lod);\n"
            "}\n"
            "vec4 policroma_wrap_texture(texture2D t, vec2 c) { return policroma_wrap_textureLod(t, c, 0.0); }\n"
            "vec4 policroma_wrap_texture(texture2D t, vec2 c, float bias) { return policroma_wrap_textureLod(t, c, 0.0); }\n";
    } else {
        header +=
            "vec4 policroma_wrap_texture(texture2D t, vec2 c) { return policroma_texture(t, fract(c)); }\n"
            "vec4 policroma_wrap_texture(texture2D t, vec2 c, float bias) { return policroma_texture(t, fract(c), bias); }\n"
            "vec4 policroma_wrap_textureLod(texture2D t, vec2 c, float lod) { return policroma_textureLod(t, fract(c), lod); }\n";
    }
    if (gather) {
        // Vulkan/D3D gather order is (i0,j1) (i1,j1) (i1,j0) (i0,j0) with j growing down the memory rows; with the
        // rows flipped the OpenGL order is the reverse. The component must be a constant at the built-in.
        header +=
            "vec4 policroma_textureGather(texture2D t, vec2 c, int comp) {\n"
            "    vec2 f = policroma_flip(c);\n"
            "    if (comp == 1) return textureGather(sampler2D(t, policroma_sampler), f, 1).wzyx;\n"
            "    if (comp == 2) return textureGather(sampler2D(t, policroma_sampler), f, 2).wzyx;\n"
            "    if (comp == 3) return textureGather(sampler2D(t, policroma_sampler), f, 3).wzyx;\n"
            "    return textureGather(sampler2D(t, policroma_sampler), f, 0).wzyx;\n"
            "}\n"
            "vec4 policroma_textureGather(texture2D t, vec2 c) { return policroma_textureGather(t, c, 0); }\n"
            "vec4 policroma_textureGatherOffset(texture2D t, vec2 c, ivec2 o, int comp) {\n"
            "    return policroma_textureGather(t, c + vec2(o) / vec2(policroma_textureSize(t, 0)), comp);\n"
            "}\n"
            "vec4 policroma_textureGatherOffset(texture2D t, vec2 c, ivec2 o) {\n"
            "    return policroma_textureGatherOffset(t, c, o, 0);\n"
            "}\n";
    }
    header += "#define policroma_view_size vec2(policroma_textureSize(policroma_t0, 0))\n"
              // Function-like, so the type name texture2D (from sampler2D below) is left alone.
              "#define texture2D(s, c) policroma_texture(s, c)\n"
              "#define texture policroma_texture\n"
              "#define texture2DLod policroma_textureLod\n"
              "#define textureLod policroma_textureLod\n"
              "#define texture2DGrad policroma_textureGrad\n"
              "#define textureGrad policroma_textureGrad\n"
              "#define textureOffset policroma_textureOffset\n"
              "#define texelFetch policroma_texelFetch\n"
              "#define texelFetch2D policroma_texelFetch\n"
              "#define textureSize policroma_textureSize\n"
              "#define sampler2D texture2D\n"
              "#define sampler2DShadow texture2D\n"
              "#define shadow2D policroma_shadow2D\n"
              "#define shadow2DLod(s, c, l) policroma_shadow2D(s, c)\n";
    if (vertex) {
        // Compatibility profile of a composite vertex shader: the host draws one fullscreen triangle (vertices 0, 1
        // and 2), whose OpenGL texture coordinates are (0, 1), (2, 1) and (0, -1).
        header += "#define policroma_gl_uv vec2(gl_VertexIndex == 1 ? 2.0 : 0.0, gl_VertexIndex == 2 ? -1.0 : 1.0)\n"
                  "#define gl_Vertex vec4(policroma_gl_uv, 0.0, 1.0)\n"
                  "#define gl_MultiTexCoord0 vec4(policroma_gl_uv, 0.0, 1.0)\n"
                  "#define gl_MultiTexCoord1 vec4(0.0, 0.0, 0.0, 1.0)\n"
                  "#define gl_MultiTexCoord2 vec4(0.0, 0.0, 0.0, 1.0)\n"
                  "#define gl_Color vec4(1.0)\n"
                  "#define gl_Normal vec3(0.0, 0.0, 1.0)\n"
                  "#define gl_ModelViewMatrix mat4(1.0)\n"
                  "#define gl_ModelViewMatrixInverse mat4(1.0)\n"
                  "#define gl_ProjectionMatrix " PolicromaOrtho "\n"
                  "#define gl_ProjectionMatrixInverse inverse(" PolicromaOrtho ")\n"
                  "#define gl_ModelViewProjectionMatrix " PolicromaOrtho "\n"
                  "#define gl_NormalMatrix mat3(1.0)\n"
                  "const mat4 policroma_texture_matrix[8] = mat4[8](mat4(1.0), mat4(1.0), mat4(1.0), mat4(1.0), "
                  "mat4(1.0), mat4(1.0), mat4(1.0), mat4(1.0));\n"
                  "#define gl_TextureMatrix policroma_texture_matrix\n"
                  "#define ftransform() (" PolicromaOrtho " * gl_Vertex)\n"
                  "#define attribute in\n"
                  "#define varying out\n";
    } else {
        header += "#define varying in\n"
                  // Vulkan's FragCoord has its origin at the top; OpenGL's at the bottom.
                  "#define gl_FragCoord vec4(gl_FragCoord.x, policroma_view_size.y - gl_FragCoord.y, gl_FragCoord.zw)\n";
    }
    if (gather) {
        header += "#define textureGather policroma_textureGather\n"
                  "#define textureGatherOffset policroma_textureGatherOffset\n";
    }
    if (fragData) {
        header += "vec4 policroma_FragData[8];\n"
                  "#define gl_FragData policroma_FragData\n"
                  "#define gl_FragColor policroma_FragData[0]\n"
                  "#define main policroma_iris_main\n";
    }
    return header;
}

// Drops the program's own #version line; the header sets 450 with the compatibility macros above.
std::string WithoutVersion(std::string_view source) {
    const auto trimmed = Trim(source.substr(0, source.find('\n')));
    if (!trimmed.starts_with("#version")) return std::string(source);
    const auto end = source.find('\n');
    return end == source.npos ? std::string() : "\n" + std::string(source.substr(end + 1));
}

// Iris without final.fsh presents colortex0; this pass does that when the last program cannot be the final pass
// (several targets, or not colortex0). Same size as the screen, so texelFetch copies texel by texel.
constexpr std::string_view PresentShader = R"(#version 450
layout(binding = 16) uniform sampler policroma_sampler;
layout(binding = 0) uniform texture2D policroma_t0;
layout(location = 0) out vec4 policroma_out;
void main() {
    policroma_out = texelFetch(sampler2D(policroma_t0, policroma_sampler), ivec2(gl_FragCoord.xy), 0);
}
)";

struct MaterialBuffers {
    int normal;
    int specular;

    bool contains(int index) const { return normal == index || specular == index; }
};

bool ParseMaterialBuffers(std::string_view properties, std::optional<MaterialBuffers>& buffers, std::string& error) {
    while (!properties.empty()) {
        const auto end = properties.find('\n');
        const auto line = Trim(properties.substr(0, end));
        properties.remove_prefix(end == properties.npos ? properties.size() : end + 1);
        const auto equals = line.find('=');
        if (Trim(line.substr(0, equals)) != MaterialProperty) continue;
        const auto value = equals == line.npos ? std::string_view{} : Trim(line.substr(equals + 1));
        const auto split = value.find_first_of(" \t");
        const auto normal = value.substr(0, split);
        const auto specular = split == value.npos ? std::string_view{} : Trim(value.substr(split + 1));
        const auto* normalAlias = normal.starts_with("colortex") ? FindSampler(normal) : nullptr;
        const auto* specularAlias = specular.starts_with("colortex") ? FindSampler(specular) : nullptr;
        if (buffers || !normalAlias || !specularAlias || normalAlias->colortex == 0 ||
            specularAlias->colortex == 0 || normalAlias->colortex == specularAlias->colortex) {
            error = "linkspan.materialGbuffer exige dois nomes colortex1..15 distintos, na ordem normal especular, "
                    "em uma única declaração.";
            return false;
        }
        buffers = MaterialBuffers{ normalAlias->colortex, specularAlias->colortex };
    }
    return true;
}

} // namespace

std::vector<std::string> IrisProgramFiles(const std::string& name) {
    return { "shaders/world0/" + name + ".fsh", "shaders/" + name + ".fsh" };
}

std::vector<IrisTexture> ParseIrisTextures(std::string_view properties) {
    std::vector<IrisTexture> textures;
    for (size_t at = 0; at < properties.size();) {
        auto end = properties.find('\n', at);
        if (end == properties.npos) end = properties.size();
        const auto line = Trim(properties.substr(at, end - at));
        at = end + 1;
        if (!line.starts_with("texture.")) continue;
        const auto equals = line.find('=');
        if (equals == line.npos) continue;
        const auto key = Trim(line.substr(8, equals - 8));
        auto path = std::string(Trim(line.substr(equals + 1)));
        // Only PNG files of the pack itself: resource locations and raw textures (".dat TEXTURE_2D ...") stay out.
        auto lower = path;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (path.empty() || path.find(':') != path.npos || path.find(' ') != path.npos || path.find('\\') != path.npos ||
            !lower.ends_with(".png"))
            continue;
        while (path.starts_with('/')) path.erase(0, 1);
        while (path.starts_with("./")) path.erase(0, 2);
        if (path.find("..") != path.npos || path.empty()) continue;
        IrisTexture texture;
        if (key == "noise") {
            texture.stage = "noise";
        } else {
            const auto dot = key.find('.');
            if (dot == key.npos) continue;
            texture.stage = std::string(key.substr(0, dot));
            texture.buffer = std::string(key.substr(dot + 1));
            if (texture.stage != "deferred" && texture.stage != "composite") continue;
            const auto* alias = FindSampler(texture.buffer);
            if (!alias || alias->colortex < 0) continue;
        }
        texture.file = "shaders/" + path;
        // A later line for the same stage and buffer replaces the earlier one.
        std::erase_if(textures, [&](const IrisTexture& other) {
            return other.stage == texture.stage && other.buffer == texture.buffer;
        });
        textures.push_back(std::move(texture));
    }
    return textures;
}

std::vector<IrisCustomUniform> ParseIrisUniforms(std::string_view properties) {
    std::vector<IrisCustomUniform> uniforms;
    for (size_t at = 0; at < properties.size();) {
        auto end = properties.find('\n', at);
        if (end == properties.npos) end = properties.size();
        const auto line = Trim(properties.substr(at, end - at));
        at = end + 1;
        const bool uniform = line.starts_with("uniform.");
        if (!uniform && !line.starts_with("variable.")) continue;
        const auto equals = line.find('=');
        if (equals == line.npos) continue;
        const auto key = Trim(line.substr(uniform ? 8 : 9, equals - (uniform ? 8 : 9)));
        const auto dot = key.find('.');
        if (dot == key.npos) continue;
        IrisCustomUniform entry{ std::string(key.substr(0, dot)), std::string(key.substr(dot + 1)),
                                 std::string(Trim(line.substr(equals + 1))), uniform };
        static constexpr std::string_view types[] = { "float", "int", "bool", "vec2", "vec3", "vec4" };
        if (std::find(std::begin(types), std::end(types), entry.type) == std::end(types) || !Identifier(entry.name) ||
            entry.expression.empty())
            continue;
        std::erase_if(uniforms, [&](const IrisCustomUniform& other) { return other.name == entry.name; });
        uniforms.push_back(std::move(entry));
    }
    return uniforms;
}

bool IrisTextureClamp(std::string_view mcmeta) {
    try {
        const auto meta = nlohmann::json::parse(mcmeta);
        const auto& texture = meta.at("texture");
        return texture.contains("clamp") && texture.at("clamp").is_boolean() && texture.at("clamp").get<bool>();
    } catch (...) { return false; }
}

std::string IrisVertexFile(const std::string& fragmentFile) {
    return fragmentFile.ends_with(".fsh") ? fragmentFile.substr(0, fragmentFile.size() - 4) + ".vsh" : std::string();
}

std::vector<std::string> IrisProgramOrder() {
    std::vector<std::string> order;
    for (const char* stage : { "deferred", "composite" }) {
        order.push_back(stage);
        for (int i = 1; i <= 15; ++i) order.push_back(stage + std::to_string(i));
    }
    order.push_back("final");
    return order;
}

bool BuildIrisGraph(const std::vector<IrisProgram>& packPrograms, const IncludeFiles& includes,
                    std::vector<std::string>& glsl, std::string& manifest, std::string& error, IrisHost host,
                    std::vector<std::string>* vertexGlsl, const std::vector<IrisTexture>& textures,
                    const std::vector<IrisCustomUniform>& uniforms, std::string_view properties) {
    host.textures &= host.v3;
    host.parameters &= host.v3;
    host.syntheticGbuffer &= host.v3;
    std::optional<MaterialBuffers> material;
    if (!ParseMaterialBuffers(properties, material, error)) return false;
    if (material && !host.v3) {
        error = "linkspan.materialGbuffer exige a ponte Policroma V3 com os buffers de material.";
        return false;
    }
    std::map<std::string, const IrisCustomUniform*, std::less<>> customs;
    for (const auto& uniform : uniforms) customs[uniform.name] = &uniform;
    const size_t maxPrograms = (host.v3 ? MaxPasses : LegacyMaxPasses) - (material ? 1u : 0u);
    const size_t maxOutputs = host.v3 ? MaxPassOutputs : 1;
    const auto tooMany = [&] {
        error = "O Iris reduzido executa até " + std::to_string(maxPrograms) + " programas (deferred, composite e final)" +
                (host.v3 ? "." : "; a ponte Policroma V3 do host aceita 16.");
    };
    if (packPrograms.empty()) {
        error = "O pack Iris não tem deferred, composite nem final.";
        return false;
    }
    if (packPrograms.size() > maxPrograms) {
        tooMany();
        return false;
    }
    // First the targets of every program and the buffers the pack keeps between frames: a program may read a
    // buffer that only a later program writes, which Iris then serves from the previous frame.
    struct Prepared {
        std::string expanded, preprocessed;
        bool active = false;
        std::vector<int> targets{ 0 };
    };
    std::vector<Prepared> prepared(packPrograms.size());
    std::set<int> kept; // colortexNClear = false
    const auto prepare = [&](const IrisProgram& program, Prepared& prep) {
        if (!Expand(program.source, program.file, includes, 0, prep.expanded, error)) return false;
        std::string ignored;
        // If the preprocessor stops (an #error, say), the raw text stands in for usage and the last directive.
        prep.active = PreprocessGlsl("#version 450\n" + WithoutVersion(MarkDirectives(prep.expanded)), program.file,
                                     prep.preprocessed, ignored);
        if (program.name != "final") {
            const std::string directive = prep.active ? ActiveDirective(prep.preprocessed) : std::string();
            if (!Targets(prep.active ? std::string_view(directive) : std::string_view(prep.expanded), program.name,
                         maxOutputs, prep.targets, error))
                return false;
        }
        KeptBuffers(prep.active ? prep.preprocessed : prep.expanded, kept);
        return true;
    };
    for (size_t index = 0; index < packPrograms.size(); ++index)
        if (!prepare(packPrograms[index], prepared[index])) return false;

    // Opt-in F0 gbuffer program, first in the graph: it fills the colortex a pack program reads before any program of
    // the pack writes it (colortex4/6/3 for Complementary), and nothing else. A buffer the pack seeds from the host
    // material buffers is left alone. Without a reader the graph stays exactly the pack's own.
    std::vector<IrisProgram> withGbuffer;
    if (host.syntheticGbuffer) {
        std::set<int> written;
        std::map<int, bool> wanted{ { GbufferF0Normal, false }, { GbufferF0Material, false },
                                    { GbufferF0Translucent, false } };
        if (material) {
            wanted.erase(material->normal);
            wanted.erase(material->specular);
        }
        for (size_t index = 0; index < packPrograms.size(); ++index) {
            const auto& prep = prepared[index];
            for (auto& [colortex, needed] : wanted)
                needed |= !written.contains(colortex) &&
                          ReadsColortex(prep.active ? prep.preprocessed : prep.expanded, colortex);
            written.insert(prep.targets.begin(), prep.targets.end());
        }
        if (std::any_of(wanted.begin(), wanted.end(), [](const auto& entry) { return entry.second; })) {
            // Every option defined, each right after the #version line.
            const std::string_view source = GbufferF0Glsl;
            const auto firstLine = source.find('\n') + 1;
            std::string defines;
            for (const auto& [name, colortex] : { std::pair{ "LINKSPAN_F0_NORMAL", GbufferF0Normal },
                                                  std::pair{ "LINKSPAN_F0_MATERIAL", GbufferF0Material },
                                                  std::pair{ "LINKSPAN_F0_TRANSLUCENT", GbufferF0Translucent } })
                defines += "#define " + std::string(name) + (wanted.contains(colortex) && wanted.at(colortex) ? " 1\n" : " 0\n");
            withGbuffer.push_back({ std::string(GbufferF0Name), std::string(GbufferF0File),
                                    std::string(source.substr(0, firstLine)) + defines + std::string(source.substr(firstLine)),
                                    {}, {} });
            withGbuffer.insert(withGbuffer.end(), packPrograms.begin(), packPrograms.end());
            prepared.insert(prepared.begin(), Prepared{});
            if (!prepare(withGbuffer.front(), prepared.front())) return false;
            if (withGbuffer.size() > maxPrograms) {
                tooMany();
                return false;
            }
        }
    }
    const auto& programs = withGbuffer.empty() ? packPrograms : withGbuffer;
    std::map<int, std::pair<size_t, size_t>> lastWriter; // colortex -> (program, output)
    for (size_t index = 0; index < programs.size(); ++index)
        for (size_t k = 0; k < prepared[index].targets.size(); ++k) lastWriter[prepared[index].targets[k]] = { index, k };
    if (material) {
        for (int index : { material->normal, material->specular }) {
            const bool hasCustomTexture = std::any_of(textures.begin(), textures.end(), [index](const IrisTexture& t) {
                const auto* alias = FindSampler(t.buffer);
                return alias && alias->colortex == index;
            });
            if (lastWriter.contains(index) || kept.contains(index) || hasCustomTexture) {
                error = "linkspan.materialGbuffer colide com colortex" + std::to_string(index) +
                        " (escrita, histórico ou textura do pack).";
                return false;
            }
        }
    }
    // Custom textures the graph reads: manifest name per file (one host texture per file), in first-use order.
    std::map<std::string, std::string> textureNames;
    nlohmann::json textureList = nlohmann::json::array();
    const auto useTexture = [&](size_t index) -> std::optional<std::string> {
        const auto& file = textures[index].file;
        if (const auto found = textureNames.find(file); found != textureNames.end()) return found->second;
        if (textureNames.size() >= MaxPackTextures) return std::nullopt;
        // The file stem, in the characters the host accepts, made unique.
        auto stem = file.substr(file.find_last_of('/') + 1);
        stem = stem.substr(0, stem.find_last_of('.'));
        std::string name;
        for (const char c : stem.substr(0, 48)) {
            const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            name.push_back(std::isalnum(static_cast<unsigned char>(lower)) || lower == '_' || lower == '-' ? lower : '_');
        }
        if (name.empty()) name = "texture";
        const auto base = name;
        for (int suffix = 2; std::any_of(textureNames.begin(), textureNames.end(),
                                         [&](const auto& entry) { return entry.second == name; });
             ++suffix)
            name = base + "-" + std::to_string(suffix);
        textureNames.emplace(file, name);
        // Iris uploads the PNG with its first row at t = 0; the sampling helpers flip y for the host targets, so the
        // host stores the file bottom-up.
        textureList.push_back({ { "name", name }, { "file", file }, { "flip", true } });
        return name;
    };
    std::map<int, std::string> producer{ { 0, "scene" } };
    if (material) {
        producer[material->normal] = MaterialSeedName;
        producer[material->specular] = std::string(MaterialSeedName) + "#1";
    }
    nlohmann::json passes = nlohmann::json::array();
    std::vector<std::string> sources, vertexSources;
    std::set<size_t> persistent;
    bool shaderModel5 = false;
    std::vector<int> lastTargets;
    for (size_t index = 0; index < programs.size(); ++index) {
        const auto& program = programs[index];
        const auto& expanded = prepared[index].expanded;
        const auto& preprocessed = prepared[index].preprocessed;
        const bool active = prepared[index].active;
        std::vector<int> targets = prepared[index].targets;
        std::string ignored;
        // The program's .vsh runs only on the V3 bridge; without it the host's fullscreen vertex shader feeds the UV.
        const bool packVertex = host.v3 && !program.vertexSource.empty();
        Varyings varyings;
        std::string vertexExpanded, vertexPreprocessed;
        bool vertexActive = false;
        if (packVertex) {
            if (!Expand(program.vertexSource, program.vertexFile, includes, 0, vertexExpanded, error)) return false;
            vertexActive = PreprocessGlsl("#version 450\n" + WithoutVersion(vertexExpanded), program.vertexFile,
                                          vertexPreprocessed, ignored);
            // The outputs the active vertex code declares, before the fragment inputs are rewritten.
            Rewrite scan;
            scan.stage = Stage::Vertex;
            Varyings declared;
            scan.varyings = &declared;
            const std::string_view text = vertexActive ? vertexPreprocessed : vertexExpanded;
            for (size_t at = 0; at < text.size();) {
                auto end = text.find('\n', at);
                if (end == text.npos) end = text.size();
                std::string unused;
                RewriteInput(Trim(text.substr(at, end - at)), scan, unused);
                at = end + 1;
            }
            for (const auto& [name, location] : declared.locations) varyings.written.insert(name);
        }
        // Custom textures of this program's stage (the last declaration of a buffer wins, as in a properties file):
        // colortex index (or NoiseTexture) -> entry of textures. The repeating ones go through the wrap helpers.
        std::map<int, size_t> overrides;
        std::set<std::string> wrapped{ "noisetex" };
        if (host.textures) {
            const bool deferred = program.name.starts_with("deferred");
            const bool composite = program.name.starts_with("composite") || program.name == "final";
            for (size_t t = 0; t < textures.size(); ++t) {
                const auto& texture = textures[t];
                if (texture.stage == "noise") {
                    overrides[NoiseTexture] = t;
                } else if ((texture.stage == "deferred" && deferred) || (texture.stage == "composite" && composite)) {
                    const auto* alias = FindSampler(texture.buffer);
                    if (alias && alias->colortex >= 0) overrides[alias->colortex] = t;
                }
            }
            for (const auto& [colortex, t] : overrides) {
                if (textures[t].clamp) continue;
                for (const auto& alias : Samplers)
                    if (alias.colortex == colortex) wrapped.emplace(alias.name);
            }
            if (const auto noise = overrides.find(NoiseTexture); noise != overrides.end() && textures[noise->second].clamp)
                wrapped.erase("noisetex");
        }
        Rewrite rewrite;
        rewrite.customs = &customs;
        if (packVertex) rewrite.varyings = &varyings;
        if (!RewriteUniforms(expanded, active ? std::string_view(preprocessed) : std::string_view(expanded), program.name,
                             host, rewrite, error))
            return false;
        // The gl_FragCoord flip needs the screen height.
        if (HasToken(rewrite.body, "gl_FragCoord")) rewrite.needs |= NeedSize;
        if (HasToken(rewrite.body, "gl_Fog")) rewrite.needs |= NeedFog;
        if ((rewrite.needs & NeedSize) && rewrite.samplers.empty()) rewrite.samplers.emplace_back("colortex0");
        WrapTextures(rewrite.body, wrapped);
        const bool gather = HasToken(rewrite.body, "textureGather") || HasToken(rewrite.body, "textureGatherOffset");
        if (gather && !host.v3) {
            error = program.name + ": textureGather pede Shader Model 5; atualize o host para a ponte Policroma V3.";
            return false;
        }
        shaderModel5 |= gather;
        std::string vertexSource;
        if (packVertex) {
            // Same texture slots as the fragment stage (the vertex shader may read more) and the same varyings.
            Rewrite vertex;
            vertex.customs = &customs;
            vertex.stage = Stage::Vertex;
            vertex.varyings = &varyings;
            vertex.samplers = rewrite.samplers;
            if (!RewriteUniforms(vertexExpanded,
                                 vertexActive ? std::string_view(vertexPreprocessed) : std::string_view(vertexExpanded),
                                 program.vertexFile, host, vertex, error))
                return false;
            if ((vertex.needs & NeedSize) && vertex.samplers.empty()) vertex.samplers.emplace_back("colortex0");
            WrapTextures(vertex.body, wrapped);
            const bool vertexGather =
                HasToken(vertex.body, "textureGather") || HasToken(vertex.body, "textureGatherOffset");
            shaderModel5 |= vertexGather;
            rewrite.samplers = vertex.samplers;
            vertexSource = Header(vertex, false, vertexGather, host) + "#line 1\n" + WithoutVersion(vertex.body);
        }

        nlohmann::json inputs = nlohmann::json::array();
        for (const auto& name : rewrite.samplers) {
            const auto* alias = FindSampler(name);
            if (const auto custom = overrides.find(alias->colortex); custom != overrides.end()) {
                const auto texture = useTexture(custom->second);
                if (!texture) {
                    error = program.name + ": o pack lê mais de " + std::to_string(MaxPackTextures) +
                            " texturas próprias (shaders.properties); a ponte V3 aceita " +
                            std::to_string(MaxPackTextures) + ".";
                    return false;
                }
                inputs.push_back("texture:" + *texture);
                continue;
            }
            if (alias->colortex == DepthTexture) {
                inputs.push_back("scene.depth");
                continue;
            }
            if (alias->colortex == ShadowTexture) {
                if (!host.v3) {
                    error = program.name + ": " + name + " pede a ponte Policroma V3; atualize o host.";
                    return false;
                }
                inputs.push_back("one");
                continue;
            }
            if (alias->colortex == NoiseTexture) {
                if (!host.v3) {
                    error = program.name + ": noisetex pede a ponte Policroma V3; atualize o host.";
                    return false;
                }
                inputs.push_back("scene.noise");
                continue;
            }
            const auto found = producer.find(alias->colortex);
            const auto writer = lastWriter.find(alias->colortex);
            if (found != producer.end()) {
                inputs.push_back(found->second);
            } else if (host.v3 && kept.contains(alias->colortex) && writer != lastWriter.end() &&
                       writer->second.first + 1 < programs.size()) {
                // A buffer the pack keeps (colortexNClear = false) and no earlier program wrote this frame holds
                // what its last writer left in the previous frame (TAA history, say). The host keeps that pass's
                // previous output; the final pass cannot keep one.
                const auto [pass, output] = writer->second;
                persistent.insert(pass);
                inputs.push_back("history:" + programs[pass].name + (output ? "#" + std::to_string(output) : ""));
            } else if (host.v3) {
                // Iris clears the buffers no earlier program wrote; here they read as zero.
                inputs.push_back("zero");
            } else {
                error = program.name + ": lê colortex" + std::to_string(alias->colortex) +
                        " antes de algum programa escrevê-lo (a ponte Policroma V3 do host lê como zero).";
                return false;
            }
        }
        const bool fragData = HasToken(rewrite.body, "gl_FragData") || HasToken(rewrite.body, "gl_FragColor");
        std::string source = Header(rewrite, fragData, gather, host) + "#line 1\n" + WithoutVersion(rewrite.body);
        if (fragData) {
            source += "#undef main\n";
            for (size_t k = 0; k < targets.size(); ++k)
                source += "layout(location = " + std::to_string(k) + ") out vec4 policroma_out" + std::to_string(k) + ";\n";
            source += "void main() {\n    policroma_iris_main();\n";
            for (size_t k = 0; k < targets.size(); ++k)
                source += "    policroma_out" + std::to_string(k) + " = policroma_FragData[" + std::to_string(k) + "];\n";
            source += "}\n";
        }
        nlohmann::json pass{ { "name", program.name }, { "file", program.file }, { "divisor", 1 }, { "inputs", inputs } };
        if (targets.size() > 1) pass["outputs"] = targets.size();
        if (packVertex) pass["vertex"] = program.vertexFile;
        passes.push_back(std::move(pass));
        sources.push_back(std::move(source));
        vertexSources.push_back(std::move(vertexSource));
        for (size_t k = 0; k < targets.size(); ++k)
            producer[targets[k]] = k ? program.name + "#" + std::to_string(k) : program.name;
        lastTargets = std::move(targets);
    }
    if (programs.back().name != "final" && (lastTargets.size() != 1 || lastTargets[0] != 0)) {
        if (passes.size() >= maxPrograms) {
            error = "Sem final.fsh, o último programa precisa escrever só o colortex0 (não sobra passe para apresentá-lo).";
            return false;
        }
        passes.push_back({ { "name", "final" }, { "file", "shaders/final.fsh" }, { "divisor", 1 },
                           { "inputs", nlohmann::json::array({ producer.at(0) }) } });
        sources.emplace_back(PresentShader);
        vertexSources.emplace_back();
    }
    for (const auto pass : persistent) passes[pass]["persistent"] = true;
    if (material) {
        passes.insert(passes.begin(), nlohmann::json{
            { "name", MaterialSeedName }, { "file", "shaders/linkspan-material.hlsl" }, { "outputs", 2 },
            { "inputs", { "scene.material.normal", "scene.material.specular", "scene.depth" } }
        });
        sources.insert(sources.begin(), MaterialSeedHlsl);
        vertexSources.insert(vertexSources.begin(), std::string{});
    }
    nlohmann::json root{ { "version", host.v3 ? 3 : 2 }, { "passes", passes } };
    if (shaderModel5) root["shaderModel"] = 5;
    if (!textureList.empty()) root["textures"] = std::move(textureList);
    manifest = root.dump();
    glsl = std::move(sources);
    if (vertexGlsl) *vertexGlsl = std::move(vertexSources);
    return true;
}

} // namespace ShaderPacks
