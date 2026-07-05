/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Portrayal logic, ported verbatim from the S-411 Edition 2.0.0
 * single-source model (Annex C): colour tokens, day/dusk/night palettes,
 * display modes (concentration / stage of development / ice-class
 * traffic light) and line styles.
 *
 * Deliberately GUI-free (wxString only) so the classification and palette
 * code can be exercised by the headless test binary.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <wx/string.h>

struct S411Rgb {
  unsigned char r, g, b;
};

enum class S411Palette { Day, Dusk, Night };
enum class S411Mode { Concentration, StageOfDevelopment, Navigational };

// day colours per token (S-411 COLOR_TOKENS); dusk = x0.5, night = x0.2,
// except ICELIN which stays visible at night (230,230,230)
inline S411Rgb S411TokenRgb(const wxString &token, S411Palette pal) {
  struct Tok { const char *name; unsigned char r, g, b; };
  static const Tok T[] = {
      {"ICEFRE", 0, 100, 255},   {"ICEOPW", 150, 200, 255},
      {"ICEC13", 140, 255, 160}, {"ICEC46", 255, 255, 0},
      {"ICEC79", 255, 125, 7},   {"ICEC9T", 255, 0, 0},
      {"ICEC10", 145, 0, 0},     {"ICEFST", 150, 150, 150},
      {"ICEUNK", 228, 228, 228}, {"ICES81", 240, 210, 250},
      {"ICES82", 255, 100, 255}, {"ICES83", 170, 40, 240},
      {"ICES84", 135, 60, 215},  {"ICES85", 220, 80, 235},
      {"ICES86", 255, 255, 0},   {"ICES87", 155, 210, 0},
      {"ICES88", 215, 250, 130}, {"ICES89", 175, 250, 0},
      {"ICES91", 0, 200, 20},    {"ICES93", 0, 120, 0},
      {"ICES94", 0, 120, 0},     {"ICES95", 180, 100, 50},
      {"ICES96", 255, 120, 10},  {"ICES97", 200, 0, 0},
      {"ICELIN", 0, 0, 0},       {"ICEBRG", 0, 0, 0},
  };
  S411Rgb rgb = {228, 228, 228};  // ICEUNK fallback
  for (const auto &t : T)
    if (token == t.name) { rgb = {t.r, t.g, t.b}; break; }
  if (token == "ICELIN" && pal == S411Palette::Night)
    return {230, 230, 230};
  const double f = pal == S411Palette::Day ? 1.0
                 : pal == S411Palette::Dusk ? 0.5 : 0.2;
  return {(unsigned char)(rgb.r * f), (unsigned char)(rgb.g * f),
          (unsigned char)(rgb.b * f)};
}

// total concentration code -> colour token (S-411 CONC_CLASS)
inline wxString S411ConcToken(const wxString &ct) {
  struct Cls { const char *codes, *token; };
  static const Cls CL[] = {
      {" 98 ", "ICEFRE"},
      {" 01 02 ", "ICEOPW"},
      {" 10 12 13 20 23 24 30 34 ", "ICEC13"},
      {" 35 40 45 46 50 56 57 60 67 ", "ICEC46"},
      {" 68 70 78 79 80 89 ", "ICEC79"},
      {" 81 90 91 ", "ICEC9T"},
      {" 92 ", "ICEC10"},
  };
  if (!ct.IsEmpty()) {
    wxString probe = " " + ct + " ";
    for (const auto &c : CL)
      if (wxString(c.codes).Find(probe) != wxNOT_FOUND) return c.token;
  }
  return "ICEUNK";
}

// stage of development code -> colour token (S-411 SOD_TOKEN)
inline wxString S411SodToken(const wxString &sod) {
  static const char *codes[] = {"81", "82", "83", "84", "85", "86", "87",
                                "88", "89", "91", "93", "94", "95", "96",
                                "97"};
  for (const char *c : codes)
    if (sod == c) return "ICES" + sod;
  if (sod == "01") return "ICEFRE";
  if (sod == "70" || sod == "80") return "ICEOPW";
  return "ICEUNK";  // incl. "98" and unknown/empty
}

// human-readable code labels for the on-click feature report
// (IOC v5.2 tables 30300/30301 resp. the stage-of-development table)
inline wxString S411ConcLabel(const wxString &ct) {
  struct L { const char *code, *label; };
  static const L T[] = {
      {"98", "ice free"},   {"01", "open water (<1/10)"},
      {"02", "bergy water"},{"10", "1/10"},  {"12", "1-2/10"},
      {"13", "1-3/10"},     {"20", "2/10"},  {"23", "2-3/10"},
      {"24", "2-4/10"},     {"30", "3/10"},  {"34", "3-4/10"},
      {"35", "3-5/10"},     {"40", "4/10"},  {"45", "4-5/10"},
      {"46", "4-6/10"},     {"50", "5/10"},  {"56", "5-6/10"},
      {"57", "5-7/10"},     {"60", "6/10"},  {"67", "6-7/10"},
      {"68", "6-8/10"},     {"70", "7/10"},  {"78", "7-8/10"},
      {"79", "7-9/10"},     {"80", "8/10"},  {"81", "8-10/10"},
      {"89", "8-9/10"},     {"90", "9/10"},  {"91", "9+/10"},
      {"92", "10/10"},      {"99", "unknown"},
  };
  for (const auto &t : T)
    if (ct == t.code) return t.label;
  return "";
}

