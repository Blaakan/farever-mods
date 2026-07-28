// ---------------------------------------------------------------------------
// navigator.h - direction and distance to a tracked world position.
//
// Its own module, deliberately separate from the Collection Atlas: any mod on
// this host can ask it to track a target, and it draws its own small HUD pill
// (item name, distance, compass direction) whether or not the atlas window is
// open. It never touches the game's own map - the host is read-only - so this
// is an overlay readout, not an in-game marker. Full map pins remain the
// domain of the farever-minimap waypoint API, which collection_atlas.lua
// already drives for people running that mod.
//
// Coordinates use the game's world axes as found in the POI tables; compass
// labels assume +y = north, +x = east.
// ---------------------------------------------------------------------------
#pragma once

namespace fmk {

struct NavTarget {
    char label[96];
    double x, y, z;
};

// Worker / pose thread.
void nav_init();                                  // load persisted target
void nav_tick();                                  // persist when dirty
// rot_z is the hero's facing (ent.GameObject.rotationZ); the pill's arrow
// falls back to it when no camera is available. Stamped at ~20Hz by the
// pose thread.
void nav_set_hero_pose(bool valid, double x, double y, double z, double rot_z);

// Where the render camera sits and what it looks at. The navigator prefers
// this: the screen's forward direction is target minus pos, which is the
// definition of where the view points - no angle convention to guess. Only
// the horizontal part is used, so looking down at a steep pitch still gives
// a stable bearing.
void nav_set_camera(bool valid, double px, double py, double pz,
                    double tx, double ty, double tz);

// Any thread with a UI (render thread in practice).
// `key` identifies the tracked thing (e.g. "mounts/Mount_Wolf_05") so a
// second track request for the same key toggles tracking off.
// Returns true when now tracking, false when toggled off.
bool nav_track(const char* key, const char* name,
               const NavTarget* targets, int count);
void nav_untrack();
bool nav_is_tracked(const char* key);

// Render thread: distance/direction to the nearest target of the tracked
// item, or of an arbitrary target list (for tooltips). Returns false when no
// fresh hero position exists. `out` receives e.g. "1.24km NE".
bool nav_format_distance(const NavTarget* targets, int count,
                         char* out, int out_len);

// Render thread: the waypoint frame. Draw before the atlas window so the
// window stacks above it.
void nav_draw(float screen_w, float screen_h);


}  // namespace fmk
