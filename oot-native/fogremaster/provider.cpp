#include <algorithm>
#include <cmath>

namespace {
// 1/8 de célula além do meio passo: a reversão exige 10 u na grade de 40 u, mais que a deriva de ±4 u.
constexpr float FollowHysteresisCells = 0.125f;

int followShiftAxis(float distance, float spacing, float overlap, float hysteresis) {
    const float cells = distance / spacing;
    if (hysteresis > 0.0f && std::abs(cells) <= overlap + 0.5f + hysteresis) return 0;
    return static_cast<int>(std::lround(cells - std::clamp(cells, -overlap, overlap)));
}
} // namespace

// O teste isolado inclui apenas a decisão acima, sem os headers do SDK.
#ifndef FOGREMASTER_FOLLOW_TEST
#include "fog_field.h"
#include "fog_stable.h"
#include "fog_color.h"
#include "fog_wind.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

#include "oot_engine.h"
#include "oot_fog.h"
#include "oot_hooks.h"
#include "oot_layout_id.h"
#include "oot_render.h"
#include "z64.h"

#ifndef LINKSPAN_OOT_FOG_FLOW_DETAIL_WIDE
#define LINKSPAN_OOT_FOG_FLOW_DETAIL_WIDE 2048u
#endif

namespace {
constexpr char FogQualitySetting[] = "gDeveloperTools.FogVolumeQuality";
// 0.28.1: a grade da vila segue o Link com células de 40 u (eram 80 u presas à vila). 24 x 40 = 960 u fica abaixo
// dos 1024 u em que o shader do host troca para o perfil largo, então a vila usa o mesmo perfil de Lost Woods: piso
// interpolado entre células e detalhe fino. Com 80 u cada terraço virava uma folha reta de névoa, e o menor vórtice
// que a malha carrega (~3 células) tinha 240 u.
constexpr float KokiriSpacing = 40.0f;
// 0.28.0: névoa rasteira fica só no chão. O andar de cima (ponte de corda, plataformas) ocupa a célula inteira de
// 80 u e vira uma folha flutuando no meio das casas, na altura da ponte. A amostragem empilhada continua: é ela que
// acha o chão debaixo da ponte para a camada de baixo.
constexpr bool RenderDecks = false;
constexpr float GapRadiusCells = 1.6f; // raio, em células, onde o diagnóstico procura o vão do golpe
// A grade anda quando o alvo se afasta 2 células do centro e para a 1 célula dele. O alvo fica à frente do Link,
// na direção em que a câmera olha: atrás da câmera a névoa não aparece.
// 0.29.1: a grade fica centrada no Link e anda de célula em célula. O avanço na direção da câmera (0.28.1) fazia a
// névoa andar quando a câmera girava. A borda não é mais a da grade (que salta a cada passo): a densidade entregue
// ao host esmaece num raio contínuo em volta do Link, de KokiriFadeInner a KokiriFadeOuter, que cabe na grade em
// qualquer fase do passo. Assim a névoa à frente aparece aos poucos conforme ele anda.
constexpr int KokiriFollowCells = 1;
constexpr float KokiriFollowOverlap = 0.0f;
constexpr float KokiriEdgeFeather = 16.0f;
constexpr float KokiriFadeInner = 250.0f;
constexpr float KokiriFadeOuter = 420.0f;
constexpr float CliffSlope = 0.7f; // desnível por célula acima do qual a beirada alta fica sem névoa (~35°)

// 0.29.2: grade de desenho da vila, separada da simulação. A simulação continua fina (25 x 40 u em volta do Link);
// o que vai para o host é esta grade de 32 x 58 u (1798 u de lado), com o chão amostrado por ela mesma. Perto do
// Link ela recebe a densidade e a velocidade simuladas; longe, a densidade de repouso dos bancos, sem interação.
constexpr float SpikeRise = 15.0f; // amostra de chão este tanto acima de 6+ vizinhas é objeto, não terreno
constexpr int VillageSide = 32;
constexpr float VillageSpacing = 80.0f;
constexpr float VillageFadeInner = 1040.0f;
constexpr float VillageFadeOuter = 1220.0f;
// Sonda de dossel fixa no mundo: fora do primeiro plano ao olhar da vila para +X.
constexpr char CanopySetting[] = "gDeveloperTools.FogreKokiriCanopy";
constexpr float CanopyCenterX = 930.0f, CanopyCenterZ = 330.0f;
constexpr float CanopyRadiusX = 240.0f, CanopyRadiusZ = 170.0f;
constexpr float CanopyFeatherX = 120.0f, CanopyFeatherZ = 100.0f;
constexpr float CanopyBase = 125.0f, CanopyGroundClearance = 64.0f;
constexpr float CanopySlopeX = 25.0f, CanopySlopeZ = 15.0f, CanopyCrown = 41.0f;
constexpr float CanopyDensityAt100 = 0.025f;
// `cliff`: fechada só pela regra do degrau (CliffSlope). Água, parede e falta de chão não marcam.
struct VillageCell { float floor{}; bool open{}; bool cliff{}; };

// RFC 0034 (rascunho): grade fina aninhada. Enquanto o SDK publicado não traz o contrato, ele é espelhado aqui;
// o campo novo fica logo depois do último de ShipOotFogFlowV2, que é onde o host o lê.
#ifndef LINKSPAN_OOT_FOG_FLOW_DETAIL_GRID
#define LINKSPAN_OOT_FOG_FLOW_DETAIL_GRID 1024u
struct ShipOotFogDetailCellV1 { float density; float velocity_x, velocity_z; };
struct ShipOotFogDetailV1 {
    uint32_t size;
    uint32_t width, depth;
    float x_min, x_max, z_min, z_max;
    float blend;
    const ShipOotFogDetailCellV1* cells;
};
struct FogFlowWithDetail : ShipOotFogFlowV2 { const ShipOotFogDetailV1* detail; };
static_assert(offsetof(FogFlowWithDetail, detail) == sizeof(ShipOotFogFlowV2));
#else
using FogFlowWithDetail = ShipOotFogFlowV2;
#endif
constexpr char StableSetting[] = "gDeveloperTools.FogreStable"; // 0.33.0: 1 = Stable Fluids na grade fina
constexpr char StableVorticitySetting[] = "gDeveloperTools.FogreStableVorticity"; // ε x10 (padrão 40)
constexpr char StableRecoverySetting[] = "gDeveloperTools.FogreStableRecovery"; // 1/s x100 (padrão 8)
constexpr char StableDampingSetting[] = "gDeveloperTools.FogreStableDamping"; // 1/s x100 (padrão 60)
constexpr char StableStirSetting[] = "gDeveloperTools.FogreStableStir"; // ganho do stir em % (padrão 100)
constexpr char StableCurlSetting[] = "gDeveloperTools.FogreStableCurl"; // curl-noise, u/s² (padrão 0: com 12 o flow map do host craquelava a névoa densa, A/B de 22/11)
constexpr char StableWispSetting[] = "gDeveloperTools.FogreStableWisps"; // contraste dos fiapos em % (padrão 110)
constexpr char StableWispScaleSetting[] = "gDeveloperTools.FogreStableWispScale"; // u (padrão 180)
constexpr char StableSideSetting[] = "gDeveloperTools.FogreStableSide"; // 64, 96 ou 128; outro valor = 96
constexpr char StableCellSetting[] = "gDeveloperTools.FogreStableCell"; // décimos de u; 50..200 (padrão 100)
constexpr int StableSmallSide = 64, StableDefaultSide = 96, StableMaxSide = 128;
constexpr int StableDefaultCellTenths = 100, StableMinCellTenths = 50, StableMaxCellTenths = 200;
constexpr float StableCellUnit = 0.1f;
// Faixas aceitas das CVars do fluido (valor bruto da CVar). Fora delas o solver recebe amortecimento negativo ou
// ganhos absurdos e a velocidade vira inf/NaN; o clamp fica na fronteira, antes de setParams.
constexpr int StableVorticityDefault = 40, StableVorticityMax = 400;
constexpr int StableRecoveryDefault = 8, StableRecoveryMax = 500;
constexpr int StableDampingDefault = 60, StableDampingMax = 1000;
constexpr int StableCurlDefault = 0, StableCurlMax = 200;
// Releitura de repouso/paredes: a cada StableRefreshTicks ticks, fatiada em StableRefreshSlices ticks de linhas.
constexpr int StableRefreshTicks = 10, StableRefreshSlices = 5;
// Lâmina: varredura maior que isto entre duas poses é salto de pose (teleporte), não golpe (mesmo limite de Field::blade).
constexpr float StableBladeMaxSweep = 180.0f;
// Faixa vertical de cada interação em relação ao piso da camada (mesmos limites que o campo largo usa).
constexpr float StableBladeBottom = -32.0f, StableBladeTop = 12.0f;
constexpr float StableLandBottom = -12.0f, StableLandTop = 36.0f;
constexpr int StableLayerSamples = 9;
// Lado máximo da grade fina que o host aceita: 32 sem o bit WIDE (host antigo recusa o submit_flow inteiro com
// INVALID_ARGUMENT), LINKSPAN_OOT_FOG_DETAIL_MAX_SIDE (128) com ele.
#ifdef LINKSPAN_OOT_FOG_MAX_SIDE
constexpr int DetailNarrowMaxSide = static_cast<int>(LINKSPAN_OOT_FOG_MAX_SIDE);
#else
constexpr int DetailNarrowMaxSide = 32;
#endif
#ifdef LINKSPAN_OOT_FOG_DETAIL_MAX_SIDE
constexpr int DetailWideMaxSide = static_cast<int>(LINKSPAN_OOT_FOG_DETAIL_MAX_SIDE);
#else
constexpr int DetailWideMaxSide = 128;
#endif
constexpr char DetailSetting[] = "gDeveloperTools.FogreDetailGrid"; // 0 desliga a grade fina (comparação A/B)
// 0 volta a fechar a grade fina pela máscara da grade de desenho (80 u) nas beiradas de barranco (comparação A/B).
constexpr char CliffFitSetting[] = "gDeveloperTools.FogreCliffFit";
// Névoa que o host desenharia acima do chão real: de FloatAllowed a FloatClosed u, a célula fina fecha.
constexpr float FloatAllowed = 4.0f, FloatClosed = 16.0f;
// Raio em volta do Link em que vale o ajuste. Fica dentro da área em que o host usa só a grade fina (480 u menos
// a faixa de 80 u e meio passo de 20 u): a grade anda em passos de 40 u e, se a regra mudasse junto da borda dela,
// a névoa do barranco saltaria a cada passo.
constexpr float CliffFitInner = 250.0f, CliffFitOuter = 360.0f;
struct Village {
    float x0{}, z0{};
    bool valid{};
    std::array<VillageCell, VillageSide * VillageSide> cells{};
    float worldX(int x) const { return x0 + VillageSpacing * x; }
    float worldZ(int z) const { return z0 + VillageSpacing * z; }
};

struct Fog {
    const ShipOotEngineV1* engine{};
    const ShipOotMovementV1* movement{};
    const ShipOotRenderV3* render{};
    const ShipOotFogV1* volume{};
    const ShipOotFogV2* flow{};
    float (*floorRay)(CollisionContext*, CollisionPoly**, Vec3f*){};
    int32_t (*waterSurface)(PlayState*, CollisionContext*, float, float, float*, WaterBox**){};
    int32_t (*wallLine)(CollisionContext*, Vec3f*, Vec3f*, Vec3f*, CollisionPoly**,
                        int32_t, int32_t, int32_t, int32_t, int32_t*){};
    void* (*alloc)(GraphicsContext*, size_t){};
    void (*restoreXlu)(GraphicsContext*){};
    fog::Field field;
    fog::Field upperField;
    // 0.33.0: Stable Fluids (Stam) fino que segue o Link e alimenta a grade fina; o stir só empurra.
    fog::StableFluid stable;
    bool stableReady{false};
    int stableSide{}, stableCellTenths{}; // configuração aplicada; célula antes do limite da janela
    float stableTime{};
    int stableRestAge{};   // ticks desde a última releitura do repouso e das paredes (o campo largo muda)
    int stableRefreshRow{-1}; // próxima linha da releitura fatiada em andamento; -1 = ociosa
    float stableContrast{}, stableWispScale{}; // CVars de fiapos capturadas pelos samplers instalados
    uint32_t stirs{};      // chamadas de stir aceitas pelo fluido
    uint32_t stableMicros{}; // duração do último step do fluido, us
    uint32_t stableMicrosPeak{};
    bool stackedTerrain{};
    PlayState* scene{};
    int room{-1};
    fog::Point previousBase{}, previousTip{};
    bool weaponActive{};
    bool spinActive{};
    bool enabled{true};
    bool preferVolume{true};
    bool preferCompute{true};
    bool preferParticles{true};
    bool preferLighting{true};
    bool preferSceneColor{true};
    fogcolor::FrameColor sceneColor;
    fogcolor::Environment sceneLight; // última amostra, para o diagnóstico
    int densityPercent{100}, attackPercent{100}, liftPercent{100};
    int attackTicks{fog::DefaultAttackTicks};
    int quality{-1}, hostQuality{1};
    unsigned draws{}, bodyTicks{}, actorTicks{}, cuts{};
    unsigned cylinders{}, spheres{}, quads{}, enemyAttacks{};
    unsigned volumes{}, spins{};
    unsigned flowSubmissions{};
    unsigned gridShifts{}, blasts{}, walls{}, water{}, rims{};
    unsigned decks{};
    // Pouso (0.31.0): quadros no ar, velocidade de queda do último quadro no ar (u/s) e contagem para o diagnóstico.
    int airFrames{};
    float fallSpeed{};
    unsigned landings{};
    float landPeak{};
    float referenceFloor{};
    float appearance{};
    float peakLift{}, peakDeckLift{};
    // Medição depois de cada golpe (0.28.0): giro e vão em volta de onde o Link golpeou. cutAge < 0 = parada.
    int cutAge{-1};
    uint32_t traces{};
    float cutX{}, cutZ{};
    float swirlBefore{}, swirlPeak{}, swirlLate{};
    float gapBefore{fog::RestDensity}, gapLow{fog::RestDensity}, gapLate{fog::RestDensity};
    uint32_t lastAttackFrame{};
    uint32_t lastSimulationFrame{}, generation{}, flowTick{};
    // Vento da cena (R054): relógio da rajada com a política do host, base sem rajada e portador aplicado.
    fogwind::Clock windClock;
    float windSeconds{};
    float windBaseX{fogwind::LegacyX}, windBaseZ{fogwind::LegacyZ};
    float windX{fogwind::LegacyX}, windZ{fogwind::LegacyZ};
    // Aparência (0.29.0): camada entregue ao host e deriva dos bancos. Os valores vêm das constantes do campo e
    // podem ser trocados em jogo pelas CVars de ajuste abaixo.
    float layerHeight{fog::RenderLayerHeight}, falloff{fog::RenderFalloff}, extinction{fog::RenderExtinction};
    float white{fogcolor::White};
    float bankX{}, bankZ{};
    float bankScale{1.0f}; // Kokiri: a grade de desenho (80 u) só carrega bancos com 4 células de comprimento de onda
    float linkX{}, linkZ{}; // posição contínua do Link, centro do esmaecimento da vila
    Village village;
};

// Ajuste em jogo, sem recompilar: `set gDeveloperTools.FogreExtinction 300` no console. Ausentes, valem as constantes.
constexpr char LayerHeightSetting[] = "gDeveloperTools.FogreLayerHeight"; // u
constexpr char FalloffSetting[] = "gDeveloperTools.FogreFalloff";         // u
constexpr char ExtinctionSetting[] = "gDeveloperTools.FogreExtinction";   // milésimos por u
// Brilho da névoa no Lost Woods, em %. A luz do jogo e a imagem não separam o túnel escuro da vila (testado em
// 14/11: o chão atrás da névoa tem o brilho da grama de Kokiri), então a cena leva o próprio ajuste.
constexpr char LostWoodsSetting[] = "gDeveloperTools.FogreLostWoods";
constexpr int LostWoodsBrightness = 60;
// Corredores escuros: a névoa branca brilha contra a rocha quase preta.
constexpr char GraveyardSetting[] = "gDeveloperTools.FogreGraveyard";
constexpr int GraveyardBrightness = 45;
constexpr char ShadowSetting[] = "gDeveloperTools.FogreShadow";
constexpr int ShadowBrightness = 45;
// Brilho com o sol abaixo do horizonte (%), e a faixa de altura do sol (fração do raio) em que o crepúsculo passa.
constexpr char NightSetting[] = "gDeveloperTools.FogreNight";
constexpr int NightBrightness = 55;
constexpr float NightTwilight = 0.15f;
constexpr char WhiteSetting[] = "gDeveloperTools.FogreWhite";             // %
constexpr char BankLowSetting[] = "gDeveloperTools.FogreBankLow";         // % do repouso
constexpr char BankHighSetting[] = "gDeveloperTools.FogreBankHigh";       // % do repouso
constexpr char BankSizeSetting[] = "gDeveloperTools.FogreBankSize";       // u; 0 = névoa uniforme

constexpr char WindFieldSetting[] = "gDeveloperTools.WindField";
constexpr char WindAmbientSetting[] = "gDeveloperTools.WindAmbient";
constexpr char WindOverrideSetting[] = "gDeveloperTools.WindSpeedOverride";

int windSetting(const Fog& mod, const char* key, int fallback) {
    return mod.movement && mod.movement->get_setting_int ? mod.movement->get_setting_int(key, fallback) : fallback;
}

// ---- 0.33.0: Stable Fluids + stir -------------------------------------------------------------------------
bool stableWanted(const Fog& mod) { return windSetting(mod, StableSetting, 1) != 0; }

int stableSide(const Fog& mod) {
    const int side = windSetting(mod, StableSideSetting, StableDefaultSide);
    return side == StableSmallSide || side == StableDefaultSide || side == StableMaxSide ? side : StableDefaultSide;
}

uint32_t flowFeatures(const Fog& mod);
bool cpuFieldActive(const Fog& mod);
bool finite(fog::Point p);

bool hostWideDetail(const Fog& mod) { return (flowFeatures(mod) & LINKSPAN_OOT_FOG_FLOW_DETAIL_WIDE) != 0u; }

int hostDetailMaxSide(const Fog& mod) { return hostWideDetail(mod) ? DetailWideMaxSide : DetailNarrowMaxSide; }

// O fluido só tem consumidor no caminho de submitVillage com a grade fina: vila ativa (Kokiri), volume preferido,
// campo da CPU, bit DETAIL_GRID e a CVar de detalhe ligada. Fora disso simular é custo sem destino.
bool stableConsumed(const Fog& mod) {
    const auto features = flowFeatures(mod);
    return mod.preferVolume && mod.village.valid && cpuFieldActive(mod) &&
           (features & LINKSPAN_OOT_FOG_FLOW_VORTICES) && (features & LINKSPAN_OOT_FOG_FLOW_DETAIL_GRID) &&
           mod.flow && mod.flow->submit_flow && windSetting(mod, DetailSetting, 1) != 0;
}

// Sem o bit WIDE o fluido (>32) nunca é enviado: não gasta CPU simulando o que o host não receberia.
bool stableUsable(const Fog& mod) {
    return stableWanted(mod) && stableSide(mod) <= hostDetailMaxSide(mod) && stableConsumed(mod);
}

int stableCellTenths(const Fog& mod) {
    return std::clamp(windSetting(mod, StableCellSetting, StableDefaultCellTenths),
                      StableMinCellTenths, StableMaxCellTenths);
}

// O fluido fino precisa caber na grade principal enviada; fora da vila ela é a própria grade de 25 células.
float stableCellSize(const Fog& mod) {
    const float span = mod.field.spacing() * float(fog::Side - 1);
    const float cell = StableCellUnit * float(mod.stableCellTenths);
    return std::min(cell, 0.92f * span / float(mod.stableSide));
}

// setParams invalida o cache de curl do fluido: só chama quando algum valor aplicado muda.
void stableParams(Fog& mod) {
    fog::StableParams p;
    p.vorticity = 0.1f * float(std::clamp(windSetting(mod, StableVorticitySetting, StableVorticityDefault), 0,
                                          StableVorticityMax));
    p.recovery = 0.01f * float(std::clamp(windSetting(mod, StableRecoverySetting, StableRecoveryDefault), 0,
                                          StableRecoveryMax));
    p.damping = 0.01f * float(std::clamp(windSetting(mod, StableDampingSetting, StableDampingDefault), 0,
                                         StableDampingMax));
    p.curlAmp = float(std::clamp(windSetting(mod, StableCurlSetting, StableCurlDefault), 0, StableCurlMax));
    p.windX = mod.windX;
    p.windZ = mod.windZ;
    const auto& a = mod.stable.params();
    if (a.vorticity == p.vorticity && a.recovery == p.recovery && a.damping == p.damping &&
        a.curlAmp == p.curlAmp && a.windX == p.windX && a.windZ == p.windZ)
        return;
    // O resto de `p` são os padrões da struct; `a` os conserva porque só este caminho chama setParams.
    mod.stable.setParams(p);
}

// Repouso e paredes vêm do campo largo: densidade de repouso dos bancos e células fechadas (casa, parede).
// Fiapos (0.33.2): densidade uniforme transportada não mostra nada; a névoa precisa de textura para o fluxo
// aparecer. Ruído de valor em 3 oitavas, fixo no mundo: o fluido arrasta os fiapos e a recuperação os devolve.
float hash2(int x, int z) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(z) * 0xd8163841u;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    return float((h ^ (h >> 15)) & 0xffffu) / 65535.0f;
}
float valueNoise(float x, float z) {
    const float fx = std::floor(x), fz = std::floor(z);
    const int ix = int(fx), iz = int(fz);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float a = hash2(ix, iz), b = hash2(ix + 1, iz), c = hash2(ix, iz + 1), d = hash2(ix + 1, iz + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}
float wisps(float wx, float wz, float scale) {
    // Duas oitavas largas (>= 9 células): a oitava de 0,2 x escala ficava abaixo de 3 células e virava linhas no volume.
    return 0.65f * valueNoise(wx / scale, wz / scale) +
           0.35f * valueNoise(wx / (0.5f * scale) + 17.3f, wz / (0.5f * scale) - 4.1f); // 0..1, média ~0.5
}

float stableContrastSetting(const Fog& mod) { return 0.01f * float(windSetting(mod, StableWispSetting, 110)); }
float stableWispScaleSetting(const Fog& mod) {
    return float(std::max(windSetting(mod, StableWispScaleSetting, 180), 20));
}

float stableRestAt(const Fog& mod, float contrast, float scale, float wx, float wz) {
    const float k = std::clamp(1.0f + contrast * (wisps(wx, wz, scale) - 0.5f) * 2.0f, 0.05f, 2.2f);
    return std::min(mod.field.restWorld(wx, wz) * k, fog::MaxDensity);
}

bool stableSolidAt(const Fog& mod, float wx, float wz) {
    const float gx = (wx - mod.field.worldX(0)) / mod.field.spacing();
    const float gz = (wz - mod.field.worldZ(0)) / mod.field.spacing();
    const int x = int(std::lround(gx)), z = int(std::lround(gz));
    if (x < 0 || z < 0 || x >= fog::Side || z >= fog::Side) return false;
    return !mod.field.cell(x, z).open;
}

// Instala os samplers (usados nas células novas do recenter) e reavalia a janela inteira. Só no reset ou quando as
// CVars de fiapos mudam; a releitura periódica é stableRefresh.
void stableSamplers(Fog& mod) {
    const float contrast = stableContrastSetting(mod), scale = stableWispScaleSetting(mod);
    mod.stableContrast = contrast;
    mod.stableWispScale = scale;
    mod.stable.setRestSampler([&mod, contrast, scale](float wx, float wz) {
        return stableRestAt(mod, contrast, scale, wx, wz);
    });
    mod.stable.setSolidSampler([&mod](float wx, float wz) { return stableSolidAt(mod, wx, wz); });
    mod.stableRefreshRow = -1;
}

// O repouso do campo largo (bancos da vila, piso) e as paredes mudam com o tempo. A cada StableRefreshTicks ticks
// começa uma passada que reavalia as células em fatias de linhas, uma fatia por tick, e só escreve o que mudou:
// setSolid suja a topologia e setRest é escrita, então reaplicar o mesmo valor seria custo à toa.
void stableRefresh(Fog& mod) {
    const int n = mod.stable.size();
    if (mod.stableRefreshRow < 0) {
        if (++mod.stableRestAge < StableRefreshTicks) return;
        mod.stableRestAge = 0;
        if (stableContrastSetting(mod) != mod.stableContrast || stableWispScaleSetting(mod) != mod.stableWispScale) {
            stableSamplers(mod); // CVar de fiapos mudou: os samplers instalados capturaram o valor velho
            return;
        }
        mod.stableRefreshRow = 0;
    }
    const int rows = (n + StableRefreshSlices - 1) / StableRefreshSlices;
    const int end = std::min(n, mod.stableRefreshRow + rows);
    for (int j = mod.stableRefreshRow; j < end; ++j) {
        for (int i = 0; i < n; ++i) {
            const float wx = mod.stable.cellCenterX(i), wz = mod.stable.cellCenterZ(j);
            const float rest = stableRestAt(mod, mod.stableContrast, mod.stableWispScale, wx, wz);
            if (rest != mod.stable.rest(i, j)) mod.stable.setRest(i, j, rest); // antes de setSolid: ele relê rest
            const bool solid = stableSolidAt(mod, wx, wz);
            if (solid != mod.stable.solid(i, j)) mod.stable.setSolid(i, j, solid);
        }
    }
    mod.stableRefreshRow = end >= n ? -1 : end;
}

void stableReset(Fog& mod) {
    mod.stableReady = false;
    if (!stableUsable(mod)) return;
    mod.stableSide = stableSide(mod);
    mod.stableCellTenths = stableCellTenths(mod);
    const float h = stableCellSize(mod);
    const float half = 0.5f * h * float(mod.stableSide);
    mod.stable.reset(mod.stableSide, h, mod.linkX - half, mod.linkZ - half);
    stableParams(mod);
    stableSamplers(mod);
    for (int j = 0; j < mod.stable.size(); ++j)
        for (int i = 0; i < mod.stable.size(); ++i)
            if (!mod.stable.solid(i, j)) mod.stable.setDensity(i, j, mod.stable.rest(i, j));
    mod.stableRestAge = 0;
    mod.stableReady = true;
}

float stableStirGain(const Fog& mod) {
    return 0.01f * float(windSetting(mod, StableStirSetting, 100));
}

// Segmento a-b varrido com a velocidade v (u/s): a força entra pelo solver, o giro sai dele.
// O fluido é 2D: a altura é filtrada aqui, contra o piso da camada (como Field::disturb faz por célula). O ponto
// (x, y, z) entra se a faixa [y + bottom, y + top] toca a camada [piso, piso + LayerHeight].
bool stableLayerHit(const Fog& mod, float x, float y, float z, float bottom, float top) {
    const float gx = (x - mod.field.worldX(0)) / mod.field.spacing();
    const float gz = (z - mod.field.worldZ(0)) / mod.field.spacing();
    const int cx = int(std::lround(gx)), cz = int(std::lround(gz));
    if (cx < 0 || cz < 0 || cx >= fog::Side || cz >= fog::Side) return false;
    const auto& cell = mod.field.cell(cx, cz);
    if (!cell.open) return false;
    return !(y + bottom > cell.floor + fog::LayerHeight || y + top < cell.floor);
}

void stableStir(Fog& mod, fog::Point a, fog::Point b, float vx, float vz, float radius, float strength,
                float bottom, float top) {
    if (!mod.stableReady || strength <= 0.0f) return;
    // Amostra o segmento e empurra só o trecho que toca a camada (o piso varia ao longo dele).
    const float length = std::hypot(b.x - a.x, b.z - a.z);
    const int samples = std::clamp(int(std::ceil(length / mod.field.spacing())) + 1, 2, StableLayerSamples);
    int first = -1, last = -1;
    for (int k = 0; k < samples; ++k) {
        const float t = float(k) / float(samples - 1);
        if (!stableLayerHit(mod, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, bottom, top))
            continue;
        if (first < 0) first = k;
        last = k;
    }
    if (first < 0) return;
    const float t0 = std::max(0.0f, (float(first) - 0.5f) / float(samples - 1));
    const float t1 = std::min(1.0f, (float(last) + 0.5f) / float(samples - 1));
    mod.stable.stirSegment({a.x + (b.x - a.x) * t0, a.z + (b.z - a.z) * t0},
                           {a.x + (b.x - a.x) * t1, a.z + (b.z - a.z) * t1}, {vx, vz}, radius, strength);
    ++mod.stirs;
}

// Lâmina: pose velha (oldBase-oldTip) para a nova (base-tip), em 4 pedaços; cada um empurra com a velocidade do
// seu ponto médio. A força é toda a entrada: o vórtice atrás da lâmina sai da projeção e do confinamento.
void stableBlade(Fog& mod, fog::Point oldBase, fog::Point oldTip, fog::Point base, fog::Point tip, float radius) {
    if (!mod.stableReady) return;
    // Pose descontínua (teleporte, reset) não é golpe: dividir o salto por StepSeconds viraria velocidade enorme.
    if (!finite(oldBase) || !finite(oldTip) || !finite(base) || !finite(tip)) return;
    const float sweep = std::max(std::hypot(base.x - oldBase.x, base.y - oldBase.y, base.z - oldBase.z),
                                 std::hypot(tip.x - oldTip.x, tip.y - oldTip.y, tip.z - oldTip.z));
    if (sweep > StableBladeMaxSweep) return;
    const auto lerp = [](fog::Point p, fog::Point q, float t) {
        return fog::Point{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t};
    };
    const float gain = stableStirGain(mod) * (mod.attackPercent * 0.01f);
    for (int k = 0; k < 4; ++k) {
        const float t0 = k / 4.0f, t1 = (k + 1) / 4.0f, tm = 0.5f * (t0 + t1);
        const auto old = lerp(oldBase, oldTip, tm), now = lerp(base, tip, tm);
        stableStir(mod, lerp(base, tip, t0), lerp(base, tip, t1), (now.x - old.x) / fog::StepSeconds,
                   (now.z - old.z) / fog::StepSeconds, radius, gain, StableBladeBottom, StableBladeTop);
    }
}
// -------------------------------------------------------------------------------------------------------------

// Mesmo vento que o WindField do host vê: envCtx da cena, brisa ambiente sob céu aberto e as CVars dele.
void updateWind(Fog& mod, const PlayState& play) {
    fogwind::Scene scene;
    const auto setting = [&mod](const char* key, int fallback) { return windSetting(mod, key, fallback); };
    scene.hostWind = setting(WindFieldSetting, 1) != 0;
    scene.direction = {float(play.envCtx.windDirection.x), float(play.envCtx.windDirection.y),
                       float(play.envCtx.windDirection.z)};
    scene.speed = float(play.envCtx.windSpeed);
    scene.skybox = play.skyboxId;
    scene.ambient = float(setting(WindAmbientSetting, 40));
    scene.override = setting(WindOverrideSetting, -1);
    const float cx = mod.field.worldX(fog::Side / 2), cz = mod.field.worldZ(fog::Side / 2);
    const auto base = fogwind::Base(scene);
    const auto wind = fogwind::Carrier(scene, cx, cz, mod.windSeconds);
    mod.windBaseX = base[0];
    mod.windBaseZ = base[2];
    mod.windX = wind[0];
    mod.windZ = wind[1];
    mod.field.setWind(wind[0], wind[1]);
    if (mod.stackedTerrain) mod.upperField.setWind(wind[0], wind[1]);
}

// Camada e bancos. `drift` anda o padrão dos bancos com o vento deste tick; na troca de cena ele só é reaplicado,
// antes de o chão ser amostrado, para a névoa já nascer em bancos.
void updateLook(Fog& mod, bool drift) {
    const auto setting = [&mod](const char* key, float fallback, float scale, float lo, float hi) {
        return std::clamp(float(windSetting(mod, key, int(std::lround(fallback / scale)))) * scale, lo, hi);
    };
    mod.layerHeight = setting(LayerHeightSetting, fog::RenderLayerHeight, 1.0f, 4.0f, 96.0f);
    mod.falloff = setting(FalloffSetting, fog::RenderFalloff, 1.0f, 2.0f, 96.0f);
    mod.extinction = setting(ExtinctionSetting, fog::RenderExtinction, 0.001f, 0.0f, 2.0f);
    mod.white = setting(WhiteSetting, fogcolor::White, 0.01f, 0.0f, 1.0f);
    if (drift) {
        mod.bankX += mod.windX * fog::StepSeconds * fog::BankDrift;
        mod.bankZ += mod.windZ * fog::StepSeconds * fog::BankDrift;
    }
    const float low = setting(BankLowSetting, fog::BankLow, 0.01f, 0.0f, 1.0f);
    const float high = setting(BankHighSetting, fog::BankHigh, 0.01f, 0.0f, 1.0f / fog::RestDensity);
    const float size = setting(BankSizeSetting, fog::BankWavelength, 1.0f, 0.0f, 2048.0f);
    mod.field.banks(low, high, size * mod.bankScale, mod.bankX, mod.bankZ);
    mod.upperField.banks(low, high, size * mod.bankScale, mod.bankX, mod.bankZ);
    // Física da camada rasa, ajustável ao vivo (`set gDeveloperTools.FogreWave 45`). Ausentes, valem as constantes.
    auto options = mod.field.solverOptions();
    if (options.transport == fog::Transport::Shallow) {
        const float wave = setting("gDeveloperTools.FogreWave", fog::ShallowWaveSpeed, 1.0f, 5.0f, 160.0f);
        const float values[]{wave * wave / fog::RestDensity,
                             setting("gDeveloperTools.FogreDamping", fog::ShallowDamping, 0.01f, 0.0f, 10.0f),
                             setting("gDeveloperTools.FogreRecovery", fog::ShallowRecovery, 0.01f, 0.0f, 5.0f),
                             setting("gDeveloperTools.FogreBladeGain", fog::ShallowBladeGain, 0.01f, 0.0f, 4.0f),
                             setting("gDeveloperTools.FogreBladeCells", fog::ShallowBladeCells, 0.1f, 0.0f, 4.0f),
                             setting("gDeveloperTools.FogreBladeRadial", fog::ShallowBladeRadial, 0.01f, 0.0f, 4.0f),
                             setting("gDeveloperTools.FogreAttackCap", fog::ShallowAttackCap, 1.0f, 1.0f, 180.0f),
                             setting("gDeveloperTools.FogreConfinement", fog::ShallowConfinement, 0.1f, 0.0f, 12.0f),
                             setting("gDeveloperTools.FogreDivDamp", fog::ShallowDivergenceDamping, 0.01f, 0.0f, 0.5f),
                             setting("gDeveloperTools.FogreViscosity", fog::ShallowViscosity, 0.001f, 0.0f, 0.2f)};
        float* const fields[]{&options.gravity, &options.damping, &options.recovery, &options.bladeGain,
                              &options.bladeCells, &options.bladeRadial, &options.attackCap, &options.confinement,
                              &options.divergenceDamping, &options.viscosity};
        bool changed = false;
        for (int i = 0; i < 10; ++i) {
            if (std::abs(*fields[i] - values[i]) > 1e-4f * std::max(1.0f, std::abs(values[i]))) changed = true;
            *fields[i] = values[i];
        }
        if (changed) {
            mod.field.setSolverOptions(options);
            mod.upperField.setSolverOptions(options);
        }
    }
}

// lightSettings contém a mistura ativa (também em interiores); não indexar a lista da cena.
void updateColor(Fog& mod, const PlayState& play) {
    const auto& env = play.envCtx;
    fogcolor::Environment sample;
    for (unsigned i = 0; i < 3; ++i) {
        sample.ambient[i] = float(env.lightSettings.ambientColor[i]) + float(env.adjAmbientColor[i]);
        sample.directional[i] = float(env.lightSettings.light1Color[i]) + float(env.adjLight1Color[i]);
        sample.fog[i] = float(env.lightSettings.fogColor[i]) + float(env.adjFogColor[i]);
    }
    mod.sceneLight = sample;
    mod.sceneColor.update(sample, reinterpret_cast<std::uintptr_t>(&play), play.sceneNum, play.gameplayFrames,
                          play.pauseCtx.state != 0 || play.pauseCtx.debugState != 0, mod.preferSceneColor);
}

fog::Point point(Vec3f v) { return {v.x, v.y, v.z}; }

bool finite(fog::Point p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

uint32_t flowFeatures(const Fog& mod) {
    return mod.flow && mod.flow->size >= offsetof(ShipOotFogV2, features) + sizeof(mod.flow->features)
               ? mod.flow->features : 0u;
}

template <class Action>
bool affectFields(Fog& mod, Action action) {
    bool affected = action(mod.field);
    if (mod.stackedTerrain) affected |= action(mod.upperField);
    return affected;
}

bool cpuFieldActive(const Fog& mod) {
    return !mod.preferCompute && (flowFeatures(mod) & LINKSPAN_OOT_FOG_FLOW_CPU_FIELD);
}

void applyTransport(Fog& mod) {
#ifdef FOGREMASTER_FLUID
    // GFX-UNIFY-001: variante de prova com advecção semi-lagrangiana e projeção de pressão.
    const auto wanted = fog::Transport::SemiLagrangian;
#else
    // 0.28.0: quando o host desenha o campo da CPU, a física é do mod (camada rasa com pressão). O compute do host
    // segue o transporte antigo e continua disponível para comparar.
    const auto wanted = cpuFieldActive(mod) ? fog::Transport::Shallow : fog::Transport::Legacy;
#endif
    if (mod.field.solverOptions().transport == wanted) return;
    fog::SolverOptions options = wanted == fog::Transport::Shallow ? fog::shallowOptions() : fog::SolverOptions{};
    options.transport = wanted;
    mod.field.setSolverOptions(options);
    mod.upperField.setSolverOptions(options);
}

void resetFields(Fog& mod, float x, float z, float spacing) {
    applyTransport(mod);
    mod.field.reset(x, z, spacing);
    if (mod.stackedTerrain) mod.upperField.reset(x, z, spacing);
    // O fluido NÃO reinicia aqui: o campo recém-zerado ainda não tem terreno (células fechadas) e o sampler
    // sólido o leria como parede. Quem chama reinicia com stableReset depois de sampleFloor.
    mod.stableReady = false;
}

void sampleFloor(Fog& mod, PlayState& play, int dx, int dz, bool full) {
    const bool kokiri = play.sceneNum == SCENE_KOKIRI_FOREST;
    // Sample the village against a stable world height while Link crosses its terraces.
    const float low = kokiri ? -260.0f : mod.referenceFloor - 95.0f;
    const float high = kokiri ? 135.0f : mod.referenceFloor + 50.0f;
    const auto changed = [&](int x, int z) {
        return full || x + dx < 0 || x + dx >= fog::Side || z + dz < 0 || z + dz >= fog::Side;
    };
    // Altura do apoio num ponto do mundo: `top` é o que o andar de cima enxerga, `base` o chão da camada de baixo.
    struct Probe { float top{}, base{}; bool open{}; };
    const auto probe = [&](float worldX, float worldZ, bool count) {
        Vec3f query{worldX, high, worldZ};
        CollisionPoly* poly{};
        float y = mod.floorRay(&play.colCtx, &poly, &query);
        bool open = poly != nullptr && std::isfinite(y) && y > low && y <= high;
        float waterY = low;
        bool waterOnTop = false;
        if (mod.waterSurface) {
            WaterBox* box{};
            float surface = query.y;
            if (mod.waterSurface(&play, &play.colCtx, query.x, query.z, &surface, &box)) {
                waterY = surface;
                if (kokiri && surface > low && surface <= high && (!open || surface > y)) {
                    // 0.29.0: sem névoa sobre a água da vila. Densa, ela enchia o canal do riacho e virava uma
                    // placa branca de borda reta, cortada pela beirada do canal.
                    y = surface;
                    open = false;
                    waterOnTop = true;
                    if (count) ++mod.water;
                } else if (!kokiri && (!poly || surface > y + 24.0f) &&
                           surface < mod.referenceFloor + fog::LayerHeight) {
                    open = false;
                    if (count) ++mod.water;
                }
            }
        }
        Probe result{y, y, open};
        if (mod.stackedTerrain && open && !waterOnTop && y - fog::LayerHeight > low) {
            query.y = y - fog::LayerHeight;
            CollisionPoly* lowerPoly{};
            const float floor = mod.floorRay(&play.colCtx, &lowerPoly, &query);
            const float lower = std::max(floor, waterY);
            if ((lowerPoly || waterY > low) && std::isfinite(lower) && lower > low && lower <= query.y) {
                result.base = lower;
            }
        }
        return result;
    };
    // Beirada de degrau: a célula que fica mais alta que uma vizinha aberta por mais que CliffSlope x espaçamento
    // não recebe névoa. O host interpola o piso entre células; sem isso a névoa sobe o barranco como uma rampa
    // fina e brilhante. Assim ela esmaece ao pé do paredão e na borda do terraço. A regra só depende da posição
    // no mundo, então vale igual depois de a grade andar.
    const float spacing = mod.field.spacing();
    const float cliffDrop = CliffSlope * spacing;
    // Oito vizinhas: com só quatro, um barranco na diagonal da grade deixava a névoa do terraço pendurada no ar.
    constexpr float sides[8][2] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f},
                                   {1.0f, 1.0f}, {-1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, -1.0f}};
    for (int z = 0; z < fog::Side; ++z) {
        for (int x = 0; x < fog::Side; ++x) {
            if (!changed(x, z)) continue;
            const float worldX = mod.field.worldX(x), worldZ = mod.field.worldZ(z);
            const Probe here = probe(worldX, worldZ, true);
            if (mod.stackedTerrain) mod.upperField.terrain(x, z, here.top, here.open, !full && !kokiri);
            bool open = here.open;
            for (int side = 0; open && side < 8; ++side) {
                const Probe next = probe(worldX + sides[side][0] * spacing, worldZ + sides[side][1] * spacing, false);
                if (next.open && here.base - next.base > cliffDrop) {
                    open = false;
                    ++mod.rims;
                }
            }
            mod.field.terrain(x, z, here.base, open, !full && !kokiri);
        }
    }
    if (!mod.wallLine) return;
    const auto test = [&](fog::Field& field, int x, int z, int otherX, int otherZ) {
        if (!changed(x, z) && !changed(otherX, otherZ)) return;
        const auto& a = field.cell(x, z);
        const auto& b = field.cell(otherX, otherZ);
        bool blocked = false;
        if (fog::Field::connected(a, b)) {
            Vec3f from{field.worldX(x), a.floor + 16.0f, field.worldZ(z)};
            Vec3f to{field.worldX(otherX), b.floor + 16.0f, field.worldZ(otherZ)};
            Vec3f hit{};
            CollisionPoly* poly{};
            int32_t bgId{};
            blocked = mod.wallLine(&play.colCtx, &from, &to, &hit, &poly, true, false, false, false, &bgId);
            if (blocked) ++mod.walls;
        }
        field.blockEdge(x, z, otherX, otherZ, blocked);
    };
    const auto sampleEdges = [&](fog::Field& field) {
        for (int z = 0; z < fog::Side; ++z) {
            for (int x = 0; x < fog::Side; ++x) {
                if (x + 1 < fog::Side) test(field, x, z, x + 1, z);
                if (z + 1 < fog::Side) test(field, x, z, x, z + 1);
            }
        }
    };
    sampleEdges(mod.field);
    if (mod.stackedTerrain) sampleEdges(mod.upperField);
}

