// Stable Fluids 2D para a névoa de chão (Stam 1999/GDC 2003, GPU Gems 38), independente do fog::Field.
// Grade MAC (staggered): velocidade nas faces, densidade e pressão no centro da célula. A projeção é exata até a
// tolerância do solver (a divergência discreta de cada célula vai a ~0), sem o modo xadrez da grade colocada.
// Eixos: x e z do mundo (y é a altura e não entra). A janela segue o Link em passos inteiros de célula.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace fog {

struct Vec2 {
    float x = 0.0f;
    float z = 0.0f;
};

struct StableParams {
    int iterations = 20;          // iterações red-black Gauss-Seidel (SOR) da pressão
    float omega = 1.9f;           // sobre-relaxação do SOR (1 = Gauss-Seidel puro)
    bool warmStart = true;        // reaproveita a pressão do tick anterior
    bool macCormack = true;       // MacCormack limitado (Selle 2008); false = semi-Lagrangiana pura
    float vorticity = 3.0f;       // ε do confinamento de vorticidade (Fedkiw 2001), em 1/s
    float viscosity = 0.0f;       // u²/s; 0 desliga
    int viscosityIterations = 4;  // Jacobi da difusão implícita
    float damping = 0.6f;         // amortecimento da velocidade, 1/s
    float recovery = 0.15f;       // relaxamento da densidade ao repouso, 1/s
    float maxSpeed = 400.0f;      // teto da velocidade por face, u/s
    float windX = 0.0f, windZ = 0.0f;  // vento ambiente, u/s
    float windRelax = 0.5f;       // 1/s com que a velocidade tende ao vento
    float curlAmp = 0.0f;         // curl-noise (Bridson 2007), aceleração em u/s²; 0 desliga
    float curlScale = 400.0f;     // comprimento de onda do ruído, u
    float curlSpeed = 0.05f;      // velocidade com que o potencial anda no tempo (ciclos/s)
};

struct StableProjectionStats {
    float divBefore = 0.0f;  // média de |div| nas células de fluido antes da projeção, 1/s
    float divAfter = 0.0f;   // idem depois
    float maxBefore = 0.0f;
    float maxAfter = 0.0f;
};

class StableFluid {
public:
    using SolidSampler = std::function<bool(float wx, float wz)>;
    using RestSampler = std::function<float(float wx, float wz)>;

    // Aloca tudo aqui (nenhuma alocação em step). n >= 8. Densidade = repouso, velocidade = 0.
    void reset(int n, float cellSize, float originX, float originZ);

    // So o curl-noise depende de curlAmp/curlScale/curlSpeed: vento, vorticidade etc. nao invalidam o potencial.
    void setParams(const StableParams& p) {
        if (p.curlAmp != params_.curlAmp || p.curlScale != params_.curlScale || p.curlSpeed != params_.curlSpeed)
            curlValid_ = false;
        params_ = p;
    }
    const StableParams& params() const { return params_; }

    // Callbacks chamados no reset/recenter para as células novas; ao instalar, reavaliam a janela inteira.
    void setSolidSampler(SolidSampler fn);
    void setRestSampler(RestSampler fn);
    void setSolid(int i, int j, bool solid);
    void setRest(int i, int j, float rho);
    void setDensity(int i, int j, float rho);

    // Janela segue (x, z) em passos inteiros de célula; devolve o deslocamento (células) aplicado em x e z.
    void recenter(float x, float z, int* shiftX = nullptr, int* shiftZ = nullptr);

    // Stir: só força (velocidade). Nunca altera a densidade.
    // Splat gaussiano (sigma = radius/2) em torno do segmento a-b; dv = strength * g * velocity.
    void stirSegment(Vec2 a, Vec2 b, Vec2 velocity, float radius, float strength);
    // Impulso radial (pouso): dv = speed * g * direção radial; g gaussiana de sigma = radius/2 a partir do centro.
    void stirRadial(Vec2 center, float speed, float radius);

    void step(float dt);

