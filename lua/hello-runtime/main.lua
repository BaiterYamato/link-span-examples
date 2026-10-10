-- hello-runtime: the minimal mod from the v0.1.0-alpha.1 release.
-- Uses only the common API: logging, feature detection, events and hotkeys.
-- The same file runs on both hosts and on the mock runtime.
local ship = require("ship")

ship.events.on("game.ready", function()
    ship.log.info("hello-runtime loaded on " .. ship.game.id())

    -- Feature detection: the hotkey is registered only when the host supports input.
    if ship.capabilities.has("core.input") then
        ship.hotkeys.register("hello", { default = "H", label = "Hello runtime" }, function()
            ship.log.info("Hello from " .. ship.game.id() .. "!")
        end)
        ship.log.info("hello-runtime ready — press H (rebindable)")
    else
        ship.log.warn("capability core.input unavailable — hello-runtime hotkey disabled")
    end
end)
