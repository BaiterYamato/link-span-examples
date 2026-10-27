local ship = require("ship")

-- O mock do core não instala bindings OoT; só essas chamadas são simuladas.
local original_oot = ship.oot
local original_player = ship.player
local original_has = ship.capabilities.has
ship.oot = ship.oot or { player = {} }
ship.player = ship.player or {}
ship.capabilities.has = function(name)
    return name == "oot.player.mask" or name == "player.speed" or original_has(name)
end
mock.events.emit("game.ready")

local function with_functions(target, replacements, check)
    local saved = {}
    for name, fn in pairs(replacements) do
        saved[name] = target[name]
        target[name] = fn
    end
    local ok, err = pcall(check)
    for name in pairs(replacements) do target[name] = saved[name] end
    if not ok then error(err) end
end

describe("Bunny Hood com primitivas genéricas", function()
    it("equipa e remove a máscara restaurando a velocidade normal", function()
        local masks, speeds = {}, {}
        with_functions(ship.oot.player, {
            set_mask = function(name) masks[#masks + 1] = name; return true end,
        }, function()
            with_functions(ship.player, {
                set_speed_multiplier = function(speed) speeds[#speeds + 1] = speed; return true end,
            }, function()
                assert.equals(1, mock.input.press("U"))
                assert.equals(1, mock.input.press("U"))
            end)
        end)
        assert.equals("bunny_hood", masks[1])
        assert.equals("none", masks[2])
        assert.equals(1.5, speeds[1])
        assert.equals(1.0, speeds[2])
    end)

    it("não altera a velocidade quando a máscara é recusada", function()
        local speeds = 0
        with_functions(ship.oot.player, { set_mask = function() return false end }, function()
            with_functions(ship.player, {
                set_speed_multiplier = function() speeds = speeds + 1; return true end,
            }, function() mock.input.press("U") end)
        end)
        assert.equals(0, speeds)
    end)

    for _, capability in ipairs({ "oot.player.mask", "player.speed" }) do
        it("não registra tecla sem " .. capability, function()
            local registrations = 0
            local has = ship.capabilities.has
            with_functions(ship.capabilities, {
                has = function(name) return name ~= capability and has(name) end,
            }, function()
                with_functions(ship.hotkeys, {
                    register = function() registrations = registrations + 1 end,
                }, function() mock.events.emit("game.ready") end)
            end)
            assert.equals(0, registrations)
        end)
    end
end)

ship.oot = original_oot
ship.player = original_player
ship.capabilities.has = original_has