float playerFloor(const Actor& actor, float positionY) {
    return actor.floorPoly && std::isfinite(actor.floorHeight) && std::abs(actor.floorHeight - positionY) < 95.0f
               ? actor.floorHeight
               : positionY;
}

// Centro que a grade persegue: sempre o Link. Nada aqui pode depender da câmera.
fog::Point domainTarget(const PlayState&, fog::Point p, float) { return p; }

// Chão da vila num ponto do mundo, para a grade de desenho: o mesmo critério da amostragem da simulação em Kokiri
// (sem névoa sobre a água; debaixo de ponte e plataforma vale o chão de baixo).
VillageCell villageGround(Fog& mod, PlayState& play, float worldX, float worldZ) {
    constexpr float low = -260.0f, high = 135.0f;
    Vec3f query{worldX, high, worldZ};
    CollisionPoly* poly{};
    float y = mod.floorRay(&play.colCtx, &poly, &query);
    const bool open = poly != nullptr && std::isfinite(y) && y > low && y <= high;
    float waterY = low;
    if (mod.waterSurface) {
        WaterBox* box{};
        float surface = query.y;
        if (mod.waterSurface(&play, &play.colCtx, query.x, query.z, &surface, &box)) {
            waterY = surface;
            if (surface > low && surface <= high && (!open || surface > y)) return {surface, false};
        }
    }
    if (open && y - fog::LayerHeight > low) {
        query.y = y - fog::LayerHeight;
        CollisionPoly* lowerPoly{};
        const float floor = mod.floorRay(&play.colCtx, &lowerPoly, &query);
        const float lower = std::max(floor, waterY);
        if ((lowerPoly || waterY > low) && std::isfinite(lower) && lower > low && lower <= query.y) y = lower;
    }
    return {open ? y : 0.0f, open};
}

