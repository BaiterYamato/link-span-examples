#include "fog_field.h"

#include <algorithm>
#include <cmath>

namespace fog {
namespace {
Point lerp(Point a, Point b, float t) {
    return {std::lerp(a.x, b.x, t), std::lerp(a.y, b.y, t), std::lerp(a.z, b.z, t)};
}

float lattice(int x, int z) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(z) * 0xd8163841u;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    return float(h & 0xffffu) / 65535.0f;
}

// Ruído de valor em [0, 1], contínuo, com período de uma unidade entre nós.
float valueNoise(float x, float z) {
    const float fx = std::floor(x), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    return std::lerp(std::lerp(lattice(ix, iz), lattice(ix + 1, iz), tx),
                     std::lerp(lattice(ix, iz + 1), lattice(ix + 1, iz + 1), tx), tz);
}
} // namespace

float Field::restAt(float worldX, float worldZ) const {
    if (bankWavelength_ <= 0.0f) return RestDensity;
    const float u = (worldX - bankOffsetX_) / bankWavelength_, v = (worldZ - bankOffsetZ_) / bankWavelength_;
    // O ruído de valor tem cristas retas alinhadas à sua malha; sem girar e entortar o domínio, os bancos saem como
    // faixas retas leste-oeste ou norte-sul com centenas de unidades.
    const float x = u + 0.9f * (valueNoise(0.83f * u + 31.7f, 0.83f * v + 4.1f) - 0.5f);
    const float z = v + 0.9f * (valueNoise(0.83f * u - 12.9f, 0.83f * v + 47.3f) - 0.5f);
    const float value = 0.68f * valueNoise(0.857f * x - 0.515f * z, 0.515f * x + 0.857f * z) +
                        0.32f * valueNoise(2.1f * (0.545f * x + 0.839f * z) + 17.3f,
                                           2.1f * (-0.839f * x + 0.545f * z) - 9.1f);
    const float t = std::clamp((value - 0.36f) / 0.26f, 0.0f, 1.0f);
    return RestDensity * std::lerp(bankLow_, bankHigh_, t * t * (3.0f - 2.0f * t));
}

void Field::refreshRest() {
    for (int z = 0; z < Side; ++z) {
        for (int x = 0; x < Side; ++x) rest_[index(x, z)] = restAt(worldX(x), worldZ(z));
    }
}

void Field::banks(float low, float high, float wavelength, float offsetX, float offsetZ) {
    const auto bounded = [](float v, float fallback, float lo, float hi) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    bankLow_ = bounded(low, 1.0f, 0.0f, 1.0f / RestDensity);
    bankHigh_ = bounded(high, 1.0f, bankLow_, 1.0f / RestDensity);
    bankWavelength_ = bounded(wavelength, 0.0f, 0.0f, 4096.0f);
    bankOffsetX_ = std::isfinite(offsetX) ? offsetX : 0.0f;
    bankOffsetZ_ = std::isfinite(offsetZ) ? offsetZ : 0.0f;
    refreshRest();
}

void Field::reset(float x, float z, float spacing) {
    spacing_ = std::isfinite(spacing) && spacing > 0.0f ? spacing : Spacing;
    x0_ = x - (Side - 1) * spacing_ * 0.5f;
    z0_ = z - (Side - 1) * spacing_ * 0.5f;
    refreshRest();
    cells_.fill({});
    pressure_.fill({});
    pressureTick_ = 0;
    remainder_ = 0.0;
    projectionStats_ = {};
    beginTick();
}

void Field::shift(int dx, int dz) {
    x0_ += dx * spacing_;
    z0_ += dz * spacing_;
    const auto shiftGrid = [dx, dz](auto& grid) {
        const auto previous = grid;
        grid.fill({});
        for (int z = std::max(0, -dz); z < std::min(Side, Side - dz); ++z) {
            for (int x = std::max(0, -dx); x < std::min(Side, Side - dx); ++x) {
                grid[index(x, z)] = previous[index(x + dx, z + dz)];
            }
        }
    };
    shiftGrid(cells_);
    for (auto& grid : pressure_) shiftGrid(grid);
    refreshRest();
    beginTick();
    projectionStats_ = {};
}

void Field::terrain(int x, int z, float y, bool open, bool fadeIn) {
    const int i = index(x, z);
    Cell& cell = cells_[i];
    if (cell.open && open && std::abs(y - cell.floor) <= 4.0f) {
        cell.floor = y;
    } else {
        cell = {open ? y : 0.0f, open, open && !fadeIn ? rest_[i] : 0.0f, 0.0f, 0.0f};
        for (auto& grid : pressure_) grid[i] = {};
    }
}

bool Field::connected(const Cell& a, const Cell& b) {
    return a.open && b.open && std::abs(a.floor - b.floor) < LayerHeight;
}

void Field::blockEdge(int x, int z, int otherX, int otherZ, bool blocked) {
    const Edge toward = x == otherX ? (otherZ < z ? NegativeZ : PositiveZ) :
                                     (otherX < x ? NegativeX : PositiveX);
    const auto set = [blocked](Cell& cell, uint8_t edge) {
        if (blocked) cell.blocked |= edge;
        else cell.blocked &= ~edge;
    };
    set(cells_[index(x, z)], toward);
    set(cells_[index(otherX, otherZ)], toward & (NegativeX | NegativeZ) ? toward << 1 : toward >> 1);
}

