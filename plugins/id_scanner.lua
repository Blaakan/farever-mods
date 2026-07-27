-- ============================================================================
-- id_scanner.lua  -  v1.0.0
--
-- Discovers what the plugin runtime actually exposes, and what actually fires,
-- on YOUR install - then exports it as JSON.
--
-- Three jobs:
--
--   1. API PROBE. Walks the `farever` and `imgui` tables reflectively and
--      reports every function that exists, then safely calls the read-only
--      ones to show you a live sample value. This finds getters the docs do
--      not mention and tells you exactly which ones your DLL version has,
--      instead of you guessing from a changelog.
--
--   2. EVENT RECORDER. Registers for every documented event, and records the
--      name, the payload's field names and types, a sample value, and a count.
--      An event the docs never mentioned still shows up here the first time
--      the game fires it.
--
--   3. VOCABULARY RECORDER. Every distinct internal id seen while you play -
--      skills, statuses, items, monsters, POI kinds - with how often and when
--      it was first seen.
--
-- Pair it with the offline scanner (tools/scan-hlboot.mjs), which pulls the
-- complete id list out of the game's bytecode. Static gives you everything
-- that exists; this gives you what is live, what it is called at runtime, and
-- which API surface your build has.
--
-- Read-only. Mutating APIs (add/remove/set/write/toast/sound/log) are never
-- auto-invoked - see MUTATORS below.
-- ============================================================================

local VERSION = "1.0.0"
local TABS = { "API", "Events", "Vocabulary", "Export" }

-- Never auto-call these while probing: they change state or spam the user.
local MUTATORS = {
    add = true, remove = true, set = true, set_style = true, write = true,
    toast = true, sound = true, info = true, warn = true, error = true,
    add_marker = true, remove_marker = true, write_combatlog = true,
    icon = true, atlas_icon = true, text = true, button = true,
}

-- Every event name the host mod documents. Unknown names are still captured -
-- on_event records whatever it is handed - this list only seeds the display.
local KNOWN_EVENTS = {
    "hero_locked", "fight_start", "fight_end", "damage_dealt", "heal_dealt",
    "shield_applied", "target_changed", "cast_start", "cast_end", "weapon_changed",
}

local tab        = 1
local api        = {}      -- { path, kind, sample }
local api_count  = 0
local events     = {}      -- name -> { n, fields = { "k:type" -> sample }, last }
local vocab      = {}      -- category -> { id -> { n, first } }
local last_poll  = 0
local dirty      = false
local last_save  = 0
local status_msg = ""

local POLL = 1.0
local SAVE = 5.0

local function has(fn) return type(fn) == "function" end

-- ---------------------------------------------------------------------------
-- Vocabulary
-- ---------------------------------------------------------------------------

local function note(cat, id)
    if type(id) ~= "string" or id == "" then return end
    local c = vocab[cat]
    if not c then c = {}; vocab[cat] = c end
    local e = c[id]
    if e then
        e.n = e.n + 1
    else
        c[id] = { n = 1, first = farever.now() }
        dirty = true
    end
end

local function vocab_count(cat)
    local n = 0
    for _ in pairs(vocab[cat] or {}) do n = n + 1 end
    return n
end

-- ---------------------------------------------------------------------------
-- API probe
-- ---------------------------------------------------------------------------

