#include "Catalog.h"
#include "BridgeSelection.h"
#include "shiplua/native/ship_native_abi.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

namespace {

struct Policroma {
    ShaderPacks::BridgeSelection bridge;
    std::filesystem::path root;
    bool active = false;
    // Iris cloudHeight in OoT units, from the menu ("parameters").
    float cloudHeight = ShaderPacks::DefaultIrisCloudHeight;
    // Fog calibration: blocks per OoT unit, OoT height of the floor (0: no offset) and the sky light under the eye.
    // The defaults (k = 1/32, floor 0, open sky) match the Lua ones, used when the menu storage has no value.
    float fogScale = ShaderPacks::DefaultIrisFogScale;
    float groundY = ShaderPacks::DefaultIrisGroundY;
    float skyLight = ShaderPacks::DefaultIrisSkyLight;
    // "Reconstructed normals" (F0, GbufferF0.h): 0 off, 1 on. Changes the graph, so it only applies when a pack loads.
    float gbufferF0 = 0.0f;
};

// The provider parameters the host reads every frame (LinkSpanIris vector 11): x = cloudHeight, y = k (blocks per OoT
// unit), z = groundY, w = eyeBrightness sky (0..240). The host zeroes what the provider does not send, so all four go.
void SendParameters(const Policroma& mod) {
    // sky_light = -1 (automatic) needs the host's sky vector; a host without it gets open air, the 0.7.8 shader's input.
    const float sky = mod.skyLight < 0.0f && !mod.bridge.HasSkyExposure() ? ShaderPacks::MaxIrisSkyLight : mod.skyLight;
    const float values[4]{ mod.cloudHeight, mod.fogScale, mod.groundY, sky };
    mod.bridge.SetParameters(values, 4);
}

// One "parameters" key: the member it sets and the range the provider accepts.
struct ParameterKey {
    std::string_view name;
    float Policroma::*member;
    double minimum;
    double maximum;
    double positiveMinimum; // a value above 0 may not fall below it (fog_scale: 0 is automatic, 0 < k < 1/256 is not)
    std::string_view rangeError;
    bool atLoad = false; // changes the pack's graph: the active pack has to be loaded again
};

constexpr ParameterKey ParameterKeys[] = {
    { "cloud_height=", &Policroma::cloudHeight, -4000.0, 20000.0, -4000.0, "Altura das nuvens fora de -4000..20000." },
    { "fog_scale=", &Policroma::fogScale, ShaderPacks::AutoIrisFogScale, ShaderPacks::MaxIrisFogScale,
      ShaderPacks::MinIrisFogScale, "Escala da névoa fora de 0 (automática) ou 1/256..4." },
    { "ground_y=", &Policroma::groundY, -20000.0, 20000.0, -20000.0, "Altura do chão fora de -20000..20000." },
    { "sky_light=", &Policroma::skyLight, ShaderPacks::AutoIrisSkyLight, ShaderPacks::MaxIrisSkyLight,
      ShaderPacks::AutoIrisSkyLight, "Luz do céu fora de -1 (automática) ou 0..240." },
    { "gbuffer_f0=", &Policroma::gbufferF0, 0.0, 1.0, 0.0, "Normais reconstruídas fora de 0..1.", true },
};
constexpr float GbufferF0On = 0.5f; // gbufferF0 at or above it turns the F0 program on

std::string Utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

PolicromaText Text(std::string_view value) {
    return {value.data(), static_cast<uint32_t>(value.size())};
}

ShipNativeStatus Reply(ShipNativeWriteFn write, void* writer, std::string_view result) {
    return write(writer, result.data(), static_cast<uint32_t>(result.size()));
}

std::string_view Kind(const ShaderPacks::Pack& pack) {
    switch (pack.format) {
        case ShaderPacks::Format::LinkSpan: return "Link-Span";
        case ShaderPacks::Format::Iris: return "Iris";
        case ShaderPacks::Format::Unreadable: return "Ilegível";
        case ShaderPacks::Format::Unknown: return "Desconhecido";
    }
    return "Desconhecido";
}

ShipNativeStatus SHIP_NATIVE_CALL List(void* user, const char*, uint32_t, ShipNativeWriteFn write, void* writer) {
    try {
        const auto& mod = *static_cast<const Policroma*>(user);
        std::error_code error;
        const auto packs = ShaderPacks::Scan(mod.root, error);
        if (error) return Reply(write, writer, "erro\nFalha ao abrir a pasta shaderpacks.");
        std::string output = "ok\n";
        for (const auto& pack : packs) {
            const auto name = Utf8(pack.path.filename());
            const auto kind = Kind(pack);
            if (name.find_first_of("\t\r\n") != name.npos) continue;
            if (output.size() + kind.size() + name.size() + 2 > SHIP_NATIVE_MAX_BYTES)
                return Reply(write, writer, "erro\nO catálogo excede o limite de resposta.");
            output += kind;
            output += '\t';
            output += name;
            output += '\n';
        }
        return Reply(write, writer, output);
    } catch (...) {
        return Reply(write, writer, "erro\nFalha ao catalogar shaderpacks.");
    }
}

bool Submit(Policroma& mod, std::string_view name, const std::vector<ShaderPacks::PassSource>& sources,
            const ShaderPacks::NativePackMetadata& metadata, char* diagnostic, uint32_t capacity) {
    std::vector<PolicromaPass> passes;
    for (const auto& source : sources) {
        PolicromaPass pass{{Text(source.file), Text(source.source)}, source.divisor, {}};
        // A struct leva as 4 entradas legadas; com manifesto (v2/v3) o host lê todas do JSON.
        std::copy_n(source.inputs.begin(), PolicromaMaxInputs, pass.inputs);
        passes.push_back(pass);
    }
    std::vector<PolicromaSource> includes;
    if (const auto& files = sources.front().includes) {
        for (const auto& [path, source] : *files) includes.push_back({Text(path), Text(source)});
    }
    // V3: o vertex shader de cada passe vai como include, no caminho do campo "vertex" do manifesto.
    for (const auto& source : sources) {
        if (!source.vertexSource.empty()) includes.push_back({Text(source.vertexFile), Text(source.vertexSource)});
    }
    std::vector<PolicromaMaterial> materials;
    if (const auto& rules = sources.front().transmissionMaterials) {
        for (const auto& rule : *rules) {
            PolicromaMaterial material{rule.scene, Text(rule.texture), {}, {}};
            std::copy(rule.tint.begin(), rule.tint.end(), material.tint);
            std::copy(rule.light.begin(), rule.light.end(), material.light);
            materials.push_back(material);
        }
    }
    const PolicromaProgram program{
        Text(name), passes.data(), includes.data(), materials.data(), static_cast<uint32_t>(passes.size()),
        static_cast<uint32_t>(includes.size()), static_cast<uint32_t>(materials.size())};
    return mod.bridge.Activate(program, metadata, diagnostic, capacity);
}

ShipNativeStatus SHIP_NATIVE_CALL Load(void* user, const char* request, uint32_t length, ShipNativeWriteFn write,
                                      void* writer) {
    try {
        auto& mod = *static_cast<Policroma*>(user);
        const std::string_view selection(request, length);
        const auto separator = selection.find('\n');
        const auto name = selection.substr(0, separator);
        const auto profile = separator == selection.npos ? std::string_view{} : selection.substr(separator + 1);
        if (name.empty() || name.size() > 1024) return Reply(write, writer, "erro\nNome de pack inválido.");
        if (profile.size() > 32) return Reply(write, writer, "erro\nNome de perfil inválido.");

        std::error_code error;
        const auto packs = ShaderPacks::Scan(mod.root, error);
        if (error) return Reply(write, writer, "erro\nFalha ao abrir a pasta shaderpacks.");
        const auto pack = std::find_if(packs.begin(), packs.end(), [name](const auto& item) {
            return Utf8(item.path.filename()) == name;
        });
        if (pack == packs.end()) return Reply(write, writer, "erro\nPack ausente em shaderpacks.");
        if (pack->format != ShaderPacks::Format::LinkSpan && pack->format != ShaderPacks::Format::Iris)
            return Reply(write, writer, "erro\nFormato de shaderpack indisponível.");

        std::vector<ShaderPacks::PassSource> sources;
        ShaderPacks::ProfileSelection profiles;
        ShaderPacks::NativePackMetadata metadata;
        std::string details;
        if (pack->format == ShaderPacks::Format::Iris) {
            if (!profile.empty()) return Reply(write, writer, "erro\nPacks Iris não declaram perfis Link-Span.");
            if (!ShaderPacks::ReadIrisPack(pack->path, sources, metadata, details, {mod.bridge.HasV3(), mod.bridge.HasPackTextures(),
                                                                                  mod.bridge.HasParameters(), mod.cloudHeight,
                                                                                  mod.gbufferF0 >= GbufferF0On,
                                                                                  mod.bridge.HasFogRamp(), mod.bridge.HasSkyExposure()}))
                return Reply(write, writer, "erro\nIris reduzido: " + details);
        } else if (!ShaderPacks::ReadNativePack(pack->path, sources, details, profile, &profiles, &metadata)) {
            return Reply(write, writer, "erro\n" + details);
        }
        std::string output = "ok\n" + profiles.active + '\n';
        for (const auto& [id, label] : profiles.choices) output += id + '\t' + label + '\n';
        char diagnostic[2048]{};
        if (!Submit(mod, name, sources, metadata, diagnostic, sizeof(diagnostic)))
            return Reply(write, writer, std::string("erro\n") + diagnostic);

        mod.active = true;
        SendParameters(mod);
        return Reply(write, writer, output);
    } catch (...) {
        return Reply(write, writer, "erro\nFalha ao carregar o shaderpack.");
    }
}

// "<key>=<number>", keys of ParameterKeys: cloud_height (OoT units), fog_scale (k), ground_y, sky_light, gbuffer_f0 (0|1). Replies "ok\nlive" when the host applies it from
// the next frame, or "ok\nreload" when the active pack must be loaded again to see it (host without provider
// parameters, or gbuffer_f0, which changes the graph).
ShipNativeStatus SHIP_NATIVE_CALL Parameters(void* user, const char* request, uint32_t length, ShipNativeWriteFn write,
                                              void* writer) {
    try {
        auto& mod = *static_cast<Policroma*>(user);
        const std::string text(request, length);
        const auto parameter = std::find_if(std::begin(ParameterKeys), std::end(ParameterKeys),
                                            [&text](const ParameterKey& key) { return text.starts_with(key.name); });
        if (parameter == std::end(ParameterKeys)) return Reply(write, writer, "erro\nParâmetro desconhecido.");
        char* end = nullptr;
        const auto value = std::strtod(text.c_str() + parameter->name.size(), &end);
        if (end == text.c_str() + parameter->name.size() || *end != '\0' || !std::isfinite(value) ||
            value < parameter->minimum || value > parameter->maximum ||
            (value > 0.0 && value < parameter->positiveMinimum))
            return Reply(write, writer, "erro\n" + std::string(parameter->rangeError));
        mod.*(parameter->member) = static_cast<float>(value);
        SendParameters(mod);
        return Reply(write, writer, mod.bridge.HasParameters() && !parameter->atLoad ? "ok\nlive" : "ok\nreload");
    } catch (...) {
        return Reply(write, writer, "erro\nFalha ao aplicar o parâmetro.");
    }
}

void Clear(Policroma& mod) {
    if (std::exchange(mod.active, false)) mod.bridge.Off();
}

ShipNativeStatus SHIP_NATIVE_CALL Off(void* user, const char*, uint32_t, ShipNativeWriteFn write, void* writer) {
    Clear(*static_cast<Policroma*>(user));
    return Reply(write, writer, "ok\nShaderpacks desligados.");
}

ShipNativeStatus SHIP_NATIVE_CALL Init(const ShipNativeRuntime* runtime, void** instance) {
    if (!runtime || !instance || runtime->size < sizeof(*runtime) || !runtime->register_function)
        return SHIP_NATIVE_UNSUPPORTED;
    *instance = nullptr;
    try {
        // GetProcAddress não garante tipos de função compatíveis com casts GCC.
        const auto module = GetModuleHandleW(nullptr);
        const auto v3Address = GetProcAddress(module, "LinkSpan_GetPolicromaBridgeV3");
        const auto v2Address = GetProcAddress(module, "LinkSpan_GetPolicromaBridgeV2");
        const auto v1Address = GetProcAddress(module, "LinkSpan_GetPolicromaBridgeV1");
        GetPolicromaBridgeV3 getV3 = nullptr;
        GetPolicromaBridgeV2 getV2 = nullptr;
        GetPolicromaBridgeV1 getV1 = nullptr;
        static_assert(sizeof(getV3) == sizeof(v3Address) && sizeof(getV2) == sizeof(v2Address) &&
                      sizeof(getV1) == sizeof(v1Address));
        std::memcpy(&getV3, &v3Address, sizeof(getV3));
        std::memcpy(&getV2, &v2Address, sizeof(getV2));
        std::memcpy(&getV1, &v1Address, sizeof(getV1));
        const ShaderPacks::BridgeSelection bridge(getV1 ? getV1() : nullptr, getV2 ? getV2() : nullptr,
                                                  getV3 ? getV3() : nullptr);
        if (!bridge.Available()) return SHIP_NATIVE_UNSUPPORTED;

        const uint32_t size = bridge.AppDirectory(nullptr, 0);
        if (!size || size > 32768) return SHIP_NATIVE_UNSUPPORTED;
        std::u8string directory(size, u8'\0');
        if (bridge.AppDirectory(reinterpret_cast<char*>(directory.data()), size) != size)
            return SHIP_NATIVE_FAILURE;
        directory.pop_back();
        auto mod = std::make_unique<Policroma>(Policroma{
            bridge, std::filesystem::absolute(std::filesystem::path(directory)) / "shaderpacks"});
        for (const auto& [name, function] : {std::pair{"list", &List}, {"load", &Load}, {"off", &Off}, {"parameters", &Parameters}}) {
            const auto status = runtime->register_function(runtime->context, name, function, mod.get());
            if (status != SHIP_NATIVE_OK) return status;
        }
        *instance = mod.release();
        return SHIP_NATIVE_OK;
    } catch (...) {
        return SHIP_NATIVE_FAILURE;
    }
}

void SHIP_NATIVE_CALL Shutdown(void* instance) {
    std::unique_ptr<Policroma> mod(static_cast<Policroma*>(instance));
    if (mod) Clear(*mod);
}

} // namespace

extern "C" SHIP_NATIVE_EXPORT const ShipNativeDescriptor* SHIP_NATIVE_CALL ShipNative_Query() {
    static const ShipNativeDescriptor descriptor{sizeof(ShipNativeDescriptor), SHIP_NATIVE_ABI_MAJOR, 4, Init, Shutdown};
    return &descriptor;
}
