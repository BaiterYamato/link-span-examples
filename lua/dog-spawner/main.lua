local ship = require("ship")

-- Resolve a função de spawn do jogo ativo, se o host oferecer a capability.
-- OOT expõe ship.oot.spawn_dog (En_Dog, Hyrule Market); MM expõe
-- ship.mm.spawn_dog (En_Dg, Clock Town). Se nenhuma das duas existir, o mod
-- registra a hotkey mas avisa que não há suporte neste host.
local function resolveSpawnDog()
    if ship.capabilities.has("oot.spawn_dog") and ship.oot and ship.oot.spawn_dog then
        return ship.oot.spawn_dog
    end
    if ship.capabilities.has("mm.spawn_dog") and ship.mm and ship.mm.spawn_dog then
        return ship.mm.spawn_dog
    end
    return nil
end

ship.events.on("game.ready", function()
    if ship.hotkeys == nil then
        ship.log.warn("host does not support ship.hotkeys — dog spawner disabled")
        return
    end

    local spawnDog = resolveSpawnDog()
    if spawnDog == nil then
        ship.log.warn("no spawn_dog capability available on this host")
        return
    end

    ship.hotkeys.register("spawn_dog", { default = "F", label = "Spawn Dog" }, function()
        if spawnDog() then
            ship.log.info("Woof! dog spawned")
        else
            ship.log.warn("could not spawn here — try in game, in a scene with a dog")
        end
    end)
    ship.log.info("Dog Spawner ready — press F (rebindable)")
end)
