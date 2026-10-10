-- hello-runtime tests on the mock runtime, without the game or a ROM.
-- The runner executes the entrypoint and emits game.ready before this file.
local ship = require("ship")

describe("hello-runtime", function()
    it("logs the load on game.ready", function()
        local found = false
        for _, entry in ipairs(mock.log.entries()) do
            if string.find(entry.message, "hello-runtime loaded", 1, true) ~= nil then
                found = true
                break
            end
        end
        assert.is_true(found, "expected the mod load log")
    end)

    it("detects the mock core capabilities", function()
        assert.is_true(ship.capabilities.has("core.events"))
        assert.is_true(ship.capabilities.has("core.input"))
    end)

    it("answers the H hotkey with a log", function()
        mock.reset()
        assert.equals(1, mock.input.press("H"), "one hotkey should fire")
        local found = false
        for _, entry in ipairs(mock.log.entries()) do
            if string.find(entry.message, "Hello from", 1, true) ~= nil then
                found = true
                break
            end
        end
        assert.is_true(found, "expected the H hotkey log")
    end)
end)
