# Chat

A chat window over the game's own, with the session's whole scrollback,
timestamps, an ignore list, per-channel filters, item links, and a command
language typed into the game's own chat box.

Part of the standalone host ([`host/`](../host/README.md#chat)) — it needs
nothing installed but the game and one `dxgi.dll`. It does not need the atlas
database; the only thing a DLL-only install costs is the icons on item links.

> **What has been seen working, and what has not.** This has been run in three
> live sessions. Observed on screen: the window drawing over the game's message
> area and aligned to it, the session's scrollback read out of
> `ChatClient.history`, the anonymous-structure decode, the channel
> classification, whisper targets resolved to a name (*To Emsey: test
> whisper*), the timestamps the game records and never shows, and the channel
> filter chips.
>
> **The `!!` command surface fires.** This page used to say it was the one part
> still unconfirmed; it no longer is. `!!help` has been seen printing its list
> into the window, `!!ignore` and `!!unignore` seen hiding a player's lines and
> bringing them back with their backlog, and `!!link Credence` seen putting a
> link on the clipboard — after a fix, because the lookup searched the atlas by
> id and *Credence* is `Bow_Craft`. See [Item links](#item-links).
>
> **Built and compiling, but not yet seen running:** `!!size`, `!!opacity`, and
> the `Ignored N` chip with its click-to-remove panel. Read those parts of this
> page as a description of what the code does.
>
> Two things failed in the first run and are fixed: the chat box was looked for
> in `ui.BaseUI.elements`, where it does not live — the game's own route is
> `gui.gameRoot.hud.chat` (`ui.GameUI.get_hud`, GameUI.hx:33) — and the message
> area was sized by guesswork rather than by reading `h2d.Flow`'s
> `calculatedWidth`/`calculatedHeight`, which are generated now. The window is
> also opaque by default now, which is a correctness matter rather than a look:
> see [The window](#the-window).

## What it adds over the game's chat box

- **The whole session.** `st.player.ChatClient.history` is appended to and
  never trimmed, so nothing said since you logged in has scrolled away.
- **Timestamps**, which the game records on every message and never shows.
- **An ignore list**, which the game does not have at all — there is no
  ignore, mute or block function anywhere in the client.
- **Per-channel filters** — six chips, one per `st.Channel` constructor.
- **Item links** — `[Copper Ore]` in anyone's message draws with that item's
  own icon and rarity colour.
- **An optional session log**, on disk, tailable while you play.

## Commands

Typed into **the game's own chat box**, not into a field of ours. They are
never sent — see [How the commands work](#how-the-commands-work).

| Command | What it does |
|---|---|
| `!!help` | Lists the commands, in the window |
| `!!ignore <name>` | Hides that character here and in the log |
| `!!unignore <name>` | Undoes that, backlog included |
| `!!ignores` | Lists who is ignored |
| `!!find <text>` | Searches this session's chat, sender and message body |
| `!!clear` | Empties the window; the history is untouched |
| `!!chat` | Turns the window off and on |
| `!!time` | Timestamps on and off |
| `!!align` | Follow the game's chat box, or float free |
| `!!where` | What the navigator is following, and how far through a route. A player followed from the [Players page](players.md#following-a-player) answers here by name |
| `!!link <text>` | Copies `[Item Name]` to the clipboard, ready to paste |
| `!!size <9-28>` | Text size; rows and item icons scale with it |
| `!!opacity <20-100>` | How solid the window is. Default 100 — see below for why |

Both `!!size` and `!!opacity` report the current value when given no argument,
and say what they clamped to when given one out of range rather than silently
doing something else.

Matching is case-insensitive. An unrecognised `!!` command names the closest
one it knows — `!!ignroe` answers *Did you mean !!ignore?* — because a
mistyped command is nearly always one letter away from the one that was
meant.

`!!find` searches sender names and message bodies, skips ignored senders and
the mod's own replies, reports the total number of matches and prints the
newest twelve oldest-first, so the newest match ends up nearest the bottom
where the eye already is.

`!!link` matches the item's **display name**, case-insensitively — the name
you would type, and the one the atlas's own search box matches. An exact name
wins outright; failing that, a name that appears inside exactly one item's
name is taken. A fragment that several items share is deliberately *not*
resolved, because putting one of four swords on your clipboard on your behalf
is worse than saying the name was not specific enough.

It searches the name and not the id for a reason worth stating: an id is not
the name with the spaces taken out. `Credence` is `Bow_Craft`. Only **182 of
the 1639 entries** — one in nine — have an id that can be derived from their
name at all, so an earlier version that transformed the typed name into a
candidate id failed for the other eight. Ids are still accepted, so
`!!link Bow_Craft` works too.

Replies are drawn in the window in blue, tagged `!!`, and are never filtered by
the channel chips. They are **not** written to the log — the log is a record of
what was said in the game.

**The game's own four commands are untouched.** `!say`, `!map`, `!group` and
`!to <name>` are what `processMessage` matches, and a line that already starts
with `!` picks its own channel rather than the dropdown's (ChatBox.hx:133-134)
— so `!to Emsey hello` whispers without the channel selector being clicked.
The [Players page](players.md#the-dm-and-copy-buttons) puts `!to <name> ` on
the clipboard for you, which is as far as a reader can help: the paste is
yours and the send is the game's.

Whispering somebody once also adds them to the game's **own** channel dropdown
for the rest of the session, while being whispered at does not — which is worth
knowing before you type the same `!to` twenty times. The disassembly for that
is on the [Players page](players.md#whispering-somebody-adds-them-to-the-games-own-dropdown).

### How the commands work

`ui.hud.ChatBox.processMessage` (ChatBox.hx:132-171) trims what you typed,
prefixes it with `"!" + <the selected channel> + " "` when it does not already
start with `!`, splits on spaces and switches on the first token. Exactly four
match — `!say`, `!map`, `!group`, `!to` — and the default case at
ChatBox.hx:165 is

```
chatError("Unknown chat command " + cmd);
return;
```

The single call to `ChatClient.sendMessage` is at ChatBox.hx:169, past that
return. So an unrecognised `!command` draws one line in your own client and
**never reaches the network**: no packet, no rate limit, nobody else sees it.
That is the whole reason a host that only reads can have a command language at
all — the game throws the command away by itself, and nothing has to be
cancelled.

The prefix is `!!` rather than `!` alone. Any unmatched `!x` would work
identically; `!!` is chosen because dropping one `!` still leaves an unknown
command, whereas dropping the only `!` broadcasts what you typed on whatever
channel the dropdown has selected. That is the one real footgun here.

Reproduce it yourself rather than taking this page's word for it:

```bash
node tools/dis-hlcode.mjs ChatBox.processMessage
```

**How the host notices one.** It polls `ChatBox.messageInput` ten times a
second, keeps the last non-empty value, and runs it when the field goes empty
— gated on nothing but the `!!` prefix. It is deliberately not gated on the
input holding focus, because that read reports false when it cannot resolve
and would take the whole surface down with it.

An earlier version also waited for the game's own *Unknown chat command* echo
to appear as a new bare `ui.hud.ChatBoxLine`, as a second signal that Enter
rather than Escape had been pressed. That signal is not available — in a live
session the flow's children read back as `ui.UIElement` — so the test never
passed and no command ever ran. It turned out not to be needed: every `!!`
token is unmatched and swallowed locally whatever the host does, so the worst
an over-eager trigger can do is run a local read-only command somebody meant
to abandon. See [Limitations](#limitations) for what that costs.

The full disassembly and what it settles is in
[RESEARCH.md](../RESEARCH.md#can-a-mod-be-driven-by-in-game-commands).

## The window

**Aligned** (the default) puts the window over the game's own message area.
All four edges are the game's own: `ui.BaseElement` extends `h2d.Flow`, and a
Flow records the box its layout settled on in
`calculatedWidth`/`calculatedHeight`, so the `messages` flow gives its left
edge, its top and its size directly. The window covers the messages and leaves
the footer and the text field below them visible and clickable. Enter still
opens the game's input, typing still goes to the game, and sending a message
is still the game doing it.

An earlier build believed a Flow's size could not be generated, derived the
height from the gap down to the `footer` and guessed the width from the saved
one. On screen that was visibly the wrong size. The saved width is still the
fallback if `calculatedWidth` comes back as nothing usable — under 120 pixels,
which is what mid-layout or a flow that has never been laid out reads as — and
when the rectangle cannot be read at all the
window falls back to free placement rather than quietly appearing somewhere
unexpected. With the atlas open it says so on itself; with the atlas closed it
does not, because the whole chip row it belongs to is only drawn then.

**Free** (`!!align`) floats the window wherever you put it. Open the atlas
(**F8**) to drag it by anywhere in its body and resize it from the grip in the
bottom-right corner; both persist. Dragging is tied to the atlas being open —
the rule the waypoint pill and the loot feed already follow — so over the
world the window never eats a click.

Scrolling is the mouse wheel over the window, three lines a detent, claimed
only when the cursor is inside it and the atlas window is not drawn over it —
but **with the atlas shut as well as open**, which dragging is not. Scrollback
that needed a second window open would not be scrollback: reading what somebody
said a minute ago is a thing you do mid-play. Only the wheel is taken; a click
over the frame still reaches the game's own chat box underneath. A
`12 newer` badge in the corner says how much is below the view. Scrolling back
is anchored to the line you are looking at, not to an index, so a message
arriving while you read does not yank the view down; scrolling to the bottom
re-pins it to the newest line.

With the atlas open the window also grows a row of chips along the bottom: one
per channel, then **Time**, **Aligned**, and `Ignored N` — the last of which
opens [the ignore list](#the-ignore-list) above the row. The first three are
the same settings the commands toggle and they persist the same way. A chip
that would not fit the window's width is not drawn, so a narrow window loses
them from the right.

Long messages wrap; a single token wider than the window breaks mid-word, so
one pasted URL cannot push a row off the edge. Messages are truncated at 600
characters and control characters are replaced with spaces on the way in.

The window keeps the last **3000** lines. The game's own history keeps
everything, but a session's chat is thousands of strings and the mod's copy is
bounded on purpose.

**It cannot hide the game's own chat box.** Hiding it would mean writing to
the game, which the host does not do — so the game goes on drawing its own
copy of every line underneath this window.

The window is therefore **opaque by default**, which is what keeps that copy
out of sight. `!!opacity <20-100>` turns it down if you would rather see the
world behind the window; below 100 the game's lines show through and the same
message appears twice, in two sizes, which is a rendering fault rather than a
look. The setting persists as `[chat] opacity`.

The game's box carries its own **×** (`ui.hud.ChatBox`'s `minimizeButton`, an
icon named *Cross*), and it is **not** a way round this: tried in game, the
box does not stay hidden. So the opaque window is the answer, not a workaround
you are expected to apply by hand.

**Could the game's box be hidden outright? Yes, and this deliberately does
not.** `ui.hud.ChatBox` is an `h2d.Object`, so `visible` is one `BOOL` at
`+0x50`; writing `0` to it would hide the game's chat perfectly, with no
opacity trick and no second copy of anything. That is one write, to a UI flag,
and it is still a write — which is the first house rule, the basis of the
safety argument (a reader that walks a stale pointer shows nothing; a writer
that does can corrupt state), and the claim the front page opens with. It is
recorded here as a known option that was weighed and not taken, so that the
next person to want it can find the reasoning instead of the offset.

A second complete replacement is possible without any write at all: cover the
whole box and **mirror the input**, drawing the typed text and caret from
`messageInput.input.text` and `h2d.TextInput.cursorIndex`, both of which are
already read. It is not built, because seeing the game's own input box turns
out to be fine, and mirroring it would buy a cosmetic win in exchange for
typing latency, a covered channel dropdown, and no IME composition.

That applies to the game's own *Unknown chat command* echoes as well: every
`!!` you type leaves one. Aligned, the echo lands under the window and is
dimmed by it; free, the window is wherever you put it and the echo is not
covered at all. Neither is the host removing it — that would be a write.

## Settings

`farever-modkit.ini`, next to `Farever.exe`, section `[chat]`. Written about a
second after anything changes.

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `1` | Draw the window at all (`!!chat`) |
| `align` | `1` | Follow the game's chat box; `0` floats free (`!!align`) |
| `timestamps` | `1` | Show the clock column (`!!time`) |
| `textsize` | `13` | Body text size, 9–28 (`!!size`) |
| `opacity` | `100` | Window opacity, 20–100 (`!!opacity`) |
| `log` | `0` | Write `farever-chat-log.txt` |
| `x`, `y` | unset | Free placement, in pixels |
| `w`, `h` | unset | Free size; `w` is also the aligned width, but only as the fallback when the game's own does not read |
| `show_local` | `1` | Local chat |
| `show_all` | `1` | The All channel |
| `show_allsystem` | `1` | All-system announcements |
| `show_whisper` | `1` | Whispers, both directions |
| `show_group` | `1` | Group chat |
| `show_system` | `1` | System messages |

`log` is read once, at startup, and there is no command for it: set it, then
restart the game.

`textsize` and `opacity` are clamped on the way in as well as where they are
set. They are two of the few keys somebody will reasonably hand-edit, and a
2-pixel font or a window at zero opacity reads as the mod being broken rather
than as the number being out of range.

## The ignore list

Three ways to see it, because a list you cannot look at is a list you stop
trusting:

- **The `Ignored N` chip**, in the chip row along the bottom of the window
  with the atlas (**F8**) open. The count is on the chip itself, so an ignore
  that did not take — a rejected name, a file that would not load — shows
  without opening anything. Click the chip and the list opens above the row;
  **click any name to unignore it**, and their backlog comes straight back,
  because hiding is applied when the window draws rather than when a line
  arrives.
- **`!!ignores`**, which prints the list into the window at any time, atlas or
  no atlas.
- **The file itself**, below.

### The file

`farever-chat-ignore.txt`, next to the game. One character name per line, `#`
starts a comment, blank lines are skipped:

```
# farever-modkit chat ignore list.
# One character name per line; '#' starts a comment. Matching is
# case-insensitive. Edit by hand, or use !!ignore / !!unignore in
# the game's own chat box.

Someone
SomeoneElse
```

Matching is on the **whole name**, case-insensitively — there are no wildcards
and no substrings. Duplicates are dropped, lines over 64 characters are
ignored, and a file over 1 MB is not read at all. `!!ignore` and `!!unignore`
rewrite the file, header and all, which loses any comments you added to it.

`!!ignore` refuses a name that would not survive that round trip, and says
which: nothing at all, longer than 32 characters, starting with `#` (the file
would read it back as a comment and quietly drop it on the next start), or
containing a control character (the file is one name to a line). Refusing and
saying why beats writing a file that reads differently from what was typed.

An ignored sender is hidden in the window **and** kept out of the log: the
point of ignoring someone is that they leave no trace. Hiding happens when the
window draws rather than when a line arrives, so `!!unignore` brings their
backlog back with them.

## The log

`farever-chat-log.txt`, next to the game, appended to rather than truncated —
yesterday's whisper is what a log is for. It opens with a session banner and
flushes after every line, so it can be tailed while the game runs.

```
--- session 2026-08-02 21:14:07 ---
[2026-08-02 21:14:33] [L] Someone: where is the vault
[2026-08-02 21:14:41] [W] Someone: got a spare copper?
[2026-08-02 21:15:02] [S] Server restarting in 10 minutes
```

The tag is the channel: `L` local, `A` all, `A*` all-system, `W` whisper, `G`
group, `S` system, `?` a channel that did not decode.

**The channel chips deliberately do not apply to the log.** They are a chip
you click to quieten the window for a minute, and a log that silently stopped
recording a channel because of that would be worth less than no log — the
record is the thing you cannot get back. The ignore list *does* apply, for the
reason above.

## Item links

Any `[bracketed run]` in a message is looked up in the atlas database. A hit
draws in that item's rarity colour with its icon in front of it; a miss draws
as the plain text everyone else sees. This works on other people's messages
too, since it is a property of the text rather than of who typed it — but only
you see it, and only if you have the atlas database installed. Anyone without
the mod sees `[Copper Ore]`, which is why the convention is worth using at
all.

### What actually resolves

`link_lookup` tries four lookups and stops at the first hit:

1. the **display name**, case-insensitively (`atlas_ui_find_by_name`) — an
   exact name wins outright, and failing that a name that appears inside
   exactly one entry's name is taken. A fragment several items share resolves
   to nothing;
2. the text **exactly as typed**, against the id index — which is how an id is
   matched;
3. the text with every non-alphanumeric character removed —
   `Copper Ore` → `CopperOre`;
4. that same squashed form re-cased as CamelCase — `copper ore` → `CopperOre`.

Steps 2 to 4 came first and were the whole of it. The atlas's id index matches
exactly and the id is not the name with the spaces taken out, so `!!link
Credence` found nothing while the atlas's own search box found it immediately;
the name search is step 1 for that reason, and the id derivations are kept
behind it because an id typed deliberately should still work.

**The ids are mostly not the display names.** An earlier version of this page
said they were the display names with the spaces taken out; check it and they
are not. Of the 1639 entries in a generated `farever-atlas.tsv`, **182 — about
one in nine — have an id that a typed display name derives.** Where it holds
is gathered materials (41 of 93), recipes (60 of 190) and consumables (35 of
87): `Copper Ore` really is `CopperOre`, `Health Potion` really is
`HealthPotion`. Where it does not hold is everything cosmetic and everything
equipped — **not one** of the 428 appearances, 64 mounts, 73 pets, 68 gliders
or 37 weapons is reachable by name, and only 7 of 252 creatures are. Those
carry internal ids: *Abyssal Shoulderplates* is `Shoulders_RManfish_FigAss`.

That is what step 1 is the answer to: the name you would type now matches
whatever the id happens to be, for the whole database rather than one entry in
nine. The id is still the way to be exact when a name is shared, and the place
to read one off is the Atlas window itself — search by name on the relevant
page and the entry's detail panel prints its id, faintly, at the bottom.

Two consequences worth knowing before you rely on this:

- **`!!link` copies `[Display Name]`**, and rendering a link runs the same
  lookup — so a link made by `!!link` resolves for anybody else running this
  mod, whichever way it was found. Linking by id still produces a link whose
  text is the name, so what travels is always readable.
- Matching is case-insensitive on the display name, and falls back to the id.
  A fragment shared by several items resolves to nothing rather than to a
  guess, so a link either names one item or is left as plain text.

`!!link <name>` builds one and copies it to the clipboard, which is the part
that would otherwise be tedious to type correctly.

## What is read, and from where

Every one of these is a validated read through `hl_runtime.h`; nothing is
written, hooked or called.

| What | Path |
|---|---|
| The messages | `ent.Hero.player` → `st.Player.chatClient` → `ChatClient.history` |
| Each message | an anonymous structure: `channel`, `sender`, `text`, `localStamp`, `localTextId` — matched by field **name**, since a structure's field order is not guaranteed |
| Where the box is | `GameApp.gui` → `gameRoot` → `hud` → `chat`, the game's own route (`ui.GameUI.get_hud`, GameUI.hx:33) |
| How big it is | that box's `messages` flow: `h2d.Object.absX`/`absY` and `h2d.Flow.calculatedWidth`/`calculatedHeight`, scaled by `h2d.Scene.width`/`height` against the frame |
| What is being typed | `ChatBox.messageInput` → `InputBox.input` → `h2d.Text.text` |
| Whether it has focus | `h2d.Scene.events.currentFocus` compared against the input's `interactive` — the same test `h2d.Interactive.hasFocus()` makes. Read, but **not** used to gate the command surface: it is the most fragile read on that path and reports false when it cannot resolve, which would take the whole surface with it |
| The developer console | `GameApp.gui.console` → `h2d.Console.bg.visible`, read only so the host can stay out of its way |

The `ui.hud.ChatBox` pointer is cached and re-validated by class name on every
use, keyed on the `ui.GameUI` that owned it — a dead HashLink object goes on
answering to its class name until the collector reuses its block, so without
the key a character select would leave this pointing at the previous session's
box.

## Limitations

- **A sender is named only when it is a player character.** A null sender is a
  system line; any other `ent.Unit` has no name field among the generated
  offsets, only its unit id, so those lines draw without a name rather than
  with a guess.
- **The far end of a whisper is best-effort.** `st.Channel.Player` carries the
  other party, but an enum's parameters live at offsets that come from the
  enum construct table, which the offset generator does not emit. The read is
  validated by class name and left blank when it does not validate. `Group` is
  not attempted at all — `st.Group` has no readable name, only its player
  list, so a group message says *Group* and no more.
- **Lines the client generates for itself show an id.** They carry a
  `localTextId` instead of drawn text, and resolving one needs the language
  table, which this host does not read. The id is shown rather than a sentence
  being invented for it.
- **Timestamps are when the mod first saw the line**, not the game's own
  `localStamp`. For live chat those are the same second; for the backlog
  decoded on the first poll after a load, they are all the moment of that
  poll.
- **A command can run when it was not sent, or with the wrong arguments.** The
  trigger is the chat input going empty, and Escape empties it exactly as
  Enter does — so a `!!` command typed and then abandoned still runs. And what
  the poller buffers is the last sample before that, so a command typed and
  submitted inside one 100 ms poll can be caught mid-word. Matching is on the
  **first token only**, so a truncated sample whose first token is already a
  whole command name runs that command with the arguments it had at the time:
  `!!ignore Emsey` caught as `!!ignore Em` ignores *Em*.

  Nothing available to a reader closes that window. What the module does
  instead is count whether the text stood still for a whole poll interval
  before the box emptied — the difference between *they had stopped typing*
  and *no idea* — and when it did not, it prints the line it is about to act
  on before acting: *Running '…' — that text appeared and was submitted inside
  one poll, so check it is not cut short.* Wrong, but never silently wrong.

  It is a tolerable failure rather than a dangerous one only because every
  command here is local and read-only — the worst of them, `!!ignore`, is
  undone with `!!unignore`. Closing the gap properly would mean writing to the
  game.
- **`!!find` searches the mod's 3000-line copy**, not the game's full history,
  which for a very long session is the older part of it.
- **Nothing is drawn out of world.** At the main menu, character select or a
  loading screen the window is gone, and it stays gone until the poll has read
  the history once since the world came back — until then what is held could
  still be the last character's chat.

  The lines themselves are **kept** across that, and so is the index into the
  history. An earlier build dropped both, on the grounds that the client builds
  a new `ChatClient` for the next session; that is true of a relog and not of a
  loading screen or a zone handover, and on every one of those it re-decoded
  the whole session — appending a second copy of it to the log and pushing
  every line into the window as though it had just been said. A genuinely new
  `ChatClient` is recognised from the history instead: `localReceiveMessage`
  never trims, so a history shorter than the index into the old one is a
  different history. That is confirmed over two polls before anything is
  thrown away, because acting on it wrongly is the expensive mistake.
- **While the developer console is open the window takes no click and no
  wheel**, anywhere. The console owns the `/` key and is a password-gated
  admin surface; the host stays out of it entirely.
- **Turning the window off hides its own replies.** `!!chat` is typed into the
  game's box, so the way back always works, and with the atlas open a line on
  screen says what to type — but the reply to `!!chat off` lands in a window
  that is not being drawn.
