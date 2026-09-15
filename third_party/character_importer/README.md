# Character importer runtime notices

Native release packages freeze the first-party files listed by
`tools/build_character_importer.py` into a separate `character_importer`
helper. The build contract is:

- CPython 3.13.13 runtime, built by the target platform's pinned release job;
- PyInstaller 6.22.2 bootloader from the hash-pinned platform wheel in
  `tools/character_importer_build_requirements.txt`;
- first-party importer modules authenticated by the generated
  `character_importer.manifest.json`; and
- no ROM, model, portrait, example package, or other game/community asset.

`CPython-LICENSE.txt` is the unmodified license from CPython tag `v3.13.13`
(SHA-256 `78b12c3a81360b357002334f0e70ea0e92eebf7a9b358805c03c48484945f3bb`).
It covers the embedded interpreter and standard-library components, including
the third-party notices reproduced by that license.

`PyInstaller-COPYING.txt` is the unmodified licensing terms and bootloader
exception from PyInstaller tag `v6.22.2`
(SHA-256 `dcf75fdb959db1e3b41c0f8505069d2ece781b5ec6b3d0a4d30975cfc6580245`).
The exception permits distributing the frozen executable under the same terms
as the application source; none of PyInstaller's build system is linked into
the game executable.

PyInstaller's other exact requirements (altgraph, packaging,
pyinstaller-hooks-contrib, setuptools, and the platform-only macholib or
pefile/pywin32-ctypes packages) execute only in the isolated release build
environment. They are not imported by the frozen entry point and are not
redistributed in the helper. Their versions and official wheel hashes remain
tracked so the build cannot silently resolve new code.
