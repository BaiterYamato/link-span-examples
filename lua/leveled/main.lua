local ship = require("ship")
local curve = require("leveled.curve")
local stats = require("leveled.stats")
local species = require("leveled.enemies")
local options = require("leveled.options").load()
local hud = require("leveled.hud")
local menu = require("leveled.menu")

local PLAYER, ENEMY, BOSS = 2, 5, 9
local TOKEN = 0x71
-- slot: arquivo com save.loaded já entregue. O save.loaded chega no primeiro frame com o Link; antes dele, o
-- save.saving da carga (check tracker do SoH) e o game.frame ainda veriam o estado do arquivo anterior.
local state = { xp = 0, level = 1, slot = nil }
local enemies, defeated = {}, {}
local restore_next_frame
local frames = 0
hud.configure(options, state)

local function key(handle)
    if not handle or handle.slot == nil or handle.generation == nil or handle.scene_generation == nil then
        return nil
    end
    return handle.slot .. ":" .. handle.generation .. ":" .. handle.scene_generation
end

local function player(field)
    if ship.capabilities.has("player.fields") then
        return ship.player.get(field)
    end
end

-- O ship.player.get devolve float; o save guarda inteiro.
local function integer(value)
    return type(value) == "number" and math.tointeger(value) or nil
end

local function debug_log(message)
    if ship.capabilities.has("core.storage") and ship.storage.get("debug_log", false) == true then
        ship.log.info(message)
    end
end

local function resolve(actor)
    local scene = -1
    if ship.capabilities.has("oot.env") then
        scene = ship.oot.env.get("scene_id") or -1
    end
    return species.resolve({
        actor_id = actor.actor_id, category = actor.category, params = actor.params or 0,
        scene = scene, age = player("age") or 0, gs_tokens = player("gs_tokens") or 0,
        player_level = state.level, level_scale = options.level_scale,
        ganon_max_level = options.ganon_max_level,
    })
end

local function info_for(actor)
    local actor_key = key(actor.handle)
    return (actor_key and enemies[actor_key]) or resolve(actor)
end

local function apply_hearts()
    if state.slot == nil or not ship.save.available() or restore_next_frame then
        return 0
    end
    local current = integer(player("health_capacity"))
    if current == nil then
        return 0
    end
    state.base_capacity = state.base_capacity or current
    if state.written_capacity and current ~= state.written_capacity then
        state.base_capacity = state.base_capacity + current - state.written_capacity
    end
    ship.save.set("base_capacity", state.base_capacity)
    local target = state.base_capacity
    if options.bonus_hearts then
        target = math.min(target + stats.bonus_hearts(state.level) * 16, 320)
    end
    if ship.player.set("health_capacity", target) then
        state.written_capacity = target
        return math.max(0, target - current)
    end
    return 0
end

ship.events.on("save.loaded", function(payload)
    -- Nenhum handle, bônus escrito ou aviso pertence ao próximo arquivo.
    enemies, defeated = {}, {}
    restore_next_frame = nil
    state.xp, state.base_capacity, state.written_capacity = 0, nil, nil
    state.slot = payload and payload.slot or -1
    frames = 0
    hud.reset()
    if ship.save.available() then
        state.xp = ship.save.get("xp", 0)
        state.base_capacity = integer(ship.save.get("base_capacity")) or integer(player("health_capacity"))
        ship.save.set("schema", 1)
    end
    state.level = curve.level_for_xp(state.xp)
    apply_hearts()
end)

ship.events.on("save.saving", function(payload)
    if state.slot == nil or (payload and payload.slot ~= state.slot) or not ship.save.available() or
        restore_next_frame then
        return
    end
    apply_hearts()
    local health = player("health")
    if not state.written_capacity or type(health) ~= "number" then
        return
    end
    restore_next_frame = { capacity = state.written_capacity, health = health }
    ship.player.set("health_capacity", state.base_capacity)
    ship.player.set("health", math.min(health, state.base_capacity))
end)

ship.events.on("game.frame", function()
    if not ship.save.available() then
        restore_next_frame = nil
        state.slot = nil
        hud.reset()
        return
    end
    if state.slot == nil then
        return
    end
    if restore_next_frame then
        local saved = restore_next_frame
        restore_next_frame = nil
        -- Restaura antes da comparação, para não contar a retirada do bônus como coração novo.
        ship.player.set("health_capacity", saved.capacity)
        apply_hearts()
        ship.player.set("health", math.min(saved.health, state.written_capacity))
    end
    frames = frames + 1
    if frames % 30 == 0 then
        apply_hearts()
    end
    hud.update()
end)

