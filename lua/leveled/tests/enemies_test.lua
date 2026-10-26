local enemies = require("leveled.enemies")
local ids = require("leveled.ids")

local function ctx(t)
    t.category = t.category or 5
    t.params = t.params or 0
    t.age = t.age or 0
    t.player_level = t.player_level or 1
    t.gs_tokens = t.gs_tokens or 0
    t.level_scale = t.level_scale or 1.0
    t.ganon_max_level = t.ganon_max_level or false
    return t
end

describe("inimigos", function()
    it("ids conferem com o jogo", function()
        assert.equals(0, ids.scene.DEKU_TREE); assert.equals(81, ids.scene.HYRULE_FIELD)
        assert.equals(85, ids.scene.KOKIRI_FOREST); assert.equals(0x13, ids.actor.EN_FIREFLY)
        assert.equals(0x55, ids.actor.EN_DEKUBABA); assert.equals(2, ids.actor.EN_TEST)
    end)
    it("nível de cena por idade", function()
        assert.equals(5, enemies.scene_level(ids.scene.HYRULE_FIELD, 1, 0, false))
        assert.equals(20, enemies.scene_level(ids.scene.HYRULE_FIELD, 0, 0, false))
        assert.equals(52, enemies.scene_level(ids.scene.INSIDE_GANONS_CASTLE, 0, 100, false))
        assert.equals(120, enemies.scene_level(ids.scene.INSIDE_GANONS_CASTLE, 0, 100, true))
    end)
    it("Keese no Fire Temple", function()
        local r = enemies.resolve(ctx{ actor_id = ids.actor.EN_FIREFLY, scene = ids.scene.FIRE_TEMPLE })
        assert.equals(29, r.level); assert.equals(17, r.exp)
    end)
    it("Keese de gelo na Ice Cavern", function()
        local r = enemies.resolve(ctx{ actor_id = ids.actor.EN_FIREFLY, params = 4, scene = ids.scene.ICE_CAVERN })
        assert.equals(33, r.level); assert.equals(26, r.exp)
    end)
    it("Deku Baba na Deku Tree e Big Baba adulto na floresta", function()
        local r = enemies.resolve(ctx{ actor_id = ids.actor.EN_DEKUBABA, scene = ids.scene.DEKU_TREE, age = 1 })
        assert.equals(1, r.level); assert.equals(2, r.exp)
        r = enemies.resolve(ctx{ actor_id = ids.actor.EN_DEKUBABA, params = 1, scene = ids.scene.KOKIRI_FOREST })
        assert.equals(22, r.level); assert.equals(24, r.exp)
    end)
    it("escala de nível e ator sem tabela", function()
        local r = enemies.resolve(ctx{ actor_id = ids.actor.EN_FIREFLY, scene = ids.scene.FIRE_TEMPLE, level_scale = 1.5 })
        assert.equals(43, r.level); assert.equals(37, r.exp)
        r = enemies.resolve(ctx{ actor_id = 0x0FFF, scene = ids.scene.HYRULE_FIELD, age = 1 })
        assert.equals(5, r.level); assert.equals(0, r.exp)
    end)
    it("Castelo de Ganon com 100 tokens e a opção ligada", function()
        local r = enemies.resolve(ctx{ actor_id = ids.actor.EN_DEKUBABA, params = 1, scene = ids.scene.INSIDE_GANONS_CASTLE,
                                       gs_tokens = 100, ganon_max_level = true })
        assert.equals(99, r.level)
    end)
end)

