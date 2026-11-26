#include "fog_stable.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace fog {
namespace {

constexpr float kCurlRefreshSeconds = 0.1f;  // o potencial do curl-noise anda devagar: 10 Hz basta
constexpr float kSigmaPerRadius = 0.5f;      // sigma da gaussiana do stir = radius / 2
#if defined(_M_X64) || defined(__SSE2__)
// O host conserva seu ambiente numerico; apenas este solver descarta subnormais.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | kFlushBits); }
    ~ScopedFlushDenormals() { _mm_setcsr(saved_); }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;
private:
    static constexpr unsigned int kFlushBits = 0x8040;  // FTZ (15), DAZ (6)
    unsigned int saved_;
};
#endif

enum class AdvectGrid { Centers, FaceU, FaceW };

constexpr float kReachSigmas = 3.0f;         // corte do splat

// Bilinear com clamp. mask != nullptr: células marcadas não pesam (renormaliza); tudo sólido -> fallback.
// mn/mx recebem o mínimo e o máximo dos cantos usados (limitador do MacCormack).
float bilerp(const float* a, int w, int h, float gx, float gz, const unsigned char* mask = nullptr,
             float fallback = 0.0f, float* mn = nullptr, float* mx = nullptr) {
    gx = std::clamp(gx, 0.0f, static_cast<float>(w - 1));
    gz = std::clamp(gz, 0.0f, static_cast<float>(h - 1));
    const int i0 = std::min(static_cast<int>(gx), w - 1);
    const int j0 = std::min(static_cast<int>(gz), h - 1);
    const int i1 = std::min(i0 + 1, w - 1);
    const int j1 = std::min(j0 + 1, h - 1);
    const float fx = gx - static_cast<float>(i0);
    const float fz = gz - static_cast<float>(j0);
    const size_t k00 = static_cast<size_t>(j0) * w + i0, k10 = static_cast<size_t>(j0) * w + i1;
    const size_t k01 = static_cast<size_t>(j1) * w + i0, k11 = static_cast<size_t>(j1) * w + i1;
    float w00 = (1 - fx) * (1 - fz), w10 = fx * (1 - fz), w01 = (1 - fx) * fz, w11 = fx * fz;
    if (mask) {
        if (mask[k00]) w00 = 0;
        if (mask[k10]) w10 = 0;
        if (mask[k01]) w01 = 0;
        if (mask[k11]) w11 = 0;
    }
    const float sum = w00 + w10 + w01 + w11;
    if (sum <= 1e-6f) {
        if (mn) *mn = *mx = fallback;
        return fallback;
    }
    if (mn) {
        float lo = 1e30f, hi = -1e30f;
        const float v[4] = {a[k00], a[k10], a[k01], a[k11]};
        const float ww[4] = {w00, w10, w01, w11};
        for (int c = 0; c < 4; ++c) {
            if (ww[c] <= 0.0f && mask) continue;
            lo = std::min(lo, v[c]);
            hi = std::max(hi, v[c]);
        }
        *mn = lo;
        *mx = hi;
    }
    return (a[k00] * w00 + a[k10] * w10 + a[k01] * w01 + a[k11] * w11) / sum;
}

// Bilinear sem máscara; lo/hi = mínimo e máximo dos 4 cantos (limitador do MacCormack).
struct Taps {
    int k00, k10, k01, k11;
    float fx, fz;
};

inline Taps makeTaps(int w, int h, float gx, float gz) {
    gx = std::min(std::max(gx, 0.0f), static_cast<float>(w - 1));
    gz = std::min(std::max(gz, 0.0f), static_cast<float>(h - 1));
    const int i0 = std::min(static_cast<int>(gx), w - 1);
    const int j0 = std::min(static_cast<int>(gz), h - 1);
    const int i1 = std::min(i0 + 1, w - 1);
    const int j1 = std::min(j0 + 1, h - 1);
    return {j0 * w + i0, j0 * w + i1, j1 * w + i0, j1 * w + i1, gx - static_cast<float>(i0), gz - static_cast<float>(j0)};
}

inline float lerpTaps(const float* a, const Taps& t) {
    const float top = a[t.k00] + (a[t.k10] - a[t.k00]) * t.fx;
    const float bot = a[t.k01] + (a[t.k11] - a[t.k01]) * t.fx;
    return top + (bot - top) * t.fz;
}

#if defined(_M_X64) || defined(__SSE2__)
inline __m128i multiplyFour(__m128i a, __m128i b) {
    const __m128i even = _mm_mul_epu32(a, b);
    const __m128i odd = _mm_mul_epu32(_mm_srli_si128(a, 4), _mm_srli_si128(b, 4));
    return _mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(2, 0, 2, 0)),
                             _mm_shuffle_epi32(odd, _MM_SHUFFLE(2, 0, 2, 0)));
}

// SSE2 de base no x64: quatro rastros independentes, sem alterar as flags do build.
inline __m128 gatherFour(const float* a, __m128i indices) {
    alignas(16) int k[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(k), indices);
    return _mm_setr_ps(a[k[0]], a[k[1]], a[k[2]], a[k[3]]);
}

// near != nullptr: *nearAny recebe o OR de near[] nas quatro celulas k00 (flag "algum canto do bilinear e solido").
inline __m128 sampleFour(const float* a, int w, int h, __m128 gx, __m128 gz,
                          float* lo = nullptr, float* hi = nullptr,
                          const unsigned char* near = nullptr, int* nearAny = nullptr) {
    const __m128 zero = _mm_setzero_ps();
    const __m128 maxX = _mm_set1_ps(static_cast<float>(w - 1));
    const __m128 maxZ = _mm_set1_ps(static_cast<float>(h - 1));
    gx = _mm_min_ps(_mm_max_ps(gx, zero), maxX);
    gz = _mm_min_ps(_mm_max_ps(gz, zero), maxZ);
    const __m128i ix = _mm_cvttps_epi32(gx), iz = _mm_cvttps_epi32(gz);
    const __m128 x = _mm_cvtepi32_ps(ix), z = _mm_cvtepi32_ps(iz);
    const __m128 fx = _mm_sub_ps(gx, x), fz = _mm_sub_ps(gz, z);
    // Indices inteiros tambem em grades cujo total de celulas excede a precisao do float.
    const __m128i k00 = _mm_add_epi32(multiplyFour(iz, _mm_set1_epi32(w)), ix);
    const __m128i dx = _mm_andnot_si128(_mm_cmpeq_epi32(ix, _mm_set1_epi32(w - 1)), _mm_set1_epi32(1));
    const __m128i dz = _mm_andnot_si128(_mm_cmpeq_epi32(iz, _mm_set1_epi32(h - 1)), _mm_set1_epi32(w));
    if (near) {
        alignas(16) int kk[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(kk), k00);
        *nearAny = near[kk[0]] | near[kk[1]] | near[kk[2]] | near[kk[3]];
    }
    const __m128 aa = gatherFour(a, k00), b = gatherFour(a, _mm_add_epi32(k00, dx));
    const __m128 c = gatherFour(a, _mm_add_epi32(k00, dz)), d = gatherFour(a, _mm_add_epi32(_mm_add_epi32(k00, dz), dx));
    if (lo) {
        _mm_storeu_ps(lo, _mm_min_ps(_mm_min_ps(aa, b), _mm_min_ps(c, d)));
        _mm_storeu_ps(hi, _mm_max_ps(_mm_max_ps(aa, b), _mm_max_ps(c, d)));
    }
    const __m128 top = _mm_add_ps(aa, _mm_mul_ps(_mm_sub_ps(b, aa), fx));
    const __m128 bot = _mm_add_ps(c, _mm_mul_ps(_mm_sub_ps(d, c), fx));
    return _mm_add_ps(top, _mm_mul_ps(_mm_sub_ps(bot, top), fz));
}
#endif

