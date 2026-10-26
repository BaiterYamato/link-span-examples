-- Port de leveled_actor_level_table.c e leveled_map_levels.c (Leveled 1.1.3.6).
local ids = require("leveled.ids")
local stats = require("leveled.stats")
local curve = require("leveled.curve")
local S, A = ids.scene, ids.actor
local enemies = {}

local function trunc(x)
    return x >= 0 and math.floor(x) or math.ceil(x)
end

local function clamp_level(level)
    return math.max(1, math.min(99, trunc(level)))
end

-- Nível da cena; -1 sem tabela. O valor 120 só é cortado no resolve.
function enemies.scene_level(scene, age, gs_tokens, ganon_max_level)
    if scene == S.DEKU_TREE or scene == S.DEKU_TREE_BOSS then
        return 2
    end
    if scene == S.DODONGOS_CAVERN or scene == S.DODONGOS_CAVERN_BOSS then
        return 10
    end
    if scene == S.JABU_JABU or scene == S.JABU_JABU_BOSS then
        return 16
    end
    if scene == S.FOREST_TEMPLE or scene == S.FOREST_TEMPLE_BOSS then
        return 24
    end
    if scene == S.FIRE_TEMPLE or scene == S.FIRE_TEMPLE_BOSS then
        return 30
    end
    if scene == S.WATER_TEMPLE or scene == S.WATER_TEMPLE_BOSS then
        return 35
    end
    if scene == S.SPIRIT_TEMPLE or scene == S.SPIRIT_TEMPLE_BOSS then
        return 45
    end
    if scene == S.SHADOW_TEMPLE or scene == S.SHADOW_TEMPLE_BOSS then
        return 40
    end
    if scene == S.BOTTOM_OF_THE_WELL then
        return 38
    end
    if scene == S.ICE_CAVERN then
        return 32
    end
    if scene == S.INSIDE_GANONS_CASTLE or scene == S.GANONS_TOWER or scene == S.GANONDORF_BOSS or scene == S.GANONS_TOWER_COLLAPSE_EXTERIOR or scene == S.GANON_BOSS then
        if ganon_max_level and gs_tokens == 100 then
            return 120
        else
            return 52
        end
    end
    if scene == S.GERUDO_TRAINING_GROUND then
        return 43
    end
    if scene == S.THIEVES_HIDEOUT then
        return 42
    end
    if scene == S.GANONS_TOWER_COLLAPSE_INTERIOR or scene == S.INSIDE_GANONS_CASTLE_COLLAPSE then
        return 53
    end
    if scene == S.MARKET_RUINS then
        return 23
    end
    if scene == S.REDEAD_GRAVE then
        return 10
    end
    if scene == S.ROYAL_FAMILYS_TOMB then
        return 8
    end
    if scene == S.WINDMILL_AND_DAMPES_GRAVE then
        return 22
    end
    if scene == S.HOUSE_OF_SKULLTULA then
        return 99
    end
    if scene == S.HYRULE_FIELD then
        if (age == 1) then
            return 5
        else
            return 20
        end
    end
    if scene == S.GRAVEYARD then
        if (age == 1) then
            return 7
        else
            return 20
        end
    end
    if scene == S.ZORAS_RIVER then
        if (age == 1) then
            return 8
        else
            return 24
        end
    end
    if scene == S.KOKIRI_FOREST then
        if (age == 1) then
            return 2
        else
            return 21
        end
    end
    if scene == S.SACRED_FOREST_MEADOW then
        if (age == 1) then
            return 9
        else
            return 23
        end
    end
    if scene == S.LAKE_HYLIA then
        if (age == 1) then
            return 7
        else
            return 22
        end
    end
    if scene == S.ZORAS_FOUNTAIN then
        if (age == 1) then
            return 13
        else
            return 30
        end
    end
    if scene == S.GERUDO_VALLEY then
        if (age == 1) then
            return 10
        else
            return 28
        end
    end
    if scene == S.LOST_WOODS then
        if (age == 1) then
            return 7
        else
            return 22
        end
    end
    if scene == S.DESERT_COLOSSUS then
        return 42
    end
    if scene == S.GERUDOS_FORTRESS then
        return 40
    end
    if scene == S.DEATH_MOUNTAIN_TRAIL then
        if (age == 1) then
            return 8
        else
            return 24
        end
    end
    if scene == S.DEATH_MOUNTAIN_CRATER then
        return 11
    end
    if scene == S.LON_LON_RANCH then
        return 6
    end
    if scene == S.OUTSIDE_GANONS_CASTLE then
        return 25
    end
    if scene == S.TEST01 or scene == S.BESITU or scene == S.DEPTH_TEST or scene == S.SYOTES or scene == S.SYOTES2 or scene == S.SUTARU or scene == S.HAIRAL_NIWA2 or scene == S.SASATEST or scene == S.TESTROOM then
        return 45
    end
    return -1
