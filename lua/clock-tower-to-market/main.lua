local ship = require("ship")

-- Contraparte de link-home-to-clock-tower: a viagem de volta, de MM para OoT.
-- O host do MM não exige uma cena específica (o de OoT exige a casa do Link),
-- então a volta pode ser pedida de qualquer lugar do jogo.
--
-- Destinos aceitos pelo host OoT:
--   "oot.market"    -> saída sul do Mercado de Hyrule
--   "oot.kakariko"  -> portão da Vila Kakariko

local DESTINATION = "oot.market"

ship.events.on("game.ready", function()
    if ship.game.id() ~= "mm" then
        return
    end
    if not ship.capabilities.has("world.travel") then
        ship.log.warn("world.travel unavailable — start from link-span.exe with both games installed")
        return
    end

    ship.hotkeys.register(
        "clock_tower_to_market",
        { default = "Y", label = "Back to the Market (OoT)" },
        function()
            local ok, err = ship.world.travel("oot", DESTINATION)
            if not ok then
                ship.log.warn("travel failed [" .. err.code .. "]: " .. err.message)
            end
        end
    )
    ship.log.info("Return trip ready: press Y to cross over to Hyrule Market")
end)