float hash01(int x, int y, int z) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u +
                      static_cast<std::uint32_t>(z) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float valueNoise(float x, float y, float z) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    const float tx = smooth(x - fx), ty = smooth(y - fy), tz = smooth(z - fz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float c[2];
    for (int dz = 0; dz < 2; ++dz) {
        const float a = lerp(hash01(ix, iy, iz + dz), hash01(ix + 1, iy, iz + dz), tx);
        const float b = lerp(hash01(ix, iy + 1, iz + dz), hash01(ix + 1, iy + 1, iz + dz), tx);
        c[dz] = lerp(a, b, ty);
    }
    return lerp(c[0], c[1], tz);
}

#if defined(_M_X64) || defined(__SSE2__)
// Quatro bytes de solid_ -> mascara de 32 bits por lane (todos os bits = celula de fluido).
inline __m128 fluidMaskFour(const unsigned char* solid) {
    int packed;
    std::memcpy(&packed, solid, sizeof packed);
    __m128i v = _mm_cvtsi32_si128(packed);
    v = _mm_unpacklo_epi8(v, _mm_setzero_si128());
    v = _mm_unpacklo_epi16(v, _mm_setzero_si128());
    return _mm_castsi128_ps(_mm_cmpeq_epi32(v, _mm_setzero_si128()));
}

inline __m128 hashFour(__m128i x, int y, int z) {
    const std::uint32_t yz = static_cast<std::uint32_t>(y) * 668265263u + static_cast<std::uint32_t>(z) * 2147483647u;
    __m128i h = _mm_add_epi32(multiplyFour(x, _mm_set1_epi32(374761393)), _mm_set1_epi32(static_cast<int>(yz)));
    h = multiplyFour(_mm_xor_si128(h, _mm_srli_epi32(h, 13)), _mm_set1_epi32(1274126177));
    h = _mm_xor_si128(h, _mm_srli_epi32(h, 16));
    return _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(h, _mm_set1_epi32(0xFFFFFF))), _mm_set1_ps(1.0f / 16777216.0f));
}

// Mesmo potencial e mesma frequencia de refresh; apenas quatro coordenadas x por lote.
inline __m128 valueNoiseFour(__m128 x, float y, float z) {
    const float fy = std::floor(y), fz = std::floor(z);
    const int iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    __m128i ix = _mm_cvttps_epi32(x);
    ix = _mm_add_epi32(ix, _mm_castps_si128(_mm_cmplt_ps(x, _mm_cvtepi32_ps(ix))));
    const __m128 t = _mm_sub_ps(x, _mm_cvtepi32_ps(ix));
    const __m128 tx = _mm_mul_ps(_mm_mul_ps(t, t), _mm_sub_ps(_mm_set1_ps(3.0f), _mm_mul_ps(_mm_set1_ps(2.0f), t)));
    const __m128 ty = _mm_set1_ps(smooth(y - fy)), tz = _mm_set1_ps(smooth(z - fz));
    const __m128i next = _mm_add_epi32(ix, _mm_set1_epi32(1));
    __m128 c[2];
    for (int dz = 0; dz < 2; ++dz) {
        const __m128 h00 = hashFour(ix, iy, iz + dz), h10 = hashFour(next, iy, iz + dz);
        const __m128 h01 = hashFour(ix, iy + 1, iz + dz), h11 = hashFour(next, iy + 1, iz + dz);
        const __m128 a = _mm_add_ps(h00, _mm_mul_ps(_mm_sub_ps(h10, h00), tx));
        const __m128 b = _mm_add_ps(h01, _mm_mul_ps(_mm_sub_ps(h11, h01), tx));
        c[dz] = _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), ty));
    }
    return _mm_add_ps(c[0], _mm_mul_ps(_mm_sub_ps(c[1], c[0]), tz));
}
#endif

template <typename T>
void shiftInPlace(std::vector<T>& a, std::vector<T>& tmp, int w, int h, int sx, int sz, T fill) {
    std::copy_n(a.begin(), static_cast<size_t>(w) * h, tmp.begin());
    const int begin = std::clamp(-sx, 0, w), end = std::clamp(w - sx, 0, w);
    for (int j = 0; j < h; ++j) {
        auto row = a.begin() + static_cast<std::ptrdiff_t>(j) * w;
        const int sourceRow = j + sz;
        if (sourceRow < 0 || sourceRow >= h || begin >= end) {
            std::fill_n(row, w, fill);
            continue;
        }
        std::fill_n(row, begin, fill);
        std::copy_n(tmp.begin() + static_cast<std::ptrdiff_t>(sourceRow) * w + begin + sx, end - begin, row + begin);
        std::fill(row + end, row + w, fill);
    }
}

}  // namespace

void StableFluid::reset(int n, float cellSize, float originX, float originZ) {
    n_ = std::max(n, 8);
    h_ = cellSize;
    x0_ = originX;
    z0_ = originZ;
    time_ = 0.0f;
    const size_t cells = static_cast<size_t>(n_) * n_;
    const size_t faces = static_cast<size_t>(n_ + 1) * n_;
    const size_t corners = static_cast<size_t>(n_ + 1) * (n_ + 1);
    for (auto* v : {&u_, &w_, &un_, &wn_, &hatU_, &hatW_, &fu_, &fw_, &curlU_, &curlW_}) v->assign(faces, 0.0f);
    for (auto* v : {&rho_, &rhoN_, &hatR_, &rest_, &press_, &cu_, &cw_, &omega_, &scratch_, &scratch2_})
        v->assign(cells, 0.0f);
    backX_.assign(corners, 0.0f);
    backZ_.assign(corners, 0.0f);
    loBuf_.assign(corners, 0.0f);
    hiBuf_.assign(corners, 0.0f);
    shiftTmp_.assign(corners, 0.0f);
    psi_.assign(corners, 0.0f);
    psiNext_.assign(corners, 0.0f);
    wallU_.assign(faces, 0);
    wallW_.assign(faces, 0);
    nbr_.assign(cells, 0);
    nearSolid_.assign(cells, 0);
    sorMask_.assign(cells * 4, 0u);
    sorInv_.assign(cells, 0.0f);
    solid_.assign(cells, 0);
    pad_.assign(cells + 2 * static_cast<size_t>(n_), 0.0f);
    packedPress_.assign(static_cast<size_t>(n_ + 2) * n_ + 2, 0.0f);
    topoDirty_ = true;
    solidTmp_.assign(corners, 0);
    curlValid_ = false;
    dirtyForce_ = false;
    stats_ = {};
    std::fill(rest_.begin(), rest_.end(), 1.0f);
    for (int j = 0; j < n_; ++j) {
        for (int i = 0; i < n_; ++i) {
            if (solidFn_) solid_[idx(i, j)] = solidFn_(cellCenterX(i), cellCenterZ(j)) ? 1 : 0;
            if (restFn_) rest_[idx(i, j)] = restFn_(cellCenterX(i), cellCenterZ(j));
        }
    }
    for (size_t k = 0; k < cells; ++k) rho_[k] = solid_[k] ? 0.0f : rest_[k];
}