end

local function Leveled_Gohma(ctx, entry)
    entry.level = 7
    entry.exp = 120
    entry.modifier = 3
    entry.rate = 3000
    if ctx.category == 5 then
        entry.level = 5
        entry.modifier = 1
        entry.rate = 0
        if ctx.params < 10 then
            entry.exp = 5
            entry.rate = 100
        end
    end
end

local function Leveled_Dodongo(ctx, entry)
    if ctx.category == 9 then
        entry.level = 15
        entry.exp = 500
        entry.modifier = 5
        entry.rate = 4000
    else
        entry.level = 14
        entry.exp = 18
        entry.modifier = 4
        entry.rate = 160
    end
end

local function Leveled_Barinade(ctx, entry)
    entry.level = 21
    entry.modifier = 5
    if ctx.params == -1 then
        entry.exp = 850
        entry.rate = 5000
    end
end

local function Leveled_PhantomGanon(ctx, entry)
    entry.level = 28
    entry.modifier = 5
    if ctx.params == 1 then
        entry.exp = 1400
    end
    -- Sem chaves no C: a taxa é incondicional.
    entry.rate = 6000
end

local function Leveled_Volvagia(ctx, entry)
    entry.level = 35
    entry.modifier = 5
    if ctx.actor_id == A.BOSS_FD2 then
        entry.exp = 2475
    end
    -- Sem chaves no C: a taxa é incondicional.
    entry.rate = 6300
end

local function Leveled_Morpha(ctx, entry)
    entry.level = 40
    entry.exp = 3500
    entry.modifier = 5
    entry.rate = 6600
end

local function Leveled_BongoBongo(ctx, entry)
    entry.level = 45
    entry.exp = 4666
    entry.modifier = 5
    entry.rate = 7000
end

local function Leveled_Twinrova(ctx, entry)
    entry.level = 52
    entry.modifier = 6
    if ctx.params == 2 then
        entry.exp = 6000
    end
    -- Sem chaves no C: a taxa é incondicional.
    entry.rate = 7000
end

local function Leveled_Ganondorf(ctx, entry)
    entry.level = 56
    entry.exp = 9999
    entry.modifier = 5
    entry.rate = 40000
end

local function Leveled_Ganon(ctx, entry)
    entry.level = 60
    entry.modifier = 8
end

local function Leveled_BigOctorock(ctx, entry)
    entry.level = 20
    entry.exp = 320
    entry.modifier = 4
    entry.rate = 2100
end

local function Leveled_FlareDancer(ctx, entry)
    entry.level = 34
    entry.modifier = 4
    if ctx.actor_id == A.EN_FW then
        entry.exp = 1000
        entry.rate = 3000
    end
end

local function Leveled_DarkLink(ctx, entry)
    entry.level = ctx.player_level
    -- O helper C não tem return; usa-se a diferença pretendida da tabela.
    local span = 0
    if ctx.player_level < curve.MAX_LEVEL then
        span = curve.xp_at_level(ctx.player_level + 1) - curve.xp_at_level(ctx.player_level)
    end
    entry.exp = trunc(span * 0.50)
    entry.ignore = true
