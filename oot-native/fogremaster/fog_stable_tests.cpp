#include "fog_stable.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#endif
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <vector>

// Contagem por thread apenas nos caminhos explicitamente vigiados.
namespace {
thread_local bool countAllocations = false;
thread_local size_t allocations = 0;
}

void* operator new(size_t size) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace fog {
struct StableFluidTestAccess {
    static float curlTime(const StableFluid& f) { return f.curlTime_; }
    static bool curlValid(const StableFluid& f) { return f.curlValid_; }
    static bool topoDirty(const StableFluid& f) { return f.topoDirty_; }
    static void forceScalar(StableFluid& f) { f.forceScalar_ = true; }
    static void refreshCurl(StableFluid& f) { f.curlValid_ = false; f.refreshCurl(1.0f / 30.0f); }
    static void advanceCurl(StableFluid& f, float dt) { f.time_ += dt; f.refreshCurl(dt); }
    static std::vector<float> integralPotential(const StableFluid& f) {
        StableFluid reference = f;
        reference.time_ = f.curlBuildTime_;
        reference.curlValid_ = false;
        reference.refreshCurl(1.0f);
        return reference.psi_;
    }
    static std::vector<float> potential(const StableFluid& f) { return f.psi_; }
    static double maxCurlDivergence(const StableFluid& f) {
        double maximum = 0.0;
        for (int j = 0; j < f.n_; ++j) for (int i = 0; i < f.n_; ++i) {
            const double d = (static_cast<double>(f.curlU_[static_cast<size_t>(j) * (f.n_ + 1) + i + 1]) -
                              f.curlU_[static_cast<size_t>(j) * (f.n_ + 1) + i] +
                              f.curlW_[static_cast<size_t>(j + 1) * f.n_ + i] -
                              f.curlW_[static_cast<size_t>(j) * f.n_ + i]) / f.h_;
            maximum = std::max(maximum, std::abs(d));
        }
        return maximum;
    }
    static void seedDenormals(StableFluid& f) {
        std::fill(f.u_.begin(), f.u_.end(), std::numeric_limits<float>::min() * 0.125f);
        std::fill(f.w_.begin(), f.w_.end(), std::numeric_limits<float>::min() * 0.125f);
        std::fill(f.rho_.begin(), f.rho_.end(), 0.0f);
    }
};
}

