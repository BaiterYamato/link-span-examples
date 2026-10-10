local ship = require("ship")

-- Cross-world de verdade no seu próprio Link: a display list do punho com a
-- Kokiri Sword vem do object_link_child do OoT, lido ao vivo do oot.o2r
-- vizinho. Os nomes de asset dos dois jogos não colidem (gLinkChild* vs
-- gLinkHuman*), então as texturas da espada resolvem sozinhas.

local using_oot = false

ship.events.on("game.ready", function()
    if not ship.capabilities.has("mm.player.sword_skin") then
        ship.log.warn("mm.player.sword_skin unavailable on this host")
        return
    end

    ship.hotkeys.register("oot_kokiri_sword", { default = "T", label = "Kokiri Sword: OoT/MM" }, function()
        local wanted = using_oot and "mm" or "oot"
        if not ship.mm.player.set_sword_skin(wanted) then
            ship.log.warn("could not switch the sword — is the OoT oot.o2r mounted?")
            return
        end
        using_oot = not using_oot
        ship.log.info("Kokiri Sword now uses the " .. (using_oot and "OoT" or "MM") .. " look")
    end)

    ship.log.info("Kokiri Sword ready — press T to switch OoT/MM (draw the Kokiri Sword to see it)")
end)