end

local function Leveled_DeadHand(ctx, entry)
    entry.level = 44
    entry.modifier = 4
    if ctx.actor_id == A.EN_DH then
        entry.exp = 2465
        entry.rate = 3666
    end
end

local function Leveled_IronKnuckle(ctx, entry)
    entry.level = 50
    entry.exp = 2733
    entry.modifier = 5
    entry.rate = 5000
    if ctx.params == 0 then
        entry.level = 52
        entry.exp = 3100
        entry.modifier = 6
    end
    if ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
        entry.level = 55
        entry.exp = 2850
    end
end

local function Leveled_Dekubaba(ctx, entry)
    if ctx.params == 1 then
        entry.level = 24
        entry.exp = 32
        entry.modifier = 1
        entry.rate = 116
        if (ctx.age == 1) then
            entry.level = 5
            entry.exp = 8
        end
    else
        entry.level = 2
        entry.exp = 3
        entry.modifier = -2
        entry.rate = 60
        if ctx.scene == S.FOREST_TEMPLE then
            entry.level = 23
            entry.exp = 19
        end
    end
end

local function Leveled_StickDekuBaba(ctx, entry)
    entry.level = 1
    entry.exp = 1
    entry.modifier = -99
    entry.rate = 1
end

local function Leveled_Skulltula(ctx, entry)
    entry.level = 3
    entry.exp = 3
    entry.modifier = 0
    entry.rate = 80
    if ctx.scene == S.FOREST_TEMPLE then
        entry.level = 21
        entry.exp = 19
    end
    if ctx.scene == S.BOTTOM_OF_THE_WELL or ctx.scene == S.SHADOW_TEMPLE then
        entry.level = 38
        entry.exp = 30
    elseif ctx.scene == S.SPIRIT_TEMPLE then
        entry.level = 44
        entry.exp = 34
    elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
        entry.level = 50
        entry.exp = 50
    end
    if ctx.params == 1 then
        entry.level = entry.level + 3
        entry.exp = entry.exp + 2
        entry.modifier = 1
        entry.rate = 90
    end
end

local function Leveled_Wall_Skulltula(ctx, entry)
    if ((ctx.params & 0xE000) >> 0xD) ~= 0 then
        entry.level = ctx.gs_tokens + 1
        entry.exp = 0
        entry.ignore = true
    else
        entry.level = 3
        entry.exp = 2
        entry.modifier = -2
        entry.rate = 40
        if ctx.scene == S.FOREST_TEMPLE then
            entry.level = 24
            entry.exp = 18
        end
    end
end

local function Leveled_DekuScrub(ctx, entry)
    entry.level = 8
    entry.exp = 7
    entry.modifier = 0
    entry.rate = 75
end

local function Leveled_Tektite(ctx, entry)
    if ctx.params == -2 then
        entry.level = 12
        entry.exp = 12
        entry.modifier = 2
        entry.rate = 100
        if (ctx.age == 0) then
            entry.level = 25
            entry.exp = 21
        end
        if ctx.scene == S.WATER_TEMPLE then
            entry.level = 36
            entry.exp = 44
        end
    else
        entry.level = 9
        entry.exp = 9
        entry.modifier = 1
        entry.rate = 90
        if (ctx.age == 0) then
            entry.level = 27
            entry.exp = 24
        end
    end
end

local function Leveled_Guay(ctx, entry)
    entry.level = 8
    entry.exp = 4
    entry.modifier = -1
    entry.rate = 50
    if (ctx.age == 0) then
        entry.level = 21
        entry.exp = 13
    end
end