void sampleVillage(Fog& mod, PlayState& play, int dx, int dz, bool full) {
    auto& village = mod.village;
    constexpr float drop = CliffSlope * VillageSpacing;
    constexpr float sides[8][2] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f},
                                   {1.0f, 1.0f}, {-1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, -1.0f}};
    for (int z = 0; z < VillageSide; ++z) {
        for (int x = 0; x < VillageSide; ++x) {
            if (!full && x + dx >= 0 && x + dx < VillageSide && z + dz >= 0 && z + dz < VillageSide) continue;
            const float worldX = village.worldX(x), worldZ = village.worldZ(z);
            VillageCell cell = villageGround(mod, play, worldX, worldZ);
            if (cell.open) {
                VillageCell around[8];
                float lower[8];
                int lowerCount = 0;
                for (int side = 0; side < 8; ++side) {
                    around[side] = villageGround(mod, play, worldX + sides[side][0] * VillageSpacing,
                                                 worldZ + sides[side][1] * VillageSpacing);
                    if (around[side].open && cell.floor - around[side].floor > SpikeRise)
                        lower[lowerCount++] = around[side].floor;
                }
                // Pico isolado (pedra, placa, cerca, tábua de ponte): a amostra caiu em cima de um objeto e quase
                // toda a volta está mais baixa. Vale o chão da volta, senão a névoa sobe num ponto só ou a célula
                // fecha e abre um furo.
                if (lowerCount >= 6) {
                    std::sort(lower, lower + lowerCount);
                    cell.floor = lower[lowerCount / 2];
                }
                for (int side = 0; side < 8; ++side) {
                    if (around[side].open && cell.floor - around[side].floor > drop) cell.open = false;
                }
                cell.cliff = !cell.open;
            }
            village.cells[z * VillageSide + x] = cell;
        }
    }
}

