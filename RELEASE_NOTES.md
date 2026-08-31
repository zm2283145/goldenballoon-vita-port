# Golden Balloon 1.6.0

*Released 2026-09-01. Online multiplayer is a beta — see below.*

This release adds online multiplayer as a beta and closes a batch of reports.
You can now race a friend online, and the fixes cover both online and offline
play: you race the character you picked, characters no longer briefly T-pose at
the start of a race, and several HUD and menu glitches are gone. Offline play is
unchanged.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Online multiplayer (beta)

You can now race another player online. One of you creates a private race and
shares a six-digit code — or a QR code — and the other joins with it. Pick your
character from the grid, choose a track, and play a single race or a full
tournament. When you connect, both of you see a short pair of words; when they
match on both screens ("Words Match"), you know you are linked to the right
person.

- **Cross-region.** North American and European copies of the game can race
  each other. Online races run at the same speed for everyone, whichever region
  your game is from. Offline play is untouched and still runs at your region's
  original speed.
- **Track selection** is the full track list, grouped by world, so you can see
  every track at a glance.
- Online play runs over Golden Balloon's free hosted service.

This is a beta.

## Fixes

- **Online:** you now race the character you picked. In an early beta build a
  race could start you on a different character than the one you chose on the
  grid.
- Characters no longer briefly appear unanimated ("T-posing") at the start of a
  race. This includes the long-standing offline case tied to the **Model
  Detail** setting, which is now fixed no matter how that setting is set
  (issue #48).
- The **best time** and **best lap** numbers in the track previews no longer
  rock back and forth while the preview flyby plays (issue #59).
- With the widescreen HUD turned on, the banana counter in the banana challenge
  no longer overlaps a character portrait, and the minimap markers stay lined
  up with the map at the right size (issue #57). With the widescreen HUD off,
  nothing changes.
- Added characters can now save Time Trial ghosts. They are marked so they stay
  separate from the original characters' records, which are unchanged
  (issue #54).

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from earlier
1.5.x releases carry over unchanged. The launcher is keyboard and gamepad
operable, but does not claim a VoiceOver, UI Automation, or other screen-reader
semantic tree.

## Known limitations

- Online races are for two players in this beta.

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
