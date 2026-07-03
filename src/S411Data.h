/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Data model and GML parser for S-411 Edition 2.0.0 datasets
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <wx/string.h>

#include <vector>

struct S411Pos {
  double lat;
  double lon;
};

enum class S411Kind { Point, Line, Area };

struct S411Feature {
  wxString type;                              // e.g. "SeaIce", "IceEdge"
  S411Kind kind = S411Kind::Area;
  // Area: rings[0] = exterior, further entries = holes (even-odd fill).
  // Line: rings[0] = the line string. Point: rings[0][0] = the position.
  std::vector<std::vector<S411Pos>> rings;
  wxString totalConcentration;                // IOC v5.2 code, may be empty
  bool fastIce = false;                       // any formOfIce == "09"
  // bounding box for cheap view culling
  double latMin = 90, latMax = -90, lonMin = 180, lonMax = -180;
};

struct S411Dataset {
  wxString title;
  wxString file;
  std::vector<S411Feature> features;
};

/** Parse one S-411 Edition 2.0.0 GML file. Returns false and sets err on
 *  failure; a file without any geographic features also counts as failure. */
bool LoadS411File(const wxString &path, S411Dataset &out, wxString &err);