namespace {

bool require(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    return condition;
}

constexpr float kCell = 7.5f;
constexpr float kDt = 1.0f / 30.0f;
constexpr float kPi = 3.14159265f;
constexpr double kBaselineCirculation = 22419.9;
constexpr double kCirculationTolerance = 0.05;
constexpr float kBaselineDiv128 = 0.076424621f;
constexpr float kBaselineDiv96 = 0.292403609f;
constexpr double kMeanBudget128 = 1.3;
constexpr double kP99Budget128 = 1.6;
constexpr int kTimingWarmup = 300;
constexpr int kTimingSamples = 10000;

fog::StableParams quietParams() {
    fog::StableParams p;
    p.damping = 0.0f;
    p.recovery = 0.0f;
    p.windRelax = 0.0f;
    p.curlAmp = 0.0f;
    return p;
}

float centerOf(const fog::StableFluid& f) { return f.originX() + 0.5f * static_cast<float>(f.size()) * f.cellSize(); }

// Arco de espada em torno de c: n segmentos com velocidade tangente (anti-horário em x,z).
void stirArc(fog::StableFluid& f, fog::Vec2 c, float r, float a0, float a1, float speed, float radius, float strength) {
    const int segs = 6;
    for (int s = 0; s < segs; ++s) {
        const float t0 = a0 + (a1 - a0) * static_cast<float>(s) / segs;
        const float t1 = a0 + (a1 - a0) * static_cast<float>(s + 1) / segs;
        const float tm = 0.5f * (t0 + t1);
        f.stirSegment({c.x + r * std::cos(t0), c.z + r * std::sin(t0)}, {c.x + r * std::cos(t1), c.z + r * std::sin(t1)},
                      {-std::sin(tm) * speed, std::cos(tm) * speed}, radius, strength);
    }
}

double circulation(const fog::StableFluid& f, fog::Vec2 c, float radius) {
    double g = 0.0;
    for (int j = 0; j < f.size(); ++j)
        for (int i = 0; i < f.size(); ++i) {
            const float dx = f.cellCenterX(i) - c.x, dz = f.cellCenterZ(j) - c.z;
            if (dx * dx + dz * dz <= radius * radius) g += f.vorticity(i, j) * f.cellSize() * f.cellSize();
        }
    return g;
}

double absVorticity(const fog::StableFluid& f) {
    double g = 0.0;
    for (int j = 0; j < f.size(); ++j)
        for (int i = 0; i < f.size(); ++i) g += std::abs(f.vorticity(i, j)) * f.cellSize() * f.cellSize();
    return g;
}

float peakSpeed(const fog::StableFluid& f) {
    float peak = 0.0f;
    for (int j = 0; j < f.size(); ++j)
        for (int i = 0; i < f.size(); ++i) {
            const float vx = f.velocityX(i, j), vz = f.velocityZ(i, j);
            if (!std::isfinite(vx) || !std::isfinite(vz)) return INFINITY;
            peak = std::max(peak, std::sqrt(vx * vx + vz * vz));
        }
    return peak;
}

bool allFinite(const fog::StableFluid& f) {
    for (int j = 0; j < f.size(); ++j)
        for (int i = 0; i < f.size(); ++i)
            if (!std::isfinite(f.density(i, j)) || !std::isfinite(f.velocityX(i, j)) || !std::isfinite(f.velocityZ(i, j)))
                return false;
    return true;
}

// Repouso com gradiente suave em x (bancos de névoa): 0.4 a 1.0 na janela de ~960 u.
float rampRest(float wx, float) { return 0.7f + 0.3f * std::sin(wx * 0.004f); }

bool testProjection(int n, int iterations, float maxRatio, float maxAbs, const char* name) {
    fog::StableFluid f;
    f.reset(n, kCell, 0.0f, 0.0f);
    auto p = quietParams();
    p.iterations = iterations;
    p.vorticity = 0.0f;
    f.setParams(p);
    const fog::Vec2 c{centerOf(f), centerOf(f)};
    // Aquece a pressão (warm start) com alguns ticks de golpe, como no jogo, e mede o último.
    for (int t = 0; t < 8; ++t) {
        stirArc(f, c, 60.0f, 0.0f, 1.5f, 250.0f, 30.0f, 0.5f);
        f.step(kDt);
    }
    const auto s = f.stats();
    const float ratio = s.divAfter / std::max(s.divBefore, 1e-20f);
    std::printf("  %s: div media antes=%.9g depois=%.9g (razao %.3g), max antes=%.4g depois=%.4g\n", name, s.divBefore,
                s.divAfter, ratio, s.maxBefore, s.maxAfter);
    bool ok = require(s.divBefore > 1e-3f && (ratio < maxRatio || s.divAfter < maxAbs), name);
    if (iterations == 20) {
        const float baseline = n == 128 ? kBaselineDiv128 : kBaselineDiv96;
        ok &= require(s.divAfter <= baseline, "projecao: divergencia nao piora a referencia pre-opt2");
    }
    return ok;
}

bool testMass() {
    bool ok = true;
    for (int n : {128, 96}) {
        fog::StableFluid f;
        f.setRestSampler(rampRest);
        f.reset(n, kCell, 0.0f, 0.0f);
        f.setRestSampler(rampRest);
        f.setParams(quietParams());
        const fog::Vec2 c{centerOf(f), centerOf(f)};
        stirArc(f, c, 80.0f, 0.0f, 2.5f, 200.0f, 40.0f, 0.5f);
        const double m0 = f.totalMass();
        for (int t = 0; t < 100; ++t) f.step(kDt);
        const double drift = std::abs(f.totalMass() - m0) / m0;
        std::printf("  N=%d massa: deriva %.4f%% em 100 ticks\n", n, drift * 100.0);
        ok &= require(drift < 0.01, n == 128 ? "massa conservada +-1% em 100 ticks (N=128)" : "massa conservada +-1% em 100 ticks (N=96)");
    }
    return ok;
}

bool testStability() {
    fog::StableFluid f;
    f.setRestSampler(rampRest);
    f.reset(128, kCell, 0.0f, 0.0f);
    f.setRestSampler(rampRest);
    auto p = fog::StableParams{};
    p.curlAmp = 40.0f;
    f.setParams(p);
    const fog::Vec2 c{centerOf(f), centerOf(f)};
    float worst = 0.0f;
    bool finite = true;
    for (int t = 0; t < 200; ++t) {
        const float a = 0.4f * static_cast<float>(t);
        stirArc(f, c, 90.0f, a, a + 0.8f, 300.0f, 35.0f, 1.0f);
        f.stirRadial(c, 300.0f, 60.0f);
        f.step(0.05f);
        finite &= allFinite(f);
        worst = std::max(worst, peakSpeed(f));
    }
    std::printf("  estabilidade: velocidade maxima %.1f u/s (teto %.0f)\n", worst, fog::StableParams{}.maxSpeed);
    bool ok = require(finite, "estabilidade: sem NaN com dt=0.05 e stir de 300 u/s por 200 ticks");
    ok &= require(worst <= fog::StableParams{}.maxSpeed * 1.4143f, "estabilidade: velocidade limitada");
    float lo = 1e9f, hi = -1e9f;
    for (int j = 0; j < 128; ++j) for (int i = 0; i < 128; ++i) { lo = std::min(lo, f.density(i, j)); hi = std::max(hi, f.density(i, j)); }
    ok &= require(lo >= -1e-4f && hi <= 1.0f + 1e-4f, "estabilidade: densidade dentro do intervalo do repouso (limitador)");
    return ok;
}

bool testStirAddsNoDensity() {
    fog::StableFluid f;
    f.setRestSampler(rampRest);
    f.reset(128, kCell, 0.0f, 0.0f);
    f.setRestSampler(rampRest);
    const std::vector<float> before(f.densityData(), f.densityData() + 128 * 128);
    const fog::Vec2 c{centerOf(f), centerOf(f)};
    stirArc(f, c, 60.0f, 0.0f, 2.0f, 250.0f, 30.0f, 1.0f);
    f.stirRadial(c, 200.0f, 50.0f);
    const bool same = std::equal(before.begin(), before.end(), f.densityData());
    return require(same, "stir nao altera a densidade (so forca)");
}

bool testVortex() {
    fog::StableFluid f;
    f.setRestSampler(rampRest);
    f.reset(128, kCell, 0.0f, 0.0f);
    f.setRestSampler(rampRest);
    auto p = fog::StableParams{};
    p.recovery = 0.0f;
    f.setParams(p);
    const fog::Vec2 c{centerOf(f), centerOf(f)};
    double peak = 0.0;
    for (int t = 0; t < 3; ++t) {  // golpe: 3 ticks de arco
        const float a = 1.0f * static_cast<float>(t);
        stirArc(f, c, 70.0f, a, a + 1.0f, 280.0f, 35.0f, 0.5f);
        f.step(kDt);
        peak = std::max(peak, std::abs(circulation(f, c, 90.0f)));
    }
    const double w0 = absVorticity(f);
    float peakOmega = 0.0f;
    for (int t = 0; t < 30; ++t) f.step(kDt);  // 1 s
    for (int j = 0; j < 128; ++j) for (int i = 0; i < 128; ++i) peakOmega = std::max(peakOmega, std::abs(f.vorticity(i, j)));
    const double w1 = absVorticity(f);
    const double g1 = circulation(f, c, 90.0f);
    std::printf("  vortice: circulacao pico=%.1f, apos 1s=%.1f; |omega| total %.1f -> %.1f; omega max=%.2f 1/s\n", peak, g1,
                w0, w1, peakOmega);
    bool ok = require(peak > 1000.0, "golpe gera circulacao mensuravel");
    ok &= require(std::abs(g1 - kBaselineCirculation) <= kCirculationTolerance * kBaselineCirculation,
                  "circulacao apos 1 s preservada em +-5% da referencia pre-opt2");
    ok &= require(std::abs(g1) > 0.2 * peak && w1 > 0.2 * w0 && peakOmega > 0.3f, "vortice persiste mais de 1 s");
    // Reabre/fecha: a densidade deformada difere do repouso e volta com recovery.
    double dev = 0.0, tot = 0.0;
    for (int j = 0; j < 128; ++j) for (int i = 0; i < 128; ++i) { dev += std::abs(f.density(i, j) - f.rest(i, j)); tot += f.rest(i, j); }
    std::printf("  densidade: desvio relativo ao repouso apos 1 s = %.3f%%\n", 100.0 * dev / tot);
    ok &= require(dev / tot > 0.002, "golpe reabre/fecha a densidade (campo deformado)");
    auto q = p;
    q.recovery = 1.0f;
    f.setParams(q);
    for (int t = 0; t < 30 * 6; ++t) f.step(kDt);
    double dev2 = 0.0;
    for (int j = 0; j < 128; ++j) for (int i = 0; i < 128; ++i) dev2 += std::abs(f.density(i, j) - f.rest(i, j));
    std::printf("  densidade: desvio apos 6 s com recovery=1: %.4f%%\n", 100.0 * dev2 / tot);
    std::printf("  velocidade maxima apos 6 s: %.2f u/s\n", peakSpeed(f));
    ok &= require(dev2 < 0.1 * dev, "densidade volta ao repouso com recovery");
    return ok;
}

bool testSolid() {
    fog::StableFluid f;
    constexpr int n = 128;
    f.reset(n, kCell, 0.0f, 0.0f);
    for (int j = 0; j < n; ++j) f.setSolid(64, j, true);  // parede vertical inteira
    for (int j = 0; j < n; ++j)
        for (int i = 65; i < n; ++i) { f.setRest(i, j, 0.0f); f.setDensity(i, j, 0.0f); }
    auto p = quietParams();
    p.vorticity = 4.0f;
    f.setParams(p);
    float leakFlux = 0.0f, leakDensity = 0.0f;
    for (int t = 0; t < 120; ++t) {
        f.stirSegment({400.0f, 480.0f}, {440.0f, 480.0f}, {300.0f, 0.0f}, 40.0f, 1.0f);  // empurra contra a parede
        f.step(kDt);
        for (int j = 0; j < n; ++j) {
            leakFlux = std::max({leakFlux, std::abs(f.faceU(64, j)), std::abs(f.faceU(65, j))});
            for (int i = 65; i < n; ++i) leakDensity = std::max(leakDensity, std::abs(f.density(i, j)));
        }
    }
    float inside = 0.0f;
    for (int j = 0; j < n; ++j) inside = std::max(inside, std::abs(f.density(64, j)));
    std::printf("  solido: fluxo max na parede=%.3g, densidade max do outro lado=%.3g, dentro=%.3g\n", leakFlux, leakDensity, inside);
    bool ok = require(leakFlux == 0.0f, "solido: nenhum fluxo atravessa a parede");
    ok &= require(leakDensity == 0.0f && inside == 0.0f, "solido: nenhuma densidade atravessa a parede");
    ok &= require(peakSpeed(f) > 1.0f, "solido: o lado agitado segue vivo");
    return ok;
}

bool testRecenter() {
    fog::StableFluid f;
    constexpr int n = 96;
    f.setRestSampler([](float, float) { return 0.9f; });
    f.reset(n, kCell, 0.0f, 0.0f);
    f.setRestSampler([](float, float) { return 0.9f; });
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) f.setDensity(i, j, 0.1f + 0.001f * static_cast<float>(i + 3 * j));
    f.stirSegment({300.0f, 300.0f}, {400.0f, 300.0f}, {200.0f, 100.0f}, 40.0f, 1.0f);
    auto p = quietParams();
    f.setParams(p);
    f.step(kDt);
    const std::vector<float> d0(f.densityData(), f.densityData() + n * n);
    std::vector<float> vx0(n * n), vz0(n * n);
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) { vx0[j * n + i] = f.velocityX(i, j); vz0[j * n + i] = f.velocityZ(i, j); }
    const float oldX0 = f.originX();
    int sx = 0, sz = 0;
    f.recenter(centerOf(f) + 5.0f * kCell + 0.2f * kCell, centerOf(f) - 3.0f * kCell, &sx, &sz);
    bool shifted = sx == 5 && sz == -3 && std::abs(f.originX() - (oldX0 + 5.0f * kCell)) < 1e-3f;
    bool kept = true, freshRest = true, freshVel = true;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int oi = i + sx, oj = j + sz;
            if (oi >= 0 && oi < n && oj >= 0 && oj < n) {
                kept &= f.density(i, j) == d0[oj * n + oi];
                // velocidade no centro: so confere o interior, longe das faces de borda recem-criadas
                if (oi > 1 && oi < n - 2 && oj > 1 && oj < n - 2 && i > 1 && i < n - 2 && j > 1 && j < n - 2)
                    kept &= std::abs(f.velocityX(i, j) - vx0[oj * n + oi]) < 1e-4f && std::abs(f.velocityZ(i, j) - vz0[oj * n + oi]) < 1e-4f;
            } else {
                freshRest &= f.density(i, j) == 0.9f;
                freshVel &= f.velocityX(i, j) == 0.0f && f.velocityZ(i, j) == 0.0f;
            }
        }
    }
    bool ok = require(shifted, "recenter: janela anda em passos inteiros de celula");
    ok &= require(kept, "recenter preserva o conteudo deslocado (densidade e velocidade)");
    ok &= require(freshRest, "recenter: celulas novas entram com densidade de repouso");
    ok &= require(freshVel, "recenter: celulas novas entram com velocidade zero");
    int s2x = 9, s2z = 9;
    f.recenter(f.originX() + 0.5f * n * kCell + 0.3f * kCell, f.originZ() + 0.5f * n * kCell, &s2x, &s2z);
    ok &= require(s2x == 0 && s2z == 0, "recenter: dentro de meia celula nao desloca");
    return ok;
}

