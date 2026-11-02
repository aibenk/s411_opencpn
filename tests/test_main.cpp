/***************************************************************************
 * s411_pi parser/loader tests - headless console app (wx base + xml only),
 * run in CI against real S-411 datasets:
 *   fixtures/TDS1/S100_ROOT/...           synthetic Baltic exchange set
 *   fixtures/TDS1.zip                     the same set as a zip archive
 *   fixtures/411DE00EK1250220.GML         real BSH chart (337 features)
 *   fixtures/411US01AK260701.GML          real NWS Alaska chart (crosses 180)
 *   fixtures/411DE00EIS250217.GML         ice report + point stations
 *   fixtures/S411_NATICE_ArcticExtent_*.zip  real NIC Arctic chart (1.x zip,
 *                                         pole-enclosing, 1652 holes)
 *   fixtures/circumpolar.gml, prec_*.gml, forecast.gml   synthetic
 *   fixtures/expected_portrayal/          Annex G golden display lists
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/init.h>
#include <wx/stopwatch.h>
#include <wx/string.h>
#include <wx/xml/xml.h>

#include <cmath>
#include <cstdio>
#include <map>

#include "S411Boundary.h"
#include "S411Data.h"
#include "S411Portrayal.h"
#include "S411Router.h"
#include "S411Symbols.h"
#include "S411Tess.h"

static int g_fail = 0;

static bool Contains(const wxString &h, const char *n) {
  return h.Find(n) != wxNOT_FOUND;
}

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

  // --- 6a: WMO egg code for the pick report ------------------------------
  {
    S411Dataset bsh, v1;
    LoadS411File(fx + "/411DE00EK1250220.GML", bsh, err);
    size_t eggs = 0;
    bool consistent = true;
    for (const auto &f : bsh.features) {
      if (f.egg.Empty()) continue;
      eggs++;
      if (f.egg.ct != f.totalConcentration || f.egg.cols.size() > 3)
        consistent = false;
      // the first stage of the first column is the area's stage of development
      if (!f.egg.cols.empty() && !f.egg.cols[0].sf.empty() &&
          f.egg.cols[0].sf[0].first != f.stageOfDevelopment)
        consistent = false;
    }
    Check(eggs >= 100, "egg: real BSH chart areas carry egg-code data");
    Check(consistent, "egg: Ct and Sa agree with the parsed attributes");
    LoadS411File(fx + "/v1_latlon.gml", v1, err);
    const S411Egg &e = v1.features[0].egg;
    Check(e.ct == "60" && e.cols.size() == 1 && e.cols[0].sf.size() == 1 &&
              e.cols[0].sf[0].first == "91" && e.cols[0].sf[0].second == "07",
          "egg: Edition 1.x lists iceact/icesod/iceflz -> Ct, Sa, Fa");
    S411Dataset sg;
    S411LoadSigridFile(fx + "/sigrid3_geo_20261020.shp", sg, err);
    Check(!sg.features.empty() && sg.features[0].egg.ct == "92" &&
              sg.features[0].egg.cols.size() == 1 &&
              sg.features[0].egg.cols[0].sf[0].first == "93" &&
              sg.features[0].egg.cols[0].sf[0].second == "04",
          "egg: SIGRID-3 CT/SA/FA -> Ct, Sa, Fa");
  }

  // --- 6b: nested zips and gzip -------------------------------------------
  {
    std::vector<S411Dataset> sets;
    errors.Clear();
    size_t n = S411LoadPath(fx + "/nested.zip", sets, errors);
    Check(n == 2, "zip in zip + .gml.gz entry: both datasets load");
    bool signedInner = false;
    for (const auto &d : sets)
      signedInner |= d.producer == "DE00" && d.signature != S411SigState::None;
    Check(signedInner, "nested exchange-set zip keeps its catalogue/signature");
    sets.clear();
    n = S411LoadPath(fx + "/circumpolar.gml.gz", sets, errors);
    Check(n == 1 && sets[0].features.size() == 4, "gzipped GML loads");
    sets.clear();
    n = S411LoadPath(fx + "/TDS1.zip.gz", sets, errors);
    Check(n == 1 && sets[0].producer == "DE00", "gzipped zip (.zip.gz) loads");
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

    // --- 7c: attributes, hit test, code labels ----------------------------
    Check(!ds3.features.empty() && !ds3.features[0].attrs.empty(),
          "feature attributes captured for the pick report");
    Check(ds3.latMin > 62 && ds3.latMax < 65 && ds3.lonMin > 20 &&
              ds3.lonMax < 23,
          "dataset bounding box aggregated (zoom-to-data)");

    std::vector<S411Dataset> pool = {ds3};
    std::vector<S411Hit> hits;
    Check(S411HitTest(pool, 63.5, 21.5, 0.05, hits) >= 1,
          "hit test: click inside the area finds the feature");
    Check(S411HitTest(pool, 60.0, 21.5, 0.05, hits) == 0,
          "hit test: click far away finds nothing");
    pool[0].visible = false;
    Check(S411HitTest(pool, 63.5, 21.5, 0.05, hits) == 0,
          "hit test: hidden datasets are not picked");

    Check(S411ConcLabel("92") == "10/10", "conc label 92 -> 10/10");
    Check(S411ConcLabel("91") == "9+/10", "conc label 91 -> 9+/10");
    Check(S411SodLabel("95") == "old ice", "SoD label 95 -> old ice");
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

    // ice-class traffic light (docs/DECISIONS.md in the spec repo): green
    // below the class's amber threshold, red from 9/10 for every class
    // except IA Super (amber at 9/10, red only at full cover)
    Check(S411NavToken("78", "") == "ICEC46",
          "nav: 7/10, no class -> caution");
    Check(S411NavToken("78", "IASuper") == "ICEC13",
          "nav: 7/10, IA Super -> below limit");
    Check(S411NavToken("92", "IASuper") == "ICEC9T",
          "nav: 10/10, IA Super -> hazardous");
    Check(S411NavToken("98", "IA") == "ICEFRE", "nav: ice free -> safe");
    Check(S411NavToken("90", "IASuper") == "ICEC46",
          "nav: 9/10, IA Super -> caution (not green, not red)");
    Check(S411NavToken("80", "IASuper") == "ICEC13",
          "nav: 8/10, IA Super -> below limit");
    Check(S411NavToken("79", "") == "ICEC9T",
          "nav: 7-9/10, no class -> hazardous (range reaches 9/10)");
    Check(S411NavToken("02", "IA") == "ICEOPW",
          "nav: bergy water -> own token, never 'safe'");
    Check(S411NavToken("9+", "IA") == "ICEUNK",
          "nav: non-IOC notation '9+' -> unknown");
    Check(S411CtMaxTenths(" 98") == -1,
          "maxTenths: padded code -> unknown, not full cover");
    Check(S411CtMaxTenths("95") == -1,
          "maxTenths: undefined 9x code -> unknown");

    // palettes dim the fill; night keeps line foreground visible
    S411Rgb day = S411TokenRgb("ICEC9T", S411Palette::Day);
    S411Rgb night = S411TokenRgb("ICEC9T", S411Palette::Night);
    Check(day.r == 255 && day.g == 0, "day ICEC9T = pure red");
    Check(night.r < day.r, "night palette dims the fill");
    S411Rgb ln = S411TokenRgb("ICELIN", S411Palette::Night);
    Check(ln.r > 200, "night ICELIN stays light (visible lines)");

    // exact numeric parity with the model's colour profile (round half-to-even,
    // not truncation): these three tripped the old truncation bug
    S411Rgb duskC46 = S411TokenRgb("ICEC46", S411Palette::Dusk);
    Check(duskC46.r == 128 && duskC46.g == 128 && duskC46.b == 0,
          "dusk ICEC46 = 128,128,0 (rounded, not truncated 127)");
    S411Rgb nightUnk = S411TokenRgb("ICEUNK", S411Palette::Night);
    Check(nightUnk.r == 46 && nightUnk.g == 46 && nightUnk.b == 46,
          "night ICEUNK = 46,46,46 (rounded, not truncated 45)");
    S411Rgb duskFre = S411TokenRgb("ICEFRE", S411Palette::Dusk);
    Check(duskFre.r == 0 && duskFre.g == 50 && duskFre.b == 128,
          "dusk ICEFRE = 0,50,128");

    // router default parameters must match the viewer/QGIS ports
    Check(S411RouteParams().maxDim == 200,
          "router default maxDim = 200 (matches viewer/QGIS)");
  }

  // --- 9: ice-aware router -------------------------------------------------
  {
    // helper: build a rectangular area feature with a given concentration
    auto box = [](double la, double lA, double lo, double lO,
                  const char *ct) {
      S411Feature f;
      f.kind = S411Kind::Area;
      f.type = "SeaIce";
      f.totalConcentration = ct;
      f.rings.push_back({{la, lo}, {la, lO}, {lA, lO}, {lA, lo}, {la, lo}});
      f.latMin = la; f.latMax = lA; f.lonMin = lo; f.lonMax = lO;
      return f;
    };
    auto dataset = [](std::vector<S411Feature> feats) {
      std::vector<S411Dataset> d(1);
      d[0].title = "synthetic";
      d[0].visible = true;
      d[0].features = std::move(feats);
      return d;
    };

    S411RouteParams pr;
    pr.iceClass = "";  // no ice class: 9+/10 is hazardous, lower is caution
    S411LandFn noLand;

    // (a) a solid 10/10 barrier across the corridor with a clear gap: the
    //     route must reach the far side and pass THROUGH the gap
    {
      // barrier spans lon 9..11 for lat 0..2 and lat 3..5, gap at lat 2..3
      auto data = dataset({box(0.0, 2.0, 9.0, 11.0, "92"),
                           box(3.0, 5.0, 9.0, 11.0, "92")});
      S411RouteResult r;
      bool ok = S411ComputeIceRoute(data, {2.5, 5.0}, {2.5, 15.0}, pr,
                                    noLand, r);
      Check(ok, "router: finds a path through the ice gap");
      bool clearedFar = !r.waypoints.empty() &&
                        r.waypoints.back().lon > 14.0;
      Check(clearedFar, "router: path reaches the destination side");
      // every waypoint must avoid the 10/10 boxes (blocked)
      bool avoidsBlocked = true;
      for (const auto &w : r.waypoints)
        if (w.lon > 9.2 && w.lon < 10.8 &&
            ((w.lat > 0.2 && w.lat < 1.8) || (w.lat > 3.2 && w.lat < 4.8)))
          avoidsBlocked = false;
      Check(avoidsBlocked, "router: path stays out of 10/10 ice");
    }

    // (a2) a diagonal wall of blocked cells must not be crossed at a corner
    //      (review finding #1: 8-connected corner cutting)
    {
      // a staircase of 10/10 boxes on the anti-diagonal from SW to NE
      std::vector<S411Feature> feats;
      for (int k = 0; k < 8; k++)
        feats.push_back(box(k * 1.0, k * 1.0 + 1.0, 13.0 - k * 1.0,
                            14.0 - k * 1.0, "92"));
      auto data = dataset(feats);
      // start south of the wall, end north of it, on opposite corners
      S411RouteResult r;
      bool ok = S411ComputeIceRoute(data, {0.5, 5.0}, {7.5, 16.0}, pr, noLand,
                                    r);
      // whether or not a path exists, no waypoint segment may pass through a
      // blocked box; sample the polyline densely and check
      bool through = false;
      for (size_t i = 0; ok && i + 1 < r.waypoints.size(); i++)
        for (int s = 0; s <= 40; s++) {
          const double t = s / 40.0;
          const double la = r.waypoints[i].lat +
                            (r.waypoints[i + 1].lat - r.waypoints[i].lat) * t;
          double lo = r.waypoints[i].lon +
                      (r.waypoints[i + 1].lon - r.waypoints[i].lon) * t;
          for (int k = 0; k < 8; k++)
            if (la > k * 1.0 + 0.05 && la < k * 1.0 + 0.95 &&
                lo > 13.0 - k * 1.0 + 0.05 && lo < 14.0 - k * 1.0 - 0.05)
              through = true;
        }
      Check(!through, "router: no diagonal corner-cut through blocked ice");
    }

    // (a3) a detour that only fits in a WIDER corridor: the retry must widen
    //      the margin and still find the way around (review B8)
    {
      // wall lon 9..11 spanning lat -0.4..0.4; a tight initial corridor
      // (margin 0.3) is fully sealed, a x2 retry (0.6) exposes a gap
      S411RouteParams p2 = pr;
      p2.minMarginDeg = 0.3;
      p2.marginFrac = 0.05;
      auto data = dataset({box(-0.4, 0.4, 9.0, 11.0, "92")});
      S411RouteResult r;
      bool ok = S411ComputeIceRoute(data, {0.0, 5.0}, {0.0, 15.0}, p2, noLand,
                                    r);
      Check(ok, "router: widens the corridor to find a detour");
      double maxAbs = 0;
      for (const auto &w : r.waypoints) maxAbs = std::max(maxAbs, std::fabs(w.lat));
      Check(maxAbs > 0.4, "router: retry detour leaves the initial corridor");
    }

    // (b) a barrier that fully seals the corridor -> no route
    {
      auto data = dataset({box(-10.0, 10.0, 9.0, 11.0, "92")});
      S411RouteResult r;
      bool ok = S411ComputeIceRoute(data, {0.0, 5.0}, {0.0, 15.0}, pr,
                                    noLand, r);
      Check(!ok, "router: reports no route when ice seals the corridor");
      Check(!r.error.IsEmpty(), "router: gives a reason when blocked");
    }

    // (c) amber (caution) ice is passable but penalised: a detour of mild
    //     ice is preferred when the penalty is high enough
    {
      // amber 5/10 block straight ahead; open water detour to the north
      S411RouteParams p2 = pr;
      p2.cautionPenalty = 8.0;
      auto data = dataset({box(-0.5, 0.5, 9.0, 11.0, "50")});  // 5/10 amber
      S411RouteResult r;
      bool ok = S411ComputeIceRoute(data, {0.0, 5.0}, {0.0, 15.0}, p2,
                                    noLand, r);
      Check(ok, "router: routes around penalised amber ice");
      double maxAbsLat = 0;
      for (const auto &w : r.waypoints)
        maxAbsLat = std::max(maxAbsLat, std::fabs(w.lat));
      Check(maxAbsLat > 0.4,
            "router: detours clear of the amber block (either side)");
    }

    // (d) avoidCaution makes amber impassable -> if that seals it, no route
    {
      S411RouteParams p3 = pr;
      p3.avoidCaution = true;
      p3.minMarginDeg = 0.3;  // tight corridor so the amber wall really seals
      p3.marginFrac = 0.05;
      auto data = dataset({box(-10.0, 10.0, 9.0, 11.0, "50")});
      S411RouteResult r;
      bool ok = S411ComputeIceRoute(data, {0.0, 9.5}, {0.0, 10.5}, p3,
                                    noLand, r);
      Check(!ok, "router: avoidCaution treats amber as impassable");
    }

    // (e) land avoidance: a stub that blocks a latitude band forces a detour
    {
      auto data = dataset({});  // no ice at all
      S411LandFn land = [](double la1, double lo1, double la2, double lo2) {
        // "land" occupies lat 0.4..0.6 for lon 9..11
        auto hits = [](double la, double lo) {
          return la > 0.4 && la < 0.6 && lo > 9.0 && lo < 11.0;
        };
        return hits(la1, lo1) || hits(la2, lo2);
      };
      S411RouteResult r;
      bool ok = S411ComputeIceRoute(data, {0.5, 5.0}, {0.5, 15.0}, pr, land,
                                    r);
      Check(ok, "router: finds a way around a land obstruction");
    }

    // (f) hazard extraction + ring simplification
    {
      // a 10/10 blob with many vertices on its ring
      S411Feature f;
      f.kind = S411Kind::Area;
      f.type = "SeaIce";
      f.totalConcentration = "92";
      std::vector<S411Pos> circle;
      for (int i = 0; i < 400; i++) {
        const double a = i * 2.0 * 3.14159265 / 400;
        circle.push_back({60.0 + 0.5 * std::sin(a), 20.0 + 0.5 * std::cos(a)});
      }
      f.rings.push_back(circle);
      f.latMin = 59.5; f.latMax = 60.5; f.lonMin = 19.5; f.lonMax = 20.5;
      auto data = dataset({f});
      std::vector<S411HazardZone> zones;
      size_t n = S411ExtractHazards(data, "", false, 60, zones);
      Check(n == 1, "hazard: one blocked zone extracted");
      Check(!zones.empty() && zones[0].ring.size() <= 60,
            "hazard: ring simplified to the vertex cap");
      Check(!zones.empty() && zones[0].ring.size() >= 8,
            "hazard: simplified ring keeps enough shape");
      // caution excluded by default
      std::vector<S411HazardZone> zones2;
      auto amber = dataset({box(0, 1, 0, 1, "50")});
      Check(S411ExtractHazards(amber, "", false, 60, zones2) == 0,
            "hazard: amber excluded unless requested");
      Check(S411ExtractHazards(amber, "", true, 60, zones2) == 1,
            "hazard: amber included when requested");
    }

    // (g) router + hazards on REAL geometry (many small polygons, holes,
    //     antimeridian) - pins the Shift/NormLon path end to end
    {
      std::vector<S411Dataset> baltic(1);
      wxString e;
      if (LoadS411File(fx + "/411DE00EK1250220.GML", baltic[0], e)) {
        std::vector<S411HazardZone> zones;
        size_t nz = S411ExtractHazards(baltic, "", false, 60, zones);
        Check(nz > 0, "real: hazards extracted from the BSH Baltic chart");
        bool ringsOk = true;
        for (const auto &z : zones)
          if (z.ring.size() < 3 || z.ring.size() > 60) ringsOk = false;
          else
            for (const auto &pnt : z.ring)
              if (pnt.lon < -180.5 || pnt.lon > 180.5) ringsOk = false;
        Check(ringsOk, "real: hazard rings within [3,60] pts and in-range lon");
        // route across the aggregate lon range at a mid latitude: must return
        // cleanly (a route with sane stats, or a non-empty 'no route' reason)
        S411RouteParams rp;
        double midLat = (baltic[0].latMin + baltic[0].latMax) / 2.0;
        S411RouteResult rr;
        bool rok = S411ComputeIceRoute(
            baltic, {midLat, baltic[0].lonMin + 0.3},
            {midLat, baltic[0].lonMax - 0.3}, rp, nullptr, rr);
        bool sane =
            rok ? (rr.waypoints.size() >= 2 && rr.distanceNm > 0 &&
                   rr.distanceNm < 1e5 && rr.cautionNm >= 0)
                : !rr.error.IsEmpty();
        Check(sane, "real: BSH route returns a coherent result or clean error");
      }
      std::vector<S411Dataset> ak(1);
      if (LoadS411File(fx + "/411US01AK260701.GML", ak[0], e)) {
        S411RouteParams rp;
        S411RouteResult rr;
        // straddle the antimeridian (Chukchi to Beaufort)
        S411ComputeIceRoute(ak, {71.0, 179.0}, {71.5, -179.0}, rp, nullptr, rr);
        bool inRange = true;
        for (const auto &w : rr.waypoints)
          if (w.lon < -180.5 || w.lon > 180.5) inRange = false;
        Check(inRange, "real: Alaska route waypoints stay in [-180,180]");
      }
    }
  }

  // --- 10: ocpn_draw boundary export protocol -----------------------------
  {
    S411HazardZone z;
    z.severity = S411CellState::Blocked;
    z.source = "Baltic \"BSH\" 2025";  // embedded quotes must be escaped
    z.ring = {{54.0, 19.0}, {54.0, 20.0}, {55.0, 20.0}, {55.0, 19.0}};
    wxString j = S411BuildCreateBoundary(z, "ice-1");
    Check(Contains(j, "\"Msg\":\"CreateBoundary\""),
          "boundary: CreateBoundary message");
    Check(Contains(j, "\"BoundaryType\":\"Exclusion\""),
          "boundary: exported as Exclusion (WR 'do not cross')");
    Check(Contains(j, "\"Source\":\"S411_PI\""),
          "boundary: reply channel set");
    Check(Contains(j, "#C80000"), "boundary: blocked ice is red");
    Check(Contains(j, "\\\"BSH\\\""),
          "boundary: dataset title quotes are JSON-escaped");
    // exactly 4 points sent (ring must NOT be self-closed - ocpn_draw closes)
    int npts = 0, from = 0;
    while ((from = j.find("\"BoundaryPointType\"", from)) != wxNOT_FOUND) {
      npts++;
      from += 5;
    }
    Check(npts == 4, "boundary: 4 points, no duplicated closing point");

    S411HazardZone amber = z;
    amber.severity = S411CellState::Caution;
    Check(Contains(S411BuildCreateBoundary(amber, "x"), "#FFB400"),
          "boundary: caution ice is amber");

    // delete + handshake payloads
    Check(Contains(S411BuildDeleteBoundary("abc-123"),
                   "\"Msg\":\"DeleteBoundary\"") &&
              Contains(S411BuildDeleteBoundary("abc-123"), "\"GUID\":\"abc-123\""),
          "boundary: DeleteBoundary with top-level GUID");
    Check(Contains(S411BuildVersionRequest(), "\"Msg\":\"Version\""),
          "boundary: version handshake request");

    // response scanning
    wxString resp =
        "{\"Source\":\"OCPN_DRAW_PI\",\"Msg\":\"CreateBoundary\","
        "\"Type\":\"Response\",\"MsgId\":\"ice-1\",\"Created\":true,"
        "\"GUID\":\"11112222-3333\"}";
    Check(S411JsonCreated(resp), "boundary: parse Created:true");
    Check(S411JsonField(resp, "GUID") == "11112222-3333",
          "boundary: parse generated GUID");
    Check(S411JsonField(resp, "MsgId") == "ice-1",
          "boundary: parse correlation id");

    // enumerate our own boundaries for idempotent cleanup
    wxString info =
        "{\"Boundaries\":["
        "{\"Name\":\"S-411 ice: hazardous (X)\",\"GUID\":\"g-ours-1\"},"
        "{\"Name\":\"User no-go\",\"GUID\":\"g-user\"},"
        "{\"Name\":\"S-411 ice: caution\",\"GUID\":\"g-ours-2\"}]}";
    std::vector<wxString> ours;
    size_t n = S411JsonOurBoundaryGuids(info, ours);
    Check(n == 2, "boundary: enumerate only our tagged boundaries");
    bool right = ours.size() == 2 && ours[0] == "g-ours-1" &&
                 ours[1] == "g-ours-2";
    Check(right, "boundary: user boundaries are left untouched");

    // regression: an UNNAMED user boundary right after one of ours must NOT
    // inherit our tag (review finding #4 - data loss)
    wxString info2 =
        "{\"Boundaries\":["
        "{\"Name\":\"S-411 ice: hazardous\",\"GUID\":\"g-ours\"},"
        "{\"GUID\":\"g-user-unnamed\"}]}";
    std::vector<wxString> o2;
    S411JsonOurBoundaryGuids(info2, o2);
    Check(o2.size() == 1 && o2[0] == "g-ours",
          "boundary: unnamed user boundary is never mis-tagged");

    // regression: GUID emitted BEFORE Name within the object (review #5)
    wxString info3 =
        "{\"Boundaries\":["
        "{\"GUID\":\"g-ours-3\",\"Name\":\"S-411 ice: caution\"},"
        "{\"GUID\":\"g-user\",\"Name\":\"My fishing area\"}]}";
    std::vector<wxString> o3;
    S411JsonOurBoundaryGuids(info3, o3);
    Check(o3.size() == 1 && o3[0] == "g-ours-3",
          "boundary: GUID-before-Name ordering is handled object-scoped");

    // 'GUID' must not match inside a longer key like 'BoundaryGUID'
    Check(S411JsonField("{\"BoundaryGUID\":\"x\",\"GUID\":\"real\"}", "GUID") ==
              "real",
          "boundary: field scan matches the exact key, not a substring");

    // a closed source ring must not export a duplicated first point
    S411HazardZone closed;
    closed.severity = S411CellState::Blocked;
    closed.ring = {{54.0, 19.0}, {54.0, 20.0}, {55.0, 20.0}, {54.0, 19.0}};
    wxString jc = S411BuildCreateBoundary(closed, "c");
    int cpts = 0, cf = 0;
    while ((cf = jc.find("\"BoundaryPointType\"", cf)) != wxNOT_FOUND) {
      cpts++;
      cf += 5;
    }
    Check(cpts == 3, "boundary: closed ring exports without the closing dup");
  }

  // --- 11: tessellation speed on huge polygons with many holes ------------
  {
    // one exterior and 1600 holes of 100 vertices: the shape that made the
    // NIC Arctic chart take ~60 s in a single GLU pass (IMPROVEMENTS 2.1)
    std::vector<std::vector<S411Pos>> rings(1);
    for (int i = 0; i < 400; i++) rings[0].push_back({60.0, i * 0.25});
    for (int i = 0; i < 400; i++) rings[0].push_back({60.0 + i * 0.05, 100.0});
    for (int i = 0; i < 400; i++) rings[0].push_back({80.0, 100.0 - i * 0.25});
    for (int i = 0; i < 400; i++) rings[0].push_back({80.0 - i * 0.05, 0.0});
    double holeArea = 0;
    for (int hx = 0; hx < 40; hx++)
      for (int hy = 0; hy < 40; hy++) {
        std::vector<S411Pos> h;
        const double cx = 1.25 + hx * 2.45, cy = 60.25 + hy * 0.49, r = 0.15;
        for (int k = 0; k < 100; k++) {
          const double a = 2 * 3.14159265358979 * k / 100;
          h.push_back({cy + r * std::sin(a), cx + r * std::cos(a)});
        }
        holeArea += RingArea(h);
        rings.push_back(h);
      }
    std::vector<S411Pos> tris;
    wxStopWatch sw;
    S411Triangulate(rings, tris);
    const long ms = sw.Time();
    std::printf("      (1 exterior + 1600 holes, 161600 vertices: %ld ms)\n", ms);
    Check(ms < 8000, "tess: 160k vertices / 1600 holes in < 8 s");
    const double want = RingArea(rings[0]) - holeArea;
    Check(std::fabs(TrisArea(tris) - want) / want < 0.005,
          "tess: tiled triangulation area = exterior minus holes");
  }

  // --- 12: real NIC Arctic chart (1.x zip): load-time budget --------------
  {
    std::vector<S411Dataset> sets;
    errors.Clear();
    wxStopWatch sw;
    size_t n = LoadS411Zip(fx + "/S411_NATICE_ArcticExtent_20260701.zip", sets,
                           errors);
    const long ms = sw.Time();
    std::printf("      (NIC Arctic extent: %ld ms)\n", ms);
    Check(n == 1, "NIC Arctic zip loads");
    Check(ms < 20000, "NIC Arctic loads in < 20 s (was ~56 s)");
    bool polar = false, areaOk = true;
    if (n == 1)
      for (const auto &f : sets[0].features) {
        if (f.kind != S411Kind::Area || f.rings.size() < 100) continue;
        polar = f.lonMax - f.lonMin >= 359.0;
        double want = RingArea(f.rings[0]);
        for (size_t i = 1; i < f.rings.size(); i++) want -= RingArea(f.rings[i]);
        if (std::fabs(TrisArea(f.tris) - want) / want > 0.02) areaOk = false;
      }
    Check(polar, "NIC: the pole-enclosing ice area spans 360 deg");
    Check(areaOk, "NIC: big area triangulated completely (area within 2%)");
    if (n == 1) {
      Check(sets[0].analysisTime == S411ParseIsoTime("2026-07-01") &&
                sets[0].timeApprox,
            "NIC 1.x: date from catalogue.xml, marked approximate");
      Check(!sets[0].producer.IsEmpty(), "NIC 1.x: producer from catalogue");
    }
  }

  // --- 13: circumpolar ring + point symbols + value text ------------------
  {
    S411Dataset ds;
    bool ok = LoadS411File(fx + "/circumpolar.gml", ds, err);
    Check(ok, "circumpolar fixture loads");
    const S411Feature *sea = nullptr;
    wxString bergSym, driftSym, driftTxt, thkTxt;
    for (const auto &f : ds.features) {
      if (f.type == "SeaIce") sea = &f;
      if (f.type == "Iceberg") bergSym = f.symbol;
      if (f.type == "IceDrift") { driftSym = f.symbol; driftTxt = f.valueText; }
      if (f.type == "IceThickness") thkTxt = f.valueText;
    }
    Check(sea && !sea->tris.empty(), "circumpolar: ring is tessellated");
    if (sea) {
      const double want = RingArea(sea->rings[0]) - RingArea(sea->rings[1]);
      Check(std::fabs(TrisArea(sea->tris) - want) / want < 0.01,
            "circumpolar: area closed along the pole (minus the hole)");
    }
    std::vector<S411Dataset> v(1, ds);
    std::vector<S411Hit> hits;
    auto seaHit = [&](double lat, double lon) {
      S411HitTest(v, lat, lon, 0.01, hits);
      for (const auto &h : hits)
        if (h.feature->type == "SeaIce") return true;
      return false;
    };
    Check(seaHit(-75, 100) && seaHit(-75, -179.9) && seaHit(-89, 0),
          "circumpolar: pick works everywhere inside, incl. 180 and the pole");
    Check(!seaHit(-79, 20) && !seaHit(-65, 100),
          "circumpolar: no pick in the hole or outside the ring");
    Check(ds.coverage.size() == 1, "DataCoverage kept for precedence");
    Check(bergSym == "ICEBRG03", "symbol: iceberg by icebergSize");
    Check(driftSym == "ICEDFT02" && driftTxt == "0.5 kn",
          "symbol: drift arrow by direction, speed text in kn");
    Check(thkTxt == "35 cm", "text: mean ice thickness in cm");
    Check(S411FindSymbol("ICEBRG03") && S411FindSymbol("REPSTA01") &&
              S411FindSymbol("ICEDFT99"),
          "symbols: Annex C vector symbols compiled in");
  }

  // --- 13b: polar stereographic input (SHN Argentina ships EPSG:3031) -----
  {
    S411Dataset ds;
    bool ok = LoadS411File(fx + "/v1_epsg3031.gml", ds, err);
    Check(ok && ds.features.size() == 1, "EPSG:3031 dataset loads");
    if (ok && !ds.features.empty()) {
      const S411Feature &f = ds.features[0];
      Check(std::fabs(f.latMin + 65) < 1e-4 && std::fabs(f.latMax + 64) < 1e-4 &&
                std::fabs(f.lonMin + 62) < 1e-4 && std::fabs(f.lonMax + 58) < 1e-4,
            "EPSG:3031 metres -> lat/lon (pyproj reference, 1e-4 deg)");
    }
  }

  // --- 13c: SIGRID-3 shapefiles (geographic, and polar stereographic zip) --
  for (int k = 0; k < 2; k++) {
    std::vector<S411Dataset> sets;
    errors.Clear();
    if (k == 0) {
      S411Dataset ds;
      if (S411LoadSigridFile(fx + "/sigrid3_geo_20261020.shp", ds, err))
        sets.push_back(ds);
    } else {
      LoadS411Zip(fx + "/sigrid3_ant_20261020.zip", sets, errors);
    }
    const char *what = k == 0 ? "SIGRID-3 (lon/lat)" : "SIGRID-3 (EPSG:3031 zip)";
    Check(sets.size() == 1 && sets[0].features.size() == 2,
          wxString::Format("%s: ice + water polygons load, land is skipped",
                           what).mb_str());
    if (sets.size() == 1 && sets[0].features.size() == 2) {
      const S411Feature &ice = sets[0].features[0];
      const double want = RingArea(ice.rings[0]) - RingArea(ice.rings[1]);
      Check(ice.totalConcentration == "92" && ice.stageOfDevelopment == "93" &&
                sets[0].features[1].totalConcentration == "98",
            wxString::Format("%s: CT/SA mapped (CT 00 -> 98 ice free)", what).mb_str());
      Check(std::fabs(ice.latMin + 65) < 1e-3 && std::fabs(ice.lonMax + 58) < 1e-3 &&
                std::fabs(TrisArea(ice.tris) - want) / want < 0.01,
            wxString::Format("%s: position and hole correct", what).mb_str());
      Check(S411FormatUtc(sets[0].analysisTime, true) == "20 Oct 2026",
            wxString::Format("%s: date from the file name", what).mb_str());
    }
  }

  // --- 14: metadata, validity and signatures (PS 8.1, 11.3) ---------------
  {
    std::vector<S411Dataset> sets;
    errors.Clear();
    LoadS411ExchangeSet(fx + "/TDS1/S100_ROOT/CATALOG.XML", sets, errors);
    Check(sets.size() == 1, "signed TDS1 exchange set loads");
    if (sets.size() == 1) {
      const S411Dataset &d = sets[0];
      Check(d.producer == "DE00", "metadata: producer code DE00");
      Check(S411FormatUtc(d.analysisTime) == "1 Mar 2026 12:00",
            "metadata: analysisDateTime parsed (UTC)");
      Check(S411FormatUtc(d.validUntil) == "1 Mar 2026 23:59",
            "metadata: valid until = catalogue timeInstantEnd");
#if defined(_WIN32) || defined(S411_HAVE_OPENSSL)
      Check(d.signature == S411SigState::Valid,
            "signature: TDS1 verifies (ECDSA-384-SHA2, TEST PKI)");
      Check(d.signatureNote.Contains("TEST PKI"),
            "signature: TEST PKI is flagged as not for navigation");
#else
      Check(d.signature == S411SigState::Unverified,
            "signature: reported unverified without a crypto library");
#endif
      std::printf("      (%s: %s)\n",
                  (const char *)S411SigLabel(d.signature).utf8_str(),
                  (const char *)d.signatureNote.utf8_str());
    }
    std::vector<S411Dataset> zs;
    LoadS411Zip(fx + "/TDS1.zip", zs, errors);
    Check(zs.size() == 1 && zs[0].signature == sets[0].signature &&
              zs[0].producer == "DE00",
          "signature: same result from the zipped exchange set");
    S411Dataset single;
    LoadS411File(fx + "/TDS1/S100_ROOT/S-411/DATASET_FILES/411DE00BAL260301.GML",
                 single, err);
    Check(single.signature == sets[0].signature,
          "signature: a single GML finds its exchange catalogue");

    // tampered copy: one byte of the dataset changed
    wxString tmp = wxFileName::CreateTempFileName("s411");
    wxRemoveFile(tmp);
    const wxString root = tmp + "/S100_ROOT", dsDir = root + "/S-411/DATASET_FILES";
    wxFileName::Mkdir(dsDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxCopyFile(fx + "/TDS1/S100_ROOT/CATALOG.XML", root + "/CATALOG.XML");
    wxCopyFile(fx + "/TDS1/S100_ROOT/CATALOG.SIGN", root + "/CATALOG.SIGN");
    {
      wxFFile in(fx + "/TDS1/S100_ROOT/S-411/DATASET_FILES/411DE00BAL260301.GML", "rb");
      wxString all;
      in.ReadAll(&all, wxConvUTF8);
      all.Replace("Bothnian Bay", "Bothnian Bax", false);
      wxFFile out(dsDir + "/411DE00BAL260301.GML", "wb");
      out.Write(all, wxConvUTF8);
    }
    std::vector<S411Dataset> bad;
    LoadS411ExchangeSet(root + "/CATALOG.XML", bad, errors);
#if defined(_WIN32) || defined(S411_HAVE_OPENSSL)
    Check(bad.size() == 1 && bad[0].signature == S411SigState::Invalid &&
              !bad[0].visible,
          "signature: tampered dataset is INVALID and not displayed");
#endif
    wxFileName::Rmdir(tmp, wxPATH_RMDIR_RECURSIVE);

    S411Dataset loose;
    LoadS411File(fx + "/411DE00EK1250220.GML", loose, err);
    Check(loose.signature == S411SigState::None && loose.producer == "DE00",
          "metadata: loose GML is unsigned, producer from the dataset name");
  }

  // --- 15: precedence and forecasts (PS 4.6, Table 4-2) -------------------
  {
    std::vector<S411Dataset> v(3);
    LoadS411File(fx + "/prec_old.gml", v[0], err);
    LoadS411File(fx + "/prec_new.gml", v[1], err);
    LoadS411File(fx + "/forecast.gml", v[2], err);
    Check(!v[2].visible && v[2].forecastTime != 0,
          "forecast: loaded but not displayed by default");
    S411ApplyPrecedence(v);
    Check(v[0].features.size() == 2 && v[0].features[0].superseded &&
              !v[0].features[1].superseded,
          "precedence: newer analysis supersedes the old one in its coverage");
    Check(v[0].supersededBy.IsEmpty() && !v[1].features[0].superseded,
          "precedence: remainder of the older chart stays displayable");
    std::vector<S411Hit> hits;
    S411HitTest(v, 61, 15, 0.01, hits);
    Check(hits.size() == 1 && hits[0].ds == &v[1],
          "precedence: pick reports the newer analysis only");
    v[1].visible = false;
    S411ApplyPrecedence(v);
    Check(!v[0].features[0].superseded,
          "precedence: hiding the newer chart brings the older one back");
    v[1].visible = true;
    v[1].producer = "YY00";
    S411ApplyPrecedence(v);
    Check(!v[0].features[0].superseded &&
              S411OverlappingProducers(v) == "XX00, YY00",
          "precedence: other producers never merge, overlap is indicated");
  }

  // --- 16: age of the ice information (PS 8.1) ----------------------------
  {
    S411Dataset d;
    d.analysisTime = S411ParseIsoTime("2026-03-01T12:00:00Z");
    const time_t h = 3600;
    Check(S411DatasetAge(d, d.analysisTime + 10 * h, 72) == S411Age::Current,
          "age: 10 h old is current");
    Check(S411AgeText(d, d.analysisTime + 18 * h) ==
              "Ice analysis 1 Mar 2026 12:00 UTC - 18 h old",
          "age: text as in PS 8.1");
    Check(S411DatasetAge(d, d.analysisTime + 100 * h, 72) == S411Age::OverLimit,
          "age: older than the 72 h limit");
    d.validUntil = d.analysisTime + 12 * h;
    Check(S411DatasetAge(d, d.analysisTime + 13 * h, 72) == S411Age::Outdated,
          "age: past valid-until is outdated");
    Check(S411DatasetAge(d, d.analysisTime + 31 * 24 * h, 72) == S411Age::Expired,
          "age: older than 30 days");
    Check(S411ParseIsoTime("20260701") == S411ParseIsoTime("2026-07-01T00:00:00Z") &&
              S411ParseIsoTime("garbage") == 0,
          "age: ISO and compact dates parse, garbage does not");
  }

  // --- 17: route ice check (PS 9.6 alerts) --------------------------------
  {
    std::vector<S411Dataset> v(1);
    S411Dataset &d = v[0];
    auto area = [&](const char *type, const char *ct, double la0, double la1,
                    double lo0, double lo1) {
      S411Feature f;
      f.type = type;
      f.totalConcentration = ct;
      f.rings.push_back({{la0, lo0}, {la0, lo1}, {la1, lo1}, {la1, lo0}});
      f.latMin = la0; f.latMax = la1; f.lonMin = lo0; f.lonMax = lo1;
      d.features.push_back(f);
    };
    area("SeaIce", "92", 60, 61, 20, 21);   // red for every class
    area("SeaIce", "50", 60, 61, 22, 23);   // amber without ice class
    S411Feature berg;
    berg.type = "Iceberg";
    berg.kind = S411Kind::Point;
    berg.rings.push_back({{60.505, 25.0}});
    berg.latMin = berg.latMax = 60.505;
    berg.lonMin = berg.lonMax = 25.0;
    d.features.push_back(berg);
    std::vector<S411RouteAlert> al;
    S411CheckRoute(v, {{60.5, 19.0}, {60.5, 26.0}}, "", 1.0, al);
    bool hz = false, df = false, ib = false;
    for (const auto &a : al) {
      if (a.alert == S411Alert::IceHazard && std::fabs(a.where.lon - 20) < 1e-6) hz = true;
      if (a.alert == S411Alert::IceDifficult && std::fabs(a.where.lon - 22) < 1e-6) df = true;
      if (a.alert == S411Alert::Icebergs) ib = true;
    }
    Check(al.size() == 3 && hz && df && ib,
          "route check: hazard, difficult and iceberg found at their entry");
    S411CheckRoute(v, {{60.5, 19.0}, {60.5, 26.0}}, "IA", 1.0, al);
    Check(al.size() == 2, "route check: amber 5/10 is fine for ice class IA");
    S411CheckRoute(v, {{62.0, 19.0}, {62.0, 26.0}}, "", 1.0, al);
    Check(al.empty(), "route check: clear route has no alerts");
  }

  // --- 18: golden portrayal (Annex G Expected_Portrayal display lists) ----
  {
    const char *ids[] = {"411DE00BAL260301", "411DE00EK1250220",
                         "411DE00EIS250217", "411US01AK260701"};
    const char *paths[] = {
        "/TDS1/S100_ROOT/S-411/DATASET_FILES/411DE00BAL260301.GML",
        "/411DE00EK1250220.GML", "/411DE00EIS250217.GML",
        "/411US01AK260701.GML"};
    struct Setting { const char *file; S411Mode mode; const char *cls; };
    const Setting settings[] = {
        {"Concentration_none", S411Mode::Concentration, ""},
        {"StageOfDevelopment_none", S411Mode::StageOfDevelopment, ""},
        {"Navigational_IC", S411Mode::Navigational, "IC"},
        {"Navigational_IASuper", S411Mode::Navigational, "IASuper"}};
    int okFill = 0, okAlert = 0, okSym = 0, okText = 0, total = 0;
    for (int k = 0; k < 4; k++) {
      S411Dataset ds;
      LoadS411File(fx + paths[k], ds, err);
      for (const auto &s : settings) {
        total++;
        std::map<wxString, int> wantFill, gotFill, wantAlert, gotAlert,
            wantSym, gotSym, wantText, gotText;
        wxXmlDocument doc;
        if (!doc.Load(fx + "/expected_portrayal/" + ids[k] + "/" + s.file +
                      ".xml"))
          continue;
        for (wxXmlNode *n = doc.GetRoot()->GetChildren(); n; n = n->GetNext()) {
          if (n->GetType() != wxXML_ELEMENT_NODE) continue;
          for (wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
            if (c->GetName() == "alertReference")
              wantAlert[c->GetNodeContent()]++;
            if (c->GetName() == "colorFill")
              for (wxXmlNode *col = c->GetChildren(); col; col = col->GetNext())
                if (col->GetName() == "color") wantFill[col->GetNodeContent()]++;
          }
          if (n->GetName() == "pointInstruction")
            for (wxXmlNode *c = n->GetChildren(); c; c = c->GetNext())
              if (c->GetName() == "symbol") {
                wantSym[c->GetAttribute("reference")]++;
                break;  // first symbol = the feature's own
              }
          if (n->GetName() == "textInstruction") {
            for (wxXmlNode *a = n->GetChildren(); a; a = a->GetNext())
              if (a->GetName() == "textPoint")
                for (wxXmlNode *e = a->GetChildren(); e; e = e->GetNext())
                  if (e->GetName() == "element")
                    for (wxXmlNode *t = e->GetChildren(); t; t = t->GetNext())
                      if (t->GetName() == "text") wantText[t->GetNodeContent()]++;
          }
        }
        for (const auto &f : ds.features) {
          const bool iceArea = f.kind == S411Kind::Area &&
                               (f.type == "SeaIce" || f.type == "LakeIce" ||
                                f.type == "StripsAndPatches");
          if (iceArea)
            gotFill[S411AreaTokenFor(f.totalConcentration, f.stageOfDevelopment,
                                     f.fastIce, s.mode, s.cls)]++;
          S411Alert a = S411Alert::None;
          if (iceArea)
            a = S411AreaAlert(f.totalConcentration, f.fastIce, s.cls);
          else if (f.type != "StripsAndPatches")
            a = S411FeatureAlert(f.type, f.totalConcentration, f.fastIce, s.cls);
          if (a != S411Alert::None) gotAlert[S411AlertName(a)]++;
          if (f.kind == S411Kind::Point && !f.symbol.IsEmpty()) gotSym[f.symbol]++;
          if (!f.valueText.IsEmpty()) gotText[f.valueText]++;
        }
        okFill += wantFill == gotFill;
        okAlert += wantAlert == gotAlert;
        okSym += wantSym == gotSym;
        okText += wantText == gotText;
      }
    }
    std::printf("      (fills %d/%d, alerts %d/%d, symbols %d/%d, text %d/%d)\n",
                okFill, total, okAlert, total, okSym, total, okText, total);
    Check(total == 16 && okFill == total,
          "golden: area colour tokens match the expected display lists");
    Check(okAlert == total, "golden: alert references match (PS 9.6)");
    Check(okSym == total, "golden: point symbols match (Annex C)");
    Check(okText == total, "golden: value texts match (PS 9.7)");
  }

  std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL TESTS PASSED",
              g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
