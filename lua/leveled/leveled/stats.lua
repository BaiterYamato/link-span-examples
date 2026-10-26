-- Stats e fórmulas do Leveled 1.1.3.6 (leveled_stat_math.c). As conversões para inteiro imitam os casts u8/u16 do C.
local stats = {}

local function trunc(x)
    if x >= 0 then
        return math.floor(x)
    end
    return math.ceil(x)
end

local function clamp(x, lo, hi)
    if x < lo then
        return lo
    elseif x > hi then
        return hi
    end
    return x
end

function stats.player_power(level) return 2 + trunc(75 * level / 99) end
function stats.player_courage(level) return 3 + trunc(84 * level / 99) end
function stats.enemy_power(level) return 3 + trunc(84 * level / 99) end
function stats.enemy_courage(level) return 2 + trunc(75 * level / 99) end

-- GetActorStat_Attack: u16 na entrada e na saída. Acima de 65535 o valor segue; o C dá a volta no cast.
function stats.attack(attack, power)
    local p = power - 2
    return trunc(trunc(attack) * (1 + p * (0.14 + p * 0.0006)))
end

-- GetActorStat_EnemyAttack: u16 na entrada, float na saída.
function stats.enemy_attack(attack, power)
    local p = power - 2
    return trunc(attack) * (1 + p * (0.007 + p * 0.0002))
end

-- GetActorStat_EnemyMaxHealth. opts.multiplier (padrão 9), opts.hp_percent (padrão 1.0).
function stats.enemy_max_health(base, level, opts)
    local value = stats.attack(base * opts.multiplier, stats.player_power(level)) * opts.hp_percent
    return trunc(clamp(value, 1, 0xFFFF))
end

-- GetPlayerStat_BonusHearts: um coração a cada 8 níveis, até 10.
function stats.bonus_hearts(level)
    return math.min((level + 1) // 8, 10)
end

-- Leveled_DamageFormula: alvo que não é o Link.
function stats.formula(attack, power, courage)
    local damage = stats.attack(attack, power)
    if power >= courage then
        for _ = 1, power - courage do
            damage = damage * 1.01
        end
    else
        for _ = 1, courage - power do
            damage = damage * 0.97
        end
    end
    return damage
end

-- Leveled_DamageFormulaOnPlayer. scales = opção "Enemy Level Affects Base Attack".
function stats.formula_on_player(attack, power, courage, scales)
    local damage = attack
    if scales then
        damage = stats.enemy_attack(attack, power)
        if power >= courage then
            for i = 0, power - courage - 1 do
                local add = math.max((0.07 - power * 0.0005) - i / (100.0 - power * 0.33), 0)
                damage = damage * (1.04 + add)
            end
        else
            for _ = 1, courage - power do
                damage = damage * 0.96
            end
        end
    else
        if power >= courage then
            for i = 0, power - courage - 1 do
                damage = damage * (1.05 + math.max(0.05 - i / 100.0, 0))
            end
        else
            for _ = 1, courage - power do
                damage = damage * 0.96
            end
        end
    end
    return damage
end

-- Leveled_DamageModify. rng(n) devolve um float em [0, n).
function stats.modify(target_is_player, attack, power, courage, scales, rng)
    power = clamp(power, 0, 255)
    courage = clamp(courage, 0, 255)
    local damage
    if target_is_player then
        damage = stats.formula_on_player(attack, power, courage, scales)
    else
        damage = stats.formula(attack, power, courage)
    end
    if damage >= 1.25 then
        damage = damage + rng(1) - 0.2
    end
    if damage >= 6 then
        damage = damage + rng(damage * 0.12) - damage * 0.06
    end
    if damage > 9999 then
        damage = 9999
    end
    damage = trunc(damage + 0.5)
    local minimum = attack > 0 and 1 or 0
    if damage < minimum then
        damage = minimum
    end
    return damage
end

-- GetEnemyExperienceReward.
function stats.reward(level, rate)
    if rate == 0 then
        return 0
    end
    local value = (3 + math.min((level // 6) * 4, 4) +
        (level - 1) * ((0.1 + level / 95.0) + math.max(level - 8, 0) ^ 1.25 / 80.0)) * rate / 100.0
    return clamp(math.floor(value + 0.5), 1, 9999)
end

-- Leveled_GoldSkulltulaExperience: o acumulador é u16, truncado a cada passo.
function stats.token_xp(tokens, token_rate)
    local xp = 5
    for i = 0, tokens - 1 do
        xp = trunc(xp + 5 + 5 * i / 10.0)
    end
    return trunc(xp * token_rate)
end

return stats
