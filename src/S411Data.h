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

#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "S411Catalog.h"

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
  // points: Annex C symbol name (e.g. ICEBRG03) and the PS clause 9.7 value
  // text ("35 cm", "0.5 kn"), both resolved at load time
  wxString symbol;
  wxString valueText;
  // set by S411ApplyPrecedence: lies inside the coverage of a newer analysis
  // of the same producer (PS clause 4.6), so it is not drawn
  bool superseded = false;
  // Mercator copy for the OpenGL fast path (built at load time): x = lon,
  // y = Mercator northing in degrees, both relative to (mx0, my0) so float
  // precision suffices. mTris mirrors tris; mStrips are the outlines (areas:
  // every ring, split where a ring runs along a pole closure; lines: one).
  std::vector<float> mTris;
  std::vector<std::vector<float>> mStrips;
  double mx0 = 0, my0 = 0;
  // bounding box (unwrapped lons) for view culling
  double latMin = 90, latMax = -90, lonMin = 999, lonMax = -999;
};

struct S411Dataset {
  wxString title;
  wxString file;
  wxString datasetId;                         // datasetFileIdentifier / stem
  wxString producer;                          // producer code (DE00) or agency
  wxString agency;                            // producing agency, if known
  // PS clause 8.1 validity (UTC seconds, 0 = unknown)
  time_t analysisTime = 0;
  time_t forecastTime = 0;                    // set = forecast, not analysis
  time_t validUntil = 0;                      // catalogue timeInstantEnd
  bool timeApprox = false;                    // date only (1.x catalogue/name)
  // S-100 Part 15 signature from the exchange catalogue
  S411SigState signature = S411SigState::None;
  wxString signatureNote;
  wxString supersededBy;                      // title of the retiring dataset
  // DataCoverage rings (unwrapped like the features); empty = use the box
  std::vector<std::vector<S411Pos>> coverage;
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

/** Finish a feature whose rings hold plain lat/lon: unwrap longitudes,
 *  close pole-enclosing rings, triangulate areas, compute the box and the
 *  OpenGL Mercator copy. Shared by the GML and the shapefile reader. */
void S411FinishFeature(S411Feature &f);

/** Inverse ellipsoidal (WGS 84) polar stereographic projection; latTs < 0
 *  selects the south polar aspect. */
void S411PolarInverse(double latTs, double lon0, double x, double y,
                      double &lat, double &lon);

/** Analysis date from a file name ("..._20260701...", "...T1800Z", or the
 *  S-100 dataset name 411PPPPxxxYYMMDD); 0 when there is none. */
time_t S411TimeFromName(const wxString &stem);

/** Load a SIGRID-3 shapefile (WMO ice chart format: polygons with the CT/
 *  SA/FA egg-code fields) given its .shp bytes plus .dbf and optional .prj.
 *  Geographic and polar stereographic .prj are supported. */
bool S411LoadSigrid(const std::string &shp, const std::string &dbf,
                    const std::string &prj, const wxString &source,
                    S411Dataset &out, wxString &err);

/** Load a SIGRID-3 shapefile from disk (.shp next to its .dbf/.prj). */
bool S411LoadSigridFile(const wxString &shpPath, S411Dataset &out,
                        wxString &err);

/** Mercator northing in degrees (latitude clamped to +/-89.5 deg). */
double S411MercY(double lat);

/** Even-odd point-in-area test on the feature's world copy nearest lon. */
bool S411AreaContains(const S411Feature &f, double lat, double lon);

/** Find the features at (lat, lon) among the VISIBLE datasets: areas by
 *  even-odd point-in-polygon, lines and points within tolDeg degrees.
 *  Results are ordered points first, then lines, then areas. */
size_t S411HitTest(const std::vector<S411Dataset> &data, double lat,
                   double lon, double tolDeg, std::vector<S411Hit> &out);

/** Load one S-411 GML file (Edition 2.0.0 or 1.x). */
bool LoadS411File(const wxString &path, S411Dataset &out, wxString &err);

/** Load every S-411 GML dataset (and SIGRID-3 shapefile) found in a .zip
 *  archive (e.g. a downloaded exchange set), including zips nested in it and
 *  gzip-compressed entries; the archive itself may be gzipped (.zip.gz).
 *  Appends to `out`; returns the number of datasets loaded and appends
 *  per-file problems to `errors`. */
size_t LoadS411Zip(const wxString &zipPath, std::vector<S411Dataset> &out,
                   wxString &errors);

/** Load whatever the user picked: CATALOG.XML (exchange set), .shp
 *  (SIGRID-3), zip archives - also nested zips and gzip-compressed files
 *  (.gml.gz, .zip.gz), recognised by content, not by extension - or a single
 *  GML file. Appends to `out`; returns the number of datasets loaded. */
size_t S411LoadPath(const wxString &path, std::vector<S411Dataset> &out,
                    wxString &errors);

/** Load an unpacked S-100 exchange set: the user picks CATALOG.XML and the
 *  directory tree below it is scanned for *.gml/*.GML dataset files (robust
 *  against catalogue quirks of individual producers). */
size_t LoadS411ExchangeSet(const wxString &catalogPath,
                           std::vector<S411Dataset> &out, wxString &errors);

/** Apply the precedence rules of PS clause 4.6 / Table 4-2 to the visible
 *  datasets: a later analysis of the same producer supersedes the features
 *  of earlier analyses that lie inside its DataCoverage (feature.superseded);
 *  an earlier dataset that is superseded completely gets supersededBy set.
 *  Forecasts and datasets of other producers never supersede anything. */
void S411ApplyPrecedence(std::vector<S411Dataset> &data);

/** Producers whose visible analyses overlap one another (clause 4.6: data of
 *  different producers is never merged, the overlap must be indicated).
 *  Returns e.g. "CA00, US00", empty when nothing overlaps. */
wxString S411OverlappingProducers(const std::vector<S411Dataset> &data);

/** Validity of a dataset under PS clause 8.1. */
enum class S411Age {
  Unknown,    // no analysis time in the data
  Current,    // within the mariner's age limit and still valid
  OverLimit,  // older than the age limit
  Outdated,   // past its "valid until" (forecast time or catalogue end)
  Expired,    // older than 30 days: not displayed by default
};

S411Age S411DatasetAge(const S411Dataset &ds, time_t now, double limitHours);

/** "Ice analysis 20 Feb 2025 12:00 UTC - 18 h old" (or "forecast for ...",
 *  "age unknown"); ages over two days are given in days. */
wxString S411AgeText(const S411Dataset &ds, time_t now);

/** UTC "20 Feb 2025 12:00" (date only when the time is approximate). */
wxString S411FormatUtc(time_t t, bool dateOnly = false);
