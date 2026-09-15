# Khronos glTF Validator release inputs

Golden Balloon ships Khronos glTF Validator `2.0.0-dev.3.10` at commit
`bcd52cc4ba5f333b2999a58f67cc05ddf28b4fb1` beside the frozen Character
Workshop importer. It validates every GLB before the project-owned character
profile and compiler checks run.

Linux x86-64 and Windows x86-64 use the official upstream release archives.
The upstream macOS release is x86-64-only, so macOS arm64 is reproducibly
compiled from the exact source archive with Dart SDK 2.19.6 and this directory's
fully resolved `pubspec.lock`. `tools/build_gltf_validator.py` authenticates all
archives, rejects unsafe members, pins the resulting native executable hash,
and emits the adjacent build manifest consumed by
`tools/gltf_validator_adapter.py`.

This repository does not vendor the validator source, SDK, dependency cache, or
native executable. `LICENSE.txt` and `NOTICES.txt` are the unmodified LF bytes
from the pinned upstream release and are included in every native package.
