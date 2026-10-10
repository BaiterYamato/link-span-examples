local ship = require("ship")

-- Resolve a função de pulo do jogo ativo, se o host oferecer a capability.
-- OOT expõe ship.oot.player.jump; MM expõe ship.mm.player.jump.
local function resolveJump()
    if ship.capabilities.has("oot.player.jump") and ship.oot and ship.oot.player then
        return ship.oot.player.jump
    end
    if ship.capabilities.has("mm.player.jump") and ship.mm and ship.mm.player then
        return ship.mm.player.jump
    end
    return nil
end

ship.events.on("game.ready", function()
    local jump = resolveJump()
    if jump == nil then
        ship.log.warn("no jump capability available — jump mod disabled")
        return
    end

    ship.hotkeys.register("jump", { default = "J", label = "Jump" }, function()
        if not jump() then
            ship.log.debug("jump ignored: the player must be on the ground")
        end
    end)
    ship.log.info("Jump ready — press J or change the shortcut in the Link-Span settings")
end)
