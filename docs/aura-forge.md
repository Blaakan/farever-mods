# AuraForge

A WeakAuras-style HUD for Farever: buff bars with icons, stacks and countdowns,
cooldown bars, and rule-driven alerts.

Install: copy [`plugins/aura_forge.lua`](../plugins/aura_forge.lua) into
`<Farever>\data\plugins\`.

## How it presents (v2)

The plugin window **is** the HUD. Drag it where you want it — the host mod
saves window positions — and lock the overlay with the padlock when you're
done. Content, top to bottom:

1. **Alerts** — your rule-driven auras, as large pulsing text (plus a progress
   bar when the trigger has a duration, e.g. an enemy cast).
2. **Buffs** — one row per active status: icon (when resolvable), name, stack
   count, and a draining bar with the remaining time. Expiring buffs sort
   first; **permanent buffs render as full bars with their stacks** and sit
   below the timed ones.
3. **Cooldowns** — one row per tracked skill on cooldown, filling back up as
   it recovers. Optionally keeps ready skills visible.

Tick the small **settings** checkbox to flip the window into config mode
(Status diagnostics, Buffs, Cooldowns, Auras, Share tabs). Untick to play.

### Buff semantics, from the live game

Read out of a running session's log rather than assumed:

- **Permanent auras** (passives, stack accumulators) report `duration` **0.00
  or −1.00** through the API. AuraForge gives them a `permanent` mode: always
  full, no timer, stacks displayed. A "stack-only" aura is just a permanent
  aura whose stack count moves.
- **Timed auras** report a positive duration; whether that number is the
  total length or the remaining time is unspecified, so it's detected at
  runtime by watching whether it decays. The Status tab shows the detected
  mode per buff (`[permanent]`, `[remaining]`, `[total]`).
- **Icons**: a status kind (`Staff_Censer_Passive_Buff`) rarely has its own
  icon, but its parent skill usually does — icons resolve through a fallback
  chain (strip `_Status`/`_Buff`, then `_Passive`/`_Accum`) and are cached.

## Auras (the alert rules)

Each aura has a **trigger**:

| Trigger | Fires on |
|---|---|
| Status (buff) | a matching buff being active; shows its countdown |
| Skill cooldown | a matching skill being on cooldown; shows remaining |
| Resource | health %, shield, energy, rage, spark, focus, combo, poise, oxygen vs a threshold |
| In combat | the game's combat flag |
| Target casting | your target casting a matching skill; shows the cast bar |
| Target HP | target health % vs a threshold |

Plus: **Invert** ("show when NOT matched" — missing-buff warnings), colour,
pulse, **actions** (sound/toast, fired once per activation), and **load
conditions** (class / min level / in-combat only).

Matching is by comma-separated substrings against internal ids
(`shield,barrier`). The Status tab lists live ids so you know what to type.

Two starters ship: **LOW HEALTH** (under 35%) and **TARGET CASTING**.

## Cooldown tracking — the honest limits

`skills()` reports each skill's cooldown **duration**, never the remaining
time, so remaining is derived: a cooldown starts when the skill is observed
being used via the `damage_dealt` / `heal_dealt` / `shield_applied` events.

Consequences: a skill that neither damages, heals nor shields cannot be
tracked; no skill is known until used once in the session; multi-hit skills
don't restart their own cooldown (a cooldown only starts when the skill was
off cooldown).

## Share

Settings → Share: export the whole config as JSON to a text box or to
`%LOCALAPPDATA%\farever-minimap\combatlogs\aura-forge-config.json`; paste and
import someone else's.

## v1 → v2

v1 painted a free-floating HUD with the absolute draw primitives, positioned
by nine screen anchors plus a resolution setting. In the real game those
draws clip to the plugin's window, so the HUD was invisible. v2 renders
everything in-window (the pattern the host's example plugins use), which also
deletes the resolution/anchor configuration — position is now just "drag the
window". Permanent buffs also rendered as expired in v1; v2 fixes that with
the `permanent` mode above.