// Tamanhos que exercitam os lotes de quatro, as caudas escalares e a troca de topologia.
bool testGridVariants() {
    bool ok = true;
    for (int n : {8, 9, 31, 32, 95, 96, 127, 128, 129}) {
        for (bool correction : {false, true}) {
            fog::StableFluid f;
            f.setRestSampler(rampRest);
            f.reset(n, kCell, 0.0f, 0.0f);
            auto p = quietParams();
            p.macCormack = correction;
            p.warmStart = correction;
            p.viscosity = 0.3f;
            f.setParams(p);
            for (int tick = 0; tick < 8; ++tick) {
                if (tick == 2 || tick == 4)
                    for (int j = 1; j < n - 1; ++j) f.setSolid(n / 2, j, tick == 2);
                f.stirSegment({kCell, kCell}, {centerOf(f), centerOf(f)}, {200.0f, -150.0f}, 30.0f, 0.5f);
                f.stirRadial({0.0f, 0.0f}, 150.0f, 30.0f);
                f.step(kDt);
                ok &= allFinite(f);
                for (int j = 0; j < n; ++j) {
                    ok &= f.faceU(0, j) == 0.0f && f.faceU(n, j) == 0.0f;
                    for (int i = 0; i < n; ++i) {
                        ok &= f.density(i, j) >= 0.0f && f.density(i, j) <= 1.0001f;
                        if (f.solid(i, j)) ok &= f.density(i, j) == 0.0f;
                        if (tick == 2 || tick == 3) {
                            if (j > 0 && j < n - 1)
                                ok &= f.faceU(n / 2, j) == 0.0f && f.faceU(n / 2 + 1, j) == 0.0f;
                        }
                    }
                }
                for (int i = 0; i < n; ++i) ok &= f.faceW(i, 0) == 0.0f && f.faceW(i, n) == 0.0f;
                if (tick == 5) f.recenter(centerOf(f) + kCell, centerOf(f) - kCell);
            }
            double energy = 0.0;
            for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
                const double u = f.velocityX(i, j), w = f.velocityZ(i, j);
                energy += u * u + w * w;
            }
            std::printf("  grade N=%d MacCormack=%d: massa=%.12g energia=%.12g div=%.9g\n",
                        n, correction ? 1 : 0, f.totalMass(), energy, f.stats().divAfter);
        }
    }
    return require(ok, "grades pares/impares, SL/MacCormack, viscosidade, warm start, solidos dinamicos e recenter");
}

