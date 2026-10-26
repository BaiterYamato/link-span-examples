local ship = require("ship")
local curve = require("leveled.curve")
local hud = {}
local options, state
local numbers, notice = {}, nil
local colors = { enemy = { 255, 255, 255 }, player = { 255, 80, 80 }, xp = { 255, 220, 80 } }

function hud.configure(opts, current)
    options, state = opts, current
end

function hud.reset()
    numbers, notice = {}, nil
end

local function position(number)
    local x, y
    if number.handle then
        x = ship.actor.get(number.handle, "screen_x")
        y = ship.actor.get(number.handle, "screen_y")
    elseif ship.capabilities.has("player.fields") then
        x = ship.player.get("screen_x")
        y = ship.player.get("screen_y")
    end
    if type(x) == "number" and type(y) == "number" then
        number.x, number.y = x, y - 12
    end
end

local function push(handle, amount, kind)
    if not options["numbers_" .. kind] or amount <= 0 then
        return
    end
    local number = {
        handle = handle, kind = kind, age = 0,
        text = kind == "xp" and ("+" .. amount .. " XP") or tostring(amount),
    }
    position(number)
    if #numbers == 24 then
        table.remove(numbers, 1)
    end
    numbers[#numbers + 1] = number
end

function hud.push_number(handle, amount, kind)
    if handle then
        push(handle, amount, kind or "enemy")
    end
end

function hud.push_player_number(amount) push(nil, amount, "player") end
function hud.push_xp(amount) push(nil, amount, "xp") end

function hud.level_up(level, diff)
    local parts = {}
    for _, stat in ipairs({ { "power", "Power" }, { "courage", "Courage" }, { "hearts", "Hearts" } }) do
        local amount = diff[stat[1]] or 0
        if amount ~= 0 then
            parts[#parts + 1] = stat[2] .. " " .. (amount > 0 and "+" or "") .. amount
        end
    end
    notice = { age = 0, title = "LEVEL UP!  Lv " .. level, detail = table.concat(parts, "  ") }
end

-- Tempo em frames do jogo, mesmo sem uma passagem de desenho.
function hud.update()
    for i = #numbers, 1, -1 do
        local number = numbers[i]
        number.age = number.age + 1
        if number.age >= 45 then
            table.remove(numbers, i)
        end
    end
    if notice then
        notice.age = notice.age + 1
        if notice.age >= 90 then
            notice = nil
        end
    end
end

local function text(value, x, y, color, alpha, scale)
    ship.hud.draw_text(value, math.floor(x), math.floor(y), color[1], color[2], color[3],
        math.floor(alpha), scale)
end

local function centered(value, y, scale)
    -- A API não mede texto: aproxima o avanço da fonte de HUD em 8 px.
    text(value, 160 - #value * 8 * scale / 2, y, colors.enemy, 255, scale)
end

function hud.draw()
    if options.xp_bar then
        text("Lv " .. state.level, 26, 54, colors.enemy, 255, 0.6)
        local fraction = 1
        if state.level < curve.MAX_LEVEL then
            local start = curve.xp_at_level(state.level)
            fraction = (state.xp - start) / (curve.xp_at_level(state.level + 1) - start)
        end
        fraction = math.max(0, math.min(1, fraction))
        ship.hud.draw_rect(26, 62, 64, 3, 40, 40, 40, 160)
        ship.hud.draw_rect(26, 62, math.floor(64 * fraction), 3, 90, 200, 255, 220)
    end
    for _, number in ipairs(numbers) do
        if options["numbers_" .. number.kind] then
            position(number)
            if number.x and number.y then
                local alpha = 255 * math.min(1, (45 - number.age) / 15)
                text(number.text, number.x, number.y - number.age * 0.5, colors[number.kind], alpha, 0.6)
            end
        end
    end
    if notice and options.level_up then
        centered(notice.title, 90, 1.0)
        if notice.detail ~= "" then
            centered(notice.detail, 104, 0.6)
        end
    end
end

return hud
