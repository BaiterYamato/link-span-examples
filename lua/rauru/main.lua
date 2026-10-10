local ship = require("ship")

-- Reverso da estátua da Elegia: o esqueleto e a animação do Rauru (En_Rl,
-- object_rl) são lidos em runtime do oot.o2r da instalação vizinha do OOT,
-- montado pelo host MM sob o namespace "oot/".

local rauru = nil

local function toggle_rauru()
    if rauru and ship.actor.exists(rauru) then
        ship.actor.destroy(rauru)
        rauru = nil
        ship.log.info("Rauru dismissed")
        return
    end

    -- Posição (0,0,0) = na frente do player.
    local actor, err = ship.actor.spawn("mm.oot_rauru", {
        position = { x = 0, y = 0, z = 0 },
        rotation = { x = 0, y = 0, z = 0 },
    })
    if not actor then
        ship.log.warn("Rauru failed [" .. err.code .. "]: " .. err.message)
        return
    end
    rauru = actor
    ship.log.info("Rauru, the Sage of Light, crossed the worlds — OOT inside MM OK")
end

ship.events.on("game.ready", function()
    ship.log.info("Rauru ready — press K to summon/dismiss the Sage of Light")
end)

ship.hotkeys.register("rauru", {
    default = "K",
    label = "Rauru (OOT)",
}, toggle_rauru)
