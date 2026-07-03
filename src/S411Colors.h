/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * WMO ice chart portrayal (day palette), ported verbatim from the S-411
 * Edition 2.0.0 single-source model (Annex C portrayal catalogue).
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <wx/colour.h>
#include <wx/pen.h>
#include <wx/string.h>

// fill colour by IOC v5.2 total concentration code (S-411 CONC_CLASS)
inline wxColour S411AreaColour(const wxString &ct, bool fastIce) {
  if (fastIce) return wxColour(150, 150, 150);            // ICEFST
  struct Cls { const char *codes; unsigned char r, g, b; };
  static const Cls CL[] = {
      {" 98 ", 0, 100, 255},                              // ICEFRE
      {" 01 02 ", 150, 200, 255},                         // ICEOPW
      {" 10 12 13 20 23 24 30 34 ", 140, 255, 160},       // ICEC13
      {" 35 40 45 46 50 56 57 60 67 ", 255, 255, 0},      // ICEC46
      {" 68 70 78 79 80 89 ", 255, 125, 7},               // ICEC79
      {" 81 90 91 ", 255, 0, 0},                          // ICEC9T
      {" 92 ", 145, 0, 0},                                // ICEC10 (Ed 1.1.0)
  };
  if (!ct.IsEmpty()) {
    wxString probe = " " + ct + " ";
    for (const auto &c : CL)
      if (wxString(c.codes).Find(probe) != wxNOT_FOUND)
        return wxColour(c.r, c.g, c.b);
  }
  return wxColour(228, 228, 228);                         // ICEUNK
}

struct S411LineStyle {
  double width;
  wxPenStyle style;      // approximation of the Annex C dash patterns
  unsigned short stipple;  // OpenGL line stipple pattern (0 = solid)
};

// line portrayal per feature type (S-411 LINE_PATTERNS, day colours: black)
inline bool S411LineStyleFor(const wxString &type, S411LineStyle &out) {
  if (type == "IceEdge") out = {2.4, wxPENSTYLE_SOLID, 0};
  else if (type == "IcebergLimit") out = {2.0, wxPENSTYLE_LONG_DASH, 0x0FF0};
  else if (type == "OpenWaterLimit") out = {1.4, wxPENSTYLE_LONG_DASH, 0x0FF0};
  else if (type == "AllKnownIceLimit") out = {2.0, wxPENSTYLE_DOT_DASH, 0x1C47};
  else if (type == "IceRidgeLine") out = {1.6, wxPENSTYLE_DOT_DASH, 0x1C47};
  else if (type == "IceLeadLine") out = {1.6, wxPENSTYLE_LONG_DASH, 0x0FF0};
  else if (type == "IceFractureLine") out = {1.4, wxPENSTYLE_LONG_DASH, 0x0FF0};
  else if (type == "IceCrackLine") out = {1.4, wxPENSTYLE_DOT, 0x3333};
  else if (type == "JammedBrashBarrier") out = {2.8, wxPENSTYLE_SHORT_DASH, 0x0F0F};
  else { out = {1.5, wxPENSTYLE_SOLID, 0}; return false; }
  return true;
}
