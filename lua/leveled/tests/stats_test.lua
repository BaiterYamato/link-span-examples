local stats = require("leveled.stats")
local defaults = { multiplier = 9, hp_percent = 1.0 }
local function near(expected, actual) assert.is_true(math.abs(expected - actual) < 1e-3, expected .. " ~= " .. actual) end
local function no_rng() return 0 end

describe("stats", function()
    it("D4: taxa de token também multiplica em f32 antes de truncar", function()
        assert.equals(483, stats.token_xp(21, 2.3))
    end)
    it("D5: trunca ataque e vida depois das operações f32 do C", function()
        assert.equals(3608, stats.attack(225, 82))
        for _, vector in ipairs({
            { 1, 70, 9, 63 }, { 1, 71, 9, 63 }, { 2, 35, 9, 63 },
            { 3, 94, 32, 938 }, { 3, 95, 32, 938 }, { 5, 10, 9, 63 },
            { 5, 53, 9, 238 }, { 5, 54, 9, 238 }, { 5, 57, 32, 910 },
            { 5, 58, 32, 910 }, { 6, 85, 9, 469 }, { 7, 39, 9, 245 },
        }) do
            assert.equals(vector[4], stats.enemy_max_health(vector[1], vector[2],
                { multiplier = vector[3], hp_percent = 0.7 }))
        end
    end)
    it("D5: dano f32 com sorteios iguais ao C nos dois alvos", function()
        for _, vector in ipairs({
            { false, 18, 78, 25, 0.5, 459 }, { false, 36, 86, 30, 0, 1002 },
            { false, 72, 76, 50, 0.5, 1365 }, { false, 72, 78, 20, 0, 1819 },
            { false, 72, 95, 0, 0.5, 3559 }, { false, 144, 83, 15, 0.5, 4609 },
            { false, 144, 85, 30, 0.5, 4169 }, { true, 9, 64, 0, 0.5, 262 },
            { true, 9, 88, 5, 0, 700 }, { true, 36, 81, 0, 0.5, 2529 },
            { true, 36, 85, 0, 0.5, 3106 }, { true, 36, 92, 55, 0, 484 },
        }) do
            assert.equals(vector[6], stats.modify(vector[1], vector[2], vector[3], vector[4], true,
                function(n) return n * vector[5] end))
        end
    end)
    it("D8: fórmula mantém o mínimo 1 mesmo com base zero", function()
        assert.equals(1, stats.enemy_max_health(0, 1, defaults))
    end)
    it("stats por nível", function()
        assert.equals(2, stats.player_power(1)); assert.equals(77, stats.player_power(99))
        assert.equals(3, stats.player_courage(1)); assert.equals(87, stats.player_courage(99))
        assert.equals(9, stats.player_power(10))
    end)
    it("vida escalada (vetor da análise)", function()
        assert.equals(535, stats.enemy_max_health(4, 99, defaults))
        assert.equals(18, stats.enemy_max_health(2, 1, defaults))
        assert.equals(200, stats.enemy_max_health(6, 24, defaults))
        assert.equals(504, stats.enemy_max_health(8, 50, defaults))
    end)
    it("corações bônus", function()
        assert.equals(0, stats.bonus_hearts(1)); assert.equals(1, stats.bonus_hearts(7))
        assert.equals(6, stats.bonus_hearts(50)); assert.equals(10, stats.bonus_hearts(99))
    end)
    it("fórmulas", function()
        near(9.0, stats.formula(9, stats.player_power(1), stats.enemy_courage(1)))
        near(36.36, stats.formula(18, stats.player_power(10), stats.enemy_courage(9)))
        near(5.20156, stats.formula(9, stats.player_power(1), stats.enemy_courage(24)))
        near(8.0576, stats.formula_on_player(8, stats.enemy_power(1), stats.player_courage(1), true))
        near(22.11038, stats.formula_on_player(8, stats.enemy_power(24), stats.player_courage(5), true))
    end)
    it("modify: arredondamento, mínimo e teto", function()
        -- 9.0 - 0.2 = 8.8; 8.8 - 8.8 * 0.06 = 8.272 -> 8
        assert.equals(8, stats.modify(false, 9, stats.player_power(1), stats.enemy_courage(1), true, no_rng))
        assert.equals(1, stats.modify(false, 1, 0, 255, true, no_rng))
        assert.equals(0, stats.modify(false, 0, 99, 2, true, no_rng))
        assert.equals(9999, stats.modify(false, 5000, 255, 0, true, no_rng))
    end)
    it("recompensa e token", function()
        assert.equals(2, stats.reward(1, 50)); assert.equals(9, stats.reward(10, 100))
        assert.equals(28, stats.reward(24, 116)); assert.equals(463, stats.reward(99, 100))
        assert.equals(0, stats.reward(50, 0))
        assert.equals(10, stats.token_xp(1, 1.0)); assert.equals(75, stats.token_xp(10, 1.0))
        assert.equals(2955, stats.token_xp(100, 1.0))
    end)
end)
