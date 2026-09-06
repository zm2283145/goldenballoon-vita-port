# Basis Universal runtime notices

Golden Balloon compiles the bounded Basis Universal KTX2 transcoder and its
single-file Zstandard decoder from KTX-Software commit
`4d6fc70eaf62ad0558e63e8d97eb9766118327a6`. The source is fetched only at
configure time from immutable raw GitHub URLs, or from the optional
`MDKR_BASISU_LOCAL_CACHE` mirror, and every individual file is checked against
the SHA-256 manifest in `cmake/character_basisu.cmake`.

The fetched transcoder is locally amended by
`cmake/character_basisu_alignment.cmake` after upstream hash verification.
The amendment copies byte-addressed UASTC input blocks into aligned local
storage before decoding the formats shipped by Golden Balloon. It changes
neither the encoded data nor the output-format algorithms. Configure fails
if the amendment no longer matches the pinned source. This is a local change,
not an upstream release or an unmodified upstream binary.

Only the files enumerated in that CMake manifest are compiled. No encoder,
command-line tool, sample asset, or character content is downloaded or shipped.

- Basis Universal transcoder: Copyright 2019–2021 Binomial LLC, Apache-2.0.
  The complete upstream license is [LICENSE.txt](LICENSE.txt), SHA-256
  `c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4`.
- Zstandard single-file decoder: Copyright 2016–present Facebook, Inc., used
  under its BSD license option. The complete upstream license is
  [Zstd-LICENSE.txt](Zstd-LICENSE.txt), SHA-256
  `2c1a7fa704df8f3a606f6fc010b8b5aaebf403f3aeec339a12048f1ba7331a0b`.

These three files must travel with every native package containing the
transcoder.
