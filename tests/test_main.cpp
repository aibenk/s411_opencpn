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

  std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL TESTS PASSED",
              g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
