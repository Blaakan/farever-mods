-- ============================================================================
-- collection_atlas.lua  -  v1.0.0
--
-- A completion tracker for Farever: what collectibles exist, which ones you
-- still need, and where to find them.
--
-- Built for the farever-minimap plugin runtime:
--   https://github.com/ramisotti13-eng/farever-minimap
-- Drop this file into  <Farever>\data\plugins\  and it hot-loads in ~1s.
--
-- WHAT IT ADDS OVER THE BUILT-IN MINIMAP
--   The host mod already draws collectible markers and lets you right-click
--   one to dim it. This plugin is the bookkeeping layer on top:
--     * completion percentages per category and per area of the world
--     * areas auto-named after the nearest landmark POI, so "where to find
--       them" reads as "7 chests left near <dungeon name>" instead of raw
--       coordinates
--     * a live nearest-uncollected list with distance, heading and altitude
--     * a greedy route planner that pushes real map waypoints
--     * account-wide collection records for what the game treats as
--       collections - mounts, gliders, armor appearances - plus a vault
--       list of bank-worthy weapons and trinkets, observed from your gear
--       and bag
--     * a bestiary/codex progress view
--     * a JSON export of everything
--
--   The plugin keeps its OWN collected-set. The host mod does not expose its
--   poi_done__<name>.json to plugins, so the two are independent by design;
--   see "Marking things collected" in docs/collection-atlas.md.
--
-- API SURFACE USED (all read-only)
--   farever.pois()                     world POI table
--   farever.player.{x,y,z,rot_z,name,class,level,locked,uid}
--   farever.player.{inventory,equipment,currencies,codex}
--   farever.waypoints.{add,remove,list}
--   farever.compass.{add_marker,remove_marker}
--   farever.store.{get,set}            scalars only - see set_encode below
--   farever.write_combatlog(name, text)
--   farever.toast / farever.sound / farever.log / farever.now
-- ============================================================================

local VERSION = "1.1.0"

-- ---------------------------------------------------------------------------
-- Tunables
-- ---------------------------------------------------------------------------

local POLL_INTERVAL  = 1.0   -- seconds between farever.pois() / inventory scans
local FLUSH_INTERVAL = 3.0   -- seconds between store writes (set() hits disk)
local MAX_ROUTE      = 20    -- waypoint budget we are willing to spend

-- One-shot collectibles: collecting them is permanent, so they get a counter.
local ONE_SHOT = { chest = true, red_orb = true }
-- Respawning nodes: no completion meaning, tracked for "nearby" only.
local RESPAWNS = { plant = true, ore = true }
-- Kinds that make good area labels.
local LANDMARK = {
    dungeon = true, merchant = true, activity = true,
    obelisk = true, respawn = true, town = true, camp = true,
}

-- Item routing, matching what the game actually does with items:
--
--   * Mounts, gliders and armor APPEARANCES are account-wide collection
--     unlocks, not bag contents. Armor unlocks its appearance when obtained
--     and is then recycle / sell fodder.
--   * Weapons and trinkets unlock nothing and are usually worth keeping, so
--     they get a separate "vault" record with the best level/upgrade seen.
--   * Everything else (materials, consumables) is churn and is not tracked.
--
-- The plugin API exposes no account-collections getter, so ownership is
-- OBSERVED: an item is recorded when it passes through your equipment or bag.
-- Equip each mount / glider once and the collection fills in.
--
-- Prefixes come from the real id vocabulary extracted out of hlboot.dat by
-- tools/scan-hlboot.mjs (Mount_Boar_05, Glider_Falcon_Blue,
-- Feet_RKobold_FigCle_Craft...). KNOWN_TOTALS are from the same scan of this
-- build - regenerate after a patch.
local KNOWN_TOTALS = { mount = 63, glider = 70 }   -- July 2026 build

local WEAPON_SLOTS  = { Weapon1 = true, Weapon2 = true, OffhandWeapon = true }
local TRINKET_SLOTS = { Trinket = true, Neck = true,
                        FingerLeft = true, FingerRight = true }
local GEAR_SLOTS    = { Head = true, Shoulders = true, Chest = true,
                        Back = true, Hands = true, Waist = true,
                        Legs = true, Feet = true }

local WEAPON_PREFIXES  = { "sword_", "staff_", "bow_", "daggers_", "dagger_",
                           "axe_", "mace_", "hammer_", "spear_", "wand_",
                           "book_", "shield_" }
local ARMOR_PREFIXES   = { "head_", "hair_", "shoulders_", "back_", "hands_",
                           "waist_", "legs_", "feet_", "chest_", "torso_" }
-- "necklace_" / "finger_" confirmed from a live equipment dump
-- (Necklace_Z2RCraft in slot Neck, Finger_Z3RCraft_Cri in the finger slots).
local TRINKET_PREFIXES = { "trinket_", "necklace_", "neck_", "finger_",
                           "ring_", "amulet_" }
-- Companions: Sprout_* ids carry cosmetic variants in the bytecode
-- (Sprout_Rice_Spark, Sprout_Onion_Orange...). Zone-tagged Sprout_*_Z2W ids
-- are world mobs, but mobs never appear in equipment or bags, so the bare
-- prefix is safe in item context.
local COMPANION_PREFIXES = { "sprout_", "companion_", "pet_" }

local TABS = { "Dashboard", "Nearby", "Areas", "Route", "Collections", "Vault", "Codex", "Settings" }

-- ---------------------------------------------------------------------------
-- Runtime state (reset on every hot reload; anything durable lives in store)
-- ---------------------------------------------------------------------------

local cfg = {}            -- user settings, mirrored into the store
local tab = 1