void StableFluid::setSolidSampler(SolidSampler fn) {
    solidFn_ = std::move(fn);
    if (!solidFn_) return;
    for (int j = 0; j < n_; ++j)
        for (int i = 0; i < n_; ++i) setSolid(i, j, solidFn_(cellCenterX(i), cellCenterZ(j)));
}

void StableFluid::setRestSampler(RestSampler fn) {
    restFn_ = std::move(fn);
    if (!restFn_) return;
    for (int j = 0; j < n_; ++j)
        for (int i = 0; i < n_; ++i) setRest(i, j, restFn_(cellCenterX(i), cellCenterZ(j)));
}

void StableFluid::setSolid(int i, int j, bool solid) {
    const unsigned char value = solid ? 1 : 0;
    if (solid_[idx(i, j)] != value) {
        solid_[idx(i, j)] = value;
        topoDirty_ = true;
    }
    if (solid) rho_[idx(i, j)] = 0.0f;
    else if (rho_[idx(i, j)] == 0.0f) rho_[idx(i, j)] = rest_[idx(i, j)];
}

void StableFluid::setRest(int i, int j, float rho) { rest_[idx(i, j)] = rho; }

void StableFluid::setDensity(int i, int j, float rho) { rho_[idx(i, j)] = solid_[idx(i, j)] ? 0.0f : rho; }

void StableFluid::recenter(float x, float z, int* shiftX, int* shiftZ) {
    const float ox = x - 0.5f * static_cast<float>(n_) * h_;
    const float oz = z - 0.5f * static_cast<float>(n_) * h_;
    const int sx = static_cast<int>(std::lround((ox - x0_) / h_));
    const int sz = static_cast<int>(std::lround((oz - z0_) / h_));
    if (shiftX) *shiftX = sx;
    if (shiftZ) *shiftZ = sz;
    if (sx == 0 && sz == 0) return;
    // Mais de uma janela de distância: tudo é novo (teletransporte).
    shiftInPlace(u_, shiftTmp_, n_ + 1, n_, sx, sz, 0.0f);
    shiftInPlace(fu_, shiftTmp_, n_ + 1, n_, sx, sz, 0.0f);
    shiftInPlace(w_, shiftTmp_, n_, n_ + 1, sx, sz, 0.0f);
    shiftInPlace(fw_, shiftTmp_, n_, n_ + 1, sx, sz, 0.0f);
    shiftInPlace(rho_, shiftTmp_, n_, n_, sx, sz, 0.0f);
    shiftInPlace(rest_, shiftTmp_, n_, n_, sx, sz, 1.0f);
    shiftInPlace(press_, shiftTmp_, n_, n_, sx, sz, 0.0f);
    std::vector<unsigned char>& st = solidTmp_;
    shiftInPlace(solid_, st, n_, n_, sx, sz, static_cast<unsigned char>(0));
    x0_ += static_cast<float>(sx) * h_;
    z0_ += static_cast<float>(sz) * h_;
    bool newSolid = false;
    for (int j = 0; j < n_; ++j) {
        for (int i = 0; i < n_; ++i) {
            const int si = i + sx, sj = j + sz;
            if (si >= 0 && si < n_ && sj >= 0 && sj < n_) continue;
            const float cx = cellCenterX(i), cz = cellCenterZ(j);
            if (restFn_) rest_[idx(i, j)] = restFn_(cx, cz);
            if (solidFn_) {
                solid_[idx(i, j)] = solidFn_(cx, cz) ? 1 : 0;
                newSolid |= solid_[idx(i, j)] != 0;
            }
            rho_[idx(i, j)] = solid_[idx(i, j)] ? 0.0f : rest_[idx(i, j)];
        }
    }
    curlValid_ = false;
    // Sem solidos antes e sem solido novo na borda, a topologia continua a mesma apos o deslocamento.
    topoDirty_ = topoDirty_ || !allFluid_ || newSolid;
    enforceBoundary();
    centerVelocity();
}

