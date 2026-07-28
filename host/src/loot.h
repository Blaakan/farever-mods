// ---------------------------------------------------------------------------
// loot.h - Recent Loots: what you just picked up, for longer than a toast.
//
// The game shows a loot line for a second or two. In a fight, or while a
// chest chain-opens, that is not long enough to read - and once it is gone
// there is no way back to it. This keeps a feed on screen: items with their
// own icon and rarity, experience, and currency, newest at the top, each
// line fading out on its own timer.
//
// **How it knows.** There is no loot event to hook, because the host never
// calls into the game - it only reads. So this is a diff: a small slice of
// state (bags, purse, experience) is sampled twice a second and compared with
// the previous sample, and anything that went up is something you gained.
//
// That has one honest consequence worth stating: the feed reports *gains to
// your bags*, not literally "loot". Withdrawing from the bank, buying from a
// merchant and receiving a trade all read the same as picking something up,
// because to a reader of memory they are the same thing. Losses are never
// reported, so selling and depositing pass in silence.
// ---------------------------------------------------------------------------
#pragma once

namespace fmk {

// Worker thread. Only reads the saved layout, so it does not have to wait on
// the atlas - an item looted before the atlas has finished loading is shown
// under its raw id rather than not at all.
void loot_init();

// Loot thread, about twice a second. Reads the live state, diffs it against
// the previous reading, and turns what grew into feed lines. `in_world` false
// (main menu, logout, character select) drops the baseline, so the first
// reading after logging back in never arrives as a wall of loot.
void loot_poll(bool in_world);

// Worker thread, about once a second: persists layout when it changed.
void loot_tick();

// Render thread: the feed. Drawn under the atlas window so the window can
// cover it, like the navigator's pill.
void loot_draw(float screen_w, float screen_h);

}  // namespace fmk
