# Testing s411_pi — guide

This guide covers three levels: **(1)** the automated parser tests (no OpenCPN),
**(2)** hands-on testing inside a running OpenCPN on Linux/WSL — the fastest way
to real hands-on use — and **(3)** the native Windows build. Plus a test
checklist with expected results.

---

## 1. Automated tests (no OpenCPN, 2 minutes)

The load pipeline (GML parser, exchange sets, zips, antimeridian, triangulation)
is testable headlessly — the same tests run on every push in GitHub CI.

**Linux / WSL:**

```sh
sudo apt install cmake g++ libwxgtk3.2-dev libglu1-mesa-dev
git clone https://github.com/aibenk/s411_opencpn
cd s411_opencpn
cmake -B build -DCMAKE_BUILD_TYPE=Release -DS411_BUILD_TESTS=ON
cmake --build build --parallel
./build/s411_test tests/fixtures
```

**Expected:** `ALL TESTS PASSED (0 failures)` — 53 checks against real data:
a BSH Baltic chart with 337 features, NWS Alaska across 180°, ice-report
stations, an exchange set as a directory and as a zip, **axis detection**
(lat/lon vs lon/lat, 0–360° longitudes, a CRS84 `srsName` for the ambiguous
Baltic case), the hit test for feature picking, attribute capture, code labels
and the traffic-light semantics (the same tables as the web viewer and the
normative `rules/main.xsl` in the S-411 specification project).

**Windows without CMake:** `testbuild\s411_test.exe tests\fixtures` (kept in
step by the build steps in this repo).

---

## 2. Hands-on in OpenCPN (recommended: Ubuntu in WSL2)

Windows 11 with WSL2 has built-in GUI support (WSLg) — OpenCPN runs in it as a
normal window. This avoids the tedious native Windows build.

### 2.1 Setup (once)

```powershell
wsl --install -d Ubuntu-24.04     # if you have no WSL yet; then reboot
```

In the Ubuntu shell:

```sh
sudo apt update
sudo apt install -y opencpn cmake g++ libwxgtk3.2-dev libglu1-mesa-dev git
```

> If your package source has no OpenCPN: `sudo add-apt-repository ppa:opencpn/opencpn && sudo apt update && sudo apt install opencpn`

### 2.2 Build and install the plugin

```sh
git clone https://github.com/aibenk/s411_opencpn
cd s411_opencpn
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# find the OpenCPN installation's plugin directory:
ls -d /usr/lib/opencpn /usr/lib/*/opencpn 2>/dev/null
sudo cp build/libs411_pi.so /usr/lib/opencpn/        # adjust the path if needed
```

> **Important:** the plugin must be built against the same wxWidgets version as
> OpenCPN itself. With "apt OpenCPN + apt wxWidgets" this matches automatically.
> If OpenCPN does not load the plugin (it stays invisible in the plugin list),
> start `opencpn` once from the terminal and read the messages.

### 2.3 Enable and use

1. Start OpenCPN (`opencpn`).
2. *Options → Plugins* → tick **S-411 Ice** → *OK*.
3. A **411** button appears in the toolbar. Clicking it opens only the **control
   dialog** (**nothing is loaded automatically**) — like the left panel in the
   web viewer:
   - **Show ice overlay** – on/off (data stays loaded)
   - **Load data… / Clear** – add datasets / remove all. After loading, the
     **chart jumps automatically** to the new data.
   - **Dataset list** – checkbox = per-dataset visibility; *Zoom to data*
     (selected dataset, or all visible ones if none selected); *Remove* removes
     the selected one individually
   - **Display mode** – *Ice concentration* / *Stage of development* /
     *Navigational (ice-class traffic light)*
   - **Vessel ice class** – active only in navigational mode (No class …
     IA Super)
   - **Colour palette** – *Day* / *Dusk* / *Night*
   - **Fill opacity** – opacity of the area fills
   - **Legend** – adapts to mode and palette
   Mode, ice class, palette and opacity are saved and restored on the next
   start. The same dialog is available under *Options → Plugins → S-411 Ice →
   Preferences*.
   On the **very first open** a **guided tour** starts automatically (dimmed
   panel, a yellow-outlined focus on one control at a time, card with
   *← Back / Next → / Skip*). Reopen it anytime with the **?** button at the top
   right of the panel. The "seen" flag is stored in the OpenCPN config
   (`/PlugIns/S411/TutorialSeen`).
4. **Feature report:** a short click (no drag, < 4 px mouse movement) on an ice
   feature opens a window with all attributes; concentration and
   stage-of-development codes are decoded to plain language
   (e.g. `totalConcentration = 92 (10/10)`). Panning/zooming is unaffected; a
   click on open sea does nothing.

### 2.4 Getting test data