void Field::beginTick() {
    before_.fill({});
    after_.fill({});
    beforeLift_.fill(0.0f);
    stepped_ = false;
}

void Field::applyImpulse(int index, float densityScale, float vx, float vz) {
    const auto clampSpeed = [](float value) { return std::clamp(value, -SpeedLimit, SpeedLimit); };
    Cell& cell = cells_[index];
    cell.density *= densityScale;
    cell.raisedDensity *= densityScale;
    cell.vx = clampSpeed(cell.vx + vx);
    cell.vz = clampSpeed(cell.vz + vz);

    // Several saturated additions compose to one addition with tighter bounds.
    Impulse& batch = (stepped_ ? after_ : before_)[index];
    batch.densityScale *= densityScale;
    batch.vx += vx;
    batch.vz += vz;
    batch.minX = clampSpeed(batch.minX + vx);
    batch.maxX = clampSpeed(batch.maxX + vx);
    batch.minZ = clampSpeed(batch.minZ + vz);
    batch.maxZ = clampSpeed(batch.maxZ + vz);
}

void Field::queueAttack(int i, float vx, float vz, float lift, int ticks, int delay) {
    // Differences of smoothstep distribute a fixed impulse over time with a soft start and end.
    float previous = 0.0f;
    for (int offset = delay; offset < ticks; ++offset) {
        const float t = float(offset - delay + 1) / float(ticks - delay);
        const float integral = t * t * (3.0f - 2.0f * t);
        auto& push = pressure_[(pressureTick_ + offset) % MaxAttackTicks][i];
        const float fraction = integral - previous;
        push.x += vx * fraction;
        push.z += vz * fraction;
        push.lift += lift * fraction;
        previous = integral;
    }
}

void Field::advanceAttackPressure(float liftStrength) {
    auto& due = pressure_[pressureTick_];
    beforeLift_.fill(0.0f);
    for (int i = 0; i < Side * Side; ++i) {
        const auto& push = due[i];
        if (!cells_[i].open || (push.x == 0.0f && push.z == 0.0f && push.lift == 0.0f)) continue;
        const float magnitude = std::hypot(push.x, push.z);
        const float cap = options_.transport == Transport::Shallow ? options_.attackCap : LegacyAttackCap;
        const float limit = cap / std::max(cap, magnitude);
        // The attack adds momentum. The face flux transports the density it displaces.
        applyImpulse(i, 1.0f, push.x * limit, push.z * limit);
        auto& cell = cells_[i];
        const float lift = std::clamp(push.lift, -16.0f, 16.0f) * liftStrength;
        beforeLift_[i] = lift;
        cell.verticalSpeed = std::clamp(cell.verticalSpeed + lift, -84.0f, 84.0f);
    }
    due.fill({});
    pressureTick_ = (pressureTick_ + 1) % MaxAttackTicks;
}

bool Field::disturb(Point a, Point b, float radius, float bottom, float top, Point force, float clearance,
                    float core, float radialSpeed, float strength, int attackTicks) {
    force.y += 0.12f * radialSpeed;
    return disturbMotion(a, b, radius, bottom, top, force, force, clearance, core, radialSpeed, strength, attackTicks);
}

bool Field::disturbMotion(Point a, Point b, float radius, float bottom, float top, Point baseForce, Point tipForce,
                          float clearance, float core, float radialSpeed, float strength, int attackTicks,
                          float radialBase) {
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    const float dy = b.y - a.y;
    const float len2 = dx * dx + dz * dz;
    const float maxX = worldX(Side - 1);
    const float maxZ = worldZ(Side - 1);
    const float left = std::min(a.x, b.x) - radius, right = std::max(a.x, b.x) + radius;
    const float near = std::min(a.z, b.z) - radius, far = std::max(a.z, b.z) + radius;
    if (left > maxX || right < x0_ || near > maxZ || far < z0_) return false;
    const int xMin = static_cast<int>((std::max(left, x0_) - x0_) / spacing_);
    const int zMin = static_cast<int>((std::max(near, z0_) - z0_) / spacing_);
    const int xMax = static_cast<int>((std::min(right, maxX) - x0_) / spacing_);
    const int zMax = static_cast<int>((std::min(far, maxZ) - z0_) / spacing_);
    bool affected = false;
    for (int z = zMin; z <= zMax; ++z) {
        for (int x = xMin; x <= xMax; ++x) {
            const Cell& c = cells_[index(x, z)];
            if (!c.open) continue;
            const float ax = worldX(x) - a.x, az = worldZ(z) - a.z;
            // Find the closest point only on the part of the sweep that overlaps this layer.
            const float lowY = c.floor - top;
            const float highY = c.floor + LayerHeight - bottom;
            float first = 0.0f, last = 1.0f;
            if (dy == 0.0f) {
                if (a.y < lowY || a.y > highY) continue;
            } else {
                const float enter = (lowY - a.y) / dy;
                const float leave = (highY - a.y) / dy;
                first = std::max(first, std::min(enter, leave));
                last = std::min(last, std::max(enter, leave));
                if (last < first) continue;
            }
            const float nearest = len2 > 0.01f ? (ax * dx + az * dz) / len2 : first;
            const float t = std::clamp(nearest, first, last);
            const float offsetX = ax - t * dx, offsetZ = az - t * dz;
            const float d = std::sqrt(offsetX * offsetX + offsetZ * offsetZ);
            if (d >= radius) continue;
            const float weight = strength * std::min((radius - d) / (radius * (1.0f - core)), 1.0f);
            const float radial = radialBase + radialSpeed / std::max(d, Spacing);
            const auto force = lerp(baseForce, tipForce, t);
            const float pushX = (force.x + offsetX * radial) * weight, pushZ = (force.z + offsetZ * radial) * weight;
            if (attackTicks) {
                const int delay = static_cast<int>((d / radius) * (attackTicks / 3));
                const float lift = 0.42f * std::hypot(pushX, pushZ) + force.y * weight;
                queueAttack(index(x, z), pushX, pushZ, lift, attackTicks, delay);
            } else {
                applyImpulse(index(x, z), 1.0f - std::min(clearance * weight, 1.0f), pushX, pushZ);
            }
            affected = true;
        }
    }
    return affected;
}

