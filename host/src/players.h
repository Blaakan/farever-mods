// ---------------------------------------------------------------------------
// players.h - the roster this client has been sent, listed in full.
//
// The game's own Manage Party window (ui.win.GroupWindow.init, findex 20537,
// src/ui/win/GroupWindow.hx:58-63) walks `myPlayer.layer.players`, splits that
// list on a squared distance against Const.UI.GroupWindow_NearDist, and draws
// the near bucket as player cards. The far bucket is built too, and then drawn
// only when Config.prefs.admin is set, under a header reading
// "(ADMIN) Other loaded players (". The constant is 100, and its own CastleDB
// description says exactly what it does: "Other players within this distance
// are shown in the Manage Party window".
//
// So the whole roster is already in client memory and the distance limit is
// presentation. This page lists all of it. That is the entire feature: there
// is no invite button and no control here that calls into the game, because
// the host only reads. Whether to add one is the user's decision and it has
// not been taken.
//
// The DM button is the shape every action on this page has to take. It copies
// `!to <name> ` - the game's own whisper command, ChatBox.hx:132-171 - and
// stops. Typing it into the chat box, or moving the channel dropdown, would
// both be writes, and input synthesis is banned outright; so the paste is the
// player's and the send is the game's.
//
// Clicking a row follows that player: the navigator's pill is re-pointed at
// their current position on every poll, so the arrow tracks them as they move.
// It is the same read the distance column already does, published to a module
// that draws it bigger; nothing is written and nothing is asked of the game.
// Following stops when the row is clicked again, and it also stops on its own
// when there is no longer a position to point at - see players.cpp for what is
// said then, which is never "they left" and never "they are far away", because
// this client cannot know either.
//
// Three things this page deliberately does NOT claim, each of them stated on
// the page itself, because each is easy to misread as an answer:
//
//   * The roster is what the SERVER chose to replicate to this client. It is
//     not provably everyone on the shard, and it is not worded as though it
//     were.
//   * st.Player.hero is null for a player whose character has not been
//     replicated here, and a player with no hero has no position at all. That
//     is NOT the same as being far away, so those rows show "-" rather than a
//     number, and never the word "far".
//   * st.Player.group is network property bit 12 and sits in the conditional
//     visibility mask, so it reads null for everyone except the local player.
//     Your own party is therefore readable and nobody else's is - which is why
//     there is no "already in a party" or "invitable" column. There is nothing
//     truthful to put in one, and a guess dressed up as a column would be read
//     as knowledge.
// ---------------------------------------------------------------------------
#pragma once

#include "input.h"

namespace fmk {

// Worker thread, at startup beside the other modules' inits. There is nothing
// to load - the roster only ever exists in the running game - so this brings
// up the critical section the poll and the draw share, and nothing else. It
// must run before either of those can be reached.
void players_init();

// Worker thread, on the once-a-second tick. Reads the roster and re-sorts it;
// its own throttle decides whether enough time has passed.
//
// This is deliberately NOT driven from the draw side. An earlier version had
// players_count() and players_draw() poll before using anything, which put a
// walk of the whole roster - a few hundred validated reads and a string per
// player - on the render thread. host_draw in dllmain.cpp states the rule it
// broke: the draw callback never walks game memory. Every other module here
// polls on a worker and publishes a snapshot, and so does this one.
void players_poll();

// Render thread: the Players page. Draws inside the content band the atlas
// window hands it and owns everything in there, including its own scrolling.
void players_draw(const InputState& in, bool clicked, float x, float y,
                  float w, float h);

// How many players the last read saw, for the tab label. Zero also means "not
// read yet" - the page itself says which.
int players_count();

}  // namespace fmk
