// ---------------------------------------------------------------------------
// mapwatch.h - waypoints from the game's own map.
//
// Open the map, click a point of interest, and it becomes a navigator
// waypoint. The host never touches the map to do it: `ui.win.MapWindow` runs
// the hit test itself and leaves the result in `nearClickableMarker`, and
// every marker on that map carries a world-space `worldPos`. So this is two
// reads and no projection - the map's zoom and panning never come into it,
// and neither does a screen-to-world guess.
//
// The click is not taken either. `input.cpp` counts left-clicks that fall
// outside every host rectangle and passes them straight through, so the map
// still does whatever it was going to do; this just notices.
//
// Which clicks count is `[map] click` in farever-modkit.ini:
//
//   0   off
//   1   any clean left-click on a point of interest  (default)
//   2   shift + left-click only
//
// "Clean" means pressed and released within half a second and six pixels -
// the game's map pans with the same button, and the start of a pan is not a
// click.
//
// F9 keeps its meaning of "drop a waypoint", and reads it off the map while
// the map is open: whatever is hovered, or failing that the spot under the
// cursor. With the map closed it is still the ground you are standing on.
// ---------------------------------------------------------------------------
#pragma once

namespace fmk {

// Worker thread: reads the setting.
void mapwatch_init();

// Pose thread, ~20Hz. Owns the F9 key, because what F9 means depends on
// whether the map is open.
void mapwatch_poll(bool in_world);

}  // namespace fmk
