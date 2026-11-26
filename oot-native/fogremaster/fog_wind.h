#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Cópia das fórmulas puras do WindField do host (soh/soh/Enhancements/Graphics/Weather/wind_field.h, R054).
// O mod não depende de serviço novo: lê o vento da cena no envCtx e reproduz a base, a brisa ambiente e a
// rajada. Mantenha os números iguais aos do host; o teste fog_wind compara com valores calculados por ele.
namespace fogwind {
inline constexpr float MaxSpeed = 60.0f;
inline constexpr float GustAmplitude = 0.6f;
inline constexpr float GustWavelength = 700.0f;
inline constexpr float GustFrontSpeed = 1.3f;
// Vento fixo da FogreMaster 0.25.0, usado quando o host desliga o campo de vento.
inline constexpr float LegacyX = 16.0f;
inline constexpr float LegacyZ = 5.0f;

inline float Finite(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

// Só o plano, como no host: o y do windDirection é direção de efeito, não corrente ascendente.
inline std::array<float, 3> BaseVelocity(const std::array<float, 3>& direction, float speed) {
    const float x = Finite(direction[0]), z = Finite(direction[2]);
    const float length = std::sqrt(x * x + z * z);
    float s = Finite(speed);
    s = s < 0.0f ? 0.0f : (s > 255.0f ? 255.0f : s);
    if (length < 0.0001f || s <= 0.0f)
        return {};
    const float scale = (s / 255.0f) * MaxSpeed / length;
    return {x * scale, 0.0f, z * scale};
}

inline bool AmbientSky(int skybox) {
    switch (skybox) {
        case 0x01: // SKYBOX_NORMAL_SKY
        case 0x03: // SKYBOX_OVERCAST_SUNSET
        case 0x04: // SKYBOX_MARKET_ADULT
        case 0x05: // SKYBOX_CUTSCENE_MAP
        case 0x09: // SKYBOX_MARKET_CHILD_DAY
        case 0x0A: // SKYBOX_MARKET_CHILD_NIGHT
        case 0x1D: // SKYBOX_UNSET_1D (Kokiri)
            return true;
        default:
            return false;
    }
}

inline float EffectiveSpeed(float sceneSpeed, bool hasSky, float ambient) {
    const float s = Finite(sceneSpeed);
    if (s > 0.0f)
        return s;
    return hasSky ? Finite(ambient) : 0.0f;
}

inline float Gust(float x, float z, float seconds, const std::array<float, 3>& velocity) {
    const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[2] * velocity[2]);
    if (speed < 0.001f)
        return 1.0f;
    const float dx = velocity[0] / speed, dz = velocity[2] / speed;
    const float along = x * dx + z * dz;
    const float across = -x * dz + z * dx;
    const float phase = (along - GustFrontSpeed * speed * seconds) / GustWavelength +
                        0.15f * std::sin(across / 450.0f + 0.31f * seconds);
    const float front = 0.5f + 0.5f * std::sin(6.2831853f * phase);
    const float envelope = 0.5f + 0.5f * std::sin(0.37f * seconds + 1.7f * std::sin(0.13f * seconds));
    return 1.0f + GustAmplitude * (2.0f * front * envelope - 0.5f);
}

struct Scene {
    bool hostWind = true;          // gDeveloperTools.WindField
    std::array<float, 3> direction{}; // envCtx.windDirection
    float speed = 0.0f;            // envCtx.windSpeed, 0..255
    int skybox = 0;                // play->skyboxId
    float ambient = 40.0f;         // gDeveloperTools.WindAmbient
    int override = -1;             // gDeveloperTools.WindSpeedOverride (-1 = sem)
};

// Velocidade base da cena (u/s, plano xz em [0] e [2]), sem rajada. Campo desligado no host = vento fixo antigo.
inline std::array<float, 3> Base(const Scene& scene) {
    if (!scene.hostWind)
        return {LegacyX, 0.0f, LegacyZ};
    float speed = EffectiveSpeed(scene.speed, AmbientSky(scene.skybox), scene.ambient);
    if (scene.override >= 0)
        speed = static_cast<float>(scene.override);
    return BaseVelocity(scene.direction, speed);
}

// Portador horizontal da névoa no ponto (x, z) e no relógio do vento (segundos de jogo na cena).
// O vento fixo antigo não leva rajada, para o caminho de fallback ser idêntico à 0.25.0.
inline std::array<float, 2> Carrier(const Scene& scene, float x, float z, float seconds) {
    const auto base = Base(scene);
    if (!scene.hostWind)
        return {base[0], base[2]};
    const float g = Gust(x, z, seconds, base);
    return {base[0] * g, base[2] * g};
}
inline constexpr float TickSeconds = 1.0f / 20.0f;

// Relógio da rajada com a política do WindField::State do host: zera com o campo desligado ou sem sessão,
// reinicia por sessão, cena ou retrocesso de tick (não por sala nem pela ativação da neblina) e avança um
// passo por tick novo fora da pausa. Render repetido ou salto de contador não recupera tempo.
struct Clock {
    double seconds = 0.0;
    std::uintptr_t session = 0;
    int scene = 0;
    std::uint32_t tick = 0;
    bool initialized = false;

    float Step(std::uintptr_t nextSession, int nextScene, std::uint32_t nextTick, bool paused, bool enabled) {
        if (nextSession == 0 || !enabled) {
            *this = {};
            return 0.0f;
        }
        const bool reset = !initialized || nextSession != session || nextScene != scene ||
                           static_cast<std::uint32_t>(nextTick - tick) > 0x7fffffffu;
        const bool advance = !reset && nextTick != tick;
        if (reset)
            seconds = 0.0;
        initialized = true;
        session = nextSession;
        scene = nextScene;
        tick = nextTick;
        if (advance && !paused)
            seconds += static_cast<double>(TickSeconds);
        return static_cast<float>(seconds);
    }
};

} // namespace fogwind
