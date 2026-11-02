# s411_pi — test report and improvement backlog

Status of **v0.7.0** (commit `50c5317`), tested on 2026-11-02 on Windows 11 with
OpenCPN 5.14.0 (wxWidgets 3.2.9, Win32 build, Visual Studio 2022).

## 1 What was tested

| Test | Result |
|---|---|
| Windows build (`buildwin.ps1`, MSVC 2022, wx 3.2.9) | **OK**: `s411_pi.dll` builds without errors |
| Headless test suite (`tests/test_main.cpp` against `tests/fixtures`, built fresh with MSVC) | **OK**: 95 of 95 checks pass |
| Plugin load in OpenCPN 5.14 (`opencpn.log`) | **OK**: compatible wx library found, plugin loaded and initialised |
| Loading the **new signed S-100 5.2.0 exchange sets** of the specification (13 × `CATALOG.XML`, Annex G TDS1–TDS8) | **OK**: all 13 load |
| Loading the portal's exchange-set **zips** (TDS1–TDS8) | **OK**: all load, TDS7 = 3 and TDS8 = 5 datasets |
| Loading **today's charts of the ice services**: 22 charts converted to Edition 2.0.0 (CIS, DMI, MET Norway, AARI, NIC, NWS) + 10 original Edition 1.x zips from bsis-ice.de | **OK with findings**: all load, see 2.1 and 2.2 |
| Area colours vs. the **golden display lists** of the specification (`Annex_G_TestData/Expected_Portrayal`, 10 datasets × 4 settings: concentration, stage of development, navigational IC and IA Super) | **OK**: 40 of 40 match exactly |
| GitHub CI (Linux build + tests) | **OK**: last run green |

Not tested: interactive use in the OpenCPN GUI (clicking, routing, ocpn_draw
export). These need a person at the screen; the checklist is in
`docs/TESTING.md`, section 4.

## 2 Must fix (bugs)

### 2.1 NIC Arctic charts freeze OpenCPN for up to a minute

Loading the U.S. National Ice Center Arctic charts takes **25–56 s** per chart
(e.g. `S411_NATICE_ArcticExtent_*`: 32 polygons, 3.6 MB GML → 56 s;
`S411_NATICE_arctic*`: 635 polygons → 48–53 s). The Antarctic charts of the same
service load in about 2 s, and most other charts in well under 1 s. Loading runs
on the GUI thread, so OpenCPN freezes for that time.

- Profile `LoadS411File` on these files. Suspects: tessellation of very large
  rings near the pole, unwrapping, or string handling per vertex.
- Load in a worker thread with a progress indicator, so the chart stays usable.
- Add a regression test with a time budget (fixture: one NIC Arctic chart).

### 2.2 Ice areas around the South Pole are not drawn

In the NIC Antarctic chart (`S411_Ant_nic_antarc_*`) one ice area is not
tessellated and is therefore **invisible**. After unwrapping, the dataset spans
longitudes −198° … 257° (455°): the polygon encloses the pole. The antimeridian
handling assumes rings that do not enclose a pole.

- Detect pole-enclosing rings (winding sum of longitude steps = ±360°) and close
  them along the pole latitude (±90°) before tessellating.
- Add a fixture with a circumpolar ring.

### 2.3 Installed plugin is out of date; re-import fails while OpenCPN runs

- The DLL installed in `%LOCALAPPDATA%\opencpn\plugins` is from 2026-07-09
  (before v0.7.0). Install the current build (`buildwin.ps1 -Package`, then
  *Options → Plugins → Import plugin…* with OpenCPN restarted).
- `opencpn.log` shows a failed re-import: *"archive write install header error:
  Can't unlink already-existing object"*. The old DLL is locked while OpenCPN
  runs. Document "uninstall, restart, then import" in the README, or make
  `packaging/hotswap.ps1` the documented route.

### 2.4 Documentation is behind the code

- README "Status" still says **v0.4.0** and "points render as generic markers"
  (true, see 3.1); update it to v0.7.0 and list the guided tour and router
  safety features.
- `packaging/metadata.xml` points `tarball-url` to GitHub releases, but the repo
  has **no releases** and is private, so the metadata cannot be used by the
  OpenCPN plugin catalogue yet (see 4.1).

## 3 Should do (align with S-411 Edition 2.0.0)

