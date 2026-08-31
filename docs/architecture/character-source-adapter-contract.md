# Character source adapter handoff

Status: frozen v1 data contract for external DCC and interchange tools.

## Boundary and user promise

Golden Balloon does not load an FBX SDK, open a Blender project, import a Maya
scene, or execute a converter/plugin in the launcher or game process. An artist
chooses the authoring tool they trust and that tool produces one canonical
`.mdkrsource` artifact. The Workshop treats it only as hostile data.

This boundary provides byte integrity, reproducibility evidence, and a safe
handoff. It does **not** authenticate the adapter author, certify the conversion,
grant model rights, or sandbox code that the artist ran elsewhere. The UI uses
those exact words. A future signed adapter marketplace would be a separate
trust and update system, not a reinterpretation of this format.

## Canonical v1 archive

`.mdkrsource` is a ZIP with exactly these root members:

| Member | Required | Contract |
|---|---:|---|
| `adapter.json` | yes | Strict UTF-8 JSON, at most 64 KiB |
| `model.glb` | yes | Self-contained glTF 2.0 Binary within Workshop bounds |
| `LICENSE.txt` | no | Exact notice bytes, at most 1 MiB; metadata must be present when the member is present |

No directory, duplicate, nested archive, unknown member, symlink, encryption,
ZIP64 output, or compression method other than stored/deflate is accepted.
Declared and actual expanded sizes, aggregate size, and expansion ratio are
bounded before expensive reads. Member names are fixed, so there is no archive
path to normalize or extract.

The manifest has this exact shape; unknown and duplicate fields fail closed:

```json
{
  "schema": "mdkr-character-adapter-output-v1",
  "adapter": {
    "name": "Example Blender Exporter",
    "version": "1.2.0",
    "homepage": "https://example.invalid/exporter"
  },
  "source": {
    "format": "Blender scene",
    "sha256": "<64 lowercase hexadecimal characters>"
  },
  "conversion": {
    "profile": "Golden Balloon character-v1",
    "settings_sha256": "<digest of canonical conversion settings>"
  },
  "model": {
    "file": "model.glb",
    "sha256": "<digest of model.glb>"
  },
  "license": {
    "file": "LICENSE.txt",
    "sha256": "<digest of LICENSE.txt>",
    "spdx": "CC-BY-4.0",
    "attribution": "Artist name",
    "source_url": "https://example.invalid/model"
  }
}
```

`adapter.homepage` and the complete `license` object are optional. Everything
else is required. HTTP(S) URLs may not carry credentials or fragments. Text is
bounded printable NFC Unicode; SPDX syntax is structurally parsed. The adapter
identity and URLs are claims carried by the artifact, not authenticated facts.

For a single source file, `source.sha256` hashes its exact bytes. For a
multi-file scene, the adapter must define and document a deterministic source
snapshot/archive and hash that canonical byte stream. It must never place a
local path, username, timestamp, access token, command line, or arbitrary DCC
metadata in the manifest. `conversion.settings_sha256` similarly binds a
documented canonical settings record without leaking that record into a shared
artifact. The profile name tells a human which published settings schema the
digest uses.

## Workshop lifecycle

1. The user chooses, types, or drops a `.mdkrsource` file.
2. The frozen importer reads it as a bounded regular file, validates its exact
   schema/member inventory/digests, runs the pinned Khronos validator on the
   embedded GLB, and applies the narrower character profile. No output changes.
3. The Workshop shows adapter name/version, homepage claim, source format and
   digest, conversion profile/settings digest, model size and geometry counts,
   and exact included rights metadata. It explicitly discloses that integrity
   is not a signature.
4. The user separately accepts this external result and chooses a new `.glb`
   destination. The action stays keyboard-focusable when those requirements
   are missing so its explanation remains available to speech users.
5. At commit, the importer re-reads the artifact and requires both the reviewed
   artifact and model digests. It repeats pinned GLB validation. A changed file
   returns to review.
6. It preflights all destinations, then exclusively creates the GLB, a
   privacy-safe `<name>.mdkrsource.json` provenance sidecar, and—when present—
   `<name>.LICENSE.txt`. Any collision refuses the entire operation; a later
   write failure removes only files created by that attempt.
7. The GLB opens as an ordinary resumable raw-source draft. Included SPDX,
   attribution, source URL, and notice path are prefilled, never silently
   approved. The author still reviews axis, scale, rig, identity, donor,
   vehicles, license, provenance, performance, build diff, and local install.

A failed inspection/extraction creates only bounded private recovery metadata.
Retry never remembers acceptance or writes output; it re-inspects the exact
unchanged artifact. The original artifact is never modified or installed.

## Adapter-author workflow

Repository authors can produce the canonical archive with the reference packer:

```sh
python3 tools/character_source_adapter.py pack character.glb character.mdkrsource \
  --adapter-name "Example Blender Exporter" \
  --adapter-version "1.2.0" \
  --adapter-homepage "https://example.invalid/exporter" \
  --source-format "Blender scene" \
  --source-sha256 "$SOURCE_SHA256" \
  --conversion-profile "Golden Balloon character-v1" \
  --conversion-settings-sha256 "$SETTINGS_SHA256" \
  --license LICENSE.txt \
  --license-spdx "CC-BY-4.0" \
  --attribution "Artist name" \
  --source-url "https://example.invalid/model"
```

Omit all four license arguments together when rights bytes are unavailable;
the Workshop will then require the author to choose an exact notice. The packer
uses exclusive create, deterministic ordering/timestamps/permissions, stored
members, and self-verifies its result. Inspect without extracting with:

```sh
python3 tools/character_source_adapter.py inspect character.mdkrsource
```

An adapter may implement the same published bytes directly. Compatibility is
defined by this document and the hostile fixtures, not by importing project
Python or copying launcher internals into a plugin.

## Versioning

V1 is exact and fail-closed. New fields, members, compression methods, signature
envelopes, source graphs, or material/animation capabilities require a new
schema and explicit old/new behavior. A future signature must cover a canonical
artifact digest and identify a trust/update policy; it may not relabel the v1
integrity hash as authentication.
