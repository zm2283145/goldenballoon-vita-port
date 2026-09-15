# Custom character device profile v1

`mdkr-character-device-profile-v1` is the portable, privacy-bounded result of
one complete Custom Character Workshop performance matrix. It exists so
maintainers can collect real high-, mid-, and low-tier device observations
without asking players to share a character package, model, ROM, save data,
artwork, local path, or player-facing identity.

The launcher creates the file locally and never uploads it. Export is optional,
requires an explicit GPU/driver/build/resolution disclosure, revalidates every
row immediately before writing, uses exclusive creation, and never replaces an
existing destination.

## Comparability

Compare two reports only when both `workload.id` and `environment.build` match.
The workload ID is a SHA-256 projection of the exact source, applicable
per-context fits, renderer presentation, vehicle subset, model/texture costs,
and authored LOD costs. Raw input digests are not published. The ID is
pseudonymous rather than anonymous: someone who already holds the exact
workload can recognize its matching reports.

Backend, adapter, driver, vendor/device IDs, host platform, result contract,
and each row's output/render dimensions remain visible stratification fields;
they are not silently normalized. Different workloads, builds, presentation
contracts, or result versions must never be pooled into one device conclusion.

## Required document

The UTF-8 JSON object is bounded to 512 KiB and has these top-level members:

- `schema`: exactly `mdkr-character-device-profile-v1`;
- `comparisonRule`: exactly `same-workload-id-and-build-only`;
- `privacy`: affirmative disclosure flags, including that device/driver
  identity is present, raw workload digests are absent, and the derived ID is
  pseudonymous;
- `workload`: derived ID, enabled vehicle bitmask, LOD0 vertices/triangles/
  primitives, joint and texture counts, decoded texture bytes, and every
  authored LOD's exact vertices/triangles/primitives;
- `environment`: app build, result contract, host platform, renderer backend,
  adapter, driver, and numeric vendor/device IDs;
- `target`: the published p95 and p99 wall-interval thresholds in microseconds;
- `summary`: required/on-target row counts and the first/last capture times;
- `rows`: one entry for each applicable context and each 1P through 4P layout.

Character select is always applicable. Vehicle-mask bits 0, 1, and 2 add car,
hovercraft, and plane respectively. Every applicable context therefore has
exactly four rows. Each row names its context and player count, preserves its
capture time and physical output/render dimensions, wall cadence distribution,
tick-wall mean, replacement draw/primitive counts, target result, and the exact
GPU timestamp availability, scopes, exclusions, scene-pass distribution, and
character-draw distribution. Unsupported GPU scopes remain zero/unavailable;
no estimate is substituted.

## Producer acceptance

The v1 producer refuses the report unless all of these are true:

1. Every required row is a qualified real-time `Latest` result with warm-up,
   at least 60 intervals, a visible replacement draw, and complete device and
   physical-dimension identity.
2. Every row matches the current source, its exact context fit, renderer
   presentation, result contract, app build, backend, adapter, driver, and
   physical device. Row-owned output/render dimensions may differ and remain
   explicit.
3. Every cadence and optional GPU distribution passes the authenticated
   evidence-store structural contract. Over-target rows are retained and never
   relabelled or omitted.
4. Model, texture, and LOD costs are complete and stay inside the importer
   safety ceilings. Missing, partial, duplicate, stale, synthetic,
   mixed-device, or inapplicable rows fail closed.
5. The destination ends in `.json`, does not already exist, and completes a
   flushed/synchronized write inside the byte bound.

Unit coverage exercises complete and over-target matrices, workload identity,
partial/stale/mixed-device/invalid-cost/control-text/wrong-suffix/overwrite
negatives, and privacy assertions. The rendered launcher gate produces and
parses a real 16-row file through the visible share workflow and confirms that
package ID, display name, source directory, and raw source/fit/presentation
digests are absent.

## Corpus maintenance

A maintained corpus is evidence, not a checked box in code. Curators should:

1. publish the exact license-clean workload and build used for a collection
   round;
2. accept only structurally valid reports with its expected workload ID and
   build, while retaining device, driver, resolution, target misses, and
   unsupported GPU scopes;
3. deduplicate exact files and keep raw observations append-only;
4. summarize high-, mid-, and low-tier devices without promoting one fast
   machine or a generated CI fixture into a representative tier;
5. version any future schema rather than changing v1 field meaning; and
6. use collected evidence to revise guidance, never to weaken hard memory,
   parser, or renderer safety ceilings.

The game does not import a corpus and does not auto-tune from community files.
Any future ingestion, signing, upload, retention, or recommendation service is
a separate trust and privacy boundary.
