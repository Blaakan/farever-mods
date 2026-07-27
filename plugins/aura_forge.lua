-- ============================================================================
-- aura_forge.lua  -  v1.0.0
--
-- A WeakAuras-style HUD for Farever: movable buff, cooldown and alert displays
-- driven by user-defined trigger rules.
--
-- Built for the farever-minimap plugin runtime:
--   https://github.com/ramisotti13-eng/farever-minimap
-- Drop this file into  <Farever>\data\plugins\  and it hot-loads in ~1s.
--
-- ARCHITECTURE (and why it looks like this)
--
--   The plugin API gives absolute screen-space drawing primitives
--   (draw_rect / draw_text / draw_circle / ...) but the icon widgets
--   (imgui.icon, imgui.atlas_icon) are FLOW widgets that only render at the
--   ImGui cursor inside the plugin's own window. There is also no mouse API
--   and no screen-size query.
--
--   So the plugin splits, exactly the way WeakAuras splits display from
--   options:
--
--     HUD LAYER    - drawn with the absolute primitives, positioned by
--                    anchor + offset, freely movable anywhere on screen.
--                    Shapes and text only. This is the thing you play with.
--
--     CONFIGURATOR - this plugin window. Lists your auras, edits triggers and
--                    positions, and previews with the game's real skill icons
--                    (which is the one place icons can be drawn).
--
--   Because there is no mouse API, "move it around" is anchor + X/Y offset
--   controls plus an Unlock mode that outlines and labels every element,
--   rather than click-drag. Nine screen anchors mean an element stays put
--   when you change resolution.
--
--   Screen size cannot be queried, so it is a setting (Layout tab). Get it
--   right once and every anchor lands correctly.
--
-- TWO THINGS THE GAME DOES NOT HAND US, AND HOW THIS SOLVES THEM
--
--   1. farever.player.skills() reports each skill's cooldown DURATION, not the
--      remaining time. So remaining is tracked client-side: a skill's cooldown
--      starts when we observe it being used (damage_dealt / heal_dealt /
--      shield_applied). Skills that neither damage, heal nor shield are never
--      observed and cannot be tracked. See docs/aura-forge.md.
--
--   2. farever.player.statuses() reports a `duration` whose meaning (total vs
--      remaining) is not specified. StatusTracker below detects which one it
--      is at runtime by watching whether the value decays, then counts down
--      correctly either way.
-- ============================================================================

local VERSION = "1.0.0"

-- ---------------------------------------------------------------------------
-- Constants
-- ---------------------------------------------------------------------------

local ANCHORS = { "Top Left", "Top Center", "Top Right",
                  "Mid Left", "Center",     "Mid Right",
                  "Bot Left", "Bot Center", "Bot Right" }

local GROW      = { "Right", "Left", "Down", "Up" }
local DISPLAYS  = { "Bar", "Tile", "Text" }
local TRIGGERS  = { "Status (buff)", "Skill cooldown", "Resource",
                    "In combat", "Target casting", "Target HP" }
local OPS       = { "<", "<=", ">", ">=" }
local SOUNDS    = { "(none)", "alert", "warning", "info", "beep" }
local RESOURCES = { "health_pct", "shield", "energy", "rage", "spark",
                    "focus", "combo_point", "poise", "oxygen" }

local RES_PRESETS = {
    { label = "1920 x 1080", w = 1920, h = 1080 },
    { label = "2560 x 1440", w = 2560, h = 1440 },
    { label = "3440 x 1440", w = 3440, h = 1440 },
    { label = "3840 x 2160", w = 3840, h = 2160 },
    { label = "1280 x 720",  w = 1280, h = 720  },
    { label = "Custom",      w = 0,    h = 0    },
}
local RES_LABELS = {}
for i, r in ipairs(RES_PRESETS) do RES_LABELS[i] = r.label end

local TABS = { "Status", "Buff bar", "Cooldown bar", "Auras", "Layout", "Share" }

-- ---------------------------------------------------------------------------
-- Minimal JSON codec
--
-- The store only holds scalars, so the whole configuration is serialised to
-- one JSON string. Doubles as the import/export share format. `load` is
-- removed by the sandbox, so this is a hand-written recursive-descent parser.
-- ---------------------------------------------------------------------------

local json = {}