ship.events.on("actor.init", function(payload)
    local actor = payload.actor
    if not actor or actor.category ~= ENEMY then
        return
    end
    local actor_key = key(actor.handle)
    if not actor_key then
        return
    end
    local info = resolve(actor)
    enemies[actor_key] = info
    local base = ship.actor.get(actor.handle, "max_health")
    -- O host rejeita atores sem vida inicial com invalid_state.
    if type(base) == "number" and base > 0 then
        local maximum = stats.enemy_max_health(base, info.level, options)
        ship.actor.set_max_health(actor.handle, maximum)
        debug_log(("Leveled init %s level=%d max_health=%d"):format(actor_key, info.level, maximum))
    end
end)

ship.events.on("actor.destroy", function(payload)
    local actor_key = key(payload.handle)
    if actor_key then
        enemies[actor_key], defeated[actor_key] = nil, nil
    end
end)

local function equipment(attribute)
    if options.equipment then
        return ship.attributes.value("baiteryamato.leveled." .. attribute) or 0
    end
    return 0
end

local function rng(n) return math.random() * n end

ship.events.on("hook.oot.actor.damage", function(payload)
    local power
    if payload.attacker_is_player then
        power = stats.player_power(state.level) + equipment("power")
    elseif payload.attacker then
        power = stats.enemy_power(info_for(payload.attacker).level) + 1
    else
        return
    end
    local damage
    if payload.target_category == PLAYER then
        local courage = stats.player_courage(state.level) + equipment("courage")
        damage = stats.modify(true, payload.damage, power, courage, options.attack_scales, rng)
        -- Dano recebido (N/4): um golpe que dói continua doendo, e dano zero (Deku Nut) continua zero.
        if damage > 0 then
            damage = math.max(1, math.floor(damage * options.damage_taken / 4))
        end
        damage = math.min(255, damage)
    elseif payload.target_category == BOSS or not payload.target_extended or not payload.target then
        return
    else
        local courage = stats.enemy_courage(info_for(payload.target).level)
        damage = stats.modify(false, payload.damage * options.multiplier, power, courage,
            options.attack_scales, rng)
    end
    ship.hooks.result(damage)
    debug_log(("Leveled damage target=%s amount=%d power=%g"):format(
        key(payload.target and payload.target.handle) or "player", damage, power))
end)

ship.events.on("hook.oot.actor.damaged", function(payload)
    if options.numbers_enemy and payload.amount > 0 and payload.target then
        hud.push_number(payload.target.handle, payload.amount, "enemy")
    end
end)

ship.events.on("hook.oot.player.health_change", function(payload)
    if options.numbers_player and payload.amount < 0 then
        hud.push_player_number(-payload.amount)
    end
end)

local function gain(amount, rate)
    if state.slot == nil or not ship.save.available() then
        return
    end
    local old_xp, old_level = state.xp, state.level
    local new_xp, new_level, raised = curve.gain(old_xp, amount, rate)
    state.xp, state.level = new_xp, new_level
    ship.save.set("xp", new_xp)
    hud.push_xp(new_xp - old_xp)
    if raised > 0 then
        local hearts = apply_hearts() / 16
        local diff = {
            power = stats.player_power(new_level) - stats.player_power(old_level),
            courage = stats.player_courage(new_level) - stats.player_courage(old_level),
            hearts = math.tointeger(hearts) or hearts,
        }
        if options.level_up_sound and ship.capabilities.has("audio.play") then
            ship.audio.play_fanfare(0x22)
        end
        if options.level_up then
            hud.level_up(new_level, diff)
        end
    end
end

ship.events.on("hook.oot.enemy.defeat", function(payload)
    local actor = payload.actor
    local actor_key = actor and key(actor.handle)
    if not actor_key or defeated[actor_key] or not ship.save.available() then
        return
    end
    defeated[actor_key] = true
    local info = info_for(actor)
    gain(info.exp, options.xp_rate)
    debug_log(("Leveled defeat %s level=%d exp=%d xp=%d"):format(
        actor_key, info.level, info.exp, state.xp))
end)

ship.events.on("hook.oot.item.receive", function(payload)
    if payload.item_id == TOKEN then
        gain(stats.token_xp(player("gs_tokens") or 0, options.token_rate), options.xp_rate)
    end
end)

if ship.capabilities.has("hud.draw") then
    ship.events.on("hook.oot.hud.draw", function()
        if ship.save.available() then
            hud.draw()
        end
    end)
end
if ship.capabilities.has("core.menu") then
    menu.register(options)
end
