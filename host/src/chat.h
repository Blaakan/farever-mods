// ---------------------------------------------------------------------------
// chat.h - a chat window over the game's own, and a command line that costs
// the server nothing.
//
// The game's chat box is small, keeps no scrollback worth the name, has no
// ignore list anywhere in the build, and shows no timestamps - although it
// records one. This draws its own window over the message area, reading from
// `st.player.ChatClient.history`, which is the whole session in arrival order
// and never trimmed.
//
// **The overlay covers the messages, not the input.** `ui.hud.ChatBox` is an
// h2d.Object, so where it sits is readable, and the `messages` flow inside it
// has its own bounds. The window aligns to those and leaves the footer and
// the text field below it visible and clickable. Pressing Enter still opens
// the game's own input, typing still goes to the game, and sending a message
// is still the game doing it - the host never writes.
//
// **Commands.** `ui.hud.ChatBox.processMessage` (ChatBox.hx:132-171) trims
// what you typed, prefixes it with `"!" + <selected channel> + " "` when it
// does not already start with `!`, splits on spaces and switches on the first
// token. Four match - `!say`, `!map`, `!group`, `!to` - and the default case
// at ChatBox.hx:165 is
//
//     chatError("Unknown chat command " + cmd);
//     return;
//
// That `return` is the whole mechanism: `sendMessage` is only reached at
// ChatBox.hx:169, after the switch matched. **An unrecognised `!command`
// never reaches the network.** It draws one local line and stops. So this
// module can own a command language typed into the game's own box without a
// single write and without a packet.
//
// The prefix is `!!`. Any unmatched `!x` would work, but `!!` is much harder
// to typo into a broadcast: forget the `!` entirely and the text goes out on
// whatever channel the dropdown has selected, which is the one real footgun
// here.
//
// Reading the command back is one signal: the poller keeps the last non-empty
// chat input it sampled, and runs it when the box goes empty, gated only on
// the `!!` prefix.
//
// It used to want a second signal as well - the game's own
// `"Unknown chat command " + <first token>` echo appearing as a new bare
// `ui.hud.ChatBoxLine` - to tell Enter from Escape. That is gone, for two
// reasons. It never once fired in a live session, and it is not needed: since
// every `!!` token is unmatched by `processMessage` and swallowed locally at
// ChatBox.hx:165, the worst an over-eager trigger can do is run a local
// read-only command somebody meant to abandon.
//
// What that costs is documented where it happens, in `poll_chatbox`: the
// buffered text is the last sample before Enter, so a command typed and sent
// inside one poll interval is seen half-typed and RUNS with a truncated
// argument. `run_command` names the string it acted on whenever the text had
// not stood still for a whole interval first.
//
// Files next to the game:
//
//   farever-modkit.ini          [chat] - placement, size, filters, options
//   farever-chat-ignore.txt     one name per line; `#` comments
//   farever-chat-log.txt        the session log, when [chat] log = 1
// ---------------------------------------------------------------------------
#pragma once

namespace fmk {

// Worker thread, after the build check. Reads the settings, loads the ignore
// list, opens the log. Deliberately independent of the atlas: chat is worth
// having on a DLL-only install with no generated item database, so the only
// thing a missing atlas costs is icons on item links.
void chat_init();

// Chat thread, about ten times a second. Reads the history length, decodes
// only what is new, and applies the ignore list and channel filters. Also
// samples the game's chat input for the command surface, which is why this
// wants a faster cadence than the loot feed - a command should run when you
// press Enter, not half a second later.
//
// `in_world` false (main menu, character select, logout, and every loading
// screen) drops only what cannot outlive the world: the bounds of the game's
// chat box, and a half-typed command whose fate can no longer be learned. The
// decoded lines and the index into the history are KEPT. Dropping the index
// here re-decoded the whole session on the way back from any transient,
// appending a second copy of it to the log; a genuinely new ChatClient is
// recognised instead by its history being shorter than the index into the old
// one, which is a question the history answers about itself.
void chat_poll(bool in_world);

// Worker thread, about once a second: persists placement and settings when
// they changed, and flushes the ignore list.
void chat_tick();

// Render thread. Drawn after the loot feed and before the atlas window, so
// the atlas stacks above it the way it does over the navigator's pill.
void chat_draw(float screen_w, float screen_h);

}  // namespace fmk