bool testCurlRecenter() {
    bool ok = true;
    for (int n : {8, 31, 96, 128, 129}) {
        fog::StableFluid f;
        const float origin = -0.5f * static_cast<float>(n) * kCell;
        f.setRestSampler(rampRest);
        f.reset(n, kCell, origin, origin);
        auto p = fog::StableParams{};
        p.curlAmp = 20.0f;
        p.curlScale = 45.0f;
        p.curlSpeed = -0.5f;
        f.setParams(p);
        for (int tick = 0; tick < 12; ++tick) {
            if (tick == 5) f.recenter(2.0f * kCell, -3.0f * kCell);
            if (tick == 8) { p.curlScale = 90.0f; f.setParams(p); }
            f.step(kDt);
            ok &= allFinite(f);
        }
        double energy = 0.0;
        for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
            const double u = f.velocityX(i, j), w = f.velocityZ(i, j);
            energy += u * u + w * w;
        }
        ok &= energy > 0.0 && std::isfinite(f.stats().divAfter);
        std::printf("  curl N=%d: massa=%.12g energia=%.12g div=%.9g\n", n, f.totalMass(), energy, f.stats().divAfter);
    }
    return require(ok, "curl: coordenadas negativas, caudas SIMD, recenter e invalidacao dos parametros");
}

