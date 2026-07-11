/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Ice-aware route computation (A* over a cost raster derived from the
 * loaded S-411 charts) and hazard-polygon extraction for interop with
 * weather_routing_pi / ocpn_draw_pi boundaries.
 *
 * Deliberately GUI-free (wx base + std only): the whole router is exercised
 * by the headless test binary against synthetic ice fields. Land avoidance
 * is injected as a callback so tests can stub OpenCPN's GSHHS check.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <functional>
#include <vector>

#include "S411Data.h"
#include "S411Portrayal.h"

/** Severity of a raster cell, derived from the ice-class traffic light. */
enum class S411CellState : unsigned char { Free = 0, Mild, Caution, Blocked };

/** Classify one area feature for routing under the given vessel ice class:
 *  fast ice and 'hazardous' (red) are Blocked, 'caution' (amber), unknown
 *  concentration and iceberg areas are Caution, ice below the class limit is
 *  Mild, ice-free/open water is Free. */
S411CellState S411RouteSeverity(const S411Feature &f, const wxString &iceClass);

struct S411RouteParams {
  wxString iceClass;             // "", IC, IB, IA, IASuper
  bool avoidCaution = false;     // treat amber as impassable
  double cautionPenalty = 4.0;   // cost multiplier inside amber ice
  double mildPenalty = 1.15;     // slight preference for open water
  int maxDim = 200;              // max raster cells per axis (matches viewer/QGIS)
  double marginFrac = 0.30;      // corridor margin around the start-end box
  double minMarginDeg = 0.8;
};

struct S411RouteResult {
  std::vector<S411Pos> waypoints;  // start ... end (lon in [-180, 180))
  double distanceNm = 0;
  double cautionNm = 0;            // distance sailed through amber ice
  wxString error;                  // set when no route was found
};

/** Land test: returns true when the segment (lat1,lon1)-(lat2,lon2) crosses
 *  land. The plugin passes PlugIn_GSHHS_CrossesLand; tests pass stubs.
 *  May be empty (no land checking). */
typedef std::function<bool(double, double, double, double)> S411LandFn;

/** Compute an ice-aware route between two positions using the VISIBLE
 *  datasets. Returns false and sets result.error when no passable route
 *  exists within the search corridor. */
bool S411ComputeIceRoute(const std::vector<S411Dataset> &data,
                         const S411Pos &start, const S411Pos &end,
                         const S411RouteParams &params,
                         const S411LandFn &crossesLand, S411RouteResult &out);

/** One exportable ice hazard zone (exterior ring only, simplified). */
struct S411HazardZone {
  std::vector<S411Pos> ring;       // closed not required; lon [-180,180)
  S411CellState severity;          // Blocked or Caution
  wxString source;                 // dataset title
};

/** Extract hazard polygons (Blocked, optionally Caution) from the visible
 *  datasets for export as ocpn_draw boundaries. Rings are simplified with
 *  Douglas-Peucker to at most maxPoints vertices. */
size_t S411ExtractHazards(const std::vector<S411Dataset> &data,
                          const wxString &iceClass, bool includeCaution,
                          size_t maxPoints,
                          std::vector<S411HazardZone> &out);

/** Douglas-Peucker ring simplification to <= maxPoints vertices (public for
 *  tests). Keeps first/last point of the input open ring. */
void S411SimplifyRing(const std::vector<S411Pos> &in, size_t maxPoints,
                      std::vector<S411Pos> &out);
