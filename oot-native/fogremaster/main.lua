local ship = require("ship")

local controls = {
    { id = "enabled", type = "checkbox", label = "Névoa interativa", default = true },
    { id = "volume", type = "checkbox", label = "Renderização volumétrica", default = true,
        tooltip = "Desmarque para comparar com as camadas translúcidas." },
    -- 0.28.0: chave nova, para que o "compute" salvo da 0.27 não prenda ninguém no campo antigo.
    { id = "gpu_field", type = "checkbox", label = "Campo antigo na GPU (comparação)", default = false,
        tooltip = "Desmarcado, o mod simula a névoa como camada rasa: o golpe abre a névoa, ela volta escorrendo e " ..
            "os vórtices saem da própria simulação. Marque para comparar com o campo antigo do compute." },
    { id = "particles", type = "checkbox", label = "Partículas na GPU", default = true,
        tooltip = "As partículas seguem a velocidade do fluido no caminho GPU do DirectX 11." },
    { id = "density", type = "slider_int", label = "Densidade da névoa", default = 100, min = 25, max = 200,
        step = 5, format = "%d%%" },
    { id = "attack", type = "slider_int", label = "Força dos ataques", default = 100, min = 25, max = 200,
        step = 5, format = "%d%%", tooltip = "Ajusta quanto os golpes e habilidades deslocam a névoa." },
    { id = "push_duration", type = "slider_int", label = "Duração do empurrão (ms)", default = 450, min = 150,
        max = 1000, step = 50, tooltip = "Valores altos deixam a saída da névoa mais gradual." },
    { id = "quality", type = "choice", label = "Qualidade do volume", default = -1,
        tooltip = "O ajuste de resolução e reprojeção usa o caminho GPU do DirectX 11.",
        choices = {
            { value = -1, label = "Padrão do host" },
            { value = 0, label = "Resolução cheia" },
            { value = 1, label = "Meia resolução" },
            { value = 2, label = "Meia resolução com reprojeção" },
        } },
    { id = "attack_lift", type = "slider_int", label = "Elevação nos ataques", default = 100, min = 0, max = 200,
        step = 5, format = "%d%%", tooltip = "Controla quanto os golpes levantam a névoa volumétrica no DirectX 11." },
    { id = "lighting", type = "checkbox", label = "Iluminação local da névoa", default = true,
        tooltip = "Deixa tochas e outras luzes pontuais colorirem a névoa próxima no DirectX 11." },
    { id = "scene_color", type = "checkbox", label = "Cor da névoa acompanha a cena", default = true,
        tooltip = "Segue a luz ambiente com transição suave. Desmarque para comparar com a cor fixa original." },
    { id = "height", type = "slider_int", label = "Altura da névoa", default = 100, min = 35, max = 250,
        step = 5, format = "%d%%", tooltip = "Ajusta a altura da camada de névoa rente ao chão." },
    { id = "speed", type = "slider_int", label = "Velocidade da névoa", default = 100, min = 0, max = 250,
        step = 5, format = "%d%%", tooltip = "Velocidade do deslocamento ambiente (0% = parada até você interagir)." },
    { id = "dispersion", type = "slider_int", label = "Dispersão da névoa", default = 65, min = 0, max = 100,
        step = 5, format = "%d%%", tooltip = "Espalha a névoa de forma mais uniforme pelo cenário, reduzindo vãos e acúmulos." },
    { id = "calibration_logs", type = "checkbox", label = "Logs de calibração", default = false,
        tooltip = "Registra vento, luz, golpes, pousos e contadores durante medições." },
}

local persisted = ship.capabilities.has("core.storage")
local values = {}

local function storageKey(id)
    return "settings." .. id
end

local function loadValue(control)
    if not persisted then return control.default end
    local value = ship.storage.get(storageKey(control.id), control.default)
    if control.type == "checkbox" then
        if type(value) == "boolean" then return value end
    else
        local integer = math.tointeger(value)
        if control.type == "choice" then
            for _, choice in ipairs(control.choices) do
                if integer == choice.value then return integer end
            end
        elseif integer then
            return math.max(control.min, math.min(control.max, integer))
        end
    end
    return control.default
end

for _, control in ipairs(controls) do
    values[control.id] = loadValue(control)