bool Field::blade(Point oldBase, Point oldTip, Point base, Point tip, float radius, float strength, int attackTicks) {
    // Camada rasa: a lâmina arrasta o ar no sentido em que se move e afasta um pouco para os lados (bem menos que no
    // modo antigo); o vão e o giro saem desse empurrão. O alcance mínimo deixa o giro com tamanho que a grade carrega.
    const bool shallow = options_.transport == Transport::Shallow;
    if (shallow) radius = std::max(radius, options_.bladeCells * spacing_);
    const Point baseDelta{base.x - oldBase.x, base.y - oldBase.y, base.z - oldBase.z};
    const Point tipDelta{tip.x - oldTip.x, tip.y - oldTip.y, tip.z - oldTip.z};
    const float distance = std::max(std::hypot(baseDelta.x, baseDelta.y, baseDelta.z),
                                    std::hypot(tipDelta.x, tipDelta.y, tipDelta.z));
    if (distance < 1.0f || distance > 180.0f) return false;
    const int slices = std::clamp(static_cast<int>(std::ceil(distance / 16.0f)), 1, 12);
    const float scale = 4.0f / slices * (shallow ? options_.bladeGain : 1.0f);
    const Point baseForce{baseDelta.x * scale, baseDelta.y * scale * 0.42f, baseDelta.z * scale};
    const Point tipForce{tipDelta.x * scale, tipDelta.y * scale * 0.42f, tipDelta.z * scale};
    bool affected = false;
    for (int i = 1; i <= slices; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(slices);
        affected |= disturbMotion(lerp(oldBase, base, t), lerp(oldTip, tip, t), radius, -32.0f, 12.0f,
                                  baseForce, tipForce, 0.0f, 0.38f, 0.0f, strength, attackTicks,
                                  shallow ? options_.bladeRadial : 1.8f);
    }
    return affected;
}

bool Field::spin(Point center, float radius, float speed, float strength, int attackTicks) {
    bool affected = false;
    for (int z = 0; z < Side; ++z) {
        for (int x = 0; x < Side; ++x) {
            const Cell& c = cells_[index(x, z)];
            if (!c.open || center.y - 18.0f > c.floor + LayerHeight || center.y + 48.0f < c.floor) continue;
            const float dx = worldX(x) - center.x;
            const float dz = worldZ(z) - center.z;
            const float distance = std::hypot(dx, dz);
            if (distance >= radius) continue;
            const float falloff = strength * std::min((radius - distance) / (0.42f * radius), 1.0f);
            const float impulse = speed * falloff / std::max(distance, Spacing);
            const float swirl = impulse * 0.55f * std::min(1.0f, distance * 2.0f / radius);
            const int delay = static_cast<int>((distance / radius) * (attackTicks / 3));
            const float vx = dx * impulse - dz * swirl, vz = dz * impulse + dx * swirl;
            queueAttack(index(x, z), vx, vz, 0.42f * std::hypot(vx, vz), attackTicks, delay);
            affected = true;
        }
    }
    return affected;
}

bool Field::land(Point center, float impactSpeed, float strength, int attackTicks) {
    if (!std::isfinite(impactSpeed) || impactSpeed < LandMinSpeed || strength <= 0.0f) return false;
    // A batida desloca o volume do corpo: a névoa sai por igual para todos os lados (só a parte radial do
    // disturbMotion, radialBase 0) e a onda da camada rasa leva o anel adiante; nada aqui escreve o anel.
    // O teto do golpe vale por tick; espalhado em attackTicks, o empurrão total pode passar dele.
    const float outward = std::min(LandOutward * impactSpeed, 4.0f * options_.attackCap);
    const float radius = std::clamp(2.0f * spacing_ + 0.2f * (impactSpeed - LandMinSpeed), 2.0f * spacing_,
                                    4.0f * spacing_);
    return disturbMotion(center, center, radius, -12.0f, 36.0f, {}, {}, 0.0f, 0.3f, outward, strength,
                         std::max(attackTicks, MinAttackTicks), 0.0f);
}