void StableFluid::stirSegment(Vec2 a, Vec2 b, Vec2 velocity, float radius, float strength) {
    if (radius <= 0.0f || strength == 0.0f) return;
    dirtyForce_ = true;
    const float sigma = radius * kSigmaPerRadius;
    const float reach = sigma * kReachSigmas;
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    const float abx = b.x - a.x, abz = b.z - a.z;
    const float len2 = abx * abx + abz * abz;
    auto weight = [&](float px, float pz) {
        float t = len2 > 1e-12f ? ((px - a.x) * abx + (pz - a.z) * abz) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float dx = px - (a.x + abx * t), dz = pz - (a.z + abz * t);
        return std::exp(-(dx * dx + dz * dz) * inv2s2);
    };
    const float minX = std::min(a.x, b.x) - reach, maxX = std::max(a.x, b.x) + reach;
    const float minZ = std::min(a.z, b.z) - reach, maxZ = std::max(a.z, b.z) + reach;
    // u: faces em x0 + i h, z0 + (j + .5) h
    {
        const int i0 = std::max(0, static_cast<int>(std::floor((minX - x0_) / h_)));
        const int i1 = std::min(n_, static_cast<int>(std::ceil((maxX - x0_) / h_)));
        const int j0 = std::max(0, static_cast<int>(std::floor((minZ - z0_) / h_ - 0.5f)));
        const int j1 = std::min(n_ - 1, static_cast<int>(std::ceil((maxZ - z0_) / h_ - 0.5f)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                fu_[static_cast<size_t>(j) * (n_ + 1) + i] +=
                    strength * velocity.x * weight(x0_ + static_cast<float>(i) * h_, z0_ + (static_cast<float>(j) + 0.5f) * h_);
    }
    {
        const int i0 = std::max(0, static_cast<int>(std::floor((minX - x0_) / h_ - 0.5f)));
        const int i1 = std::min(n_ - 1, static_cast<int>(std::ceil((maxX - x0_) / h_ - 0.5f)));
        const int j0 = std::max(0, static_cast<int>(std::floor((minZ - z0_) / h_)));
        const int j1 = std::min(n_, static_cast<int>(std::ceil((maxZ - z0_) / h_)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                fw_[static_cast<size_t>(j) * n_ + i] +=
                    strength * velocity.z * weight(x0_ + (static_cast<float>(i) + 0.5f) * h_, z0_ + static_cast<float>(j) * h_);
    }
}

void StableFluid::stirRadial(Vec2 center, float speed, float radius) {
    if (radius <= 0.0f || speed == 0.0f) return;
    dirtyForce_ = true;
    const float sigma = radius * kSigmaPerRadius;
    const float reach = sigma * kReachSigmas;
    // Perfil (d/sigma) e^(1/2 - d²/2sigma²): zero no centro, pico 1 em d = sigma (anel que empurra para fora).
    auto push = [&](float px, float pz, float* gx, float* gz) {
        const float dx = px - center.x, dz = pz - center.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < 1e-4f) { *gx = *gz = 0.0f; return; }
        const float g = (d / sigma) * std::exp(0.5f - d * d / (2.0f * sigma * sigma));
        *gx = g * dx / d;
        *gz = g * dz / d;
    };
    const int i0 = std::max(0, static_cast<int>(std::floor((center.x - reach - x0_) / h_ - 0.5f)));
    const int i1 = std::min(n_, static_cast<int>(std::ceil((center.x + reach - x0_) / h_ + 0.5f)));
    const int j0 = std::max(0, static_cast<int>(std::floor((center.z - reach - z0_) / h_ - 0.5f)));
    const int j1 = std::min(n_, static_cast<int>(std::ceil((center.z + reach - z0_) / h_ + 0.5f)));
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            float gx = 0, gz = 0;
            if (j < n_) {
                push(x0_ + static_cast<float>(i) * h_, z0_ + (static_cast<float>(j) + 0.5f) * h_, &gx, &gz);
                fu_[static_cast<size_t>(j) * (n_ + 1) + i] += speed * gx;
            }
            if (i < n_) {
                push(x0_ + (static_cast<float>(i) + 0.5f) * h_, z0_ + static_cast<float>(j) * h_, &gx, &gz);
                fw_[static_cast<size_t>(j) * n_ + i] += speed * gz;
            }
        }
    }
}

void StableFluid::rebuildTopology() {
    allFluid_ = std::none_of(solid_.begin(), solid_.end(), [](unsigned char v) { return v != 0; });
    for (int j = 0; j < n_; ++j)
        for (int i = 0; i <= n_; ++i)
            wallU_[static_cast<size_t>(j) * (n_ + 1) + i] = i == 0 || i == n_ || solid_[idx(i - 1, j)] || solid_[idx(i, j)];
    for (int j = 0; j <= n_; ++j)
        for (int i = 0; i < n_; ++i)
            wallW_[static_cast<size_t>(j) * n_ + i] = j == 0 || j == n_ || solid_[idx(i, j - 1)] || solid_[idx(i, j)];
    for (int j = 0; j < n_; ++j) {
        for (int i = 0; i < n_; ++i) {
            const int l = !wallU_[static_cast<size_t>(j) * (n_ + 1) + i], r = !wallU_[static_cast<size_t>(j) * (n_ + 1) + i + 1];
            const int d = !wallW_[static_cast<size_t>(j) * n_ + i], u = !wallW_[static_cast<size_t>(j + 1) * n_ + i];
            nbr_[idx(i, j)] = static_cast<unsigned char>(l | (r << 1) | (d << 2) | (u << 3));
        }
    }
    if (!allFluid_) buildSolidCaches();
    topoDirty_ = false;
}

// Caches usados pelos caminhos SSE quando ha solidos; so mudam com a topologia.
void StableFluid::buildSolidCaches() {
    const int n = n_;
    for (int j = 0; j < n; ++j) {
        const int jn = std::min(j + 1, n - 1);
        for (int i = 0; i < n; ++i) {
            const int in = std::min(i + 1, n - 1);
            nearSolid_[idx(i, j)] = solid_[idx(i, j)] | solid_[idx(in, j)] | solid_[idx(i, jn)] | solid_[idx(in, jn)];
        }
    }
    if (n & 1) return;
    const int half = n / 2;
    const size_t plane = static_cast<size_t>(n) * half;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const size_t pos = static_cast<size_t>(j) * half + i / 2;
            const size_t color = static_cast<size_t>((i + j) & 1);
            const int code = nbr_[idx(i, j)];
            for (int m = 0; m < 4; ++m) sorMask_[(color * 4 + m) * plane + pos] = ((code >> m) & 1) ? ~0u : 0u;
            const int nf = (code & 1) + ((code >> 1) & 1) + ((code >> 2) & 1) + ((code >> 3) & 1);
            sorInv_[color * plane + pos] = nf ? 1.0f / static_cast<float>(nf) : 0.0f;
        }
    }
}

void StableFluid::enforceBoundary() {
    if (topoDirty_) rebuildTopology();
    for (size_t k = 0; k < u_.size(); ++k) u_[k] = wallU_[k] ? 0.0f : u_[k];
    for (size_t k = 0; k < w_.size(); ++k) w_[k] = wallW_[k] ? 0.0f : w_[k];
}

void StableFluid::centerVelocity() {
    for (int j = 0; j < n_; ++j) {
        for (int i = 0; i < n_; ++i) {
            cu_[idx(i, j)] = 0.5f * (u_[static_cast<size_t>(j) * (n_ + 1) + i] + u_[static_cast<size_t>(j) * (n_ + 1) + i + 1]);
            cw_[idx(i, j)] = 0.5f * (w_[static_cast<size_t>(j) * n_ + i] + w_[static_cast<size_t>(j + 1) * n_ + i]);
        }
    }
}

float StableFluid::vorticity(int i, int j) const {
    const int il = std::max(i - 1, 0), ir = std::min(i + 1, n_ - 1);
    const int jl = std::max(j - 1, 0), jr = std::min(j + 1, n_ - 1);
    const float dwdx = (cw_[idx(ir, j)] - cw_[idx(il, j)]) / (static_cast<float>(ir - il) * h_);
    const float dudz = (cu_[idx(i, jr)] - cu_[idx(i, jl)]) / (static_cast<float>(jr - jl) * h_);
    return dwdx - dudz;
}

void StableFluid::refreshCurl(float dt) {
    // Publicacao atomica do potencial: misturar linhas de epocas diferentes quebraria div(curl)=0.
    // O primeiro campo e as invalidacoes sao imediatos; os refreshes seguintes preparam a proxima
    // epoca em fatias proporcionais a dt, sem reduzir resolucao, octavas ou frequencia.
    const bool initial = !curlValid_;
    if (initial) curlNextRow_ = 0;
    if (curlNextRow_ == 0) curlBuildTime_ = initial ? time_ : curlTime_ + kCurlRefreshSeconds;
    const int rows = initial ? n_ + 1 : std::max(1, static_cast<int>(std::ceil(
        static_cast<float>(n_ + 1) * std::min(dt / kCurlRefreshSeconds, 1.0f))));
    const int endRow = std::min(n_ + 1, curlNextRow_ + rows);
    const float s = std::max(params_.curlScale, h_);
    const float t = curlBuildTime_ * params_.curlSpeed;
    for (int j = curlNextRow_; j < endRow; ++j) {
        int first = 0;
#if defined(_M_X64) || defined(__SSE2__)
        const float rowZ = (z0_ + static_cast<float>(j) * h_) / s;
        for (; first + 4 <= n_ + 1; first += 4) {
            const __m128 x = _mm_add_ps(_mm_set1_ps(static_cast<float>(first)), _mm_setr_ps(0, 1, 2, 3));
            const __m128 px = _mm_div_ps(_mm_add_ps(_mm_set1_ps(x0_), _mm_mul_ps(x, _mm_set1_ps(h_))), _mm_set1_ps(s));
            const __m128 a = valueNoiseFour(px, rowZ, t);
            const __m128 b = valueNoiseFour(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(2.0f), px), _mm_set1_ps(17.0f)),
                                           2.0f * rowZ, 2.0f * t + 5.0f);
            _mm_storeu_ps(psiNext_.data() + static_cast<size_t>(j) * (n_ + 1) + first,
                          _mm_mul_ps(_mm_set1_ps(s), _mm_add_ps(a, _mm_mul_ps(_mm_set1_ps(0.5f), b))));
        }
