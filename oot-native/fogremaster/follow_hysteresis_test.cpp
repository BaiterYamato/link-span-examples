// cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 follow_hysteresis_test.cpp
#define FOGREMASTER_FOLLOW_TEST
#include "provider.cpp"

#include <cstdio>

namespace {
constexpr float MainSpacing = 40.0f, VillageTestSpacing = 80.0f;
constexpr float Drift = 4.0f, WalkDistance = 400.0f;
constexpr int Frames = 1000;

bool check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    return condition;
}

int legacyShift(float distance, float spacing, float overlap) {
    const float cells = distance / spacing;
    return static_cast<int>(std::lround(cells - std::clamp(cells, -overlap, overlap)));
}

// Centro discreto em qualquer um dos lados da borda; alterna os extremos da deriva.
int driftShifts(float spacing, float center, float boundary, bool legacy) {
    int shifts = 0;
    for (int frame = 0; frame < Frames; ++frame) {
        const float target = boundary + (frame % 2 ? Drift : -Drift);
        const int cells = legacy ? legacyShift(target - center, spacing, 0.0f)
                                 : followShiftAxis(target - center, spacing, 0.0f, FollowHysteresisCells);
        center += cells * spacing;
        shifts += cells != 0;
    }
    return shifts;
}

bool walk(float spacing, int direction, bool mainGate) {
    float center = 0.0f;
    int movedCells = 0, shifts = 0;
    bool pass = true;
    for (int step = 0; step <= static_cast<int>(WalkDistance / Drift); ++step) {
        const float target = direction * step * Drift;
        const float distance = target - center;
        const bool shift = !mainGate || std::abs(distance) >= spacing;
        const int cells = shift ? followShiftAxis(distance, spacing, 0.0f, FollowHysteresisCells) : 0;
        center += cells * spacing;
        movedCells += cells;
        shifts += cells != 0;
        pass &= cells * direction >= 0;
        pass &= std::abs(target - center) <= (mainGate ? spacing : (0.5f + FollowHysteresisCells) * spacing);
        pass &= (mainGate && std::abs(target - center) < spacing) ||
                followShiftAxis(target - center, spacing, 0.0f, FollowHysteresisCells) == 0;
    }
    const int expectedCells = static_cast<int>(WalkDistance / spacing);
    std::printf("  caminhada %+.0f u, celula %.0f u: %d shifts, centro %+.0f u\n",
                direction * WalkDistance, spacing, shifts, center);
    return pass && movedCells == direction * expectedCells && shifts == expectedCells;
}
} // namespace

int main() {
    bool pass = true;
    const int oldVillage = driftShifts(VillageTestSpacing, 440.0f, 480.0f, true);
    const int oldMainAxis = driftShifts(MainSpacing, 460.0f, 480.0f, true);
    std::printf("Reproducao antiga: vila=%d, eixo secundario=%d shifts em %d amostras\n",
                oldVillage, oldMainAxis, Frames);
    pass &= check(oldVillage > Frames / 2 && oldMainAxis > Frames / 2,
                  "fixture reproduz alternancia do arredondamento antigo");
    pass &= check(driftShifts(VillageTestSpacing, 440.0f, 480.0f, false) == 0 &&
                  driftShifts(VillageTestSpacing, 520.0f, 480.0f, false) == 0,
                  "vila: deriva +/-4 u em Z=480 conserva ambos os centros discretos");
    pass &= check(driftShifts(MainSpacing, 460.0f, 480.0f, false) == 0 &&
                  driftShifts(MainSpacing, 500.0f, 480.0f, false) == 0,
                  "eixo secundario de 40 u: deriva +/-4 u nao oscila");

    const float threshold = (0.5f + FollowHysteresisCells) * MainSpacing;
    pass &= check(followShiftAxis(threshold, MainSpacing, 0.0f, FollowHysteresisCells) == 0 &&
                  followShiftAxis(-threshold, MainSpacing, 0.0f, FollowHysteresisCells) == 0 &&
                  followShiftAxis(threshold + 0.01f, MainSpacing, 0.0f, FollowHysteresisCells) == 1 &&
                  followShiftAxis(-threshold - 0.01f, MainSpacing, 0.0f, FollowHysteresisCells) == -1,
                  "limiar estrito e simetrico: so move depois da faixa de retencao");
    pass &= check(driftShifts(MainSpacing, MainSpacing, threshold, false) == 0 &&
                  driftShifts(MainSpacing, 0.0f, threshold, false) == 1,
                  "apos cruzar o limiar, deriva +/-4 u nao desfaz o passo");

    for (const int direction : {-1, 1}) {
        pass &= check(walk(MainSpacing, direction, true), "grade principal acompanha caminhada de 400 u");
        pass &= check(walk(VillageTestSpacing, direction, false), "vila acompanha caminhada de 400 u");
        pass &= check(followShiftAxis(direction * WalkDistance, MainSpacing, 0.0f,
                                    FollowHysteresisCells) == direction * 10,
                      "salto de 400 u preserva deslocamento de multiplas celulas");
    }
    bool unchanged = true;
    for (int i = -200; i <= 200; ++i) {
        const float distance = i * 10.0f;
        unchanged &= followShiftAxis(distance, VillageTestSpacing, 3.0f, 0.0f) ==
                     legacyShift(distance, VillageTestSpacing, 3.0f);
    }
    pass &= check(unchanged, "fora de Kokiri: overlap e empates mantem a decisao antiga");
    std::printf("%s\n", pass ? "Todos os testes passaram." : "Ha criterios de aceite reprovados.");
    return pass ? 0 : 1;
}
