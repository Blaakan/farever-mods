-- ---------------------------------------------------------------------------
-- mock_host.lua
--
-- A stand-in for the farever-minimap plugin runtime, so plugins can be run and
-- exercised outside the game. Mirrors the documented API in
-- farever-minimap/data/plugins/README.md.
--
-- It is deliberately strict where the real host is strict:
--   * store.set rejects anything that is not a string / number / boolean / nil
--   * write_combatlog enforces the single-safe-component filename rule
--   * stateful imgui widgets return (value, changed) in the documented order
--
-- MOCK is the control surface the driver pokes to simulate the game.
-- ---------------------------------------------------------------------------

MOCK = {
    t          = 1000.0,   -- farever.now()
    clicks     = {},       -- label -> true, consumed by the next imgui.button
    checks     = {},       -- label -> forced bool, consumed by the next checkbox
    combo      = {},       -- label -> forced index, makes combo report changed
    draws      = 0,        -- count of absolute draw_* primitives issued
    draw_texts = {},
    texts      = {},       -- everything passed to imgui.text / text_colored
    toasts     = {},
    sounds     = {},
    logs       = {},
    store      = {},
    store_bad  = {},       -- non-scalar values someone tried to persist
    files      = {},
    waypoints  = {},
    next_wp    = 1,
    markers    = {},
    next_mk    = 1,

    locked     = true,
    name       = "Testchar",
    class      = "Mage",
    uid        = "S5a690f03",
    level      = 27,
    in_combat  = false,
    x = 1400.0, y = 1350.0, z = 180.0, rot_z = 0.4,

    statuses   = {},
    skills     = {},
    equipment  = {},
    inventory  = {},
    currencies = {},
    pois       = {},
    codex      = {},

    target = {
        exists = false, name = "", level = 1, hp = 100, max_hp = 100,
        casting = false, cast_skill = "", cast_elapsed = 0, cast_total = 0,
        x = 0, y = 0, z = 0,
    },
}

local function scalar_ok(v)
    local t = type(v)
    return v == nil or t == "string" or t == "number" or t == "boolean"
end

-- ---------------------------------------------------------------------------
-- farever
-- ---------------------------------------------------------------------------

local function res(name, default)
    return function() return MOCK[name] ~= nil and MOCK[name] or default end
end

farever = {}

farever.now = function() return MOCK.t end

farever.player = {
    x = function() return MOCK.x end,
    y = function() return MOCK.y end,
    z = function() return MOCK.z end,
    rot_z = function() return MOCK.rot_z end,
    locked = function() return MOCK.locked end,
    in_combat = function() return MOCK.in_combat end,
    combat_start = function() return 0 end,
    has_target = function() return MOCK.target.exists end,
    name = function() return MOCK.name end,
    class = function() return MOCK.class end,
    uid = function() return MOCK.uid end,
    level = function() return MOCK.level end,

    health = res("health", 800), max_health = res("max_health", 1000),
    health_pct = function() return (MOCK.health or 800) / (MOCK.max_health or 1000) end,
    health_regen = function() return 3 end,
    shield = res("shield", 0),
    energy = res("energy", 45), energy_regen = function() return 2 end,

    vitality = res("vitality", 120), strength = res("strength", 40),
    dexterity = res("dexterity", 55), faith = res("faith", 30),
    intellect = res("intellect", 210),

    crit_chance = function() return 0.24 end,
    crit_damage = function() return 1.75 end,
    armor_penetration = function() return 0.1 end,
    spell_penetration = function() return 0.12 end,
    fervor = function() return 0.05 end,
    block_mitigation = function() return 0 end,
    dodge_chance = function() return 0.03 end,
    magic_mastery = function() return 1.2 end,
    physical_mastery = function() return 0.9 end,
    spell_cast_time_reduction = function() return 0.08 end,
    knock_resistance = function() return 0 end,
    cooldown_reduction = function() return 0.12 end,
    armor = function() return 210 end,
    magic_armor = function() return 180 end,
    magic_reduction = function() return 0.1 end,
    move_speed_factor = function() return 1 end,
    damage = function() return 100 end,
    heal = function() return 0 end,

    poise = res("poise", 0), poise_regen = function() return 0 end,
    oxygen = res("oxygen", 100),
    rage = res("rage", 0), rage_regen = function() return 0 end,
    spark = res("spark", 3), spark_regen = function() return 0.5 end,
    combo_point = res("combo_point", 0),
    focus = res("focus", 0),
    damage_modifier = function() return 1 end,
    damage_taken_modifier = function() return 1 end,
    heal_given_multiplier = function() return 1 end,
    shield_power_multiplier = function() return 1 end,
    glide_speed = function() return 12 end,

    weapon_kind = function() return "Staff_Craft_C" end,
    weapon_level = function() return 27 end,
    weapon_upgrade = function() return 3 end,

    statuses  = function() return MOCK.statuses end,
    skills    = function() return MOCK.skills end,
    equipment = function() return MOCK.equipment end,
    inventory = function() return MOCK.inventory end,
    currencies = function() return MOCK.currencies end,
    weapon_skills = function() return {} end,
    stats = function() return {} end,
    codex = function(kind) return MOCK.codex[kind] end,
}

