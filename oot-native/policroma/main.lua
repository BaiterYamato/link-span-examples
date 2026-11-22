local ship = require("ship")

if not ship.native.has("list") then return end

local storage = ship.capabilities.has("core.storage")
local menu = ship.capabilities.has("core.menu")
local selectionKey = "shaderpack"
-- Altura das nuvens dos packs Iris (cloudHeight), em unidades do OoT; o provider usa o mesmo padrão.
local cloudKey = "iris.cloud_height"
local cloudDefault = 3000
local cloudNeedsReload = false
-- Calibração da névoa (vetor 11 do LinkSpanIris). Padrões: k = 1/32 (o jogo entra na faixa do pack), sem deslocamento
-- de altura e céu aberto; os mesmos do provider. O provider recebe k em blocos por unidade do OoT; o menu mostra o
-- divisor (1/N); 0 = automático (0.7.7): o shader calcula k pela névoa nativa da cena. Valor já gravado no storage
-- vale sobre o padrão.
local fogDivisorKey, fogDivisorDefault = "iris.fog_divisor", 32
local groundKey, groundDefault = "iris.ground_y", 0
-- Luz do céu (eyeBrightness): 0..240 manual; -1 = automático (0.7.9), pela cena (interior 30, céu aberto 240) quando o host
-- anuncia o bit PolicromaFeatureSkyExposure. Padrão 240 (manual) até a prova em jogo.
local skyLightKey, skyLightDefault, skyLightAuto = "iris.sky_light", 240, -1
-- Normais reconstruídas (F0): um passe sintético antes do pack preenche colortex4/6/3 a partir da profundidade, só para
-- os que o pack lê. Muda o grafo, então vale ao (re)carregar o pack. Padrão desligado.
local gbufferKey, gbufferDefault = "iris.gbuffer_f0", false
local pageSize = 20
local active
local activeProfile
local profiles = {}
local diagnostic
local packs = {}
local page = 1
local publish

local function call(operation, argument)
    local result, err = ship.native.call(operation, argument or "")
    if result then
        local status, body = result:match("^([^\n]+)\n(.*)$")
        if status == "ok" then
            diagnostic = nil
            return body
        end
        diagnostic = body or "Resposta inválida do provider."
    else
        diagnostic = err and (err.message or err.code) or "Provider indisponível."
    end
    ship.log.warn("Policroma: " .. diagnostic)
end

