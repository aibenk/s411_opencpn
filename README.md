# s411_pi — S-411 Ice Information plugin for OpenCPN

Loads **S-411 "Ice Information"** datasets (IHO S-100 based ice charts,
Edition 2.0.0 GML) into [OpenCPN](https://opencpn.org/) and renders them as a
chart overlay with the WMO ice chart colour code:

- **Concentration fills** (ice free … 10/10, incl. the Edition 1.1.0
  10/10 colour), **fast ice** (form of ice = 09), IcebergArea hatching
- **Ice lines**: ice edge, iceberg/ice limits, ridges, leads, fractures,
  cracks, jammed brash barriers — with the Annex C dash patterns
- **Point features** as markers
- Antimeridian-safe (NWS Alaska charts crossing 180° render correctly)
- Namespace-agnostic GML parser: loads S-411 2.0.0 datasets from any
  producer (BSH, CIS, NIC, MET Norway, AARI, NWS); lat/lon axis-order and
  0–360° longitudes are detected and normalised

The portrayal (colours, line styles, classification rules) is ported verbatim
from the machine-readable model of the
[S-411 Edition 2.0.0 specification project](https://github.com/aibenk/s411).

## Status

**v0.1.0 — early alpha.** Compiles in CI (Linux); not yet exercised inside a
running OpenCPN — expect rough edges. Pick reports, egg-code decoding and the
full Annex C point symbol set are not implemented yet.

## Usage

1. Install the plugin (see build below), enable **S-411 Ice** in
   *Options → Plugins*.
2. Click the **411** toolbar button: a file dialog opens — select one or more
   S-411 GML files (e.g. from `S100_ROOT/S-411/DATASET_FILES/` of an
   exchange set). *Cancel* keeps previously loaded data.
3. The ice overlay is drawn on the chart. Clicking the button again hides it
   (data is kept); activating it once more lets you add further files.

Test data: the S-411 Ed. 2.0.0 project ships eight test datasets (synthetic
Baltic/Greenland plus real BSH, CIS, NIC, MET Norway, AARI and NWS charts).

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
| `src/S411Colors.h` | WMO portrayal: concentration classes, line styles |
| `api/ocpn_plugin.h` | OpenCPN plugin API header (vendored from OpenCPN master) |

## License

GPL v2 or later — the same license as OpenCPN. See [LICENSE](LICENSE).
`api/ocpn_plugin.h` is © the OpenCPN authors, GPL v2+.
