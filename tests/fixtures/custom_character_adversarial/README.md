# Adversarial custom-character fixture

`tools/character_spike_fixture.py` deterministically generates this fixture in
code so every geometric and authoring property remains auditable. No binary
model is committed and no retail or third-party art is used.

The fixture intentionally combines:

- a pelvis and spine that are siblings below `Skl_Root`;
- centimeter coordinates plus an additional scene scale;
- a three-joint hair chain, distinct body/hair/face materials, and a visibly
  recognizable front;
- a valid animation container whose two keys are identical;
- long arms, short legs, a wide torso, and an oversized head/hair volume; and
- a reviewed role map that uses `Skl_Root`, not the misleading `Hip`, as the
  structural hips authority.

The generated `model.glb`, `portrait.png`, `manifest.json`, and `LICENSE.txt`
are CC0-1.0. They exist only in the caller-selected temporary/evidence
directory and never enter the user's normal character library.