end

local function report(value, err)
    if value then
        ship.log.info("FogreMaster: " .. value)
    else
        local detail = type(err) == "table" and (err.message or err.code) or err
        ship.log.warn("FogreMaster: " .. tostring(detail))
    end
end

local function apply()
    local arguments = {}
    for _, control in ipairs(controls) do
        if control.id ~= "calibration_logs" then
            local value = values[control.id]
            if type(value) == "boolean" then value = value and 1 or 0 end
            arguments[#arguments + 1] = value
        end
    end
    local result, err = ship.native.call("configure", table.concat(arguments, " "))
    report(result, err)
    return result ~= nil
end

local function change(id, value)
    local previous = values[id]
    values[id] = value
    if id ~= "calibration_logs" and not apply() then values[id] = previous end
    if persisted then ship.storage.set(storageKey(id), values[id]) end
end

local function toggle(id, hotkey, label)
    ship.hotkeys.register(hotkey, { label = label }, function()
        change(id, not values[id])
    end)
end

toggle("enabled", "fogremaster_toggle", "FogreMaster: ligar/desligar")
toggle("volume", "fogremaster_renderer", "FogreMaster: alternar volume e camadas")
toggle("gpu_field", "fogremaster_compute", "FogreMaster: alternar camada rasa e campo antigo na GPU")
toggle("particles", "fogremaster_particles", "FogreMaster: alternar partículas")

ship.hotkeys.register("fogremaster_stats", { label = "FogreMaster: registrar diagnóstico" }, function()
    report(ship.native.call("stats", ""))
end)

if persisted and ship.capabilities.has("core.menu") then
    local widgets = {}
    for _, control in ipairs(controls) do
        local widget = {}
        for key, value in pairs(control) do widget[key] = value end
        local id = control.id
        widget.storage_key = storageKey(id)
        widget.on_change = function(value) change(id, value) end
        widgets[#widgets + 1] = widget
    end
    widgets[#widgets + 1] = {
        id = "reset", type = "action", label = "Restaurar padrões",
        on_activate = function()
            local previous = values
            values = {}
            for _, control in ipairs(controls) do values[control.id] = control.default end
            if not apply() then
                values = previous
                return
            end
            for _, control in ipairs(controls) do ship.storage.delete(storageKey(control.id)) end
        end,
    }
    ship.menu.register({ id = "fogremaster", title = "FogreMaster", sidebar = "FogreMaster", widgets = widgets })
end

ship.events.on("game.ready", function()
    apply()
    local seen = {}
    local windBase = nil
    local trace, traceBudget = "0", 8
    local lands, landBudget = "0", 8
    local lastLight, lightBudget = nil, 16
    ship.timer.every(20, function()
        if not values.calibration_logs then return end
        local value = ship.native.call("stats", "")
        if not value then return end
        -- Vento da cena (R054): registra quando a base muda (troca de cena ou override), não a cada rajada.
        local base = value:match("windbase=(%S+)")
        local changed = base ~= windBase
        windBase = base
        -- Luz da cena: registra as primeiras trocas da sessão, para calibrar a exposição da névoa por cena.
        local light = value:match("env=(%S+)")
        if light and light ~= lastLight and lightBudget > 0 then
            lastLight = light
            lightBudget = lightBudget - 1
            changed = true
        end
        -- Golpe medido (giro e vão, 5 s depois): os primeiros de cada sessão vão para o log.
        local measured = value:match("trace=(%d+)")
        if measured and measured ~= trace then
            trace = measured
            if traceBudget > 0 then
                traceBudget = traceBudget - 1
                changed = true
            end
        end
        -- Pouso (0.31.0): os primeiros de cada sessão vão para o log, com a batida mais forte até ali.
        local landed = value:match("lands=(%d+)")
        if landed and landed ~= lands then
            lands = landed
            if landBudget > 0 then
                landBudget = landBudget - 1
                changed = true
            end
        end
        for key, count in value:gmatch("(%a+)=(%d+)") do
            if key ~= "active" and count ~= "0" and not seen[key] then
                seen[key] = true
                changed = true
            end
        end
        if changed then ship.log.info("FogreMaster: " .. value) end
    end)
end)
