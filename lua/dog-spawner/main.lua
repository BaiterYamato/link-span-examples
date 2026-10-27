local ship = require("ship")

local active_actor = nil

-- OoT usa handles genéricos e conserva no máximo um cão deste mod.
local function spawnOotDog()
    if active_actor then
        local alive, err = ship.actor.exists(active_actor)
        if alive == nil then
            return nil, err
        end
        if alive then
            local destroyed, destroy_err = ship.actor.destroy(active_actor)
            if not destroyed then
                return nil, destroy_err
            end
        end
        active_actor = nil
    end

    local actor, err = ship.actor.spawn("oot.en_dog", {
        -- No host OoT, a origem pede a posição à frente do jogador.
        position = { x = 0, y = 0, z = 0 },
        rotation = { x = 0, y = 0, z = 0 },
    })
    active_actor = actor
    return actor, err
end

-- Resolve o caminho do jogo ativo, sem depender de conteúdo MM no host OoT.
local function resolveSpawnDog()
    if ship.game.id() == "oot" and ship.capabilities.has("actor.spawn")
        and ship.capabilities.has("actor.destroy") and ship.capabilities.has("actor.exists") then
        return spawnOotDog
    end
    if ship.game.id() == "mm" and ship.capabilities.has("mm.spawn_dog") and ship.mm and ship.mm.spawn_dog then
        return ship.mm.spawn_dog
    end
    return nil
end

ship.events.on("game.ready", function()
    if ship.hotkeys == nil then
        ship.log.warn("host sem ship.hotkeys — Dog Spawner desativado")
        return
    end

    local spawnDog = resolveSpawnDog()
    if spawnDog == nil then
        ship.log.warn("host sem as capabilities necessárias para criar um cachorro")
        return
    end

    ship.hotkeys.register("spawn_dog", { default = "F", label = "Criar cachorro" }, function()
        local dog, err = spawnDog()
        if dog then
            ship.log.info("Au! Cachorro criado")
        elseif err then
            ship.log.warn("Falha ao criar cachorro [" .. err.code .. "]: " .. err.message)
        else
            ship.log.warn("Não foi possível criar aqui — tente em jogo, numa cena com cachorro")
        end
    end)
    ship.log.info("Dog Spawner pronto — aperte F (reconfigurável)")
end)
