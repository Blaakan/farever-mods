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
//
// One tracked thing is always a *list* of waypoints. What differs is what the
// list means, which is what NavMode says - see below. Tracking an atlas item
// and following a 60-chest farming route are the same machinery with a
// different reading of the same list.
// ---------------------------------------------------------------------------
#pragma once

#include <vector>

namespace fmk {

struct NavTarget {
    char label[96];
    double x, y, z;
};

// What a list of waypoints means.
//
//   kNavNearest  Alternatives. "This mount is sold by three vendors" - you
//                want whichever is closest, and standing on one changes
//                nothing, because the others are the same thing.
//   kNavRoute    A collection. "Every chest in Primevalley" - order does not
//                matter, so always aim at the closest one still outstanding,
//                and cross each off as you reach it.
//   kNavOrder    An itinerary. Someone chose this order for a reason (a
//                gathering circuit, a jumping puzzle), so follow it, and
//                cross each off as you reach it.
//
// Only the last two consume; nav_track uses the first, which is why clicking
// an atlas item and walking to the vendor does not silently stop tracking.
enum NavMode : int {
    kNavNearest = 0,
    kNavRoute = 1,
    kNavOrder = 2,
};

// How close counts as "reached", in world units (roughly metres). Horizontal
// distance decides, but a vertical gate keeps a cave chest from being crossed
// off while you run over the hill above it.
constexpr double kArriveRadius = 15.0;
constexpr double kArriveHeight = 25.0;

// A waypoint dropped where you are standing is inside its own arrival radius
// the instant it exists, so it would be crossed off before you let go of the
// key - which makes recording a route by walking it erase itself as you go.
// Such a waypoint is created *unarmed*: it cannot be reached until you have
// once been this far from it. Waypoints from a saved route are armed from the
// start, because standing on the first chest of a chest run really does mean
// you have done that one.
constexpr double kArmDistance = 40.0;

// Worker / pose thread.
void nav_init();                                  // load persisted route
void nav_tick();                                  // persist when dirty
// rot_z is the hero's facing (ent.GameObject.rotationZ); the pill's arrow
// falls back to it when no camera is available. Stamped at ~20Hz by the
// pose thread. This is also where arrival is noticed: it is the only place
// that learns the hero moved.
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

// Same, but the list is a route: waypoints are crossed off as they are
// reached and the pill moves on to the next. Re-starting the same key
// restarts the route rather than toggling it off - a half-finished route is
// worth restarting far more often than it is worth silently discarding.
bool nav_start_route(const char* key, const char* name,
                     const NavTarget* targets, int count, NavMode mode);

// Appends one waypoint to the ad-hoc route, creating it if nothing is
// tracked or if what is tracked is an atlas item rather than a route. This
// is what a map click, or a typed coordinate, ends up calling.
void nav_queue(const char* label, double x, double y, double z);

// Adds an atlas entry to whatever is being followed - a route grows, a
// single tracked item is replaced by the ad-hoc list.
//
// Several targets on one entry are *alternatives* ("three vendors sell this
// mount"), so only the nearest of them belongs in a route: adding all three
// would send you round every vendor for one item.
//
// `front` puts it at the head of the list rather than the tail. That decides
// where the arrow goes next only in kNavOrder - the nearest-first modes pick
// by distance, and no list order will change that. The Routes page can
// switch between the two.
void nav_add(const char* label, const NavTarget* targets, int count,
             bool front);

// Switch what the active list means. Turning a nearest-first list into an
// ordered one is how a queue built by hand gets followed in the order it was
// built.
void nav_set_mode(NavMode mode);

void nav_untrack();
bool nav_is_tracked(const char* key);

// Where the hero is standing, if that is known recently enough to act on.
// Recording a route is "walk there, press the key", so whatever drops a
// waypoint needs this - and the navigator already samples it at 20Hz, so
// nothing else has to walk game memory for it.
bool nav_hero_pos(double* x, double* y, double* z);

// Cross off the current waypoint without going there, and put every crossed
// waypoint back. Both are no-ops on a kNavNearest track.
void nav_skip();
void nav_restart();

// What the pill is showing, for any UI that wants to say so. `total` counts
// every waypoint the route started with, `done` how many are crossed off.
struct NavStatus {
    bool active = false;
    bool is_route = false;         // false for a plain nav_track item
    int  mode = kNavNearest;
    int  done = 0, total = 0;
    char key[192] = {0};
    char name[96] = {0};
    char label[96] = {0};          // the waypoint being aimed at
    char where[64] = {0};          // "1.24km NE", empty when no live hero
};
void nav_status(NavStatus* out);

// Every waypoint of whatever is being followed, crossed off or not. This is
// how a route recorded by dropping waypoints gets saved under a name - the
// navigator is the only place that list exists until then.
void nav_active_points(std::vector<NavTarget>* out);

// Render thread: distance/direction to the nearest target of the tracked
// item, or of an arbitrary target list (for tooltips). Returns false when no
// fresh hero position exists. `out` receives e.g. "1.24km NE".
bool nav_format_distance(const NavTarget* targets, int count,
                         char* out, int out_len);

// Render thread: the waypoint frame. Draw before the atlas window so the
// window stacks above it.
void nav_draw(float screen_w, float screen_h);


}  // namespace fmk