farever.dps = {
    current = function() return 0 end, total = function() return 0 end,
    elapsed = function() return 0 end, in_combat = function() return MOCK.in_combat end,
}

farever.target = {
    exists = function() return MOCK.target.exists end,
    name = function() return MOCK.target.name end,
    x = function() return MOCK.target.x end,
    y = function() return MOCK.target.y end,
    z = function() return MOCK.target.z end,
    level = function() return MOCK.target.level end,
    hp = function() return MOCK.target.hp end,
    max_hp = function() return MOCK.target.max_hp end,
    hp_pct = function() return MOCK.target.hp / MOCK.target.max_hp end,
    is_casting = function() return MOCK.target.casting end,
    cast_skill = function() return MOCK.target.cast_skill end,
    cast_elapsed_sec = function() return MOCK.target.cast_elapsed end,
    cast_total_sec = function() return MOCK.target.cast_total end,
    cast_remaining_sec = function()
        return math.max(0, MOCK.target.cast_total - MOCK.target.cast_elapsed)
    end,
    cast_progress = function()
        if MOCK.target.cast_total <= 0 then return 0 end
        return MOCK.target.cast_elapsed / MOCK.target.cast_total
    end,
    armor = function() return 0 end,
    magic_armor = function() return 0 end,
    magic_reduction = function() return 0 end,
}

farever.party = {
    is_in_party = function() return false end,
    count = function() return 0 end,
    list = function() return {} end,
}

farever.compass = {
    is_visible = function() return true end,
    radius = function() return 150 end,
    cardinals_on = function() return true end,
    add_marker = function(x, y, opts)
        local id = MOCK.next_mk; MOCK.next_mk = id + 1
        MOCK.markers[id] = { x = x, y = y, opts = opts }
        return id
    end,
    remove_marker = function(id) MOCK.markers[id] = nil; return true end,
}