#endif
        for (int i = first; i <= n_; ++i) {
            const float px = (x0_ + static_cast<float>(i) * h_) / s, pz = (z0_ + static_cast<float>(j) * h_) / s;
            psiNext_[static_cast<size_t>(j) * (n_ + 1) + i] =
                s * (valueNoise(px, pz, t) + 0.5f * valueNoise(2.0f * px + 17.0f, 2.0f * pz, 2.0f * t + 5.0f));
        }
    }
    curlNextRow_ = endRow;
    if (curlNextRow_ <= n_ || (!initial && time_ - curlTime_ < kCurlRefreshSeconds)) return;
    psi_.swap(psiNext_);
    curlNextRow_ = 0;
    // u = dpsi/dz, w = -dpsi/dx entre cantos adjacentes: divergência discreta nula por construção.
    for (int j = 0; j < n_; ++j)
        for (int i = 0; i <= n_; ++i)
            curlU_[static_cast<size_t>(j) * (n_ + 1) + i] =
                (psi_[static_cast<size_t>(j + 1) * (n_ + 1) + i] - psi_[static_cast<size_t>(j) * (n_ + 1) + i]) / h_;
    for (int j = 0; j <= n_; ++j)
        for (int i = 0; i < n_; ++i)
            curlW_[static_cast<size_t>(j) * n_ + i] =
                -(psi_[static_cast<size_t>(j) * (n_ + 1) + i + 1] - psi_[static_cast<size_t>(j) * (n_ + 1) + i]) / h_;
    curlValid_ = true;
    curlTime_ = time_;
}

void StableFluid::applyForces(float dt) {
    const size_t nu = u_.size();
    const float relax = std::min(1.0f, params_.windRelax * dt);
    if (relax > 0.0f) {
        for (size_t k = 0; k < nu; ++k) u_[k] += relax * (params_.windX - u_[k]);
        for (size_t k = 0; k < nu; ++k) w_[k] += relax * (params_.windZ - w_[k]);
    }
    if (params_.curlAmp != 0.0f) {
        refreshCurl(dt);
        const float a = params_.curlAmp * dt;
        for (size_t k = 0; k < nu; ++k) u_[k] += a * curlU_[k];
        for (size_t k = 0; k < nu; ++k) w_[k] += a * curlW_[k];
    }
    if (dirtyForce_) {
        for (size_t k = 0; k < nu; ++k) u_[k] += fu_[k];
        for (size_t k = 0; k < nu; ++k) w_[k] += fw_[k];
        std::fill(fu_.begin(), fu_.end(), 0.0f);
        std::fill(fw_.begin(), fw_.end(), 0.0f);
        dirtyForce_ = false;
    }
    enforceBoundary();
}

void StableFluid::vorticityConfinement(float dt) {
    if (params_.vorticity <= 0.0f) return;
    centerVelocity();
    for (int j = 0; j < n_; ++j) {
        int first = 0;
#if defined(_M_X64) || defined(__SSE2__)
        if (!forceScalar_) {
            omega_[idx(0, j)] = solid_[idx(0, j)] ? 0.0f : vorticity(0, j);
            first = 1;
            const int jl = std::max(j - 1, 0), jr = std::min(j + 1, n_ - 1);
            const __m128 dx = _mm_set1_ps(2.0f * h_);
            const __m128 dz = _mm_set1_ps(static_cast<float>(jr - jl) * h_);
            for (; first + 4 <= n_ - 1; first += 4) {
                const size_t k = idx(first, j);
                const __m128 dwdx = _mm_div_ps(_mm_sub_ps(_mm_loadu_ps(cw_.data() + k + 1), _mm_loadu_ps(cw_.data() + k - 1)), dx);
                const __m128 dudz = _mm_div_ps(_mm_sub_ps(_mm_loadu_ps(cu_.data() + idx(first, jr)), _mm_loadu_ps(cu_.data() + idx(first, jl))), dz);
                _mm_storeu_ps(omega_.data() + k, _mm_and_ps(fluidMaskFour(solid_.data() + k), _mm_sub_ps(dwdx, dudz)));
            }
        }
#endif
        for (int i = first; i < n_; ++i) omega_[idx(i, j)] = solid_[idx(i, j)] ? 0.0f : vorticity(i, j);
    }
    // f = eps h (N x omega); com omega = dw/dx - du/dz isso dá (fx, fz) = eps h omega (nz, -nx).
    const float k = params_.vorticity * h_;
    for (int j = 0; j < n_; ++j) {
        int first = 0;
#if defined(_M_X64) || defined(__SSE2__)
        if (!forceScalar_) {
            const int jl = std::max(j - 1, 0), jr = std::min(j + 1, n_ - 1);
            const __m128 dx = _mm_set1_ps(2.0f * h_);
            const __m128 dz = _mm_set1_ps(static_cast<float>(jr - jl) * h_);
            const __m128 sign = _mm_set1_ps(-0.0f), eps = _mm_set1_ps(1e-8f);
            first = 1;
            for (; first + 4 <= n_ - 1; first += 4) {
                const size_t q = idx(first, j);
                const __m128 ex = _mm_div_ps(_mm_sub_ps(_mm_andnot_ps(sign, _mm_loadu_ps(omega_.data() + q + 1)),
                                                       _mm_andnot_ps(sign, _mm_loadu_ps(omega_.data() + q - 1))), dx);
                const __m128 ez = _mm_div_ps(_mm_sub_ps(_mm_andnot_ps(sign, _mm_loadu_ps(omega_.data() + idx(first, jr))),
                                                       _mm_andnot_ps(sign, _mm_loadu_ps(omega_.data() + idx(first, jl)))), dz);
                const __m128 len = _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(ex, ex), _mm_mul_ps(ez, ez)));
                const __m128 active = _mm_and_ps(_mm_cmpge_ps(len, eps), fluidMaskFour(solid_.data() + q));
                const __m128 divisor = _mm_max_ps(len, eps);
                const __m128 spin = _mm_loadu_ps(omega_.data() + q);
                const __m128 fx = _mm_mul_ps(_mm_mul_ps(_mm_set1_ps(k), spin), _mm_div_ps(ez, divisor));
                const __m128 fz = _mm_mul_ps(_mm_mul_ps(_mm_set1_ps(-k), spin), _mm_div_ps(ex, divisor));
                _mm_storeu_ps(scratch_.data() + q, _mm_and_ps(active, fx));
                _mm_storeu_ps(scratch2_.data() + q, _mm_and_ps(active, fz));
            }
        }