struct TimingSummary {
    double mean, p99, p999, maximum;
};

TimingSummary summarize(std::vector<double>& ms) {
    if (ms.empty()) return {};
    double sum = 0.0;
    for (double v : ms) sum += v;
    std::sort(ms.begin(), ms.end());
    // Nearest rank: p99.9 tem dez observacoes na cauda em 10000 amostras.
    const auto percentile = [&](double q) { return ms[static_cast<size_t>(std::ceil(q * ms.size())) - 1]; };
    return {sum / static_cast<double>(ms.size()), percentile(0.99), percentile(0.999), ms.back()};
}

void printTiming(const char* path, int n, std::vector<double>& ms) {
    const auto s = summarize(ms);
    std::printf("  TEMPO %s N=%d amostras=%zu media=%.3f p99=%.3f p99.9=%.3f max=%.3f ms\n",
                path, n, ms.size(), s.mean, s.p99, s.p999, s.maximum);
}

bool testNoAllocations() {
    fog::StableFluid f;
    // Captura maior que o armazenamento pequeno de std::function, instalada fora da medicao.
    const float values[64] = {0.9f};
    f.setRestSampler([values](float, float) { return values[0]; });
    f.setSolidSampler([](float x, float z) { return x > 80.0f && x < 90.0f && z > 20.0f; });
    f.reset(128, kCell, 0.0f, 0.0f);
    auto p = quietParams();
    p.curlAmp = 20.0f;
    p.viscosity = 0.3f;
    f.setParams(p);
    allocations = 0;
    countAllocations = true;
    f.step(0.0f);
    for (int tick = 0; tick < 12; ++tick) {
        f.stirSegment({300.0f, 300.0f}, {340.0f, 310.0f}, {200.0f, 100.0f}, 40.0f, 0.4f);
        f.stirRadial({300.0f, 300.0f}, 100.0f, 40.0f);
        f.setSolid(10, 10, (tick & 1) != 0);
        f.step(kDt);  // inclui o primeiro step e a topologia suja
        f.recenter(centerOf(f) + kCell, f.originZ() + 0.5f * f.size() * kCell - kCell);
    }
    f.recenter(10000.0f, -10000.0f);  // teletransporte
    f.step(kDt);
    countAllocations = false;
    std::printf("  alocacoes em step/recenter/stir: %zu\n", allocations);
    return require(allocations == 0, "nenhuma alocacao desde o primeiro step, inclusive recenter e stir");
}

bool testCurlSlices() {
    bool ok = true;
    for (int n : {8, 31, 96, 128, 129}) {
        fog::StableFluid f;
        f.reset(n, kCell, -200.0f, -100.0f);
        auto p = quietParams();
        p.curlAmp = 20.0f;
        p.curlScale = 45.0f;
        p.curlSpeed = -0.5f;
        f.setParams(p);
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        const auto before = fog::StableFluidTestAccess::potential(f);
        // O buffer ativo so pode mudar quando TODAS as fatias da proxima epoca estiverem prontas.
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        ok &= before == fog::StableFluidTestAccess::potential(f);
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        ok &= before == fog::StableFluidTestAccess::potential(f);
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        ok &= before != fog::StableFluidTestAccess::potential(f);
        ok &= fog::StableFluidTestAccess::potential(f) == fog::StableFluidTestAccess::integralPotential(f);
        ok &= fog::StableFluidTestAccess::maxCurlDivergence(f) < 1e-6;
        // Interrompe uma epoca em construcao: origem, parametros e reset nao podem publicar fatias antigas.
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        f.recenter(300.0f, -150.0f);
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        ok &= fog::StableFluidTestAccess::potential(f) == fog::StableFluidTestAccess::integralPotential(f);
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        p.curlScale = 90.0f;
        f.setParams(p);
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        ok &= fog::StableFluidTestAccess::potential(f) == fog::StableFluidTestAccess::integralPotential(f);
        f.reset(n + 1, kCell, -300.0f, -250.0f);
        fog::StableFluidTestAccess::advanceCurl(f, kDt);
        ok &= fog::StableFluidTestAccess::potential(f) == fog::StableFluidTestAccess::integralPotential(f);
    }
    return require(ok, "curl: fatias atomicas a 10 Hz preservam a divergencia discreta nula");
}