// A grade de desenho fica centrada no Link, presa a múltiplos do espaçamento: uma célula corresponde sempre ao
// mesmo ponto do mundo, então andar só acrescenta e tira fileiras na borda, fora do raio em que há névoa.
void followVillage(Fog& mod, PlayState& play, fog::Point p) {
    auto& village = mod.village;
    const float half = 0.5f * (VillageSide - 1) * VillageSpacing;
    // Retém o centro discreto atual perto do meio passo; arredondar p a cada tick alternava a origem em Z=480.
    const float x0 = village.valid
                         ? village.x0 + followShiftAxis(p.x - (village.x0 + half), VillageSpacing, 0.0f,
                                                       FollowHysteresisCells) * VillageSpacing
                         : std::round((p.x - half) / VillageSpacing) * VillageSpacing;
    const float z0 = village.valid
                         ? village.z0 + followShiftAxis(p.z - (village.z0 + half), VillageSpacing, 0.0f,
                                                       FollowHysteresisCells) * VillageSpacing
                         : std::round((p.z - half) / VillageSpacing) * VillageSpacing;
    if (village.valid && x0 == village.x0 && z0 == village.z0) return;
    const int dx = village.valid ? static_cast<int>(std::lround((x0 - village.x0) / VillageSpacing)) : VillageSide;
    const int dz = village.valid ? static_cast<int>(std::lround((z0 - village.z0) / VillageSpacing)) : VillageSide;
    const bool full = std::abs(dx) >= VillageSide || std::abs(dz) >= VillageSide;
    if (!full) {
        const auto previous = village.cells;
        for (int z = std::max(0, -dz); z < std::min(VillageSide, VillageSide - dz); ++z) {
            for (int x = std::max(0, -dx); x < std::min(VillageSide, VillageSide - dx); ++x) {
                village.cells[z * VillageSide + x] = previous[(z + dz) * VillageSide + x + dx];
            }
        }
    }
    village.x0 = x0;
    village.z0 = z0;
    village.valid = true;
    sampleVillage(mod, play, dx, dz, full);
}

// Entrega ao host a grade de desenho da vila. Devolve false se o host não aceitar; o chamador cai no caminho antigo.
bool submitVillage(Fog& mod, PlayState* play, const fogcolor::Rgb& color, float density) {
    const auto& village = mod.village;
    const auto features = flowFeatures(mod);
    if (!village.valid || !cpuFieldActive(mod) || !(features & LINKSPAN_OOT_FOG_FLOW_VORTICES) || !mod.flow ||
        !mod.flow->submit_flow)
        return false;
    constexpr int count = VillageSide * VillageSide;
    // Com a grade fina (RFC 0034) o host não soma a deriva fixa dele: a velocidade enviada é a da superfície,
    // com o vento da cena. Sem ela o host continua somando a deriva antiga e o vento não entra aqui.
    const bool sendDetail =
        (features & LINKSPAN_OOT_FOG_FLOW_DETAIL_GRID) && windSetting(mod, DetailSetting, 1) != 0;
    const float ambientX = sendDetail ? mod.windX : 0.0f, ambientZ = sendDetail ? mod.windZ : 0.0f;
    const float densityMax = sendDetail ? fog::MaxDensity : fog::WireDensity;
    static ShipOotFogCellV1 cells[count];
    static ShipOotFogFlowCellV2 flowCells[count];
    static ShipOotFogCellV1 canopyCells[count];
    const float canopyDensity = (features & LINKSPAN_OOT_FOG_FLOW_STACKED_SUPPORTS)
                                    ? CanopyDensityAt100 * 0.01f * std::clamp(windSetting(mod, CanopySetting, 0), 0, 250)
                                    : 0.0f;
    const float canopyMinClearance = std::max(CanopyGroundClearance, 0.5f * mod.layerHeight);
    unsigned canopyCount = 0;
    // Névoa que sobe (0.32.0): a simulação levanta parte da coluna no golpe, no pouso e nas explosões
    // (raisedDensity), mas a vila não mandava isso ao host e a névoa só escorria no chão. A fração levantada vai na
    // grade de desenho, lida da simulação perto do Link como o desvio de densidade.
    const bool useLift = (features & LINKSPAN_OOT_FOG_FLOW_LIFT) && mod.liftPercent != 0;
    static float raised[count];
    const auto& field = mod.field;
    const float spacing = field.spacing();
    const ShipOotFogImpulseV2 still{1.0f, 0.0f, 0.0f, -fog::SpeedLimit, fog::SpeedLimit, -fog::SpeedLimit,
                                    fog::SpeedLimit};
    const auto smooth = [](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    // FogreCliffFit: 0 = máscara de 80 u (0.30.0); 1 = a grade fina fecha só onde a névoa ficaria pendurada.
    // Testado e descartado (14/11): pôr a célula de beirada no piso desenhado dentro da grade fina. Muitas dessas
    // células caem em telhado ou toco, puxam o piso para cima e a névoa em volta some.
    const bool cliffFit = sendDetail && windSetting(mod, CliffFitSetting, 1) != 0;
    for (int z = 0; z < VillageSide; ++z) {
        for (int x = 0; x < VillageSide; ++x) {
            const int i = z * VillageSide + x;
            const auto& ground = village.cells[i];
            const float worldX = village.worldX(x), worldZ = village.worldZ(z);
            const float distance = std::hypot(worldX - mod.linkX, worldZ - mod.linkZ);
            float value = 0.0f, vx = ambientX, vz = ambientZ, lifted = 0.0f;
            if (ground.open) {
                // Repouso: média de 3x3 amostras na área da célula. A segunda oitava dos bancos é mais fina que a
                // grade de desenho e, amostrada num ponto só, vira risco reto alinhado aos eixos.
                for (int tap = 0; tap < 9; ++tap) {
                    value += field.restWorld(worldX + (tap % 3 - 1) * (VillageSpacing / 3.0f),
                                             worldZ + (tap / 3 - 1) * (VillageSpacing / 3.0f)) / 9.0f;
                }
                // Perto do Link soma-se o desvio simulado em relação ao repouso (o vão do golpe, o que ele empurrou),
                // lido no ponto: é ele que tem de chegar nítido, e já é largo o bastante para a grade de desenho.
                const float near = smooth((KokiriFadeOuter - distance) / (KokiriFadeOuter - KokiriFadeInner));
                const float fx = (worldX - field.worldX(0)) / spacing, fz = (worldZ - field.worldZ(0)) / spacing;
                if (near > 0.0f && fx >= 0.0f && fz >= 0.0f && fx <= fog::Side - 1 && fz <= fog::Side - 1) {
                    const int ix = std::min(static_cast<int>(fx), fog::Side - 2);
                    const int iz = std::min(static_cast<int>(fz), fog::Side - 2);
                    const float tx = fx - ix, tz = fz - iz;
                    float deviation = 0.0f, sx = 0.0f, sz = 0.0f;
                    for (int corner = 0; corner < 4; ++corner) {
                        const int cx = ix + (corner & 1), cz = iz + (corner >> 1);
                        const auto& cell = field.cell(cx, cz);
                        const float weight = (corner & 1 ? tx : 1.0f - tx) * (corner >> 1 ? tz : 1.0f - tz);
                        if (cell.open) {
                            deviation += weight * (cell.density - field.rest(cx, cz));
                            lifted += weight * cell.raisedDensity / std::max(cell.density, 0.0001f);
                        }
                        sx += weight * cell.vx;
                        sz += weight * cell.vz;
                    }
                    lifted = std::clamp(lifted * near, 0.0f, LINKSPAN_OOT_FOG_MAX_RAISED_FRACTION);
                    // O host recusa a grade inteira se uma célula passar do teto: 1,0 sem a grade fina, 1,5 com ela
                    // (RFC 0034), que é a folga da borda do golpe num banco cheio.
                    value = std::clamp(value + deviation * near, 0.0f, densityMax);
                    vx = ambientX + sx * near;
                    vz = ambientZ + sz * near;
                }
                value *= smooth((VillageFadeOuter - distance) / (VillageFadeOuter - VillageFadeInner));
            }
            cells[i] = {ground.floor, value};
            if (canopyDensity > 0.0f) {
                const float dx = worldX - CanopyCenterX, dz = worldZ - CanopyCenterZ;
                const float nx = std::clamp(dx / CanopyRadiusX, -1.0f, 1.0f);
                const float nz = std::clamp(dz / CanopyRadiusZ, -1.0f, 1.0f);
                // Crest arredondado sobre uma inclinação suave, sem uma cota constante na direção da câmera.
                const float crown = std::max(0.0f, 1.0f - nx * nx - nz * nz);
                const float upperFloor = CanopyBase + CanopySlopeX * nx + CanopySlopeZ * nz + CanopyCrown * crown * crown;
                float upper = 0.0f;
                if (ground.open && ground.floor <= upperFloor - canopyMinClearance) {
                    const float maskX = smooth((CanopyRadiusX - std::abs(dx)) / CanopyFeatherX);
                    const float maskZ = smooth((CanopyRadiusZ - std::abs(dz)) / CanopyFeatherZ);
                    upper = canopyDensity * maskX * maskZ;
                }
                canopyCells[i] = {upperFloor, upper};
                canopyCount += upper > 0.0f;
            }
            raised[i] = value > 0.0f ? lifted : 0.0f;
            if (useLift) mod.peakLift = std::max(mod.peakLift, raised[i]);
            flowCells[i] = {std::clamp(vx, -180.0f, 180.0f), std::clamp(vz, -180.0f, 180.0f), uint32_t(ground.open),
                            still, still};
        }
    }
    const ShipOotFogGridV1 grid{sizeof(grid), VillageSide, VillageSide,
                                village.worldX(0), village.worldX(VillageSide - 1),
                                village.worldZ(0), village.worldZ(VillageSide - 1),
                                mod.layerHeight, mod.falloff, fog::volumeExtinction(mod.extinction, density), KokiriEdgeFeather,
                                {color[0], color[1], color[2]}, cells};
    const auto flags = LINKSPAN_OOT_FOG_FLOW_VORTICES | LINKSPAN_OOT_FOG_FLOW_CPU_FIELD |
                       (mod.preferParticles ? LINKSPAN_OOT_FOG_FLOW_PARTICLES : 0u) |
                       (features & LINKSPAN_OOT_FOG_FLOW_ATMOSPHERE) |
                       (useLift ? LINKSPAN_OOT_FOG_FLOW_LIFT : 0u) |
                       (canopyCount != 0 ? LINKSPAN_OOT_FOG_FLOW_STACKED_SUPPORTS : 0u) |
                       (mod.preferLighting ? features & LINKSPAN_OOT_FOG_FLOW_LOCAL_LIGHTS : 0u);
    FogFlowWithDetail flow{};
    static_cast<ShipOotFogFlowV2&>(flow) = {sizeof(ShipOotFogFlowV2), mod.generation, mod.flowTick,
                                            fog::StepSeconds, flowCells, flags,
                                            nullptr, useLift ? raised : nullptr,
                                            canopyCount != 0 ? canopyCells : nullptr, nullptr, nullptr};
    // Grade fina: a própria grade da simulação, com o repouso no ponto mais o desvio simulado. Onde a grade larga
    // está fechada (parede, borda de barranco) a fina também fecha, senão a névoa voltaria a escorrer no degrau.
    static ShipOotFogDetailCellV1 fine[fog::Side * fog::Side];
    static float fineOpen[fog::Side * fog::Side]; // 0.33.0: máscara de abertura da grade de 25, para o fluido fino
    ShipOotFogDetailV1 detail{};
    if (sendDetail) {
        for (int z = 0; z < fog::Side; ++z) {
            for (int x = 0; x < fog::Side; ++x) {
                const auto& cell = field.cell(x, z);
                const float worldX = field.worldX(x), worldZ = field.worldZ(z);
                const float gx = std::clamp((worldX - village.worldX(0)) / VillageSpacing, 0.0f, VillageSide - 1.001f);
                const float gz = std::clamp((worldZ - village.worldZ(0)) / VillageSpacing, 0.0f, VillageSide - 1.001f);
                const int ix = static_cast<int>(gx), iz = static_cast<int>(gz);
                const float tx = gx - ix, tz = gz - iz;
                float open = 0.0f, fitted = 0.0f, weightSum = 0.0f, floorSum = 0.0f;
                for (int corner = 0; corner < 4; ++corner) {
                    const float weight = (corner & 1 ? tx : 1.0f - tx) * (corner >> 1 ? tz : 1.0f - tz);
                    const int index = (iz + (corner >> 1)) * VillageSide + ix + (corner & 1);
                    const auto& ground = village.cells[index];
                    if (ground.open) open += weight;
                    if (ground.open || ground.cliff) fitted += weight;
                    weightSum += weight * cells[index].density;
                    floorSum += weight * cells[index].density * cells[index].floor_y;
                }
                // Beirada de barranco (0.30.1): a máscara de 80 u tirava a névoa de uma faixa de 80-150 u no topo.
                // O host desenha o piso como média bilinear pesada pela densidade da grade de desenho, então a
                // célula fina só fecha onde esse piso fica acima do chão real dela, ou seja, onde a névoa ficaria
                // pendurada no ar. Piso abaixo do chão só enterra a névoa, que o próprio terreno esconde.
                const float reach =
                    cliffFit ? smooth((CliffFitOuter - std::hypot(worldX - mod.linkX, worldZ - mod.linkZ)) /
                                      (CliffFitOuter - CliffFitInner))
                             : 0.0f;
                if (reach > 0.0f) {
                    const bool groundKnown = std::isfinite(cell.floor) && cell.floor > -255.0f;
                    if (weightSum < 0.01f || !groundKnown) {
                        fitted = 0.0f;
                    } else {
                        const float hanging = floorSum / weightSum - cell.floor;
                        fitted *= 1.0f - smooth((hanging - FloatAllowed) / (FloatClosed - FloatAllowed));
                    }
                    open += (fitted - open) * reach;
                }
                // Célula fechada na simulação não zera a névoa: fica só o repouso, como na grade larga. Zerar abria
                // buracos de borda serrilhada (40 u) junto de casas e barrancos; quem fecha de verdade é `open`.
                float value = field.restWorld(worldX, worldZ);
                if (cell.open) value += cell.density - field.rest(x, z);
                fineOpen[z * fog::Side + x] = smooth(open);
                fine[z * fog::Side + x] = {std::clamp(value * smooth(open), 0.0f, fog::MaxDensity),
                                           std::clamp(ambientX + cell.vx, -180.0f, 180.0f),
                                           std::clamp(ambientZ + cell.vz, -180.0f, 180.0f)};
            }
        }
        detail = {sizeof(detail), fog::Side, fog::Side, field.worldX(0), field.worldX(fog::Side - 1),
                  field.worldZ(0), field.worldZ(fog::Side - 1), 2.0f * spacing, fine};
        // 0.33.0: com o Stable Fluids, a grade fina passa a ser o fluido no lado escolhido. A máscara de abertura
        // (barranco, água, parede) é a mesma da grade de 25, lida bilinear no ponto de cada célula fina.
        static ShipOotFogDetailCellV1 stableCells[StableMaxSide * StableMaxSide];
        if (mod.stableReady && (features & LINKSPAN_OOT_FOG_FLOW_DETAIL_GRID) &&
            mod.stable.size() <= hostDetailMaxSide(mod)) {
            const auto openAt = [&](float wx, float wz) {
                const float gx = std::clamp((wx - field.worldX(0)) / spacing, 0.0f, fog::Side - 1.001f);
                const float gz = std::clamp((wz - field.worldZ(0)) / spacing, 0.0f, fog::Side - 1.001f);
                const int ix = int(gx), iz = int(gz);
                const float tx = gx - ix, tz = gz - iz;
                float out = 0.0f;
                for (int corner = 0; corner < 4; ++corner) {
                    const float w = (corner & 1 ? tx : 1.0f - tx) * (corner >> 1 ? tz : 1.0f - tz);
                    const int cx = ix + (corner & 1), cz = iz + (corner >> 1);
                    out += w * fineOpen[cz * fog::Side + cx];
                }
                return out;
            };
            const int n = mod.stable.size();
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    const float wx = mod.stable.cellCenterX(i), wz = mod.stable.cellCenterZ(j);
                    // Sólido na simulação (parede, borda de degrau) não zera a névoa: fica o repouso, e quem fecha
                    // de verdade é a máscara visual (que o ajuste de barranco pode reabrir), como no detalhe antigo.
                    const float source = mod.stable.solid(i, j) ? mod.stable.rest(i, j) : mod.stable.density(i, j);
                    const float value = source * openAt(wx, wz);
                    stableCells[j * n + i] = {std::clamp(value, 0.0f, fog::MaxDensity),
                                              std::clamp(mod.stable.velocityX(i, j), -180.0f, 180.0f),
                                              std::clamp(mod.stable.velocityZ(i, j), -180.0f, 180.0f)};
                }
            }
            const float h = mod.stable.cellSize();
            ShipOotFogDetailV1 fluid{sizeof(fluid), uint32_t(n), uint32_t(n), mod.stable.cellCenterX(0),
                                     mod.stable.cellCenterX(n - 1), mod.stable.cellCenterZ(0),
                                     mod.stable.cellCenterZ(n - 1), 6.0f * h, stableCells};
            if (fluid.x_min >= grid.x_min && fluid.x_max <= grid.x_max && fluid.z_min >= grid.z_min &&
                fluid.z_max <= grid.z_max)
                detail = fluid;
        }
        if (detail.x_min >= grid.x_min && detail.x_max <= grid.x_max && detail.z_min >= grid.z_min &&
            detail.z_max <= grid.z_max) {
            flow.size = sizeof(flow);
            flow.flags |= LINKSPAN_OOT_FOG_FLOW_DETAIL_GRID;
            flow.detail = &detail;
        }
    }
    if (mod.flow->submit_flow(play, &grid, &flow) != SHIP_NATIVE_OK) return false;
    mod.decks = canopyCount;
    return true;
}

