# SQLite amalgamation (vendored)

SQLite **3.54.1**, the public-domain amalgamation from
<https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip>, unmodified
(`sqlite3.c`, `sqlite3.h`). SPEC.md §4.3 requires a bundled SQLite ≥ 3.45 so
WAL and `VACUUM INTO` behave the same on every machine, rather than the
distribution's or Qt's copy.

- Zip SHA3-256 (as published on sqlite.org/download.html):
  `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`
- Zip SHA-256: `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`

Compile options are in `CMakeLists.txt` next to this file. To update: download
the new amalgamation zip, check its SHA3-256 against sqlite.org, replace the
two files, update this README and the CHANGELOG.