local function sample_of(v)
    local t = type(v)
    if t == "number" then
        if v == math.floor(v) then return string.format("%d", v) end
        return string.format("%.3f", v)
    elseif t == "string" then
        if v == "" then return '""' end
        return '"' .. string.sub(v, 1, 40) .. '"'
    elseif t == "boolean" then
        return tostring(v)
    elseif t == "table" then
        return string.format("table[%d]", #v)
    elseif t == "nil" then
        return "nil"
    end
    return t
end

-- Call a zero-arg getter defensively. Anything that needs arguments simply
-- errors inside pcall and is reported as "(needs args)".
local function try_call(fn, name)
    if MUTATORS[name] then return "(not called: mutator)" end
    local ok, v = pcall(fn)
    if not ok then return "(needs args)" end
    return sample_of(v)
end

local function probe(tbl, prefix, depth)
    if type(tbl) ~= "table" or depth > 2 then return end
    local keys = {}
    for k in pairs(tbl) do
        if type(k) == "string" then keys[#keys + 1] = k end
    end
    table.sort(keys)
    for _, k in ipairs(keys) do
        local v = tbl[k]
        local path = prefix .. "." .. k
        local t = type(v)
        if t == "function" then
            api[#api + 1] = { path = path, kind = "fn", sample = try_call(v, k) }
            api_count = api_count + 1
        elseif t == "table" then
            api[#api + 1] = { path = path, kind = "table", sample = "" }
            probe(v, path, depth + 1)
        end
    end
end

local function run_probe()
    api, api_count = {}, 0
    probe(farever, "farever", 0)
    probe(imgui, "imgui", 0)
    status_msg = string.format("probed %d entries", #api)
    farever.log.info("id_scanner: " .. status_msg)
end

-- ---------------------------------------------------------------------------
-- Runtime sampling
-- ---------------------------------------------------------------------------

local function poll()
    if has(farever.player.statuses) then
        local ok, list = pcall(farever.player.statuses)
        if ok and type(list) == "table" then
            for _, s in ipairs(list) do note("status", s.kind) end
        end
    end
    if has(farever.player.skills) then
        local ok, list = pcall(farever.player.skills)
        if ok and type(list) == "table" then
            for _, s in ipairs(list) do note("skill", s.kind) end
        end
    end
    if has(farever.player.weapon_skills) then
        local ok, list = pcall(farever.player.weapon_skills)
        if ok and type(list) == "table" then
            for _, s in ipairs(list) do note("weapon_skill", s.kind) end
        end
    end
    if has(farever.player.inventory) then
        local ok, list = pcall(farever.player.inventory)
        if ok and type(list) == "table" then
            for _, it in ipairs(list) do note("item", it.kind) end
        end
    end
    if has(farever.player.equipment) then
        local ok, list = pcall(farever.player.equipment)
        if ok and type(list) == "table" then
            for _, it in ipairs(list) do
                note("item", it.kind)
                note("slot", it.slot_name)
            end
        end
    end
    if has(farever.player.currencies) then
        local ok, list = pcall(farever.player.currencies)
        if ok and type(list) == "table" then
            for _, c in ipairs(list) do note("currency", c.kind) end
        end
    end
    if farever.target.exists() then
        note("monster", farever.target.name())
        if farever.target.is_casting() then
            note("cast_skill", farever.target.cast_skill())
        end
    end
    if has(farever.pois) then
        local ok, list = pcall(farever.pois)
        if ok and type(list) == "table" then
            for _, p in ipairs(list) do
                note("poi_kind", p.kind)
                if p.subkind and p.subkind ~= "" then note("poi_subkind", p.subkind) end
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- Persistence (store holds scalars only, so records are packed into strings)
-- ---------------------------------------------------------------------------

local function save(force)
    if not dirty then return end
    local now = farever.now()
    if not force and (now - last_save) < SAVE then return end
    for cat, ids in pairs(vocab) do
        local out = {}
        for id in pairs(ids) do
            if not string.find(id, ",", 1, true) then out[#out + 1] = id end
        end
        table.sort(out)
        farever.store.set("v_" .. cat, table.concat(out, ","))
    end
    local names = {}
    for name, e in pairs(events) do names[#names + 1] = name .. ":" .. e.n end
    table.sort(names)
    farever.store.set("events", table.concat(names, ","))
    last_save = now
    dirty = false
end

local function load_saved()
    for _, cat in ipairs({ "status", "skill", "weapon_skill", "item", "slot",
                           "currency", "monster", "cast_skill", "poi_kind",
                           "poi_subkind" }) do
        local raw = farever.store.get("v_" .. cat, "")
        if raw ~= "" then
            for id in string.gmatch(raw, "[^,]+") do note(cat, id) end
        end
    end
end

-- ---------------------------------------------------------------------------
-- Export
-- ---------------------------------------------------------------------------

local function esc(s)
    s = tostring(s)
    s = string.gsub(s, "\\", "\\\\")
    s = string.gsub(s, '"', '\\"')
    s = string.gsub(s, "[\n\r\t]", " ")
    return s
end

local function export()
    local b = {}
    local function add(x) b[#b + 1] = x end

    add('{"plugin":"id_scanner","version":"' .. VERSION .. '"')
    add(',"character":"' .. esc(farever.player.name()) .. '"')
    add(',"class":"' .. esc(has(farever.player.class) and farever.player.class() or "") .. '"')

    add(',"api":[')
    for i, e in ipairs(api) do
        if i > 1 then add(",") end
        add(string.format('{"path":"%s","kind":"%s","sample":"%s"}',
            esc(e.path), esc(e.kind), esc(e.sample)))
    end
    add("]")

    add(',"events":{')
    local enames = {}
    for n in pairs(events) do enames[#enames + 1] = n end
    table.sort(enames)
    for i, n in ipairs(enames) do
        if i > 1 then add(",") end
        local e = events[n]
        add(string.format('"%s":{"count":%d,"fields":[', esc(n), e.n))
        local fkeys = {}
        for f in pairs(e.fields) do fkeys[#fkeys + 1] = f end
        table.sort(fkeys)
        for j, f in ipairs(fkeys) do
            if j > 1 then add(",") end
            add(string.format('{"field":"%s","sample":"%s"}', esc(f), esc(e.fields[f])))
        end
        add("]}")
    end
    add("}")

    add(',"vocabulary":{')
    local cats = {}
    for c in pairs(vocab) do cats[#cats + 1] = c end
    table.sort(cats)
    for i, c in ipairs(cats) do
        if i > 1 then add(",") end
        add(string.format('"%s":[', esc(c)))
        local ids = {}
        for id in pairs(vocab[c]) do ids[#ids + 1] = id end
        table.sort(ids)
        for j, id in ipairs(ids) do
            if j > 1 then add(",") end
            add(string.format('{"id":"%s","seen":%d}', esc(id), vocab[c][id].n))
        end
        add("]")
    end
    add("}}")

    local text = table.concat(b)
    if not has(farever.write_combatlog) then
        farever.toast("export needs mod v1.1.7+")
        return
    end
    local path, err = farever.write_combatlog("farever-api-scan.json", text)
    if path then
        farever.toast("Exported to " .. path, 5.0)
        status_msg = "exported " .. tostring(#text) .. " bytes"
    else
        status_msg = "export failed: " .. tostring(err)
        farever.toast(status_msg, 5.0)
    end
end

-- ---------------------------------------------------------------------------
-- UI
-- ---------------------------------------------------------------------------

local function draw_api()
    imgui.text(string.format("%d entries on this build", #api))
    if imgui.button("Re-probe") then run_probe() end
    imgui.separator()
    local last_ns = ""
    for _, e in ipairs(api) do
        if e.kind == "table" then
            imgui.spacing()
            imgui.text_colored(0.5, 0.8, 1.0, 1.0, e.path)
            last_ns = e.path
        else
            imgui.text(string.format("  %-38s %s",
                string.sub(e.path, 1, 38), string.sub(e.sample, 1, 30)))
        end
    end
end

local function draw_events()
    imgui.text("Events seen this session. Play to fill it in.")
    imgui.separator()
    local names = {}
    for n in pairs(events) do names[#names + 1] = n end
    table.sort(names)

    if #names == 0 then
        imgui.text_colored(0.7, 0.7, 0.7, 1, "nothing fired yet")
    end
    for _, n in ipairs(names) do
        local e = events[n]
        imgui.text_colored(0.5, 1.0, 0.6, 1.0, string.format("%s  x%d", n, e.n))
        local fkeys = {}
        for f in pairs(e.fields) do fkeys[#fkeys + 1] = f end
        table.sort(fkeys)
        for _, f in ipairs(fkeys) do
            imgui.text(string.format("    %-22s %s", f, string.sub(e.fields[f], 1, 34)))
        end
    end

    imgui.separator()
    imgui.text_colored(0.7, 0.7, 0.7, 1, "documented but not yet seen:")
    local line = ""
    for _, n in ipairs(KNOWN_EVENTS) do
        if not events[n] then line = line .. n .. "  " end
    end
    imgui.text("  " .. (line == "" and "(none - all seen)" or line))
end

local function draw_vocab()
    local cats = {}
    for c in pairs(vocab) do cats[#cats + 1] = c end
    table.sort(cats)
    if #cats == 0 then
        imgui.text("nothing recorded yet")
        return
    end
    for _, c in ipairs(cats) do
        imgui.text_colored(0.5, 0.8, 1.0, 1.0,
            string.format("%s (%d)", c, vocab_count(c)))
        local ids = {}
        for id in pairs(vocab[c]) do ids[#ids + 1] = id end
        table.sort(ids)
        for i, id in ipairs(ids) do
            if i > 60 then
                imgui.text(string.format("    ... %d more", #ids - 60))
                break
            end
            imgui.text(string.format("    %-34s x%d", string.sub(id, 1, 34), vocab[c][id].n))
        end
        imgui.spacing()
    end
end

local function draw_export()
    imgui.text("Writes everything above to")
    imgui.text("%LOCALAPPDATA%\\farever-minimap\\combatlogs\\farever-api-scan.json")
    imgui.separator()
    if imgui.button("Export JSON") then export() end
    imgui.same_line()
    if imgui.button("Save now") then save(true); farever.toast("Saved") end
    imgui.same_line()
    if imgui.button("Reset vocabulary") then
        vocab = {}
        dirty = true
        save(true)
        farever.toast("Vocabulary cleared")
    end
    imgui.separator()
    imgui.text(string.format("api %d   events %d   categories %d",
        #api, (function() local n = 0; for _ in pairs(events) do n = n + 1 end; return n end)(),
        (function() local n = 0; for _ in pairs(vocab) do n = n + 1 end; return n end)()))
    if status_msg ~= "" then
        imgui.text_colored(0.7, 0.8, 1, 1, status_msg)
    end
end

-- ---------------------------------------------------------------------------
-- Lifecycle
-- ---------------------------------------------------------------------------

function on_init()
    run_probe()
    load_saved()
    last_poll = farever.now()
    last_save = farever.now()
    farever.log.info("id_scanner v" .. VERSION .. " ready")
end

function on_event(name, data)
    local e = events[name]
    if not e then
        e = { n = 0, fields = {} }
        events[name] = e
        dirty = true
    end
    e.n = e.n + 1

    if type(data) == "table" then
        for k, v in pairs(data) do
            e.fields[tostring(k) .. ":" .. type(v)] = sample_of(v)
        end
        -- Harvest ids straight out of the payload.
        note("skill", data.skill)
        note("monster", data.target)
        if name == "target_changed" then note("monster", data.kind) end
        if name == "weapon_changed" then
            note("item", data.kind)
            note("item", data.prev_kind)
        end
        if name == "fight_end" then note("skill", data.top_skill) end
    end
end

function on_render()
    local now = farever.now()
    if (now - last_poll) >= POLL then
        last_poll = now
        poll()
    end
    save(false)

    local v, c = imgui.combo("##tab", tab, TABS)
    if c then tab = v end
    imgui.separator()

    if     tab == 1 then draw_api()
    elseif tab == 2 then draw_events()
    elseif tab == 3 then draw_vocab()
    else                 draw_export() end
end
