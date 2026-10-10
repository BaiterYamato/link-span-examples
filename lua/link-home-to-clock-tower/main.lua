local ship = require("ship")

ship.events.on("game.ready", function()
    if not ship.capabilities.has("world.travel") then
        ship.log.warn("world.travel unavailable; cross-world teleport disabled")
        return
    end
    if ship.game.id() ~= "oot" then
        return
    end

    ship.hotkeys.register(
        "link_home_to_clock_tower",
        { default = "Y", label = "Go to the Clock Tower entrance" },
        function()
            ship.world.travel("mm", "mm.clock_tower.entrance")
        end
    )
    ship.log.info("Teleport ready: press Y in Link's house")
end)
