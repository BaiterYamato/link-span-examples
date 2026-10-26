local ship = require("ship")
local menu = {}

function menu.register(options)
    local widgets = {}
    local function group(id, label)
        widgets[#widgets + 1] = { id = id, type = "separator", label = label }
    end
    local function option(key, label)
        local def = options.definitions[key]
        local widget = {
            id = key, label = label, storage_key = key, default = options[key],
            on_change = function(value) options.set(key, value) end,
        }
        if type(def.default) == "boolean" then
            widget.type = "checkbox"
        else
            widget.type = def.integer and "slider_int" or "slider_number"
            widget.min, widget.max = def.min, def.max
            widget.step = def.integer and 1 or 0.1
            widget.format = def.integer and "%d" or "%.1f"
        end
        widgets[#widgets + 1] = widget
    end

    group("hud", "HUD")
    option("xp_bar", "Nível e barra de XP")
    option("numbers_enemy", "Dano nos inimigos")
    option("numbers_player", "Dano no Link")
    option("numbers_xp", "XP recebido")
    option("level_up", "Aviso de nível")
    option("level_up_sound", "Som ao subir de nível")

    group("player", "Jogador")
    option("bonus_hearts", "Corações por nível")
    option("equipment", "Stats de equipamento")
    option("damage_taken", "Dano recebido (N/4)")
    option("xp_rate", "Taxa de XP")
    option("token_rate", "Taxa de XP de token")

    group("enemies", "Inimigos")
    option("attack_scales", "Nível afeta o ataque")
    option("multiplier", "Multiplicador de ataque e vida")
    option("hp_percent", "Fator de vida")
    option("level_scale", "Escala de nível")
    option("ganon_max_level", "Castelo de Ganon no máximo com 100 tokens")

    return ship.menu.register({
        id = "leveled", title = "Leveled", sidebar = "Leveled", widgets = widgets,
    })
end

return menu
