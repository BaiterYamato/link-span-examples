local ship = require("ship")

-- Plan-sdk §26: o host expõe o puppet lógico "oot.link_child_puppet", que
-- desenha o esqueleto do Young Link. Um .otr substituto de
-- objects/object_link_child (como o Kafei replacer) troca a aparência do
-- puppet automaticamente, sem código nativo específico.

local puppet = nil

local function spawn_puppet()
    if puppet and ship.actor.exists(puppet) then
        ship.actor.destroy(puppet)
        puppet = nil
        ship.log.info("puppet removed")
        return
    end

    -- Posição (0,0,0) = na frente do player, virado para ele.
    local actor, err = ship.actor.spawn("oot.link_child_puppet", {
        position = { x = 0, y = 0, z = 0 },
        rotation = { x = 0, y = 0, z = 0 },
    })
    if not actor then
        ship.log.warn("puppet failed [" .. err.code .. "]: " .. err.message)
        return
    end
    puppet = actor
    ship.log.info("puppet spawned — with the Kafei replacer in the mods folder, say hi to Kafei")
end

ship.events.on("game.ready", function()
    ship.log.info("Kafei Puppet ready — press H to spawn/remove the puppet")
end)

ship.hotkeys.register("kafei_puppet", {
    default = "H",
    label = "Kafei Puppet",
}, spawn_puppet)
