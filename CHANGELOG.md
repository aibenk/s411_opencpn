# Changelog

The version is kept in `src/s411_version.h` (single source); `CMakeLists.txt`
reads it, `packaging/pack.py` refuses to package when `packaging/metadata.xml`
disagrees, and the plugin reports it to OpenCPN and in its panel title.

## 0.8.1 — 2026-11-02

- **Fix: OpenCPN crashed when loading any data on Windows.** OpenCPN ships the
  VS 2017 C++ runtime (`msvcp140.dll` 14.12); the background loader's
  `std::mutex` (compiled with VS 2022) crashes in it. The loader now uses
  `wxThread`/`wxMutex`.
- Fix: "…", "←/→" and "°" showed as `â€¦` etc. in the panel, the tour and the
  reports.
- WMO **egg code** drawn in the pick report (Edition 2.0.0, 1.x and SIGRID-3).
- Zips inside zips and gzip files (`.gml.gz`, `.zip.gz`) load; archives are
  recognised by content.
- Versioning: plugin API 1.17 so OpenCPN shows major.minor.patch; version in
  the panel title.

## 0.8.0 — 2026-11-02

Implements the v0.7.0 test-report backlog (see `IMPROVEMENTS.md`): tiled
tessellation and background loading (NIC Arctic 63 s → 1.4 s), pole-enclosing
areas, Annex C symbols and value texts, iceberg-area pattern, route ice check,
data age and validity, dataset precedence, S-100 signature verification,
current charts from the Ice Portal, SIGRID-3 and polar stereographic input,
OpenGL vertex-array path, golden-portrayal tests, Windows CI.

## 0.7.0

Guided tour with coach marks (first run and "?" button).