Two datasets ship as fixtures directly in this repo (`tests/fixtures/`). A full
set of S-411 test datasets lives in the S-411 specification project
([github.com/aibenk/s411](https://github.com/aibenk/s411)) under
`S-411_Ed_2.0.0/Annex_G_TestData/` — the plugin can load those zips **directly**,
without unpacking. A useful selection:

| Dataset | Exercises |
|---|---|
| `TDS4_RealData_Baltic.zip` | real BSH chart, 337 features, all colours |
| `TDS8_RealData_International.zip` | 5 producers, incl. **NWS Alaska** (antimeridian!) |
| `TDS1_BalticSea_IceChart.zip` | small synthetic set (fast smoke test) |
| `TDS6_IceReport_GermanCoast.zip` | point features (ice-report stations) |

For an **Edition 1.x test**: fetch a current winter set from the BSH FTP —
`ftp://ftp.bsh.de/outgoing/eisbericht/S411/Winter2025/` (anonymous) — and feed
the zip to the plugin unchanged.

---

## 3. Test checklist (with expected results)

The 411 button opens the control dialog; loading happens only via *Load data…*
there. First the most important test — **georeferencing** (the historical bug
"Baltic ice in the Indian Ocean" was a forced lon/lat interpretation of
Edition 1.x files):

| # | Test | Expected |
|---|---|---|
| G1 | Load a BSH winter zip (from the S-411 project's `reference/real_data/`, or fresh from the BSH FTP) | chart jumps to the Baltic; ice sits in the Bothnian/Gulf of Finland (~63°N/21°E) — **not** the Persian Gulf / Indian Ocean |
| G2 | Load `tests/fixtures/v1_lonlat_crs84.gml` | area in the Bothnian Bay (63–64°N, 21–22°E) — a lon/lat file recognisable **only** by the CRS84 `srsName` (both axes ≤ 90°) |
| G3 | Load `tests/fixtures/v1_lonlat.gml` | area in Antarctica (~64°S, 56°W) — lon/lat with 0–360° longitudes, detected and normalised |
| G4 | Load an NWS Alaska chart (TDS8) | Alaska / Bering Sea, areas **contiguous** across the date line |

Then the functional checklist:

| # | Test | Expected |
|---|---|---|
| 1 | 411 button → pick `TDS1_...zip` | Baltic ice chart appears as an overlay: concentration colours (blue = ice free … dark red = 10/10), **fast ice grey**, black outlines |
| 2 | Compare colours with the web viewer (same dataset) | identical area colours (same colour table) |
| 3 | Button off → on again → *Cancel* in the dialog | overlay disappears / reappears without reloading |
| 4 | Load `TDS4_RealData_Baltic.zip`, zoom/pan into the Baltic | smooth pan/zoom despite 337 features (triangulation happens only on load) |
| 5 | Load `TDS8_...zip`, navigate to the **Bering Strait / Alaska** | ice areas sit correctly on both sides of the date line — **no** globe-spanning bands |
| 6 | Load `TDS6_...zip`, German Baltic coast | black triangular markers at the station positions |
| 7 | Test an unpacked exchange set: unzip it, pick the **CATALOG.XML** in the dialog | same effect as loading the zip |
| 8 | Pick a single `.GML` from `DATASET_FILES/` | loads as well |
| 9 | **Edition 1.x**: load a BSH FTP winter zip directly | chart appears with correct colours (iceact→concentration, fast ice detected) |
| 10 | Error case: pick any non-XML file | warning, no crash |
| 11 | Toggle *Options → Display → Advanced → OpenGL*, repeat tests 1+4 | both render paths (GL and non-GL) show the same picture; in non-GL mode fills are semi-transparent too |
| 12 | Split screen (second chart window) | overlay appears in both canvases |
| 13 | Control dialog → **Display mode** to *Stage of development* | areas colour by stage of development (purples/greens) instead of concentration; fast ice follows SOD in this mode, not grey |
| 14 | **Display mode** *Navigational* + step through **Vessel ice class** | traffic light per the reference table below; a higher ice class shifts the boundary (more green). 10/10 ice stays red for every class |
| 15 | Step through **Colour palette** *Dusk* / *Night* | fills dimmed; lines light/visible in Night mode; legend dims with it |
| 16 | Compare with the web viewer (same dataset, same mode/palette/ice class) | identical portrayal — plugin, viewer and the normative `rules/main.xsl` use the same tables (CI parity checks in both repos) |
| 17 | Click an ice area (no drag) | feature report: feature type, dataset, all attributes; `totalConcentration`/`stageOfDevelopment` with plain-language labels; click on open sea: nothing; panning unaffected |
| 18 | Load a second dataset, toggle the list checkboxes | only the ticked datasets are drawn; *Remove* removes one; *Zoom to data* jumps to the selected one or to all visible ones |
| 19 | Slide **Fill opacity**, restart OpenCPN | opacity changes live and is kept after a restart |
| 20 | Robustness: copy `tests/fixtures/v1_latlon.gml`, set the `iceact` value to `9+` or empty, load | area **grey "Unknown"** in nav and concentration mode — not red, not green "ice free" |

**Navigational traffic-light reference** (grading the maximum coverage in
tenths; the decision is documented in the S-411 project, `docs/DECISIONS.md` #17):

| ct | no class | IB / IC (PC6–7) | IA (PC4–5) | IA Super (PC1–3) |
|---|---|---|---|---|
| 46 (4–6/10) | amber | amber | **green** | green |
| 80 (8/10) | amber | amber | amber | **green** |
| 90 (9/10) | red | red | red | **amber** |
| 92 (10/10) | red | red | red | red |
| 02 (bergy water) | light blue *Open water / bergy water* everywhere — never "Safe (ice free)" | | | |

**Ice routing (v0.5.0):**

| # | Test | Expected |
|---|---|---|
| 21 | Load a dataset; in the panel, **Start → Set…**, click the chart; then **End → Set…**, click | the two positions appear as coordinates in the panel |
| 22 | Pick a **Vessel ice class**, **Compute ice route** | a route appears in OpenCPN (route manager) that goes around red (hazardous) ice; the status line reports distance, miles through caution ice and the number of legs |
| 23 | Change the ice class from *No class* to *IA Super*, Compute again | the route may cross more ice (higher class → more green/amber passable) |
| 24 | Set the start or destination in the middle of dense ice, Compute | the router lets the track leave the start cell; if the **destination** is entirely in red ice, a clear error message |
| 25 | Channel completely sealed by ice, Compute | message "No route: …" instead of a crash or a route straight through the ice |
| 26 | Tick **Avoid caution ice too**, Compute | the route also avoids amber areas (or reports that no open passage remains) |
| 27 | *(only with ocpn_draw_pi installed)* **Export no-go zones** | message "Exported N …"; red exclusion boundaries appear in ocpn_draw around the hazardous ice; re-exporting replaces only its own, the user's own ocpn_draw boundaries stay |
| 28 | Then Weather Routing: *Configuration → Detect Boundary* on, compute a route | the weather route avoids the exported ice zones |
| 29 | *Export no-go zones* **without** ocpn_draw_pi | a note that ocpn_draw_pi is missing — no crash |

The router works on the **visible** datasets and the **vessel ice class**
selected above; it uses the same traffic-light logic as the display. Since
v0.6.0 the search corridor is automatically widened (×2/×4) on "no route in the
corridor", and recomputing **replaces** the previous ice route instead of
stacking it.

**Known limits (v0.6.0, not bugs):** point features are drawn as generic
triangles rather than Annex C symbols, line dash patterns are approximations,
and on a split screen the feature report uses the last-drawn canvas viewport.
The router searches a corridor around start/destination (no global routing
across large ocean basins); start/destination above 85° are rejected. **10/10
ice (and fast ice) is impassable for every ice class** — deliberately identical
to the traffic-light display; in deep winter "no route" can therefore be the
correct answer. (A permeable mode for the highest ice classes with a heavy
penalty would be a possible future option.)

---

## 4. Native Windows build (one command)

Prerequisites: Visual Studio 2022 with the C++ workload, OpenCPN (Windows build,
32-bit) installed. Then, in the repo:

```powershell
powershell -ExecutionPolicy Bypass -File buildwin.ps1 -Package
```

On the first run the script downloads the matching wxWidgets 3.2.9-vc14x SDK
(~35 MB), builds the OpenCPN import library **directly from the installed
`opencpn.exe`** (dumpbin → .def → lib — no separate SDK needed), compiles
Win32/`/MD` against the same wx DLLs OpenCPN ships, and builds

```
dist\s411_pi-<version>-msvc-wx32-win32.tar.gz
```

— an import package in the OpenCPN plugin-manager format (metadata.xml with
`target: msvc-wx32`, DLL under `plugins/`).

**Install via the plugin manager:** OpenCPN → *Options → Plugins* →
**Import plugin…** → pick the `.tar.gz` from `dist\` → enable **S-411 Ice** in
the list.

**Uninstall:** OpenCPN → *Options → Plugins* → click **S-411 Ice** →
**Uninstall**. The plugin never loads data on its own (not even at start);
disabling or uninstalling removes the overlay immediately and frees all memory.
If anything is left behind (e.g. the DLL was locked because OpenCPN was
running): close OpenCPN and run
`powershell -ExecutionPolicy Bypass -File packaging\uninstall.ps1` — it removes
the DLL and the registration cleanly. **Before importing a new version**,
uninstall the old one this way.

Alternatively, `buildwin.ps1 -Install` copies the DLL straight to
`C:\Program Files (x86)\OpenCPN\plugins\` (UAC) — but then **not both**: either
Import OR direct copy, otherwise the plugin is found twice.

After an OpenCPN **update**, delete `sdk\opencpn.lib` and re-run the script (the
import library must match the new exe).

---

## 5. Reporting findings

Please note, per finding: the test number from the checklist, the dataset,
OpenGL on/off, a screenshot. For load problems, also the message from the
warning dialog — it contains the file and cause for each failed file.