farever.waypoints = {
    add = function(x, y, z, name, opts)
        if #MOCK.waypoints >= 256 then return nil end
        local id = MOCK.next_wp; MOCK.next_wp = id + 1
        MOCK.waypoints[#MOCK.waypoints + 1] =
            { id = id, x = x, y = y, z = z, name = name, opts = opts }
        return id
    end,
    remove = function(id)
        for i, w in ipairs(MOCK.waypoints) do
            if w.id == id then table.remove(MOCK.waypoints, i); return true end
        end
        return false
    end,
    list = function() return MOCK.waypoints end,
    set_style = function() return true end,
}

farever.pois = function() return MOCK.pois end

farever.store = {
    get = function(k, d)
        local v = MOCK.store[k]
        if v == nil then return d end
        return v
    end,
    set = function(k, v)
        if not scalar_ok(v) then
            MOCK.store_bad[#MOCK.store_bad + 1] = tostring(k) .. "=" .. type(v)
            error("store.set: non-scalar value for key " .. tostring(k))
        end
        MOCK.store[k] = v
    end,
}

farever.write_combatlog = function(filename, contents)
    if type(filename) ~= "string" or not string.match(filename, "^[A-Za-z0-9._%-]+$") then
        return nil, "bad filename"
    end
    if #filename > 128 then return nil, "filename too long" end
    if #contents > 4 * 1024 * 1024 then return nil, "too large" end
    MOCK.files[filename] = contents
    return "C:\\mock\\combatlogs\\" .. filename
end

farever.toast = function(msg, dur)
    MOCK.toasts[#MOCK.toasts + 1] = tostring(msg)
end

farever.sound = function(name)
    MOCK.sounds[#MOCK.sounds + 1] = tostring(name)
end

farever.log = {
    info  = function(m) MOCK.logs[#MOCK.logs + 1] = "INFO  " .. tostring(m) end,
    warn  = function(m) MOCK.logs[#MOCK.logs + 1] = "WARN  " .. tostring(m) end,
    error = function(m) MOCK.logs[#MOCK.logs + 1] = "ERROR " .. tostring(m) end,
}

farever.icons = {
    skill = function(kind)
        return { atlas = "atlas_class_Mage_96PX.png", x = 0, y = 0, size = 96,
                 width = 1, height = 1, px = 0, py = 0, w = 96, h = 96 }
    end,
}

-- ---------------------------------------------------------------------------
-- imgui
-- ---------------------------------------------------------------------------

local function take_click(label)
    if MOCK.clicks[label] then MOCK.clicks[label] = nil; return true end
    return false
end

local function note_text(s)
    MOCK.texts[#MOCK.texts + 1] = s
    if #MOCK.texts > 4000 then table.remove(MOCK.texts, 1) end
end

imgui = {
    text = function(s)
        assert(type(s) == "string", "imgui.text needs a string")
        note_text(s)
    end,
    text_colored = function(r, g, b, a, s)
        assert(type(s) == "string", "imgui.text_colored needs a string last")
        note_text(s)
    end,
    button = function(label) return take_click(label) end,
    checkbox = function(label, cur)
        local f = MOCK.checks[label]
        if f ~= nil then MOCK.checks[label] = nil; return f, true end
        return cur, false
    end,
    slider_float = function(label, v) return v, false end,
    drag_float = function(label, v) return v, false end,
    input_text = function(label, cur) return cur, false end,
    color_edit = function(label, r, g, b) return r, g, b, false end,
    combo = function(label, idx, items)
        assert(type(items) == "table", "imgui.combo needs a table of strings")
        local forced = MOCK.combo[label]
        if forced then return forced, true end
        return idx, false
    end,
    progress = function(v, overlay)
        if type(overlay) == "string" and overlay ~= "" then note_text(overlay) end
    end,
    separator = function() end,
    spacing = function() end,
    same_line = function() end,
    dummy = function(w, h) end,
    font_scale = function(s) end,
    cursor_pos = function() return 100.0, 100.0 end,
    icon = function(kind, size) return true end,
    atlas_icon = function() return true end,

    draw_rect_filled = function() MOCK.draws = MOCK.draws + 1 end,
    draw_rect = function() MOCK.draws = MOCK.draws + 1 end,
    draw_circle_filled = function() MOCK.draws = MOCK.draws + 1 end,
    draw_circle = function() MOCK.draws = MOCK.draws + 1 end,
    draw_line = function() MOCK.draws = MOCK.draws + 1 end,
    draw_triangle_filled = function() MOCK.draws = MOCK.draws + 1 end,
    draw_triangle = function() MOCK.draws = MOCK.draws + 1 end,
    draw_text = function(x, y, r, g, b, a, s)
        MOCK.draws = MOCK.draws + 1
        MOCK.draw_texts[#MOCK.draw_texts + 1] = tostring(s)
    end,
}

-- ---------------------------------------------------------------------------
-- World generators
-- ---------------------------------------------------------------------------

function MOCK.make_pois(n)
    local kinds = { "chest", "red_orb", "plant", "ore",
                    "dungeon", "merchant", "activity" }
    local out = {}
    local seed = 12345
    local function rnd()
        seed = (seed * 1103515245 + 12345) % 2147483648
        return seed / 2147483648
    end
    for i = 1, n do
        local k = kinds[(i % #kinds) + 1]
        out[i] = {
            id   = string.format("poi_%s_%04d", k, i),
            kind = k,
            subkind = "",
            name = string.format("%s %d", k, i),
            x = 1000 + rnd() * 2000,
            y = 1000 + rnd() * 2000,
            z = rnd() * 300,
        }
    end
    return out
end