local JESC = {
    ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b',
    ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function jstr(s)
    return '"' .. string.gsub(s, '[%c"\\]', function(c)
        return JESC[c] or string.format("\\u%04x", string.byte(c))
    end) .. '"'
end

function json.encode(v)
    local t = type(v)
    if v == nil then return "null" end
    if t == "boolean" then return tostring(v) end
    if t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "0" end
        if math.type and math.type(v) == "integer" then return string.format("%d", v) end
        return string.format("%.6g", v)
    end
    if t == "string" then return jstr(v) end
    if t == "table" then
        local n = #v
        if n > 0 then
            local out = {}
            for i = 1, n do out[i] = json.encode(v[i]) end
            return "[" .. table.concat(out, ",") .. "]"
        end
        local keys = {}
        for k in pairs(v) do
            if type(k) == "string" then keys[#keys + 1] = k end
        end
        table.sort(keys)
        local out = {}
        for _, k in ipairs(keys) do
            out[#out + 1] = jstr(k) .. ":" .. json.encode(v[k])
        end
        return "{" .. table.concat(out, ",") .. "}"
    end
    return "null"
end

local function jskip(s, i)
    local _, j = string.find(s, "^[ \t\r\n]*", i)
    return (j or i - 1) + 1
end

local jvalue   -- forward declaration

local function jstring(s, i)
    -- s[i] == '"'
    i = i + 1
    local buf = {}
    while true do
        local c = string.sub(s, i, i)
        if c == "" then error("unterminated string") end
        if c == '"' then return table.concat(buf), i + 1 end
        if c == "\\" then
            local e = string.sub(s, i + 1, i + 1)
            if     e == "n" then buf[#buf + 1] = "\n"
            elseif e == "t" then buf[#buf + 1] = "\t"
            elseif e == "r" then buf[#buf + 1] = "\r"
            elseif e == "b" then buf[#buf + 1] = "\b"
            elseif e == "f" then buf[#buf + 1] = "\f"
            elseif e == "u" then
                local hex = string.sub(s, i + 2, i + 5)
                local n = tonumber(hex, 16) or 63
                buf[#buf + 1] = (n < 256) and string.char(n) or "?"
                i = i + 4
            else buf[#buf + 1] = e end
            i = i + 2
        else
            buf[#buf + 1] = c
            i = i + 1
        end
    end
end

jvalue = function(s, i)
    i = jskip(s, i)
    local c = string.sub(s, i, i)
    if c == "" then error("unexpected end of input") end
    if c == '"' then return jstring(s, i) end
    if c == "{" then
        local obj = {}
        i = jskip(s, i + 1)
        if string.sub(s, i, i) == "}" then return obj, i + 1 end
        while true do
            i = jskip(s, i)
            if string.sub(s, i, i) ~= '"' then error("expected key") end
            local k; k, i = jstring(s, i)
            i = jskip(s, i)
            if string.sub(s, i, i) ~= ":" then error("expected ':'") end
            local v; v, i = jvalue(s, i + 1)
            obj[k] = v
            i = jskip(s, i)
            local d = string.sub(s, i, i)
            if d == "," then i = i + 1
            elseif d == "}" then return obj, i + 1
            else error("expected ',' or '}'") end
        end
    end
    if c == "[" then
        local arr = {}
        i = jskip(s, i + 1)
        if string.sub(s, i, i) == "]" then return arr, i + 1 end
        while true do
            local v; v, i = jvalue(s, i)
            arr[#arr + 1] = v
            i = jskip(s, i)
            local d = string.sub(s, i, i)
            if d == "," then i = i + 1
            elseif d == "]" then return arr, i + 1
            else error("expected ',' or ']'") end
        end
    end
    if string.sub(s, i, i + 3) == "true"  then return true,  i + 4 end
    if string.sub(s, i, i + 4) == "false" then return false, i + 5 end
    if string.sub(s, i, i + 3) == "null"  then return nil,   i + 4 end
    local num, j = string.match(s, "^(%-?%d+%.?%d*[eE]?[-+]?%d*)()", i)
    if num then return tonumber(num), j end
    error("unexpected character '" .. c .. "'")
end

function json.decode(s)
    if type(s) ~= "string" or s == "" then return nil, "empty" end
    local ok, v = pcall(function()
        local val = jvalue(s, 1)
        return val
    end)
    if not ok then return nil, tostring(v) end
    return v
end

-- ---------------------------------------------------------------------------
-- Configuration model
-- ---------------------------------------------------------------------------

local function default_group(kind)
    return {
        enabled  = true,
        anchor   = kind == "buff" and 8 or 8,   -- Bot Center
        dx       = kind == "buff" and -260 or 0,
        dy       = kind == "buff" and -180 or -140,
        grow     = 1,        -- Right
        w        = 84,
        h        = 22,
        spacing  = 4,
        limit    = 8,
        include  = "",       -- comma-separated substrings; empty = everything
        exclude  = "",
        show_ready = false,  -- cooldown group only
    }
end

local function default_aura(id)
    return {
        id       = id,
        name     = "New aura",
        enabled  = true,
        trigger  = 1,        -- Status (buff)
        pattern  = "",
        resource = 1,
        op       = 1,
        value    = 0.35,
        invert   = false,
        display  = 1,        -- Bar
        anchor   = 5,        -- Center
        dx       = 0,
        dy       = -140,
        w        = 180,
        h        = 24,
        r = 1.0, g = 0.45, b = 0.25,
        sound    = 1,
        toast    = false,
        pulse    = true,
        load_class = "",
        load_min_level = 0,
        load_combat = false,
    }
end

local cfg = {
    enabled   = true,
    unlocked  = false,
    screen_w  = 1920,
    screen_h  = 1080,
    res_idx   = 1,
    buffs     = default_group("buff"),
    cds       = default_group("cd"),
    auras     = {},
    next_id   = 1,
}

-- ---------------------------------------------------------------------------
-- Runtime state
-- ---------------------------------------------------------------------------

local tab          = 1
local sel_aura     = 1
local share_text   = ""
local share_msg    = ""
local dirty        = false
local last_save    = 0
local last_poll    = 0
local status_cache = {}    -- kind -> tracker record
local skill_cache  = {}    -- kind -> { cooldown, base_cooldown, charges, icon }
local skill_used   = {}    -- kind -> timestamp of last observed use
local fired        = {}    -- aura id -> true while its action has already run

local SAVE_INTERVAL = 2.0
local POLL_INTERVAL = 0.25

local function has(fn) return type(fn) == "function" end
local function mark_dirty() dirty = true end

-- ---------------------------------------------------------------------------
-- Persistence
-- ---------------------------------------------------------------------------

local function save(force)
    if not dirty then return end
    local now = farever.now()
    if not force and (now - last_save) < SAVE_INTERVAL then return end
    farever.store.set("config", json.encode(cfg))
    last_save = now
    dirty = false
end

local function merge_defaults(dst, src)
    for k, v in pairs(src) do
        if dst[k] == nil then dst[k] = v end
    end
    return dst
end

local function load_config()
    local raw = farever.store.get("config", "")
    if raw == "" then return end
    local v, err = json.decode(raw)
    if type(v) ~= "table" then
        farever.log.warn("aura_forge: config unreadable (" .. tostring(err) .. "), using defaults")
        return
    end
    cfg.enabled  = v.enabled ~= false
    cfg.unlocked = v.unlocked == true
    cfg.screen_w = tonumber(v.screen_w) or 1920
    cfg.screen_h = tonumber(v.screen_h) or 1080
    cfg.res_idx  = tonumber(v.res_idx) or 1
    cfg.next_id  = tonumber(v.next_id) or 1
    if type(v.buffs) == "table" then cfg.buffs = merge_defaults(v.buffs, default_group("buff")) end
    if type(v.cds)   == "table" then cfg.cds   = merge_defaults(v.cds,   default_group("cd"))   end
    cfg.auras = {}
    if type(v.auras) == "table" then
        for _, a in ipairs(v.auras) do
            if type(a) == "table" then
                cfg.auras[#cfg.auras + 1] = merge_defaults(a, default_aura(a.id or 0))
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- StatusTracker
--
-- statuses() gives { kind, duration, stacks, shield_amount } but does not say
-- whether `duration` is the total or the remaining time. Watch it: if it
-- decays it is remaining; if it holds steady it is total and we count down
-- from first sight ourselves.
-- ---------------------------------------------------------------------------

local function status_tick(now)
    if not has(farever.player.statuses) then return end
    local ok, list = pcall(farever.player.statuses)
    if not ok or type(list) ~= "table" then return end

    local seen = {}
    for _, s in ipairs(list) do
        local kind = s.kind
        if kind and kind ~= "" then
            seen[kind] = true
            local d   = tonumber(s.duration) or 0
            local rec = status_cache[kind]

            if not rec then
                rec = { kind = kind, first = now, d0 = d, dlast = d,
                        mode = "unknown", stacks = 0, shield = 0 }
                status_cache[kind] = rec
            end

            -- A jump upward means the buff was refreshed / reapplied.
            if d > rec.dlast + 0.25 then
                rec.first = now
                rec.d0    = d
            elseif rec.mode == "unknown" then
                if d < rec.dlast - 0.05 then
                    rec.mode = "remaining"
                elseif (now - rec.first) > 1.5 and math.abs(d - rec.d0) < 0.01 then
                    rec.mode = "total"
                end
            end

            rec.dlast   = d
            rec.stacks  = tonumber(s.stacks) or 0
            rec.shield  = tonumber(s.shield_amount) or 0
            rec.active  = true
            rec.seen_at = now
        end
    end

    for kind, rec in pairs(status_cache) do
        if not seen[kind] then rec.active = false end
    end
end

local function status_remaining(rec, now)
    if not rec or not rec.active then return 0, 0 end
    if rec.mode == "remaining" then
        return math.max(0, rec.dlast), math.max(rec.d0, rec.dlast)
    elseif rec.mode == "total" then
        return math.max(0, rec.dlast - (now - rec.first)), rec.dlast
    end
    -- undecided: show the raw value, assume it is the whole window
    return math.max(0, rec.dlast), math.max(rec.d0, rec.dlast)
end

-- ---------------------------------------------------------------------------
-- CooldownTracker
-- ---------------------------------------------------------------------------

local function skills_tick()
    if not has(farever.player.skills) then return end
    local ok, list = pcall(farever.player.skills)
    if not ok or type(list) ~= "table" then return end
    for _, s in ipairs(list) do
        if s.kind and s.kind ~= "" then
            skill_cache[s.kind] = {
                cooldown      = tonumber(s.cooldown) or 0,
                base_cooldown = tonumber(s.base_cooldown) or 0,
                charges       = tonumber(s.charges) or 0,
                icon          = s.icon,
            }
        end
    end
end

local function cd_remaining(kind, now)
    local rec = skill_cache[kind]
    if not rec or rec.cooldown <= 0 then return 0, 0 end
    local used = skill_used[kind]
    if not used then return 0, rec.cooldown end
    return math.max(0, rec.cooldown - (now - used)), rec.cooldown
end

-- A skill firing several damage events (multi-hit, DoT ticks) must not keep
-- restarting its own cooldown, so only a skill already off cooldown starts one.
local function note_skill_used(kind, now)
    if not kind or kind == "" then return end
    local rem = cd_remaining(kind, now)
    if rem <= 0 then skill_used[kind] = now end
end

-- ---------------------------------------------------------------------------
-- Resource / target reads
-- ---------------------------------------------------------------------------

local function read_resource(name)
    local p = farever.player
    local fn = p[name]
    if has(fn) then
        local ok, v = pcall(fn)
        if ok then return tonumber(v) or 0 end
    end
    return 0
end

-- ---------------------------------------------------------------------------
-- Trigger evaluation
--
-- Returns: active, remaining, total, label, stacks
-- ---------------------------------------------------------------------------

local function match_pattern(kind, pat)
    if pat == "" then return true end
    local lk = string.lower(kind)
    for term in string.gmatch(pat, "[^,]+") do
        term = string.lower((string.gsub(term, "^%s*(.-)%s*$", "%1")))
        if term ~= "" and string.find(lk, term, 1, true) then return true end
    end
    return false
end

local function compare(v, op, target)
    if op == 1 then return v <  target end
    if op == 2 then return v <= target end
    if op == 3 then return v >  target end
    return v >= target
end

local function aura_loaded(a)
    if a.load_class ~= "" and has(farever.player.class) then
        if string.lower(farever.player.class()) ~= string.lower(a.load_class) then
            return false
        end
    end
    if a.load_min_level > 0 and farever.player.level() < a.load_min_level then
        return false
    end
    if a.load_combat and not farever.player.in_combat() then return false end
    return true
end

local function eval_aura(a, now)
    local active, rem, total, label, stacks = false, 0, 0, a.name, 0

    if a.trigger == 1 then                          -- Status (buff)
        local best
        for kind, rec in pairs(status_cache) do
            if rec.active and match_pattern(kind, a.pattern) then
                local r, t = status_remaining(rec, now)
                if not best or r > best.r then
                    best = { r = r, t = t, kind = kind, stacks = rec.stacks }
                end
            end
        end
        if best then
            active, rem, total, stacks = true, best.r, best.t, best.stacks
            if a.name == "" then label = best.kind end
        end

    elseif a.trigger == 2 then                      -- Skill cooldown
        local best
        for kind in pairs(skill_cache) do
            if match_pattern(kind, a.pattern) then
                local r, t = cd_remaining(kind, now)
                if r > 0 and (not best or r > best.r) then
                    best = { r = r, t = t, kind = kind }
                end
            end
        end
        if best then
            active, rem, total = true, best.r, best.t
            if a.name == "" then label = best.kind end
        end

    elseif a.trigger == 3 then                      -- Resource
        local name = RESOURCES[a.resource] or "health_pct"
        local v = read_resource(name)
        active = compare(v, a.op, a.value)
        rem, total = v, math.max(v, a.value)
        if name == "health_pct" then
            label = string.format("%s %.0f%%", a.name, v * 100)
        else
            label = string.format("%s %.0f", a.name, v)
        end

    elseif a.trigger == 4 then                      -- In combat
        active = farever.player.in_combat()

    elseif a.trigger == 5 then                      -- Target casting
        if farever.target.exists() and farever.target.is_casting() then
            local skill = farever.target.cast_skill()
            if match_pattern(skill, a.pattern) then
                active = true
                rem   = farever.target.cast_remaining_sec()
                total = farever.target.cast_total_sec()
                if a.name == "" then label = skill end
            end
        end

    elseif a.trigger == 6 then                      -- Target HP
        if farever.target.exists() then
            local v = farever.target.hp_pct()
            active = compare(v, a.op, a.value)
            rem, total = v, 1.0
            label = string.format("%s %.0f%%", a.name, v * 100)
        end
    end

    if a.invert then
        active = not active
        rem, total = 0, 0
    end
    return active, rem, total, label, stacks
end

-- ---------------------------------------------------------------------------
-- HUD drawing
-- ---------------------------------------------------------------------------

local function anchor_origin(idx)
    local w, h = cfg.screen_w, cfg.screen_h
    local col = (idx - 1) % 3          -- 0 left, 1 center, 2 right
    local row = math.floor((idx - 1) / 3)
    local x = (col == 0) and 0 or (col == 1) and (w / 2) or w
    local y = (row == 0) and 0 or (row == 1) and (h / 2) or h
    return x, y
end

local function fmt_time(t)
    if t <= 0 then return "" end
    if t >= 60 then return string.format("%d:%02d", math.floor(t / 60), math.floor(t % 60)) end
    if t < 10 then return string.format("%.1f", t) end
    return string.format("%d", math.floor(t + 0.5))
end

-- One HUD element. Style 1 = Bar, 2 = Tile, 3 = Text.
local function draw_element(x, y, w, h, style, fill, label, right_text,
                            r, g, b, pulse_on, stacks)
    local a = 1.0
    if pulse_on then a = 0.55 + 0.45 * ((math.sin(farever.now() * 8) + 1) / 2) end

    if style == 3 then
        imgui.draw_text(x, y, r, g, b, a, label)
        if right_text ~= "" then
            imgui.draw_text(x + w - 34, y, r, g, b, a, right_text)
        end
        return
    end

    -- backdrop
    imgui.draw_rect_filled(x, y, x + w, y + h, 0.05, 0.06, 0.09, 0.78)
    -- fill
    if fill > 0 then
        local fw = w * math.max(0, math.min(1, fill))
        imgui.draw_rect_filled(x, y, x + fw, y + h, r, g, b, 0.85 * a)
    end
    -- border
    imgui.draw_rect(x, y, x + w, y + h, r * 0.9, g * 0.9, b * 0.9, a, 1.5)

    if style == 2 then
        -- Tile: centred countdown, small label underneath
        if right_text ~= "" then
            imgui.draw_text(x + w * 0.5 - 10, y + h * 0.5 - 8, 1, 1, 1, a, right_text)
        end
        imgui.draw_text(x, y + h + 1, 0.8, 0.8, 0.85, a * 0.9,
                        string.sub(label, 1, 12))
    else
        imgui.draw_text(x + 5, y + h * 0.5 - 8, 1, 1, 1, a, string.sub(label, 1, 22))
        if right_text ~= "" then
            imgui.draw_text(x + w - 34, y + h * 0.5 - 8, 1, 1, 1, a, right_text)
        end
    end

    if stacks and stacks > 1 then
        imgui.draw_text(x + w - 14, y + 1, 1, 0.9, 0.3, a, tostring(stacks))
    end
end

local function outline(x, y, w, h, name)
    imgui.draw_rect(x - 1, y - 1, x + w + 1, y + h + 1, 0.2, 1.0, 0.4, 0.9, 1.0)
    imgui.draw_text(x, y - 15, 0.2, 1.0, 0.4, 1.0, name)
end

-- Lay out the i-th member of a group. Groups always render as bars, so the
-- vertical step is just the bar height; no room needed for a tile caption.
local function group_slot(grp, ox, oy, i)
    local step = (grp.grow <= 2) and (grp.w + grp.spacing) or (grp.h + grp.spacing)
    local n = i - 1
    if     grp.grow == 1 then return ox + n * step, oy
    elseif grp.grow == 2 then return ox - n * step, oy
    elseif grp.grow == 3 then return ox, oy + n * step
    else                      return ox, oy - n * step end
end

local function draw_buff_group(now)
    local grp = cfg.buffs
    if not grp.enabled then return end
    local ax, ay = anchor_origin(grp.anchor)
    local ox, oy = ax + grp.dx, ay + grp.dy

    local list = {}
    for kind, rec in pairs(status_cache) do
        if rec.active and match_pattern(kind, grp.include)
           and not (grp.exclude ~= "" and match_pattern(kind, grp.exclude)) then
            local r, t = status_remaining(rec, now)
            list[#list + 1] = { kind = kind, r = r, t = t, stacks = rec.stacks }
        end
    end
    table.sort(list, function(p, q) return p.r < q.r end)

    if cfg.unlocked then
        outline(ox, oy, grp.w, grp.h, "Buff bar")
        if #list == 0 then
            draw_element(ox, oy, grp.w, grp.h, 1, 0.6, "(no buffs)", "",
                         0.3, 0.7, 1.0, false, 0)
        end
    end

    for i = 1, math.min(#list, grp.limit) do
        local e = list[i]
        local x, y = group_slot(grp, ox, oy, i)
        local fill = (e.t > 0) and (e.r / e.t) or 1.0
        local r, g, b = 0.3, 0.75, 1.0
        if e.r > 0 and e.r <= 3 then r, g, b = 1.0, 0.55, 0.2 end
        draw_element(x, y, grp.w, grp.h, 1, fill,
                     string.sub(e.kind, 1, 18), fmt_time(e.r),
                     r, g, b, e.r > 0 and e.r <= 2, e.stacks)
    end
end

local function draw_cd_group(now)
    local grp = cfg.cds
    if not grp.enabled then return end
    local ax, ay = anchor_origin(grp.anchor)
    local ox, oy = ax + grp.dx, ay + grp.dy

    local list = {}
    for kind, rec in pairs(skill_cache) do
        if match_pattern(kind, grp.include)
           and not (grp.exclude ~= "" and match_pattern(kind, grp.exclude)) then
            local r, t = cd_remaining(kind, now)
            if r > 0 or grp.show_ready then
                list[#list + 1] = { kind = kind, r = r, t = t }
            end
        end
    end
    table.sort(list, function(p, q) return p.r < q.r end)

    if cfg.unlocked then
        outline(ox, oy, grp.w, grp.h, "Cooldown bar")
        if #list == 0 then
            draw_element(ox, oy, grp.w, grp.h, 1, 0.6, "(no cooldowns)", "",
                         0.8, 0.5, 1.0, false, 0)
        end
    end

    for i = 1, math.min(#list, grp.limit) do
        local e = list[i]
        local x, y = group_slot(grp, ox, oy, i)
        local ready = e.r <= 0
        local fill  = ready and 1.0 or (1.0 - (e.t > 0 and e.r / e.t or 0))
        local r, g, b = 0.75, 0.45, 1.0
        if ready then r, g, b = 0.35, 0.9, 0.45 end
        draw_element(x, y, grp.w, grp.h, 1, fill,
                     string.sub(e.kind, 1, 18),
                     ready and "" or fmt_time(e.r),
                     r, g, b, false, 0)
    end
end

local function run_actions(a, active)
    if active and not fired[a.id] then
        fired[a.id] = true
        if a.sound > 1 then farever.sound(SOUNDS[a.sound]) end
        if a.toast then farever.toast(a.name, 2.0) end
    elseif not active then
        fired[a.id] = nil
    end
end

local function draw_auras(now)
    for _, a in ipairs(cfg.auras) do
        if a.enabled and aura_loaded(a) then
            local active, rem, total, label, stacks = eval_aura(a, now)
            run_actions(a, active)
            if active or cfg.unlocked then
                local ax, ay = anchor_origin(a.anchor)
                local x, y = ax + a.dx, ay + a.dy
                local fill = 1.0
                if total > 0 then fill = math.max(0, math.min(1, rem / total)) end
                local rt = ""
                if a.trigger == 1 or a.trigger == 2 or a.trigger == 5 then
                    rt = fmt_time(rem)
                end
                if cfg.unlocked then outline(x, y, a.w, a.h, a.name) end
                draw_element(x, y, a.w, a.h, a.display, fill, label, rt,
                             a.r, a.g, a.b, a.pulse and active, stacks)
            end
        end
    end
end

local function draw_hud()
    if not cfg.enabled then return end
    if not farever.player.locked() then return end
    local now = farever.now()
    draw_buff_group(now)
    draw_cd_group(now)
    draw_auras(now)
end

-- ---------------------------------------------------------------------------
-- Configurator UI
-- ---------------------------------------------------------------------------

local function group_editor(grp, title, is_cd)
    local v, c
    v, c = imgui.checkbox("Enabled##" .. title, grp.enabled)
    if c then grp.enabled = v; mark_dirty() end

    v, c = imgui.combo("Anchor##" .. title, grp.anchor, ANCHORS)
    if c then grp.anchor = v; mark_dirty() end
    v, c = imgui.drag_float("Offset X##" .. title, grp.dx, 1, -4000, 4000)
    if c then grp.dx = v; mark_dirty() end
    v, c = imgui.drag_float("Offset Y##" .. title, grp.dy, 1, -4000, 4000)
    if c then grp.dy = v; mark_dirty() end
    v, c = imgui.combo("Grow##" .. title, grp.grow, GROW)
    if c then grp.grow = v; mark_dirty() end

    imgui.separator()
    v, c = imgui.drag_float("Width##" .. title, grp.w, 1, 24, 500)
    if c then grp.w = v; mark_dirty() end
    v, c = imgui.drag_float("Height##" .. title, grp.h, 1, 8, 120)
    if c then grp.h = v; mark_dirty() end
    v, c = imgui.drag_float("Spacing##" .. title, grp.spacing, 1, 0, 60)
    if c then grp.spacing = v; mark_dirty() end
    v, c = imgui.drag_float("Max shown##" .. title, grp.limit, 1, 1, 24)
    if c then grp.limit = math.floor(v); mark_dirty() end

    imgui.separator()
    v, c = imgui.input_text("Include##" .. title, grp.include)
    if c then grp.include = v; mark_dirty() end
    v, c = imgui.input_text("Exclude##" .. title, grp.exclude)
    if c then grp.exclude = v; mark_dirty() end
    imgui.text_colored(0.7, 0.7, 0.7, 1,
        "comma-separated substrings, empty Include = everything")

    if is_cd then
        v, c = imgui.checkbox("Also show ready skills##" .. title, grp.show_ready)
        if c then grp.show_ready = v; mark_dirty() end
    end
end

local function draw_status_tab()
    imgui.text(string.format("aura_forge v%s", VERSION))
    local v, c
    v, c = imgui.checkbox("HUD enabled", cfg.enabled)
    if c then cfg.enabled = v; mark_dirty() end
    imgui.same_line()
    v, c = imgui.checkbox("Unlocked (show placement outlines)", cfg.unlocked)
    if c then cfg.unlocked = v; mark_dirty() end
    imgui.separator()

    if not farever.player.locked() then
        imgui.text_colored(1, 0.6, 0.2, 1, "waiting for player lock...")
        return
    end

    local now = farever.now()

    local n_active = 0
    for _, rec in pairs(status_cache) do if rec.active then n_active = n_active + 1 end end
    imgui.text(string.format("Active statuses: %d", n_active))
    for kind, rec in pairs(status_cache) do
        if rec.active then
            local r, t = status_remaining(rec, now)
            imgui.text(string.format("  %-26s %5s  x%d  [%s]",
                string.sub(kind, 1, 26), fmt_time(r), rec.stacks, rec.mode))
        end
    end

    imgui.separator()
    local n_sk, n_cd = 0, 0
    for kind in pairs(skill_cache) do
        n_sk = n_sk + 1
        if cd_remaining(kind, now) > 0 then n_cd = n_cd + 1 end
    end
    imgui.text(string.format("Skills resolved: %d   on cooldown: %d", n_sk, n_cd))
    for kind, rec in pairs(skill_cache) do
        local r = cd_remaining(kind, now)
        -- The configurator is the one place the game's real icons can be drawn.
        if not imgui.icon(kind, 18) then imgui.text(" ") end
        imgui.same_line()
        if r > 0 then
            imgui.text_colored(1, 0.7, 0.3, 1, string.format("%-24s %5s / %.0fs",
                string.sub(kind, 1, 24), fmt_time(r), rec.cooldown))
        else
            imgui.text_colored(0.5, 0.9, 0.5, 1, string.format("%-24s ready (%.0fs)",
                string.sub(kind, 1, 24), rec.cooldown))
        end
    end
    if n_sk == 0 then
        imgui.text_colored(0.7, 0.7, 0.7, 1,
            "Skills resolve as you use them - go hit something.")
    end
end

local function aura_editor()
    local names = {}
    for i, a in ipairs(cfg.auras) do
        names[i] = string.format("%d. %s%s", i, a.name, a.enabled and "" or "  (off)")
    end
    if #names == 0 then names[1] = "(no auras)" end
    if sel_aura > #cfg.auras then sel_aura = math.max(1, #cfg.auras) end

    local v, c = imgui.combo("Aura##sel", sel_aura, names)
    if c then sel_aura = v end

    if imgui.button("Add") then
        local a = default_aura(cfg.next_id)
        cfg.next_id = cfg.next_id + 1
        cfg.auras[#cfg.auras + 1] = a
        sel_aura = #cfg.auras
        mark_dirty()
    end
    imgui.same_line()
    if imgui.button("Duplicate") and cfg.auras[sel_aura] then
        local src = cfg.auras[sel_aura]
        local copy = {}
        for k, val in pairs(src) do copy[k] = val end
        copy.id = cfg.next_id
        copy.name = src.name .. " copy"
        cfg.next_id = cfg.next_id + 1
        cfg.auras[#cfg.auras + 1] = copy
        sel_aura = #cfg.auras
        mark_dirty()
    end
    imgui.same_line()
    if imgui.button("Delete") and cfg.auras[sel_aura] then
        table.remove(cfg.auras, sel_aura)
        if sel_aura > #cfg.auras then sel_aura = math.max(1, #cfg.auras) end
        mark_dirty()
    end

    local a = cfg.auras[sel_aura]
    if not a then
        imgui.separator()
        imgui.text("No aura selected. Hit Add to make one.")
        return
    end

    imgui.separator()
    v, c = imgui.input_text("Name", a.name)
    if c then a.name = v; mark_dirty() end
    v, c = imgui.checkbox("Enabled##a", a.enabled)
    if c then a.enabled = v; mark_dirty() end

    imgui.separator()
    imgui.text("Trigger")
    v, c = imgui.combo("Type", a.trigger, TRIGGERS)
    if c then a.trigger = v; mark_dirty() end

    if a.trigger == 1 or a.trigger == 2 or a.trigger == 5 then
        v, c = imgui.input_text("Match", a.pattern)
        if c then a.pattern = v; mark_dirty() end
        imgui.text_colored(0.7, 0.7, 0.7, 1,
            "substring of the internal id, e.g. 'spark' or 'shield,barrier'")
    elseif a.trigger == 3 then
        v, c = imgui.combo("Resource", a.resource, RESOURCES)
        if c then a.resource = v; mark_dirty() end
        v, c = imgui.combo("Op", a.op, OPS)
        if c then a.op = v; mark_dirty() end
        v, c = imgui.drag_float("Value", a.value, 0.01, 0, 100000)
        if c then a.value = v; mark_dirty() end
        imgui.text_colored(0.7, 0.7, 0.7, 1, "health_pct is 0.0 - 1.0")
    elseif a.trigger == 6 then
        v, c = imgui.combo("Op", a.op, OPS)
        if c then a.op = v; mark_dirty() end
        v, c = imgui.drag_float("Value (0-1)", a.value, 0.01, 0, 1)
        if c then a.value = v; mark_dirty() end
    end

    v, c = imgui.checkbox("Invert (show when NOT matched)", a.invert)
    if c then a.invert = v; mark_dirty() end

    imgui.separator()
    imgui.text("Display")
    v, c = imgui.combo("Style", a.display, DISPLAYS)
    if c then a.display = v; mark_dirty() end
    v, c = imgui.combo("Anchor##a", a.anchor, ANCHORS)
    if c then a.anchor = v; mark_dirty() end
    v, c = imgui.drag_float("Offset X##a", a.dx, 1, -4000, 4000)
    if c then a.dx = v; mark_dirty() end
    v, c = imgui.drag_float("Offset Y##a", a.dy, 1, -4000, 4000)
    if c then a.dy = v; mark_dirty() end
    v, c = imgui.drag_float("Width##a", a.w, 1, 20, 800)
    if c then a.w = v; mark_dirty() end
    v, c = imgui.drag_float("Height##a", a.h, 1, 8, 200)
    if c then a.h = v; mark_dirty() end

    local r, g, b, ch = imgui.color_edit("Color", a.r, a.g, a.b)
    if ch then a.r, a.g, a.b = r, g, b; mark_dirty() end
    v, c = imgui.checkbox("Pulse while active", a.pulse)
    if c then a.pulse = v; mark_dirty() end

    imgui.separator()
    imgui.text("Actions when it turns on")
    v, c = imgui.combo("Sound", a.sound, SOUNDS)
    if c then a.sound = v; mark_dirty() end
    v, c = imgui.checkbox("Toast", a.toast)
    if c then a.toast = v; mark_dirty() end

    imgui.separator()
    imgui.text("Load conditions")
    v, c = imgui.input_text("Only class", a.load_class)
    if c then a.load_class = v; mark_dirty() end
    v, c = imgui.drag_float("Min level", a.load_min_level, 1, 0, 60)
    if c then a.load_min_level = math.floor(v); mark_dirty() end
    v, c = imgui.checkbox("Only in combat", a.load_combat)
    if c then a.load_combat = v; mark_dirty() end

    imgui.separator()
    local active, rem, total, label = eval_aura(a, farever.now())
    if active then
        imgui.text_colored(0.4, 1, 0.4, 1,
            string.format("LIVE: on   %s  %s", label, fmt_time(rem)))
    else
        imgui.text_colored(0.6, 0.6, 0.6, 1, "LIVE: off")
    end
end

local function draw_layout_tab()
    imgui.text("Anchors are computed from the screen size, which the plugin")
    imgui.text("API cannot query. Set it once so anchors land correctly.")
    imgui.separator()

    local v, c = imgui.combo("Resolution", cfg.res_idx, RES_LABELS)
    if c then
        cfg.res_idx = v
        local p = RES_PRESETS[v]
        if p and p.w > 0 then cfg.screen_w, cfg.screen_h = p.w, p.h end
        mark_dirty()
    end
    if cfg.res_idx == #RES_PRESETS then
        v, c = imgui.drag_float("Width", cfg.screen_w, 1, 640, 8000)
        if c then cfg.screen_w = v; mark_dirty() end
        v, c = imgui.drag_float("Height", cfg.screen_h, 1, 480, 5000)
        if c then cfg.screen_h = v; mark_dirty() end
    end
    imgui.text(string.format("Using %.0f x %.0f", cfg.screen_w, cfg.screen_h))

    imgui.separator()
    v, c = imgui.checkbox("Unlocked (outline + label every element)", cfg.unlocked)
    if c then cfg.unlocked = v; mark_dirty() end
    imgui.text_colored(0.7, 0.7, 0.7, 1,
        "Turn this on, nudge the Offset X / Y values, turn it off to play.")

    imgui.separator()
    if imgui.button("Save now") then save(true); farever.toast("Layout saved") end
    imgui.same_line()
    if imgui.button("Reset all positions") then
        cfg.buffs = default_group("buff")
        cfg.cds   = default_group("cd")
        mark_dirty(); save(true)
        farever.toast("Group positions reset")
    end

    imgui.separator()
    imgui.text_colored(0.7, 0.7, 0.7, 1, "If HUD elements do not appear at all,")
    imgui.text_colored(0.7, 0.7, 0.7, 1, "see the clipping note in docs/aura-forge.md.")
end

local function draw_share_tab()
    imgui.text("Export copies your whole setup into the box as JSON.")
    imgui.text("Paste someone else's string in and hit Import.")
    imgui.separator()

    if imgui.button("Export to box") then
        share_text = json.encode(cfg)
        share_msg  = string.format("exported %d chars", #share_text)
    end
    imgui.same_line()
    if imgui.button("Import from box") then
        local v, err = json.decode(share_text)
        if type(v) ~= "table" then
            share_msg = "import failed: " .. tostring(err)
            farever.sound("warning")
        else
            farever.store.set("config", share_text)
            load_config()
            share_msg = "imported OK"
            farever.toast("aura_forge: config imported", 3.0)
        end
    end
    imgui.same_line()
    if imgui.button("Write to file") then
        if has(farever.write_combatlog) then
            local path, err = farever.write_combatlog("aura-forge-config.json",
                                                      json.encode(cfg))
            share_msg = path and ("written to " .. path) or ("failed: " .. tostring(err))
        else
            share_msg = "needs mod v1.1.7+"
        end
    end

    local v, c = imgui.input_text("##share", share_text)
    if c then share_text = v end
    if share_msg ~= "" then
        imgui.text_colored(0.7, 0.8, 1, 1, share_msg)
    end
end

-- ---------------------------------------------------------------------------
-- Lifecycle
-- ---------------------------------------------------------------------------

function on_init()
    load_config()
    if #cfg.auras == 0 then
        -- Two starters so a fresh install shows something useful immediately.
        local low = default_aura(cfg.next_id); cfg.next_id = cfg.next_id + 1
        low.name, low.trigger, low.resource = "LOW HEALTH", 3, 1
        low.op, low.value = 1, 0.35
        low.anchor, low.dx, low.dy = 5, -90, -150
        low.r, low.g, low.b = 1.0, 0.25, 0.25
        low.sound, low.display = 3, 1
        cfg.auras[#cfg.auras + 1] = low

        local cast = default_aura(cfg.next_id); cfg.next_id = cfg.next_id + 1
        cast.name, cast.trigger, cast.pattern = "TARGET CASTING", 5, ""
        cast.anchor, cast.dx, cast.dy = 2, -90, 120
        cast.r, cast.g, cast.b = 1.0, 0.75, 0.2
        cast.display = 1
        cfg.auras[#cfg.auras + 1] = cast

        mark_dirty(); save(true)
    end
    last_poll = farever.now()
    last_save = farever.now()
    farever.log.info("aura_forge v" .. VERSION .. " ready")
end

function on_event(name, data)
    if not data then return end
    local now = farever.now()
    if name == "damage_dealt" or name == "heal_dealt" then
        note_skill_used(data.skill, now)
    elseif name == "shield_applied" then
        note_skill_used(data.skill, now)
    elseif name == "hero_locked" then
        status_cache, skill_cache, skill_used, fired = {}, {}, {}, {}
    end
end

function on_render()
    local now = farever.now()
    if (now - last_poll) >= POLL_INTERVAL then
        last_poll = now
        status_tick(now)
        skills_tick()
    end

    -- The HUD is drawn every frame so countdowns stay smooth.
    draw_hud()
    save(false)

    local v, c = imgui.combo("##tab", tab, TABS)
    if c then tab = v end
    imgui.separator()

    if     tab == 1 then draw_status_tab()
    elseif tab == 2 then group_editor(cfg.buffs, "buffs", false)
    elseif tab == 3 then group_editor(cfg.cds, "cds", true)
    elseif tab == 4 then aura_editor()
    elseif tab == 5 then draw_layout_tab()
    else                 draw_share_tab() end
end
