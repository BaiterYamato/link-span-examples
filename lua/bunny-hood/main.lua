local ship = require("ship")

-- Composto de duas primitivas genéricas — a mesma dupla serve
-- para qualquer máscara com qualquer efeito, sem tocar em C++:
--
--   ship.oot.player.set_mask(nome)          -- keaton, skull, spooky,
--                                              bunny_hood, goron, zora,
--                                              gerudo, truth, none
--   ship.player.set_speed_multiplier(fator) -- 0.1 a 5.0; 1.0 restaura
--
-- A máscara aumenta a velocidade; não acrescenta o pulo extra do MM.

local MASK = "bunny_hood"
local SPEED = 1.5

local wearing = false

local function set(enabled)
    if ship.capabilities.has("oot.player.mask") and ship.capabilities.has("player.speed") then
        if not ship.oot.player.set_mask(enabled and MASK or "none") then
            return false
        end
        return ship.player.set_speed_multiplier(enabled and SPEED or 1.0)
    end
    return false
end

ship.events.on("game.ready", function()
    local ok = ship.capabilities.has("oot.player.mask") and ship.capabilities.has("player.speed")
    if not ok then
        ship.log.warn("host lacks the mask/speed capabilities")
        return
    end

    ship.hotkeys.register("bunny_hood", { default = "U", label = "Bunny Hood (MM speed)" }, function()
        if not set(not wearing) then
            ship.log.warn("could not toggle the Bunny Hood — you need to be in game")
            return
        end
        wearing = not wearing
        ship.log.info(wearing and "Bunny Hood on — run!" or "Bunny Hood put away")
    end)

    ship.log.info("Bunny Hood ready — press U to put it on/take it off")
end)