void followPlayer(Fog& mod, PlayState& play, const Actor& player) {
    const auto p = point(player.world.pos);
    if (!finite(p)) return;
    const auto target = domainTarget(play, p, mod.field.spacing());
    const float offsetX = target.x - mod.field.worldX(fog::Side / 2);
    const float offsetZ = target.z - mod.field.worldZ(fog::Side / 2);
    const bool kokiri = play.sceneNum == SCENE_KOKIRI_FOREST;
    const float y = playerFloor(player, p.y);
    const float spacing = mod.field.spacing();
    const int shiftCells = kokiri ? KokiriFollowCells : 6;
    const float offset = std::max(std::abs(offsetX), std::abs(offsetZ));
    const bool shift = offset >= shiftCells * spacing;
    const bool changeFloor = !kokiri && (player.bgCheckFlags & BGCHECKFLAG_GROUND) &&
                             std::abs(y - mod.referenceFloor) > 60.0f;
    if (!shift && !changeFloor) return;
    const bool reset = offset >= fog::Side * spacing;
    if (reset) {
        resetFields(mod, target.x, target.z, spacing);
        mod.referenceFloor = y;
        sampleFloor(mod, play, 0, 0, true);
        stableReset(mod);
        mod.weaponActive = false; // a pose anterior da arma não vale depois do salto
        mod.appearance = 0.0f;
    } else {
        const auto shiftAxis = [spacing, kokiri](float distance) {
            const float overlap = kokiri ? KokiriFollowOverlap : 3.0f;
            return followShiftAxis(distance, spacing, overlap, kokiri ? FollowHysteresisCells : 0.0f);
        };
        const int dx = shift ? shiftAxis(offsetX) : 0;
        const int dz = shift ? shiftAxis(offsetZ) : 0;
        if (shift) {
            mod.field.shift(dx, dz);
            if (mod.stackedTerrain) mod.upperField.shift(dx, dz);
        }
        if (changeFloor) mod.referenceFloor = y;
        sampleFloor(mod, play, dx, dz, changeFloor);
    }
    if (reset || changeFloor || !(flowFeatures(mod) & LINKSPAN_OOT_FOG_FLOW_DOMAIN_SHIFT)) ++mod.generation;
    ++mod.gridShifts;
}

bool displaceBody(Fog& mod, const Actor& actor, fog::Point p, float radius, float height) {
    const fog::Point current = point(actor.world.pos);
    const fog::Point old = point(actor.prevPos);
    if (!finite(p) || !finite(current) || !finite(old) || radius <= 0.0f || height <= 0.0f) return false;
    const float dx = current.x - old.x, dz = current.z - old.z, dy = current.y - old.y;
    const float distance = std::hypot(dx, dz);
    if (distance > 120.0f || std::abs(dy) > 120.0f) return false;
    // The depth buffer already occludes stationary bodies. Let their wakes refill when they stop.
    const float motion = std::clamp((distance + 0.5f * std::abs(dy) - 0.30f) / 4.7f, 0.0f, 1.0f);
    if (motion <= 0.0f) return false;
    const float strength = motion * motion * (3.0f - 2.0f * motion);
    stableStir(mod, {p.x - dx, p.y - dy, p.z - dz}, p, dx / fog::StepSeconds, dz / fog::StepSeconds,
               std::clamp(radius + 12.0f, 20.0f, 80.0f), strength * stableStirGain(mod), 0.0f, height);
    return affectFields(mod, [&](fog::Field& field) {
        return field.disturb({p.x - dx, p.y - dy, p.z - dz}, p, std::clamp(radius + 25.0f, 36.0f, 112.0f),
                             0.0f, height, {dx * 9.0f, 0.0f, dz * 9.0f}, std::min(0.3f + distance * 0.045f, 0.85f),
                             0.0f, 0.0f, strength);
    });
}

void displaceActors(Fog& mod, const PlayState& play, const Player& player) {
    const auto& ctx = play.colChkCtx;
    const int count = std::clamp(ctx.colOCCount, 0, static_cast<int>(std::size(ctx.colOC)));
    bool affected = false;
    for (int i = 0; i < count; ++i) {
        const Collider* collider = ctx.colOC[i];
        if (!collider || !collider->actor || collider->actor == &player.actor || !(collider->ocFlags1 & OC1_ON))
            continue;
        if (collider->shape == COLSHAPE_CYLINDER) {
            const auto& dim = reinterpret_cast<const ColliderCylinder*>(collider)->dim;
            affected |= displaceBody(mod, *collider->actor,
                                      {float(dim.pos.x), float(dim.pos.y) + dim.yShift, float(dim.pos.z)},
                                      float(dim.radius), float(dim.height));
        } else if (collider->shape == COLSHAPE_JNTSPH) {
            const auto& spheres = *reinterpret_cast<const ColliderJntSph*>(collider);
            if (!spheres.elements) continue;
            for (int s = 0; s < std::min(spheres.count, 12); ++s) {
                const auto& sphere = spheres.elements[s].dim.worldSphere;
                affected |= displaceBody(mod, *collider->actor,
                                          {float(sphere.center.x), float(sphere.center.y) - sphere.radius,
                                           float(sphere.center.z)},
                                          float(sphere.radius), float(sphere.radius) * 2.0f);
            }
        }
    }
    if (affected) ++mod.actorTicks;
}

bool attackVolume(Fog& mod, const Actor& actor, fog::Point center, float radius, float bottom, float top) {
    const auto p = point(actor.world.pos), prev = point(actor.prevPos);
    if (!finite(center) || !finite(p) || !finite(prev) || radius <= 0.0f || top <= bottom) return false;
    const float dx = p.x - prev.x, dy = p.y - prev.y, dz = p.z - prev.z;
    if (std::hypot(dx, dy, dz) > 240.0f) return false;
    const float strength = mod.attackPercent * 0.01f;
    if (actor.id == ACTOR_EN_BOM || actor.id == ACTOR_EN_BOMBF || actor.id == ACTOR_MAGIC_FIRE) {
        const float reach = std::clamp(radius + 48.0f, 64.0f, 240.0f);
        const bool affected = affectFields(mod, [&](fog::Field& field) {
            return field.disturb(center, center, reach, bottom - 18.0f, top, {}, 0.0f, 0.58f, 145.0f,
                                 strength, mod.attackTicks);
        });
        if (affected) ++mod.blasts;
        return affected;
    }
    return affectFields(mod, [&](fog::Field& field) {
        return field.disturb({center.x - dx, center.y - dy, center.z - dz}, center,
                             std::clamp(radius + 34.0f, 42.0f, 200.0f), bottom - 10.0f, top,
                             {dx * 5.0f, dy * 2.1f, dz * 5.0f}, 0.0f, 0.42f, 0.0f, strength, mod.attackTicks);
    });
}

bool attackQuad(Fog& mod, const ColliderQuad& collider) {
    // O OoT triangula o quad em [2,3,1] e [1,0,2]. As arestas opostas 2→3 e 0→1
    // são as poses velha e nova para os quads produzidos pela trilha da arma.
    const auto& v = collider.dim.quad;
    const auto a = point(v[0]), b = point(v[1]), c = point(v[2]), d = point(v[3]);
    if (!finite(a) || !finite(b) || !finite(c) || !finite(d)) return false;
    const auto span = [](fog::Point x, fog::Point y) { return std::hypot(x.x - y.x, x.y - y.y, x.z - y.z); };
    if (span(a, b) > 240.0f || span(a, c) > 240.0f || span(b, d) > 240.0f || span(c, d) > 240.0f)
        return false;
    const bool projectile = collider.base.actor->id == ACTOR_EN_ARROW;
    const float strength = mod.attackPercent * 0.01f;
    stableBlade(mod, c, d, a, b, projectile ? 14.0f : 18.0f);
    return affectFields(mod, [&](fog::Field& field) {
        if (field.blade(c, d, a, b, projectile ? 60.0f : 48.0f, strength, mod.attackTicks)) return true;
        // Quads estreitos de projéteis podem ter ponta e base quase coincidentes.
        const bool base = field.disturb(c, a, 43.0f, -26.0f, 12.0f,
                                        {(a.x - c.x) * 5.0f, (a.y - c.y) * 2.1f, (a.z - c.z) * 5.0f},
                                        0.0f, 0.35f, 0.0f, strength, mod.attackTicks);
        const bool tip = field.disturb(d, b, 43.0f, -26.0f, 12.0f,
                                       {(b.x - d.x) * 5.0f, (b.y - d.y) * 2.1f, (b.z - d.z) * 5.0f},
                                       0.0f, 0.35f, 0.0f, strength, mod.attackTicks);
        return base || tip;
    });
}

