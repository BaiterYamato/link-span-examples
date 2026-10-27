local ship = require("ship")

-- Restaura as funções mesmo quando uma asserção falha.
local function with_functions(target, replacements, check)
    local saved = {}
    for name, fn in pairs(replacements) do
        saved[name] = target[name]
        target[name] = fn
    end
    local ok, err = pcall(check)
    for name in pairs(replacements) do
        target[name] = saved[name]
    end
    if not ok then error(err) end
end

local function last_warning()
    local result = ""
    for _, entry in ipairs(mock.log.entries()) do
        result = entry.message
    end
    return result
end

describe("Dog Spawner", function()
    if ship.game.id() == "oot" then
        it("substitui o cão próprio com handles reais e limite de um ator", function()
            local spawned = {}
            local spawn = ship.actor.spawn
            with_functions(ship.actor, {
                spawn = function(name, options)
                    assert.equals("oot.en_dog", name)
                    assert.equals(0, options.position.x)
                    assert.equals(0, options.position.y)
                    assert.equals(0, options.position.z)
                    local actor, err = spawn(name, options)
                    assert.is_nil(err)
                    assert.equals("actor", actor.kind)
                    spawned[#spawned + 1] = actor
                    return actor, err
                end,
            }, function()
                assert.equals(1, mock.input.press("F"))
                assert.equals(1, mock.input.press("F"))
                assert.equals(2, #spawned)
                assert.is_false(ship.actor.exists(spawned[1]))
                assert.is_true(ship.actor.exists(spawned[2]))
            end)
        end)

        it("descarta handle expirado sem tentar destruí-lo novamente", function()
            local current
            local spawn = ship.actor.spawn
            with_functions(ship.actor, {
                spawn = function(name, options)
                    local actor, err = spawn(name, options)
                    current = actor
                    return actor, err
                end,
            }, function()
                mock.input.press("F")
                assert.is_true(ship.actor.destroy(current))
                local destroyed = 0
                with_functions(ship.actor, {
                    destroy = function() destroyed = destroyed + 1; return true end,
                }, function()
                    mock.input.press("F")
                end)
                assert.equals(0, destroyed)
                assert.is_true(ship.actor.exists(current))
            end)
        end)

        for _, operation in ipairs({ "exists", "destroy", "spawn" }) do
            it("relata falha de " .. operation .. " sem criar um segundo cão", function()
                local spawns = 0
                local replacements = {
                    exists = function() return true end,
                    destroy = function() return true end,
                    spawn = function() spawns = spawns + 1; return {} end,
                }
                replacements[operation] = function()
                    return nil, { code = "host_failure", message = "falha simulada" }
                end
                mock.log.clear()
                with_functions(ship.actor, replacements, function()
                    mock.input.press("F")
                end)
                assert.equals(0, spawns)
                assert.is_true(last_warning():find("host_failure", 1, true) ~= nil)
            end)
        end
    else
        it("usa somente o helper MM e conserva o retorno booleano", function()
            local calls = 0
            -- O mock do core não instala bindings específicos dos hosts.
            local original_mm = ship.mm
            ship.mm = ship.mm or {}
            with_functions(ship.actor, {
                spawn = function() error("MM não deve usar spawn genérico") end,
                exists = function() error("MM não deve consultar handle OoT") end,
                destroy = function() error("MM não deve destruir handle OoT") end,
            }, function()
                with_functions(ship.mm, {
                    spawn_dog = function() calls = calls + 1; return calls == 1 end,
                }, function()
                    local has = ship.capabilities.has
                    with_functions(ship.capabilities, {
                        has = function(name) return name == "mm.spawn_dog" or has(name) end,
                    }, function() mock.events.emit("game.ready") end)
                    assert.equals(1, mock.input.press("F"))
                    mock.log.clear()
                    assert.equals(1, mock.input.press("F"))
                    assert.equals(2, calls)
                    assert.is_true(last_warning():find("Não foi possível", 1, true) ~= nil)
                end)
            end)
            ship.mm = original_mm
        end)
    end

    it("não registra tecla sem a capability do jogo ativo", function()
        local registrations = 0
        local has = ship.capabilities.has
        local missing = ship.game.id() == "oot" and "actor.spawn" or "mm.spawn_dog"
        with_functions(ship.capabilities, {
            has = function(name) return name ~= missing and has(name) end,
        }, function()
            with_functions(ship.hotkeys, {
                register = function() registrations = registrations + 1 end,
            }, function()
                mock.events.emit("game.ready")
            end)
        end)
        assert.equals(0, registrations)
    end)
end)
