local ship = require("ship")

local frames = 0
local last_status = nil

ship.hotkeys.register("fairy_light_color_toggle", {
    default = "J",
    label = "Luz da fada com a cor dela: ligar/desligar",
}, function()
    local result, failure = ship.native.call("toggle", "toggle")
    ship.log.info("fairy-light: " .. (result or ("falhou: " .. tostring(failure))))
end)

ship.events.on("game.frame", function()
    frames = frames + 1
    if frames % 300 ~= 0 then
        return
    end
    local status, failure = ship.native.call("status", "")
    local current = status or ("falhou: " .. tostring(failure))
    if current ~= last_status then
        ship.log.info("fairy-light: " .. current)
        last_status = current
    end
end)