ShipNativeStatus SHIP_NATIVE_CALL attacks(void* user, const ShipNativeHookCall* call) {
    auto& mod = *static_cast<Fog*>(user);
    auto* play = static_cast<PlayState*>(static_cast<const ShipOotPlayHookV1*>(call->payload)->play_state);
    if (!mod.enabled || !mod.scene || mod.scene != play || mod.room != play->roomCtx.curRoom.num ||
        play->pauseCtx.state != 0 ||
        mod.lastAttackFrame == play->gameplayFrames)
        return SHIP_NATIVE_OK;
    const auto* player = static_cast<const Player*>(mod.engine->get_player());
    if (!player) return SHIP_NATIVE_OK;
    mod.lastAttackFrame = play->gameplayFrames;
    followPlayer(mod, *play, player->actor);
    if (play->sceneNum == SCENE_KOKIRI_FOREST) followVillage(mod, *play, point(player->actor.world.pos));
    const auto& ctx = play->colChkCtx;
    const int count = std::clamp<int>(ctx.colATCount, 0, static_cast<int>(std::size(ctx.colAT)));
    int budget = 48;
    // A lista inclui quads emitidos no draw anterior. O Play_Update original
    // vai consumi-la e limpá-la. Ler geometria e mover só a névoa antes disso.
    for (int i = 0; i < count && budget > 0; ++i) {
        const auto* collider = ctx.colAT[i];
        if (!collider || !collider->actor || collider->actor == &player->actor || !(collider->atFlags & AT_ON))
            continue;
        bool affected = false;
        if (collider->shape == COLSHAPE_CYLINDER) {
            const auto& cylinder = *reinterpret_cast<const ColliderCylinder*>(collider);
            if (!(cylinder.info.toucherFlags & TOUCH_ON)) continue;
            --budget;
            const auto& dim = cylinder.dim;
            affected = attackVolume(mod, *collider->actor,
                                    {float(dim.pos.x), float(dim.pos.y) + dim.yShift, float(dim.pos.z)},
                                    float(dim.radius), 0.0f, float(dim.height));
            if (affected) ++mod.cylinders;
        } else if (collider->shape == COLSHAPE_JNTSPH) {
            const auto& spheres = *reinterpret_cast<const ColliderJntSph*>(collider);
            if (!spheres.elements) continue;
            for (int s = 0; s < std::min(spheres.count, 12) && budget > 0; ++s) {
                const auto& element = spheres.elements[s];
                if (!(element.info.toucherFlags & TOUCH_ON)) continue;
                --budget;
                const auto& sphere = element.dim.worldSphere;
                const float radius = float(sphere.radius);
                affected |= attackVolume(mod, *collider->actor,
                                         {float(sphere.center.x), float(sphere.center.y), float(sphere.center.z)},
                                         radius, -radius, radius);
            }
            if (affected) ++mod.spheres;
        } else if (collider->shape == COLSHAPE_QUAD) {
            const auto& quad = *reinterpret_cast<const ColliderQuad*>(collider);
            if (!(quad.info.toucherFlags & TOUCH_ON)) continue;
            --budget;
            affected = attackQuad(mod, quad);
            if (affected) ++mod.quads;
        }
        if (affected && (collider->atFlags & AT_TYPE_ENEMY)) ++mod.enemyAttacks;
    }
    return SHIP_NATIVE_OK;
}

ShipNativeStatus SHIP_NATIVE_CALL update(void* user, const ShipNativeHookCall* call) {
    auto& mod = *static_cast<Fog*>(user);
    const auto& event = *static_cast<const ShipOotPlayHookV1*>(call->payload);
    auto& play = *static_cast<PlayState*>(event.play_state);
    const auto* player = static_cast<const Player*>(mod.engine->get_player());
    const auto* save = static_cast<const SaveContext*>(mod.engine->get_save_context());
    // Antes de qualquer retorno: o relógio da rajada anda em toda sala e com a neblina desligada, como no host.
    mod.windSeconds = mod.windClock.Step(reinterpret_cast<std::uintptr_t>(&play), play.sceneNum, play.gameplayFrames,
                                         play.pauseCtx.state != 0 || play.pauseCtx.debugState != 0,
                                         windSetting(mod, WindFieldSetting, 1) != 0);
    updateColor(mod, play); // Atualizar inclusive em cenas sem névoa e com o efeito desligado.
    const int room = play.roomCtx.curRoom.num;
    const bool supported = play.sceneNum == SCENE_KOKIRI_FOREST || play.sceneNum == SCENE_LOST_WOODS ||
                           (play.sceneNum == SCENE_GRAVEYARD && room == 0) ||
                           (play.sceneNum == SCENE_SHADOW_TEMPLE && room == 2);
    if (!player || !save || save->gameMode != 0 || !supported || (!mod.enabled && mod.appearance <= 0.0f)) {
        mod.scene = nullptr;
        mod.appearance = 0.0f;
        mod.weaponActive = false;
        mod.spinActive = false;
        return SHIP_NATIVE_OK;
    }
    const auto p = point(player->actor.world.pos);
    if (!finite(p)) return SHIP_NATIVE_OK;
    mod.linkX = p.x;
    mod.linkZ = p.z;
    if (mod.scene != &play || mod.room != room) {
        const bool kokiri = play.sceneNum == SCENE_KOKIRI_FOREST;
        const float spacing = kokiri ? KokiriSpacing : fog::Spacing;
        const auto target = domainTarget(play, p, spacing);
        mod.stackedTerrain = kokiri && (flowFeatures(mod) & LINKSPAN_OOT_FOG_FLOW_STACKED_SUPPORTS);
        mod.bankScale = kokiri ? std::max(1.0f, 4.0f * VillageSpacing / fog::BankWavelength) : 1.0f;
        updateLook(mod, false);
        mod.village.valid = false;
        resetFields(mod, target.x, target.z, spacing);
        mod.referenceFloor = playerFloor(player->actor, p.y);
        sampleFloor(mod, play, 0, 0, true);
        stableReset(mod);
        mod.scene = &play;
        mod.room = room;
        mod.weaponActive = false;
        mod.spinActive = false;
        mod.appearance = 0.0f;
        mod.lastAttackFrame = play.gameplayFrames - 1u;
        mod.lastSimulationFrame = play.gameplayFrames - 1u;
        mod.peakLift = 0.0f;
        mod.peakDeckLift = 0.0f;
        mod.decks = 0;
        mod.cutAge = -1; // o campo recomeçou: a medição do golpe anterior não vale mais
        mod.airFrames = 0; // a queda da transição não é pouso na névoa nova
        mod.fallSpeed = 0.0f;
        ++mod.generation;
    }
    if (mod.lastSimulationFrame == play.gameplayFrames) return SHIP_NATIVE_OK;
    mod.lastSimulationFrame = play.gameplayFrames;
    mod.appearance = std::clamp(mod.appearance + fog::StepSeconds * (mod.enabled ? 1.25f : -1.6f), 0.0f, 1.0f);
    ++mod.flowTick;
    updateWind(mod, play);
    updateLook(mod, true);
    mod.field.beginTick();
    mod.field.step(fog::StepSeconds, mod.liftPercent * 0.01f);
    if (stableUsable(mod) && (!mod.stableReady || mod.stableSide != stableSide(mod) ||
                              mod.stableCellTenths != stableCellTenths(mod)))
        stableReset(mod);
    if (!stableUsable(mod)) mod.stableReady = false;
    if (mod.stableReady) {
        stableParams(mod);
        mod.stable.recenter(mod.linkX, mod.linkZ);
        // O repouso do campo largo (bancos da vila, piso) muda com o tempo: relê a cada 0,5 s, fatiado por linhas.
        stableRefresh(mod);
        const auto t0 = std::chrono::steady_clock::now();
        mod.stable.step(fog::StepSeconds);
        mod.stableMicros = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
        mod.stableMicrosPeak = std::max(mod.stableMicrosPeak, mod.stableMicros);
    }
    if (mod.stackedTerrain) {
        mod.upperField.beginTick();
        mod.upperField.step(fog::StepSeconds, mod.liftPercent * 0.01f);
    }
    if (mod.cutAge >= 0) {
        // 2 s depois do golpe o empurrão já passou: o que sobra de giro é vórtice. Em 5 s o vão deve ter fechado.
        const float swirl = mod.field.circulation(mod.cutX, mod.cutZ);
        const float gap = mod.field.lowestDensity(mod.cutX, mod.cutZ, GapRadiusCells * mod.field.spacing());
        if (std::abs(swirl) > std::abs(mod.swirlPeak)) mod.swirlPeak = swirl;
        mod.gapLow = std::min(mod.gapLow, gap);
        if (mod.cutAge == 40) mod.swirlLate = swirl;
        if (mod.cutAge == 100) {
            mod.gapLate = gap;
            ++mod.traces; // medição fechada: o main.lua registra a linha
            mod.cutAge = -1;
        } else {
            ++mod.cutAge;
        }
    }
    if (!mod.enabled) return SHIP_NATIVE_OK;
    const auto& dim = player->cylinder.dim;
    if (displaceBody(mod, player->actor, {p.x, p.y + dim.yShift, p.z}, float(dim.radius), float(dim.height)))
        ++mod.bodyTicks;
    displaceActors(mod, play, *player);
    // Pouso: no quadro em que o Link volta ao chão depois de pelo menos 3 quadros no ar, a velocidade de queda do
    // último quadro no ar vira a batida. Degrau e passo ficam abaixo de fog::LandMinSpeed.
    if (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) {
        if (mod.airFrames >= 3 && mod.fallSpeed >= fog::LandMinSpeed) {
            const float speed = mod.fallSpeed;
            if (mod.stableReady && stableLayerHit(mod, p.x, p.y, p.z, StableLandBottom, StableLandTop)) {
                const float gain = stableStirGain(mod) * (mod.attackPercent * 0.01f);
                mod.stable.stirRadial({p.x, p.z}, std::min(speed, 400.0f) * 0.5f * gain, 70.0f);
                ++mod.stirs;
            }
            if (affectFields(mod, [&](fog::Field& field) {
                    return field.land(p, speed, mod.attackPercent * 0.01f, mod.attackTicks);
                })) {
                ++mod.landings;
                mod.landPeak = std::max(mod.landPeak, speed);
            }
        }
        mod.airFrames = 0;
        mod.fallSpeed = 0.0f;
    } else {
        const float drop = player->actor.prevPos.y - player->actor.world.pos.y; // u por quadro de jogo (20 Hz)
        mod.airFrames = std::min(mod.airFrames + 1, 1000);
        mod.fallSpeed = std::isfinite(drop) && drop > 0.0f && drop < 120.0f ? drop / fog::StepSeconds : 0.0f;
    }

    // A pose publicada por Player_Draw representa a última trajetória da arma, mesmo sem alvo.
    const auto& weapon = player->meleeWeaponInfo[0];
    const auto base = point(weapon.base), tip = point(weapon.tip);
    const bool active = player->meleeWeaponState != 0 && weapon.active && finite(base) && finite(tip);
    const auto animation = player->meleeWeaponAnimation;
    // En_M_Thunder pode consumir a flag de spin no mesmo update. A janela
    // ativa da animação da arma continua disponível com e sem magia.
    const bool spin = player->meleeWeaponState != 0 &&
                      animation >= PLAYER_MWA_SPIN_ATTACK_1H && animation <= PLAYER_MWA_BIG_SPIN_2H;
    // Camada rasa: o ataque giratório é um golpe como os outros. A lâmina arrasta o ar tick a tick e o redemoinho
    // em volta do Link sai disso; o campo antigo continua com o giro escrito em Field::spin.
    const bool physical = mod.field.solverOptions().transport == fog::Transport::Shallow;
    if (spin && !mod.spinActive) {
        if (physical) {
            ++mod.spins;
        } else {
            const bool strong = animation >= PLAYER_MWA_BIG_SPIN_1H;
            const float reach = active ? std::hypot(tip.x - p.x, tip.z - p.z) : 0.0f;
            const float radius = std::clamp(reach + 68.0f, strong ? 184.0f : 140.0f, 220.0f);
            if (affectFields(mod, [&](fog::Field& field) {
                    return field.spin(p, radius, 260.0f, mod.attackPercent * 0.01f, mod.attackTicks);
                })) ++mod.spins;
        }
        mod.spinActive = true;
    }
    if (!spin) mod.spinActive = false;
    if ((!spin || physical) && active && mod.weaponActive) {
        // Antes de empurrar: o corpo do Link já afasta a névoa, então o vão do golpe se mede contra este valor,
        // e o giro que já existia no lugar (vento, atores, golpe anterior) é a linha de base do vórtice.
        const float before = mod.field.lowestDensity(p.x, p.z, GapRadiusCells * mod.field.spacing());
        const float swirlBefore = mod.field.circulation(p.x, p.z);
        stableBlade(mod, mod.previousBase, mod.previousTip, base, tip, 22.0f);
        if (affectFields(mod, [&](fog::Field& field) {
                return field.blade(mod.previousBase, mod.previousTip, base, tip, 64.0f,
                                   mod.attackPercent * 0.01f, mod.attackTicks);
            })) {
            ++mod.cuts;
            if (mod.cutAge < 0 || mod.cutAge > 12) { // golpe novo, não o tick seguinte do mesmo arco
                mod.cutX = p.x;
                mod.cutZ = p.z;
                mod.swirlBefore = swirlBefore;
                mod.swirlPeak = mod.swirlLate = 0.0f;
                mod.gapBefore = mod.gapLow = mod.gapLate = before;
            }
            mod.cutAge = 0;
        }
    }
    mod.weaponActive = active;
    mod.previousBase = base;
    mod.previousTip = tip;
    return SHIP_NATIVE_OK;
}