local pois        = {}    -- cached farever.pois() snapshot
local poi_by_id   = {}
local kinds       = {}    -- kind -> { total, done, respawn }
local kind_names  = {}    -- sorted kind list for the combo
local areas       = {}    -- clustered regions
local areas_sorted = {}

local done        = {}    -- id -> true, the collected set for this character
local codex_seen  = {}    -- monster kind -> "state:progress:max"

-- Account-wide records; their store keys carry no character suffix.
local acct   = { mount = {}, glider = {}, companion = {}, appearance = {} }
local acct_n = { mount = 0,  glider = 0,  companion = 0,  appearance = 0 }
local vault  = {}    -- kind -> "level|upgrade|char"

local profile     = ""    -- store key suffix for the active character
local dirty       = false
local last_flush  = 0
local last_poll   = 0
local pois_loaded = false

local route       = {}    -- ordered list of poi refs
local route_wps   = {}    -- waypoint ids we created, so we only remove our own

local filter_kind = 1     -- index into kind_names, 1 == "(all one-shot)"
local status_line = ""

-- ---------------------------------------------------------------------------
-- Small helpers
-- ---------------------------------------------------------------------------

local function has(fn) return type(fn) == "function" end

-- farever.waypoints is a TABLE of functions, so it must be probed for the
-- member, not with has() - testing the table itself always fails and would
-- silently disable every waypoint feature.
local function wp_ready()
    return type(farever.waypoints) == "table" and has(farever.waypoints.add)
end

local function lower(s) return string.lower(tostring(s or "")) end

local function contains(hay, needle)
    return string.find(lower(hay), needle, 1, true) ~= nil
end

local function round(v) return math.floor(v + 0.5) end

local function fmt_dist(d)
    if d >= 1000 then return string.format("%.1fkm", d / 1000) end
    return string.format("%dm", round(d))
end

local function pct(a, b)
    if not b or b <= 0 then return 0 end
    return a / b
end

-- 3D and planar distance from the player to a point.
local function dist_to(px, py, pz, x, y, z)
    local dx, dy, dz = x - px, y - py, (z or 0) - pz
    local d2 = math.sqrt(dx * dx + dy * dy)
    return math.sqrt(d2 * d2 + dz * dz), d2, dz
end

-- Angle to a point relative to where the player is facing. 0 = dead ahead,
-- positive = to the right. Same convention as examples/plugins/nav_arrow.lua.
local function bearing_to(px, py, heading, x, y)
    local dx, dy = x - px, y - py
    local ch, sh = math.cos(heading), math.sin(heading)
    local forward = dx * ch + dy * sh
    local right   = -dx * sh + dy * ch
    return math.atan(right, forward)
end

-- A coarse compass glyph for the bearing, so the list is readable at a glance.
local function arrow_for(theta)
    local d = math.deg(theta)
    if d < 0 then d = d + 360 end
    local sectors = { "^", "/", ">", "\\", "v", "\\", "<", "/" }
    local i = (math.floor((d + 22.5) / 45.0) % 8) + 1
    return sectors[i]
end

-- ---------------------------------------------------------------------------
-- Store codec
--
-- farever.store only holds strings / numbers / booleans, so sets are packed
-- into one comma-joined string (the idiom used by the community POI finder).
-- Ids containing a comma would corrupt the round-trip, so they are dropped
-- with a warning rather than silently mangled.
-- ---------------------------------------------------------------------------

local function set_decode(s)
    local t, n = {}, 0
    if type(s) ~= "string" or s == "" then return t, 0 end
    for id in string.gmatch(s, "[^,]+") do
        if not t[id] then t[id] = true; n = n + 1 end
    end
    return t, n
end

