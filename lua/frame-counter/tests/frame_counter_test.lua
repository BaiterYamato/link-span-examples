-- Testes do frame-counter rodando no mock runtime (sem jogo/ROM).
-- O runner emite game.ready antes de executar este arquivo.
local ship = require("ship")

describe("counter", function()
    it("logs the start on game.ready", function()
        local encontrado = false
        for _, entry in ipairs(mock.log.entries()) do
            if string.find(entry.message, "counter started", 1, true) ~= nil then
                encontrado = true
                break
            end
        end
        assert.is_true(encontrado, "expected the mod start log")
    end)

    it("detects the mock core capabilities", function()
        assert.is_true(ship.capabilities.has("core.events"))
        assert.is_true(ship.capabilities.has("core.timers"))
        assert.is_true(ship.capabilities.has("core.storage"))
        assert.is_true(ship.capabilities.has("core.input"))
    end)

    it("increments when C is pressed", function()
        mock.reset()
        assert.equals(1, mock.input.press("C"), "one hotkey should fire")
        assert.equals(1, tonumber(ship.storage.get("count")))
        mock.input.press("C")
        assert.equals(2, tonumber(ship.storage.get("count")))
    end)

    it("emits input.hotkey when the key is pressed", function()
        mock.reset()
        local eventos = 0
        ship.events.on("input.hotkey", function(event)
            if event.key == "C" then
                eventos = eventos + 1
            end
        end)
        mock.input.press("C")
        assert.equals(1, eventos)
    end)

    it("counts one tick every 10 frames", function()
        mock.reset()
        mock.timer.advance(10)
        assert.equals(1, tonumber(ship.storage.get("ticks")))
        mock.timer.advance(9)
        assert.equals(1, tonumber(ship.storage.get("ticks")))
        mock.timer.advance(1)
        assert.equals(2, tonumber(ship.storage.get("ticks")))
    end)

    it("storage persists during the session and delete removes the key", function()
        mock.reset()
        ship.storage.set("anotacao", "valor")
        assert.equals("valor", ship.storage.get("anotacao"))
        assert.is_true(ship.storage.delete("anotacao"))
        assert.is_nil(ship.storage.get("anotacao"))
        assert.equals("padrao", ship.storage.get("anotacao", "padrao"))
    end)
end)