ShipNativeStatus SHIP_NATIVE_CALL destroy(void* user, const ShipNativeHookCall*) {
    auto& mod = *static_cast<Fog*>(user);
    mod.scene = nullptr;
    mod.appearance = 0.0f;
    mod.weaponActive = false;
    mod.spinActive = false;
    mod.sceneColor.invalidateFrame(); // Preservar a cor filtrada durante a transição.
    return SHIP_NATIVE_OK;
}

Vtx vertex(const fog::Field& field, int x, int z, int layer, unsigned frame, float density,
           const fogcolor::Rgb& color) {
    Vtx v{};
    const auto& cell = field.cell(x, z);
    const int edgeCells = std::min({x, z, fog::Side - 1 - x, fog::Side - 1 - z});
    const float edge = std::clamp(edgeCells * (field.spacing() == KokiriSpacing ? 0.25f : 0.32f), 0.0f, 1.0f);
    const float feather = edge * edge * (3.0f - 2.0f * edge);
    const float wx = field.worldX(x), wz = field.worldZ(z);
    const float noise = 0.79f + 0.21f * std::sin(wx * 0.026f + wz * 0.013f - float(frame % 4096u) * 0.04f);
    v.v.ob[0] = static_cast<int32_t>(std::lround(wx));
    v.v.ob[1] = static_cast<int32_t>(std::lround(cell.floor + 9.0f + 17.0f * layer));
    v.v.ob[2] = static_cast<int32_t>(std::lround(wz));
    constexpr float legacyBytes[]{189.0f, 204.0f, 213.0f};
    for (unsigned i = 0; i < 3; ++i)
        v.v.cn[i] = static_cast<uint8_t>(std::lround(legacyBytes[i] * color[i] / fogcolor::Legacy[i]));
    v.v.cn[3] = static_cast<uint8_t>(std::min(cell.density * density * feather * noise *
                                            (layer == 0 ? 138.0f : 87.0f), 255.0f));
    return v;
}