inline wxString S411SodLabel(const wxString &sod) {
  struct L { const char *code, *label; };
  static const L T[] = {
      {"01", "ice free"},        {"70", "brash ice"},
      {"80", "no stage reported"},{"81", "new ice"},
      {"82", "nilas, ice rind"}, {"83", "young ice"},
      {"84", "grey ice"},        {"85", "grey-white ice"},
      {"86", "first-year ice"},  {"87", "thin first-year ice"},
      {"88", "thin first-year, stage 1"},
      {"89", "thin first-year, stage 2"},
      {"91", "medium first-year ice"},
      {"93", "thick first-year ice"},
      {"94", "residual ice"},    {"95", "old ice"},
      {"96", "second-year ice"}, {"97", "multi-year ice"},
      {"98", "glacier ice"},     {"99", "unknown"},
  };
  for (const auto &t : T)
    if (sod == t.code) return t.label;
  return "";
}

// concentration code -> maximum tenths covered (S-411 ctMaxTenths).
// Two-digit range code "lo hi": upper bound = hi tenths, except hi==0 means
// exactly lo, and 91 (9+/10), 92 (10/10) and the x1 range codes which top
// out at 10 tenths. Values outside the IOC v5.2 code table ("", "9+",
// 93-97, padded text) yield -1 = unknown. Same table as the web viewer,
// rules/main.xsl and tools/validate.py CT_MAX (see docs/DECISIONS.md).
inline int S411CtMaxTenths(const wxString &ct) {
  if (ct == "99" || ct.IsEmpty()) return -1;              // unknown
  if (ct == "98" || ct == "01" || ct == "02") return 0;   // ice free / open
  if (S411ConcToken(ct) == "ICEUNK") return -1;           // not an IOC code
  long n;
  if (!ct.ToLong(&n)) return -1;        // unreachable for the closed enum
  long lo = n / 10, hi = n % 10;
  if (hi == 0) return (int)lo;          // 90 -> 9
  if (lo == 9) return 10;               // 91, 92 -> full cover (98 above)
  if (hi == 1) return 10;               // 81 (8-10/10), the only other x1
  return (int)hi;                        // 46 -> 6
}

// ice-class traffic light (S-411 navToken); iceClass: "", IC, IB, IA,
// IASuper. Green below the class's amber threshold, red from 9/10 for every
// class except IA Super (red only at full cover, 9/10 is amber). Bergy
// water keeps its own token - bergs may be about, it is never "safe".
inline wxString S411NavToken(const wxString &ct, const wxString &iceClass) {
  if (ct == "02") return "ICEOPW";
  const int v = S411CtMaxTenths(ct);
  if (v < 0) return "ICEUNK";
  int amber = 1;  // no ice class: any ice is amber
  if (iceClass == "IASuper") amber = 9;
  else if (iceClass == "IA") amber = 8;
  else if (iceClass == "IB" || iceClass == "IC") amber = 5;
  const int red = iceClass == "IASuper" ? 10 : 9;
  if (v < 1) return "ICEFRE";
  if (v < amber) return "ICEC13";
  if (v < red) return "ICEC46";
  return "ICEC9T";
}

// area fill token for a feature in the given display mode
inline wxString S411AreaTokenFor(const wxString &totalConcentration,
                                 const wxString &stageOfDevelopment,
                                 bool fastIce, S411Mode mode,
                                 const wxString &iceClass) {
  if (fastIce && mode != S411Mode::StageOfDevelopment) return "ICEFST";
  switch (mode) {
    case S411Mode::StageOfDevelopment:
      return S411SodToken(stageOfDevelopment);
    case S411Mode::Navigational:
      return S411NavToken(totalConcentration, iceClass);
    default:
      return S411ConcToken(totalConcentration);
  }
}

// line portrayal per feature type (S-411 LINE_PATTERNS)
enum class S411Dash { Solid, LongDash, ShortDash, Dot, DotDash };
struct S411LineStyle {
  double width;
  S411Dash dash;
  unsigned short stipple;  // OpenGL line stipple pattern (0 = solid)
  const char *token;       // colour token (ICELIN / ICEBRG)
};

inline bool S411LineStyleFor(const wxString &type, S411LineStyle &out) {
  if (type == "IceEdge") out = {2.4, S411Dash::Solid, 0, "ICELIN"};
  else if (type == "IcebergLimit") out = {2.0, S411Dash::LongDash, 0x0FF0, "ICEBRG"};
  else if (type == "OpenWaterLimit") out = {1.4, S411Dash::LongDash, 0x0FF0, "ICELIN"};
  else if (type == "AllKnownIceLimit") out = {2.0, S411Dash::DotDash, 0x1C47, "ICELIN"};
  else if (type == "IceRidgeLine") out = {1.6, S411Dash::DotDash, 0x1C47, "ICELIN"};
  else if (type == "IceLeadLine") out = {1.6, S411Dash::LongDash, 0x0FF0, "ICELIN"};
  else if (type == "IceFractureLine") out = {1.4, S411Dash::LongDash, 0x0FF0, "ICELIN"};
  else if (type == "IceCrackLine") out = {1.4, S411Dash::Dot, 0x3333, "ICELIN"};
  else if (type == "JammedBrashBarrier") out = {2.8, S411Dash::ShortDash, 0x0F0F, "ICELIN"};
  else { out = {1.5, S411Dash::Solid, 0, "ICELIN"}; return false; }
  return true;
}