#endif
        const int skipped = first > 0 ? 1 : 0;
        for (int tail = -skipped; tail < n_ - first; ++tail) {
            const int i = tail < 0 ? 0 : first + tail;
            scratch_[idx(i, j)] = scratch2_[idx(i, j)] = 0.0f;
            if (solid_[idx(i, j)]) continue;
            const int il = std::max(i - 1, 0), ir = std::min(i + 1, n_ - 1);
            const int jl = std::max(j - 1, 0), jr = std::min(j + 1, n_ - 1);
            const float ex = (std::abs(omega_[idx(ir, j)]) - std::abs(omega_[idx(il, j)])) / (static_cast<float>(ir - il) * h_);
            const float ez = (std::abs(omega_[idx(i, jr)]) - std::abs(omega_[idx(i, jl)])) / (static_cast<float>(jr - jl) * h_);
            const float len = std::sqrt(ex * ex + ez * ez);
            if (len < 1e-8f) continue;
            const float nx = ex / len, nz = ez / len;
            scratch_[idx(i, j)] = k * omega_[idx(i, j)] * nz;
            scratch2_[idx(i, j)] = -k * omega_[idx(i, j)] * nx;
        }
    }
    for (int j = 0; j < n_; ++j)
        for (int i = 1; i < n_; ++i)
            u_[static_cast<size_t>(j) * (n_ + 1) + i] += dt * 0.5f * (scratch_[idx(i - 1, j)] + scratch_[idx(i, j)]);
    for (int j = 1; j < n_; ++j)
        for (int i = 0; i < n_; ++i)
            w_[static_cast<size_t>(j) * n_ + i] += dt * 0.5f * (scratch2_[idx(i, j - 1)] + scratch2_[idx(i, j)]);
    enforceBoundary();
}

void StableFluid::diffuse(float dt) {
    if (params_.viscosity <= 0.0f) return;
    const float a = params_.viscosity * dt / (h_ * h_);
    auto relaxField = [&](std::vector<float>& f, std::vector<float>& b, std::vector<float>& tmp, int W, int H) {
        std::copy(f.begin(), f.end(), b.begin());
        for (int it = 0; it < params_.viscosityIterations; ++it) {
            for (int j = 0; j < H; ++j) {
                for (int i = 0; i < W; ++i) {
                    const int il = std::max(i - 1, 0), ir = std::min(i + 1, W - 1);
                    const int jl = std::max(j - 1, 0), jr = std::min(j + 1, H - 1);
                    const float sum = f[static_cast<size_t>(j) * W + il] + f[static_cast<size_t>(j) * W + ir] +
                                      f[static_cast<size_t>(jl) * W + i] + f[static_cast<size_t>(jr) * W + i];
                    tmp[static_cast<size_t>(j) * W + i] = (b[static_cast<size_t>(j) * W + i] + a * sum) / (1.0f + 4.0f * a);
                }
            }
            std::copy(tmp.begin(), tmp.end(), f.begin());
        }
    };
    relaxField(u_, hatU_, un_, n_ + 1, n_);
    relaxField(w_, hatW_, wn_, n_, n_ + 1);
    enforceBoundary();
}

namespace {
// Coeficientes do SOR por código de vizinhança (bits L,R,D,U): peso de cada vizinho, 1/nf e omega (0 = célula parada).
struct SorCoef {
    float l, r, d, u, inv, om;
};
}  // namespace

void StableFluid::project() {
    const int n = n_;
    if (topoDirty_) rebuildTopology();
    const size_t un = static_cast<size_t>(n) + 1;
    const float invh = 1.0f / h_;

    // Divergência antes + rhs = h * (du + dw): a equação discreta é sum(q_viz - q) = rhs nas faces abertas.
    double sumB = 0.0;
    float maxB = 0.0f;
    int count = 0;
    for (int j = 0; j < n; ++j) {
        const float* ur = u_.data() + static_cast<size_t>(j) * un;
        const float* w0 = w_.data() + static_cast<size_t>(j) * n;
        const float* w1 = w0 + n;
        const unsigned char* sol = solid_.data() + static_cast<size_t>(j) * n;
        float* rhs = scratch_.data() + static_cast<size_t>(j) * n;
        float rowSum = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float d = ur[i + 1] - ur[i] + w1[i] - w0[i];
            const bool fluid = !sol[i];
            rhs[i] = fluid ? h_ * d : 0.0f;
            const float ad = fluid ? std::abs(d) * invh : 0.0f;
            rowSum += ad;
            maxB = std::max(maxB, ad);
            count += fluid;
        }
        sumB += rowSum;
    }
    stats_.divBefore = count ? static_cast<float>(sumB / count) : 0.0f;
    stats_.maxBefore = maxB;

    SorCoef tab[16];
    for (int b = 0; b < 16; ++b) {
        const int nf = (b & 1) + ((b >> 1) & 1) + ((b >> 2) & 1) + ((b >> 3) & 1);
        tab[b] = {static_cast<float>(b & 1), static_cast<float>((b >> 1) & 1), static_cast<float>((b >> 2) & 1),
                  static_cast<float>((b >> 3) & 1), nf ? 1.0f / static_cast<float>(nf) : 0.0f, nf ? params_.omega : 0.0f};
    }
    const size_t cells = static_cast<size_t>(n) * n;
    float* p = pad_.data() + n;  // folga de n antes e depois: vizinho fora da janela tem peso 0 e nunca é lido de fora do vetor
    std::fill(pad_.begin(), pad_.begin() + n, 0.0f);
    std::fill(pad_.end() - n, pad_.end(), 0.0f);
    if (params_.warmStart) std::copy(press_.begin(), press_.end(), p);
    else std::fill(p, p + cells, 0.0f);

    // A mesma ordem red-black e os mesmos coeficientes; separar as cores permite quatro
    // atualizacoes SSE2 contiguas. Solidos e tamanhos impares mantem o stencil geral.
#if defined(_M_X64) || defined(__SSE2__)
    if (!forceScalar_ && (n & 1) == 0) {
        const int half = n / 2;
        const size_t plane = static_cast<size_t>(n + 2) * half;
        float* colors[2] = {packedPress_.data() + half + 1, packedPress_.data() + plane + half + 1};
        // Compacta o RHS junto da pressão para que as varreduras leiam quatro valores contíguos.
        const size_t cellPlane = static_cast<size_t>(n) * half;
        float* packedRhs = scratch2_.data();  // o confinamento já consumiu esse scratch
        for (int color = 0; color < 2; ++color) {
            for (int j = 0; j < n; ++j) {
                const int parity = (j + color) & 1;
                for (int k = 0; k < half; ++k) {
                    const int i = 2 * k + parity;
                    colors[color][j * half + k] = p[j * n + i];
                    packedRhs[color * cellPlane + j * half + k] = scratch_[j * n + i];
                }
            }
        }
        const __m128 om = _mm_set1_ps(params_.omega);
        const __m128 quarter = _mm_set1_ps(0.25f);
        for (int it = 0; it < params_.iterations; ++it) {
            for (int color = 0; color < 2; ++color) {
                for (int j = 0; j < n; ++j) {
                    float* out = colors[color] + j * half;
                    const float* other = colors[1 - color] + j * half;
                    const int parity = (j + color) & 1;
                    auto edge = [&](int k) {
                        const int i = 2 * k + parity;
                        const SorCoef& t = tab[nbr_[static_cast<size_t>(j) * n + i]];
                        const float sum = t.l * other[k + parity - 1] + t.r * other[k + parity] +
                                          t.d * other[k - half] + t.u * other[k + half];
                        const float target = (sum - scratch_[static_cast<size_t>(j) * n + i]) * t.inv;
                        out[k] += t.om * (target - out[k]);
                    };
                    if (j == 0 || j == n - 1) {
                        for (int k = 0; k < half; ++k) edge(k);
                        continue;
                    }
                    edge(0);
                    int k = 1;
                    const size_t base = static_cast<size_t>(j) * half;
                    const float* rhs = packedRhs + static_cast<size_t>(color) * cellPlane + base;
                    const std::uint32_t* mask = sorMask_.data() + static_cast<size_t>(color) * 4 * cellPlane + base;
                    const float* invs = sorInv_.data() + static_cast<size_t>(color) * cellPlane + base;
                    for (; k + 4 <= half - 1; k += 4) {
                        __m128 left = _mm_loadu_ps(other + k + parity - 1);
                        __m128 right = _mm_loadu_ps(other + k + parity);
                        __m128 down = _mm_loadu_ps(other + k - half);
                        __m128 up = _mm_loadu_ps(other + k + half);
                        const __m128 inv = allFluid_ ? quarter : _mm_loadu_ps(invs + k);
                        __m128 weight = om;
                        // 1/4 nas quatro lanes garante quatro vizinhos abertos em cada célula.
                        if (!allFluid_ && _mm_movemask_ps(_mm_cmpeq_ps(inv, quarter)) != 0xF) {
                            const auto m = [&](int c) {
                                return _mm_castsi128_ps(_mm_loadu_si128(
                                    reinterpret_cast<const __m128i*>(mask + c * cellPlane + k)));
                            };
                            left = _mm_and_ps(m(0), left);
                            right = _mm_and_ps(m(1), right);
                            down = _mm_and_ps(m(2), down);
                            up = _mm_and_ps(m(3), up);
                            weight = _mm_and_ps(_mm_cmpneq_ps(inv, _mm_setzero_ps()), om);
                        }
                        __m128 sum = _mm_add_ps(left, right);
                        sum = _mm_add_ps(sum, down);
                        sum = _mm_add_ps(sum, up);
                        const __m128 target = _mm_mul_ps(_mm_sub_ps(sum, _mm_loadu_ps(rhs + k)), inv);
                        const __m128 prev = _mm_loadu_ps(out + k);
                        _mm_storeu_ps(out + k, _mm_add_ps(prev, _mm_mul_ps(weight, _mm_sub_ps(target, prev))));
                    }
                    for (; k < half; ++k) edge(k);
                }
            }
        }
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                p[j * n + i] = colors[(i + j) & 1][j * half + i / 2];
    } else