ShipNativeStatus SHIP_NATIVE_CALL draw(void* user, const ShipNativeHookCall* call) {
    auto& mod = *static_cast<Fog*>(user);
    auto* play = static_cast<PlayState*>(static_cast<const ShipOotRenderPlayHookV1*>(call->payload)->play_state);
    if (!mod.scene || mod.scene != play || mod.room != play->roomCtx.curRoom.num || mod.appearance <= 0.0f)
        return SHIP_NATIVE_OK;
    auto color = fogcolor::Whiten(mod.preferSceneColor ? mod.sceneColor.color.value() : fogcolor::Legacy, mod.white);
    const char* brightnessKey = nullptr;
    int brightnessDefault = 100;
    switch (play->sceneNum) {
        case SCENE_LOST_WOODS: brightnessKey = LostWoodsSetting; brightnessDefault = LostWoodsBrightness; break;
        case SCENE_GRAVEYARD: brightnessKey = GraveyardSetting; brightnessDefault = GraveyardBrightness; break;
        case SCENE_SHADOW_TEMPLE: brightnessKey = ShadowSetting; brightnessDefault = ShadowBrightness; break;
        default: break;
    }
    float dim = 1.0f;
    if (brightnessKey) dim = std::clamp(windSetting(mod, brightnessKey, brightnessDefault) * 0.01f, 0.0f, 1.0f);
    // Noite (0.30.1): a luz ambiente noturna de Kokiri ainda passa do platô de exposição (pico ~105 contra 100) e a
    // névoa ficava tão clara quanto de dia, mais clara que tudo na cena. O sinal é a altura do sol, que o jogo
    // recalcula do relógio a cada quadro e passa de forma contínua pelo anoitecer. Só em cena com céu: em
    // interior o relógio não muda a luz.
    // Os dois ajustes corrigem a mesma coisa (cena mais escura do que a luz ambiente indica): vale o mais forte.
    // Multiplicados, o Graveyard à noite ficava em 25% e a névoa virava sombra (14/11 16:55).
    if (!play->envCtx.skyboxDisabled) {
        const float sun = std::clamp(play->envCtx.sunPos.y / 3000.0f, -1.0f, 1.0f);
        const float night = std::clamp(windSetting(mod, NightSetting, NightBrightness) * 0.01f, 0.0f, 1.0f);
        const float t = std::clamp((sun + NightTwilight) / (2.0f * NightTwilight), 0.0f, 1.0f);
        dim = std::min(dim, std::lerp(night, 1.0f, t * t * (3.0f - 2.0f * t)));
    }
    for (auto& channel : color) channel *= dim;
    constexpr int side = fog::Side;
    const float fade = mod.appearance * mod.appearance * (3.0f - 2.0f * mod.appearance);
    const float density = fade * mod.densityPercent * 0.01f / fog::RestDensity;
    const auto& fallback = mod.stackedTerrain && RenderDecks ? mod.upperField : mod.field;
    if (mod.preferVolume && play->sceneNum == SCENE_KOKIRI_FOREST && submitVillage(mod, play, color, density)) {
        ++mod.flowSubmissions;
        ++mod.volumes;
        ++mod.draws;
        return SHIP_NATIVE_OK;
    }
    if (mod.preferVolume && (mod.flow || mod.volume)) {
        ShipOotFogCellV1 cells[side * side];
        uint8_t blockedEdges[side * side];
        bool hasBlockedEdges = false;
        const bool kokiriFade = play->sceneNum == SCENE_KOKIRI_FOREST;
        static_assert(fog::NegativeX == LINKSPAN_OOT_FOG_EDGE_NEGATIVE_X &&
                      fog::PositiveX == LINKSPAN_OOT_FOG_EDGE_POSITIVE_X &&
                      fog::NegativeZ == LINKSPAN_OOT_FOG_EDGE_NEGATIVE_Z &&
                      fog::PositiveZ == LINKSPAN_OOT_FOG_EDGE_POSITIVE_Z);
        static_assert(fog::RestDensity == LINKSPAN_OOT_FOG_VORTEX_REST_DENSITY);
        for (int z = 0; z < side; ++z) {
            for (int x = 0; x < side; ++x) {
                const auto& cell = mod.field.cell(x, z);
                float reach = 1.0f;
                if (kokiriFade) {
                    const float distance = std::hypot(mod.field.worldX(x) - mod.linkX, mod.field.worldZ(z) - mod.linkZ);
                    const float t = std::clamp((KokiriFadeOuter - distance) / (KokiriFadeOuter - KokiriFadeInner),
                                               0.0f, 1.0f);
                    reach = t * t * (3.0f - 2.0f * t);
                }
                cells[z * side + x] = {cell.floor, std::min(cell.density, fog::WireDensity) * reach};
                blockedEdges[z * side + x] = cell.blocked;
                hasBlockedEdges |= cell.blocked != 0;
            }
        }
        const ShipOotFogGridV1 grid{sizeof(grid), side, side,
                                    mod.field.worldX(0), mod.field.worldX(side - 1),
                                    mod.field.worldZ(0), mod.field.worldZ(side - 1),
                                    mod.layerHeight, mod.falloff, fog::volumeExtinction(mod.extinction, density),
                                    play->sceneNum == SCENE_KOKIRI_FOREST ? KokiriEdgeFeather : 110.0f,
                                    {color[0], color[1], color[2]}, cells};
        auto status = SHIP_NATIVE_UNSUPPORTED;
        const auto features = flowFeatures(mod);
        const bool cpuField = cpuFieldActive(mod);
        if ((mod.preferCompute || cpuField) && (features & LINKSPAN_OOT_FOG_FLOW_VORTICES) && mod.flow->submit_flow &&
            (!hasBlockedEdges || (features & LINKSPAN_OOT_FOG_FLOW_BLOCKED_EDGES))) {
            ShipOotFogFlowCellV2 flowCells[side * side];
            ShipOotFogVerticalCell vertical[side * side];
            ShipOotFogCellV1 upperCells[side * side]{};
            float upperRaised[side * side]{};
            unsigned decks = 0;
            float raised[side * side];
            const bool useLift = (features & LINKSPAN_OOT_FOG_FLOW_LIFT) && mod.liftPercent != 0;
            const bool useGpuLift = !cpuField && useLift && (features & LINKSPAN_OOT_FOG_FLOW_VERTICAL_COMPUTE);
            const auto impulse = [](const fog::Impulse& i) {
                return ShipOotFogImpulseV2{i.densityScale, i.vx, i.vz, i.minX, i.maxX, i.minZ, i.maxZ};
            };
            const auto raisedFraction = [](const fog::Cell& cell) {
                return std::min(cell.raisedDensity / std::max(cell.density, 0.0001f),
                                LINKSPAN_OOT_FOG_MAX_RAISED_FRACTION);
            };
            for (int z = 0; z < side; ++z) {
                for (int x = 0; x < side; ++x) {
                    const auto& cell = mod.field.cell(x, z);
                    flowCells[z * side + x] = {cell.vx, cell.vz, uint32_t(cell.open),
                                              impulse(mod.field.beforeStep(x, z)), impulse(mod.field.afterStep(x, z))};
                    if (useLift) {
                        const float fraction = raisedFraction(cell);
                        raised[z * side + x] = fraction;
                        mod.peakLift = std::max(mod.peakLift, fraction);
                    }
                    if (useGpuLift) {
                        vertical[z * side + x] = {cell.raisedDensity, cell.verticalSpeed, mod.field.liftBeforeStep(x, z)};
                    }
                    if (mod.stackedTerrain && RenderDecks) {
                        const auto& upper = mod.upperField.cell(x, z);
                        if (upper.density > 0.0f && upper.floor - cell.floor >= fog::LayerHeight) {
                            upperCells[z * side + x] = {upper.floor, std::min(upper.density, fog::WireDensity)};
                            if (useLift) {
                                const float fraction = raisedFraction(upper);
                                upperRaised[z * side + x] = fraction;
                                mod.peakDeckLift = std::max(mod.peakDeckLift, fraction);
                            }
                            ++decks;
                        }
                    }
                }
            }
            const auto flags = LINKSPAN_OOT_FOG_FLOW_VORTICES |
                               (mod.preferParticles ? LINKSPAN_OOT_FOG_FLOW_PARTICLES : 0u) |
                               (useLift ? LINKSPAN_OOT_FOG_FLOW_LIFT : 0u) |
                               (useGpuLift ? LINKSPAN_OOT_FOG_FLOW_VERTICAL_COMPUTE : 0u) |
                               (cpuField ? LINKSPAN_OOT_FOG_FLOW_CPU_FIELD : 0u) |
                               (decks ? LINKSPAN_OOT_FOG_FLOW_STACKED_SUPPORTS : 0u) |
                               (play->sceneNum == SCENE_KOKIRI_FOREST ? features & LINKSPAN_OOT_FOG_FLOW_ATMOSPHERE : 0u) |
                               (mod.preferLighting ? features & LINKSPAN_OOT_FOG_FLOW_LOCAL_LIGHTS : 0u);
            const ShipOotFogFlowV2 flow{sizeof(flow), mod.generation, mod.flowTick, fog::StepSeconds, flowCells, flags,
                                        hasBlockedEdges ? blockedEdges : nullptr, useLift ? raised : nullptr,
                                        decks ? upperCells : nullptr, decks && useLift ? upperRaised : nullptr,
                                        useGpuLift ? vertical : nullptr};
            status = mod.flow->submit_flow(play, &grid, &flow);
            if (status == SHIP_NATIVE_OK) {
                ++mod.flowSubmissions;
                mod.decks = decks;
            }
        }
        if (status != SHIP_NATIVE_OK && mod.volume && mod.volume->submit) {
            if (mod.stackedTerrain) {
                for (int z = 0; z < side; ++z) {
                    for (int x = 0; x < side; ++x) {
                        const auto& cell = fallback.cell(x, z);
                        cells[z * side + x] = {cell.floor, std::min(cell.density, fog::WireDensity)};
                    }
                }
            }
            status = mod.volume->submit(play, &grid);
        }
        if (status == SHIP_NATIVE_OK) {
            ++mod.volumes;
            ++mod.draws;
            return SHIP_NATIVE_OK;
        }
    }
    auto* gfx = play->state.gfxCtx;
    constexpr int layers = 2;
    constexpr int perStrip = 12;
    constexpr int stripsPerRow = (side - 1 + perStrip - 1) / perStrip;
    constexpr int maxCmds = 16 + layers * (side - 1) * (side - 1 + stripsPerRow);
    constexpr int maxVertices = layers * (side - 1) * 2 * (side - 1 + stripsPerRow);
    struct Geometry { Gfx commands[maxCmds]; Vtx vertices[maxVertices]; };
    const auto available = reinterpret_cast<uintptr_t>(gfx->polyOpa.d);
    const auto used = reinterpret_cast<uintptr_t>(gfx->polyOpa.p);
    if (available <= used || available - used < sizeof(Geometry) + 4096u) return SHIP_NATIVE_OK;
    auto* geometry = static_cast<Geometry*>(mod.alloc(gfx, sizeof(Geometry)));
    if (!geometry || mod.render->matrix_push() != SHIP_NATIVE_OK) return SHIP_NATIVE_OK;
    const void* identity{};
    const auto matrixStatus = mod.render->matrix_translate_new(0, 0, 0) == SHIP_NATIVE_OK
                                  ? mod.render->export_current_matrix(&identity) : SHIP_NATIVE_FAILURE;
    mod.render->matrix_pop();
    if (matrixStatus != SHIP_NATIVE_OK || !identity) return SHIP_NATIVE_OK;

    Gfx* g = geometry->commands;
    Vtx* vertices = geometry->vertices;
    gDPPipeSync(g++);
    gSPClearGeometryMode(g++, G_LIGHTING | G_FOG | G_CULL_FRONT | G_CULL_BACK);
    gSPSetGeometryMode(g++, G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(g++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetAlphaCompare(g++, G_AC_NONE);
    gSPMatrix(g++, const_cast<void*>(identity), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    for (int layer = 0; layer < layers; ++layer) {
        for (int z = 0; z + 1 < side; ++z) {
            for (int start = 0; start + 1 < side; start += perStrip) {
                const int quads = std::min(perStrip, side - 1 - start);
                for (int i = 0; i <= quads; ++i) {
                    vertices[2 * i] = vertex(fallback, start + i, z, layer, play->gameplayFrames, density, color);
                    vertices[2 * i + 1] = vertex(fallback, start + i, z + 1, layer, play->gameplayFrames, density, color);
                }
                __gSPVertex(g++, reinterpret_cast<uintptr_t>(vertices), 2 * (quads + 1), 0);
                for (int i = 0; i < quads; ++i) {
                    const int x = start + i;
                    if (!fallback.surfaceQuadOpen(x, z)) continue;
                    const int v = 2 * i;
                    gSP2Triangles(g++, v, v + 1, v + 2, 0, v + 2, v + 1, v + 3, 0);
                }
                vertices += 2 * (quads + 1);
            }
        }
    }
    gSPEndDisplayList(g++);
    if (mod.render->draw_native_display_list(geometry->commands, LINKSPAN_OOT_RENDER_TRANSLUCENT) == SHIP_NATIVE_OK) {
        mod.restoreXlu(gfx);
        ++mod.draws;
    }
    return SHIP_NATIVE_OK;
}

ShipNativeStatus SHIP_NATIVE_CALL stats(void* user, const char*, uint32_t len, ShipNativeWriteFn write, void* writer) {
    if (len) return SHIP_NATIVE_INVALID_ARGUMENT;
    const auto& mod = *static_cast<Fog*>(user);
    // trace = golpes com medição fechada; swirl = giro (décimos de u/s) antes do golpe, no pico e 2 s depois;
    // gap = densidade ×1000 antes do golpe, no fundo do vão e 5 s depois.
    // fluid = stirs, us do último step, pico de us, velocidade máxima (u/s), |vorticidade| máxima (x10, 1/s)
    // solids = células sólidas da janela fina (0 = allFluid, caminho SIMD completo); curl = amplitude em uso.
    float vmax = 0.0f, wmax = 0.0f;
    unsigned solids = 0;
    if (mod.stableReady) {
        for (int j = 0; j < mod.stable.size(); ++j)
            for (int i = 0; i < mod.stable.size(); ++i) solids += mod.stable.solid(i, j) ? 1u : 0u;
        for (int j = 1; j + 1 < mod.stable.size(); ++j)
            for (int i = 1; i + 1 < mod.stable.size(); ++i) {
                vmax = std::max(vmax, std::hypot(mod.stable.velocityX(i, j), mod.stable.velocityZ(i, j)));
                wmax = std::max(wmax, std::abs(mod.stable.vorticity(i, j)));
            }
    }
    char message[1280];
    const int n = std::snprintf(message, sizeof(message),
                                "active=%d draw=%u link=%u actors=%u cuts=%u cylinders=%u spheres=%u quads=%u enemy=%u "
                                "volumes=%u spins=%u flows=%u shifts=%u blasts=%u walls=%u water=%u "
                                "volume_pref=%d compute_pref=%d particles_pref=%d density_pct=%d attack_pct=%d quality=%d "
                                "push_ms=%d lift_pct=%d loft=%ld lights_pref=%d decks=%u decklift=%ld "
                                "windbase=%ld,%ld wind=%ld,%ld scene_color=%d fog_rgb=%ld,%ld,%ld solver=%d "
                                "trace=%u swirl=%ld,%ld,%ld gap=%ld,%ld,%ld rims=%u cell=%ld env=%d,%d,%d/%d,%d,%d/%d,%d,%d lands=%u landpeak=%ld fluid=%u,%u,%u,%ld,%ld wide=%d solids=%u curl=%ld",
                                mod.enabled, mod.draws, mod.bodyTicks, mod.actorTicks, mod.cuts, mod.cylinders,
                                mod.spheres, mod.quads, mod.enemyAttacks, mod.volumes, mod.spins, mod.flowSubmissions,
                                mod.gridShifts, mod.blasts, mod.walls, mod.water, mod.preferVolume, mod.preferCompute,
                                mod.preferParticles, mod.densityPercent, mod.attackPercent, mod.quality,
                                mod.attackTicks * fog::AttackTickMilliseconds, mod.liftPercent,
                                std::lround(1000.0f * mod.peakLift), mod.preferLighting, mod.decks,
                                std::lround(1000.0f * mod.peakDeckLift), std::lround(10.0f * mod.windBaseX),
                                std::lround(10.0f * mod.windBaseZ), std::lround(10.0f * mod.windX),
                                std::lround(10.0f * mod.windZ), mod.preferSceneColor,
                                std::lround(1000.0f * mod.sceneColor.color.value()[0]),
                                std::lround(1000.0f * mod.sceneColor.color.value()[1]),
                                std::lround(1000.0f * mod.sceneColor.color.value()[2]),
                                int(mod.field.solverOptions().transport), mod.traces,
                                std::lround(10.0f * mod.swirlBefore), std::lround(10.0f * mod.swirlPeak),
                                std::lround(10.0f * mod.swirlLate),
                                std::lround(1000.0f * mod.gapBefore), std::lround(1000.0f * mod.gapLow),
                                std::lround(1000.0f * mod.gapLate), mod.rims, std::lround(mod.field.spacing()),
                                int(mod.sceneLight.ambient[0]), int(mod.sceneLight.ambient[1]), int(mod.sceneLight.ambient[2]),
                                int(mod.sceneLight.directional[0]), int(mod.sceneLight.directional[1]),
                                int(mod.sceneLight.directional[2]), int(mod.sceneLight.fog[0]),
                                int(mod.sceneLight.fog[1]), int(mod.sceneLight.fog[2]), mod.landings,
                                std::lround(mod.landPeak), mod.stirs, mod.stableMicros, mod.stableMicrosPeak,
                                std::lround(vmax), std::lround(10.0f * wmax), hostWideDetail(mod) ? 1 : 0, solids,
                                std::lround(mod.stable.params().curlAmp));
    return n > 0 && n < int(sizeof(message)) ? write(writer, message, uint32_t(n)) : SHIP_NATIVE_FAILURE;
}

ShipNativeStatus SHIP_NATIVE_CALL configure(void* user, const char* args, uint32_t len, ShipNativeWriteFn write,
                                             void* writer) {
    if (!args || !len || len > 48) return SHIP_NATIVE_INVALID_ARGUMENT;
    int values[11]{0, 0, 0, 0, 0, 0, 0, 0, 100, 1, 1};
    const char* end = args + len;
    const char* next = args;
    for (size_t i = 0; i < std::size(values); ++i) {
        const auto parsed = std::from_chars(next, end, values[i]);
        if (parsed.ec != std::errc{}) return SHIP_NATIVE_INVALID_ARGUMENT;
        if (i >= 7 && parsed.ptr == end) break;
        if (i + 1 == std::size(values)) {
            if (parsed.ptr != end) return SHIP_NATIVE_INVALID_ARGUMENT;
        } else {
            if (parsed.ptr == end || *parsed.ptr != ' ') return SHIP_NATIVE_INVALID_ARGUMENT;
            next = parsed.ptr + 1;
        }
    }
    const auto [enabled, volume, compute, particles, density, attack, pushMs, quality, lift, lights, sceneColor] = values;
    if ((enabled & ~1) || (volume & ~1) || (compute & ~1) || (particles & ~1) || (lights & ~1) || (sceneColor & ~1) ||
        density < 25 || density > 200 || attack < 25 || attack > 200 || quality < -1 || quality > 2 ||
        lift < 0 || lift > 200 ||
        pushMs < fog::MinAttackTicks * fog::AttackTickMilliseconds ||
        pushMs > fog::MaxAttackTicks * fog::AttackTickMilliseconds) {
        return SHIP_NATIVE_INVALID_ARGUMENT;
    }
    auto& mod = *static_cast<Fog*>(user);
    if (quality != mod.quality) {
        if (!mod.movement || !mod.movement->get_setting_int || !mod.movement->set_setting_int) {
            return SHIP_NATIVE_UNSUPPORTED;
        }
        if (mod.quality < 0) mod.hostQuality = mod.movement->get_setting_int(FogQualitySetting, 1);
        const auto result = mod.movement->set_setting_int(FogQualitySetting, quality < 0 ? mod.hostQuality : quality);
        if (result != SHIP_NATIVE_OK) return result;
        mod.quality = quality;
    }
    if (mod.enabled != bool(enabled)) {
        mod.weaponActive = false;
        mod.spinActive = false;
    }
    const bool legacyComputeChange = mod.preferCompute != bool(compute) &&
                                     !(flowFeatures(mod) & LINKSPAN_OOT_FOG_FLOW_CPU_FIELD);
    if (mod.preferVolume != bool(volume) || legacyComputeChange || mod.preferParticles != bool(particles) ||
        (mod.liftPercent == 0) != (lift == 0)) {
        ++mod.generation;
    }
    mod.enabled = enabled != 0;
    mod.preferVolume = volume != 0;
    mod.preferCompute = compute != 0;
    applyTransport(mod);
    mod.preferParticles = particles != 0;
    mod.densityPercent = density;
    mod.attackPercent = attack;
    mod.liftPercent = lift;
    mod.preferLighting = lights != 0;
    mod.preferSceneColor = sceneColor != 0;
    if (!mod.preferSceneColor) mod.sceneColor.color.fixed();
    mod.attackTicks = (pushMs + fog::AttackTickMilliseconds / 2) / fog::AttackTickMilliseconds;
    return stats(user, nullptr, 0, write, writer);
}

template <class Fn>
bool resolve(const ShipNativeRuntime& rt, const char* name, Fn& fn) {
    uintptr_t address{};
    if (rt.resolve_symbol(rt.context, name, &address) != SHIP_NATIVE_OK || !address) return false;
    fn = reinterpret_cast<Fn>(address);
    return true;
}

ShipNativeStatus SHIP_NATIVE_CALL init(const ShipNativeRuntime* rt, void** instance) {
    if (!rt || !instance || rt->size < sizeof(*rt) || rt->abi_minor < 4 || !rt->get_service ||
        !rt->resolve_symbol || !rt->register_hook || !rt->unregister_hook || !rt->register_function)
        return SHIP_NATIVE_UNSUPPORTED;
    auto* mod = new (std::nothrow) Fog;
    if (!mod) return SHIP_NATIVE_FAILURE;
    mod->engine = static_cast<const ShipOotEngineV1*>(rt->get_service(rt->context, LINKSPAN_OOT_ENGINE_SERVICE,
                                                                      LINKSPAN_OOT_ENGINE_VERSION,
                                                                      sizeof(*mod->engine)));
    mod->movement = static_cast<const ShipOotMovementV1*>(rt->get_service(rt->context, LINKSPAN_OOT_MOVEMENT_SERVICE,
                                                                          LINKSPAN_OOT_MOVEMENT_VERSION,
                                                                          sizeof(*mod->movement)));
    mod->render = static_cast<const ShipOotRenderV3*>(rt->get_service(rt->context, LINKSPAN_OOT_RENDER_SERVICE,
                                                                    LINKSPAN_OOT_RENDER_VERSION_3, sizeof(*mod->render)));
    mod->volume = static_cast<const ShipOotFogV1*>(
        rt->get_service(rt->context, LINKSPAN_OOT_FOG_SERVICE, LINKSPAN_OOT_FOG_VERSION, sizeof(*mod->volume)));
    mod->flow = static_cast<const ShipOotFogV2*>(rt->get_service(
        rt->context, LINKSPAN_OOT_FOG_SERVICE, LINKSPAN_OOT_FOG_VERSION_2, offsetof(ShipOotFogV2, features)));
    resolve(*rt, "WaterBox_GetSurface1", mod->waterSurface);
    resolve(*rt, "BgCheck_EntityLineTest1", mod->wallLine);
    if (!mod->engine || !mod->engine->get_save_context || !mod->engine->get_player || !mod->render ||
        !mod->engine->layout_id || std::strcmp(mod->engine->layout_id, LINKSPAN_OOT_LAYOUT_ID) != 0 ||
        mod->engine->play_state_size != sizeof(PlayState) || mod->engine->player_size != sizeof(Player) ||
        mod->engine->save_context_size != sizeof(SaveContext) ||
        !resolve(*rt, "BgCheck_EntityRaycastFloor1", mod->floorRay) ||
        !resolve(*rt, "Graph_Alloc", mod->alloc) || !resolve(*rt, "Gfx_SetupDL_25Xlu", mod->restoreXlu)) {
        delete mod;
        return SHIP_NATIVE_UNSUPPORTED;
    }
    const ShipNativeHookSpec hooks[] = {
        {sizeof(ShipNativeHookSpec), LINKSPAN_OOT_HOOK_PLAY_UPDATE, 1, sizeof(ShipOotPlayHookV1),
         SHIP_NATIVE_HOOK_OBSERVE, SHIP_NATIVE_HOOK_BEFORE, 0, attacks, mod},
        {sizeof(ShipNativeHookSpec), LINKSPAN_OOT_HOOK_PLAY_UPDATE, 1, sizeof(ShipOotPlayHookV1),
         SHIP_NATIVE_HOOK_OBSERVE, SHIP_NATIVE_HOOK_AFTER, 0, update, mod},
        {sizeof(ShipNativeHookSpec), LINKSPAN_OOT_HOOK_PLAY_DESTROY, 1, sizeof(ShipOotPlayHookV1),
         SHIP_NATIVE_HOOK_OBSERVE, SHIP_NATIVE_HOOK_BEFORE, 0, destroy, mod},
        {sizeof(ShipNativeHookSpec), LINKSPAN_OOT_HOOK_RENDER_WORLD_LIGHTS, 1, sizeof(ShipOotRenderPlayHookV1),
         SHIP_NATIVE_HOOK_OBSERVE, SHIP_NATIVE_HOOK_BEFORE, 0, draw, mod},
    };
    uint64_t handles[std::size(hooks)]{};
    size_t count = 0;
    ShipNativeStatus result = SHIP_NATIVE_OK;
    for (const auto& hook : hooks) {
        result = rt->register_hook(rt->context, &hook, &handles[count]);
        if (result != SHIP_NATIVE_OK) break;
        ++count;
    }
    if (result == SHIP_NATIVE_OK) result = rt->register_function(rt->context, "stats", stats, mod);
    if (result == SHIP_NATIVE_OK) result = rt->register_function(rt->context, "configure", configure, mod);
    if (result != SHIP_NATIVE_OK) {
        while (count) rt->unregister_hook(rt->context, handles[--count]);
        delete mod;
        return result;
    }
    *instance = mod;
    return SHIP_NATIVE_OK;
}

void SHIP_NATIVE_CALL shutdown(void* instance) {
    auto* mod = static_cast<Fog*>(instance);
    if (mod->quality >= 0 && mod->movement->get_setting_int(FogQualitySetting, -1) == mod->quality) {
        mod->movement->set_setting_int(FogQualitySetting, mod->hostQuality);
    }
    delete mod;
}
} // namespace

extern "C" SHIP_NATIVE_EXPORT const ShipNativeDescriptor* SHIP_NATIVE_CALL ShipNative_Query() {
    static const ShipNativeDescriptor descriptor{sizeof(ShipNativeDescriptor), SHIP_NATIVE_ABI_MAJOR, 4, init, shutdown};
    return &descriptor;
}
#endif // FOGREMASTER_FOLLOW_TEST
