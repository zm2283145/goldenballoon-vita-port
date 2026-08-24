# Golden Balloon 1.5.2

*Released 2026-08-24.*

A bug-fix release closing the reports that came in after 1.5.1: the
Widescreen HUD now lays out every game mode correctly, a transient rectangle
glitch in the Save Options screen is gone, time-trial times and ghosts now
save reliably on Linux, a Hot Top Volcano out-of-bounds exit no longer hangs,
and Nintendo Switch Online N64 controllers map correctly. There are no new
features and no changes to gameplay.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Fixes

- The Widescreen HUD — the "Expanded HUD" from the reports — now positions
  every screen correctly: time-trial lap times no longer pile up on the
  right, the TAJ MAGIC label is centered, the battle-mode HUD sits where
  the game intended, and the race-start HUD slide now begins off screen
  instead of parking at the right edge (issue #51). The Widescreen HUD is
  still off by default, and turning it off is unchanged, byte for byte.
- Switching Game Paks in Save Options no longer flashes a thin colored
  rectangle across the screen. The game occasionally asks for a rectangle
  drawn with its corners swapped; real N64 hardware refuses those, and now
  the port does too — which also cleans up fainter versions of the same
  artifact elsewhere in the menus (issue #52).
- Two internal robustness fixes in the out-of-bounds geometry system,
  found while investigating a Walrus Cove report (issue #53). The brief
  blue flash in the cave there matches the original game's own
  out-of-bounds curtain — the level data itself specifies that exact blue,
  and it appears identically with every enhancement switched off.
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
1.5.0 and 1.5.1 carry over unchanged. Phone Party and Online Room remain
out of the player-facing build, exactly as in 1.5.1. The launcher is
keyboard and gamepad operable, but does not claim a
VoiceOver, UI Automation, or other screen-reader semantic tree.

## Known reports under investigation

One Windows report of characters briefly appearing unanimated ("T-posing")
at race start (issue #48) still could not be reproduced from source, and
the game applies each racer's seated animation before the first frame is
drawn. If you saw this, please retest on this release and report either
way in the issue.

One report of some spectate-camera cuts feeling abrupt in a three-player
time-trial spectate view is still being looked at; it does not affect normal
racing.