describe("inimigos: fidelidade ao C", function()
    it("sem cena mantém a espécie ou usa o nível do jogador", function()
        local r = enemies.resolve{ actor_id = ids.actor.EN_FIREFLY, scene = -1, level_scale = 10 }
        assert.equals(9, r.level); assert.equals(5, r.exp)
        r = enemies.resolve{ actor_id = 0x0FFF, scene = -1, player_level = 42 }
        assert.equals(42, r.level); assert.equals(0, r.exp)
        assert.equals(-1, enemies.scene_level(-1, 1, 0, false))
    end)
    it("Poh compositor também cai em Poh_Field", function()
        local r = enemies.resolve{ actor_id = ids.actor.EN_POH, params = 2, scene = -1 }
        assert.equals(24, r.level); assert.equals(25, r.exp)
    end)
    it("Dark Link ignora cena e escala e recebe metade do intervalo de XP", function()
        local r = enemies.resolve{ actor_id = ids.actor.EN_TORCH2, scene = ids.scene.FIRE_TEMPLE,
                                   player_level = 10, level_scale = 10 }
        assert.equals(10, r.level); assert.equals(80, r.exp)
        r = enemies.resolve{ actor_id = ids.actor.EN_TORCH2, player_level = 99 }
        assert.equals(99, r.level); assert.equals(0, r.exp)
    end)
    it("Gold Skulltula ignora cena e respeita o limite público de nível", function()
        local r = enemies.resolve{ actor_id = ids.actor.EN_SW, params = 0x2000,
                                   scene = ids.scene.DEKU_TREE, gs_tokens = 100, level_scale = 0 }
        assert.equals(99, r.level); assert.equals(0, r.exp)
        r = enemies.resolve{ actor_id = ids.actor.EN_SW, params = 0, scene = ids.scene.DEKU_TREE }
        assert.equals(1, r.level); assert.equals(1, r.exp)
    end)
    it("taxas dos if sem chaves são incondicionais", function()
        local pairs = {
            { ids.actor.BOSS_GANONDROF, ids.scene.FOREST_TEMPLE, 0, 1 },
            { ids.actor.BOSS_TW, ids.scene.SPIRIT_TEMPLE, 0, 2 },
        }
        for _, pair in ipairs(pairs) do
            local a = enemies.resolve{ actor_id = pair[1], scene = pair[2], params = pair[3] }
            local b = enemies.resolve{ actor_id = pair[1], scene = pair[2], params = pair[4] }
            assert.equals(a.exp, b.exp); assert.is_true(a.exp > 0)
        end
        local a = enemies.resolve{ actor_id = ids.actor.BOSS_FD, scene = ids.scene.FIRE_TEMPLE }
        local b = enemies.resolve{ actor_id = ids.actor.BOSS_FD2, scene = ids.scene.FIRE_TEMPLE }
        assert.equals(a.exp, b.exp); assert.is_true(a.exp > 0)
    end)
    it("partes sem recompensa começam com XP zero", function()
        for _, actor in ipairs({ ids.actor.BOSS_VA, ids.actor.EN_DHA, ids.actor.EN_FD,
                                 ids.actor.EN_ANUBICE_FIRE, ids.actor.EN_BUBBLE }) do
            local r = enemies.resolve{ actor_id = actor, scene = -1, params = 0 }
            assert.equals(0, r.exp)
        end
    end)
    it("params de Stalchild truncam como os casts C", function()
        local r = enemies.resolve{ actor_id = ids.actor.EN_SKB, params = 5, scene = -1 }
        assert.equals(8, r.level); assert.equals(5, r.exp)
        r = enemies.resolve{ actor_id = ids.actor.EN_SKB, params = 5, scene = ids.scene.DEKU_TREE }
        assert.equals(5, r.level); assert.equals(3, r.exp)
    end)
    it("escala zero e defaults preservam a entrada", function()
        local input = { actor_id = ids.actor.EN_FIREFLY, scene = ids.scene.FIRE_TEMPLE, level_scale = 0 }
        local r = enemies.resolve(input)
        assert.equals(1, r.level); assert.equals(2, r.exp)
        assert.equals(nil, input.params); assert.equals(0, input.level_scale)
        r = enemies.resolve{ actor_id = ids.actor.EN_OKUTA, params = 1, scene = -1 }
        assert.equals(1, r.level); assert.equals(0, r.exp)
        r = enemies.resolve{ actor_id = ids.actor.EN_DEKUBABA, params = 1,
                             scene = ids.scene.INSIDE_GANONS_CASTLE, gs_tokens = 99, ganon_max_level = true }
        assert.equals(53, r.level)
    end)
end)