local function refresh()
    local result = call("list")
    if not result then return end
    packs = {}
    for kind, name in result:gmatch("([^\t\n]+)\t([^\n]+)\n") do
        packs[#packs + 1] = { kind = kind, name = name }
    end
    page = math.min(page, math.max(1, math.ceil(#packs / pageSize)))
    ship.log.info("Policroma: " .. #packs .. " candidatos em shaderpacks/.")
end

local function activate(name, requested)
    local request = requested and (name .. "\n" .. requested) or name
    local result = call("load", request)
    if not result then return false end
    active = name
    local profile, choices = result:match("^([^\n]*)\n(.*)$")
    activeProfile = profile ~= "" and profile or nil
    profiles = {}
    for id, label in (choices or ""):gmatch("([^\t\n]+)\t([^\n]+)\n") do
        profiles[#profiles + 1] = { id = id, label = label }
    end
    local saved = activeProfile and (name .. "\n" .. activeProfile) or name
    if storage then ship.storage.set(selectionKey, saved) end
    local suffix = activeProfile and (" (perfil " .. activeProfile .. ")") or ""
    ship.log.info("Policroma: pack ativado " .. name .. suffix)
    return true
end

local function stored(key, default)
    local value = storage and ship.storage.get(key)
    return type(value) == "number" and value or default
end

local function cloudHeight()
    return stored(cloudKey, cloudDefault)
end

-- Manda a altura ao provider. Num host com parâmetros ao vivo vale no próximo quadro; sem eles, só ao recarregar.
local function sendCloudHeight(value)
    if not ship.native.has("parameters") then return end
    local result = call("parameters", "cloud_height=" .. tostring(math.floor(value)))
    cloudNeedsReload = result == "reload"
end

-- Névoa: k = 1/divisor (blocos por unidade do OoT), altura do chão e luz do céu. O mesmo "parameters" do provider.
local function sendFog(key, value)
    if not ship.native.has("parameters") then return end
    call("parameters", key .. "=" .. string.format("%.9g", value))
end

local fogAuto = 0
local function sendFogScale(divisor) sendFog("fog_scale", divisor == fogAuto and 0 or 1 / divisor) end
local function sendGround(value) sendFog("ground_y", math.floor(value)) end
local function sendSkyLight(value) sendFog("sky_light", math.floor(value)) end

local function sendFogSettings()
    sendFogScale(stored(fogDivisorKey, fogDivisorDefault))
    sendGround(stored(groundKey, groundDefault))
    sendSkyLight(stored(skyLightKey, skyLightDefault))
end

local function gbufferF0()
    local value = storage and ship.storage.get(gbufferKey)
    if type(value) == "boolean" then return value end
    return gbufferDefault
end

local function sendGbufferF0(enabled)
    if not ship.native.has("parameters") then return false end
    return call("parameters", "gbuffer_f0=" .. (enabled and "1" or "0")) ~= nil
end

local function off()
    if not call("off") then return end
    active = nil
    activeProfile = nil
    profiles = {}
    if storage then ship.storage.delete(selectionKey) end
    ship.log.info("Policroma: shaderpacks desligados.")
end

local function shortName(name)
    if #name <= 96 then return name end
    local last = 96
    while last > 0 and name:byte(last + 1) >= 128 and name:byte(last + 1) < 192 do
        last = last - 1
    end
    return name:sub(1, last) .. "…"
end

publish = function()
    if not menu then return end
    local widgets = {
        { id = "status", type = "text", label = active and ("Ativo: " .. shortName(active)) or "Shaderpacks desligados" },
        { id = "refresh", type = "action", label = "Atualizar lista",
            on_activate = function() refresh(); publish() end },
        { id = "reload", type = "action", label = "Recarregar pack ativo",
            on_activate = function() if active then activate(active, activeProfile) end; publish() end },
        { id = "off", type = "action", label = "Desligar shaderpack",
            on_activate = function() off(); publish() end },
    }
    if diagnostic then
        widgets[#widgets + 1] = { id = "diagnostic", type = "text", label = shortName(diagnostic:match("^[^\r\n]*")) }
    end
    if ship.native.has("parameters") then
        widgets[#widgets + 1] = { id = "iris", type = "separator", label = "Packs Iris" }
        widgets[#widgets + 1] = { id = "cloud_height", type = "slider_int",
            label = "Altura das nuvens", storage_key = cloudKey, default = cloudDefault,
            min = -1000, max = 8000, step = 50, format = "%d",
            tooltip = "cloudHeight do Iris em unidades do jogo. 3000 passa acima da copa da Árvore Deku.",
            on_change = function(value)
                local before = cloudNeedsReload
                sendCloudHeight(value)
                if cloudNeedsReload ~= before then publish() end
            end }
        widgets[#widgets + 1] = { id = "cloud_reset", type = "action", label = "Altura padrão das nuvens",
            on_activate = function()
                if storage then ship.storage.delete(cloudKey) end
                sendCloudHeight(cloudDefault)
                publish()
            end }
        if cloudNeedsReload then
            widgets[#widgets + 1] = { id = "cloud_reload", type = "text",
                label = "Este host aplica a altura ao recarregar o pack ativo." }
        end
        local fogIsAuto = stored(fogDivisorKey, fogDivisorDefault) == fogAuto
        widgets[#widgets + 1] = { id = "fog_scale", type = "slider_int",
            label = "Escala da névoa (1/N; 0 = automático)", storage_key = fogDivisorKey, default = fogDivisorDefault,
            min = 0, max = 64, step = 1, format = "%d",
            tooltip = "k = 1/N: blocos do pack por unidade do jogo. 1 = sem escala; 32 (padrão) aproxima o jogo de um bloco do Minecraft. 0 = Automático (pela névoa da cena): k sai da névoa nativa de cada cena, limitado a 1/64..1/4.",
            on_change = function(value)
                sendFogScale(value)
                if (value == fogAuto) ~= fogIsAuto then publish() end
            end }
        if fogIsAuto then
            widgets[#widgets + 1] = { id = "fog_auto", type = "text", label = "Automático (pela névoa da cena)" }
        end
        widgets[#widgets + 1] = { id = "ground_y", type = "slider_int",
            label = "Altura do chão", storage_key = groundKey, default = groundDefault,
            min = -2000, max = 8000, step = 10, format = "%d",
            tooltip = "Altura do chão da cena em unidades do jogo; o pack passa a vê-lo perto do bloco 64. 0 = sem deslocamento.",
            on_change = sendGround }
        local skyIsAuto = stored(skyLightKey, skyLightDefault) == skyLightAuto
        widgets[#widgets + 1] = { id = "sky_light", type = "slider_int",
            label = "Luz do céu (caverna; -1 = automático)", storage_key = skyLightKey, default = skyLightDefault,
            min = skyLightAuto, max = 240, step = 10, format = "%d",
            tooltip = "eyeBrightness do pack: 240 = ar livre; valores menores acendem a névoa de caverna. -1 = Automático (pela cena): 30 em interiores, 240 com céu aberto, suavizado pelo host; sem o recurso no host vale 240.",
            on_change = function(value)
                sendSkyLight(value)
                if (value == skyLightAuto) ~= skyIsAuto then publish() end
            end }
        if skyIsAuto then
            widgets[#widgets + 1] = { id = "sky_auto", type = "text", label = "Automático (pela cena)" }
        end
        widgets[#widgets + 1] = { id = "gbuffer_f0", type = "checkbox",
            label = "Normais reconstruídas (F0)", storage_key = gbufferKey, default = gbufferDefault,
            tooltip = "Reconstrói a normal do mundo pela profundidade e preenche colortex4/6/3 para o pack (reflexos e contorno do Complementary). Recarrega o pack ativo.",
            on_change = function(value)
                if sendGbufferF0(value) and active then activate(active, activeProfile) end
                publish()
            end }
        widgets[#widgets + 1] = { id = "fog_reset", type = "action", label = "Névoa padrão (1/32)",
            on_activate = function()
                if storage then
                    ship.storage.delete(fogDivisorKey)
                    ship.storage.delete(groundKey)
                    ship.storage.delete(skyLightKey)
                end
                sendFogSettings()
                publish()
            end }
    end
    if #profiles > 0 then
        widgets[#widgets + 1] = { id = "profiles", type = "separator", label = "Perfis do pack ativo" }
        for _, profile in ipairs(profiles) do
            local selected = activeProfile == profile.id
            widgets[#widgets + 1] = { id = "profile_" .. profile.id, type = "action",
                label = selected and (profile.label .. " (ativo)") or profile.label,
                on_activate = function() activate(active, profile.id); publish() end }
        end
    end
    if #packs == 0 then
        widgets[#widgets + 1] = { id = "empty", type = "text", label = "Coloque um ZIP em shaderpacks/ e atualize a lista." }
    end
    local first = (page - 1) * pageSize + 1
    for i = first, math.min(#packs, first + pageSize - 1) do
        local pack = packs[i]
        widgets[#widgets + 1] = { id = "pack_" .. i, type = "action",
            label = shortName(pack.name) .. " [" .. pack.kind .. "]",
            on_activate = function() activate(pack.name); publish() end }
    end
    if page > 1 then
        widgets[#widgets + 1] = { id = "previous", type = "action", label = "Página anterior",
            on_activate = function() page = page - 1; publish() end }
    end
    if first + pageSize <= #packs then
        widgets[#widgets + 1] = { id = "next", type = "action", label = "Próxima página",
            on_activate = function() page = page + 1; publish() end }
    end
    ship.menu.register({ id = "policroma", title = "Policroma", sidebar = "Policroma", widgets = widgets })
end

refresh()
sendCloudHeight(cloudHeight())
sendFogSettings()
sendGbufferF0(gbufferF0())
local saved = storage and ship.storage.get(selectionKey)
if type(saved) == "string" and saved ~= "" then
    local name, profile = saved:match("^([^\n]+)\n([^\n]+)$")
    if name then
        if not activate(name, profile) then activate(name) end
    else
        activate(saved)
    end
end
publish()