bool testMxcsr() {
#if defined(_M_X64) || defined(__SSE2__)
    constexpr unsigned int flushBits = 0x8040;  // FTZ e DAZ
    const unsigned int saved = _mm_getcsr();
    fog::StableFluid f;
    f.reset(32, kCell, 0.0f, 0.0f);
    f.setParams(quietParams());
    fog::StableFluidTestAccess::seedDenormals(f);
    const unsigned int gradual = saved & ~flushBits;
    _mm_setcsr(gradual);
    f.step(kDt);
    const unsigned int after = _mm_getcsr();
    bool flushed = true;
    for (int j = 0; j < f.size(); ++j) for (int i = 0; i < f.size(); ++i)
        flushed &= f.velocityX(i, j) == 0.0f && f.velocityZ(i, j) == 0.0f;
    _mm_setcsr(saved);
    bool ok = require(after == gradual, "step restaura todo o MXCSR do chamador");
    ok &= require(flushed, "step trata velocidades denormais como zero");
    _mm_setcsr(gradual);
    f.step(0.0f);
    f.step(-kDt);
    const unsigned int invalidAfter = _mm_getcsr();
    _mm_setcsr(saved);
    ok &= require(invalidAfter == gradual, "dt invalido preserva MXCSR");
    return ok;
#else
    return true;
#endif
}

double threadCycles() {
#if defined(_WIN32)
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) std::abort();
    return static_cast<double>(cycles);
#else
    return 0.0;
#endif
}

bool testTiming() {
    bool ok = true;
    for (int n : {128, 96}) {
        fog::StableFluid f;
        f.setRestSampler(rampRest);
        f.reset(n, kCell, 0.0f, 0.0f);
        auto p = fog::StableParams{};
        p.curlAmp = 20.0f;
        f.setParams(p);
        const fog::Vec2 c{centerOf(f), centerOf(f)};
        std::vector<double> ms, refreshed, cached, cycles;
        cycles.reserve(kTimingSamples);
        double worstWall = 0.0, worstWallCycles = 0.0;
        ms.reserve(kTimingSamples);
        refreshed.reserve(kTimingSamples);
        cached.reserve(kTimingSamples);
        for (int t = 0; t < kTimingWarmup + kTimingSamples; ++t) {
            const float a = 0.3f * static_cast<float>(t);
            stirArc(f, c, 70.0f, a, a + 0.6f, 250.0f, 30.0f, 0.4f);
            const float curlBefore = fog::StableFluidTestAccess::curlTime(f);
            const double cyclesBefore = threadCycles();
            const auto t0 = std::chrono::steady_clock::now();
            f.step(kDt);
            const auto t1 = std::chrono::steady_clock::now();
            const double elapsedCycles = threadCycles() - cyclesBefore;
            if (t < kTimingWarmup) continue;
            const double elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();
            ms.push_back(elapsed);
            cycles.push_back(elapsedCycles);
            if (elapsed > worstWall) { worstWall = elapsed; worstWallCycles = elapsedCycles; }
            (curlBefore != fog::StableFluidTestAccess::curlTime(f) ? refreshed : cached).push_back(elapsed);
        }
        printTiming("step", n, ms);
        printTiming("curl-publicado", n, refreshed);
        printTiming("curl-cache", n, cached);
        std::sort(cycles.begin(), cycles.end());
        std::printf("  CPU N=%d ciclos mediana=%.0f p99=%.0f; no pior tick (%.3f ms)=%.0f\n", n,
                    cycles[cycles.size() / 2], cycles[cycles.size() * 99 / 100], worstWall, worstWallCycles);
#ifdef NDEBUG
        if (n == 128) {
            const auto s = summarize(ms);
            ok &= require(s.mean <= kMeanBudget128, "META custo: N=128 media <= 1.3 ms (Release)");
            ok &= require(s.p99 <= kP99Budget128, "META custo: N=128 p99 <= 1.6 ms (Release)");
        }
#endif
    }
    return ok;
}

// Paredes em blocos de 8x6 celulas, ~15% da janela (casas/muros da vila). Determinístico.
constexpr int kGameBlockW = 8;
constexpr int kGameBlockH = 6;
constexpr double kGameSolidFraction = 0.15;
constexpr int kGameSetSolidPeriod = 10;
constexpr double kGameMeanBudget128 = 1.1, kGameP99Budget128 = 2.0, kGameMeanBudget96 = 0.7;

bool gameSolid(float wx, float wz) {
    const int bi = static_cast<int>(std::floor(wx / (kCell * kGameBlockW)));
    const int bj = static_cast<int>(std::floor(wz / (kCell * kGameBlockH)));
    std::uint32_t h = static_cast<std::uint32_t>(bi) * 374761393u + static_cast<std::uint32_t>(bj) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<double>(h & 0xFFFFu) < kGameSolidFraction * 65536.0;
}

// Parametros reais do jogo: vorticidade 4, recuperacao 0.08, curl 12, vento, setParams todo tick.
fog::StableParams gameParams() {
    fog::StableParams p;
    p.vorticity = 4.0f;
    p.recovery = 0.08f;
    p.damping = 0.6f;
    p.curlAmp = 12.0f;
    p.windX = 6.0f;
    p.windZ = -3.0f;
    return p;
}