void Field::step(float dt, float liftStrength) {
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    if (options_.transport == Transport::SemiLagrangian) {
        remainder_ += dt;
        while (remainder_ >= double(StepSeconds)) {
            stepFluid(liftStrength);
            remainder_ -= double(StepSeconds);
        }
        return;
    }
    advanceAttackPressure(liftStrength);
    stepped_ = true;
    const auto previous = cells_;
    std::array<std::array<int, 4>, Side * Side> neighbors{};
    std::array<float, Side * Side> curl{};
    constexpr int dx[] = {-1, 1, 0, 0}, dz[] = {0, 0, -1, 1};
    for (int z = 0; z < Side; ++z) {
        for (int x = 0; x < Side; ++x) {
            const int i = index(x, z);
            const auto& src = previous[i];
            if (!src.open) continue;
            auto& links = neighbors[i];
            links.fill(i);
            for (int d = 0; d < 4; ++d) {
                const int nx = x + dx[d], nz = z + dz[d];
                if (nx < 0 || nx >= Side || nz < 0 || nz >= Side || (src.blocked & (1u << d))) continue;
                const int other = index(nx, nz);
                if (connected(src, previous[other]) && !(previous[other].blocked & (1u << (d ^ 1)))) links[d] = other;
            }
            curl[i] = 0.5f * (previous[links[1]].vz - previous[links[0]].vz -
                               (previous[links[3]].vx - previous[links[2]].vx));
        }
    }

    // Portador de fundo: padrão (16, 5) = vento fixo antigo; setWind() troca pelo vento da cena por tick.
    const bool shallow = options_.transport == Transport::Shallow;
    const float wind[] = {-options_.windX, options_.windX, -options_.windZ, options_.windZ};
    const float damping = std::exp(-(shallow ? options_.damping : VelocityDamping) * dt);
    const float verticalDamping = std::exp(-2.4f * dt);
    const float ratio = dt / spacing_;
    const float refill = std::min(dt * (shallow ? options_.recovery : Recovery), 1.0f);
    const float confinementGain = shallow ? options_.confinement : 3.8f;
    // Camada rasa: ganho de velocidade por unidade de diferença de densidade entre células vizinhas (g' Δh/Δx).
    const float pressure = shallow ? dt * options_.gravity / spacing_ : 0.0f;
    // Advecção a montante da velocidade e confinamento de vorticidade, iguais nos dois transportes.
    const auto carried = [&](int i, float& vx, float& vz) {
        const auto& src = previous[i];
        const auto& links = neighbors[i];
        const float flowX = src.vx + options_.windX, flowZ = src.vz + options_.windZ;
        const auto& upwindX = previous[links[flowX >= 0.0f ? 0 : 1]];
        const auto& upwindZ = previous[links[flowZ >= 0.0f ? 2 : 3]];
        const float weightX = std::min(std::abs(flowX) * ratio, 0.45f);
        const float weightZ = std::min(std::abs(flowZ) * ratio, 0.45f);
        const float gx = std::abs(curl[links[1]]) - std::abs(curl[links[0]]);
        const float gz = std::abs(curl[links[3]]) - std::abs(curl[links[2]]);
        const float confinement = confinementGain * dt * curl[i] / std::max(std::sqrt(gx * gx + gz * gz), 1.0f);
        vx = src.vx + (upwindX.vx - src.vx) * weightX + (upwindZ.vx - src.vx) * weightZ + gz * confinement;
        vz = src.vz + (upwindX.vz - src.vz) * weightX + (upwindZ.vz - src.vz) * weightZ - gx * confinement;
    };
    if (shallow) {
        // Para a frente e para trás: a velocidade nova (com a pressão da densidade atual) move a massa neste mesmo
        // passo, o que deixa as ondas estáveis. flow_ guarda a velocidade antes da pressão; a face soma a ela a
        // diferença compacta de densidade dos dois lados (Rhie-Chow), então o vão se fecha sem tabuleiro de xadrez.
        // Divergência atual (mesmo estêncil do rotacional). Puxar a velocidade pelo seu gradiente amortece só a
        // parte que comprime e expande, isto é, a onda; a parte que gira não tem divergência e passa intacta.
        auto& spread = poisson_;
        for (int i = 0; i < Side * Side; ++i) {
            if (!previous[i].open) continue;
            const auto& links = neighbors[i];
            spread[i] = 0.5f * (previous[links[1]].vx - previous[links[0]].vx + previous[links[3]].vz -
                                previous[links[2]].vz);
        }
        const float divergenceDamping = options_.divergenceDamping;
        for (int i = 0; i < Side * Side; ++i) {
            if (!previous[i].open) continue;
            const auto& links = neighbors[i];
            float vx, vz;
            carried(i, vx, vz);
            vx += divergenceDamping * 0.5f * (spread[links[1]] - spread[links[0]]);
            vz += divergenceDamping * 0.5f * (spread[links[3]] - spread[links[2]]);
            // Laplaciano compacto (parede = a própria célula, sem atrito com ela).
            float lapX = 0.0f, lapZ = 0.0f;
            for (int d = 0; d < 4; ++d) {
                lapX += previous[links[d]].vx - previous[i].vx;
                lapZ += previous[links[d]].vz - previous[i].vz;
            }
            vx += options_.viscosity * lapX;
            vz += options_.viscosity * lapZ;
            flow_[i].vx = vx;
            flow_[i].vz = vz;
            // A pressão vem do desvio em relação ao repouso: um banco em equilíbrio não escorre para o vão ao lado.
            const auto excess = [&](int j) { return previous[j].density - rest_[j]; };
            const float gx = 0.5f * (excess(links[1]) - excess(links[0]));
            const float gz = 0.5f * (excess(links[3]) - excess(links[2]));
            cells_[i].vx = std::clamp((vx - pressure * gx) * damping, -SpeedLimit, SpeedLimit);
            cells_[i].vz = std::clamp((vz - pressure * gz) * damping, -SpeedLimit, SpeedLimit);
        }
    }
    for (int i = 0; i < Side * Side; ++i) {
        const auto& src = previous[i];
        if (!src.open) continue;
        const auto& links = neighbors[i];
        float density = src.density;
        float raised = src.raisedDensity;
        for (int d = 0; d < 4; ++d) {
            const int j = links[d];
            if (j == i) continue;
            const auto& other = previous[j];
            float speed;
            if (shallow) {
                const float sum = d < 2 ? flow_[i].vx + flow_[j].vx : flow_[i].vz + flow_[j].vz;
                const float excess = (other.density - rest_[j]) - (src.density - rest_[i]);
                speed = ((d & 1 ? 0.5f : -0.5f) * sum - pressure * excess) * damping + wind[d];
            } else {
                const float sum = d < 2 ? src.vx + other.vx : src.vz + other.vz;
                speed = (d & 1 ? 0.5f : -0.5f) * sum + wind[d];
            }
            const float flux = std::clamp(speed * ratio, -0.24f, 0.24f);
            // Constant ambient wind transports only the departure from the resting fog density.
            const auto& upstream = flux >= 0.0f ? src : other;
            const float background = std::clamp(wind[d] * ratio, -0.24f, 0.24f) * rest_[flux >= 0.0f ? i : j];
            density -= flux * upstream.density - background;
            raised -= flux * upstream.raisedDensity;
        }

        const float flowX = src.vx + options_.windX, flowZ = src.vz + options_.windZ;
        const auto& upwindX = previous[links[flowX >= 0.0f ? 0 : 1]];
        const auto& upwindZ = previous[links[flowZ >= 0.0f ? 2 : 3]];
        const float weightX = std::min(std::abs(flowX) * ratio, 0.45f);
        const float weightZ = std::min(std::abs(flowZ) * ratio, 0.45f);
        auto& dest = cells_[i];
        dest.density = std::lerp(std::clamp(density, 0.0f, MaxDensity), rest_[i], refill);
        if (!shallow) {
            float vx, vz;
            carried(i, vx, vz);
            dest.vx = std::clamp(vx * damping, -SpeedLimit, SpeedLimit);
            dest.vz = std::clamp(vz * damping, -SpeedLimit, SpeedLimit);
        }
        const float vertical = src.verticalSpeed + (upwindX.verticalSpeed - src.verticalSpeed) * weightX +
                               (upwindZ.verticalSpeed - src.verticalSpeed) * weightZ;
        if (raised <= 0.0f && vertical <= 0.0f) {
            dest.raisedDensity = 0.0f;
            dest.verticalSpeed = 0.0f;
            continue;
        }
        raised = std::clamp(raised, 0.0f, dest.density);
        const float fraction = raised / std::max(dest.density, 0.0001f);
        const float speed = (vertical - dt * (24.0f + 112.0f * fraction)) * verticalDamping;
        // Move density between the two bands without altering the total density of the column.
        const float available = speed > 0.0f ? dest.density - raised : std::min(10.0f * raised, dest.density);
        raised += (speed * dt / (0.65f * LayerHeight)) * available;
        dest.raisedDensity = std::clamp(raised, 0.0f, 0.70f * dest.density);
        if (dest.raisedDensity <= 0.0005f && speed <= 0.0f) {
            dest.raisedDensity = 0.0f;
            dest.verticalSpeed = 0.0f;
        } else {
            dest.verticalSpeed = speed;
        }
    }
}
} // namespace fog