#endif
    {
        for (int it = 0; it < params_.iterations; ++it) {
            for (int color = 0; color < 2; ++color) {
                for (int j = 0; j < n; ++j) {
                    float* pr = p + static_cast<size_t>(j) * n;
                    const float* rhs = scratch_.data() + static_cast<size_t>(j) * n;
                    const unsigned char* code = nbr_.data() + static_cast<size_t>(j) * n;
                    for (int i = (j + color) & 1; i < n; i += 2) {
                        const SorCoef& t = tab[code[i]];
                        const float sum = t.l * pr[i - 1] + t.r * pr[i + 1] + t.d * pr[i - n] + t.u * pr[i + n];
                        const float target = (sum - rhs[i]) * t.inv;
                        pr[i] += t.om * (target - pr[i]);
                    }
                }
            }
        }
    }
    std::copy(p, p + cells, press_.begin());

    for (int j = 0; j < n; ++j) {
        float* ur = u_.data() + static_cast<size_t>(j) * un;
        const unsigned char* wall = wallU_.data() + static_cast<size_t>(j) * un;
        const float* pr = press_.data() + static_cast<size_t>(j) * n;
        for (int i = 1; i < n; ++i)
            if (!wall[i]) ur[i] -= (pr[i] - pr[i - 1]) * invh;
    }
    for (int j = 1; j < n; ++j) {
        float* wr = w_.data() + static_cast<size_t>(j) * n;
        const unsigned char* wall = wallW_.data() + static_cast<size_t>(j) * n;
        const float* pr = press_.data() + static_cast<size_t>(j) * n;
        for (int i = 0; i < n; ++i)
            if (!wall[i]) wr[i] -= (pr[i] - pr[i - n]) * invh;
    }

    double sumA = 0.0;
    float maxA = 0.0f;
    for (int j = 0; j < n; ++j) {
        const float* ur = u_.data() + static_cast<size_t>(j) * un;
        const float* w0 = w_.data() + static_cast<size_t>(j) * n;
        const float* w1 = w0 + n;
        const unsigned char* sol = solid_.data() + static_cast<size_t>(j) * n;
        float rowSum = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float ad = sol[i] ? 0.0f : std::abs(ur[i + 1] - ur[i] + w1[i] - w0[i]) * invh;
            rowSum += ad;
            maxA = std::max(maxA, ad);
        }
        sumA += rowSum;
    }
    stats_.divAfter = count ? static_cast<float>(sumA / count) : 0.0f;
    stats_.maxAfter = maxA;
}

