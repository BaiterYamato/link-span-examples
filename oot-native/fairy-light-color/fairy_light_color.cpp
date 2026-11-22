// Fairy Light Color (GFX-UNIFY-001, playtest de 12/11): a luz pontual de cada fada toma a cor da aura dela.
//
// O jogo grava as duas luzes da fada (sem brilho e com brilho) sempre em branco (255, 255, 255); a cor da Navi
// (azul em repouso, amarela com inimigo, verde em objeto) fica só no sprite. No hook oot.light.fairy o host entrega
// a aura (EnElf::outerColor) e grava de volta a cor da luz; daqui a luz do jogo, o brilho em volta da fada e o pack
// de shaders (que lê play->lightCtx) passam a ter a cor dela. Conteúdo de jogo fica no mod, o host só oferece o hook.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>

#include "oot_engine.h"
#include "oot_hooks.h"
#include "oot_layout_id.h"

namespace {

// Parte de branco somada à aura: a azul pura (0, 0, 255) iluminaria tudo de anil.
constexpr float kWhiteMix = 0.30f;
// Abaixo disto a aura está apagada (fada sumindo): a luz fica como o jogo deixou.
constexpr float kDarkAura = 8.0f;
// Raio da luz com brilho que o jogo dá à Navi (EnElf_UpdateLights); usado nas outras fadas.
constexpr int16_t kFairyGlowRadius = 100;

struct State {
    bool enabled = true;
    uint64_t fairies = 0;
    uint64_t tinted = 0;
    uint64_t lit = 0;
    uint64_t deferred = 0;
    uint8_t lastNavi[3] = {255, 255, 255};
    float lastAura[3] = {0.0f, 0.0f, 0.0f};
};

// Cor da luz para uma aura 0..255: mistura com branco e normaliza o canal mais forte em 255, para a luz manter a
// energia da branca original.
bool LightColor(const float aura[3], uint8_t out[3]) {
    const float peak = std::max(aura[0], std::max(aura[1], aura[2]));
    if (!std::isfinite(peak) || peak < kDarkAura) return false;
    float mixed[3];
    for (int c = 0; c < 3; ++c) mixed[c] = std::clamp(aura[c], 0.0f, 255.0f) * (1.0f - kWhiteMix) + 255.0f * kWhiteMix;
    const float top = std::max(mixed[0], std::max(mixed[1], mixed[2]));
    for (int c = 0; c < 3; ++c) out[c] = static_cast<uint8_t>(std::lround(std::clamp(mixed[c] * 255.0f / top, 0.0f, 255.0f)));
    return true;
}

ShipNativeStatus SHIP_NATIVE_CALL TransformFairy(void* user, const ShipNativeHookCall* call) {
    auto& state = *static_cast<State*>(user);
    ++state.fairies;
    if (!call || !call->payload) return SHIP_NATIVE_OK;
    auto* fairy = static_cast<ShipOotFairyLightHookV1*>(call->payload);
    const bool navi = fairy->params == LINKSPAN_OOT_FAIRY_NAVI;
    if (!state.enabled) {
        // Desligado: as fadas que não são a Navi voltam ao que o jogo deixa (raio 0, branca). A Navi o jogo regrava
        // sozinho a cada quadro em EnElf_UpdateLights.
        if (!navi) {
            fairy->glow.radius = 0;
            for (auto* light : {&fairy->no_glow, &fairy->glow}) std::fill(light->color, light->color + 3, uint8_t{255});
        }
        return SHIP_NATIVE_OK;
    }
    uint8_t color[3];
    // Valores absolutos a cada update: o host avisa que a cor pode trazer o que um hook gravou antes.
    if (!LightColor(fairy->outer_color, color)) {
        // Fada que não é a Navi sumindo: apaga a luz que este mod acendeu.
        if (!navi) fairy->glow.radius = 0;
        return SHIP_NATIVE_OK;
    }
    for (auto* light : {&fairy->no_glow, &fairy->glow}) std::copy(color, color + 3, light->color);
    // Só a Navi tem as luzes atualizadas pelo jogo (EnElf_UpdateLights); nas outras (fadas dos Kokiri, de cura,
    // das fontes) o raio fica 0 e a luz não ilumina. O host entrega a posição do ator; aqui ela ganha o raio da Navi.
    if (!navi) {
        // Outro mod já acende esta fada (o WW-Style, com "Other Fairy Light Casting", dá raio ao no_glow). O raio
        // dele fica na luz e volta no payload do quadro seguinte, então isto independe da ordem dos hooks: aqui só
        // pinta, sem acender uma segunda luz.
        if (fairy->no_glow.radius > 0) {
            fairy->glow.radius = 0;
            ++state.deferred;
        } else {
            fairy->glow.radius = kFairyGlowRadius;
            ++state.lit;
        }
    }
    ++state.tinted;
    if (fairy->params == LINKSPAN_OOT_FAIRY_NAVI) {
        std::copy(color, color + 3, state.lastNavi);
        std::copy(fairy->outer_color, fairy->outer_color + 3, state.lastAura);
    }
    return SHIP_NATIVE_OK;
}

bool Equals(const char* data, uint32_t length, const char* text) {
    return data && std::strlen(text) == length && std::memcmp(data, text, length) == 0;
}

ShipNativeStatus SHIP_NATIVE_CALL Toggle(void* user, const char* data, uint32_t length, ShipNativeWriteFn write,
                                         void* writer) {
    auto& state = *static_cast<State*>(user);
    if (length == 0u || Equals(data, length, "toggle")) state.enabled = !state.enabled;
    else if (Equals(data, length, "on")) state.enabled = true;
    else if (Equals(data, length, "off")) state.enabled = false;
    else return SHIP_NATIVE_INVALID_ARGUMENT;
    const char* text = state.enabled ? "enabled=1" : "enabled=0";
    return write(writer, text, static_cast<uint32_t>(std::strlen(text)));
}

ShipNativeStatus SHIP_NATIVE_CALL Status(void* user, const char*, uint32_t, ShipNativeWriteFn write, void* writer) {
    const auto& state = *static_cast<State*>(user);
    char text[256];
    const int count = std::snprintf(text, sizeof(text),
        "version=0.2.1 enabled=%u fairies=%llu tinted=%llu lit=%llu deferred=%llu navi_aura=%.0f,%.0f,%.0f navi_light=%u,%u,%u layout=%.8s",
        state.enabled ? 1u : 0u, static_cast<unsigned long long>(state.fairies),
        static_cast<unsigned long long>(state.tinted), static_cast<unsigned long long>(state.lit), static_cast<unsigned long long>(state.deferred), state.lastAura[0], state.lastAura[1], state.lastAura[2],
        state.lastNavi[0], state.lastNavi[1], state.lastNavi[2], LINKSPAN_OOT_LAYOUT_ID);
    return count < 0 || static_cast<size_t>(count) >= sizeof(text) ? SHIP_NATIVE_FAILURE
        : write(writer, text, static_cast<uint32_t>(count));
}

ShipNativeStatus SHIP_NATIVE_CALL Init(const ShipNativeRuntime* runtime, void** instance) {
    if (!runtime || !instance || runtime->size < sizeof(ShipNativeRuntime) || runtime->abi_minor < 2 ||
        !runtime->get_service || !runtime->register_hook || !runtime->register_function) return SHIP_NATIVE_UNSUPPORTED;
    const auto* engine = static_cast<const ShipOotEngineV1*>(runtime->get_service(
        runtime->context, LINKSPAN_OOT_ENGINE_SERVICE, LINKSPAN_OOT_ENGINE_VERSION, sizeof(ShipOotEngineV1)));
    if (!engine || !engine->layout_id || std::strcmp(engine->layout_id, LINKSPAN_OOT_LAYOUT_ID))
        return SHIP_NATIVE_UNSUPPORTED;
    auto* state = new (std::nothrow) State;
    if (!state) return SHIP_NATIVE_FAILURE;
    const ShipNativeHookSpec spec{sizeof(ShipNativeHookSpec), LINKSPAN_OOT_HOOK_LIGHT_FAIRY,
                                  LINKSPAN_OOT_HOOK_LIGHT_FAIRY_VERSION, sizeof(ShipOotFairyLightHookV1),
                                  SHIP_NATIVE_HOOK_TRANSFORM, SHIP_NATIVE_HOOK_BEFORE, 0, TransformFairy, state};
    uint64_t handle = 0;
    ShipNativeStatus status = runtime->register_hook(runtime->context, &spec, &handle);
    if (status == SHIP_NATIVE_OK) status = runtime->register_function(runtime->context, "toggle", Toggle, state);
    if (status == SHIP_NATIVE_OK) status = runtime->register_function(runtime->context, "status", Status, state);
    if (status != SHIP_NATIVE_OK) {
        delete state;
        return status;
    }
    *instance = state;
    return SHIP_NATIVE_OK;
}

void SHIP_NATIVE_CALL Shutdown(void* instance) {
    delete static_cast<State*>(instance);
}

} // namespace

extern "C" SHIP_NATIVE_EXPORT const ShipNativeDescriptor* SHIP_NATIVE_CALL ShipNative_Query(void) {
    static const ShipNativeDescriptor descriptor{sizeof(ShipNativeDescriptor), SHIP_NATIVE_ABI_MAJOR, 2u, Init, Shutdown};
    return &descriptor;
}
