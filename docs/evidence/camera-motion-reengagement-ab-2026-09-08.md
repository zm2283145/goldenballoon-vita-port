# MOTION-01 re-engagement: v1.6.0 / v1.7.0-candidate A/B

Date: 2026-09-08. Route: `ancient-lake-race` (`tests/input_scripts/race_drive_long.txt`,
5200 frames, 1280x960, `--fov 70`). Both binaries driven with the pinned environment
`check_camera_motion_quality.run_route` builds, by hand, so the comparison could not
inherit the suite's per-task binary wiring.

## Why this was re-measured

An earlier A/B concluded "byte-identical, not a regression". That run was invalid:
`run_checks` handed `check_pacing_quality` the 1.6.0 build and
`check_camera_motion_quality` the candidate build in the same invocation, so the
camera arm compared the candidate against itself. The conclusion happened to be
right; the evidence for it did not exist. Re-run directly against
`mdkr64-int-1.6.0/build-160/mdkr64`.

## Result

The census is identical between the shipped v1.6.0 binary and the candidate, field
for field, except for `shoulder.basis_crossings`, which is instrumentation the
candidate adds (`abe214f7`) and 1.6.0 does not emit:

    slot_ticks=5187  ticks=5187  cut_ticks=715
    profiles={full=2409 safety_only=0 depenetrate_only=2778}
    events={retract=9 recovery=2 alternate_entry=0 alternate_exit=0
            emergency_entry=0 discontinuity=718 degenerate=0}
    release_hold={held_ticks=72 spans=9 window=9}
    chatter={oscillation_cycles=1 oscillation_cycles_excused=0
             correction_reengagements=1}
    shoulder={flips=0 continuous_surface=0 new_surface=0}
    churn={blocker_changes=2 same_surface=2 new_surface=0}
    emergency={max_dwell=0}
    discontinuity_per_1000_ticks=138.4230

`correction_reengagements=1` is present in the shipped v1.6.0 build. The candidate
neither introduces nor worsens it, and no other counter moves.

## The event

    tick=2136 viewport=0 release_tick=2125 gap=11
    last_contact={tick=2116 kind=1 id=212 normal=(-0.95789,-0.12744,0.25730)}
    contact={kind=1 id=317 normal=(-1.00000,0.00000,0.00000)}
    state={prior_recovering=0 recovering=0 held=0 alternate=0 emergency=0 degenerate=0}

Contact on surface 212 is lost at 2116. The release hysteresis holds the retraction
for its full nine ticks and the phase goes clear at 2125. Eleven ticks later the
boom is blocked again, by a different surface: id 317, and an axis-aligned normal
where 212's was a slanted facet. Every hysteresis state flag is clear at the moment
of re-engagement, so this is not a latch dropout.

## Why no tuning is proposed here

`660ce8cf` sized the release hold at nine ticks as "the longest measured [false
clear] run plus one tick of margin, and still short of the 12-tick window the census
asserts on, so a clear run of 9 to 11 ticks still registers as a re-engagement". The
9-11 band is uncovered by design. This event sits at gap 11.

That commit also established the discriminator the hold rests on: of the five short
clear runs it fixed, "two of them carry the identical blocker id on both sides of the
gap, which is what proves the gap is a false negative rather than a wall the kart
drove past". This event carries different ids on the two sides.

Extending the hold cannot close this structurally. Any finite hold ends, and the
12-tick chatter window reopens the moment it does: raising the hold to 12 moves clear
onset to 2128 and leaves a gap of 8, still inside the window. Covering this event
requires holding a retraction for the full 20 ticks between the two contacts, over a
corridor the sweep reported clear for eleven of them, which is the cost the census
already reports as `release_hold.held_ticks` and which `660ce8cf` deliberately
bounded.

The alternative is to let surface identity excuse a re-engagement, which the runtime
refuses on purpose ("normal agreement or a different face ID cannot excuse chatter"),
and which sits against the adjacent invariant (a.2) asserting shoulder flips only on
a continuous surface. Whether a re-engagement onto new geometry is a defect or is the
track being narrow is a quality judgement, not a structural one.

Section 7.3 reserves those bounds for a signed review calibrated on device, and
`docs/architecture/camera-obstruction.md` 10.1 records that default-on was reverted
the same day it landed on device acceptance. Picking a number here to turn the gate
green would launder a guess into a gate, which is the one thing the gate's own
docstring says it exists not to do.

## Scope

The Modern resolver is opt-in. An unset `MDKR_CAMERA_OBSTRUCTION` resolves to
`observe`, which measures and leaves the authored camera in place, so default play
does not run this correction at all. A player reaches it through the launcher's
`Camera.Obstruction` control. This finding therefore belongs to the open
default-on item, not to the v1.7.0 release.
