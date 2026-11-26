#include "fog_color.h"

#include <cstdio>
#include <limits>

namespace {
bool check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    return condition;
}
bool near(const fogcolor::Rgb& a, const fogcolor::Rgb& b, float tolerance = 0.00001f) {
    for (unsigned i = 0; i < 3; ++i)
        if (std::abs(a[i] - b[i]) > tolerance) return false;
    return true;
}
bool bounded(const fogcolor::Rgb& color) {
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(color[i]) || color[i] < 0.0f || color[i] > fogcolor::Legacy[i]) return false;
    return true;
}
} // namespace

int main() {
    // Exemplos sintéticos de canais ativos; não são capturas da iluminação em jogo.
    const fogcolor::Environment day{{80, 80, 80}, {255, 255, 255}, {100, 100, 120}};
    const fogcolor::Environment dusk{{90, 45, 25}, {180, 100, 65}, {150, 65, 35}};
    const fogcolor::Environment night{{8, 12, 24}, {20, 30, 60}, {20, 30, 60}};
    const auto d = fogcolor::Target(day), s = fogcolor::Target(dusk), n = fogcolor::Target(night);
    bool pass = true;
    pass &= check(near(d, fogcolor::Legacy, 0.021f), "dia: diferença de até 0,021 por canal");
    pass &= check(n[0] < 0.20f && n[1] < 0.22f && n[2] < 0.30f && n[2] > n[0] * 1.5f,
                  "noite: escura e fria");
    pass &= check(s[0] > s[1] && s[1] > s[2], "entardecer: tom quente");
    auto brightFogNight = night;
    brightFogNight.fog = {255, 255, 255};
    const auto bf = fogcolor::Target(brightFogNight);
    pass &= check(bf[0] < 0.25f && bf[2] < 0.30f, "névoa atmosférica clara não cria exposição noturna");
    const fogcolor::Environment indoor{{30, 8, 4}, {80, 20, 8}, {70, 15, 8}};
    const auto inside = fogcolor::Target(indoor);
    pass &= check(inside[0] < 0.4f && inside[0] > inside[2], "interior: acompanha luz fraca e quente");

    fogcolor::Color filter;
    filter.step(day, 0.05f);
    pass &= check(near(filter.value(), d), "primeira amostra diurna inicializa direto");
    auto previous = filter.value();
    bool smooth = true;
    for (int frame = 0; frame < 120; ++frame) {
        filter.step(night, 0.05f);
        for (unsigned i = 0; i < 3; ++i)
            smooth &= filter.value()[i] <= previous[i] && filter.value()[i] >= n[i] - 0.000001f &&
                      previous[i] - filter.value()[i] < 0.04f;
        previous = filter.value();
    }
    pass &= check(smooth && near(filter.value(), n, 0.0003f), "transição contínua, monótona e convergente em 6 s");
    fogcolor::Color startup;
    startup.step(night, 0.05f);
    pass &= check(near(startup.value(), n), "primeira imagem noturna não começa branca");
    fogcolor::Color full, halves;
    full.fixed(); halves.fixed();
    full.step(night, 0.1f);
    halves.step(night, 0.05f); halves.step(night, 0.05f);
    pass &= check(near(full.value(), halves.value()), "exponencial independe da divisão do intervalo");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto old = filter.value();
    for (float dt : {0.0f, -1.0f, nan, inf, -inf}) filter.step(day, dt);
    pass &= check(near(filter.value(), old), "dt inválido não move o filtro");
    filter.step(day, std::numeric_limits<float>::max());
    pass &= check(bounded(filter.value()) && filter.value()[0] - old[0] < 0.08f,
                  "dt extremo tem avanço limitado");
    bool safe = true;
    for (float v : {-65535.0f, -1.0f, 0.0f, 1.0f, 255.0f, 65535.0f, nan, inf, -inf}) {
        const fogcolor::Environment extreme{{v, nan, inf}, {-v, v, -inf}, {inf, -v, v}};
        safe &= bounded(fogcolor::Target(extreme));
        filter.step(extreme, 0.05f);
        safe &= bounded(filter.value());
    }
    pass &= check(safe && bounded(fogcolor::Target({})), "canais extremos/NaN/infinito e escuridão: saída finita");
    pass &= check(near(fogcolor::Target({{300, 300, 300}, {999, 999, 999}, {255, 255, 255}}), fogcolor::Legacy),
                  "saturação superior preserva cor fixa");

    fogcolor::FrameColor frame;
    frame.update(day, 1, 85, 100, false, true);
    frame.update(night, 1, 85, 101, false, true);
    const auto once = frame.color.value();
    frame.update(night, 1, 85, 101, false, true);
    pass &= check(near(frame.color.value(), once), "chamada duplicada não acelera o filtro");
    frame.update(night, 1, 85, 102, true, true);
    pass &= check(near(frame.color.value(), once), "pausa congela a cor");
    frame.invalidateFrame();
    frame.update(night, 2, 7, 0, false, true);
    pass &= check(frame.color.value()[0] < once[0] && once[0] - frame.color.value()[0] < 0.04f,
                  "destroy/nova cena/retrocesso mantêm continuidade");
    const auto beforeSkip = frame.color.value();
    frame.update(night, 2, 7, 100000, false, true);
    pass &= check(beforeSkip[0] - frame.color.value()[0] < 0.04f, "salto de contador não recupera tempo");
    frame.update(night, 2, 7, 100001, false, false);
    pass &= check(frame.color.value() == fogcolor::Legacy, "A/B desativado retorna exatamente à cor fixa");
    frame.update(night, 2, 7, 100002, false, true);
    pass &= check(frame.color.value()[0] < fogcolor::Legacy[0] && frame.color.value()[0] > 0.69f,
                  "reativar começa suavemente a partir da cor fixa");
    std::printf("Exemplos RGB: dia=(%.4f,%.4f,%.4f) entardecer=(%.4f,%.4f,%.4f) noite=(%.4f,%.4f,%.4f)\n",
                d[0], d[1], d[2], s[0], s[1], s[2], n[0], n[1], n[2]);
    return pass ? 0 : 1;
}
