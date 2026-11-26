#include "fog_field.h"
#include "fog_wind.h"
#ifdef FOG_WIND_HOST_PARITY
#include "wind_field.h"
#endif

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <cstring>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>

namespace fog {
struct FieldTestAccess {
    static Cell& cell(Field& f, int x, int z) { return f.cells_[Field::index(x, z)]; }
    static void project(Field& f) { f.flow_ = f.cells_; f.project(); }
    static float divergence(const Field& f, int x, int z) {
        return (f.faceX_[z * (Side + 1) + x + 1] - f.faceX_[z * (Side + 1) + x] +
                f.faceZ_[(z + 1) * Side + x] - f.faceZ_[z * Side + x]) / f.spacing_;
    }
    static float wallFlux(const Field& f) {
        float peak = 0.0f;
        for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
            if (!f.link(x, z, x + 1, z)) peak = std::max(peak, std::abs(f.faceX_[z * (Side + 1) + x + 1]));
            if (!f.link(x, z, x, z + 1)) peak = std::max(peak, std::abs(f.faceZ_[(z + 1) * Side + x]));
            if (x == 0) peak = std::max(peak, std::abs(f.faceX_[z * (Side + 1)]));
            if (z == 0) peak = std::max(peak, std::abs(f.faceZ_[x]));
        }
        return peak;
    }
};
}

