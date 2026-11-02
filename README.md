# s411_pi — S-411 Ice Information plugin for OpenCPN

Loads **S-411 "Ice Information"** datasets (IHO S-100 based ice charts,
Edition 2.0.0 GML) into [OpenCPN](https://opencpn.org/) and renders them as a
chart overlay with the WMO ice chart colour code:

- **Concentration fills** (ice free … 10/10, incl. the Edition 1.1.0
  10/10 colour), **fast ice** (form of ice = 09)
- **Ice lines**: ice edge, iceberg/ice limits, ridges, leads, fractures,
  cracks, jammed brash barriers — with the Annex C dash patterns
- **Annex C point symbols** (all 37: icebergs by size, drift arrows by
  direction, compacting, divergence, shear, ridges, keels, rafting, melt,
  snow cover, grounded hummocks, report stations …), the **iceberg-area
  symbol pattern**, and the **values** next to the symbols (mean ice
  thickness in cm, drift speed in kn; switchable)
- **Switchable display mode** (like the web viewer): ice concentration,
  stage of development, or navigational **ice-class traffic light**
  (green/amber/red by vessel ice class IC…IA Super)
- **Day / Dusk / Night palettes**, adjustable fill opacity, **legend** for
  the active mode in the control panel
- **Age of the ice information** always visible on the chart ("Ice analysis
  20 Feb 12:00 UTC — 18 h old"), mariner **age limit** (default 72 h),
  "outdated" indication after the validity end, data older than 30 days
  hidden by default (PS clause 8.1)
- **Overlapping datasets** handled per PS clause 4.6: a newer analysis of
  the same producer supersedes the older one inside its coverage, forecasts
  are hidden until selected, overlapping producers are never merged and are
  indicated; the most recent analysis is drawn on top
- **Digital signatures** (S-100 Part 15, ECDSA-384-SHA2) of exchange sets
  are verified; per dataset the list shows *signed / unsigned / SIGNATURE
  INVALID*, and data that fails verification is not displayed
- **Click a feature** for its attribute report (all S-411/v1 attributes,
  codes decoded to plain text, producer, analysis time, age, signature)
- **Per-dataset visibility list**, remove single datasets, **zoom to data**
  (the chart also jumps to freshly loaded data automatically)
- **Current charts with one click**: the newest chart of every ice service
  (BSH Ice Portal: CIS, NIC, NWS, DMI, MET Norway, AARI, SHN …) is
  downloaded and loaded
- Loading runs **in the background** with a progress bar; even the largest
  operational charts (NIC Arctic: 1 650 holes, SHN: 3 million vertices) load
  in seconds without blocking OpenCPN
- Antimeridian-safe, and **pole-enclosing ice areas** (Arctic/Antarctic
  extent charts) are drawn correctly
- Namespace-agnostic GML parser: loads S-411 2.0.0 and 1.x datasets from any
  producer; lat/lon axis order, 0–360° longitudes and **polar stereographic
  coordinates** (EPSG:3031, 3976, 3413, 3995) are detected and converted
- **SIGRID-3 shapefiles** (WMO ice chart exchange format) can be loaded as
  well — geographic or polar stereographic

The portrayal (colours, line styles, symbols, alerts, classification rules)
is ported verbatim from the machine-readable model of the
[S-411 Edition 2.0.0 specification project](https://github.com/aibenk/s411)
and checked in CI against its golden display lists.

## Ice-aware routing

OpenCPN's Weather Routing plugin has **no sea-ice awareness** (it only knows
wind/current and hard exclusion boundaries), so s411_pi provides routing three
ways:

- **Route ice check** — *Check active route* walks the legs of OpenCPN's
  active (or selected) route against the displayed ice and lists, per leg, the
  S-411 alerts of PS clause 9.6 for the selected vessel ice class:
  `IceHazardForVessel` (red ice, fast ice), `IceDifficultForVessel` (amber or
  unknown ice) and `Icebergs` (iceberg areas, iceberg limits, bergs within
  1 NM). The positions are marked on the chart.
- **Built-in ice router** — set a start and end position by clicking the chart,
  then *Compute ice route*. An A\* search over a cost raster derived from the
  loaded charts finds a path that treats **hazardous ice as impassable** and
  **caution ice as a penalty** *for the selected vessel ice class*, avoids
  land (via OpenCPN's GSHHS coastline), and is added straight to OpenCPN's
  route manager. Optionally treat caution ice as impassable too. Distance and
  the miles spent in caution ice are reported.
- **Export ice no-go zones** — with the [ocpn_draw_pi](https://github.com/jongough/ocpn_draw_pi)
  plugin installed, *Export no-go zones* turns the hazardous (and optionally
  caution) ice into **Exclusion boundaries**. The **Weather Routing** plugin's
  *Detect Boundary* option then avoids them, so weather routing becomes
  ice-aware. Re-export is idempotent (only the plugin's own boundaries are
  replaced; user boundaries are never touched).

## Exchange sets & supported inputs

The file dialog accepts, in any mix:

| Input | Behaviour |
|---|---|
| `*.GML` / `*.gml` | Single S-411 dataset files (a `CATALOG.XML` up to four directories above is used for dates and signatures) |
| `CATALOG.XML` | A complete unpacked S-100 Part 17 exchange set — every dataset below `S100_ROOT` is loaded and checked against the catalogue signatures |
| `*.zip` | An exchange-set archive (e.g. as downloaded from an ice service) — all GML datasets (and SIGRID-3 shapefiles) inside are loaded without unpacking |
| zip in zip, `*.gz` | Zips inside a zip (e.g. a season of daily exchange sets) are opened recursively; gzip-compressed files (`.gml.gz`, `.zip.gz`, also as zip entries) are unpacked in memory. Archives are recognised by their content, not by the extension |
| `*.shp` | A SIGRID-3 shapefile (with its `.dbf`, optional `.prj`) |

Both **Edition 2.0.0** (IHO S-100) and legacy **Edition 1.x** datasets
(`jcomm.info/ice` namespace, as still distributed operationally, e.g. on the
BSH FTP) are detected automatically, including their different axis order and
attribute names (`iceact`/`iceflz` → concentration/fast ice). For 1.x data,
which carries no analysis time, the date of the exchange catalogue or the file
name is used and marked as approximate.

## Status

**v0.8.0 — beta.** Implements the improvement backlog of the v0.7.0 test
report ([IMPROVEMENTS.md](IMPROVEMENTS.md)). The parsing/loading pipeline is
exercised in CI (Linux and Windows) against **real operational datasets**
(BSH Baltic, NWS Alaska across 180°, NIC Arctic around the pole, ice-report
stations, signed exchange sets as directory and zip) and against the golden
portrayal of the specification (area colours, alerts, point symbols and value
texts of four datasets in four display settings). The interactive parts
(GUI, rendering, routing) are covered by the checklist in
[docs/TESTING.md](docs/TESTING.md).

## Usage

1. Install the plugin (see below), enable **S-411 Ice** in
   *Options → Plugins*.
2. Click the **411** toolbar button: the control panel opens (a short guided
   tour runs the first time; **?** shows it again).
3. *Load data…* (files, `CATALOG.XML`, zips, shapefiles) or *Current charts…*
   (download from the ice services). The chart jumps to the data once it has
   loaded.

Test data: the S-411 Ed. 2.0.0 project ships eight test datasets (synthetic
Baltic/Greenland plus real BSH, CIS, NIC, MET Norway, AARI and NWS charts);
a subset is included here under `tests/fixtures/`. Note that test data older
than 30 days loads hidden (PS clause 8.1) — tick it in the dataset list.

**Hands-on testing guide: [docs/TESTING.md](docs/TESTING.md)** (WSL/Linux walkthrough, test checklist with expected results, Windows build notes).

## Installing and updating on Windows

```powershell
powershell -ExecutionPolicy Bypass -File buildwin.ps1 -Package
```

builds `dist\s411_pi-<version>-msvc-wx32-win32.tar.gz`; import it with
*Options → Plugins → Import plugin…*.

**Updating:** OpenCPN locks the plugin DLL while it runs, so importing a new
version into a running OpenCPN fails (`opencpn.log`: *"Can't unlink
already-existing object"*). Close OpenCPN, run
`powershell -ExecutionPolicy Bypass -File packaging\hotswap.ps1` and start
OpenCPN again — or uninstall, restart OpenCPN, then import.

## Tests

```sh
cmake -B build -DS411_BUILD_TESTS=ON
cmake --build build --parallel
./build/s411_test tests/fixtures
```

or on Windows `buildwin.ps1 -TestOnly`. The test binary is a console app (wx
base+xml only, no display needed). See [docs/TESTING.md](docs/TESTING.md)
section 1 for what it covers.

## Building

### Linux

```sh
sudo apt install cmake g++ libwxgtk3.2-dev libglu1-mesa-dev libssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# install for the current user:
cp build/libs411_pi.so ~/.local/lib/opencpn/   # or /usr/lib/opencpn/
```

OpenSSL is optional; without it signatures are shown as "not verified".

### Windows (MSVC)

`buildwin.ps1` (above) is the simplest route: it needs only Visual Studio 2022
and an installed OpenCPN, downloads the matching wxWidgets SDK and generates
the OpenCPN import library from the installed `opencpn.exe`. With CMake:

```sh
cmake -B build -A Win32 -DOCPN_IMPORT_LIB=path\to\opencpn.lib ^
      -DwxWidgets_ROOT_DIR=path\to\wxWidgets
cmake --build build --config Release
```

### macOS

```sh
brew install wxwidgets cmake openssl
cmake -B build && cmake --build build
```

### Releases

`packaging/metadata.xml` points `tarball-url` at the GitHub release asset
`v<version>/s411_pi-<version>-msvc-wx32-win32.tar.gz`. To publish: build with
`buildwin.ps1 -Package`, then `gh release create v0.8.0 dist/*.tar.gz` (plus
the Linux `libs411_pi.so` artifact from CI). The repository must be public for
the OpenCPN plugin catalogue to fetch it.

## Repository layout

| Path | Content |
|---|---|
| `src/s411_pi.*` | Plugin class: control panel, background loading, DC + OpenGL overlay rendering, symbols/text, route check, chart download |
| `src/S411Data.*` | Namespace-agnostic S-411 GML parser (wxXml), exchange sets/zips, polar stereographic CRSs, precedence and data age |
| `src/S411Catalog.*` | Exchange catalogue reader and S-100 Part 15 signature verification (Windows CNG / OpenSSL) |
| `src/S411Sigrid.cpp` | SIGRID-3 shapefile reader |
| `src/S411Portrayal.h` | WMO portrayal (GUI-free): colour tokens, palettes, display modes, ice-class traffic light, line styles, point symbols, alerts |
| `src/S411Symbols.h` | The 37 Annex C symbols as vector primitives (generated by `tools/gen_symbols.py`) |
| `src/S411Tess.*` | Headless GLU triangulation, tiled for very large polygons |
| `src/S411Router.*`, `src/S411Boundary.*` | Ice router, route ice check, ocpn_draw boundary export |
| `api/ocpn_plugin.h` | OpenCPN plugin API header (vendored from OpenCPN master) |

## License

GPL v2 or later — the same license as OpenCPN. See [LICENSE](LICENSE).
`api/ocpn_plugin.h` is © the OpenCPN authors, GPL v2+. The Annex C symbol
designs in `src/S411Symbols.h` come from the S-411 Edition 2.0.0
specification (CC BY 4.0). Some test datasets in `tests/fixtures/` are real
data of the ice services, included for testing only - see
[DATA_NOTICE.md](DATA_NOTICE.md).
