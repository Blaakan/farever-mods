# AuraForge

A WeakAuras-style HUD for Farever: movable buff bars, cooldown bars, and
rule-driven alerts.

Install: copy [`plugins/aura_forge.lua`](../plugins/aura_forge.lua) into
`<Farever>\data\plugins\`.

## Set this first

Go to the **Layout** tab and pick your resolution.

The plugin API has no way to query screen size, so anchors are computed from a
value you supply. Get it right once and every element lands where you put it,
at any resolution.

Then tick **Unlocked**: every HUD element gets a green outline and a label, even
when inactive, so you can see what you are positioning. Untick it to play.

## What you get

### Buff bar
Auto-arranging list of your active statuses, shortest remaining first. Each bar
drains as its buff expires, shows the remaining time, turns orange under 3
seconds, and pulses under 2. Stack counts appear in the corner.

Configurable: anchor, X/Y offset, grow direction (right/left/down/up), bar size,
spacing, max shown, and include/exclude filters.

### Cooldown bar
Same layout engine, for skill cooldowns. Bars fill as the cooldown recovers.
Optionally keeps ready skills on screen in green instead of hiding them.

### Auras
Individually placed alerts, each driven by a trigger:

| Trigger | Fires on |
|---|---|
| **Status (buff)** | a matching buff being active; shows its countdown |
| **Skill cooldown** | a matching skill being on cooldown; shows remaining |
| **Resource** | health %, shield, energy, rage, spark, focus, combo points, poise, oxygen vs a threshold |
| **In combat** | the game's own combat flag |
| **Target casting** | your target casting a matching skill; shows the cast bar |
| **Target HP** | target health % vs a threshold |

Each aura also has:

- **Invert** — fire when *not* matched. This is how you build "you are missing
  your buff" warnings, which is most of what WeakAuras gets used for.
- **Display** — Bar, Tile (compact, centred countdown), or Text.
- **Position** — one of nine screen anchors plus an X/Y offset.
- **Colour**, and an optional pulse while active.
- **Actions** — play a sound and/or pop a toast when it turns on. Fires once per
  activation, not once per frame.
- **Load conditions** — restrict to a class, a minimum level, or in-combat only.
  An aura that fails its load conditions is not evaluated at all.

Matching is by substring against the internal id, comma-separated for
alternatives — `shield,barrier` matches either. Leave it empty to match
everything of that type. The **Status** tab lists the live ids so you can see
what to type.

Two starter auras ship on first run: a red **LOW HEALTH** alert under 35%, and a
**TARGET CASTING** bar.

### Status tab
Live diagnostics: every active status with its resolved countdown and detected
duration mode, and every skill the mod has seen with its cooldown state. This is
the tab that tells you what strings to match on.

It is also the one place the game's **real skill icons** are drawn — see below.

### Share
Export your whole setup as JSON into a text box, or paste someone else's in and
import it. **Write to file** drops it in
`%LOCALAPPDATA%\farever-minimap\combatlogs\aura-forge-config.json`.

## Design notes — why it works this way

Three API constraints shaped the whole plugin. They are worth understanding,
because they explain the parts that look unusual.

### 1. "Movable" means anchor + offset, not drag

There is no mouse API — no cursor position, no click or drag state. So elements
cannot be dragged with the mouse.

Instead: nine screen anchors plus an X/Y offset, adjusted with drag-sliders in
the configurator, with **Unlocked** mode outlining everything so you can see
what you are doing. Anchoring to a corner also means elements stay put if you
change resolution, which dragging would not give you.

### 2. The HUD is shapes and text; icons live in the configurator

`imgui.icon()` is a *flow* widget — it draws at the ImGui cursor inside the
plugin's own window. The absolute `draw_*` primitives, which are what let you
paint anywhere on screen, have no image variant.

So the plugin splits the way WeakAuras splits display from options:

- **HUD layer** — absolute primitives, positioned anywhere, freely movable.
  Bars, borders, countdown text, stack counters, pulses. No icons.
- **Configurator** — this window. Lists, editors, live previews, and the game's
  real skill icons, which can only be drawn here.

### 3. Cooldown *remaining* has to be derived

`farever.player.skills()` reports each skill's cooldown **duration**, not how
much of it is left. Nothing reports remaining time.

So AuraForge tracks it: when a skill is observed being used, its cooldown starts
locally, and remaining is `cooldown - (now - used)`. Uses are observed from the
`damage_dealt`, `heal_dealt` and `shield_applied` events.

**The consequence:** a skill that neither damages, heals, nor shields is never
observed, so it cannot be tracked. Pure mobility and utility skills are the gap.
Nothing in the current API closes it.

A skill also only appears at all once the mod has resolved it, which happens
during combat — an empty cooldown list right after login is normal.

Multi-hit skills and damage-over-time ticks fire several `damage_dealt` events
for one cast. A cooldown therefore only (re)starts when the skill is already
off cooldown, so extra hits do not keep resetting it.

### Buff duration semantics are detected at runtime

`farever.player.statuses()` returns a `duration` field, and the API docs do not
specify whether it is the buff's total length or its remaining time — they only
say plugins should compute countdowns themselves.

Rather than guess, AuraForge watches the value:

- if it **decays** between samples → it is *remaining*, use it directly;
- if it **holds steady** for over 1.5s → it is *total*, count down from first
  sight;
- a jump upward means the buff was refreshed, so the countdown restarts.

The detected mode is shown in brackets on the Status tab (`[remaining]`,
`[total]`, `[unknown]`). Both paths are covered by the test harness.

## Limitations

- **No mouse dragging**, as above.
- **Screen size is a setting**, not detected.
- **No icons on the HUD layer** — shapes and text only.
- **Utility/mobility skills cannot have cooldowns tracked**, and no skill is
  known until it has been used once in the session.
- **Your own casts of non-damaging buffs** are only seen through
  `shield_applied`, so buff-type cooldowns are partially covered.
- **Only your own statuses.** The API exposes no party or target aura list.
- **Absolute draws may clip.** The drawing primitives are documented as
  absolute screen-space, and the host mod advertises them for exactly this kind
  of custom HUD. If elements near the screen edge do not appear, they are being
  clipped to the plugin window's draw region — move or enlarge the AuraForge
  window, or bring the offsets inward.

## Performance

State polling runs at 4 Hz; the HUD redraws every frame so countdowns stay
smooth. Config saves are batched to at most one write every 2 seconds, and only
when something actually changed.