The specification's portrayal catalogue was reworked on 2026-11-02 (S-100 5.2.0
conformant, Lua + XSLT, alert catalogue). The plugin's area colours already
match it exactly. These parts are missing:

### 3.1 Point symbols

Points are drawn as one generic triangle. Use the 37 symbols of Annex C
(`Annex_C_PortrayalCatalogue/Symbols/*.svg`): iceberg by `icebergSize`, drift
arrows by `driftDirection`, compacting, divergence, shear, ridges, keels,
rafting, stage of melt, snow cover, grounded hummock, report stations. OpenCPN's
plugin API can render SVG to bitmaps (`GetBitmapFromSVGFile`); recolour per
palette (the SVGs use the CSS classes of `daySvgStyle.css` / `dusk…` /
`night…`).

### 3.2 Text and line details

- Show mean ice thickness (cm) and drift speed (kn) next to the symbol, as in
  PS clause 9.7, switchable.
- Iceberg area: pattern fill with the iceberg symbol (`AreaFills/ICEBRGA1.xml`)
  instead of hatching.

### 3.3 Ice alerts for the active route

The alert catalogue defines `IceHazardForVessel`, `IceDifficultForVessel` and
`Icebergs` (PS clause 9.6). Add *Check active route*: walk the legs of
OpenCPN's active route against the loaded ice and list the legs that cross red
ice, amber ice or iceberg features for the selected ice class. The router
already has the severity raster to do this.

### 3.4 Age and validity of the data

PS clause 8.1 requires the age of the ice information to be visible, an
"outdated" indication after the validity end, a mariner age limit (default
72 h) and hiding data older than 30 days by default. Today the plugin shows
none of this. Add the analysis time and age to the dataset list and the pick
report, and a warning colour for outdated datasets.

### 3.5 Overlapping datasets

PS clause 4.6 rule 5: a newer analysis of the same producer replaces the older
one inside its coverage; forecasts are not shown by default; datasets of
different producers are not merged. Today all loaded datasets are drawn on top
of each other in load order.

### 3.6 Signatures

Exchange sets are now signed (S-100 Part 15, ECDSA-384-SHA2). The plugin
ignores `CATALOG.XML` signatures. At least show "signed / unsigned / signature
invalid" per dataset. The test PKI is in the specification's
`Annex_G_TestData/TEST_PKI`.

## 4 Could do (wider use)

### 4.1 Distribution

- Publish GitHub releases with the Windows tarball (and Linux builds from CI).
- Register in the OpenCPN plugin catalogue (`OpenCPN/plugins` master catalogue)
  so that users can install via *Options → Plugins* without downloading files.
- Add builds for OpenCPN's other targets: Linux flatpak, Debian/Ubuntu,
  macOS, and Windows x64 if OpenCPN ships it for your users.

### 4.2 Current ice charts with one click

The ice services publish their charts daily as S-411 1.x zips on
`https://www.bsis-ice.de/IcePortal/S411/`; the plugin already reads that format.
A *Download current ice charts* dialog (list the newest chart per service,
fetch with `OCPN_downloadFile`, load) would make the plugin self-sufficient. The
specification portal does the same server-side every 3 hours.

### 4.3 More input formats

SIGRID-3 shapefiles (e.g. Argentina's SHN publishes only SIGRID-3) are not
supported. Either add a shapefile reader (polygons + DBF egg-code fields) or
point users to the S-411 converter toolkit.

### 4.4 Rendering performance

Draw tessellated areas from vertex buffers (VBO) instead of immediate mode and
cache screen projections per view, for large charts such as the NIC Arctic
analysis (635 polygons).

### 4.5 Tests

- Add a load-time budget test and a circumpolar ring test (see 2.1, 2.2).
- Add the golden-portrayal check used in this report to CI: for the
  specification's `Expected_Portrayal` datasets, the histogram of area colour
  tokens must equal the golden display lists for all four settings.
- Run the test suite on Windows in CI as well (today only Linux).

## 5 Priorities

1. 2.1 Loading time and worker thread
2. 2.2 Circumpolar polygons
3. 2.3 / 2.4 Install the current build, documentation, first release
4. 3.1 Point symbols, 3.4 data age
5. 3.3 Route ice check
6. Everything else as time allows
