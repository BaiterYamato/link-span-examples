local ship = require("ship")
local curve = require("leveled.curve")
local options = require("leveled.options")

local function loaded(slot, xp)
    mock.reset()
    options.load()
    mock.save.load(slot)
    ship.save.set("xp", xp or 0)
    mock.events.emit("save.loaded", { slot = slot })
end

local function keese(category)
    local actor = { actor_id = 0x13, category = category or 5, params = 0 }
    actor.handle = mock.actors.observe({
        actor_id = actor.actor_id, category = actor.category, params = actor.params, health = 4,
    })
    mock.events.emit("actor.init", { actor = actor })
    return actor
end

local function defeat(actor)
    mock.events.emit("hook.oot.enemy.defeat", {
        actor = actor, actor_id = actor.actor_id, category = actor.category,
        pos_x = 0, pos_y = 0, pos_z = 0,
    })
end

local function no_errors()
    for _, entry in ipairs(mock.log.entries()) do
        assert.is_true(string.lower(entry.level) ~= "error", entry.message)
    end
end

-- Substituições locais permitem testar campos e desenho sem adaptadores nativos.
local function with_services(services, body)
    local old = {}
    for name, value in pairs(services) do
        old[name] = ship[name]
        ship[name] = value
    end
    local ok, err = pcall(body)
    for name in pairs(services) do
        ship[name] = old[name]
    end
    if not ok then error(err) end
end