// Caminhos SSE com solidos (SOR, vorticidade, densidade) contra o escalar: mesmo campo ate ~1e-5.
bool testSolidSseMatchesScalar() {
    bool ok = true;
    for (int n : {96, 128}) {
        fog::StableFluid f;
        f.setRestSampler(rampRest);
        f.setSolidSampler(gameSolid);
        f.reset(n, kCell, 0.0f, 0.0f);
        f.setParams(gameParams());
        fog::StableFluid ref = f;
        fog::StableFluidTestAccess::forceScalar(ref);
        const fog::Vec2 c{centerOf(f), centerOf(f)};
        double worst = 0.0, worstDiv = 0.0;
        for (int t = 0; t < 60; ++t) {
            if (t == 25)  // muda a topologia no meio: o rebuild tambem precisa ser equivalente
                for (int k = 0; k < 40; ++k) { f.setSolid(5 + k % 7, 20 + k / 7, true); ref.setSolid(5 + k % 7, 20 + k / 7, true); }
            const float a = 0.3f * static_cast<float>(t);
            stirArc(f, c, 70.0f, a, a + 0.6f, 250.0f, 30.0f, 0.4f);
            stirArc(ref, c, 70.0f, a, a + 0.6f, 250.0f, 30.0f, 0.4f);
            f.step(kDt);
            ref.step(kDt);
            worstDiv = std::max(worstDiv, static_cast<double>(std::abs(f.stats().divAfter - ref.stats().divAfter)));
        }
        for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
            worst = std::max(worst, static_cast<double>(std::abs(f.density(i, j) - ref.density(i, j))));
            worst = std::max(worst, static_cast<double>(std::abs(f.velocityX(i, j) - ref.velocityX(i, j))) * 1e-2);
            worst = std::max(worst, static_cast<double>(std::abs(f.velocityZ(i, j) - ref.velocityZ(i, j))) * 1e-2);
        }
        std::printf("  SSE x escalar com solidos N=%d: dif max (densidade; velocidade x1e-2)=%.3g, dif divergencia=%.3g, massa %.9g x %.9g\n",
                    n, worst, worstDiv, f.totalMass(), ref.totalMass());
        ok &= worst <= 1e-5 && worstDiv <= 1e-5 && std::isfinite(f.stats().divAfter);
    }
    return require(ok, "SOR, vorticidade e densidade em SSE com solidos equivalem ao escalar (~1e-5)");
}

bool testInvalidationRules() {
    fog::StableFluid f;
    f.setSolidSampler(gameSolid);
    f.reset(64, kCell, 0.0f, 0.0f);
    auto p = gameParams();
    f.setParams(p);
    f.step(kDt);
    bool ok = require(fog::StableFluidTestAccess::curlValid(f) && !fog::StableFluidTestAccess::topoDirty(f), "curl publicado e topologia limpa apos o primeiro step");
    p.windX = 9.0f; p.vorticity = 2.0f; p.recovery = 0.3f;
    f.setParams(p);
    ok &= require(fog::StableFluidTestAccess::curlValid(f), "setParams com vento/vorticidade novos nao invalida o curl");
    for (int j = 0; j < 64; ++j) for (int i = 0; i < 64; ++i) f.setSolid(i, j, f.solid(i, j));
    ok &= require(!fog::StableFluidTestAccess::topoDirty(f), "setSolid com o mesmo valor nao suja a topologia");
    for (float* field : {&p.curlAmp, &p.curlScale, &p.curlSpeed}) {
        f.step(kDt);
        f.setParams(p);
        *field += 1.0f;
        f.setParams(p);
        ok &= require(!fog::StableFluidTestAccess::curlValid(f), "mudar curlAmp/curlScale/curlSpeed invalida o curl");
    }
    f.setSolid(3, 3, !f.solid(3, 3));
    ok &= require(fog::StableFluidTestAccess::topoDirty(f), "setSolid com valor novo suja a topologia");
    f.step(kDt);
    return ok & require(!fog::StableFluidTestAccess::topoDirty(f), "o step reconstroi a topologia");
}

bool testViscosityAndVorticity() {
    fog::StableFluid f;
    f.reset(64, kCell, 0.0f, 0.0f);
    auto p = gameParams();
    p.vorticity = 2.5f;
    p.viscosity = 12.0f;
    f.setParams(p);
    const fog::Vec2 c{centerOf(f), centerOf(f)};
    stirArc(f, c, 60.0f, 0.0f, 1.4f, 180.0f, 24.0f, 1.0f);
    for (int t = 0; t < 40; ++t) f.step(kDt);
    bool finite = true;
    float vmax = 0.0f, wmax = 0.0f;
    for (int j = 0; j < f.size(); ++j) {
        for (int i = 0; i < f.size(); ++i) {
            const float vx = f.velocityX(i, j), vz = f.velocityZ(i, j), rho = f.density(i, j);
            finite &= std::isfinite(vx) && std::isfinite(vz) && std::isfinite(rho) && rho >= 0.0f;
            vmax = std::max(vmax, std::hypot(vx, vz));
            wmax = std::max(wmax, std::abs(f.vorticity(i, j)));
        }
    }
    return require(finite && vmax > 1.0f && vmax <= p.maxSpeed && wmax > 0.05f,
                   "viscosidade 12 + vorticidade 2.5 preserva vórtice finito sem explosão");
}