local function Leveled_Octorock(ctx, entry)
    entry.level = 13
    entry.exp = 12
    entry.modifier = 0
    entry.rate = 70
    if ctx.params ~= 0 then
        entry.level = 0
        entry.exp = 0
        entry.modifier = 0
        entry.rate = 0
    end
end

local function Leveled_Armos(ctx, entry)
    entry.level = 12
    entry.exp = 17
    entry.modifier = 2
    entry.rate = 135
    if ctx.scene == S.SPIRIT_TEMPLE then
        entry.level = 45
        entry.exp = 60
    elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
        entry.level = 50
        entry.exp = 70
    end
end

local function Leveled_BabyDodongo(ctx, entry)
    entry.level = 11
    entry.exp = 7
    entry.modifier = 1
    entry.rate = 75
end

local function Leveled_Keese(ctx, entry)
    if ctx.params == 4 then
        entry.level = 34
        entry.exp = 33
        entry.modifier = 1
        entry.rate = 60
    else
        entry.level = 9
        entry.exp = 5
        entry.modifier = -1
        entry.rate = 50
        if ctx.scene == S.FIRE_TEMPLE then
            entry.level = 28
            entry.exp = 22
        elseif ctx.scene == S.ICE_CAVERN then
            entry.level = 31
            entry.exp = 26
        elseif ctx.scene == S.WATER_TEMPLE then
            entry.level = 34
            entry.exp = 29
        elseif ctx.scene == S.SPIRIT_TEMPLE then
            entry.level = 42
            entry.exp = 36
        elseif ctx.scene == S.BOTTOM_OF_THE_WELL or ctx.scene == S.SHADOW_TEMPLE then
            entry.level = 38
            entry.exp = 33
        elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
            entry.level = 50
            entry.exp = 38
        end
        if ctx.scene == S.GERUDO_TRAINING_GROUND then
            entry.level = 47
            entry.exp = 80
        end
    end
end

local function Leveled_Peahat(ctx, entry)
    entry.level = 18
    entry.exp = 47
    entry.modifier = 10
    entry.rate = 275
    if ctx.params == 1 then
        entry.level = 13
        entry.modifier = 5
        entry.rate = 0
    end
end

local function Leveled_Poh(ctx, entry)
    entry.level = 10
    entry.exp = 14
    entry.modifier = 0
    entry.rate = 130
    if ctx.params == 2 or ctx.params == 3 then
        entry.level = 15
        entry.exp = 25
        entry.modifier = 1
        entry.rate = 130
    end
end

local function Leveled_Poh_Field(ctx, entry)
    entry.level = 24
    entry.exp = 25
    entry.modifier = 0
    entry.rate = 130
end

local function Leveled_ReDead(ctx, entry)
    if ctx.params >= -1 then
        entry.level = 25
        entry.exp = 45
        entry.modifier = 2
        entry.rate = 280
        if ctx.scene == S.BOTTOM_OF_THE_WELL or ctx.scene == S.SHADOW_TEMPLE then
            entry.level = 42
            entry.exp = 171
        elseif ctx.scene == S.GANONS_TOWER_COLLAPSE_INTERIOR or ctx.scene == S.INSIDE_GANONS_CASTLE_COLLAPSE then
            entry.level = 50
            entry.exp = 230
        end
    else
        entry.level = 43
        entry.exp = 183
        entry.modifier = 3
        entry.rate = 305
    end
end

local function Leveled_Shabom(ctx, entry)
    entry.level = 15
    entry.modifier = -1
end

local function Leveled_Bari(ctx, entry)
    entry.level = 18
    entry.exp = 25
    entry.modifier = 2
    entry.rate = 85
end

local function Leveled_Biri(ctx, entry)
    entry.level = 16
    entry.exp = 17
    entry.modifier = 1
    entry.rate = 100
end

local function Leveled_Stinger(ctx, entry)
    entry.level = 17
    entry.exp = 19
    entry.modifier = 1
    entry.rate = 125
    if ctx.scene == S.WATER_TEMPLE then
        entry.level = 37
        entry.exp = 85
    end
