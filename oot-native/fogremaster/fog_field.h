#pragma once

#include <array>
#include <cstdint>

namespace fog {

struct Point {
    float x, y, z;
};

inline constexpr int Side = 25;
inline constexpr float Spacing = 32.0f;
inline constexpr float LayerHeight = 54.0f;
inline constexpr float StepSeconds = 0.05f;
inline constexpr float SpeedLimit = 180.0f;
inline constexpr float RestDensity = 0.60f;
// Teto da densidade na simulação (14/11). Com 1,0 a borda do golpe num banco cheio (repouso 0,9) não tinha folga e
// a névoa empurrada além do teto sumia. O host aceita até 1,5 (o limite do shader) só com a grade fina da RFC 0034;
// nos outros envios o provider corta em 1,0.
inline constexpr float MaxDensity = 1.5f;
inline constexpr float WireDensity = 1.0f;
inline constexpr float VelocityDamping = 1.35f;
inline constexpr float Recovery = 0.30f;
inline constexpr int AttackTickMilliseconds = 50;
inline constexpr int MinAttackTicks = 3;
inline constexpr int DefaultAttackTicks = 9;
inline constexpr int MaxAttackTicks = 20;
// Espessura visível da camada entregue ao host (0.28.0). A simulação mantém LayerHeight para a topologia dos
// patamares; a névoa desenhada fica rasteira (canela do Link criança) e mais densa, com a mesma coluna da 0.27.
// 0.29.0: a referência do mantenedor é névoa de chão densa e branca, em bancos, que esconde o piso onde está
// grossa e deixa vãos limpos entre eles. A 0.28 tinha ~0,2 de profundidade óptica vista de cima (um véu); agora um
// banco cheio passa de 2.
inline constexpr float RenderLayerHeight = 16.0f;
inline constexpr float RenderFalloff = 5.0f;
inline constexpr float RenderExtinction = 0.25f;
// Bancos: a densidade de repouso varia no chão entre BankLow e BankHigh vezes RestDensity, num padrão preso ao
// mundo que deriva com o vento. A pressão da camada rasa age sobre o desvio em relação a esse repouso, então os
// bancos ficam parados em equilíbrio e o golpe continua empurrando névoa de verdade de um lugar para outro.
inline constexpr float BankLow = 0.08f;
inline constexpr float BankHigh = 1.5f;
inline constexpr float BankWavelength = 160.0f; // u
inline constexpr float BankDrift = 0.35f;       // fração do vento com que o padrão anda
// Camada rasa (0.28.0): a névoa é uma lâmina mais densa que o ar sobre o chão. Onde ela fica mais grossa, o
// peso empurra para onde está mais fina (pressão hidrostática reduzida), então o vão aberto por um golpe se
// fecha escorrendo dos lados, em ondas a ShallowWaveSpeed. A parte giratória do empurrão não tem pressão que a
// desfaça: é ela que sobra como vórtice depois que a onda passa.
inline constexpr float ShallowWaveSpeed = 45.0f; // u/s; a 90 o buraco do golpe fechava em ~1,5 s, a 45 dura ~3 s
inline constexpr float ShallowGravity = ShallowWaveSpeed * ShallowWaveSpeed / RestDensity;
// Golpe na vila (14/11): com amortecimento 0,45 e sopro 1,8 o giro aos 2 s ficava em ~5 u/s no jogo e não
// aparecia. Varredura (scratch fog_sweep): amortecimento 0,25 + sopro 2,8 + divergente 0,25 leva o giro do teste
// de 15 para 24 u/s aos 2 s e a névoa ainda volta ao padrão dos bancos em 16 s. Confinamento acima de 3 deixa o
// campo caótico (o vórtice se sustenta sozinho e os bancos não assentam): não subir sem refazer a varredura.
inline constexpr float ShallowDamping = 0.25f;     // 1/s: o vórtice do golpe dura ~3 s
inline constexpr float ShallowRecovery = 0.30f;    // 1/s: na grade de 80 u o vão de ~3 células volta por aqui
inline constexpr float ShallowConfinement = 3.0f;  // confinamento de vorticidade (o legado usa 3,8)
// Amortecimento só da divergência (por tick, em unidades da grade): a onda que o empurrão solta perde força sem
// frear o giro. Em unidade física (32/Δx)² a vila (80 u) abre um buraco maior e o giro some, então fica por tick.
// Com amortecimento 0,25 segura a onda entre os bancos; a 80 u o vão aos 12 s fica em 0,546 (0,540 com 0,20).
inline constexpr float ShallowDivergenceDamping = 0.25f;
// O menor vórtice que a grade carrega tem umas três células; o ar arrastado pelo golpe alcança pelo menos
// ShallowBladeCells células da lâmina, senão o giro cabe numa célula só e some na primeira passada.
// A espada Kokiri tem ~30 u de lâmina e o golpe registra só 2 ticks (~130° de uma vez; trajetória gravada em jogo
// em 14/11): com 2,2 células o arrasto quase não cabia na grade de 40 u. Com 3,4 o giro desse golpe aos 2 s vai
// de ~11 para ~19 u/s na varredura, e os testes de vão e de bancos continuam passando de 3,2 a 3,6.
inline constexpr float ShallowBladeCells = 3.4f;
// Viscosidade por tick (estêncil compacto): some com o xadrez de uma célula, quase não toca no vórtice de três.
inline constexpr float ShallowViscosity = 0.03f;
// Sopro do golpe na camada rasa: ganho sobre o arrasto da lâmina e teto por tick (o legado usa 38 u/s).
inline constexpr float ShallowBladeGain = 2.8f;
inline constexpr float ShallowAttackCap = 60.0f;
inline constexpr float LegacyAttackCap = 38.0f;
// Afastamento lateral da lâmina (u/s por u de distância): o legado usa 1,8, que domina o sopro e vira ruído.
inline constexpr float ShallowBladeRadial = 1.2f;
// Pouso (0.31.0): abaixo de LandMinSpeed (u/s) é passo ou degrau e o corpo já afasta a névoa; o empurrão para fora
// é LandOutward vezes a velocidade da batida, até o teto do golpe, num disco de 2 a 4 células.
inline constexpr float LandMinSpeed = 90.0f;
inline constexpr float LandOutward = 1.0f;

enum Edge : uint8_t { NegativeX = 1u, PositiveX = 2u, NegativeZ = 4u, PositiveZ = 8u };

struct Cell {
    float floor{};
    bool open{};
    float density{};
    float vx{};
    float vz{};
    uint8_t blocked{};
    float raisedDensity{};
    float verticalSpeed{};
};

struct Impulse {
    float densityScale{1.0f};
    float vx{}, vz{};
    float minX{-SpeedLimit}, maxX{SpeedLimit}, minZ{-SpeedLimit}, maxZ{SpeedLimit};
};

// Legacy: transporte compressível sem pressão (o mesmo do compute do host). Shallow: Legacy + pressão da camada
// rasa, só no campo da CPU. SemiLagrangian: variante incompressível de prova (FOGREMASTER_FLUID).
enum class Transport { Legacy, SemiLagrangian, Shallow };

struct SolverOptions {
    Transport transport{Transport::Legacy}; // A/B opt-in: preserve the provider's compressible attack effect.
    int pressureIterations{20};
    bool correctDensity{true}; // Limited MacCormack; velocity uses first-order backtracing.
    bool conserveMass{true}; // Bounded rescaling within each connected closed region.
    float damping{VelocityDamping};
    float recovery{Recovery};
    float windX{16.0f}, windZ{5.0f};
    float gravity{ShallowGravity};         // Shallow: u²/s² por unidade de densidade
    float confinement{ShallowConfinement}; // Shallow
    float divergenceDamping{ShallowDivergenceDamping}; // Shallow
    float bladeCells{ShallowBladeCells};   // Shallow
    float viscosity{ShallowViscosity};     // Shallow
    float bladeGain{ShallowBladeGain};     // Shallow
    float attackCap{ShallowAttackCap};     // Shallow
    float bladeRadial{ShallowBladeRadial}; // Shallow
};

SolverOptions shallowOptions();
// The host validates extinction in [0, 1] independently of the cell density limit.
float volumeExtinction(float extinction, float densityScale);

struct ProjectionStats {
    float meanBefore{}, meanAfter{}, maxAfter{}; // |divergence| in seconds^-1, on MAC faces.
};

class Field {
public:
    void reset(float x, float z, float spacing = Spacing);
    void shift(int dx, int dz);
    void terrain(int x, int z, float y, bool open, bool fadeIn = false);
    void blockEdge(int x, int z, int otherX, int otherZ, bool blocked);
    bool disturb(Point a, Point b, float radius, float bottom, float top, Point force, float clearance,
                 float core = 0.0f, float radialSpeed = 0.0f, float strength = 1.0f, int attackTicks = 0);
    bool blade(Point oldBase, Point oldTip, Point base, Point tip, float radius, float strength, int attackTicks);
    bool spin(Point center, float radius, float speed, float strength = 1.0f, int attackTicks = DefaultAttackTicks);
    // Pouso (0.31.0): o corpo que cai na camada empurra a névoa para fora, num anel que se espalha da batida.
    // impactSpeed é a velocidade de queda em u/s; o empurrão vai para fora e para cima, sem giro.
    bool land(Point center, float impactSpeed, float strength = 1.0f, int attackTicks = DefaultAttackTicks);
    void beginTick();
    void step(float dt, float liftStrength = 1.0f);
    void setSolverOptions(SolverOptions options);
    void setWind(float x, float z); // u/s; vale para os dois transportes.
    // Densidade de repouso em bancos: low/high multiplicam RestDensity, wavelength em u (0 = uniforme), offset em u
    // (o provider soma o vento a ele). Recalcula o repouso de todas as células.
    void banks(float low, float high, float wavelength, float offsetX, float offsetZ);
    float rest(int x, int z) const { return rest_[index(x, z)]; }
    float restWorld(float worldX, float worldZ) const { return restAt(worldX, worldZ); }
    const SolverOptions& solverOptions() const { return options_; }
    const ProjectionStats& projectionStats() const { return projectionStats_; }
    float spacing() const { return spacing_; }
    float worldX(int x) const { return x0_ + spacing_ * x; }
    float worldZ(int z) const { return z0_ + spacing_ * z; }
    const Cell& cell(int x, int z) const { return cells_[index(x, z)]; }
    const Impulse& beforeStep(int x, int z) const { return before_[index(x, z)]; }
    const Impulse& afterStep(int x, int z) const { return after_[index(x, z)]; }
    float liftBeforeStep(int x, int z) const { return beforeLift_[index(x, z)]; }
    // Diagnóstico: velocidade tangencial média (u/s, anti-horário positivo de +x para +z) numa coroa de 1 a 4
    // células em volta de um ponto, e a menor densidade a até `radius` dele.
    float circulation(float x, float z) const;
    float lowestDensity(float x, float z, float radius) const;
    static bool connected(const Cell& a, const Cell& b);
    bool surfaceQuadOpen(int x, int z) const;

private:
    friend struct FieldTestAccess;
    static constexpr int Count = Side * Side;
    using Grid = std::array<Cell, Count>;
    struct Sample { float value{}, low{}, high{}; };
    bool link(int x, int z, int nx, int nz) const;
    Point trace(int x, int z, float dx, float dz) const;
    Sample sample(const Grid& grid, int x, int z, Point p, float Cell::*member) const;
    Point departure(const Grid& velocity, int x, int z, float dt) const;
    void stepFluid(float liftStrength);
    void project();
    void conserveDensity();
    SolverOptions options_{};
    ProjectionStats projectionStats_{};
    double remainder_{};
    // Reused scratch storage; no allocations or solver-dependent stack growth per tick.
    Grid source_{}, advected_{}, flow_{};
    std::array<float, Count> poisson_{}, divergence_{};
    std::array<std::array<int, 4>, Count> links_{};
    std::array<float, (Side + 1) * Side> faceX_{}, faceZ_{};
    struct Push { float x{}, z{}, lift{}; };
    static int index(int x, int z) { return z * Side + x; }
    void applyImpulse(int index, float densityScale, float vx, float vz);
    bool disturbMotion(Point a, Point b, float radius, float bottom, float top, Point baseForce, Point tipForce,
                       float clearance, float core, float radialSpeed, float strength, int attackTicks,
                       float radialBase = 1.8f);
    void queueAttack(int index, float vx, float vz, float lift, int ticks, int delay);
    void advanceAttackPressure(float liftStrength);
    float restAt(float worldX, float worldZ) const;
    void refreshRest();
    float x0_{}, z0_{}, spacing_{Spacing};
    float bankLow_{1.0f}, bankHigh_{1.0f}, bankWavelength_{0.0f}, bankOffsetX_{}, bankOffsetZ_{};
    std::array<float, Side * Side> rest_ = [] {
        std::array<float, Side * Side> uniform;
        uniform.fill(RestDensity);
        return uniform;
    }();
    std::array<Cell, Side * Side> cells_{};
    std::array<Impulse, Side * Side> before_{}, after_{};
    std::array<float, Side * Side> beforeLift_{};
    std::array<std::array<Push, Side * Side>, MaxAttackTicks> pressure_{};
    int pressureTick_{};
    bool stepped_{};
};

} // namespace fog
