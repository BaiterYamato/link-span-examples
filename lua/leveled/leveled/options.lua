local ship = require("ship")
local options = {}

-- Uma definição por chave, compartilhada pela carga e pelo menu.
options.definitions = {
    xp_rate = { default = 1.0, min = 0, max = 10 },
    token_rate = { default = 1.0, min = 0, max = 10 },
    hp_percent = { default = 1.0, min = 0, max = 10 },
    level_scale = { default = 1.0, min = 0, max = 10 },
    multiplier = { default = 9, min = 1, max = 32, integer = true },
    damage_taken = { default = 4, min = 1, max = 32, integer = true },
    bonus_hearts = { default = true },
    equipment = { default = true },
    attack_scales = { default = true },
    ganon_max_level = { default = false },
    numbers_enemy = { default = true },
    numbers_player = { default = true },
    numbers_xp = { default = true },
    level_up = { default = true },
    level_up_sound = { default = true },
    xp_bar = { default = true },
}

local function normalize(key, value)
    local def = options.definitions[key]
    if type(value) ~= type(def.default) then
        return def.default
    end
    if def.min then
        if value ~= value or value == math.huge or value == -math.huge then
            return def.default
        end
        value = math.max(def.min, math.min(def.max, value))
        if def.integer then
            value = math.floor(value)
        end
    end
    return value
end

function options.load()
    local persistent = ship.capabilities.has("core.storage")
    for key, def in pairs(options.definitions) do
        local value = def.default
        if persistent then
            value = ship.storage.get(key, value)
        end
        options[key] = normalize(key, value)
    end
    return options
end

function options.set(key, value)
    if not options.definitions[key] then
        return
    end
    value = normalize(key, value)
    if ship.capabilities.has("core.storage") then
        ship.storage.set(key, value)
    end
    options[key] = value
end

return options