describe("fluxo leveled", function()
    it("estende a vida e concede XP uma vez por handle", function()
        loaded(0)
        local actor = keese()
        assert.is_true(mock.actors.get(actor.handle).extended_max > 4)
        defeat(actor)
        assert.equals(5, mock.save.dump().xp)
        defeat(actor)
        assert.equals(5, mock.save.dump().xp)
        assert.equals(1, mock.save.dump().schema)
        no_errors()
    end)

    it("sobe de nível e usa a fanfarra inteira 0x22 quando disponível", function()
        loaded(1, 29)
        defeat(keese())
        assert.equals(34, mock.save.dump().xp)
        local calls = mock.audio.calls()
        if ship.capabilities.has("audio.play") then
            assert.equals(1, #calls)
            assert.equals("fanfare", calls[1].kind)
            assert.equals(0x22, calls[1].id)
        else
            assert.equals(0, #calls)
        end
        no_errors()
    end)

    it("ignora abates sem save e preserva o arquivo anterior", function()
        loaded(2)
        defeat(keese())
        mock.save.unload()
        defeat(keese())
        mock.timer.advance(1)
        no_errors()
        mock.save.load(2)
        assert.equals(5, mock.save.dump().xp)
    end)

    it("isola os arquivos e limpa o cache ao carregar", function()
        loaded(3)
        local actor = keese()
        defeat(actor)
        mock.save.load(4)
        ship.save.set("xp", 0)
        mock.events.emit("save.loaded", { slot = 4 })
        defeat(actor)
        assert.equals(5, mock.save.dump().xp)
        mock.save.load(3)
        assert.equals(5, mock.save.dump().xp)
        no_errors()
    end)

    it("limpa o handle destruído e não estende chefes", function()
        loaded(5)
        local actor = keese()
        defeat(actor)
        mock.events.emit("actor.destroy", { handle = actor.handle })
        mock.events.emit("actor.init", { actor = actor })
        defeat(actor)
        assert.equals(10, mock.save.dump().xp)
        local boss = keese(9)
        assert.equals(0, mock.actors.get(boss.handle).extended_max)
        no_errors()
    end)

    it("aplica taxas e ignora outros itens", function()
        loaded(6)
        options.set("xp_rate", 2)
        options.set("token_rate", 2)
        defeat(keese())
        assert.equals(10, mock.save.dump().xp)
        mock.events.emit("hook.oot.item.receive", { item_id = 1, get_item_id = 1 })
        assert.equals(10, mock.save.dump().xp)
        mock.events.emit("hook.oot.item.receive", { item_id = 0x71, get_item_id = 1 })
        assert.equals(30, mock.save.dump().xp)
        no_errors()
    end)

    it("salva corações vanilla e restaura vida e bônus no frame seguinte", function()
        local fields = { health_capacity = 48, health = 60 }
        local has = ship.capabilities.has
        with_services({
            capabilities = { has = function(cap) return cap == "player.fields" or has(cap) end },
            player = {
                get = function(field) return fields[field] end,
                set = function(field, value) fields[field] = value; return true end,
            },
        }, function()
            loaded(7, curve.xp_at_level(7))
            assert.equals(64, fields.health_capacity)
            assert.equals(48, mock.save.dump().base_capacity)
            fields.health_capacity = 80 -- coração obtido no jogo
            mock.timer.advance(30)
            assert.equals(64, mock.save.dump().base_capacity)
            assert.equals(80, fields.health_capacity)
            fields.health = 72
            mock.events.emit("save.saving", { slot = 7 })
            assert.equals(64, fields.health_capacity)
            assert.equals(64, fields.health)
            mock.events.emit("save.saving", { slot = 7 })
            mock.timer.advance(1)
            assert.equals(80, fields.health_capacity)
            assert.equals(72, fields.health)
            assert.equals(64, mock.save.dump().base_capacity)
            options.set("bonus_hearts", false)
            mock.timer.advance(30)
            assert.equals(64, fields.health_capacity)
            no_errors()
        end)
    end)

    it("transforma dano só nos alvos cobertos e respeita dano zero no inimigo", function()
        loaded(8)
        local actor = keese()
        local result
        with_services({ hooks = { result = function(value) result = value end } }, function()
            local payload = {
                target_id = 0x13, target_category = 5, attacker_id = 0, attacker_category = 2,
                attacker_is_player = true, dmg_flags = 0, damage_effect = 0,
                base_damage = 1, damage = 1, target = actor,
                target_health = 100, target_max_health = 100, target_extended = true,
            }
            mock.events.emit("hook.oot.actor.damage", payload)
            assert.is_true(type(result) == "number" and result > 0)
            payload.damage, result = 0, nil
            mock.events.emit("hook.oot.actor.damage", payload)
            assert.equals(0, result)
            payload.target_category, result = 9, nil
            mock.events.emit("hook.oot.actor.damage", payload)
            assert.is_nil(result)
            payload.target_category, payload.target_extended = 5, false
            mock.events.emit("hook.oot.actor.damage", payload)
            assert.is_nil(result)
            payload.target_category, payload.damage = 2, 1000
            payload.attacker_is_player, payload.attacker = false, actor
            mock.events.emit("hook.oot.actor.damage", payload)
            assert.equals(255, result)
            -- Dano zero contra o Link (Deku Nut) continua zero depois do fator N/4.
            payload.damage, result = 0, nil
            mock.events.emit("hook.oot.actor.damage", payload)
            assert.equals(0, result)
            payload.attacker, result = nil, nil
            mock.events.emit("hook.oot.actor.damage", payload)
            assert.is_nil(result)
            no_errors()
        end)
    end)

    it("aceita modificadores fracionários sem falha no log de dano", function()
        loaded(9)
        local actor = keese()
        local result
        with_services({
            attributes = { value = function() return 0.5 end },
            hooks = { result = function(value) result = value end },
        }, function()
            mock.events.emit("hook.oot.actor.damage", {
                target_id = 0x13, target_category = 5, attacker_id = 0, attacker_category = 2,
                attacker_is_player = true, dmg_flags = 0, damage_effect = 0,
                base_damage = 1, damage = 1, target = actor,
                target_health = 100, target_max_health = 100, target_extended = true,
            })
            assert.is_true(type(result) == "number" and result > 0)
            no_errors()
        end)
    end)
end)

describe("opções e menu", function()
    it("corta faixas, rejeita tipos e usa padrões sem storage", function()
        mock.reset()
        ship.storage.set("xp_rate", 20)
        ship.storage.set("hp_percent", -1)
        ship.storage.set("multiplier", 50)
        ship.storage.set("damage_taken", 0)
        ship.storage.set("equipment", "false")
        options.load()
        assert.equals(10, options.xp_rate)
        assert.equals(0, options.hp_percent)
        assert.equals(32, options.multiplier)
        assert.equals(1, options.damage_taken)
        assert.is_true(options.equipment)
        local has = ship.capabilities.has
        with_services({ capabilities = { has = function(cap) return cap ~= "core.storage" and has(cap) end } },
            function()
                options.load()
                assert.equals(1, options.xp_rate)
                assert.equals(9, options.multiplier)
                assert.is_false(options.ganon_max_level)
            end)
        mock.reset()
        options.load()
    end)

    it("publica três grupos e atualiza cada chave pelo próprio callback", function()
        local page
        with_services({ menu = { register = function(value) page = value; return true end } }, function()
            require("leveled.menu").register(options)
        end)
        local groups, values = 0, 0
        for _, widget in ipairs(page.widgets) do
            if widget.type == "separator" then
                groups = groups + 1
            else
                values = values + 1
                local def = options.definitions[widget.storage_key]
                local value = def.max
                if type(def.default) == "boolean" then value = not def.default end
                widget.on_change(value)
                assert.equals(value, options[widget.storage_key])
                assert.equals(value, ship.storage.get(widget.storage_key))
            end
        end
        assert.equals(3, groups)
        assert.equals(16, values)
        mock.reset()
        options.load()
    end)
end)

describe("HUD leveled", function()
    it("limita números, mantém a última posição, desvanece e expira por frame", function()
        local hud = require("leveled.hud")
        local draws, rects, visible = {}, {}, true
        hud.configure(options, { xp = curve.xp_at_level(99), level = 99 })
        hud.reset()
        with_services({
            actor = { get = function(_, field)
                if visible then return field == "screen_x" and 100.75 or 80.75 end
            end },
            hud = {
                draw_text = function(...) draws[#draws + 1] = { ... } end,
                draw_rect = function(...) rects[#rects + 1] = { ... } end,
            },
        }, function()
            for i = 1, 25 do hud.push_number({}, i, "enemy") end
            visible = false
            for _ = 1, 31 do hud.update() end
            hud.draw()
            assert.equals(25, #draws) -- nível + os 24 mais recentes
            assert.equals("2", draws[2][1])
            assert.equals(100, draws[2][2])
            assert.equals(53, draws[2][3])
            assert.equals(238, draws[2][7])
            assert.equals(64, rects[2][3]) -- barra cheia no nível 99
            for _ = 32, 45 do hud.update() end
            draws = {}
            hud.draw()
            assert.equals(1, #draws)
            options.xp_bar = false
            hud.level_up(2, { power = 0, courage = 1, hearts = 0 })
            draws = {}
            hud.draw()
            assert.equals("LEVEL UP!  Lv 2", draws[1][1])
            assert.equals("Courage +1", draws[2][1])
            for _ = 1, 90 do hud.update() end
            draws = {}
            hud.draw()
            assert.equals(0, #draws)
        end)
        options.load()
        hud.reset()
    end)
end)
