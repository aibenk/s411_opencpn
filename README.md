# s411_pi — S-411 Ice Information plugin for OpenCPN

Loads **S-411 "Ice Information"** datasets (IHO S-100 based ice charts,
Edition 2.0.0 GML) into [OpenCPN](https://opencpn.org/) and renders them as a
chart overlay with the WMO ice chart colour code:

- **Concentration fills** (ice free … 10/10, incl. the Edition 1.1.0
  10/10 colour), **fast ice** (form of ice = 09), IcebergArea hatching
- **Ice lines**: ice edge, iceberg/ice limits, ridges, leads, fractures,
  cracks, jammed brash barriers — with the Annex C dash patterns
- **Point features** as markers
- **Switchable display mode** (like the web viewer): ice concentration,
  stage of development, or navigational **ice-class traffic light**
  (green/amber/red by vessel ice class IC…IA Super)
- **Day / Dusk / Night palettes**, adjustable fill opacity, **legend** for
  the active mode in the control panel
- **Click a feature** for its attribute report (all S-411/v1 attributes,
  concentration and stage-of-development codes decoded to plain text)
- **Per-dataset visibility list**, remove single datasets, **zoom to data**
  (the chart also jumps to freshly loaded data automatically)
- Antimeridian-safe (NWS Alaska charts crossing 180° render correctly)
- Namespace-agnostic GML parser: loads S-411 2.0.0 datasets from any
  producer (BSH, CIS, NIC, MET Norway, AARI, NWS); lat/lon axis-order and
  0–360° longitudes are detected and normalised

The portrayal (colours, line styles, classification rules) is ported verbatim
from the machine-readable model of the
[S-411 Edition 2.0.0 specification project](https://github.com/aibenk/s411).

## Exchange sets & supported inputs

The file dialog accepts, in any mix:

| Input | Behaviour |
|---|---|
| `*.GML` / `*.gml` | Single S-411 dataset files |
| `CATALOG.XML` | A complete unpacked S-100 Part 17 exchange set — every dataset below `S100_ROOT` is loaded (robust against producer-specific catalogue quirks) |
| `*.zip` | An exchange-set archive (e.g. as downloaded from an ice service) — all GML datasets inside are loaded without unpacking |

Both **Edition 2.0.0** (IHO S-100) and legacy **Edition 1.x** datasets
(`jcomm.info/ice` namespace, as still distributed operationally, e.g. on the
BSH FTP) are detected automatically, including their different axis order and
attribute names (`iceact`/`iceflz` → concentration/fast ice).

## Status

**v0.4.0 — beta.** The parsing/loading pipeline (GML editions, exchange sets,
zips, antimeridian handling, axis-order detection, tessellation, hit testing)
is exercised in CI against **real operational datasets** (BSH Baltic chart
with 337 features, NWS Alaska chart crossing 180°, ice-report stations, a
complete exchange set as directory and as zip). Pick reports with decoded
codes, a legend, per-dataset visibility and zoom-to-data are built in; the
full Annex C point symbol set is not implemented yet (points render as
generic markers). Concave ice polygons (with holes) are triangulated once at
load time, so panning/zooming does not re-tessellate per frame.

## Usage

1. Install the plugin (see build below), enable **S-411 Ice** in
   *Options → Plugins*.
2. Click the **411** toolbar button: a file dialog opens — select GML files,
   a `CATALOG.XML` or a `.zip` (see table above). *Cancel* keeps previously
   loaded data.
3. The ice overlay is drawn on the chart. Clicking the button again hides it
   (data is kept); activating it once more lets you add further files.

Test data: the S-411 Ed. 2.0.0 project ships eight test datasets (synthetic
Baltic/Greenland plus real BSH, CIS, NIC, MET Norway, AARI and NWS charts);
a subset is included here under `tests/fixtures/`.

**Hands-on testing guide: [docs/TESTING.md](docs/TESTING.md)** (WSL/Linux walkthrough, test checklist with expected results, Windows build notes).

## Tests

```sh
cmake -B build -DS411_BUILD_TESTS=ON
cmake --build build --parallel
./build/s411_test tests/fixtures
```

The test binary is a console app (wx base+xml only, no display needed) and
validates: feature counts and attributes against the real BSH chart,
antimeridian unwrap on the NWS Alaska chart, point stations, exchange-set and
zip loading, graceful rejection of non-XML input, and that the triangle areas
of the tessellation match the ring areas.

## Building

### Linux

```sh
sudo apt install cmake g++ libwxgtk3.2-dev libglu1-mesa-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# install for the current user:
cp build/libs411_pi.so ~/.local/lib/opencpn/   # or /usr/lib/opencpn/
```

### Windows (MSVC)

Plugins must link against the OpenCPN import library and be built with the
same wxWidgets configuration as OpenCPN itself:

```sh
cmake -B build -A Win32 -DOCPN_IMPORT_LIB=path\to\opencpn.lib ^
      -DwxWidgets_ROOT_DIR=path\to\wxWidgets
cmake --build build --config Release
copy build\Release\s411_pi.dll "%LOCALAPPDATA%\opencpn\plugins\"
```

The easiest way to obtain `opencpn.lib` and matching wxWidgets binaries is an
OpenCPN source build or the OpenCPN plugin SDK used by the
[shipdriver template](https://github.com/Rasbats/shipdriver_pi).

### macOS

```sh
brew install wxwidgets cmake
cmake -B build && cmake --build build
```

## Repository layout

| Path | Content |
|---|---|
| `src/s411_pi.*` | Plugin class: toolbar tool, DC + OpenGL overlay rendering (GLU tessellation for concave ice polygons with holes) |
| `src/S411Data.*` | Namespace-agnostic S-411 GML parser (wxXml, no external deps) |
| `src/S411Portrayal.h` | WMO portrayal (GUI-free): colour tokens, day/dusk/night palettes, display modes, ice-class traffic light, line styles |
| `src/S411Tess.*` | Headless GLU polygon triangulation (concave areas with holes) |
| `api/ocpn_plugin.h` | OpenCPN plugin API header (vendored from OpenCPN master) |

## License

GPL v2 or later — the same license as OpenCPN. See [LICENSE](LICENSE).
`api/ocpn_plugin.h` is © the OpenCPN authors, GPL v2+.