bool testGameTiming() {
    bool ok = true;
    for (int n : {128, 96}) {
        fog::StableFluid f;
        f.setRestSampler(rampRest);
        f.setSolidSampler(gameSolid);
        f.reset(n, kCell, 0.0f, 0.0f);
        const fog::StableParams p = gameParams();
        f.setParams(p);
        int solids = 0;
        for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) solids += f.solid(i, j) ? 1 : 0;
        const fog::Vec2 c{centerOf(f), centerOf(f)};
        std::vector<double> ms;
        ms.reserve(kTimingSamples);
        for (int t = 0; t < kTimingWarmup + kTimingSamples; ++t) {
            f.setParams(p);
            if (t % kGameSetSolidPeriod == 0)
                for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) f.setSolid(i, j, f.solid(i, j));
            const float a = 0.3f * static_cast<float>(t);
            stirArc(f, c, 70.0f, a, a + 0.6f, 250.0f, 30.0f, 0.4f);
            const auto t0 = std::chrono::steady_clock::now();
            f.step(kDt);
            const auto t1 = std::chrono::steady_clock::now();
            if (t >= kTimingWarmup) ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::printf("  jogo N=%d: celulas solidas=%d (%.1f%%)\n", n, solids, 100.0 * solids / (static_cast<double>(n) * n));
        printTiming("step-jogo", n, ms);
#ifdef NDEBUG
        const auto s = summarize(ms);
        if (n == 128) {
            ok &= require(s.mean <= kGameMeanBudget128, "META jogo: N=128 media <= 1.1 ms (Release)");
            ok &= require(s.p99 <= kGameP99Budget128, "META jogo: N=128 p99 <= 2 ms (Release)");
        } else {
            ok &= require(s.mean <= kGameMeanBudget96, "META jogo: N=96 media <= 0.7 ms (Release)");
        }
#endif
    }
    return ok;
}

void diagnoseTiming() {
    for (int n : {128, 96}) {
        fog::StableFluid f;
        f.reset(n, kCell, 0.0f, 0.0f);
        auto p = quietParams();
        f.setParams(p);
        std::vector<double> refresh, recenter, normal, denormal, flushed;
        for (auto* samples : {&refresh, &recenter, &normal, &denormal, &flushed}) samples->reserve(300);
        for (int tick = 0; tick < 340; ++tick) {
            const auto t0 = std::chrono::steady_clock::now();
            fog::StableFluidTestAccess::refreshCurl(f);
            const auto t1 = std::chrono::steady_clock::now();
            f.recenter(centerOf(f) + kCell, f.originZ() + 0.5f * n * kCell - kCell);
            const auto t2 = std::chrono::steady_clock::now();
            if (tick < 40) continue;
            refresh.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            recenter.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        }
        printTiming("refreshCurl-isolado", n, refresh);
        printTiming("recenter-isolado", n, recenter);
        std::vector<double> slices;
        slices.reserve(300);
        for (int tick = 0; tick < 340; ++tick) {
            const auto t0 = std::chrono::steady_clock::now();
            fog::StableFluidTestAccess::advanceCurl(f, kDt);
            const auto t1 = std::chrono::steady_clock::now();
            if (tick >= 40) slices.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        printTiming("curl-fatia-isolada", n, slices);
#if defined(_M_X64) || defined(__SSE2__)
        const unsigned int saved = _mm_getcsr();
        constexpr unsigned int flushBits = 0x8040;
        for (int mode = 0; mode < 3; ++mode) {
            auto& samples = mode == 0 ? normal : mode == 1 ? denormal : flushed;
            for (int tick = 0; tick < 340; ++tick) {
                if (mode != 0) fog::StableFluidTestAccess::seedDenormals(f);
                _mm_setcsr(mode == 2 ? saved | flushBits : saved & ~flushBits);
                const auto t0 = std::chrono::steady_clock::now();
                f.step(kDt);
                const auto t1 = std::chrono::steady_clock::now();
                _mm_setcsr(saved);
                if (tick >= 40) samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
        }
        printTiming("zero", n, normal);
        printTiming("denormal", n, denormal);
        printTiming("denormal-FTZ-DAZ", n, flushed);
#endif
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::strcmp(argv[1], "--timing-only") == 0) return (testTiming() & testGameTiming()) ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--diagnostics") == 0) { diagnoseTiming(); return 0; }
    bool ok = true;
    ok &= testNoAllocations();
    ok &= testMxcsr();
    ok &= testCurlSlices();
    ok &= testProjection(128, 20, 0.8f, 0.0f, "projecao N=128, 20 iteracoes (orcamento do jogo): reduz a divergencia (convergencia parcial)");
    ok &= testProjection(96, 20, 0.8f, 0.0f, "projecao N=96, 20 iteracoes (orcamento do jogo): reduz a divergencia (convergencia parcial)");
    ok &= testProjection(128, 400, 0.0f, 1e-3f, "projecao N=128, 400 iteracoes: divergencia media residual < 1e-3 1/s (absoluta)");
    ok &= testProjection(96, 300, 0.0f, 1e-3f, "projecao N=96, 300 iteracoes: divergencia media residual < 1e-3 1/s (absoluta)");
    ok &= testMass();
    ok &= testStability();
    ok &= testStirAddsNoDensity();
    ok &= testVortex();
    ok &= testSolid();
    ok &= testRecenter();
    ok &= testGridVariants();
    ok &= testCurlRecenter();
    ok &= testSolidSseMatchesScalar();
    ok &= testInvalidationRules();
    ok &= testViscosityAndVorticity();
    if (!(argc == 2 && std::strcmp(argv[1], "--checks-only") == 0)) { ok &= testTiming(); ok &= testGameTiming(); }
    std::printf("%s\n", ok ? "TODOS OS TESTES PASSARAM" : "HA FALHAS");
    return ok ? 0 : 1;
}
