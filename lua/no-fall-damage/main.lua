local ship = require("ship")

-- VB_RECIEVE_FALL_DAMAGE é um hook "should": o engine aplica dano de queda
-- quando o resultado é true (padrão). Devolvendo false via ship.hooks.result,
-- o dano nunca acontece — nenhuma função nativa foi escrita para isto.

local enabled = true

ship.events.on("game.ready", function()
    if ship.game.id() ~= "oot" then
        return
    end
    if not ship.capabilities.has("hooks.bridge") then
        ship.log.warn("host lacks hooks.bridge — update Link-Span")
        return
    end

    ship.hotkeys.register("no_fall_damage", { default = "N", label = "Toggle fall damage" }, function()
        enabled = not enabled
        ship.log.info(enabled and "Fall damage: normal" or "Fall damage: off")
    end)

    ship.events.on("hook.oot.player.fall_damage", function()
        if not enabled then
            ship.hooks.result(false)
        end
    end)

    ship.log.info("No fall damage ready — N toggles it; press it before falling from a great height")
end)