namespace fog {
void Field::setSolverOptions(SolverOptions options) {
    options.pressureIterations = std::clamp(options.pressureIterations, 1, 256);
    const auto bounded = [](float v, float fallback, float lo, float hi) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    options.damping = bounded(options.damping, VelocityDamping, 0.0f, 100.0f);
    options.recovery = bounded(options.recovery, Recovery, 0.0f, 20.0f);
    options.windX = bounded(options.windX, 16.0f, -SpeedLimit, SpeedLimit);
    options.windZ = bounded(options.windZ, 5.0f, -SpeedLimit, SpeedLimit);
    // Onda de gravidade até ~4 células por segundo: c·dt/Δx fica abaixo de 0,25 mesmo na grade de 32 u.
    options.gravity = bounded(options.gravity, ShallowGravity, 0.0f, 4.0e4f);
    options.confinement = bounded(options.confinement, ShallowConfinement, 0.0f, 12.0f);
    options.divergenceDamping = bounded(options.divergenceDamping, ShallowDivergenceDamping, 0.0f, 0.5f);
    options.bladeCells = bounded(options.bladeCells, ShallowBladeCells, 0.0f, 4.0f);
    options.viscosity = bounded(options.viscosity, ShallowViscosity, 0.0f, 0.2f);
    options.bladeGain = bounded(options.bladeGain, ShallowBladeGain, 0.0f, 4.0f);
    options.attackCap = bounded(options.attackCap, ShallowAttackCap, 1.0f, SpeedLimit);
    options.bladeRadial = bounded(options.bladeRadial, ShallowBladeRadial, 0.0f, 4.0f);
    options_ = options;
    remainder_ = 0.0;
    projectionStats_ = {};
}

float Field::circulation(float x, float z) const {
    double sum = 0.0;
    int count = 0;
    for (int cz = 0; cz < Side; ++cz) {
        for (int cx = 0; cx < Side; ++cx) {
            const Cell& c = cells_[index(cx, cz)];
            const double dx = worldX(cx) - x, dz = worldZ(cz) - z, r = std::hypot(dx, dz);
            if (!c.open || r < 0.9 * spacing_ || r > 4.1 * spacing_) continue;
            sum += (-dz * c.vx + dx * c.vz) / r;
            ++count;
        }
    }
    return count ? float(sum / count) : 0.0f;
}

float Field::lowestDensity(float x, float z, float radius) const {
    float lowest = MaxDensity;
    bool found = false;
    for (int cz = 0; cz < Side; ++cz) {
        for (int cx = 0; cx < Side; ++cx) {
            const Cell& c = cells_[index(cx, cz)];
            if (!c.open || std::hypot(worldX(cx) - x, worldZ(cz) - z) >= radius) continue;
            lowest = std::min(lowest, c.density);
            found = true;
        }
    }
    return found ? lowest : RestDensity;
}

float volumeExtinction(float extinction, float densityScale) {
    return std::clamp(extinction * densityScale, 0.0f, 1.0f);
}

SolverOptions shallowOptions() {
    SolverOptions options;
    options.transport = Transport::Shallow;
    options.damping = ShallowDamping;
    options.recovery = ShallowRecovery;
    return options;
}

void Field::setWind(float x, float z) {
    // Só o portador: não zera o acumulador do passo nem as estatísticas, porque muda a cada tick.
    options_.windX = std::isfinite(x) ? std::clamp(x, -SpeedLimit, SpeedLimit) : 0.0f;
    options_.windZ = std::isfinite(z) ? std::clamp(z, -SpeedLimit, SpeedLimit) : 0.0f;
}

bool Field::link(int x, int z, int nx, int nz) const {
    if (nx < 0 || nx >= Side || nz < 0 || nz >= Side) return false;
    const auto& a = cells_[index(x, z)];
    const auto& b = cells_[index(nx, nz)];
    const int d = nx < x ? 0 : nx > x ? 1 : nz < z ? 2 : 3;
    return connected(a, b) && !(a.blocked & (1u << d)) && !(b.blocked & (1u << (d ^ 1)));
}

bool Field::surfaceQuadOpen(int x, int z) const {
    if (x < 0 || z < 0 || x >= Side - 1 || z >= Side - 1) return false;
    return link(x, z, x + 1, z) && link(x, z, x, z + 1) &&
           link(x + 1, z + 1, x + 1, z) && link(x + 1, z + 1, x, z + 1);
}

Point Field::trace(int x, int z, float dx, float dz) const {
    // Exact grid DDA: stop before the first solid face, including diagonal corner contacts.
    const int sx = dx < 0.0f ? -1 : 1, sz = dz < 0.0f ? -1 : 1;
    const float ax = std::abs(dx), az = std::abs(dz);
    const float tx = ax > 0.0f ? 1.0f / ax : 1.0e30f;
    const float tz = az > 0.0f ? 1.0f / az : 1.0e30f;
    float nextX = 0.5f * tx, nextZ = 0.5f * tz;
    int cx = x, cz = z;
    for (int crossing = 0; crossing < 2 * Side + 2; ++crossing) {
        const float t = std::min(nextX, nextZ);
        if (t >= 1.0f) return {x + dx, 0.0f, z + dz};
        const bool crossX = nextX <= nextZ, crossZ = nextZ <= nextX;
        bool allowed = true;
        if (crossX) allowed &= link(cx, cz, cx + sx, cz);
        if (crossZ) allowed &= link(cx, cz, cx, cz + sz);
        if (crossX && crossZ && allowed) {
            allowed &= link(cx + sx, cz, cx + sx, cz + sz) &&
                       link(cx, cz + sz, cx + sx, cz + sz);
        }
        if (!allowed) {
            const float safe = std::max(0.0f, t - 0.0001f / std::max({ax, az, 1.0f}));
            return {x + dx * safe, 0.0f, z + dz * safe};
        }
        if (crossX) { cx += sx; nextX += tx; }
        if (crossZ) { cz += sz; nextZ += tz; }
    }
    return {float(x), 0.0f, float(z)};
}

Field::Sample Field::sample(const Grid& grid, int x, int z, Point p, float Cell::*member) const {
    const int x0 = int(std::floor(p.x)), z0 = int(std::floor(p.z));
    const float fx = p.x - x0, fz = p.z - z0;
    float total = 0.0f, weight = 0.0f;
    Sample result{0.0f, 1.0e30f, -1.0e30f};
    for (int dz = 0; dz <= 1; ++dz) for (int dx = 0; dx <= 1; ++dx) {
        const int nx = x0 + dx, nz = z0 + dz;
        const float w = (dx ? fx : 1.0f - fx) * (dz ? fz : 1.0f - fz);
        if (w <= 0.0f || nx < 0 || nx >= Side || nz < 0 || nz >= Side) continue;
        // A donor must be visible from the arrival cell: no interpolation through a wall/deck.
        const Point reached = trace(x, z, float(nx - x), float(nz - z));
        if (std::abs(reached.x - nx) > 0.001f || std::abs(reached.z - nz) > 0.001f) continue;
        const float v = grid[index(nx, nz)].*member;
        total += w * v;
        weight += w;
        result.low = std::min(result.low, v);
        result.high = std::max(result.high, v);
    }
    if (weight == 0.0f) {
        const float v = grid[index(x, z)].*member;
        return {v, v, v};
    }
    result.value = total / weight;
    return result;
}

Point Field::departure(const Grid& velocity, int x, int z, float dt) const {
    const float ratio = dt / spacing_;
    const auto& c = velocity[index(x, z)];
    const Point half = trace(x, z, -0.5f * ratio * c.vx, -0.5f * ratio * c.vz);
    const float vx = sample(velocity, x, z, half, &Cell::vx).value;
    const float vz = sample(velocity, x, z, half, &Cell::vz).value;
    return trace(x, z, -ratio * vx, -ratio * vz);
}

void Field::project() {
    // Staggered MAC faces give a matching divergence / Poisson / gradient triple.
    // Missing cells, excessive floor differences, blocked faces and the outer rim are solid.
    faceX_.fill(0.0f);
    faceZ_.fill(0.0f);
    poisson_.fill(0.0f);
    divergence_.fill(0.0f);
    for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
        const int i = index(x, z);
        if (!cells_[i].open) continue;
        if (link(x, z, x + 1, z))
            faceX_[z * (Side + 1) + x + 1] = 0.5f * (flow_[i].vx + flow_[i + 1].vx);
        if (link(x, z, x, z + 1))
            faceZ_[(z + 1) * Side + x] = 0.5f * (flow_[i].vz + flow_[i + Side].vz);
    }
    constexpr int dx[] = {-1, 1, 0, 0}, dz[] = {0, 0, -1, 1};
    for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
        auto& neighbors = links_[index(x, z)];
        neighbors.fill(-1);
        if (!cells_[index(x, z)].open) continue;
        for (int d = 0; d < 4; ++d)
            if (link(x, z, x + dx[d], z + dz[d])) neighbors[d] = index(x + dx[d], z + dz[d]);
    }
    int count = 0;
    projectionStats_ = {};
    for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
        const int i = index(x, z);
        if (!cells_[i].open) continue;
        divergence_[i] = (faceX_[z * (Side + 1) + x + 1] - faceX_[z * (Side + 1) + x] +
                          faceZ_[(z + 1) * Side + x] - faceZ_[z * Side + x]) / spacing_;
        projectionStats_.meanBefore += std::abs(divergence_[i]);
        ++count;
    }
    // p represents dt * physical_pressure / density. Neumann on all solid faces.
    for (int iteration = 0; iteration < options_.pressureIterations; ++iteration) {
        for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
            const int i = index(x, z);
            if (!cells_[i].open) continue;
            float sum = 0.0f;
            int degree = 0;
            for (int neighbor : links_[i]) if (neighbor >= 0) {
                sum += poisson_[neighbor];
                ++degree;
            }
            poisson_[i] = degree ? (sum - spacing_ * spacing_ * divergence_[i]) / degree : 0.0f;
        }
    }
    float peak = SpeedLimit;
    for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
        const int i = index(x, z);
        if (!cells_[i].open) continue;
        if (link(x, z, x + 1, z)) {
            auto& u = faceX_[z * (Side + 1) + x + 1];
            u -= (poisson_[i + 1] - poisson_[i]) / spacing_;
            peak = std::max(peak, std::abs(u));
        }
        if (link(x, z, x, z + 1)) {
            auto& v = faceZ_[(z + 1) * Side + x];
            v -= (poisson_[i + Side] - poisson_[i]) / spacing_;
            peak = std::max(peak, std::abs(v));
        }
    }
    // Uniform limiting preserves the divergence reduction (independent face clamps do not).
    const float scale = SpeedLimit / peak;
    for (auto& v : faceX_) v *= scale;
    for (auto& v : faceZ_) v *= scale;
    for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
        const int i = index(x, z);
        if (!cells_[i].open) continue;
        flow_[i].vx = 0.5f * (faceX_[z * (Side + 1) + x] + faceX_[z * (Side + 1) + x + 1]);
        flow_[i].vz = 0.5f * (faceZ_[z * Side + x] + faceZ_[(z + 1) * Side + x]);
        const float div = std::abs((faceX_[z * (Side + 1) + x + 1] - faceX_[z * (Side + 1) + x] +
                                   faceZ_[(z + 1) * Side + x] - faceZ_[z * Side + x]) / spacing_);
        projectionStats_.meanAfter += div;
        projectionStats_.maxAfter = std::max(projectionStats_.maxAfter, div);
    }
    if (count) {
        projectionStats_.meanBefore /= count;
        projectionStats_.meanAfter /= count;
    }
}

