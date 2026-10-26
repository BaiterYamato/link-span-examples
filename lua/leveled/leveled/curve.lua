-- Curva de XP do Leveled 1.1.3.6 (leveled_stat_math.c: sExpTable, GetActorStat_NextLevelExp e
-- Player_GainExperience).
local curve = {}

curve.MAX_LEVEL = 99
curve.MAX_XP = 999999

-- EXP[n] é o XP acumulado para chegar ao nível n + 1.
local EXP = { [0] = 0,
    30, 64, 105, 155, 217, 292, 385, 497, 632, 792,
    981, 1202, 1458, 1753, 2089, 2471, 2903, 3388, 3930, 4534, 5203,
    5942, 6755, 7648, 8625, 9691, 10851, 12110, 13474, 14949, 16540, 18253,
    20094, 22070, 24188, 26453, 28874, 31456, 34209, 37139, 40254, 43562, 47073,
    50794, 54734, 58904, 63312, 67968, 72883, 78067, 83531, 89286, 95345, 101719,
    108421, 115463, 122859, 130623, 138770, 147314, 156270, 165655, 175485, 185778, 196552,
    207824, 219614, 231943, 244831, 258300, 272372, 287071, 302422, 318450, 335182, 352645,
    370870, 389885, 409724, 430419, 452005, 474520, 498001, 522489, 548027, 574659, 602434,
    631400, 661611, 693122, 725993, 760287, 796070, 833414, 872395, 913094, 955597, 999999 }

function curve.level_for_xp(xp)
    local level = 1
    while level < curve.MAX_LEVEL and xp >= EXP[level] do
        level = level + 1
    end
    return level
end

-- XP que falta para o próximo nível; 0 no 99.
function curve.xp_to_next(level, xp)
    if level >= curve.MAX_LEVEL then
        return 0
    end
    local left = EXP[level] - xp
    if left < 0 then
        left = 0
    end
    return left
end

-- XP acumulado no começo do nível, para a barra.
function curve.xp_at_level(level)
    return EXP[level - 1]
end

-- Ganho de XP como Player_GainExperience: taxa com round, teto de 999999, nada no 99.
-- Devolve o XP novo, o nível novo e quantos níveis subiu.
function curve.gain(xp, amount, rate)
    local level = curve.level_for_xp(xp)
    if level >= curve.MAX_LEVEL then
        return xp, level, 0
    end
    local scaled = math.floor(amount * (rate or 1.0) + 0.5)
    if scaled < 0 then
        scaled = 0
    elseif scaled > 0xFFFF then
        scaled = 0xFFFF
    end
    local new_xp = math.min(xp + scaled, curve.MAX_XP)
    local new_level = curve.level_for_xp(new_xp)
    return new_xp, new_level, new_level - level
end

return curve
