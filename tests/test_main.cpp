/***************************************************************************
 * s411_pi parser/loader tests - headless console app (wx base + xml only),
 * run in CI against real S-411 datasets:
 *   fixtures/TDS1/S100_ROOT/...           synthetic Baltic exchange set
 *   fixtures/TDS1.zip                     the same set as a zip archive
 *   fixtures/411DE00EK1250220.GML         real BSH chart (337 features)
 *   fixtures/411US01AK260701.GML          real NWS Alaska chart (crosses 180)
 *   fixtures/411DE00EIS250217.GML         ice report + point stations
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include <wx/init.h>
#include <wx/string.h>

#include <cmath>
#include <cstdio>

#include "S411Data.h"
#include "S411Portrayal.h"

static int g_fail = 0;

static void Check(bool ok, const char *what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) g_fail++;
}

// shoelace area of a single ring
static double RingArea(const std::vector<S411Pos> &r) {
  double a = 0;
  for (size_t i = 0; i < r.size(); i++) {
    const S411Pos &p = r[i], &q = r[(i + 1) % r.size()];
    a += p.lon * q.lat - q.lon * p.lat;
  }
  return std::fabs(a) / 2.0;
}

static double TrisArea(const std::vector<S411Pos> &t) {
  double total = 0;
  for (size_t i = 0; i + 2 < t.size(); i += 3) {
    double a = (t[i + 1].lon - t[i].lon) * (t[i + 2].lat - t[i].lat) -
               (t[i + 2].lon - t[i].lon) * (t[i + 1].lat - t[i].lat);
    total += std::fabs(a) / 2.0;
  }
  return total;
}

int main(int argc, char **argv) {
  wxInitializer init;
  if (!init.IsOk()) {
    std::printf("wx init failed\n");
    return 2;
  }
  wxString fx = argc > 1 ? wxString(argv[1]) : wxString("tests/fixtures");
  wxString err, errors;

  // --- 1: synthetic Baltic chart (single GML from the exchange set) -------
  {
    S411Dataset ds;
    bool ok = LoadS411File(
        fx + "/TDS1/S100_ROOT/S-411/DATASET_FILES/411DE00BAL260301.GML", ds,
        err);
    Check(ok, "TDS1 GML loads");
    Check(ds.features.size() >= 10, "TDS1 has >= 10 features");
    bool conc = false, fast = false, tess = false, areaOk = true;
    for (const auto &f : ds.features) {
      if (f.kind == S411Kind::Area && !f.totalConcentration.IsEmpty())
        conc = true;
      if (f.fastIce) fast = true;
      if (f.kind == S411Kind::Area && !f.tris.empty()) {
        tess = true;
        if (f.rings.size() == 1) {  // hole-free: areas must match exactly
          double ra = RingArea(f.rings[0]), ta = TrisArea(f.tris);
          if (ra > 1e-8 && std::fabs(ta - ra) / ra > 0.05) areaOk = false;
        }
      }
    }
    Check(conc, "TDS1 concentration attribute parsed");
    Check(fast, "TDS1 fast ice detected (formOfIce 09)");
    Check(tess, "TDS1 areas tessellated");
    Check(areaOk, "TDS1 triangle area matches ring area (within 5%)");
  }

  // --- 2: real BSH chart --------------------------------------------------
  {
    S411Dataset ds;
    bool ok = LoadS411File(fx + "/411DE00EK1250220.GML", ds, err);
    Check(ok, "real BSH chart loads");
    Check(ds.features.size() >= 300, "BSH chart has >= 300 features");
  }

  // --- 3: NWS Alaska - crosses the antimeridian ---------------------------
  {
    S411Dataset ds;
    bool ok = LoadS411File(fx + "/411US01AK260701.GML", ds, err);
    Check(ok, "NWS Alaska chart loads");
    bool sane = true, crossing = false;
    for (const auto &f : ds.features) {
      double span = f.lonMax - f.lonMin;
      if (span > 300.0) sane = false;      // world streak = unwrap failed
      if (f.lonMin < 180.0 && f.lonMax > 180.0) crossing = true;
      if (f.lonMin < -180.0 && f.lonMax > -180.0) crossing = true;
    }
    Check(sane, "Alaska: no feature spans > 300 deg (unwrap works)");
    Check(crossing, "Alaska: at least one feature crosses the antimeridian");
  }

  // --- 4: ice report stations (point features) ----------------------------
  {
    S411Dataset ds;
    bool ok = LoadS411File(fx + "/411DE00EIS250217.GML", ds, err);
    Check(ok, "ice report dataset loads");
    size_t pts = 0;
    for (const auto &f : ds.features)
      if (f.kind == S411Kind::Point) pts++;
    Check(pts >= 3, "ice report has >= 3 point stations");
  }

  // --- 5: exchange set via CATALOG.XML ------------------------------------
  {
    std::vector<S411Dataset> sets;
    errors.Clear();
    size_t n = LoadS411ExchangeSet(fx + "/TDS1/S100_ROOT/CATALOG.XML", sets,
                                   errors);
    Check(n >= 1, "exchange set (CATALOG.XML) loads >= 1 dataset");
  }

  // --- 6: exchange set as .zip archive ------------------------------------
  {
    std::vector<S411Dataset> sets;
    errors.Clear();
    size_t n = LoadS411Zip(fx + "/TDS1.zip", sets, errors);
    Check(n >= 1, "zip archive loads >= 1 dataset");
    Check(n >= 1 && !sets.empty() && sets[0].features.size() >= 10,
          "zip dataset has features");
  }

  // --- 7: garbage handling -------------------------------------------------
  {
    S411Dataset ds;
    bool ok = LoadS411File(fx + "/../test_main.cpp", ds, err);
    Check(!ok, "non-XML input is rejected gracefully");
  }

  // --- 7b: Edition 1.x axis orders (jcomm.info/ice) ------------------------
  {
    // BSH-style v1: EPSG:4326 lat/lon - must NOT be swapped (this was the
    // "Baltic ice appears over the Persian Gulf" bug)
    S411Dataset ds;
    bool ok = LoadS411File(fx + "/v1_latlon.gml", ds, err);
    Check(ok, "v1 lat/lon dataset loads");
    bool placed = false, conc = false;
    for (const auto &f : ds.features) {
      if (f.latMin > 62 && f.latMax < 65 && f.lonMin > 20 && f.lonMax < 24)
        placed = true;
      if (!f.totalConcentration.IsEmpty()) conc = true;
    }
    Check(placed, "v1 lat/lon stays in the Baltic (lat 63, lon 22) - not swapped");
    Check(conc, "v1 iceact concentration parsed");

    // AARI/NWS-style v1: lon/lat with 0..360 longitudes - MUST be swapped and
    // normalised (303 E -> -57)
    S411Dataset ds2;
    ok = LoadS411File(fx + "/v1_lonlat.gml", ds2, err);
    Check(ok, "v1 lon/lat (0-360) dataset loads");
    bool placed2 = false;
    for (const auto &f : ds2.features)
      if (f.latMin > -65 && f.latMax < -62 && f.lonMin > -58 && f.lonMax < -54)
        placed2 = true;
    Check(placed2, "v1 lon/lat detected and normalised (lat -64, lon -56)");

    // Ambiguous value ranges (both axes within +/-90, e.g. the Baltic):
    // the declared CRS decides - OGC CRS84 means lon/lat
    S411Dataset ds3;
    ok = LoadS411File(fx + "/v1_lonlat_crs84.gml", ds3, err);
    Check(ok, "v1 lon/lat CRS84 dataset loads");
    bool placed3 = false;
    for (const auto &f : ds3.features)
      if (f.latMin > 62 && f.latMax < 65 && f.lonMin > 20 && f.lonMax < 23)
        placed3 = true;
    Check(placed3, "ambiguous lon/lat decided by srsName CRS84 (lat 63, lon 21)");
  }

  // --- 8: portrayal - display modes, ice class, palettes ------------------
  {
    // concentration classification (S-411 CONC_CLASS)
    Check(S411ConcToken("98") == "ICEFRE", "conc 98 -> ICEFRE (ice free)");
    Check(S411ConcToken("92") == "ICEC10", "conc 92 -> ICEC10 (10/10)");
    Check(S411ConcToken("46") == "ICEC46", "conc 46 -> ICEC46 (4-6/10)");
    Check(S411ConcToken("99") == "ICEUNK", "conc 99 -> ICEUNK (unknown)");

    // fast ice overrides fill except in stage-of-development mode
    Check(S411AreaTokenFor("91", "95", true, S411Mode::Concentration, "") ==
              "ICEFST", "fast ice -> ICEFST in concentration mode");
    Check(S411AreaTokenFor("91", "95", true, S411Mode::StageOfDevelopment,
                           "") == S411SodToken("95"),
          "stage-of-development mode ignores fast-ice override");

    // ice-class traffic light: 7/10 (ct 78) is red without a class, but only
    // amber for an IA Super vessel (amber threshold 10/10)
    Check(S411NavToken("78", "") == "ICEC46",
          "nav: 7/10, no class -> caution");
    Check(S411NavToken("78", "IASuper") == "ICEC13",
          "nav: 7/10, IA Super -> below limit");
    Check(S411NavToken("92", "IASuper") == "ICEC9T",
          "nav: 10/10, IA Super -> hazardous");
    Check(S411NavToken("98", "IA") == "ICEFRE", "nav: ice free -> safe");

    // palettes dim the fill; night keeps line foreground visible
    S411Rgb day = S411TokenRgb("ICEC9T", S411Palette::Day);
    S411Rgb night = S411TokenRgb("ICEC9T", S411Palette::Night);
    Check(day.r == 255 && day.g == 0, "day ICEC9T = pure red");
    Check(night.r < day.r, "night palette dims the fill");
    S411Rgb ln = S411TokenRgb("ICELIN", S411Palette::Night);
    Check(ln.r > 200, "night ICELIN stays light (visible lines)");
  }

  std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL TESTS PASSED",
              g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
