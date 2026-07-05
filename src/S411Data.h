/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Data model and loaders for S-411 datasets:
 *   - Edition 2.0.0 GML files (IHO S-100, lat/lon)
 *   - Edition 1.x GML files (jcomm.info/ice namespace, lon/lat) - the
 *     format still distributed operationally by several ice services
 *   - S-100 Part 17 exchange sets (pick CATALOG.XML) and .zip archives
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <wx/string.h>

#include <utility>
#include <vector>

struct S411Pos {
  double lat;
  double lon;
};

enum class S411Kind { Point, Line, Area };

struct S411Feature {
  wxString type;                              // Edition 2.0.0 feature name
  S411Kind kind = S411Kind::Area;
  // Longitudes are UNWRAPPED at load time: consecutive vertices never jump
  // more than 180 deg, so values may leave [-180, 180] near the antimeridian.
  // Area: rings[0..] = exterior ring(s) first, then holes (even-odd fill).
  // Line: rings[0] = the line string. Point: rings[0][0] = the position.
  std::vector<std::vector<S411Pos>> rings;
  std::vector<S411Pos> tris;                  // pre-tessellated (areas only)
  wxString totalConcentration;                // IOC v5.2 code, may be empty
  wxString stageOfDevelopment;                // first Sa code, may be empty
  bool fastIce = false;                       // formOfIce 09 (v1: 08/09)
  // every non-geometry leaf element (local name -> text, document order),
  // shown verbatim in the on-click feature report
  std::vector<std::pair<wxString, wxString>> attrs;
  // bounding box (unwrapped lons) for view culling
  double latMin = 90, latMax = -90, lonMin = 999, lonMax = -999;
};

struct S411Dataset {
  wxString title;
  wxString file;
  bool visible = true;                        // per-dataset overlay toggle
  std::vector<S411Feature> features;
  // union of the feature boxes, for zoom-to-dataset
  double latMin = 90, latMax = -90, lonMin = 999, lonMax = -999;
};

/** One picked feature (pointers into the dataset vector passed to
 *  S411HitTest - only valid while that vector is unchanged). */
struct S411Hit {
  const S411Dataset *ds;
  const S411Feature *feature;
};

/** Find the features at (lat, lon) among the VISIBLE datasets: areas by
 *  even-odd point-in-polygon, lines and points within tolDeg degrees.
 *  Results are ordered points first, then lines, then areas. */
size_t S411HitTest(const std::vector<S411Dataset> &data, double lat,
                   double lon, double tolDeg, std::vector<S411Hit> &out);

/** Load one S-411 GML file (Edition 2.0.0 or 1.x). */
bool LoadS411File(const wxString &path, S411Dataset &out, wxString &err);

/** Load every S-411 GML dataset found in a .zip archive (e.g. a downloaded
 *  exchange set). Appends to `out`; returns the number of datasets loaded
 *  and appends per-file problems to `errors`. */
size_t LoadS411Zip(const wxString &zipPath, std::vector<S411Dataset> &out,
                   wxString &errors);

/** Load an unpacked S-100 exchange set: the user picks CATALOG.XML and the
 *  directory tree below it is scanned for *.gml/*.GML dataset files (robust
 *  against catalogue quirks of individual producers). */
size_t LoadS411ExchangeSet(const wxString &catalogPath,
                           std::vector<S411Dataset> &out, wxString &errors);
