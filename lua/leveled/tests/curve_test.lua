local curve = require("leveled.curve")

describe("curva", function()
    it("D4: arredonda o produto f32 das taxas de XP", function()
        for _, vector in ipairs({
            { 25, 2.3, 58 }, { 45, 0.7, 32 }, { 45, 1.3, 58 }, { 45, 2.3, 104 },
            { 55, 2.3, 127 }, { 85, 0.7, 60 }, { 85, 1.3, 110 }, { 85, 2.3, 196 },
            { 95, 1.3, 123 }, { 95, 2.3, 219 }, { 105, 2.3, 242 }, { 165, 0.7, 116 },
        }) do
            assert.equals(vector[3], (curve.gain(0, vector[1], vector[2])))
        end
    end)
    it("nível por XP", function()
        assert.equals(1, curve.level_for_xp(0))
        assert.equals(1, curve.level_for_xp(29))
        assert.equals(2, curve.level_for_xp(30))
        assert.equals(2, curve.level_for_xp(63))
        assert.equals(3, curve.level_for_xp(64))
        assert.equals(98, curve.level_for_xp(999998))
        assert.equals(99, curve.level_for_xp(999999))
    end)
    it("XP até o próximo nível", function()
        assert.equals(30, curve.xp_to_next(1, 0))
        assert.equals(0, curve.xp_to_next(99, 999999))
        assert.equals(0, curve.xp_at_level(1))
        assert.equals(30, curve.xp_at_level(2))
    end)
    it("ganho com taxa, teto e nível 99", function()
        local xp, level, ups = curve.gain(0, 30, 1.0)
        assert.equals(30, xp); assert.equals(2, level); assert.equals(1, ups)
        xp, level, ups = curve.gain(0, 15, 1.5)
        assert.equals(23, xp); assert.equals(1, level); assert.equals(0, ups)
        xp, level = curve.gain(999990, 500, 1.0)
        assert.equals(999999, xp); assert.equals(99, level)
        xp, level, ups = curve.gain(999999, 500, 1.0)
        assert.equals(999999, xp); assert.equals(99, level); assert.equals(0, ups)
    end)
end)
