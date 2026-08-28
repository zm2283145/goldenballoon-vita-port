# meshoptimizer simplifier provenance

The optional offline custom-character LOD helper compiles only
`src/meshoptimizer.h` and `src/simplifier.cpp` from meshoptimizer v1.2, pinned
to immutable commit `9d9890c73011d75920af614485296d1e03e95448`.

Every fetched file has an independently pinned SHA-256 in
`cmake/character_meshoptimizer.cmake`. Release builders can provide those exact
relative paths through `MDKR_MESHOPTIMIZER_LOCAL_CACHE`; the hashes are still
checked. The helper is a bounded, data-only subprocess used while authoring and
is never loaded into the game process.

Upstream: https://github.com/zeux/meshoptimizer

License: MIT; see `LICENSE.md`.