namespace {
bool require(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    return condition;
}
fog::SolverOptions fluidOptions() {
    fog::SolverOptions o;
    o.transport = fog::Transport::SemiLagrangian;
    o.damping = 0.0f;
    o.recovery = 0.0f;
    o.windX = o.windZ = 0.0f;
    return o;
}
void prepare(fog::Field& f, bool vortex = true) {
    f.reset(0, 0);
    f.setSolverOptions(fluidOptions());
    for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
        f.terrain(x, z, 0.0f, true);
        auto& c = fog::FieldTestAccess::cell(f, x, z);
        const float wx = f.worldX(x), wz = f.worldZ(z);
        c.density = 0.75f * std::exp(-((wx - 96.0f) * (wx - 96.0f) + wz * wz) / (2.0f * 40.0f * 40.0f));
        const float envelope = std::exp(-(wx * wx + wz * wz) / (2.0f * 170.0f * 170.0f));
        c.vx = vortex ? -0.5f * wz * envelope : 0.0f;
        c.vz = vortex ? 0.5f * wx * envelope : 0.0f;
    }
}
struct Moments { double mass{}, x{}, z{}, peak{}; };
Moments moments(const fog::Field& f) {
    Moments m;
    for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
        const auto& c = f.cell(x, z);
        m.mass += c.density;
        m.x += c.density * f.worldX(x);
        m.z += c.density * f.worldZ(z);
        m.peak = std::max(m.peak, double(c.density));
    }
    if (m.mass) { m.x /= m.mass; m.z /= m.mass; }
    return m;
}
bool sameFloat(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
bool sameState(const fog::Field& a, const fog::Field& b) {
    for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
        const auto& ac = a.cell(x, z); const auto& bc = b.cell(x, z);
        if (ac.open != bc.open || ac.blocked != bc.blocked) return false;
        for (float fog::Cell::*p : {&fog::Cell::floor, &fog::Cell::density, &fog::Cell::vx, &fog::Cell::vz,
                                    &fog::Cell::raisedDensity, &fog::Cell::verticalSpeed})
            if (!sameFloat(ac.*p, bc.*p)) return false;
        for (float fog::Impulse::*p : {&fog::Impulse::densityScale, &fog::Impulse::vx, &fog::Impulse::vz,
                                       &fog::Impulse::minX, &fog::Impulse::maxX, &fog::Impulse::minZ, &fog::Impulse::maxZ}) {
            if (!sameFloat(a.beforeStep(x, z).*p, b.beforeStep(x, z).*p) ||
                !sameFloat(a.afterStep(x, z).*p, b.afterStep(x, z).*p)) return false;
        }
        if (!sameFloat(a.liftBeforeStep(x, z), b.liftBeforeStep(x, z))) return false;
    }
    return sameFloat(a.projectionStats().meanAfter, b.projectionStats().meanAfter);
}
bool fluidTests() {
    bool pass = true;
    auto f = std::make_unique<fog::Field>();
    prepare(*f);
    const auto initial = moments(*f);
    for (int i = 0; i < 200; ++i) f->step(fog::StepSeconds, 0.0f);
    const auto final = moments(*f);
    const double massError = std::abs(final.mass / initial.mass - 1.0);
    std::printf("METRIC mass initial=%.9f final=%.9f relative_error=%.9f N=200\n", initial.mass, final.mass, massError);
    pass &= require(massError < 0.05 && final.peak <= initial.peak,
                    "massa em dominio fechado: erro < 5% em 200 passos sem fontes e sem novo maximo");

    prepare(*f, false);
    for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
        auto& c = fog::FieldTestAccess::cell(*f, x, z);
        c.vx = 90.0f * std::sin(float(x) * 0.8f) * std::cos(float(z) * 0.7f);
        c.vz = 85.0f * std::cos(float(x) * 0.6f) * std::sin(float(z) * 0.9f);
    }
    fog::FieldTestAccess::project(*f);
    double div = 0.0;
    for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
        div += std::abs(fog::FieldTestAccess::divergence(*f, x, z));
    div /= fog::Side * fog::Side;
    const auto stats = f->projectionStats();
    std::printf("METRIC divergence before=%.9f after=%.9f max=%.9f iterations=%d\n",
                stats.meanBefore, div, stats.maxAfter, f->solverOptions().pressureIterations);
    pass &= require(div < 0.03 && div < stats.meanBefore * 0.1 && std::abs(div - stats.meanAfter) < 1.e-6,
                    "projecao: divergencia media MAC < 0.03/s e reducao > 90%");

    const double div20 = div;
    auto projectionOptions = fluidOptions();
    projectionOptions.pressureIterations = 80;
    f->setSolverOptions(projectionOptions);
    fog::FieldTestAccess::project(*f);
    pass &= require(f->projectionStats().meanAfter < div20 * 0.2f,
                    "80 iteracoes reduzem o residuo em relacao a 20");

    prepare(*f, false);
    const auto start = moments(*f);
    Moments rotated;
    for (int i = 0; i < 200; ++i) {
        if (i % fog::DefaultAttackTicks == 0) f->spin({0, 0, 0}, 300, 120);
        f->step(fog::StepSeconds, 0.0f);
        if (i == 39) rotated = moments(*f);
    }
    const auto surviving = moments(*f);
    std::printf("METRIC spin centroid40=(%.6f,%.6f) centroid200=(%.6f,%.6f) mass_ratio=%.9f peak=%.9f\n",
                rotated.x, rotated.z, surviving.x, surviving.z, surviving.mass / start.mass, surviving.peak);
    pass &= require(rotated.z > 12.0 && rotated.x < start.x - 5.0 &&
                    std::hypot(rotated.x, rotated.z) > 40.0 && std::abs(surviving.mass / start.mass - 1.0) < 0.05 && surviving.peak > 0.08,
                    "spin gira blob e preserva massa a 5% e pico > 0.08 em 200 passos");

    // A separating wall, a solid column and a different deck must never leak a scalar.
    prepare(*f, false);
    for (int z = 0; z < fog::Side; ++z) {
        f->blockEdge(11, z, 12, z, true);
        for (int x = 0; x < fog::Side; ++x) {
            auto& c = fog::FieldTestAccess::cell(*f, x, z);
            c.density = x < 12 ? 0.8f : 0.0f;
            c.vx = 180.0f; c.vz = -180.0f;
        }
    }
    f->terrain(4, 4, 0.0f, false);
    f->terrain(5, 5, 100.0f, true, true);
    const auto wallMass = moments(*f).mass;
    for (int i = 0; i < 40; ++i) f->step(fog::StepSeconds, 0.0f);
    bool noLeak = true;
    for (int z = 0; z < fog::Side; ++z) for (int x = 12; x < fog::Side; ++x)
        noLeak &= f->cell(x, z).density == 0.0f;
    pass &= require(std::abs(moments(*f).mass / wallMass - 1.0) < 0.0001 && noLeak && f->cell(4, 4).vx == 0.0f && f->cell(5, 5).density == 0.0f &&
                    fog::FieldTestAccess::wallFlux(*f) == 0.0f, "solidos/desniveis/parede/quinas: sem fuga e fluxo normal zero");

    // High CFL (4.5 cells per axis/tick) with a wall and disconnected components.
    f->reset(0, 0, 2.0f);
    f->setSolverOptions(fluidOptions());
    for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
        f->terrain(x, z, 0, true);
        auto& c = fog::FieldTestAccess::cell(*f, x, z);
        c.density = x < 12 ? 0.7f : 0.0f;
        if (x == 11) f->blockEdge(x, z, x + 1, z, true);
    }
    const double highCflMass = moments(*f).mass;
    for (int i = 0; i < 60; ++i) {
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
            auto& c = fog::FieldTestAccess::cell(*f, x, z);
            c.vx = 180; c.vz = -180;
        }
        f->step(fog::StepSeconds, 0.0f);
    }
    noLeak = true;
    for (int z = 0; z < fog::Side; ++z) for (int x = 12; x < fog::Side; ++x)
        noLeak &= f->cell(x, z).density == 0.0f;
    pass &= require(noLeak && std::abs(moments(*f).mass / highCflMass - 1.0) < 0.0001,
                    "CFL alto e parede: massa por componente e ausencia de fuga");

    prepare(*f);
    auto a = std::make_unique<fog::Field>(*f);
    auto b = std::make_unique<fog::Field>(*f);
    bool deterministic = true;
    for (int i = 0; i < 300; ++i) {
        for (auto* field : {a.get(), b.get()}) {
            field->beginTick();
            if (i % 13 == 0) field->spin({0, 0, 0}, 200, 260);
            if (i % 17 == 0) field->disturb({32, 0, -32}, {32, 0, 32}, 52, 0, 55, {120, 10, 40}, 0.4f);
            if (i % 23 == 0) field->blade({32, 20, 0}, {32, 20, 64}, {64, 25, 0}, {96, 25, 64}, 64, 1, 9);
            if (i == 70) field->shift(1, -1);
            field->step(fog::StepSeconds);
        }
        deterministic &= sameState(*a, *b);
    }
    pass &= require(deterministic, "determinismo bit a bit de celulas/impulsos/lift apos 300 passos e shift");
    prepare(*a); prepare(*b);
    for (int i = 0; i < 20; ++i) a->step(fog::StepSeconds);
    for (int i = 0; i < 40; ++i) b->step(fog::StepSeconds * 0.5f);
    pass &= require(sameState(*a, *b), "passo fixo: 20 ticks equivalem a 40 meios ticks");

    prepare(*f);
    bool stable = true;
    float peakVelocity = 0.0f;
    for (int i = 0; i < 10000; ++i) {
        // Maintain high forcing, instead of letting an initial pulse decay away.
        f->disturb({0, 0, 0}, {0, 0, 0}, 700, 0, 55, {2000, 80, -2000}, 0.0f);
        if (i % 7 == 0) f->spin({0, 0, 0}, 300, 2000);
        f->step(fog::StepSeconds);
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
            const auto& c = f->cell(x, z);
            peakVelocity = std::max({peakVelocity, std::abs(c.vx), std::abs(c.vz)});
            stable &= std::isfinite(c.density) && c.density >= 0 && c.density <= 1 &&
                      std::isfinite(c.vx) && std::isfinite(c.vz) && std::abs(c.vx) <= fog::SpeedLimit &&
                      std::abs(c.vz) <= fog::SpeedLimit && std::isfinite(c.raisedDensity) &&
                      c.raisedDensity >= 0 && c.raisedDensity <= c.density * 0.70f && std::isfinite(c.verticalSpeed);
        }
    }
    std::printf("METRIC stability steps=10000 peak_cell_velocity=%.6f\n", peakVelocity);
    pass &= require(stable && peakVelocity > 60.0f, "10000 passos com forca alta continua: finito e limitado");

    // First-order option and default physical parameters get their own boundedness smoke.
    prepare(*f);
    auto options = fog::SolverOptions{};
    options.transport = fog::Transport::SemiLagrangian;
    options.correctDensity = false;
    f->setSolverOptions(options);
    for (int i = 0; i < 200; ++i) { if (i % 9 == 0) f->spin({0, 0, 0}, 200, 260); f->step(fog::StepSeconds); }
    pass &= require(std::isfinite(moments(*f).mass) && fog::FieldTestAccess::wallFlux(*f) == 0,
                    "semi-lagrangiano simples com vento/amortecimento/reposicao padrao");
    return pass;
}
void benchmark() {
    // Caminhos: 0 transporte antigo, 1 semi-lagrangiano de prova, 2 camada rasa (o do jogo com o campo da CPU).
    constexpr int repetitions = 5, ticks = 1000, paths = 3;
    double times[paths][repetitions]{};
    double checksum = 0;
    for (int repeat = 0; repeat < repetitions; ++repeat) for (int run = 0; run < paths; ++run) {
        const int path = (run + repeat) % paths;
        auto f = std::make_unique<fog::Field>();
        prepare(*f);
        fog::SolverOptions options = path == 2 ? fog::shallowOptions() : fog::SolverOptions{};
        if (path == 1) options.transport = fog::Transport::SemiLagrangian;
        f->setSolverOptions(options);
        const auto tick = [&](int i) {
            f->beginTick();
            if (i % 9 == 0) f->spin({0, 0, 0}, 200, 260);
            f->step(fog::StepSeconds);
        };
        for (int i = 0; i < 100; ++i) tick(i);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ticks; ++i) tick(i);
        const auto end = std::chrono::steady_clock::now();
        times[path][repeat] = std::chrono::duration<double, std::micro>(end - start).count() / ticks;
        if (path < 2) checksum += moments(*f).mass; // mantém a soma de controle das duas colunas antigas
    }
    for (auto& path : times) std::sort(std::begin(path), std::end(path));
    std::printf("BENCH CPU 25x25, 5x1000 passos, aquecimento100, spin/9: legacy_us=%.6f fluid_us=%.6f ratio=%.6f checksum=%.9f\n",
                times[0][2], times[1][2], times[1][2] / times[0][2], checksum);
    std::printf("BENCH ranges_us legacy=[%.6f,%.6f] fluid=[%.6f,%.6f]\n", times[0][0], times[0][4], times[1][0], times[1][4]);
    std::printf("BENCH shallow_us=%.6f range=[%.6f,%.6f] ratio_legacy=%.6f\n", times[2][2], times[2][0], times[2][4],
                times[2][2] / times[0][2]);
}
// Centro x do déficit de névoa (RestDensity - density) nas células abertas.
double deficitX(const fog::Field& f) {
    double mass = 0.0, x = 0.0;
    for (int z = 0; z < fog::Side; ++z) for (int i = 0; i < fog::Side; ++i) {
        const double d = std::max(0.0, double(fog::RestDensity - f.cell(i, z).density));
        mass += d;
        x += d * f.worldX(i);
    }
    return mass > 0.0 ? x / mass : 0.0;
}
// Centro (x, z) do déficit, para conferir o transporte nos quatro sentidos.
std::array<double, 2> deficitCenter(const fog::Field& f) {
    double mass = 0.0, x = 0.0, z = 0.0;
    for (int k = 0; k < fog::Side; ++k) for (int i = 0; i < fog::Side; ++i) {
        const double d = std::max(0.0, double(fog::RestDensity - f.cell(i, k).density));
        mass += d;
        x += d * f.worldX(i);
        z += d * f.worldZ(k);
    }
    return mass > 0.0 ? std::array<double, 2>{x / mass, z / mass} : std::array<double, 2>{};
}
void hole(fog::Field& f) {
    f.reset(0, 0);
    for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) f.terrain(x, z, 0.0f, true);
    for (int z = 10; z <= 14; ++z) for (int x = 10; x <= 14; ++x) fog::FieldTestAccess::cell(f, x, z).density = 0.05f;
}
bool windTests() {
    bool pass = true;
    // Vento fixo explícito = padrão antigo, bit a bit, com golpe e recuperação.
    auto legacy = std::make_unique<fog::Field>();
    auto explicitWind = std::make_unique<fog::Field>();
    for (auto* f : {legacy.get(), explicitWind.get()}) {
        hole(*f);
        f->blade({32, 20, 0}, {32, 20, 64}, {64, 25, 0}, {96, 25, 64}, 64.0f, 1.0f, fog::DefaultAttackTicks);
    }
    for (int i = 0; i < 60; ++i) {
        explicitWind->setWind(fogwind::LegacyX, fogwind::LegacyZ);
        legacy->step(fog::StepSeconds);
        explicitWind->step(fog::StepSeconds);
    }
    // Prova só que o setter com o valor padrão não muda o resultado atual; o sentido vem do teste de eixos abaixo.
    pass &= require(sameState(*legacy, *explicitWind), "wind: setWind(16,5) idêntico ao vento padrão do campo");

    // Quatro sentidos: o buraco anda a favor do vento em ±x e ±z (pega troca de sinal num eixo só).
    for (const auto wind : {std::array<float, 2>{60, 0}, {-60, 0}, {0, 60}, {0, -60}}) {
        auto f = std::make_unique<fog::Field>();
        hole(*f);
        const auto before = deficitCenter(*f);
        f->setWind(wind[0], wind[1]);
        for (int i = 0; i < 20; ++i) f->step(fog::StepSeconds);
        const auto after = deficitCenter(*f);
        const double along = ((after[0] - before[0]) * wind[0] + (after[1] - before[1]) * wind[1]) / 60.0;
        const double across = ((after[0] - before[0]) * wind[1] - (after[1] - before[1]) * wind[0]) / 60.0;
        pass &= require(along > 10.0 && std::abs(across) < 1.0, "wind: buraco anda a favor do vento em ±x e ±z");
    }

    // Relógio da rajada: pausa, render repetido, salto, campo desligado, cena e sessão.
    {
        fogwind::Clock clock;
        std::uint32_t tick = 0;
        float s = 0.0f;
        for (int i = 0; i < 200; ++i) s = clock.Step(1, 85, ++tick, false, true);
        pass &= require(std::abs(s - 9.95f) < 1e-4f, "wind: relógio avança um passo por tick");
        for (int i = 0; i < 200; ++i) s = clock.Step(1, 85, ++tick, true, true);
        pass &= require(std::abs(s - 9.95f) < 1e-4f, "wind: relógio parado na pausa");
        s = clock.Step(1, 85, tick += 9, false, true);
        pass &= require(std::abs(s - 10.0f) < 1e-4f && clock.Step(1, 85, tick, false, true) == s,
                        "wind: salto vale um passo e render repetido não avança");
        pass &= require(clock.Step(1, 85, ++tick, false, false) == 0.0f && clock.Step(1, 85, ++tick, false, true) == 0.0f,
                        "wind: campo desligado zera e religa em zero");
        clock.Step(1, 85, ++tick, false, true);
        pass &= require(clock.Step(1, 85, ++tick, false, true) > 0.0f && clock.Step(1, 86, ++tick, false, true) == 0.0f,
                        "wind: troca de cena reinicia (sala não entra na chave)");
    }

    // Sem vento o buraco fica no lugar; com vento de +x ele desce a favor do vento.
    auto calm = std::make_unique<fog::Field>();
    auto windy = std::make_unique<fog::Field>();
    hole(*calm);
    hole(*windy);
    const double start = deficitX(*calm);
    for (int i = 0; i < 20; ++i) {
        calm->setWind(0.0f, 0.0f);
        windy->setWind(60.0f, 0.0f);
        calm->step(fog::StepSeconds);
        windy->step(fog::StepSeconds);
    }
    const double calmShift = deficitX(*calm) - start, windyShift = deficitX(*windy) - start;
    std::printf("wind: deslocamento do buraco em 1 s: calmo %.2f, 60 u/s %.2f\n", calmShift, windyShift);
    pass &= require(std::abs(calmShift) < 1.0, "wind: sem vento o buraco não anda");
    pass &= require(windyShift > 10.0, "wind: vento de +x leva o buraco para +x");
    calm->setWind(std::numeric_limits<float>::quiet_NaN(), 1.0e9f);
    pass &= require(calm->solverOptions().windX == 0.0f && calm->solverOptions().windZ == fog::SpeedLimit,
                    "wind: setWind limita e zera não finito");

    // Fórmulas do vento da cena.
    fogwind::Scene scene;
    scene.direction = {80.0f, 80.0f, 80.0f};
    scene.speed = 255.0f;
    auto base = fogwind::Base(scene);
    pass &= require(std::abs(base[0] - 42.426f) < 0.01f && base[1] == 0.0f && std::abs(base[2] - 42.426f) < 0.01f,
                    "wind: base (80,80,80) a 255 = 60 u/s no plano");
    scene.speed = 0.0f;
    scene.skybox = 0x1D;
    base = fogwind::Base(scene);
    pass &= require(std::abs(std::hypot(base[0], base[2]) - 40.0f / 255.0f * 60.0f) < 0.01f,
                    "wind: céu aberto sem vento declarado = brisa ambiente");
    scene.skybox = 0x00;
    const auto indoor = fogwind::Carrier(scene, 0.0f, 0.0f, 3.0f);
    pass &= require(indoor[0] == 0.0f && indoor[1] == 0.0f, "wind: interior parado");
    scene.override = 255;
    pass &= require(std::abs(std::hypot(fogwind::Base(scene)[0], fogwind::Base(scene)[2]) - 60.0f) < 0.01f,
                    "wind: override força a velocidade");
    scene.hostWind = false;
    const auto off = fogwind::Carrier(scene, 100.0f, 50.0f, 7.0f);
    pass &= require(off[0] == fogwind::LegacyX && off[1] == fogwind::LegacyZ, "wind: campo desligado = vento fixo antigo");
    float low = 10.0f, high = 0.0f;
    for (int t = 0; t < 2000; ++t) {
        const float g = fogwind::Gust(float(t * 37 % 900), float(t * 53 % 700), t * 0.05f, {30.0f, 0.0f, -20.0f});
        low = std::min(low, g);
        high = std::max(high, g);
    }
    pass &= require(low >= 1.0f - 0.5f * fogwind::GustAmplitude - 1e-4f &&
                    high <= 1.0f + 1.5f * fogwind::GustAmplitude + 1e-4f, "wind: rajada dentro da faixa");
#ifdef FOG_WIND_HOST_PARITY
    // Paridade com o header do host, número a número.
    float worst = 0.0f;
    for (int t = 0; t < 4000; ++t) {
        const std::array<float, 3> dir = {float(t % 19) - 9.0f, 80.0f, float(t % 23) - 11.0f};
        const float speed = float(t % 300) - 20.0f;
        const auto a = fogwind::BaseVelocity(dir, speed);
        const auto b = WindField::BaseVelocity(dir, speed);
        const float x = float(t * 37 % 1900) - 950.0f, z = float(t * 53 % 1700) - 850.0f, s = t * 0.05f;
        worst = std::max({worst, std::abs(a[0] - b[0]), std::abs(a[2] - b[2]),
                          std::abs(fogwind::Gust(x, z, s, a) - WindField::Gust(x, z, s, b))});
        const int sky = t % 40;
        pass &= fogwind::AmbientSky(sky) == WindField::AmbientSky(sky);
        pass &= fogwind::EffectiveSpeed(speed, sky & 1, 40.0f) == WindField::EffectiveSpeed(speed, sky & 1, 40.0f);
    }
    pass &= require(worst == 0.0f, "wind: fórmulas idênticas às do WindField do host");
    {
        // Mesmo relógio que o WindField::State real, bit a bit.
        fogwind::Clock clock;
        WindField::State host;
        bool same = fogwind::TickSeconds == WindField::TickSeconds;
        std::uint32_t tick = 50;
        for (int i = 0; i < 900; ++i) {
            const int phase = i / 100;
            const std::uintptr_t session = phase == 8 ? 2 : (i == 899 ? 0 : 1);
            const int sceneId = phase >= 6 ? 86 : 85;
            if (phase != 1 || i % 3 == 0) tick += (phase == 5 && i % 10 == 0) ? 7u : 1u;
            if (phase == 7 && i == 750) tick -= 20;
            const bool paused = phase == 2;
            const bool enabled = phase != 4;
            WindField::Input in;
            in.session = session;
            in.scene = sceneId;
            in.tick = tick;
            in.paused = paused;
            in.enabled = enabled;
            in.direction = {1.0f, 0.0f, 0.0f};
            in.speed = 40.0f;
            const float hostSeconds = host.Step(in).velocityClock[3];
            const float mine = clock.Step(session, sceneId, tick, paused, enabled);
            same = same && std::memcmp(&hostSeconds, &mine, sizeof(float)) == 0;
        }
        pass &= require(same, "wind: relógio da rajada idêntico ao WindField::State do host");
    }
#endif
    return pass;
}

