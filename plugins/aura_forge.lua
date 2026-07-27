-- ============================================================================
-- aura_forge.lua  -  v2.0.0
--
-- A WeakAuras-style HUD for Farever: buff bars with icons, stacks and
-- countdowns, cooldown bars, and rule-driven alerts.
--
-- Built for the farever-minimap plugin runtime:
--   https://github.com/ramisotti13-eng/farever-minimap
-- Drop this file into  <Farever>\data\plugins\  and it hot-loads in ~1s.
--
-- v2 ARCHITECTURE
--
--   Everything renders INSIDE the plugin's own window - the pattern the
--   first-party example plugins use (cursor-relative flow widgets). v1 tried
--   to paint a free-floating HUD with the absolute draw primitives; in
--   practice those clip to the plugin window, so v1's HUD was invisible and
--   all you saw was the diagnostics text. The window itself is the HUD now:
--   drag it where you want (the host saves window positions in
--   farever_layout.ini), lock the overlay with the padlock, done.
--
--   The window has two faces:
--     HUD (default)  - alerts, then buffs, then cooldowns, as icon+bar rows
--     settings       - tick the "settings" box to open the config tabs
--
-- WHAT THE LIVE GAME TAUGHT US (read out of a running session's log)
--
--   * statuses() reports duration 0.00 or -1.00 for PERMANENT auras
--     (passives, stack accumulators). v1 treated <=0 as expired and hid
--     them; v2 gives them a "permanent" mode: full bar, stacks, no timer.
--     A stack-only aura is just a permanent aura with stacks > 1.
--   * Timed auras report a positive duration; whether it is total-length
--     or remaining is detected at runtime by watching whether it decays.
--   * Status kinds ("Staff_Censer_Passive_Buff") often have a sibling skill
--     record whose icon IS resolvable ("Staff_Censer..."), so icons are
--     looked up through a fallback chain and cached.
--
-- COOLDOWNS: skills() reports each skill's cooldown DURATION, not remaining.
-- Remaining is derived client-side: a cooldown starts when the skill is seen
-- being used (damage_dealt / heal_dealt / shield_applied events). Skills that
-- neither damage, heal nor shield cannot be tracked - API limit.
-- ============================================================================

local VERSION = "2.0.0"

local TABS      = { "Status", "Buffs", "Cooldowns", "Auras", "Share" }
local TRIGGERS  = { "Status (buff)", "Skill cooldown", "Resource",
                    "In combat", "Target casting", "Target HP" }
local OPS       = { "<", "<=", ">", ">=" }
local SOUNDS    = { "(none)", "alert", "warning", "info", "beep" }
local RESOURCES = { "health_pct", "shield", "energy", "rage", "spark",
                    "focus", "combo_point", "poise", "oxygen" }

-- ---------------------------------------------------------------------------
-- Minimal JSON codec (store holds scalars only; also the share format).
-- `load` is sandboxed away, so this is a hand-written parser.
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

local jvalue

local function jstring(s, i)
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
    local ok, v = pcall(function() return jvalue(s, 1) end)
    if not ok then return nil, tostring(v) end
    return v
end

-- ---------------------------------------------------------------------------
-- Configuration model
-- ---------------------------------------------------------------------------

local function default_group(is_cd)
    return {
        enabled    = true,
        show_icons = true,
        limit      = 10,
        include    = "",
        exclude    = "",
        show_ready = is_cd and false or nil,
    }
end

local function default_aura(id)
    return {
        id       = id,
        name     = "New aura",
        enabled  = true,
        trigger  = 1,
        pattern  = "",
        resource = 1,
        op       = 1,
        value    = 0.35,
        invert   = false,
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
    enabled = true,
    buffs   = default_group(false),
    cds     = default_group(true),
    auras   = {},
    next_id = 1,
}

-- ---------------------------------------------------------------------------
-- Runtime state
-- ---------------------------------------------------------------------------

local ui_settings  = false   -- settings face open? (not persisted: boot = HUD)
local tab          = 1
local share_text   = ""
local share_msg    = ""
local dirty        = false
local last_save    = 0
local last_poll    = 0
local status_cache = {}      -- kind -> tracker record
local skill_cache  = {}      -- kind -> { cooldown, base_cooldown, charges }
local skill_used   = {}      -- kind -> timestamp of last observed use
local fired        = {}      -- aura id -> already-actioned flag
local icon_cache   = {}      -- kind -> resolvable icon kind, or false
local sel_aura     = 1

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
    cfg.enabled = v.enabled ~= false
    cfg.next_id = tonumber(v.next_id) or 1
    if type(v.buffs) == "table" then cfg.buffs = merge_defaults(v.buffs, default_group(false)) end
    if type(v.cds)   == "table" then cfg.cds   = merge_defaults(v.cds,   default_group(true))  end
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
-- Live-game ground truth: permanent auras (passives, stack accumulators)
-- report duration 0.00 or -1.00. Timed auras report a positive duration whose
-- meaning (total vs remaining) is detected by watching whether it decays.
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
            local is_new = false

            if not rec then
                rec = { kind = kind, first = now, d0 = d, dlast = d,
                        mode = "unknown", stacks = 0, shield = 0 }
                status_cache[kind] = rec
                is_new = true
            end

            if d <= 0.01 then
                -- 0 or -1: no time limit. A pure-stack aura is just a
                -- permanent aura whose stacks field moves.
                rec.mode = "permanent"
            elseif d > rec.dlast + 0.25 then
                -- jumped upward: refreshed / reapplied
                rec.first = now
                rec.d0    = d
            elseif rec.mode == "unknown" or rec.mode == "permanent" then
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
            if is_new then
                -- One log line per new status per session: keeps the duration
                -- semantics auditable in farever-mod.log.
                farever.log.info(string.format(
                    "af_status %s duration=%.2f stacks=%d shield=%.0f",
                    kind, d, rec.stacks, rec.shield))
            end
        end
    end

    for kind, rec in pairs(status_cache) do
        if not seen[kind] then rec.active = false end
    end
end

-- remaining, total. Permanent auras return (0, 0) and are flagged by mode.
local function status_remaining(rec, now)
    if not rec or not rec.active then return 0, 0 end
    if rec.mode == "permanent" then return 0, 0 end
    if rec.mode == "remaining" then
        return math.max(0, rec.dlast), math.max(rec.d0, rec.dlast)
    elseif rec.mode == "total" then
        return math.max(0, rec.dlast - (now - rec.first)), rec.dlast
    end
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

-- Multi-hit skills fire several damage events per cast; only a skill that is
-- already off cooldown starts a new one.
local function note_skill_used(kind, now)
    if not kind or kind == "" then return end
    local rem = cd_remaining(kind, now)
    if rem <= 0 then skill_used[kind] = now end
end

-- ---------------------------------------------------------------------------
-- Trigger evaluation
-- ---------------------------------------------------------------------------

local function read_resource(name)
    local fn = farever.player[name]
    if has(fn) then
        local ok, v = pcall(fn)
        if ok then return tonumber(v) or 0 end
    end
    return 0
end

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

-- Returns: active, remaining, total, label, stacks
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

local function run_actions(a, active)
    if active and not fired[a.id] then
        fired[a.id] = true
        if a.sound > 1 then farever.sound(SOUNDS[a.sound]) end
        if a.toast then farever.toast(a.name, 2.0) end
    elseif not active then
        fired[a.id] = nil
    end
end

-- ---------------------------------------------------------------------------
-- HUD rendering (in-window flow widgets)
-- ---------------------------------------------------------------------------

local function fmt_time(t)
    if t <= 0 then return "" end
    if t >= 60 then return string.format("%d:%02d", math.floor(t / 60), math.floor(t % 60)) end
    if t < 10 then return string.format("%.1fs", t) end
    return string.format("%ds", math.floor(t + 0.5))
end

local function short(s, n)
    s = tostring(s)
    -- strip the noise suffixes for display
    s = string.gsub(s, "_Status$", "")
    s = string.gsub(s, "_Buff$", "")
    if #s <= n then return s end
    return string.sub(s, 1, n - 1) .. "~"
end

-- Statuses rarely resolve an icon under their own kind, but their parent
-- skill usually does ("Staff_Censer_Passive_Buff" -> "Staff_Censer_Passive").
-- Try a fallback chain once, cache the winner (or the failure).
local function draw_icon_for(kind, size)
    local hit = icon_cache[kind]
    if hit == false then return false end
    if hit ~= nil then return imgui.icon(hit, size) end

    local candidates = { kind }
    local base = string.gsub(kind, "_Status$", "")
    base = string.gsub(base, "_Buff$", "")
    base = string.gsub(base, "_Debuff$", "")
    if base ~= kind then candidates[#candidates + 1] = base end
    local base2 = string.gsub(base, "_Accum$", "")
    base2 = string.gsub(base2, "_Passive$", "")
    if base2 ~= base then candidates[#candidates + 1] = base2 end

    for _, cand in ipairs(candidates) do
        if imgui.icon(cand, size) then
            icon_cache[kind] = cand
            return true
        end
    end
    icon_cache[kind] = false
    return false
end

local function passes_filters(kind, grp)
    if not match_pattern(kind, grp.include) then return false end
    if grp.exclude ~= "" and match_pattern(kind, grp.exclude) then return false end
    return true
end

local function draw_alerts(now)
    for _, a in ipairs(cfg.auras) do
        if a.enabled and aura_loaded(a) then
            local active, rem, total, label = eval_aura(a, now)
            run_actions(a, active)
            if active then
                local alpha = 1.0
                if a.pulse then
                    alpha = 0.55 + 0.45 * ((math.sin(now * 8) + 1) / 2)
                end
                imgui.font_scale(1.5)
                local text = label
                if rem > 0 and (a.trigger == 1 or a.trigger == 2 or a.trigger == 5) then
                    text = text .. "  " .. fmt_time(rem)
                end
                imgui.text_colored(a.r, a.g, a.b, alpha, text)
                imgui.font_scale(1.0)
                if total > 0 then
                    imgui.progress(math.max(0, math.min(1, rem / total)), "")
                end
            end
        end
    end
end

local function draw_buffs(now)
    local grp = cfg.buffs
    if not grp.enabled then return end

    local timed, perm = {}, {}
    for kind, rec in pairs(status_cache) do
        if rec.active and passes_filters(kind, grp) then
            if rec.mode == "permanent" then
                perm[#perm + 1] = rec
            else
                local r, t = status_remaining(rec, now)
                rec._r, rec._t = r, t
                timed[#timed + 1] = rec
            end
        end
    end
    table.sort(timed, function(p, q) return p._r < q._r end)
    table.sort(perm,  function(p, q) return p.kind < q.kind end)

    local shown = 0
    local function row(rec)
        if shown >= grp.limit then return end
        shown = shown + 1
        if grp.show_icons and draw_icon_for(rec.kind, 20) then
            imgui.same_line()
        end
        local label = short(rec.kind, 24)
        if rec.stacks > 1 then label = label .. "  x" .. rec.stacks end
        if rec.mode == "permanent" then
            imgui.progress(1.0, label)
        else
            local fill = (rec._t > 0) and (rec._r / rec._t) or 0
            imgui.progress(math.max(0, math.min(1, fill)),
                           label .. "   " .. fmt_time(rec._r))
        end
    end

    -- expiring first, then the permanent block
    for _, rec in ipairs(timed) do row(rec) end
    for _, rec in ipairs(perm)  do row(rec) end
end

local function draw_cds(now)
    local grp = cfg.cds
    if not grp.enabled then return end

    local list = {}
    for kind in pairs(skill_cache) do
        if passes_filters(kind, grp) then
            local r, t = cd_remaining(kind, now)
            if r > 0 or grp.show_ready then
                list[#list + 1] = { kind = kind, r = r, t = t }
            end
        end
    end
    if #list == 0 then return end
    table.sort(list, function(p, q) return p.r < q.r end)

    imgui.separator()
    local shown = 0
    for _, e in ipairs(list) do
        if shown >= grp.limit then break end
        shown = shown + 1
        if grp.show_icons and draw_icon_for(e.kind, 20) then
            imgui.same_line()
        end
        if e.r <= 0 then
            imgui.progress(1.0, short(e.kind, 24) .. "   ready")
        else
            local fill = (e.t > 0) and (1.0 - e.r / e.t) or 0
            imgui.progress(math.max(0, math.min(1, fill)),
                           short(e.kind, 24) .. "   " .. fmt_time(e.r))
        end
    end
end

local function draw_hud(now)
    if not farever.player.locked() then
        imgui.text_colored(1, 0.6, 0.2, 1, "waiting for character...")
        return
    end
    draw_alerts(now)
    draw_buffs(now)
    draw_cds(now)
end

-- ---------------------------------------------------------------------------
-- Settings tabs
-- ---------------------------------------------------------------------------

local function draw_status_tab()
    imgui.text(string.format("aura_forge v%s", VERSION))
    local v, c = imgui.checkbox("HUD enabled", cfg.enabled)
    if c then cfg.enabled = v; mark_dirty() end
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
            local r = status_remaining(rec, now)
            local t = (rec.mode == "permanent") and "-" or fmt_time(r)
            imgui.text(string.format("  %-30s %6s  x%d  [%s]",
                string.sub(kind, 1, 30), t, rec.stacks, rec.mode))
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
        if r > 0 then
            imgui.text_colored(1, 0.7, 0.3, 1, string.format("  %-28s %6s / %.0fs",
                string.sub(kind, 1, 28), fmt_time(r), rec.cooldown))
        else
            imgui.text_colored(0.5, 0.9, 0.5, 1, string.format("  %-28s ready (%.0fs)",
                string.sub(kind, 1, 28), rec.cooldown))
        end
    end
    if n_sk == 0 then
        imgui.text_colored(0.7, 0.7, 0.7, 1,
            "Skills resolve as you use them - go hit something.")
    end
end

local function group_editor(grp, title, is_cd)
    local v, c
    v, c = imgui.checkbox("Enabled##" .. title, grp.enabled)
    if c then grp.enabled = v; mark_dirty() end
    v, c = imgui.checkbox("Show icons##" .. title, grp.show_icons)
    if c then grp.show_icons = v; mark_dirty() end
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
        local low = default_aura(cfg.next_id); cfg.next_id = cfg.next_id + 1
        low.name, low.trigger, low.resource = "LOW HEALTH", 3, 1
        low.op, low.value = 1, 0.35
        low.r, low.g, low.b = 1.0, 0.25, 0.25
        low.sound = 3
        cfg.auras[#cfg.auras + 1] = low

        local cast = default_aura(cfg.next_id); cfg.next_id = cfg.next_id + 1
        cast.name, cast.trigger, cast.pattern = "TARGET CASTING", 5, ""
        cast.r, cast.g, cast.b = 1.0, 0.75, 0.2
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
        status_cache, skill_cache, skill_used, fired, icon_cache = {}, {}, {}, {}, {}
    end
end

function on_render()
    local now = farever.now()
    if (now - last_poll) >= POLL_INTERVAL then
        last_poll = now
        status_tick(now)
        skills_tick()
    end
    save(false)

    local v, c = imgui.checkbox("settings", ui_settings)
    if c then ui_settings = v end

    if not ui_settings then
        if cfg.enabled then
            draw_hud(now)
        else
            imgui.text_colored(0.6, 0.6, 0.6, 1, "(HUD disabled - open settings)")
        end
        return
    end

    imgui.separator()
    local t, tc = imgui.combo("##tab", tab, TABS)
    if tc then tab = t end
    imgui.separator()

    if     tab == 1 then draw_status_tab()
    elseif tab == 2 then group_editor(cfg.buffs, "buffs", false)
    elseif tab == 3 then group_editor(cfg.cds, "cds", true)
    elseif tab == 4 then aura_editor()
    else                 draw_share_tab() end
end