end

local function Leveled_JabuTentacle(ctx, entry)
    entry.level = 19
    entry.modifier = 3
    if ctx.params < 3 then
        entry.exp = 61
        entry.rate = 400
    end
end

local function Leveled_Tailpasaran(ctx, entry)
    entry.level = 19
    entry.exp = 22
    entry.modifier = 3
    entry.rate = 125
end

local function Leveled_Stalchild(ctx, entry)
    -- Casts u16, campos u8/s8/u16 e divisão inteira como no C.
    local half = trunc(ctx.params / 2) % 0x10000
    entry.level = (6 + half) % 0x100
    entry.exp = (3 + trunc(ctx.params / 2.5) % 0x10000) % 0x10000
    entry.modifier = ((1 + half + 128) % 0x100) - 128
    entry.rate = (45 + (ctx.params * 5) % 0x10000) % 0x10000
end

local function Leveled_Beamos(ctx, entry)
    if ctx.params == 0 then
        entry.level = 48
        entry.exp = 90
        entry.modifier = 1
        entry.rate = 150
    else
        entry.level = 11
        entry.exp = 17
        entry.modifier = 0
        entry.rate = 150
        if ctx.scene == S.BOTTOM_OF_THE_WELL or ctx.scene == S.SHADOW_TEMPLE then
            entry.level = 43
            entry.exp = 80
        elseif ctx.scene == S.SPIRIT_TEMPLE then
            entry.level = 47
            entry.exp = 87
        elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
            entry.level = 50
            entry.exp = 90
        elseif ctx.scene == S.GERUDO_TRAINING_GROUND then
            entry.level = 47
            entry.exp = 300
        end
    end
end

local function Leveled_Wolfos(ctx, entry)
    if ctx.params == 0 then
        entry.level = 10
        entry.exp = 24
        entry.modifier = 1
        entry.rate = 180
        if (ctx.age == 0) then
            entry.level = 20
            entry.exp = 29
        end
        if ctx.scene == S.FOREST_TEMPLE then
            entry.level = 26
            entry.exp = 43
        elseif ctx.scene == S.SPIRIT_TEMPLE then
            entry.level = 47
            entry.exp = 158
        elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
            entry.level = 50
            entry.exp = 165
        end
    else
        entry.level = 37
        entry.exp = 126
        entry.modifier = 3
        entry.rate = 180
    end
    if ctx.scene == S.GERUDO_TRAINING_GROUND then
        entry.level = 47
        entry.exp = 420
    end
end

local function Leveled_Lizalfos(ctx, entry)
    if ctx.params == -2 then
        entry.level = 52
        entry.exp = 250
        entry.modifier = 4
        entry.rate = 320
    elseif ctx.params == -1 then
        entry.level = 47
        entry.exp = 210
        entry.modifier = 3
        entry.rate = 290
    else
        entry.level = 13
        entry.exp = 140
        entry.modifier = 3
        entry.rate = 1150
        -- ignoreExpReward é estado de runtime, fora desta interface pura.
    end
    if ctx.scene == S.GERUDO_TRAINING_GROUND then
        entry.level = 47
        entry.exp = 570
    end
end

local function Leveled_Moblins(ctx, entry)
    if ctx.params == -1 then
        entry.level = 24
        entry.exp = 32
        entry.modifier = 2
        entry.rate = 140
    elseif ctx.params == 0 then
        entry.level = 25
        entry.exp = 47
        entry.modifier = 3
        entry.rate = 240
    else
        entry.level = 24
        entry.exp = 24
        entry.modifier = 2
        entry.rate = 140
    end
end

