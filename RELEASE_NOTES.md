# Golden Balloon 1.6.1

*Released 2026-09-10.*

## PS Vita

The PS Vita port is now stable for normal play on tested hardware using the
default Restored visual preset. This release adds an unsigned-homebrew,
NoTrpDrm-compatible 95-trophy pack: the main set includes a custom platinum,
while Adventure 2 and every T.T./developer time-trial challenge are optional
groups and are not required for the platinum.

The VPK is versioned from the same `MDKR_VERSION` value as the compiled port;
release 1.6.1 produces Vita metadata version `01.61`. Trophies require the
[NoTrpDrm](https://github.com/TheOfficialFloW/NoTrpDrm) taiHEN plugin. Without
it, the game remains playable and safely skips trophy setup.

The Remastered visual preset still crashes on startup on Vita. Use Restored.

# Golden Balloon 1.6.0

*Released 2026-09-01. Online multiplayer is a beta — see below.*

Golden Balloon can now play online. Race a friend head-to-head over the
internet — private races with a six-digit code, single races or full
tournaments, across regions. This release also fixes T-posing characters,
wobbly best-time numbers, and several widescreen-HUD glitches.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Online multiplayer (beta)

Create a private race and share the six-digit code — or the QR code — and
your friend joins with it. Pick your racer, pick a track from the full track
list, and play a single race or a tournament. When you connect, both screens
show the same three groups of words; read them aloud to each other, and if
they match, you know you are linked to the right person.

- North American and European copies of the game race each other. Online
  races run at the same speed for everyone; offline play still runs at your
  region's original speed.
- Online races are for two players in this beta.
- **Tested so far on macOS only.** We race on macOS ourselves. Windows and
  Linux have online too, but nobody has played it there yet — if you do, you
  are the first, and we want to hear how it went.
- **Both players need the same platform for now.** Mixed pairs, like Mac to
  Windows, are declined when joining; cross-platform play comes in a later
  update.
- Online play runs over Golden Balloon's free hosted service.

Online is early access and a work in progress. Please tell us what you hit —
good or bad — on the GitHub issues page. Your reports decide what gets
improved next.

## Fixes

- Characters no longer briefly appear unanimated ("T-posing") at the start of
  a race, at any **Model Detail** setting (issue #48).
- The **best time** and **best lap** numbers in track previews no longer rock
  back and forth during the preview flyby (issue #59).
- With the widescreen HUD on, the banana counter no longer overlaps a
  portrait in the banana challenge, and minimap markers line up with the map
  at the correct size (issue #57). With it off, nothing changes.
- Added characters can now save Time Trial ghosts, kept separate from the
  original racers' records (issue #54).

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from earlier
1.5.x releases carry over unchanged. The launcher is keyboard and gamepad
operable, but does not claim a VoiceOver, UI Automation, or other
screen-reader semantic tree.

# Golden Balloon 1.5.2

*Released 2026-08-25.*

A bug-fix release closing the reports that came in after 1.5.1: the
Widescreen HUD lays out correctly across the reported screens, a stray
rectangle in the menus is gone, Time Trial times and ghosts save reliably on
Linux, a Hot Top Volcano out-of-bounds exit no longer hangs, and Nintendo
Switch Online N64 controllers map correctly. There are no new features and no
changes to gameplay.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Fixes

- The Widescreen HUD — the "Expanded HUD" from the reports — now lays out
  correctly: Time Trial lap times no longer pile up on the right, the TAJ
  MAGIC label is centered, the battle-mode HUD sits where the game intended,
  and the race-start slide begins off screen instead of parking at the right
  edge (issue #51). It is still off by default, and the normal HUD is
  unchanged.
- Switching Game Paks in Save Options, scrolling the name-entry letters, and
  the end credits no longer flash a thin stretched bar across the screen. The
  game occasionally asks for a rectangle drawn with its corners swapped; real
  N64 hardware draws nothing for those, and now the port matches — which
  clears the same artifact everywhere it appeared (issues #52 and #56).
- On the PAL (European) version, the menu text no longer creeps upward after
  you return to the launcher and start the game again.
- The brief blue flash in the Walrus Cove cave is not a bug — it is faithful
  to the original game, which draws its own out-of-bounds curtain in exactly
  that blue. It looks identical with every enhancement turned off (issue #53).
- Time Trial times and ghosts now save reliably on Linux (and other
  non-macOS native builds). Saves resolve to your per-user profile
  directory; if you were relying on a folder-local `save/` it is kept in
  place, and you can force folder-local saves with a `portable.txt` beside
  the game. The game also now tells you, instead of failing silently, if it
  ever cannot write your progress (issue #54).
- Leaving Hot Top Volcano through the out-of-bounds trophy-storage route no
  longer hangs on a black screen; it now returns to the hub exactly as the
  original game does, so the trick still works (issue #55).
- Nintendo Switch Online N64 controllers now map correctly over Bluetooth —
  the C-buttons no longer open the overlay or double as Z. You can also now
  change which button opens the in-game overlay in Settings ▸ Controls
  (issue #55).

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from
1.5.0 and 1.5.1 carry over unchanged. The launcher is keyboard and gamepad
operable, but does not claim a VoiceOver, UI Automation, or other
screen-reader semantic tree.

## Known reports under investigation

One Windows report of characters briefly appearing unanimated ("T-posing")
at race start (issue #48) still could not be reproduced from source, and
the game applies each racer's seated animation before the first frame is
drawn. If you saw this, please retest on this release and report either
way in the issue.
