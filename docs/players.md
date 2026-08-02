# Players

Every player this client has been sent, listed in full: name, class, distance,
sortable by each, your own party spelled out above it. Click a row and the
navigator follows that player as they move. **DM** copies the game's own
`!to <name> ` to the clipboard; **Copy** copies the bare name.

Part of the standalone host ([`host/`](../host/README.md#players)) — a tab in
the atlas window, opened with **F8**. The tab is labelled with the count the
last read saw, `Players 18`.

> **What has been seen working, and what has not.** Seen on screen in a live
> session: the tab, and the roster in it — **18 players, where the game's own
> Manage Party window listed 0**.
>
> Built and compiling but **not yet seen running**: click-a-row to follow, the
> sortable columns including the class column, and the DM and Copy buttons.
> Read those parts of this page as a description of what the code does.

## Why the page exists

The game already has the whole roster. Its own Manage Party window shows you a
fraction of it and throws the rest away.

`ui.win.GroupWindow.init` (GroupWindow.hx:58-63) walks `myPlayer.layer.players`,
squares `Const.UI.GroupWindow_NearDist` and buckets every player on it, builds
the near bucket as player cards — and builds the far bucket too, drawing it
**only when `Config.prefs.admin` is set**, under a header reading *"(ADMIN)
Other loaded players ("*. The constant is 100, and its own CastleDB description
says what it is for: *"Other players within this distance are shown in the
Manage Party window"*.

So the distance limit is presentation. The rest of the list is already in this
client's memory, replicated to it by the server, and the only thing standing
between it and a screen is a window that declines to draw it. Live, that was 18
players against the game's own 0.

Nothing here defeats a protection. The data is in the process; this stops
throwing it away.

## The list

Above the rows: a block of notes saying what the list is and is not, what a `-`
means, what the class column reads, and how the DM button works. Those notes
are the page, not decoration — the roster is the kind of data that is easy to
over-read, and the qualifications belong next to it rather than in a document
nobody opens.

Then your own party, then whoever is being followed, then a count: *N players,
M with a character replicated here - click a column to sort, again to
reverse*. The gap between those two numbers is the useful diagnostic; it is how
many rows this client can name but cannot place.

Rows are 30 pixels and whole ones only — the overlay cannot clip, so a half-row
would be painted past the window's edge. The wheel scrolls two rows a detent
and a bar appears at the right when there is more than a page.

### Name

`st.Player.name`. A name that has not arrived reads as *(name not readable)*
rather than as *Unknown*, so an absent name is distinguishable from somebody
called anything at all.

Under it, in small text: `you` or `in your party` where either applies, and the
row's uid. Your own row is tinted and carries a blue bar down its left edge; a
party member carries a green one. A followed row is outlined in amber — a third
thing, in a third colour, because the fill and the left edge already each carry
two meanings.

### Class

`ent.Unit.kind` read off that player's hero: the unit id that
`Unit.set_kind` (Unit.hx:686) uses as the key into `Data.unit.byId`, which on a
hero is the class — Warrior, Rogue, Mage or Priest. An id that is none of those
four is drawn exactly as it reads; this column reports what is there rather
than rounding it to the nearest class we know.

A player whose character has not been replicated to this client has no hero,
and so no class. The cell is **blank**, not a placeholder.

The column is **dropped outright** when the window is narrowed past the point
where it would land on the names. The overlay cannot clip, so the choice is
dropping it or drawing it over something else.

### Distance

Horizontal distance and a compass bearing — `412m NE`, `1.24km SW` — from the
same formatter the navigator's pill uses. Horizontal because that is what the
sort compares as well: a key that counted height would put a row above one
showing a larger number, and the list would look broken rather than
three-dimensional.

The number is formatted on the draw and the ordering only settles once a
second, with the poll. That is deliberate: a list that re-sorted every frame
while you ran would move the row out from under the pointer.

`-` means **there is no distance to show**. Either that player's character has
not been replicated to this client, or your own position could not be read at
that moment — the page adds a line saying so when it is the second. It never
means far away. See [Limitations](#limitations).

## Sorting

Click a column header to sort by it; click it again to reverse. A different
column starts in its own natural direction — names A–Z, nearest first — rather
than inheriting the last column's, which would silently sort the new one
backwards. The active column is marked with an `^` or `v` and a rule under it.

A row is ordered against the others only when it **has a value in the active
column**. `-` is not the largest distance and a blank class is not the last
class in the alphabet: both are absences, so they sit at the bottom in either
direction. Reversing reverses the rows that have a value; promoting the ones
that have none would present an absence as a ranking.

The sort is stable over the order the poll published — nearest first — so rows
tying on the active column keep a settled order instead of shuffling on every
re-sort. Changing it resets the scroll to the top, because a scroll position
measured against the old order means nothing.

Both the column and the direction persist; see [Settings](#settings).

## Following a player

Click anywhere on a row left of its buttons and the navigator is pointed at
that player. The poll re-publishes their position every tick, so the arrow
tracks them as they move. Click the row again to stop.

This is the same read the distance column already makes, handed to the module
that draws it bigger. Nothing is written and nothing is asked of the game: the
pill is an overlay readout, not a marker placed in the game's map.
[`!!where`](chat.md#commands) in the game's own chat box names who is being
followed.

Who is being followed is held by **uid**, never by row index — the list
re-sorts under the pointer on every poll, and following an index would quietly
turn into following whoever slid into it. The uid is `st.BaseState.__uid`, the
session-local id hxbit assigns to a replicated state, and a row whose uid did
not read cannot be followed: there is nothing stable to keep hold of them by,
and the page says so rather than doing nothing on the click.

Two other clicks that do not start a follow, both of which say why:

- **your own row** — the navigator already knows where you are;
- **a row with no position** — their character has not been replicated to this
  client, so there is nothing to point at. That is said in those words, and not
  as *they are too far*.

### When a followed player goes away

Following stops on its own, and the reason is written where the follow line
was. There are four of them, and **none of them is "they left" or "they are far
away"** — neither is knowable here, because a row going away is a fact about
this client's copy of the roster and not about the player:

- the navigator was pointed at something else, or stopped;
- the roster cannot be read at all just now;
- they are no longer in the roster this client has been sent, so this client
  cannot see them any more;
- their character is no longer replicated to this client, so there is no
  position left to point at.

That line stays up until the next click rather than fading like the other
feedback on the page: the reason a follow ended is the one thing worth not
missing.

A follow does not survive the process. The navigator restores what it was
following from `farever-nav-state.txt` at startup, and if that was a player it
is now an old coordinate with somebody's name on it, so the page drops it —
only its own keys, never anything else you were tracking. While a follow is
running the navigator is marked dirty on every re-publish, so that file is
rewritten about once a second. It is a few hundred bytes.

## The DM and Copy buttons

**Copy** puts the bare name on the clipboard. **DM** puts `!to <name> ` there,
trailing space included: paste it into the game's own chat box and type your
message after it.

That is the shape every action on this page has to take. The host cannot type
into the chat box or move the channel dropdown — both are writes — and input
synthesis is banned outright, so the paste is yours and the send is the game's.
`!to` is the game's own whisper command, parsed by
`ui.hud.ChatBox.processMessage` (ChatBox.hx:132-171), which resolves the name
through the layer's own player lookup.

The same goes for `!say`, `!map` and `!group`: a line that already begins with
`!` picks its own channel and the dropdown is never consulted
(ChatBox.hx:133-134). See [the chat page](chat.md#commands) for what
`processMessage` does with everything else.

DM is disabled on your own row. Your own name is in the layer's player list
like everybody else's, so nothing would stop the command resolving — it is just
not a thing anyone means to paste.

### Whispering somebody adds them to the game's own dropdown

Worth knowing because it turns the paste from a chore into a one-off.

Whisper somebody once and `processMessage` searches `channelOptions` for the
whisper it just built, does not find it, pushes `{name, icon, value}` and
rebuilds the dropdown (ChatBox.hx:159-162). After that they are a channel you
select, for the rest of the session, and every line you type goes to them with
no prefix at all.

Being whispered *at* does not do this. `ChatBox.receiveMessage`
(ChatBox.hx:126-129) builds the line and scrolls, and that is all of it — so
somebody can whisper you all evening without ever appearing in your dropdown.
The DM button is therefore also the quickest way to get a reply channel for
somebody who messaged you first.

## The invite question

It is the obvious thing to ask of a list of distant players, so: **there is no
invite button, and that is a decision rather than an oversight.**

The game's own invite has no distance term. `st.Group.invitePlayerReason`
(Group.hx:131-139) rejects on four things and none of them is range: not being
the leader, the target already being grouped, already being a member, and a
group of four. `ui.Console.invite` invites by name across the layer. So the
data model permits inviting somebody the Manage Party window will never show
you, and the proximity limit really is one filter in one window.

Every route to firing that from here is a call into the game, and this host
does not call into the game. Both actions a row offers end at the clipboard for
the same reason.

## Settings

`farever-modkit.ini`, next to `Farever.exe`, section `[players]`. The same file
the rest of the host uses; written on the worker thread shortly after a header
is clicked.

| Key | Default | Meaning |
|---|---|---|
| `sort` | `1` | Column: `0` name, `1` distance, `2` class |
| `reverse` | `0` | `1` sorts the other way |

`sort` is range-checked on the way in. A value outside those three would leave
the header marking nothing while the list stayed in whatever order the poll
published, and this file is meant to be hand-editable.

## What is read, and from where

Every one of these is a validated read through `hl_runtime.h`; nothing is
written, hooked or called. The whole roster is read in one pass on the worker
thread's once-a-second tick and published as a snapshot — the draw callback
never walks game memory, which is the rule every module on this host follows.
The module has a 500 ms floor of its own on top of that, which only comes into
play when the worker's wait is cut short by entering or leaving the world.

| What | Path |
|---|---|
| The roster | `GameApp.layer` → `st.GameLayer.players`, a replicated proxy array |
| Each entry | `st.Player`: `name`, `__uid`, `removed`, `hero`, `group` |
| The id | `st.BaseState.__uid` — **not** `st.Player.uid`, which is a separate replicated `String`; both offsets exist and reading one for the other would report a pointer as a number |
| Whether it is you | pointer equality against `ent.Hero.player`, which is the test the game makes too (GroupWindow.hx:60 is an `indexOf` with a null comparator, i.e. reference equality). `st.Player.isMe` is read only as a cross-check, and a disagreement is one line in the log |
| Your party | your own `st.Player.group` → `st.Group.players`, leader first — `st.Group.get_leader` is literally `players[0]` |
| Position | that player's `hero` → `ent.GameObject.posx`/`posy`/`posz`, the same values the game itself compares (`Entity.set_posx` writes `position.x` and then `posx` in the same breath, Entity.hx:69-70) |
| Class | that hero's `ent.Unit.kind` |
| Your own position | the navigator's hero position |

Entries with `st.BaseState.removed` set are skipped: that is the tombstone the
game itself checks before it looks at a roster entry at all
(GroupWindow.hx:62), and it means the client has already been told that player
is gone.

A roster reporting more than 4096 entries is clamped rather than rejected — the
first entries of an array whose length went wrong are usually still the real
ones — and the log says it happened, and says the list is a prefix. A party is
bounded at 64 the same way.

`farever-modkit.log` carries one line on the first successful read: how many
players were in the array, how many had a hero, how many did not, how many were
skipped as removed, and your party size. When the page is a screenful of
dashes, that line is where to look first.

## Limitations

- **This is what the server chose to replicate to this client.** It is not
  provably everyone on the shard, and nothing on the page is worded as though
  it were. A player the server has not told this client about does not appear,
  and cannot.
- **`-` is not a distance and never means far away.** `st.Player.hero` is null
  for a player whose character has not been replicated here, and a player with
  no hero has no position at all. The game's own window happens to bucket those
  two together — GroupWindow.hx:62 sends a null hero straight to the far list —
  and this deliberately does not. A `-` also appears when your own position
  cannot be read, and the page says which case it is in.
- **A blank class is the same absence.** No hero, no `kind` to read. The column
  is left empty rather than filled with a default.
- **You cannot tell whether anybody else is in a party.** `st.Player.group` is
  network property bit 12 (`st.Player.__net_mark_group`, hxbit/Macros.hx:2104)
  and sits in the conditional visibility mask, so it reads null for everyone
  except the local player. Your own party is listed; nobody else's is guessed
  at, which is why there is no *already grouped* or *invitable* column. There
  is nothing truthful to put in one, and a guess dressed as a column would be
  read as knowledge.
- **The uid is session-local.** `__uid` is what hxbit assigned this replicated
  state in this client, in this session. It is enough to hold onto somebody for
  as long as this client can see them, and it is not an account identity.
- **Distance is horizontal.** Height is in neither the number nor the sort, so
  somebody directly above or below you reads as close.
- **A failed read empties the page** rather than leaving the last zone's list
  up with distances recomputed against wherever you now stand. Failure here is
  overwhelmingly "no character in the world" — the main menu, character select,
  a logout — and in that state there is no roster. The page says it could not
  read one; the log says what the walk found.
- **The tab's count is the last read's.** Zero also means it has not read yet,
  and the page itself says which of the two you are looking at.
- **No invite, and no other control that calls into the game**, for the reason
  in [The invite question](#the-invite-question).