local function Leveled_Bubble(ctx, entry)
    if ctx.params == -1 then
        entry.level = 25
        entry.exp = 29
        entry.modifier = 1
        entry.rate = 105
    elseif ctx.params == -2 then
        entry.level = 32
        entry.exp = 47
        entry.modifier = 1
        entry.rate = 110
        if ctx.scene == S.SHADOW_TEMPLE then
            entry.level = 40
            entry.exp = 75
        end
    elseif ctx.params == -3 then
        entry.level = 45
        entry.exp = 76
        entry.modifier = 1
        entry.rate = 107
    elseif ctx.params == -4 then
        entry.level = 41
        entry.exp = 82
        entry.modifier = 2
        entry.rate = 100
    else
        entry.level = 26
        entry.exp = 31
        entry.modifier = 3
        entry.rate = 93
        if ctx.scene == S.BOTTOM_OF_THE_WELL or ctx.scene == S.SHADOW_TEMPLE then
            entry.level = 41
            entry.exp = 70
        elseif ctx.scene == S.SPIRIT_TEMPLE then
            entry.level = 45
            entry.exp = 77
        elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
            entry.level = 50
            entry.exp = 85
        end
    end
    if ctx.scene == S.GERUDO_TRAINING_GROUND then
        entry.level = 47
        entry.exp = 220
    end
end

local function Leveled_Stalfos(ctx, entry)
    entry.level = 27
    entry.exp = 64
    entry.modifier = 4
    entry.rate = 345
    if ctx.scene == S.FOREST_TEMPLE then
        -- ignoreExpReward é estado de runtime, fora desta interface pura.
        entry.exp = 250
        entry.rate = 800
    elseif ctx.scene == S.SHADOW_TEMPLE then
        entry.level = 43
        entry.exp = 214
    elseif ctx.scene == S.SPIRIT_TEMPLE then
        entry.level = 48
        entry.exp = 236
    elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER or ctx.scene == S.GANONS_TOWER_COLLAPSE_INTERIOR or ctx.scene == S.INSIDE_GANONS_CASTLE_COLLAPSE then
        entry.level = 50
        entry.exp = 260
    elseif ctx.scene == S.GERUDO_TRAINING_GROUND then
        entry.level = 47
        entry.exp = 700
    end
end

local function Leveled_Floormaster(ctx, entry)
    entry.level = 26
    entry.exp = 41
    entry.modifier = 2
    entry.rate = 170
    -- ignoreExpReward é estado de runtime, fora desta interface pura.
    if ctx.scene == S.BOTTOM_OF_THE_WELL or ctx.scene == S.SHADOW_TEMPLE then
        entry.level = 42
        entry.exp = 140
    elseif ctx.scene == S.SPIRIT_TEMPLE then
        entry.level = 47
        entry.exp = 160
    end
end

local function Leveled_Wallmaster(ctx, entry)
    entry.level = 27
    entry.exp = 44
    entry.modifier = 2
    entry.rate = 175
    if ctx.scene == S.BOTTOM_OF_THE_WELL or ctx.scene == S.SHADOW_TEMPLE then
        entry.level = 41
        entry.exp = 129
    elseif ctx.scene == S.SPIRIT_TEMPLE then
        entry.level = 47
        entry.exp = 138
    elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
        entry.level = 50
        entry.exp = 150
    end
end

local function Leveled_Poh_Sisters(ctx, entry)
    entry.level = 28
    entry.exp = 127
    entry.modifier = 4
    entry.rate = 450
end

local function Leveled_LikeLike(ctx, entry)
    entry.level = 32
    entry.exp = 99
    entry.modifier = 2
    entry.rate = 245
    if ctx.scene == S.WATER_TEMPLE then
        entry.level = 38
        entry.exp = 142
    elseif ctx.scene == S.BOTTOM_OF_THE_WELL or ctx.scene == S.SHADOW_TEMPLE then
        entry.level = 43
        entry.exp = 161
    elseif ctx.scene == S.SPIRIT_TEMPLE then
        entry.level = 48
        entry.exp = 182
    elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
        entry.level = 50
        entry.exp = 190
    elseif ctx.scene == S.GERUDO_TRAINING_GROUND then
        entry.level = 47
        entry.exp = 375
    end