local function set_encode(t)
    local ids, skipped = {}, 0
    for id in pairs(t) do
        if string.find(id, ",", 1, true) then
            skipped = skipped + 1
        else
            ids[#ids + 1] = id
        end
    end
    if skipped > 0 then
        farever.log.warn(string.format(
            "collection_atlas: %d id(s) contain a comma and were not saved", skipped))
    end
    table.sort(ids)
    return table.concat(ids, ",")
end

-- map codec for "key=value" pairs (discovery log, codex log)
local function map_decode(s)
    local t = {}
    if type(s) ~= "string" or s == "" then return t end
    for pair in string.gmatch(s, "[^,]+") do
        local k, v = string.match(pair, "^([^=]+)=(.*)$")
        if k then t[k] = v end
    end
    return t
end

local function map_encode(t)
    local out = {}
    for k, v in pairs(t) do
        if not string.find(k, "[,=]") then
            out[#out + 1] = k .. "=" .. tostring(v)
        end
    end
    table.sort(out)
    return table.concat(out, ",")
end

local function key(name) return name .. "__" .. profile end

local function mark_dirty() dirty = true end

local function flush(force)
    if not dirty then return end
    local now = farever.now()
    if not force and (now - last_flush) < FLUSH_INTERVAL then return end
    farever.store.set(key("done"),  set_encode(done))
    farever.store.set(key("codex"), map_encode(codex_seen))
    farever.store.set("acct_mounts",      set_encode(acct.mount))
    farever.store.set("acct_gliders",     set_encode(acct.glider))
    farever.store.set("acct_companions",  set_encode(acct.companion))
    farever.store.set("acct_appearances", set_encode(acct.appearance))
    farever.store.set("acct_vault",       map_encode(vault))
    last_flush = now
    dirty = false
end

-- ---------------------------------------------------------------------------
-- Settings
-- ---------------------------------------------------------------------------

local function load_cfg()
    cfg.nearby_count       = farever.store.get("nearby_count",       8)
    cfg.auto_collect       = farever.store.get("auto_collect",       false)
    cfg.auto_collect_range = farever.store.get("auto_collect_range", 6.0)
    cfg.proximity_alert    = farever.store.get("proximity_alert",    false)
    cfg.proximity_range    = farever.store.get("proximity_range",    40.0)
    cfg.area_cell          = farever.store.get("area_cell",          500.0)
    cfg.route_size         = farever.store.get("route_size",         8)
    cfg.show_respawn       = farever.store.get("show_respawn",       false)
    cfg.compass_marks      = farever.store.get("compass_marks",      false)

    -- Clamp once here so rebuild_areas() and set_done() always derive the same
    -- grid key; a stale out-of-range value would put them on different grids.
    if type(cfg.area_cell) ~= "number" or cfg.area_cell < 100 then
        cfg.area_cell = 500.0
    end
end

local function set_cfg(k, v)
    cfg[k] = v
    farever.store.set(k, v)
end

-- ---------------------------------------------------------------------------
-- Profile: everything durable is keyed per character, matching how the host
-- mod stores poi_done__<name>.json.
-- ---------------------------------------------------------------------------

local function sanitize(s)
    s = tostring(s or "")
    s = string.gsub(s, "[^%w_%-]", "_")
    if s == "" then s = "default" end
    return s
end

local function bind_profile()
    local name = has(farever.player.name) and farever.player.name() or ""
    if name == "" and has(farever.player.uid) then name = farever.player.uid() end
    local p = sanitize(name)
    if p == profile then return end

    flush(true)              -- persist the previous character before switching
    profile = p
    done       = set_decode(farever.store.get(key("done"), ""))
    codex_seen = map_decode(farever.store.get(key("codex"), ""))
    dirty = false
    farever.log.info("collection_atlas: profile bound to '" .. profile .. "'")
end

-- ---------------------------------------------------------------------------
-- POI ingest
-- ---------------------------------------------------------------------------

local function category_of(kind)
    if ONE_SHOT[kind] then return "one_shot" end
    if RESPAWNS[kind] then return "respawn" end
    if LANDMARK[kind] then return "landmark" end
    return "other"
end

local function rebuild_counts()
    kinds = {}
    for _, p in ipairs(pois) do
        local k = p.kind or "unknown"
        local e = kinds[k]
        if not e then
            e = { total = 0, done = 0, respawn = RESPAWNS[k] or false,
                  cat = category_of(k) }
            kinds[k] = e
        end
        e.total = e.total + 1
        if p.id and done[p.id] then e.done = e.done + 1 end
    end

    kind_names = { "(all one-shot)" }
    local list = {}
    for k in pairs(kinds) do list[#list + 1] = k end
    table.sort(list)
    for _, k in ipairs(list) do kind_names[#kind_names + 1] = k end
    if filter_kind > #kind_names then filter_kind = 1 end
end

-- Cluster POIs into a grid and name each cell after the landmark nearest its
-- centre. That turns "where do I find the rest" into a readable place name.
local function rebuild_areas()
    local cell = cfg.area_cell
    local cells = {}
    for _, p in ipairs(pois) do
        local gx = math.floor((p.x or 0) / cell)
        local gy = math.floor((p.y or 0) / cell)
        local k  = gx .. ":" .. gy
        local c  = cells[k]
        if not c then
            c = { gx = gx, gy = gy, sx = 0, sy = 0, n = 0,
                  total = 0, done = 0, landmarks = {}, items = {} }
            cells[k] = c
        end
        if ONE_SHOT[p.kind] then
            c.total = c.total + 1
            c.sx = c.sx + (p.x or 0)
            c.sy = c.sy + (p.y or 0)
            c.n  = c.n + 1
            c.items[#c.items + 1] = p
            if p.id and done[p.id] then c.done = c.done + 1 end
        elseif LANDMARK[p.kind] then
            c.landmarks[#c.landmarks + 1] = p
        end
    end

    areas, areas_sorted = {}, {}
    for k, c in pairs(cells) do
        if c.total > 0 then
            local cx = c.n > 0 and (c.sx / c.n) or (c.gx * cell)
            local cy = c.n > 0 and (c.sy / c.n) or (c.gy * cell)
            -- name it after the landmark closest to the collectible centroid
            local best, bestd = nil, math.huge
            for _, lm in ipairs(c.landmarks) do
                local dx, dy = (lm.x or 0) - cx, (lm.y or 0) - cy
                local d = dx * dx + dy * dy
                if d < bestd and lm.name and lm.name ~= "" then best, bestd = lm, d end
            end
            c.label = best and best.name
                      or string.format("Area %d,%d", c.gx, c.gy)
            c.cx, c.cy = cx, cy
            c.key = k
            areas[k] = c
            areas_sorted[#areas_sorted + 1] = c
        end
    end

    table.sort(areas_sorted, function(a, b)
        local ra = a.total - a.done
        local rb = b.total - b.done
        if ra ~= rb then return ra > rb end
        return a.label < b.label
    end)
end

local function refresh_pois()
    if not has(farever.pois) then
        status_line = "farever.pois() unavailable - update the mod to v0.5.6.1+"
        return
    end
    local ok, data = pcall(farever.pois)
    if not ok or type(data) ~= "table" then
        status_line = "farever.pois() failed"
        return
    end
    pois = data
    poi_by_id = {}
    for _, p in ipairs(pois) do
        if p.id then poi_by_id[p.id] = p end
    end
    pois_loaded = true
    rebuild_counts()
    rebuild_areas()
    status_line = string.format("%d POIs loaded", #pois)
end

-- ---------------------------------------------------------------------------
-- Collected set
-- ---------------------------------------------------------------------------

local function is_done(p) return p.id ~= nil and done[p.id] == true end

local function set_done(p, v)
    if not p.id then return end
    if v then done[p.id] = true else done[p.id] = nil end
    local e = kinds[p.kind]
    if e then e.done = e.done + (v and 1 or -1) end
    local cell = cfg.area_cell
    local a = areas[math.floor((p.x or 0) / cell) .. ":" .. math.floor((p.y or 0) / cell)]
    if a then a.done = a.done + (v and 1 or -1) end
    mark_dirty()
end

-- ---------------------------------------------------------------------------
-- Nearest-uncollected query
-- ---------------------------------------------------------------------------

-- Index 1 of the category combo means "everything worth chasing": one-shot
-- collectibles, plus respawning nodes if the user opted in. Any other index is
-- an explicit kind, and an explicit choice always wins over the opt-in.
local function wanted_kind(p)
    if filter_kind == 1 then
        if ONE_SHOT[p.kind] then return true end
        return cfg.show_respawn and RESPAWNS[p.kind] == true
    end
    return p.kind == kind_names[filter_kind]
end

local function nearest_uncollected(limit)
    if not pois_loaded then return {} end
    local px, py, pz = farever.player.x(), farever.player.y(), farever.player.z()
    local heading = farever.player.rot_z()

    local out = {}
    for _, p in ipairs(pois) do
        local skip = false
        if not wanted_kind(p) then skip = true end
        if not skip and ONE_SHOT[p.kind] and is_done(p) then skip = true end
        if not skip then
            local d3, d2, dz = dist_to(px, py, pz, p.x or 0, p.y or 0, p.z or 0)
            out[#out + 1] = {
                poi = p, d = d3, d2 = d2, dz = dz,
                bearing = bearing_to(px, py, heading, p.x or 0, p.y or 0),
            }
        end
    end
    table.sort(out, function(a, b) return a.d < b.d end)

    local n = limit or cfg.nearby_count
    while #out > n do table.remove(out) end
    return out
end

-- ---------------------------------------------------------------------------
-- Auto-collect + proximity alert
-- ---------------------------------------------------------------------------

local alerted = {}   -- id -> true, so a single approach only pings once

local function proximity_tick()
    if not pois_loaded or not farever.player.locked() then return end
    if not (cfg.auto_collect or cfg.proximity_alert) then return end

    local px, py, pz = farever.player.x(), farever.player.y(), farever.player.z()
    local ar = cfg.auto_collect_range
    local pr = cfg.proximity_range

    for _, p in ipairs(pois) do
        if ONE_SHOT[p.kind] and not is_done(p) then
            local d = dist_to(px, py, pz, p.x or 0, p.y or 0, p.z or 0)
            if cfg.auto_collect and d <= ar then
                set_done(p, true)
                farever.toast(string.format("Collected: %s",
                    (p.name ~= "" and p.name) or p.kind))
                farever.sound("info")
                alerted[p.id] = nil
                -- Rare enough to afford a write, and losing a pickup to a
                -- crash is exactly what the batching must not cost us.
                flush(true)
            elseif cfg.proximity_alert and d <= pr then
                if not alerted[p.id] then
                    alerted[p.id] = true
                    farever.toast(string.format("%s nearby (%s)",
                        (p.name ~= "" and p.name) or p.kind, fmt_dist(d)))
                end
            elseif d > pr * 1.5 then
                alerted[p.id] = nil
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- Collections & vault
--
-- Ownership is observed from equipment() and inventory(): the API has no
-- account-collections getter, so an entry is recorded the first time the item
-- passes through your hands. Both records are account-wide.
-- ---------------------------------------------------------------------------

local function starts_with_any(k, prefixes)
    for _, p in ipairs(prefixes) do
        if string.sub(k, 1, #p) == p then return true end
    end
    return false
end

-- What is this item to the tracker? slot_name (present on equipment entries)
-- is authoritative; bag items carry no slot, so prefixes fill in.
local function route_of(kind, slot_name)
    local k = lower(kind)
    if starts_with_any(k, { "mount_" })  then return "mount" end
    if starts_with_any(k, { "glider_" }) then return "glider" end
    if starts_with_any(k, COMPANION_PREFIXES) then return "companion" end
    if slot_name and slot_name ~= "" then
        if WEAPON_SLOTS[slot_name]  then return "weapon" end
        if TRINKET_SLOTS[slot_name] then return "trinket" end
        if GEAR_SLOTS[slot_name]    then return "armor" end
        return nil                       -- Pickaxe, Sickle, unnamed slots
    end
    if starts_with_any(k, WEAPON_PREFIXES)  then return "weapon" end
    if starts_with_any(k, TRINKET_PREFIXES) then return "trinket" end
    if string.find(k, "_trinket", 1, true)  then return "trinket" end
    if starts_with_any(k, ARMOR_PREFIXES)   then return "armor" end
    return nil
end

local function note_collection(cat, kind)
    if acct[cat][kind] then return end
    acct[cat][kind] = true
    acct_n[cat] = acct_n[cat] + 1
    mark_dirty()
    local total = KNOWN_TOTALS[cat]
    if total then
        farever.toast(string.format("New %s recorded: %s (%d / %d known)",
            cat, kind, acct_n[cat], total), 3.0)
    elseif cat == "appearance" then
        farever.toast("Appearance unlocked: " .. kind, 3.0)
    else
        farever.toast(string.format("New %s recorded: %s", cat, kind), 3.0)
    end
    farever.sound("info")
end

local function note_vault(kind, level, upgrade)
    level, upgrade = tonumber(level) or 0, tonumber(upgrade) or 0
    -- Utility slots report garbage level/upgrade values (uninitialised
    -- reads like -1745460232 seen in a live dump); clamp to sane ranges.
    if level < 0 or level > 999 then level = 0 end
    if upgrade < 0 or upgrade > 99 then upgrade = 0 end
    local cur = vault[kind]
    if cur then
        local l, u = string.match(cur, "^(%-?%d+)|(%-?%d+)")
        if level > (tonumber(l) or 0) or upgrade > (tonumber(u) or 0) then
            vault[kind] = string.format("%d|%d|%s", level, upgrade, profile)
            mark_dirty()
        end
        return
    end
    vault[kind] = string.format("%d|%d|%s", level, upgrade, profile)
    mark_dirty()
    farever.toast("Vault keeper: " .. kind, 3.0)
end

local function note_item(kind, level, upgrade, slot_name)
    if not kind or kind == "" then return end
    local r = route_of(kind, slot_name)
    if r == "mount" or r == "glider" or r == "companion" then
        note_collection(r, kind)
    elseif r == "armor" then
        note_collection("appearance", kind)
    elseif r == "weapon" or r == "trinket" then
        note_vault(kind, level, upgrade)
    end
end

-- One-shot per load: log the raw item ids so routing rules can be checked
-- against reality (the lines land in farever-mod.log as "ca_dump ...").
local dumped = false
local function dump_items()
    if dumped or not has(farever.player.equipment) then return end
    local eq_ok, eq = pcall(farever.player.equipment)
    if not eq_ok or type(eq) ~= "table" or #eq == 0 then return end
    for _, it in ipairs(eq) do
        farever.log.info(string.format("ca_dump equip kind=%s slot=%s(%s) lvl=%d upg=%d",
            tostring(it.kind), tostring(it.slot), tostring(it.slot_name),
            tonumber(it.level) or 0, tonumber(it.upgrade) or 0))
    end
    if has(farever.player.inventory) then
        local ok, inv = pcall(farever.player.inventory)
        if ok and type(inv) == "table" then
            for _, it in ipairs(inv) do
                farever.log.info(string.format("ca_dump bag kind=%s stack=%d lvl=%d",
                    tostring(it.kind), tonumber(it.stack) or 0, tonumber(it.level) or 0))
            end
        end
    end
    dumped = true
end

local function collections_tick()
    if not farever.player.locked() then return end
    dump_items()
    if has(farever.player.equipment) then
        local ok, items = pcall(farever.player.equipment)
        if ok and type(items) == "table" then
            for _, it in ipairs(items) do
                note_item(it.kind, it.level, it.upgrade, it.slot_name)
            end
        end
    end
    if has(farever.player.inventory) then
        local ok, items = pcall(farever.player.inventory)
        if ok and type(items) == "table" then
            for _, it in ipairs(items) do
                note_item(it.kind, it.level, it.upgrade, nil)
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- Codex / bestiary
-- ---------------------------------------------------------------------------

local function note_codex(kind)
    if not kind or kind == "" or not has(farever.player.codex) then return end
    local ok, c = pcall(farever.player.codex, kind)
    if not ok or type(c) ~= "table" then return end
    local rec = string.format("%s|%d|%d|%s",
        tostring(c.state), tonumber(c.progress) or 0,
        tonumber(c.max) or 0, tostring(c.name or kind))
    if codex_seen[kind] ~= rec then
        codex_seen[kind] = rec
        mark_dirty()
    end
end

-- ---------------------------------------------------------------------------
-- Route planner: greedy nearest-neighbour over the N nearest uncollected.
-- ---------------------------------------------------------------------------

local function clear_route_waypoints()
    if type(farever.waypoints) == "table" and has(farever.waypoints.remove) then
        for _, id in ipairs(route_wps) do pcall(farever.waypoints.remove, id) end
    end
    route_wps = {}
end

local function build_route()
    route = {}
    local n = math.min(cfg.route_size, MAX_ROUTE)
    local pool = nearest_uncollected(n * 3)
    if #pool == 0 then return end

    local cx, cy, cz = farever.player.x(), farever.player.y(), farever.player.z()
    local used = {}
    for _ = 1, math.min(n, #pool) do
        local best, bestd, besti = nil, math.huge, nil
        for i, e in ipairs(pool) do
            if not used[i] then
                local d = dist_to(cx, cy, cz, e.poi.x or 0, e.poi.y or 0, e.poi.z or 0)
                if d < bestd then best, bestd, besti = e, d, i end
            end
        end
        if not best then break end
        used[besti] = true
        route[#route + 1] = { poi = best.poi, leg = bestd }
        cx, cy, cz = best.poi.x or 0, best.poi.y or 0, best.poi.z or 0
    end
end

local function push_route_waypoints()
    if not wp_ready() then
        farever.toast("waypoint API unavailable")
        return
    end
    clear_route_waypoints()
    for i, step in ipairs(route) do
        local p = step.poi
        local ok, id = pcall(farever.waypoints.add, p.x, p.y, p.z or 0,
            string.format("%d. %s", i, (p.name ~= "" and p.name) or p.kind),
            { color = "orange", icon = "diamond" })
        if ok and id then route_wps[#route_wps + 1] = id end
    end
    farever.toast(string.format("%d route waypoints placed", #route_wps))
end

-- ---------------------------------------------------------------------------
-- Export
-- ---------------------------------------------------------------------------

local function json_escape(s)
    s = tostring(s)
    s = string.gsub(s, "\\", "\\\\")
    s = string.gsub(s, '"', '\\"')
    s = string.gsub(s, "\n", "\\n")
    s = string.gsub(s, "\r", "\\r")
    s = string.gsub(s, "\t", "\\t")
    return s
end

local function export_json()
    local b = {}
    local function add(s) b[#b + 1] = s end

    add('{"plugin":"collection_atlas","version":"' .. VERSION .. '"')
    add(',"character":"' .. json_escape(profile) .. '"')
    add(',"level":' .. tostring(farever.player.level()))
    add(',"class":"' .. json_escape(has(farever.player.class) and farever.player.class() or "") .. '"')

    add(',"categories":{')
    local first = true
    local names = {}
    for k in pairs(kinds) do names[#names + 1] = k end
    table.sort(names)
    for _, k in ipairs(names) do
        local e = kinds[k]
        if not first then add(",") end
        first = false
        add(string.format('"%s":{"total":%d,"done":%d,"respawn":%s}',
            json_escape(k), e.total, e.done, tostring(e.respawn)))
    end
    add("}")

    add(',"areas":[')
    for i, a in ipairs(areas_sorted) do
        if i > 1 then add(",") end
        add(string.format('{"label":"%s","x":%.1f,"y":%.1f,"total":%d,"done":%d}',
            json_escape(a.label), a.cx, a.cy, a.total, a.done))
    end
    add("]")

    add(',"collections":{')
    local ckeys = { "mount", "glider", "companion", "appearance" }
    for ci, cat in ipairs(ckeys) do
        if ci > 1 then add(",") end
        add(string.format('"%s":[', cat))
        local names = {}
        for k in pairs(acct[cat]) do names[#names + 1] = k end
        table.sort(names)
        for i, k in ipairs(names) do
            if i > 1 then add(",") end
            add('"' .. json_escape(k) .. '"')
        end
        add("]")
    end
    add("}")

    add(',"vault":[')
    local vnames = {}
    for k in pairs(vault) do vnames[#vnames + 1] = k end
    table.sort(vnames)
    for i, k in ipairs(vnames) do
        if i > 1 then add(",") end
        local l, u, ch = string.match(vault[k] or "", "^(%-?%d+)|(%-?%d+)|(.*)$")
        add(string.format('{"kind":"%s","level":%s,"upgrade":%s,"char":"%s"}',
            json_escape(k), l or "0", u or "0", json_escape(ch or "")))
    end
    add("]")

    add(',"codex":[')
    local cnames = {}
    for k in pairs(codex_seen) do cnames[#cnames + 1] = k end
    table.sort(cnames)
    for i, k in ipairs(cnames) do
        if i > 1 then add(",") end
        local st, pr, mx, nm = string.match(codex_seen[k], "^([^|]*)|([^|]*)|([^|]*)|(.*)$")
        add(string.format('{"kind":"%s","name":"%s","state":"%s","progress":%s,"max":%s}',
            json_escape(k), json_escape(nm or k), json_escape(st or ""),
            pr or "0", mx or "0"))
    end
    add("]}")

    local text = table.concat(b)
    if not has(farever.write_combatlog) then
        farever.toast("export needs mod v1.1.7+")
        return
    end
    local path, err = farever.write_combatlog("collection-atlas-" .. profile .. ".json", text)
    if path then
        farever.toast("Exported to " .. path, 5.0)
        farever.log.info("collection_atlas: exported " .. path)
    else
        farever.toast("Export failed: " .. tostring(err), 5.0)
    end
end

-- ---------------------------------------------------------------------------
-- Rendering
-- ---------------------------------------------------------------------------

local function bar(label, done_n, total_n)
    local p = pct(done_n, total_n)
    imgui.text(label)
    imgui.same_line()
    imgui.progress(p, string.format("%d / %d  (%.0f%%)", done_n, total_n, p * 100))
end

local function draw_dashboard()
    imgui.text(string.format("Character: %s   lvl %d   %s",
        profile, farever.player.level(),
        has(farever.player.class) and farever.player.class() or "?"))
    imgui.separator()

    -- Account collections up front - this is what most people came for.
    imgui.text_colored(0.5, 0.8, 1.0, 1.0, "Account collections")
    bar("Mounts ", acct_n.mount,  KNOWN_TOTALS.mount)
    bar("Gliders", acct_n.glider, KNOWN_TOTALS.glider)
    imgui.text(string.format("Companions %d    Appearances %d    Vault %d",
        acct_n.companion, acct_n.appearance,
        (function() local n = 0; for _ in pairs(vault) do n = n + 1 end; return n end)()))
    imgui.text_colored(0.7, 0.7, 0.7, 1,
        "  full lists in the Collections / Vault tabs (dropdown above)")
    imgui.separator()

    local t_total, t_done = 0, 0
    local names = {}
    for k in pairs(kinds) do names[#names + 1] = k end
    table.sort(names)

    imgui.text("One-shot collectibles")
    for _, k in ipairs(names) do
        local e = kinds[k]
        if e.cat == "one_shot" then
            bar(string.format("%-10s", k), e.done, e.total)
            t_total = t_total + e.total
            t_done  = t_done + e.done
        end
    end
    imgui.spacing()
    bar("TOTAL     ", t_done, t_total)

    imgui.separator()
    imgui.text("Respawning nodes (no completion)")
    for _, k in ipairs(names) do
        local e = kinds[k]
        if e.cat == "respawn" then
            imgui.text(string.format("  %-10s %d spawn points", k, e.total))
        end
    end

    imgui.separator()
    imgui.text("Landmarks")
    for _, k in ipairs(names) do
        local e = kinds[k]
        if e.cat == "landmark" or e.cat == "other" then
            imgui.text(string.format("  %-10s %d", k, e.total))
        end
    end
end

local function draw_nearby()
    local v, c
    v, c = imgui.combo("Category", filter_kind, kind_names)
    if c then filter_kind = v end
    v, c = imgui.drag_float("Show", cfg.nearby_count, 1, 1, 30)
    if c then set_cfg("nearby_count", math.floor(v)) end
    imgui.separator()

    if not farever.player.locked() then
        imgui.text_colored(1, 0.6, 0.2, 1, "waiting for player lock...")
        return
    end

    local list = nearest_uncollected(math.floor(cfg.nearby_count))
    if #list == 0 then
        imgui.text_colored(0.4, 1, 0.4, 1, "Nothing left in this category. Done!")
        return
    end

    for i, e in ipairs(list) do
        local p = e.poi
        local label = (p.name and p.name ~= "") and p.name or p.kind
        local uid = tostring(p.id or i)

        imgui.text(string.format("%s %-22s %7s  dz %+.0fm",
            arrow_for(e.bearing), string.sub(label, 1, 22), fmt_dist(e.d), e.dz))

        if imgui.button("Waypoint##wp" .. uid) then
            if wp_ready() then
                pcall(farever.waypoints.add, p.x, p.y, p.z or 0, label,
                      { color = "cyan", icon = "star" })
                farever.toast("Waypoint: " .. label)
            end
        end
        if ONE_SHOT[p.kind] then
            imgui.same_line()
            if imgui.button("Mark done##md" .. uid) then
                set_done(p, true)
                farever.sound("info")
            end
        end
        imgui.separator()
    end
end

local function draw_areas()
    imgui.text("One-shot collectible completion by area.")
    imgui.text("Areas are named after the nearest landmark POI.")
    imgui.separator()

    if #areas_sorted == 0 then
        imgui.text("no areas yet - waiting for the POI table")
        return
    end

    local shown = 0
    for _, a in ipairs(areas_sorted) do
        if shown >= 25 then break end
        shown = shown + 1
        local left = a.total - a.done
        local r, g, bl = 1, 0.8, 0.3
        if left == 0 then r, g, bl = 0.4, 1, 0.4 end
        imgui.text_colored(r, g, bl, 1, string.format("%-26s %2d left",
            string.sub(a.label, 1, 26), left))
        imgui.progress(pct(a.done, a.total),
            string.format("%d / %d", a.done, a.total))
        if imgui.button("Waypoint##ar" .. a.key) then
            if wp_ready() then
                pcall(farever.waypoints.add, a.cx, a.cy, 0, a.label,
                      { color = "yellow", icon = "flag" })
                farever.toast("Waypoint: " .. a.label)
            end
        end
        imgui.separator()
    end
end

local function draw_route()
    local v, c = imgui.drag_float("Stops", cfg.route_size, 1, 1, MAX_ROUTE)
    if c then set_cfg("route_size", math.floor(v)) end

    if imgui.button("Plan route") then build_route() end
    imgui.same_line()
    if imgui.button("Place waypoints") then push_route_waypoints() end
    imgui.same_line()
    if imgui.button("Clear waypoints") then
        clear_route_waypoints()
        farever.toast("Route waypoints cleared")
    end
    imgui.separator()

    if #route == 0 then
        imgui.text("No route planned. Pick a category on the Nearby tab,")
        imgui.text("then hit Plan route.")
        return
    end

    local total = 0
    for i, step in ipairs(route) do
        total = total + step.leg
        local p = step.poi
        imgui.text(string.format("%2d. %-24s  +%s",
            i, string.sub((p.name ~= "" and p.name) or p.kind, 1, 24),
            fmt_dist(step.leg)))
    end
    imgui.separator()
    imgui.text(string.format("Total path: %s over %d stops", fmt_dist(total), #route))
end

local function draw_collections()
    imgui.text("Account-wide unlocks, observed as items pass through your")
    imgui.text("hands. Equip each mount / glider once to record it.")
    imgui.separator()

    bar("Mounts ", acct_n.mount,  KNOWN_TOTALS.mount)
    bar("Gliders", acct_n.glider, KNOWN_TOTALS.glider)
    imgui.text(string.format("Companions: %d    Appearances unlocked: %d",
        acct_n.companion, acct_n.appearance))
    imgui.text_colored(0.7, 0.7, 0.7, 1,
        "  armor seen = appearance unlocked = safe to recycle / sell")
    imgui.separator()

    local sections = {
        { label = "Mounts",      set = acct.mount },
        { label = "Gliders",     set = acct.glider },
        { label = "Companions",  set = acct.companion },
        { label = "Appearances", set = acct.appearance },
    }
    for _, sec in ipairs(sections) do
        local list = {}
        for k in pairs(sec.set) do list[#list + 1] = k end
        table.sort(list)
        imgui.text_colored(0.5, 0.8, 1.0, 1.0,
            string.format("%s (%d)", sec.label, #list))
        if #list == 0 then
            imgui.text("    (none recorded yet)")
        end
        for i, k in ipairs(list) do
            if i > 40 then
                imgui.text(string.format("    ... %d more", #list - 40))
                break
            end
            imgui.text("    " .. k)
        end
        imgui.spacing()
    end
end

local function draw_vault()
    imgui.text("Weapons and trinkets seen on this account - the items worth")
    imgui.text("keeping in the bank, with the best level / upgrade observed.")
    imgui.text_colored(0.7, 0.7, 0.7, 1,
        "The bank itself is not readable through the plugin API; this")
    imgui.text_colored(0.7, 0.7, 0.7, 1,
        "records everything that passed through your bag or gear.")
    imgui.separator()

    local list = {}
    for k in pairs(vault) do list[#list + 1] = k end
    if #list == 0 then
        imgui.text("nothing recorded yet - open your bag once in game")
        return
    end
    table.sort(list)
    imgui.text(string.format("%d item(s)", #list))
    imgui.separator()
    for _, k in ipairs(list) do
        local l, u, ch = string.match(vault[k] or "", "^(%-?%d+)|(%-?%d+)|(.*)$")
        imgui.text(string.format("%-30s lvl %s +%s  (%s)",
            string.sub(k, 1, 30), l or "?", u or "?", ch or "?"))
    end
end

local function draw_codex()
    imgui.text("Bestiary progress, recorded as you target monsters.")
    imgui.separator()

    local names = {}
    for k in pairs(codex_seen) do names[#names + 1] = k end
    if #names == 0 then
        imgui.text("no codex entries yet - target a few monsters")
        return
    end
    table.sort(names)

    local complete = 0
    for _, k in ipairs(names) do
        local st, pr, mx, nm = string.match(codex_seen[k], "^([^|]*)|([^|]*)|([^|]*)|(.*)$")
        if st == "complete" then complete = complete + 1 end
    end
    bar("Codex ", complete, #names)
    imgui.separator()

    for _, k in ipairs(names) do
        local st, pr, mx, nm = string.match(codex_seen[k], "^([^|]*)|([^|]*)|([^|]*)|(.*)$")
        local r, g, b = 1, 0.8, 0.3
        if st == "complete" then r, g, b = 0.4, 1, 0.4
        elseif st == "unknown" then r, g, b = 0.6, 0.6, 0.6 end
        imgui.text_colored(r, g, b, 1, string.format("%-20s %s/%s",
            string.sub(nm ~= "" and nm or k, 1, 20), pr or "?", mx or "?"))
    end
end

local function draw_settings()
    local v, c

    v, c = imgui.checkbox("Auto-mark collected when I walk over one", cfg.auto_collect)
    if c then set_cfg("auto_collect", v) end
    imgui.text_colored(0.7, 0.7, 0.7, 1,
        "  Heuristic: proximity only. Running past an unopened chest marks it.")

    v, c = imgui.drag_float("Auto-mark range (m)", cfg.auto_collect_range, 0.5, 2, 30)
    if c then set_cfg("auto_collect_range", v) end

    v, c = imgui.checkbox("Toast when an uncollected one is nearby", cfg.proximity_alert)
    if c then set_cfg("proximity_alert", v) end
    v, c = imgui.drag_float("Alert range (m)", cfg.proximity_range, 1, 10, 200)
    if c then set_cfg("proximity_range", v) end

    imgui.separator()
    v, c = imgui.checkbox("Include plants/ore in the '(all one-shot)' category",
                          cfg.show_respawn)
    if c then set_cfg("show_respawn", v) end

    v, c = imgui.drag_float("Area grid size (m)", cfg.area_cell, 10, 100, 2000)
    if c then set_cfg("area_cell", v); rebuild_areas() end

    imgui.separator()
    imgui.text("Data")
    if imgui.button("Export JSON") then export_json() end
    imgui.same_line()
    if imgui.button("Save now") then flush(true); farever.toast("Saved") end
    imgui.same_line()
    if imgui.button("Rescan POIs") then refresh_pois() end

    imgui.spacing()
    if imgui.button("Reset collected set for this character") then
        done = {}
        rebuild_counts(); rebuild_areas()
        mark_dirty(); flush(true)
        farever.toast("Collected set cleared for " .. profile, 4.0)
    end

    imgui.separator()
    imgui.text_colored(0.6, 0.6, 0.6, 1, "collection_atlas v" .. VERSION)
    imgui.text_colored(0.6, 0.6, 0.6, 1, status_line)
end

-- ---------------------------------------------------------------------------
-- Lifecycle
-- ---------------------------------------------------------------------------

function on_init()
    load_cfg()
    local n
    acct.mount, n      = set_decode(farever.store.get("acct_mounts", ""))
    acct_n.mount       = n
    acct.glider, n     = set_decode(farever.store.get("acct_gliders", ""))
    acct_n.glider      = n
    acct.appearance, n = set_decode(farever.store.get("acct_appearances", ""))
    acct_n.appearance  = n
    acct.companion, n  = set_decode(farever.store.get("acct_companions", ""))
    acct_n.companion   = n
    vault = map_decode(farever.store.get("acct_vault", ""))
    profile = ""
    bind_profile()
    refresh_pois()
    last_poll  = farever.now()
    last_flush = farever.now()
    farever.log.info("collection_atlas v" .. VERSION .. " ready")
end

function on_event(name, data)
    if name == "hero_locked" then
        bind_profile()
        refresh_pois()
    elseif name == "target_changed" then
        if data and data.kind and data.kind ~= "" then note_codex(data.kind) end
    end
end

function on_render()
    local now = farever.now()

    if (now - last_poll) >= POLL_INTERVAL then
        last_poll = now
        bind_profile()
        if not pois_loaded then refresh_pois() end
        collections_tick()
        proximity_tick()
    end
    flush(false)

    local v, c = imgui.combo("##tab", tab, TABS)
    if c then tab = v end
    imgui.separator()

    if     tab == 1 then draw_dashboard()
    elseif tab == 2 then draw_nearby()
    elseif tab == 3 then draw_areas()
    elseif tab == 4 then draw_route()
    elseif tab == 5 then draw_collections()
    elseif tab == 6 then draw_vault()
    elseif tab == 7 then draw_codex()
    else                 draw_settings() end
end
