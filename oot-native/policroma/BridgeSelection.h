#pragma once

#include "CatalogManifest.h"
#include "PolicromaBridge.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace ShaderPacks {

class BridgeSelection {
  public:
    BridgeSelection(const PolicromaBridgeV1* legacy, const PolicromaBridgeV2* modern,
                    const PolicromaBridgeV3* latest = nullptr)
        : v1(Valid(legacy) ? legacy : nullptr), v2(Valid(modern) ? modern : nullptr),
          v3(ValidV3(latest) && latest->maxPasses >= PolicromaV3MaxPasses && latest->maxInputs >= PolicromaV3MaxInputs &&
                     latest->maxOutputs >= PolicromaV3MaxOutputs
                 ? latest
                 : nullptr) {}

    bool Available() const { return v1 || v2 || v3; }
    bool HasV3() const { return v3 != nullptr; }
    // RFC 0035: manifest "textures" and the repeat sampler s1.
    bool HasPackTextures() const { return v3 && (v3->features & PolicromaFeaturePackTextures); }
    // b10 clock.zw = the native fog ramp in "u" (0.7.8 automatic k).
    bool HasFogRamp() const { return v3 && (v3->features & PolicromaFeatureFogRamp); }
    // b10 vector 12 "sky" (0.7.9 automatic sky light): x = openness 0..1 smoothed by the host.
    bool HasSkyExposure() const { return v3 && (v3->features & PolicromaFeatureSkyExposure); }
    // RFC 0035: setParameters (vetor 11 do LinkSpanIris), ao vivo e sem recompilar o pack.
    bool HasParameters() const {
        return v3 && (v3->features & PolicromaFeatureProviderParameters) &&
               v3->size >= offsetof(PolicromaBridgeV3, setParameters) + sizeof(v3->setParameters) && v3->setParameters;
    }
    void SetParameters(const float* values, uint32_t count) const {
        if (HasParameters()) v3->setParameters(values, count);
    }
    uint32_t AppDirectory(char* output, uint32_t capacity) const {
        return v3 ? v3->appDirectory(output, capacity)
             : v2 ? v2->appDirectory(output, capacity)
             : v1 ? v1->appDirectory(output, capacity)
                  : 0;
    }
    bool Activate(const PolicromaProgram& program, const NativePackMetadata& metadata,
                  char* error, uint32_t capacity) {
        const PolicromaText manifest{metadata.manifest.data(), static_cast<uint32_t>(metadata.manifest.size())};
        if (v3 && !metadata.manifest.empty()) {
            const bool ok = v3->activate(&program, manifest, error, capacity);
            if (ok) active = 3;
            return ok; // A V3 aceita v2 e v3; falha não autoriza downgrade silencioso.
        }
        if (metadata.requiresV3) {
            return Fail(error, capacity,
                "Este pack exige LinkSpan_GetPolicromaBridgeV3 (RFC 0035: 16 passes, 8 entradas, várias saídas); "
                "atualize o host.");
        }
        if (v2 && !metadata.manifest.empty()) {
            const bool ok = v2->activate(&program, manifest, error, capacity);
            if (ok) active = 2;
            return ok; // Falha de compilação/contrato V2 não autoriza downgrade silencioso.
        }
        if (metadata.requiresV2) {
            return Fail(error, capacity,
                "Este pack exige LinkSpan_GetPolicromaBridgeV2 (size/persistent/history); atualize o host.");
        }
        if (!v1) return Fail(error, capacity, "Ponte Policroma V1 indisponível para este pack legado.");
        const bool ok = v1->activate(&program, error, capacity);
        if (ok) active = 1;
        return ok;
    }
    void Off() const {
        if (active == 3 && v3) v3->off();
        else if (active == 2 && v2) v2->off();
        else if (v1) v1->off();
        else if (v2) v2->off();
        else if (v3) v3->off();
    }

  private:
    template <typename Bridge> static bool Valid(const Bridge* bridge) {
        return bridge && bridge->size >= sizeof(Bridge) && bridge->appDirectory && bridge->activate && bridge->off;
    }
    // A V3 cresceu (setParameters): um host que exporta só até features continua valendo.
    static bool ValidV3(const PolicromaBridgeV3* bridge) {
        return bridge && bridge->size >= offsetof(PolicromaBridgeV3, setParameters) && bridge->appDirectory &&
               bridge->activate && bridge->off;
    }
    static bool Fail(char* output, uint32_t capacity, std::string_view message) {
        if (output && capacity) {
            const auto length = std::min<size_t>(message.size(), capacity - 1);
            std::memcpy(output, message.data(), length);
            output[length] = '\0';
        }
        return false;
    }
    const PolicromaBridgeV1* v1;
    const PolicromaBridgeV2* v2;
    const PolicromaBridgeV3* v3;
    int active = 0;
};

} // namespace ShaderPacks