// Semi-Lagrangiana (rastro de Euler) + MacCormack limitado (Selle 2008). Num ponto de amostra p:
// hat = SL(src) em p - dt v; corrigido = hat + 0.5 (src - SL^-1(hat)), preso ao [min, max] dos cantos do rastro.
void StableFluid::advectField(const float* src, float* dst, float* hat, int w, int h, float offX, float offZ,
                              const unsigned char* mask, float dt) {
    const float scale = dt / h_;
    float* fx = backX_.data();
    float* fz = backZ_.data();
    float* lo = loBuf_.data();
    float* hi = hiBuf_.data();
    // Especializa fora dos lacos: a grade MAC e a mascara nao mudam por celula.
    auto advect = [&]<AdvectGrid Grid, bool Masked>() {
        const bool simdOk = Masked ? !forceScalar_ : true;
        auto advectOne = [&](int i, int j) {
            const int k = j * w + i;
            // Velocidade na amostra: no centro é cu/cw direto; numa face u (ou w) o bilinear de cu/cw cai exatamente
            // na média dos dois centros vizinhos ao longo do eixo da face (a outra coordenada é inteira).
            int ka = k, kb = k;
            if constexpr (Grid != AdvectGrid::Centers) {
                if constexpr (Grid == AdvectGrid::FaceU) {
                    ka = j * n_ + std::max(i - 1, 0);
                    kb = j * n_ + std::min(i, n_ - 1);
                } else {
                    ka = std::max(j - 1, 0) * n_ + i;
                    kb = std::min(j, n_ - 1) * n_ + i;
                }
            }
            const float vx = 0.5f * (cu_[ka] + cu_[kb]) * scale;
            const float vz = 0.5f * (cw_[ka] + cw_[kb]) * scale;
            fx[k] = static_cast<float>(i) + vx;  // destino do rastro para a frente (em coordenadas do campo)
            fz[k] = static_cast<float>(j) + vz;
            const float gx = static_cast<float>(i) - vx, gz = static_cast<float>(j) - vz;
            const Taps t = makeTaps(w, h, gx, gz);
            if constexpr (Masked) {
                if (mask[t.k00] | mask[t.k10] | mask[t.k01] | mask[t.k11]) {
                    hat[k] = bilerp(src, w, h, gx, gz, mask, src[k], &lo[k], &hi[k]);
                    return;
                }
            }
            const float a = src[t.k00], b = src[t.k10], c = src[t.k01], d = src[t.k11];
            lo[k] = std::min(std::min(a, b), std::min(c, d));
            hi[k] = std::max(std::max(a, b), std::max(c, d));
            hat[k] = lerpTaps(src, t);
        };
        for (int j = 0; j < h; ++j) {
            int i = 0;
#if defined(_M_X64) || defined(__SSE2__)
            // Masked (densidade com solidos): lotes cujo bilinear toca um solido refazem as 4 celulas no caminho escalar.
            if (simdOk) {
                // Faces de borda usam a media escalar dos centros.
                if constexpr (Grid == AdvectGrid::FaceU) { advectOne(0, j); i = 1; }
                for (; i + 4 <= w - (Grid == AdvectGrid::FaceU ? 1 : 0); i += 4) {
                    const int k = j * w + i;
                    int ka = k, kb = k;
                    if constexpr (Grid == AdvectGrid::FaceU) { ka = j * n_ + i - 1; kb = ka + 1; }
                    if constexpr (Grid == AdvectGrid::FaceW) { ka = std::max(j - 1, 0) * n_ + i; kb = std::min(j, n_ - 1) * n_ + i; }
                    const __m128 half = _mm_set1_ps(0.5f), sc = _mm_set1_ps(scale);
                    const __m128 vx = _mm_mul_ps(_mm_mul_ps(_mm_add_ps(_mm_loadu_ps(cu_.data() + ka), _mm_loadu_ps(cu_.data() + kb)), half), sc);
                    const __m128 vz = _mm_mul_ps(_mm_mul_ps(_mm_add_ps(_mm_loadu_ps(cw_.data() + ka), _mm_loadu_ps(cw_.data() + kb)), half), sc);
                    const __m128 x = _mm_add_ps(_mm_set1_ps(static_cast<float>(i)), _mm_setr_ps(0, 1, 2, 3));
                    const __m128 z = _mm_set1_ps(static_cast<float>(j));
                    _mm_storeu_ps(fx + k, _mm_add_ps(x, vx));
                    _mm_storeu_ps(fz + k, _mm_add_ps(z, vz));
                    int near = 0;
                    _mm_storeu_ps(hat + k, sampleFour(src, w, h, _mm_sub_ps(x, vx), _mm_sub_ps(z, vz), lo + k, hi + k,
                                                       Masked ? nearSolid_.data() : nullptr, &near));
                    if constexpr (Masked) {
                        if (near)
                            for (int c = 0; c < 4; ++c) advectOne(i + c, j);
                    }
                }
            }
#endif
            for (; i < w; ++i) advectOne(i, j);
        }
        const size_t total = static_cast<size_t>(w) * h;
        if (!params_.macCormack) {
            std::copy(hat, hat + static_cast<std::ptrdiff_t>(total), dst);
            return;
        }
        size_t first = 0;
#if defined(_M_X64) || defined(__SSE2__)
        if (simdOk) {
            const __m128 half = _mm_set1_ps(0.5f);
            for (; first + 4 <= total; first += 4) {
                int near = 0;
                const __m128 back = sampleFour(hat, w, h, _mm_loadu_ps(fx + first), _mm_loadu_ps(fz + first), nullptr, nullptr,
                                               Masked ? nearSolid_.data() : nullptr, &near);
                const __m128 value = _mm_add_ps(_mm_loadu_ps(hat + first), _mm_mul_ps(half, _mm_sub_ps(_mm_loadu_ps(src + first), back)));
                _mm_storeu_ps(dst + first, _mm_min_ps(_mm_max_ps(value, _mm_loadu_ps(lo + first)), _mm_loadu_ps(hi + first)));
                if constexpr (Masked) {
                    if (near) {
                        for (size_t k = first; k < first + 4; ++k) {
                            const Taps t = makeTaps(w, h, fx[k], fz[k]);
                            const bool nearSolid = (mask[t.k00] | mask[t.k10] | mask[t.k01] | mask[t.k11]) != 0;
                            const float b = nearSolid ? bilerp(hat, w, h, fx[k], fz[k], mask, hat[k]) : lerpTaps(hat, t);
                            dst[k] = std::clamp(hat[k] + 0.5f * (src[k] - b), lo[k], hi[k]);
                        }
                    }
                }
            }
        }
#endif
        for (size_t k = first; k < total; ++k) {
            const Taps t = makeTaps(w, h, fx[k], fz[k]);
            const bool nearSolid = Masked && (mask[t.k00] | mask[t.k10] | mask[t.k01] | mask[t.k11]);
            const float back = nearSolid ? bilerp(hat, w, h, fx[k], fz[k], mask, hat[k]) : lerpTaps(hat, t);
            dst[k] = std::clamp(hat[k] + 0.5f * (src[k] - back), lo[k], hi[k]);
        }
    };
    if (offX == 0.0f) { advect.template operator()<AdvectGrid::FaceU, false>(); return; }
    if (offZ == 0.0f) { advect.template operator()<AdvectGrid::FaceW, false>(); return; }
    if (allFluid_) { advect.template operator()<AdvectGrid::Centers, false>(); return; }
    advect.template operator()<AdvectGrid::Centers, true>();
}

void StableFluid::advectVelocity(float dt) {
    centerVelocity();
    advectField(u_.data(), un_.data(), hatU_.data(), n_ + 1, n_, 0.0f, 0.5f, nullptr, dt);
    advectField(w_.data(), wn_.data(), hatW_.data(), n_, n_ + 1, 0.5f, 0.0f, nullptr, dt);
    u_.swap(un_);
    w_.swap(wn_);
    enforceBoundary();
}

void StableFluid::advectDensity(float dt) {
    centerVelocity();
    advectField(rho_.data(), rhoN_.data(), hatR_.data(), n_, n_, 0.5f, 0.5f, solid_.data(), dt);
    for (size_t k = 0; k < rhoN_.size(); ++k)
        if (solid_[k]) rhoN_[k] = 0.0f;
    rho_.swap(rhoN_);
}
void StableFluid::step(float dt) {
    if (!(dt > 0.0f)) return;
#if defined(_M_X64) || defined(__SSE2__)
    const ScopedFlushDenormals flushDenormals;
#endif
    time_ += dt;
    applyForces(dt);
    advectVelocity(dt);
    vorticityConfinement(dt);
    diffuse(dt);
    project();
    advectDensity(dt);
    const float rec = 1.0f - std::exp(-params_.recovery * dt);
    if (rec > 0.0f) {
        for (size_t k = 0; k < rho_.size(); ++k)
            if (!solid_[k]) rho_[k] += (rest_[k] - rho_[k]) * rec;
    }
    const float damp = std::exp(-params_.damping * dt);
    const float vmax = params_.maxSpeed;
    for (float& v : u_) v = std::clamp(v * damp, -vmax, vmax);
    for (float& v : w_) v = std::clamp(v * damp, -vmax, vmax);
    enforceBoundary();
    centerVelocity();
}

float StableFluid::sample(float wx, float wz) const {
    return bilerp(rho_.data(), n_, n_, (wx - x0_) / h_ - 0.5f, (wz - z0_) / h_ - 0.5f, solid_.data(), 0.0f);
}

Vec2 StableFluid::sampleVelocity(float wx, float wz) const {
    const float gx = (wx - x0_) / h_ - 0.5f, gz = (wz - z0_) / h_ - 0.5f;
    return {bilerp(cu_.data(), n_, n_, gx, gz), bilerp(cw_.data(), n_, n_, gx, gz)};
}

double StableFluid::totalMass() const {
    double sum = 0.0;
    for (size_t k = 0; k < rho_.size(); ++k)
        if (!solid_[k]) sum += rho_[k];
    return sum * static_cast<double>(h_) * h_;
}

}  // namespace fog
