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

#include "S411Boundary.h"
#include "S411Data.h"
#include "S411Portrayal.h"
#include "S411Router.h"

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

  std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL TESTS PASSED",
              g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