// 0.28.0: camada rasa. O golpe só empurra o ar; a abertura vem da divergência do empurrão e o vórtice, da
// parte giratória que a pressão da camada não desfaz. Nada aqui escreve um giro pronto no campo.
fog::SolverOptions calmShallow() {
    auto options = fog::shallowOptions();
    options.windX = options.windZ = 0.0f;
    return options;
}
void flatField(fog::Field& f, float spacing, const fog::SolverOptions& options) {
    f.reset(0, 0, spacing);
    f.setSolverOptions(options);
    for (int z = 0; z < fog::Side; ++z)
        for (int x = 0; x < fog::Side; ++x) f.terrain(x, z, 0, true);
}
double fieldMass(const fog::Field& f) {
    double mass = 0.0;
    for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) mass += f.cell(x, z).density;
    return mass;
}
// Arco de 90° em quatro ticks em volta do punho, como o golpe lateral do jogo (sense +1: de +x para +z).
bool swing(fog::Field& f, float sense, float reach) {
    const fog::Point base{0, 20, 0};
    fog::Point last{reach, 20, 0};
    bool hit = false;
    for (int k = 1; k <= 4; ++k) {
        const float a = sense * 0.5f * 3.14159265f * float(k) / 4.0f;
        const fog::Point tip{reach * std::cos(a), 20, reach * std::sin(a)};
        hit |= f.blade(base, last, base, tip, 64.0f, 1.0f, fog::DefaultAttackTicks);
        f.step(fog::StepSeconds);
        last = tip;
    }
    return hit;
}
// Circulação média em volta do punho, numa coroa de 1 a 4 células (a mesma medida que o mod registra em jogo).
double circulation(const fog::Field& f) { return f.circulation(0.0f, 0.0f); }
// Norma da divergência e do rotacional por diferença central (o mesmo estêncil do confinamento).
std::array<double, 2> divergenceAndCurl(const fog::Field& f) {
    double div = 0.0, rot = 0.0;
    for (int z = 1; z + 1 < fog::Side; ++z) for (int x = 1; x + 1 < fog::Side; ++x) {
        const auto& l = f.cell(x - 1, z); const auto& r = f.cell(x + 1, z);
        const auto& b = f.cell(x, z - 1); const auto& t = f.cell(x, z + 1);
        div += std::abs(0.5 * (r.vx - l.vx + t.vz - b.vz));
        rot += std::abs(0.5 * (r.vz - l.vz - (t.vx - b.vx)));
    }
    return {div, rot};
}
bool shallowTests() {
    bool pass = true;
    // 1) Repouso com vento fica em repouso (o vento só leva o que se afasta do repouso).
    {
        auto f = std::make_unique<fog::Field>();
        flatField(*f, fog::Spacing, fog::shallowOptions());
        for (int i = 0; i < 200; ++i) f->step(fog::StepSeconds);
        float worst = 0.0f;
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
            worst = std::max({worst, std::abs(f->cell(x, z).density - fog::RestDensity), std::abs(f->cell(x, z).vx),
                              std::abs(f->cell(x, z).vz)});
        pass &= require(worst < 1e-5f, "shallow: névoa em repouso com vento continua em repouso");
    }
    // 2) O buraco se fecha escorrendo dos lados, sem recuperação artificial, e a massa fica.
    {
        auto withPressure = std::make_unique<fog::Field>();
        auto without = std::make_unique<fog::Field>();
        auto options = calmShallow();
        options.recovery = 0.0f;
        hole(*withPressure);
        hole(*without);
        withPressure->setSolverOptions(options);
        options.gravity = 0.0f;
        without->setSolverOptions(options);
        const double mass = fieldMass(*withPressure);
        for (int i = 0; i < 120; ++i) {
            withPressure->step(fog::StepSeconds);
            without->step(fog::StepSeconds);
        }
        const float center = withPressure->cell(12, 12).density, still = without->cell(12, 12).density;
        const double error = std::abs(fieldMass(*withPressure) / mass - 1.0);
        std::printf("shallow: centro do buraco em 6 s: com pressão %.3f, sem %.3f, erro de massa %.6f\n", center,
                    still, error);
        pass &= require(center > 0.30f && still < 0.06f, "shallow: o vão se fecha pela pressão da camada");
        pass &= require(error < 0.01, "shallow: fechar o vão conserva a massa");
    }
    // 3) Golpe lateral: abre a névoa, a pressão desfaz o empurrão divergente e sobra o giro no sentido do arco.
    for (const float spacing : {fog::Spacing, 40.0f, 80.0f}) {
        auto ccw = std::make_unique<fog::Field>();
        auto cw = std::make_unique<fog::Field>();
        auto vertical = std::make_unique<fog::Field>();
        for (auto* f : {ccw.get(), cw.get(), vertical.get()}) flatField(*f, spacing, calmShallow());
        const float reach = std::max(64.0f, 0.9f * spacing);
        pass &= require(swing(*ccw, 1.0f, reach) && swing(*cw, -1.0f, reach), "shallow: golpe no ar afeta a névoa");
        float lowest = 1.0f;
        std::array<double, 2> early{}, late{};
        double circulationEarly = 0.0;
        float rimPeak = 0.0f, rimAt[4]{};
        for (int i = 0; i < 40; ++i) {
            ccw->step(fog::StepSeconds);
            cw->step(fog::StepSeconds);
            float rimNow = 0.0f;
            for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
                rimNow = std::max(rimNow, ccw->cell(x, z).density);
            rimPeak = std::max(rimPeak, rimNow);
            if (i == 4 || i == 9 || i == 19 || i == 39) rimAt[i == 4 ? 0 : i == 9 ? 1 : i == 19 ? 2 : 3] = rimNow;
            for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
                if (std::hypot(ccw->worldX(x), ccw->worldZ(z)) < 1.6 * spacing)
                    lowest = std::min(lowest, ccw->cell(x, z).density);
            if (i == 3) { early = divergenceAndCurl(*ccw); circulationEarly = circulation(*ccw); }
        }
        late = divergenceAndCurl(*ccw);
        const double circulationLate = circulation(*ccw);
        const double ratioEarly = early[0] / std::max(early[1], 1e-9), ratioLate = late[0] / std::max(late[1], 1e-9);
        std::printf("shallow %.0f: vão %.3f, circulação 0,4 s %.2f / 2,2 s %.2f (horário %.2f), div/rot %.2f -> %.2f\n",
                    spacing, lowest, circulationEarly, circulationLate, circulation(*cw), ratioEarly, ratioLate);
        std::printf("shallow %.0f: borda mais densa %.3f (repouso %.2f); em 0,25/0,5/1/2 s: %.3f %.3f %.3f %.3f\n",
                    spacing, rimPeak, fog::RestDensity, rimAt[0], rimAt[1], rimAt[2], rimAt[3]);
        pass &= require(lowest < 0.85f * fog::RestDensity, "shallow: o golpe abre a névoa");
        pass &= require(circulationLate > 1.0 && circulation(*cw) < -1.0, "shallow: o giro segue o sentido do arco");
        pass &= require(ratioLate < 0.8 * ratioEarly, "shallow: a pressão desfaz o empurrão e deixa o vórtice");
        // Golpe vertical, sem arco no plano: empurra, mas não gira.
        const fog::Point base{0, 10, 0};
        fog::Point last{48, 10, 0};
        for (int k = 1; k <= 4; ++k) {
            const fog::Point tip{48, 10.0f + 6.0f * k, 0};
            vertical->blade(base, last, base, tip, 64.0f, 1.0f, fog::DefaultAttackTicks);
            vertical->step(fog::StepSeconds);
            last = tip;
        }
        for (int i = 0; i < 40; ++i) vertical->step(fog::StepSeconds);
        pass &= require(std::abs(circulation(*vertical)) < 0.1 * circulationLate, "shallow: golpe vertical sem giro");
        // Depois de 12 s o vão fechou.
        for (int i = 0; i < 200; ++i) ccw->step(fog::StepSeconds);
        float gap = 1.0f;
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
            if (std::hypot(ccw->worldX(x), ccw->worldZ(z)) < 1.6 * spacing) gap = std::min(gap, ccw->cell(x, z).density);
        pass &= require(gap > 0.9f * fog::RestDensity, "shallow: a névoa volta a fechar o vão");
    }
    // 4) Estável sob golpes e giros seguidos, com vento.
    {
        auto f = std::make_unique<fog::Field>();
        flatField(*f, fog::Spacing, fog::shallowOptions());
        bool finite = true;
        for (int i = 0; i < 600; ++i) {
            const float a = 0.37f * i;
            if (i % 7 == 0) {
                f->blade({0, 20, 0}, {60 * std::cos(a), 20, 60 * std::sin(a)}, {0, 20, 0},
                         {60 * std::cos(a + 0.8f), 20, 60 * std::sin(a + 0.8f)}, 64.0f, 2.0f, fog::DefaultAttackTicks);
            }
            if (i % 53 == 0) f->spin({0, 10, 0}, 200.0f, 260.0f, 2.0f);
            f->step(fog::StepSeconds);
            for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
                const auto& c = f->cell(x, z);
                finite &= std::isfinite(c.density) && std::isfinite(c.vx) && std::isfinite(c.vz) && c.density >= 0.0f &&
                          c.density <= fog::MaxDensity && std::abs(c.vx) <= fog::SpeedLimit && std::abs(c.vz) <= fog::SpeedLimit;
            }
        }
        pass &= require(finite, "shallow: 600 passos com golpes seguidos ficam finitos e limitados");
    }
    // 5) Bancos (0.29.0): o repouso varia no chão, fica em equilíbrio com vento, anda com o deslocamento pedido e a
    //    névoa volta para ele depois de um golpe.
    {
        auto f = std::make_unique<fog::Field>();
        f->banks(fog::BankLow, fog::BankHigh, fog::BankWavelength, 0.0f, 0.0f);
        flatField(*f, 40.0f, fog::shallowOptions());
        float low = 1.0f, high = 0.0f;
        int thin = 0, thick = 0;
        std::array<float, fog::Side * fog::Side> rest{};
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
            const float r = rest[z * fog::Side + x] = f->rest(x, z);
            low = std::min(low, r);
            high = std::max(high, r);
            thin += r < 0.3f * fog::RestDensity;
            thick += r > 1.2f * fog::RestDensity;
        }
        std::printf("bancos 40: repouso de %.3f a %.3f, %d células ralas e %d cheias de %d\n", low, high, thin, thick,
                    fog::Side * fog::Side);
        pass &= require(low >= fog::BankLow * fog::RestDensity - 1e-6f && high <= fog::BankHigh * fog::RestDensity + 1e-6f,
                        "bancos: o repouso fica entre os limites");
        pass &= require(thin > 40 && thick > 80, "bancos: há vãos e bancos cheios na mesma grade");
        for (int i = 0; i < 200; ++i) f->step(fog::StepSeconds);
        float worst = 0.0f;
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
            worst = std::max({worst, std::abs(f->cell(x, z).density - f->rest(x, z)), std::abs(f->cell(x, z).vx),
                              std::abs(f->cell(x, z).vz)});
        pass &= require(worst < 1e-5f, "bancos: em repouso com vento os bancos não escorrem");
        // Golpe no meio da grade: abre e, 8 s depois, a névoa voltou para o padrão dos bancos.
        f->setSolverOptions(calmShallow());
        swing(*f, 1.0f, 64.0f);
        float opened = 0.0f;
        for (int i = 0; i < 320; ++i) {
            f->step(fog::StepSeconds);
            if (i == 10) {
                for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
                    opened = std::max(opened, std::abs(f->cell(x, z).density - f->rest(x, z)));
            }
        }
        float back = 0.0f;
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
            back = std::max(back, std::abs(f->cell(x, z).density - f->rest(x, z)));
        std::printf("bancos 40: desvio máximo do repouso 0,5 s depois do golpe %.3f, 16 s depois %.3f\n", opened, back);
        pass &= require(opened > 0.10f && back < 0.25f * opened, "bancos: o golpe desloca a névoa e ela volta ao padrão");
        // Deslocar o padrão uma célula para +x equivale a ler a célula vizinha.
        f->banks(fog::BankLow, fog::BankHigh, fog::BankWavelength, 40.0f, 0.0f);
        float drift = 0.0f;
        for (int z = 0; z < fog::Side; ++z) for (int x = 1; x < fog::Side; ++x)
            drift = std::max(drift, std::abs(f->rest(x, z) - rest[z * fog::Side + x - 1]));
        pass &= require(drift < 1e-4f, "bancos: o padrão anda com o deslocamento");
        // Sem comprimento de onda o repouso é o uniforme de antes.
        f->banks(fog::BankLow, fog::BankHigh, 0.0f, 0.0f, 0.0f);
        pass &= require(f->rest(3, 7) == fog::RestDensity && f->rest(20, 11) == fog::RestDensity,
                        "bancos: comprimento zero devolve o repouso uniforme");
    }
    // 6) Pouso (0.31.0): a batida abre a névoa no lugar, empurra um anel mais denso para fora, não gira e fecha.
    {
        auto f = std::make_unique<fog::Field>();
        flatField(*f, 40.0f, calmShallow());
        pass &= require(!f->land({0, 0, 0}, 0.8f * fog::LandMinSpeed), "pouso: passo leve não mexe na névoa");
        pass &= require(f->land({0, 0, 0}, 220.0f), "pouso: queda forte afeta a névoa");
        pass &= require(!f->land({0, 400, 0}, 220.0f), "pouso: batida acima da camada não afeta");
        const double mass = fieldMass(*f);
        float center = 1.0f, ring = 0.0f, ringRadius = 0.0f, ringLater = 0.0f, ringLaterRadius = 0.0f;
        double swirl = 0.0;
        for (int i = 0; i < 30; ++i) {
            f->step(fog::StepSeconds);
            if (i == 9 || i == 19) {
                float peak = 0.0f, where = 0.0f;
                for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
                    const float d = std::hypot(f->worldX(x), f->worldZ(z));
                    if (d < 40.0f) center = std::min(center, f->cell(x, z).density);
                    if (f->cell(x, z).density > peak) { peak = f->cell(x, z).density; where = d; }
                }
                (i == 9 ? ring : ringLater) = peak;
                (i == 9 ? ringRadius : ringLaterRadius) = where;
            }
            swirl = std::max(swirl, std::abs(circulation(*f)));
        }
        for (int i = 0; i < 240; ++i) f->step(fog::StepSeconds);
        float gap = 1.0f;
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
            if (std::hypot(f->worldX(x), f->worldZ(z)) < 80.0f) gap = std::min(gap, f->cell(x, z).density);
        const double error = std::abs(fieldMass(*f) / mass - 1.0);
        std::printf("pouso 40: centro %.3f, anel %.3f a %.0f u (0,5 s) e %.3f a %.0f u (1 s), giro %.2f, "
                    "vão 13,5 s %.3f, erro de massa %.4f\n",
                    center, ring, ringRadius, ringLater, ringLaterRadius, swirl, gap, error);
        pass &= require(center < 0.8f * fog::RestDensity, "pouso: a batida abre a névoa no lugar");
        pass &= require(ring > 1.05f * fog::RestDensity && ringRadius >= 40.0f, "pouso: sobra um anel mais denso");
        pass &= require(ringLaterRadius >= ringRadius, "pouso: o anel se espalha para fora");
        pass &= require(swirl < 1.0, "pouso: a batida não gira a névoa");
        pass &= require(gap > 0.9f * fog::RestDensity, "pouso: a névoa volta a fechar");
    }
    // 7) Névoa que sobe (0.32.0): golpe e pouso levantam parte da coluna, e ela torna a baixar sozinha.
    for (const bool landing : {false, true}) {
        auto f = std::make_unique<fog::Field>();
        flatField(*f, 40.0f, calmShallow());
        if (landing) f->land({0, 0, 0}, 300.0f);
        else swing(*f, 1.0f, 64.0f);
        const auto peakFraction = [&] {
            float peak = 0.0f;
            for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
                const auto& c = f->cell(x, z);
                // Só conta onde há névoa de verdade: no centro esvaziado pelo pouso a fração alta não aparece.
                if (c.density > 0.5f * fog::RestDensity) peak = std::max(peak, c.raisedDensity / c.density);
            }
            return peak;
        };
        float up = 0.0f;
        for (int i = 0; i < 20; ++i) {
            f->step(fog::StepSeconds);
            up = std::max(up, peakFraction());
        }
        for (int i = 0; i < 160; ++i) f->step(fog::StepSeconds);
        const float down = peakFraction();
        std::printf("sobe (%s): fração levantada máxima no 1º segundo %.3f, 9 s depois %.3f\n",
                    landing ? "pouso" : "golpe", up, down);
        pass &= require(up > 0.15f, landing ? "sobe: o pouso levanta a névoa" : "sobe: o golpe levanta a névoa");
        pass &= require(down < 0.25f * up, landing ? "sobe: depois do pouso ela baixa" : "sobe: depois do golpe ela baixa");
    }
    return pass;
}
// Mapa ASCII do rotacional (sinal e força) e da densidade em volta do punho (diagnóstico).
void dumpMaps(const fog::Field& f, const char* label) {
    std::printf("-- %s (rotacional | densidade)\n", label);
    for (int z = 18; z >= 6; --z) {
        for (int x = 6; x <= 18; ++x) {
            const auto& l = f.cell(x - 1, z); const auto& r = f.cell(x + 1, z);
            const auto& b = f.cell(x, z - 1); const auto& t = f.cell(x, z + 1);
            const float rot = 0.5f * (r.vz - l.vz - (t.vx - b.vx));
            std::printf("%c", rot > 6 ? 'O' : rot > 2 ? 'o' : rot < -6 ? 'X' : rot < -2 ? 'x' : '.');
        }
        std::printf("   ");
        for (int x = 6; x <= 18; ++x) {
            const float d = f.cell(x, z).density;
            std::printf("%c", d < 0.2f ? ' ' : d < 0.45f ? '-' : d < 0.55f ? '=' : d < 0.68f ? '#' : '@');
        }
        std::printf("\n");
    }
}
void shallowMaps(float spacing, float confinement, float damping, float spread, float cells, float viscosity) {
    auto options = calmShallow();
    options.confinement = confinement;
    options.damping = damping;
    options.divergenceDamping = spread;
    options.bladeCells = cells;
    options.viscosity = viscosity;
    auto f = std::make_unique<fog::Field>();
    flatField(*f, spacing, options);
    swing(*f, 1.0f, std::max(64.0f, 0.9f * spacing));
    std::printf("== sp=%.0f conf=%.0f damp=%.2f div=%.2f cells=%.1f visc=%.2f\n", spacing, confinement, damping, spread,
                cells, viscosity);
    for (int i = 1; i <= 50; ++i) {
        f->step(fog::StepSeconds);
        if (i == 2 || i == 10 || i == 24 || i == 50) {
            char label[32];
            std::snprintf(label, sizeof(label), "t=%.2f s", 0.2 + 0.05 * i);
            dumpMaps(*f, label);
        }
    }
}
// Varredura de parâmetros da camada rasa (diagnóstico: --shallow-sweep).
void shallowSweep(bool maps) {
    if (maps) {
        for (const float spread : {0.0f, 0.12f, 0.2f, 0.3f}) {
            for (const float spacing : {fog::Spacing, 40.0f, 80.0f}) {
                auto f = std::make_unique<fog::Field>();
                auto options = calmShallow();
                options.recovery = 0.0f;
                options.divergenceDamping = spread;
                hole(*f);
                f->reset(0, 0, spacing);
                for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) f->terrain(x, z, 0.0f, true);
                for (int z = 10; z <= 14; ++z) for (int x = 10; x <= 14; ++x)
                    fog::FieldTestAccess::cell(*f, x, z).density = 0.05f;
                f->setSolverOptions(options);
                float at[3]{};
                for (int i = 1; i <= 120; ++i) {
                    f->step(fog::StepSeconds);
                    if (i == 30) at[0] = f->cell(12, 12).density;
                    if (i == 60) at[1] = f->cell(12, 12).density;
                    if (i == 120) at[2] = f->cell(12, 12).density;
                }
                std::printf("buraco div=%.2f sp=%.0f: centro 1,5 s %.3f | 3 s %.3f | 6 s %.3f\n", spread, spacing, at[0],
                            at[1], at[2]);
            }
        }
        shallowMaps(80.0f, fog::ShallowConfinement, fog::ShallowDamping, fog::ShallowDivergenceDamping,
                    fog::ShallowBladeCells, fog::ShallowViscosity);
        shallowMaps(32.0f, fog::ShallowConfinement, fog::ShallowDamping, fog::ShallowDivergenceDamping,
                    fog::ShallowBladeCells, fog::ShallowViscosity);
        return;
    }
    for (const float spacing : {fog::Spacing, 40.0f, 80.0f})
    for (const float confinement : {2.0f, 4.0f})
    for (const float radial : {0.0f, 0.6f, 1.2f})
    for (const float viscosity : {0.03f, 0.06f}) {
        const float damping = 0.45f, spread = 0.3f, cells = 2.2f, gain = 1.8f, cap = 60.0f, wave = 70.0f;
        auto options = calmShallow();
        options.confinement = confinement;
        options.damping = damping;
        options.divergenceDamping = spread;
        options.bladeCells = cells;
        options.viscosity = viscosity;
        options.bladeGain = gain;
        options.attackCap = cap;
        options.gravity = wave * wave / fog::RestDensity;
        options.bladeRadial = radial;
        std::printf("radial=%.1f visc=%.2f ", radial, viscosity);
        auto f = std::make_unique<fog::Field>();
        flatField(*f, spacing, options);
        swing(*f, 1.0f, std::max(64.0f, 0.9f * spacing));
        float lowest = 1.0f, highest = 0.0f;
        std::array<double, 2> early{};
        double c04 = 0.0, c12 = 0.0;
        for (int i = 0; i < 40; ++i) {
            f->step(fog::StepSeconds);
            for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x) {
                const double r = std::hypot(f->worldX(x), f->worldZ(z));
                if (r < 1.6 * spacing) lowest = std::min(lowest, f->cell(x, z).density);
                if (r < 4.0 * spacing) highest = std::max(highest, f->cell(x, z).density);
            }
            if (i == 3) { early = divergenceAndCurl(*f); c04 = circulation(*f); }
            if (i == 19) c12 = circulation(*f);
        }
        const auto late = divergenceAndCurl(*f);
        const double c22 = circulation(*f);
        for (int i = 0; i < 80; ++i) f->step(fog::StepSeconds);
        float gap = 1.0f;
        for (int z = 0; z < fog::Side; ++z) for (int x = 0; x < fog::Side; ++x)
            if (std::hypot(f->worldX(x), f->worldZ(z)) < 1.6 * spacing) gap = std::min(gap, f->cell(x, z).density);
        std::printf("sp=%2.0f conf=%.0f | circ %.2f %.2f %.2f (%.0f%%) | div/rot %.2f -> %.2f"
                    " | vão %.3f pilha %.3f | 6 s %.3f\n", spacing, confinement, c04, c12, c22,
                    c04 > 0 ? 100.0 * c22 / c04 : 0.0, early[0] / std::max(early[1], 1e-9),
                    late[0] / std::max(late[1], 1e-9), lowest, highest, gap);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--shallow-sweep") == 0 || std::strcmp(argv[1], "--shallow-maps") == 0)) {
        shallowSweep(argv[1][10] == 'm');
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--benchmark-only") == 0) {
        benchmark();
        return 0;
    }
    fog::Field field;
    field.reset(0, 0);
    for (int z = 0; z < fog::Side; ++z)
        for (int x = 0; x < fog::Side; ++x)
            field.terrain(x, z, 0, x != 11);
    const auto check = [](bool pass, const char* message) {
        if (!pass) std::fprintf(stderr, "%s\n", message);
        return pass;
    };
    bool pass = true;
    const fog::Point center{0, 0, 0};
    pass &= check(field.disturb(center, {0, 0, 48}, 52, 0, 55, {0, 0, 40}, 0.8f), "grounded body");
    pass &= check(field.cell(12, 13).density < 0.5f, "swept body opens a trail");
    pass &= check(!field.disturb({0, 180, 0}, {0, 180, 48}, 52, 0, 30, {0, 0, 40}, 0.8f), "airborne body");
    pass &= check(field.blade({32, 20, 0}, {32, 20, 64}, {64, 25, 0}, {96, 25, 64},
                              64.0f, 1.0f, fog::DefaultAttackTicks), "empty-space blade");
    pass &= check(field.cell(14, 13).vx == 0.0f && field.cell(14, 13).density == fog::RestDensity,
                  "blade contact does not instantly erase or move fog");
    for (int i = 0; i < 4; ++i) field.step(fog::StepSeconds);
    pass &= check(field.cell(14, 13).vx > 0.0f, "blade pushes in sweep direction");
    pass &= check(!field.blade({0, 20, 0}, {0, 20, 48}, {600, 20, 0}, {600, 20, 48},
                               64.0f, 1.0f, fog::DefaultAttackTicks), "reject teleport");
    for (int i = 0; i < 200; ++i) field.step(0.05f);
    pass &= check(std::abs(field.cell(12, 13).density - fog::RestDensity) < 0.09f, "fog recovers");
    pass &= check(field.cell(11, 13).density == 0 && field.cell(10, 13).density == fog::RestDensity, "terrain boundary");
    field.reset(0, 0);
    for (int z = 0; z < fog::Side; ++z)
        for (int x = 0; x < fog::Side; ++x)
            field.terrain(x, z, 0, true);
    auto quick = std::make_unique<fog::Field>(field);
    auto slow = std::make_unique<fog::Field>(field);
    auto horizontalOnly = std::make_unique<fog::Field>(field);
    pass &= check(!field.spin({0, 200, 0}, 110.0f, 165.0f), "spin above fog");
    pass &= check(field.spin(center, 110.0f, 260.0f), "spin reaches ground fog");
    horizontalOnly->spin(center, 110.0f, 260.0f);
    field.step(fog::StepSeconds);
    pass &= check(field.beforeStep(13, 12).vx > 0.0f && field.beforeStep(14, 12).vx == 0.0f,
                  "the direct pressure reaches the inner edge before the outer edge");
    const auto earlyRise = field.cell(13, 12).raisedDensity;
    for (int i = 1; i < fog::DefaultAttackTicks; ++i) field.step(fog::StepSeconds);
    for (int i = 0; i < fog::DefaultAttackTicks; ++i) horizontalOnly->step(fog::StepSeconds, 0.0f);
    const auto& raised = field.cell(14, 12);
    pass &= check(earlyRise < 0.025f && raised.raisedDensity > 0.05f && field.cell(16, 12).raisedDensity > 0.002f,
                  "spin lifts fog gradually and carries elevated density past the hitbox");
    pass &= check(horizontalOnly->cell(14, 12).raisedDensity == 0.0f &&
                      std::abs(raised.density - horizontalOnly->cell(14, 12).density) < 0.00001f,
                  "vertical transfer preserves total column density and can be disabled");
    pass &= check(field.cell(10, 12).vx < -40.0f && field.cell(14, 12).vx > 40.0f &&
                      field.cell(12, 10).vz < -40.0f && field.cell(12, 14).vz > 40.0f,
                  "spin moves fog outward in all directions");
    pass &= check(field.cell(12, 12).density < fog::RestDensity - 0.12f &&
                      field.cell(16, 12).density > fog::RestDensity + 0.04f,
                  "spin transports density from its center to beyond the hitbox");
    pass &= check(field.cell(14, 12).vz > 20.0f && field.cell(12, 14).vx < -20.0f, "spin adds tangential momentum");
    quick->spin(center, 110.0f, 260.0f, 1.0f, fog::MinAttackTicks);
    slow->spin(center, 110.0f, 260.0f, 1.0f, fog::MaxAttackTicks);
    for (int i = 0; i < fog::MinAttackTicks; ++i) {
        quick->step(fog::StepSeconds);
        slow->step(fog::StepSeconds);
    }
    pass &= check(quick->cell(14, 12).vx > slow->cell(14, 12).vx + 40.0f &&
                      quick->cell(12, 12).density < slow->cell(12, 12).density,
                  "longer duration spreads the push over more simulation ticks");
    for (int i = 0; i < 80; ++i) field.step(fog::StepSeconds);
    pass &= check(field.cell(14, 12).raisedDensity < 0.003f && field.cell(16, 12).raisedDensity < 0.003f,
                  "lift settles into the ground band within four seconds");
    for (int i = 80; i < 200; ++i) field.step(fog::StepSeconds);
    pass &= check(std::abs(field.cell(12, 12).density - fog::RestDensity) < 0.09f, "spin clears the fog temporarily");
    field.reset(100, 100);
    pass &= check(!field.cell(12, 13).open && field.cell(12, 13).vx == 0, "scene reset");
    field.reset(0, 0);
    for (int x = 9; x <= 15; ++x) field.terrain(x, 12, 0, true);
    pass &= check(field.disturb(center, center, 112, -20, 60, {}, 0.0f, 0.58f, 145, 1.0f,
                                fog::DefaultAttackTicks), "bomb volume");
    const auto old = field.cell(14, 12);
    field.shift(6, -3);
    pass &= check(field.worldX(8) == 64 && field.worldZ(15) == 0 && field.cell(8, 15).density == old.density &&
                      field.cell(8, 15).vx == old.vx && !field.cell(24, 15).open,
                  "rolling field preserves overlap and leaves new terrain unsampled");
    for (int i = 0; i < 5; ++i) field.step(fog::StepSeconds);
    pass &= check(field.cell(8, 15).vx > 40.0f, "queued pressure follows the same world cell after a grid shift");
    const float shiftedDensity = field.cell(8, 15).density;
    field.terrain(8, 15, 2, true);
    pass &= check(field.cell(8, 15).density == shiftedDensity, "floor refresh preserves the same deck");
    field.terrain(8, 15, 64, true);
    pass &= check(field.cell(8, 15).density == fog::RestDensity && field.cell(8, 15).vx == 0,
                  "floor refresh seeds a different deck");
    field.step(fog::StepSeconds);
    pass &= check(field.cell(8, 15).vx == 0.0f, "queued pressure cannot reach a replacement floor");
    field.reset(0, 0);
    field.terrain(12, 12, 0, true);
    field.terrain(13, 12, 0, true);
    field.terrain(12, 13, 0, true);
    field.blockEdge(12, 12, 13, 12, true);
    field.disturb(center, center, 16, 0, 30, {140, 0, 0}, 0.98f);
    field.step(fog::StepSeconds);
    pass &= check(field.cell(13, 12).density == fog::RestDensity && field.cell(12, 13).density < fog::RestDensity,
                  "wall blocks lateral transport while open neighbor still receives flow");
    field.blockEdge(12, 12, 13, 12, false);
    field.step(fog::StepSeconds);
    pass &= check(std::abs(field.cell(13, 12).density - fog::RestDensity) > 0.001f, "opening the edge restores transport");
    field.reset(0, 0);
    field.terrain(12, 12, 0, true);
    field.terrain(13, 12, 0, true);
    field.disturb(center, center, 16, 0, 30, {180, 0, 0}, 0.0f);
    field.step(fog::StepSeconds);
    const auto outgoing = field.cell(12, 12).density;
    const auto incoming = field.cell(13, 12).density;
    pass &= check(incoming > fog::RestDensity + 0.07f && outgoing < fog::RestDensity - 0.07f &&
                      std::abs(outgoing + incoming - 2.0f * fog::RestDensity) < 0.0001f,
                  "transport accumulates fog without creating mass across the face");
    field.reset(0, 0, 80.0f);
    field.terrain(14, 12, -80, true);
    pass &= check(field.disturb({160, -80, 0}, {160, -80, 0}, 36, 0, 30, {48, 0, 0}, 0.5f),
                  "body disturbance reaches the correct world position on the wide grid");
    field.shift(3, -2);
    pass &= check(field.worldX(11) == 160 && field.worldZ(14) == 0 && field.cell(11, 14).floor == -80 &&
                      field.cell(11, 14).density < fog::RestDensity,
                  "wide grid shift preserves the floor and density at the same world position");
    field.terrain(24, 24, -80, true, true);
    pass &= check(field.cell(24, 24).open && field.cell(24, 24).density == 0.0f,
                  "newly exposed terrain starts clear");
    field.step(fog::StepSeconds);
    pass &= check(field.cell(24, 24).density > 0.0f && field.cell(24, 24).density < fog::RestDensity * 0.1f,
                  "newly exposed terrain gradually recovers fog");
    fog::Field upperField;
    field.reset(0.0f, 0.0f);
    upperField.reset(0.0f, 0.0f);
    field.terrain(13, 12, -80.0f, true);
    upperField.terrain(13, 12, 100.0f, true);
    pass &= check(field.spin({0.0f, -80.0f, 0.0f}, 120.0f, 260.0f) &&
                      !upperField.spin({0.0f, -80.0f, 0.0f}, 120.0f, 260.0f) &&
                      upperField.spin({0.0f, 100.0f, 0.0f}, 120.0f, 260.0f) &&
                      !field.spin({0.0f, 100.0f, 0.0f}, 120.0f, 260.0f),
                  "the same XZ attack interacts only with the support at its height");
    field.reset(0.0f, 0.0f);
    field.terrain(12, 12, 0.0f, true);
    auto fallingBlade = field;
    const fog::Point lowBase{-25.0f, 10.0f, 0.0f}, lowTip{25.0f, 10.0f, 0.0f};
    const fog::Point highBase{-25.0f, 34.0f, 0.0f}, highTip{25.0f, 34.0f, 0.0f};
    pass &= check(field.blade(lowBase, lowTip, highBase, highTip, 42.0f, 1.0f, fog::DefaultAttackTicks) &&
                      fallingBlade.blade(highBase, highTip, lowBase, lowTip, 42.0f, 1.0f, fog::DefaultAttackTicks),
                  "vertical sword sweeps reach fog without lateral motion");
    for (int i = 0; i < fog::DefaultAttackTicks; ++i) {
        field.step(fog::StepSeconds);
        fallingBlade.step(fog::StepSeconds);
    }
    pass &= check(field.cell(12, 12).raisedDensity > 0.04f && fallingBlade.cell(12, 12).raisedDensity == 0.0f &&
                      field.cell(12, 12).density == fallingBlade.cell(12, 12).density,
                  "upward and downward blade movement transfer different amounts of fog at the same XZ position");
    field.reset(0.0f, 0.0f);
    field.terrain(12, 12, 0.0f, true);
    fallingBlade = field;
    pass &= check(field.disturb({0, 8, 0}, {0, 32, 0}, 16, -3, 10, {0, 90, 0}, 0, 0.42f, 0, 1,
                                fog::DefaultAttackTicks) &&
                      fallingBlade.disturb({0, 32, 0}, {0, 8, 0}, 16, -3, 10, {0, -90, 0}, 0, 0.42f, 0, 1,
                                           fog::DefaultAttackTicks),
                  "vertical collider sweeps queue a force in the intersected column");
    for (int i = 0; i < fog::DefaultAttackTicks; ++i) {
        field.step(fog::StepSeconds);
        fallingBlade.step(fog::StepSeconds);
    }
    pass &= check(field.cell(12, 12).raisedDensity > 0.04f && fallingBlade.cell(12, 12).raisedDensity == 0.0f &&
                      field.cell(12, 12).density == fallingBlade.cell(12, 12).density,
                  "vertical collider inertia lifts the fog only in the direction of upward movement");
    pass &= require(pass, "suite original completa");
    pass &= fluidTests();
    pass &= windTests();
    pass &= shallowTests();
    benchmark();
    return pass ? 0 : 1;
}
