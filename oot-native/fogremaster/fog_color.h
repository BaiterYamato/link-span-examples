#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fogcolor {
using Rgb = std::array<float, 3>;
inline constexpr Rgb Legacy{0.73f, 0.80f, 0.84f};
inline constexpr float ResponseSeconds = 0.75f;
// 0.29.0: quanto a cor da cena cede para o branco-acinzentado da névoa, mantendo a exposição do momento. A cor da
// cena sozinha deixava a névoa da vila ocre, com cara de poeira.
inline constexpr float White = 0.70f;
inline constexpr float WhiteGain = 1.18f;

// Canais do OoT em 0..255, com os ajustes de ambiente já somados pelo adaptador.
struct Environment {
    Rgb ambient{}, directional{}, fog{};
};

inline float Channel(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 255.0f) : 0.0f;
}

// O shader recebe radiância ambiente, não albedo: o sol e as luzes locais são extras do host.
// A direcional é apenas uma referência de exposição/cromatismo; não somamos um segundo sol.
inline Rgb Target(const Environment& env) {
    Rgb energy{}, sceneFog{}, result{};
    float peak = 0.0f, fogPeak = 0.0f;
    for (unsigned i = 0; i < 3; ++i) {
        energy[i] = 0.75f * Channel(env.ambient[i]) + 0.25f * Channel(env.directional[i]);
        sceneFog[i] = Channel(env.fog[i]);
        peak = std::max(peak, energy[i]);
        fogPeak = std::max(fogPeak, sceneFog[i]);
    }
    // Platô diurno preserva a exposição aprovada; piso de 2,5% evita preto absoluto.
    const float exposure = std::clamp(peak / 100.0f, 0.025f, 1.0f);
    for (unsigned i = 0; i < 3; ++i) {
        const float lightTint = peak > 0.0f ? energy[i] / peak : 1.0f;
        const float fogTint = fogPeak > 0.0f ? sceneFog[i] / fogPeak : lightTint;
        const float tint = 0.75f * lightTint + 0.25f * fogTint;
        result[i] = Legacy[i] * exposure * (0.4f + 0.6f * tint);
    }
    return result;
}

// Puxa a cor para o tom neutro da névoa com o mesmo pico de brilho (vezes WhiteGain), sem passar de 1.
inline Rgb Whiten(const Rgb& color, float amount) {
    const float peak = std::max({color[0], color[1], color[2]});
    const float legacyPeak = std::max({Legacy[0], Legacy[1], Legacy[2]});
    Rgb result{};
    for (unsigned i = 0; i < 3; ++i) {
        const float neutral = std::min(Legacy[i] / legacyPeak * peak * WhiteGain, 1.0f);
        result[i] = std::lerp(color[i], neutral, std::clamp(amount, 0.0f, 1.0f));
    }
    return result;
}

class Color {
public:
    const Rgb& value() const { return value_; }
    void fixed() { value_ = Legacy; initialized_ = true; }

    void step(const Environment& env, float dt) {
        const auto target = Target(env);
        if (!initialized_) {
            value_ = target; // Primeira imagem noturna já começa escura.
            initialized_ = true;
            return;
        }
        if (!std::isfinite(dt) || dt <= 0.0f) return;
        // Suspensão/teleporte não recuperam tempo nem provocam salto de exposição.
        const float blend = -std::expm1(-std::min(dt, 0.1f) / ResponseSeconds);
        for (unsigned i = 0; i < 3; ++i) value_[i] += (target[i] - value_[i]) * blend;
    }

private:
    Rgb value_{Legacy};
    bool initialized_{};
};

// Uma amostra por tick; preservar Color entre cenas evita salto ao trocar sala/PlayState.
class FrameColor {
public:
    Color color;
    void invalidateFrame() { frameValid_ = false; }
    void update(const Environment& env, std::uintptr_t session, int scene, std::uint32_t tick,
                bool paused, bool adaptive) {
        const bool duplicate = frameValid_ && session == session_ && scene == scene_ && tick == tick_;
        session_ = session;
        scene_ = scene;
        tick_ = tick;
        frameValid_ = true;
        if (!adaptive) color.fixed();
        else if (!duplicate && !paused) color.step(env, 0.05f); // Política de 20 Hz do provider.
    }

private:
    std::uintptr_t session_{};
    int scene_{};
    std::uint32_t tick_{};
    bool frameValid_{};
};
} // namespace fogcolor