void Field::stepFluid(float liftStrength) {
    advanceAttackPressure(liftStrength);
    stepped_ = true;
    source_ = cells_;
    flow_ = cells_;
    // Wind is the background carrier; Cell velocities remain perturbations, as in the legacy API.
    for (int i = 0; i < Count; ++i) if (cells_[i].open) {
        flow_[i].vx += options_.windX;
        flow_[i].vz += options_.windZ;
    }
    const float damping = std::exp(-options_.damping * StepSeconds);
    advected_ = source_;
    for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
        const int i = index(x, z);
        if (!cells_[i].open) continue;
        const Point p = departure(flow_, x, z, StepSeconds);
        advected_[i].vx = sample(source_, x, z, p, &Cell::vx).value * damping;
        advected_[i].vz = sample(source_, x, z, p, &Cell::vz).value * damping;
    }
    for (int i = 0; i < Count; ++i) {
        flow_[i].vx = advected_[i].vx + options_.windX;
        flow_[i].vz = advected_[i].vz + options_.windZ;
    }
    project();
    // Forward density/lift advection; ping-pong source remains immutable.
    for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
        const int i = index(x, z);
        if (!cells_[i].open) continue;
        const Point p = departure(flow_, x, z, StepSeconds);
        advected_[i].density = sample(source_, x, z, p, &Cell::density).value;
        advected_[i].raisedDensity = sample(source_, x, z, p, &Cell::raisedDensity).value;
        advected_[i].verticalSpeed = sample(source_, x, z, p, &Cell::verticalSpeed).value;
    }
    const float refill = options_.recovery * StepSeconds;
    for (int z = 0; z < Side; ++z) for (int x = 0; x < Side; ++x) {
        const int i = index(x, z);
        if (!cells_[i].open) continue;
        const Point p = departure(flow_, x, z, StepSeconds);
        const Point reverse = departure(flow_, x, z, -StepSeconds);
        const auto corrected = [&](float Cell::*member) {
            const Sample donor = sample(source_, x, z, p, member);
            if (!options_.correctDensity) return advected_[i].*member;
            const float backward = sample(advected_, x, z, reverse, member).value;
            return std::clamp(advected_[i].*member + 0.5f * (source_[i].*member - backward), donor.low, donor.high);
        };
        auto& dest = cells_[i];
        dest.vx = std::clamp(flow_[i].vx - options_.windX, -SpeedLimit, SpeedLimit);
        dest.vz = std::clamp(flow_[i].vz - options_.windZ, -SpeedLimit, SpeedLimit);
        dest.density = std::clamp(corrected(&Cell::density), 0.0f, MaxDensity);
        dest.raisedDensity = corrected(&Cell::raisedDensity);
        dest.verticalSpeed = advected_[i].verticalSpeed;
    }
    if (options_.conserveMass) conserveDensity();
    for (int i = 0; i < Count; ++i) {
        auto& dest = cells_[i];
        if (!dest.open) continue;
        dest.density = std::lerp(dest.density, RestDensity, refill);
        float raised = std::clamp(dest.raisedDensity, 0.0f, dest.density);
        const float fraction = raised / std::max(dest.density, 0.0001f);
        const float speed = (dest.verticalSpeed - StepSeconds * (24.0f + 112.0f * fraction)) *
                            std::exp(-2.4f * StepSeconds);
        const float available = speed > 0.0f ? dest.density - raised : std::min(10.0f * raised, dest.density);
        raised += (speed * StepSeconds / (0.65f * LayerHeight)) * available;
        dest.raisedDensity = std::clamp(raised, 0.0f, 0.70f * dest.density);
        dest.verticalSpeed = speed;
        if (dest.raisedDensity <= 0.0005f && speed <= 0.0f) {
            dest.raisedDensity = 0.0f;
            dest.verticalSpeed = 0.0f;
        }
    }
}
} // namespace fog

