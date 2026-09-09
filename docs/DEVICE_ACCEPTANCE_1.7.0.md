# Device acceptance — 1.7.0

**Commit:** `3688e993`
**Automated state:** the complete suite passes **270 of 271** at this commit
(549m36s, `complete suite, 271/271 tasks`, shipping SDL2 2.32.10, no stale
artifacts). The single failure is `camera_motion_quality`, and §1 below is the
decision that closes it.

Everything an automated gate can settle is settled. This file is the part that
cannot be: three items that need a person, a display, and in one case hardware
this project does not own. They are written so each has a definite verdict to
record rather than an impression to form.

---

## 0. Before you start

Defaults are the shipped defaults. §1 is the only section that asks you to turn
something on, and it asks you to turn it off again afterwards.

Have the log visible (the launcher's Diagnostics view, or the console the binary
was launched from). §1 and §3 read a line out of it; §2 is with your eyes.

---

## 1. The camera correction — the one open automated failure

**What is being decided:** whether the Modern obstruction resolver's single
re-engagement on Ancient Lake is a defect a player can feel, or the track being
narrow. Section 7.3 of
[`architecture/camera-obstruction.md`](architecture/camera-obstruction.md)
reserves this to a signed review on device, which is why no gate can close it.

**Why it is not a regression:** driving the shipped v1.6.0 binary over the same
route produces a byte-identical census -- same `correction_reengagements=1`, same
retract/recovery counts, same 138.4230 cut density. The candidate neither
introduces nor worsens it. The full A/B is in
[`evidence/camera-motion-reengagement-ab-2026-09-08.md`](evidence/camera-motion-reengagement-ab-2026-09-08.md).

**Why it cannot be tuned away:** the release hold was sized at nine ticks -- the
longest measured false-clear run plus one -- deliberately leaving the 9-to-11
tick band uncovered. This event sits at gap 11. Raising the hold to 12 moves the
release to tick 2128 and leaves a gap of 8, still inside the window. Any finite
hold ends and reopens it; covering this event means holding a retraction for the
full 20 ticks between two different walls, across a corridor the sweep called
clear for eleven of them.

**Do this:**
1. Launcher -> `Camera.Obstruction` -> **Modern**. (Default is `observe`; a
   player only reaches this deliberately.)
2. Ancient Lake, time trial, one player.
3. Drive the canyon-wall approach normally, three laps. Watch the camera at the
   wall on the approach, not the kart.

**Record one of:**
- **ACCEPT** — nothing read as a stutter; the re-engagement is the track being
  narrow. Section 7.3 records the reading and the gate's hard invariant is
  amended to exempt a re-engagement onto a *different* blocker id.
- **REJECT** — a visible double-pump at the wall. The resolver stays opt-in and
  the gate stays red, correctly.

Either verdict closes the item. **Set `Camera.Obstruction` back to the default
afterwards** — it is not the shipping default and §2/§3 assume defaults.

---

## 2. Issue #61 — the purple viewport, and the two symptoms already closed

This issue is three separate reports. Two are closed by automation; only the
third needs you, and it needs **Windows with an NVIDIA GPU** (the reporter had an
RTX 2070 Super). No macOS result can settle it — the shim-versus-upstream SDL
lesson in the release checklist is exactly that a green macOS run can coexist
with a broken shipped build.

**Already closed, do not re-test:**
- *Black sky edges in split screen* — fixed; `check_split_screen_backdrop`
  measures worst-case black coverage at 0.16% (P1) and 4.27% (P2) against 1.5%
  and 5% ceilings.
- *P2 pause resolution* — the two-player check compares both pause owners in
  Restored and Remastered on GL and WebGPU, with scaled-UI and wrong-font
  controls.

**Do this, on Windows/NVIDIA:**
1. Two-player Walrus Cove.
2. Drive the loop/tunnel section the report names. Both players through it.
3. Inspect **both** viewports through and after the tunnel.

**Record one of:** *not reproduced on this build* / *reproduced, with the frame
and which viewport*. A single-player cave flash or a Fossil Canyon sky-edge pass
does **not** settle it — those are the symptoms already closed above.

---

## 3. Packaged-artifact acceptance

The four desktop artifacts are CI-built (`release.yml` for Linux and Windows,
`macos-release.yml`). What automation cannot check is that the packaged thing
behaves like the qualified thing.

Per artifact, in order, recording pass/fail:
1. **It starts from a clean machine** — no dev toolchain, no source tree.
2. **The save path works.** Create a file, race, quit, relaunch, confirm it
   persisted. This is the v1.2.1 class: that build shipped msvcrt-linked, so
   `fopen("wbx")` returned EINVAL and every save failed silently. The import
   gate now refuses a non-UCRT Windows binary (`tools/check_windows_imports.sh`,
   four self-test controls), but the gate proves the link, not the behaviour.
3. **Non-ASCII paths.** Install under a directory with non-ASCII characters and
   repeat step 2. Windows especially.
4. **A physical controller.** Bind it, race, and change **Opponent skill** with
   it — that control became a dropdown in this release (issue #62) and its
   controller navigation is the part no gate drives.
5. **Campaign through credits** on one artifact, once.

---

## What is NOT in this file

Anything a gate already decides. The suite covers 270 of 271 tasks at this
commit, including all five worlds' trophy award and cabinet display (issue #63),
the launcher-skip flow (#60), the opponent-skill control's stored values (#62),
custom-character import and KTX2 handling, online preflight/liveness/drop, and
the bonus-roster toggle added late in this cycle. Re-testing those by hand
spends the acceptance session on questions that are already answered.