    int size() const { return n_; }
    float cellSize() const { return h_; }
    float originX() const { return x0_; }
    float originZ() const { return z0_; }
    float cellCenterX(int i) const { return x0_ + (static_cast<float>(i) + 0.5f) * h_; }
    float cellCenterZ(int j) const { return z0_ + (static_cast<float>(j) + 0.5f) * h_; }

    float density(int i, int j) const { return rho_[idx(i, j)]; }
    float velocityX(int i, int j) const { return cu_[idx(i, j)]; }  // no centro da célula
    float velocityZ(int i, int j) const { return cw_[idx(i, j)]; }
    float vorticity(int i, int j) const;  // dw/dx - du/dz no centro, 1/s
    bool solid(int i, int j) const { return solid_[idx(i, j)] != 0; }
    float rest(int i, int j) const { return rest_[idx(i, j)]; }
    float faceU(int i, int j) const { return u_[static_cast<size_t>(j) * (n_ + 1) + i]; }  // i em 0..n
    float faceW(int i, int j) const { return w_[static_cast<size_t>(j) * n_ + i]; }        // j em 0..n

    const float* densityData() const { return rho_.data(); }  // n*n, linha j, coluna i

    float sample(float wx, float wz) const;  // densidade bilinear
    Vec2 sampleVelocity(float wx, float wz) const;

    const StableProjectionStats& stats() const { return stats_; }
    double totalMass() const;  // soma da densidade nas células de fluido (x h²)

private:
    friend struct StableFluidTestAccess;
    size_t idx(int i, int j) const { return static_cast<size_t>(j) * n_ + i; }
    void enforceBoundary();
    void centerVelocity();
    void applyForces(float dt);
    void vorticityConfinement(float dt);
    void diffuse(float dt);
    void project();
    void advectField(const float* src, float* dst, float* hat, int w, int h, float offX, float offZ,
                     const unsigned char* mask, float dt);
    void advectVelocity(float dt);
    void advectDensity(float dt);
    void refreshCurl(float dt);
    void buildSolidCaches();
    void rebuildTopology();  // paredes de face e códigos de vizinhança a partir de solid_

    int n_ = 0;
    float h_ = 1.0f;
    float x0_ = 0.0f, z0_ = 0.0f;
    float time_ = 0.0f;
    StableParams params_;
    StableProjectionStats stats_;
    SolidSampler solidFn_;
    RestSampler restFn_;

    std::vector<float> u_, w_, un_, wn_, hatU_, hatW_;       // faces
    std::vector<float> fu_, fw_;                              // forças acumuladas
    std::vector<float> rho_, rhoN_, hatR_, rest_, press_;     // células
    std::vector<float> cu_, cw_, omega_, scratch_, scratch2_; // velocidade/vorticidade no centro
    std::vector<float> backX_, backZ_, loBuf_, hiBuf_, shiftTmp_;             // rastros e buffer do recenter
    std::vector<unsigned char> solid_, solidTmp_;
    std::vector<unsigned char> wallU_, wallW_, nbr_;  // paredes por face e vizinhos abertos por célula (bits L,R,D,U)
    // Com solidos: mascaras de vizinho (0/~0) e coeficientes do SOR por celula, empacotados por cor (n*n/2 por plano).
    std::vector<std::uint32_t> sorMask_;  // [cor][L,R,D,U][n*n/2]
    std::vector<float> sorInv_;           // [cor][1/nf][n*n/2]; 0 = celula parada
    std::vector<unsigned char> nearSolid_;  // algum dos 4 cantos do bilinear a partir da celula e solido
    bool forceScalar_ = false;  // so testes: desliga os caminhos SSE para comparar com a referencia escalar
    std::vector<float> pad_;                            // pressão com folga de n nas pontas (SOR sem borda)
    std::vector<float> packedPress_;  // cores contiguas do SOR, com linhas de guarda
    bool allFluid_ = true;
    bool topoDirty_ = true;
    std::vector<float> curlU_, curlW_, psi_, psiNext_;
    bool curlValid_ = false;
    float curlTime_ = 0.0f;
    float curlBuildTime_ = 0.0f;
    int curlNextRow_ = 0;
    float curlOriginX_ = 0.0f, curlOriginZ_ = 0.0f;
    bool dirtyForce_ = false;
};

}  // namespace fog