namespace fog {
void Field::conserveDensity() {
    // Transport only: source_ already includes impulses; recovery is applied afterwards.
    // Never exchange mass across a solid, a blocked edge or a disconnected deck.
    std::array<bool, Count> visited{};
    std::array<int, Count> queue{};
    for (int seed = 0; seed < Count; ++seed) {
        if (visited[seed] || !cells_[seed].open) continue;
        int length = 1;
        queue[0] = seed;
        visited[seed] = true;
        double target = 0.0, mass = 0.0;
        float maximum = 0.0f;
        for (int head = 0; head < length; ++head) {
            const int i = queue[head];
            target += source_[i].density;
            mass += cells_[i].density;
            maximum = std::max(maximum, source_[i].density);
            for (int neighbor : links_[i]) if (neighbor >= 0 && !visited[neighbor]) {
                visited[neighbor] = true;
                queue[length++] = neighbor;
            }
        }
        if (mass == 0.0 && target > 0.0) {
            // A sub-cell feature can be missed entirely by pull advection: retain its source support.
            for (int n = 0; n < length; ++n) cells_[queue[n]].density = source_[queue[n]].density;
            continue;
        }
        if (mass <= 0.0) continue;
        if (target <= mass) {
            const float scale = float(target / mass);
            for (int n = 0; n < length; ++n) cells_[queue[n]].density *= scale;
        } else {
            // Redistribute missing mass by available headroom, weighted by existing density.
            // A final headroom pass handles saturated support without an unbounded iteration loop.
            for (int iteration = 0; iteration < 5; ++iteration) {
                double weight = 0.0;
                for (int n = 0; n < length; ++n) {
                    const float d = cells_[queue[n]].density;
                    weight += (maximum - d) * (iteration < 4 ? d : 1.0f);
                }
                if (weight <= 0.0) break;
                const double factor = (target - mass) / weight;
                mass = 0.0;
                for (int n = 0; n < length; ++n) {
                    auto& d = cells_[queue[n]].density;
                    d = std::clamp(float(d + factor * (maximum - d) * (iteration < 4 ? d : 1.0f)), 0.0f, maximum);
                    mass += d;
                }
                if (std::abs(target - mass) < 1.e-7 * std::max(target, 1.0)) break;
            }
        }
    }
}
} // namespace fog