end

local function Leveled_TorchSlug(ctx, entry)
    entry.level = 33
    entry.exp = 67
    entry.modifier = 1
    entry.rate = 135
    if ctx.scene == S.SPIRIT_TEMPLE then
        entry.level = 48
        entry.exp = 163
    elseif ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
        entry.level = 50
        entry.exp = 170
    elseif ctx.scene == S.GERUDO_TRAINING_GROUND then
        entry.level = 47
        entry.exp = 350
    end
end

local function Leveled_Freezer(ctx, entry)
    entry.level = 35
    entry.exp = 101
    entry.modifier = 3
    entry.rate = 205
    if ctx.scene == S.INSIDE_GANONS_CASTLE or ctx.scene == S.GANONS_TOWER then
        entry.level = 50
        entry.exp = 160
    end
end

local function Leveled_ShellBlade(ctx, entry)
    entry.level = 38
    entry.exp = 129
    entry.modifier = 2
    entry.rate = 200
    if ctx.scene == S.GERUDO_TRAINING_GROUND then
        entry.level = 47
        entry.exp = 390
    end
end

local function Leveled_RollingSpike(ctx, entry)
    entry.level = 37
    entry.exp = 100
    entry.modifier = 1
    entry.rate = 180
    if ctx.scene == S.GERUDO_TRAINING_GROUND then
        entry.level = 47
        entry.exp = 325
    end
end

local function Leveled_GerudoFighter(ctx, entry)
    entry.level = 45
    entry.exp = 455
    entry.modifier = 2
    entry.rate = 1250
end

local function Leveled_Leever(ctx, entry)
    entry.level = 45
    entry.exp = 15
    entry.modifier = 0
    entry.rate = 20
end

local function Leveled_Anubis(ctx, entry)
    entry.level = 48
    entry.modifier = 3
    if ctx.actor_id == A.EN_ANUBICE then
        entry.exp = 120
        entry.rate = 155
    end
end

local function Leveled_Arwing(ctx, entry)
    entry.level = 20
    entry.exp = 50
    entry.modifier = 3
    entry.rate = 215
end

local species = {
    [A.BOSS_DODONGO] = Leveled_Dodongo,
    [A.EN_DODONGO] = Leveled_Dodongo,
    [A.BOSS_GOMA] = Leveled_Gohma,
    [A.EN_GOMA] = Leveled_Gohma,
    [A.BOSS_VA] = Leveled_Barinade,
    [A.BOSS_GANONDROF] = Leveled_PhantomGanon,
    [A.BOSS_FD] = Leveled_Volvagia,
    [A.BOSS_FD2] = Leveled_Volvagia,
    [A.BOSS_MO] = Leveled_Morpha,
    [A.BOSS_SST] = Leveled_BongoBongo,
    [A.BOSS_TW] = Leveled_Twinrova,
    [A.BOSS_GANON] = Leveled_Ganondorf,
    [A.BOSS_GANON2] = Leveled_Ganon,
    [A.EN_DEKUBABA] = Leveled_Dekubaba,
    [A.EN_KAREBABA] = Leveled_StickDekuBaba,
    [A.EN_ST] = Leveled_Skulltula,
    [A.EN_DEKUNUTS] = Leveled_DekuScrub,
    [A.EN_TITE] = Leveled_Tektite,
    [A.EN_CROW] = Leveled_Guay,
    [A.EN_OKUTA] = Leveled_Octorock,
    [A.EN_AM] = Leveled_Armos,
    [A.EN_DODOJR] = Leveled_BabyDodongo,
    [A.EN_FIREFLY] = Leveled_Keese,
    [A.EN_PEEHAT] = Leveled_Peahat,
    -- Falta break no C: EN_POH termina com a regra de Poh_Field (T0).
    [A.EN_POH] = Leveled_Poh_Field,
    [A.EN_PO_FIELD] = Leveled_Poh_Field,
    [A.EN_RD] = Leveled_ReDead,
    [A.EN_SKB] = Leveled_Stalchild,
    [A.EN_SW] = Leveled_Wall_Skulltula,
    [A.EN_VM] = Leveled_Beamos,
    [A.EN_WF] = Leveled_Wolfos,
    [A.EN_ZF] = Leveled_Lizalfos,
    [A.EN_BUBBLE] = Leveled_Shabom,
    [A.EN_BILI] = Leveled_Biri,
    [A.EN_VALI] = Leveled_Bari,
    [A.EN_TP] = Leveled_Tailpasaran,
    [A.EN_BA] = Leveled_JabuTentacle,
    [A.EN_BX] = Leveled_JabuTentacle,
    [A.EN_EIYER] = Leveled_Stinger,
    [A.EN_WEIYER] = Leveled_Stinger,
    [A.EN_BIGOKUTA] = Leveled_BigOctorock,
    [A.EN_MB] = Leveled_Moblins,
    [A.EN_BB] = Leveled_Bubble,
    [A.EN_TEST] = Leveled_Stalfos,
    [A.EN_FLOORMAS] = Leveled_Floormaster,
    [A.EN_WALLMAS] = Leveled_Wallmaster,
    [A.EN_PO_SISTERS] = Leveled_Poh_Sisters,
    [A.EN_RR] = Leveled_LikeLike,
    [A.EN_BW] = Leveled_TorchSlug,
    [A.EN_FZ] = Leveled_Freezer,
    [A.EN_SB] = Leveled_ShellBlade,
    [A.EN_NY] = Leveled_RollingSpike,
    [A.EN_TORCH2] = Leveled_DarkLink,
    [A.EN_DH] = Leveled_DeadHand,
    [A.EN_DHA] = Leveled_DeadHand,
    [A.EN_FD] = Leveled_FlareDancer,
    [A.EN_FW] = Leveled_FlareDancer,
    [A.EN_FD_FIRE] = Leveled_FlareDancer,
    [A.EN_IK] = Leveled_IronKnuckle,
    [A.EN_GELDB] = Leveled_GerudoFighter,
    [A.EN_REEBA] = Leveled_Leever,
    [A.EN_ANUBICE] = Leveled_Anubis,
    [A.EN_ANUBICE_FIRE] = Leveled_Anubis,
    [A.EN_CLEAR_TAG] = Leveled_Arwing,
}

-- ctx: actor_id, params, category, scene, age (0 adulto, 1 criança),
-- player_level, gs_tokens, level_scale (1.0) e ganon_max_level (false).
-- Devolve { level = 1..99, exp = inteiro >= 0 } sem alterar ctx.
function enemies.resolve(ctx)
    ctx = {
        actor_id = ctx.actor_id, params = ctx.params or 0,
        category = ctx.category or 5, scene = ctx.scene,
        age = ctx.age or 0, player_level = ctx.player_level or 1,
        gs_tokens = ctx.gs_tokens or 0, level_scale = ctx.level_scale or 1.0,
        ganon_max_level = ctx.ganon_max_level or false,
    }
    local entry = { level = ctx.player_level, exp = 0, modifier = 0, rate = 0, ignore = false }
    local resolve_species = species[ctx.actor_id]
    if resolve_species then
        resolve_species(ctx, entry)
    end
    local scene_level = enemies.scene_level(ctx.scene, ctx.age, ctx.gs_tokens, ctx.ganon_max_level)
    if scene_level >= 0 and not entry.ignore then
        entry.level = clamp_level((scene_level + entry.modifier) * ctx.level_scale)
        entry.exp = stats.reward(entry.level, entry.rate)
    end
    -- O contrato público também limita espécies sem cena (ex.: Gold Skulltula).
    return { level = clamp_level(entry.level), exp = math.max(0, trunc(entry.exp)) }
end

return enemies
