/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * GML parser and exchange-set/zip loaders.
 *
 * The parser is namespace-agnostic (matches local element names) so that
 * datasets from any producer load regardless of prefix conventions. The
 * dataset edition is detected from the root namespace declarations:
 *   - "iho.int/S411"    -> Edition 2.0.0
 *   - "jcomm.info/ice"  -> Edition 1.x, v1 attribute names (iceact/iceflz)
 *                          and v1 feature codes
 * The coordinate axis order is NOT tied to the edition: both editions occur
 * as lat/lon (BSH, CIS, MetNo, NIC) and as lon/lat with 0..360 longitudes
 * (AARI, NWS). It is detected per file - see DetectAxis().
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "S411Data.h"

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/tokenzr.h>
#include <wx/wfstream.h>
#include <wx/xml/xml.h>
#include <wx/zipstrm.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "S411Tess.h"

enum class Axis { Auto, LatLon, LonLat };

struct ParseCtx {
  Axis axis = Axis::Auto;
  bool v1 = false;
};

static wxString Local(const wxXmlNode *n) {
  wxString s = n->GetName();
  int i = s.Find(':', true);
  return i == wxNOT_FOUND ? s : s.Mid(i + 1);
}

static bool IsElem(const wxXmlNode *n) {
  return n && n->GetType() == wxXML_ELEMENT_NODE;
}

// first descendant element with the given local name (depth first)
static const wxXmlNode *FindDesc(const wxXmlNode *n, const wxString &loc) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    if (Local(c) == loc) return c;
    const wxXmlNode *r = FindDesc(c, loc);
    if (r) return r;
  }
  return nullptr;
}

static bool AnyDescTextContains(const wxXmlNode *n, const wxString &loc,
                                const wxString &a, const wxString &b) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    if (Local(c) == loc) {
      wxString t = c->GetNodeContent();
      if (t.Contains(a) || (!b.IsEmpty() && t.Contains(b))) return true;
    }
    if (AnyDescTextContains(c, loc, a, b)) return true;
  }
  return false;
}

// Edition 1.x feature codes -> Edition 2.0.0 names (portrayal keys)
static wxString MapV1Type(const wxString &t) {
  static const struct { const char *v1, *v2; } M[] = {
      {"seaice", "SeaIce"}, {"lacice", "LakeIce"}, {"brgare", "IcebergArea"},
      {"icelne", "IceEdge"}, {"brglne", "IcebergLimit"},
      {"opnlne", "OpenWaterLimit"}, {"lkilne", "AllKnownIceLimit"},
      {"i_ridg", "IceRidgeLine"}, {"i_lead", "IceLeadLine"},
      {"i_fral", "IceFractureLine"}, {"i_crac", "IceCrackLine"},
      {"jmdbrr", "JammedBrashBarrier"}, {"icecom", "IceCompacting"},
      {"icelea", "IceLead"}, {"icebrg", "Iceberg"}, {"flobrg", "Floeberg"},
      {"icethk", "IceThickness"}, {"iceshr", "IceShear"},
      {"icediv", "IceDivergence"}, {"icerdg", "IceRidgeHummock"},
      {"icekel", "IceKeelBummock"}, {"icedft", "IceDrift"},
      {"icefra", "IceFracture"}, {"icerft", "IceRafting"},
      {"stgmlt", "StageOfMelt"}, {"snwcvr", "SnowCover"},
      {"strptc", "StripsAndPatches"}, {"i_grhm", "GroundedHummock"},
  };
  wxString lt = t.Lower();
  for (const auto &m : M)
    if (lt == m.v1) return m.v2;
  return t;
}

// first value of a possibly list-valued v1 attribute: "[95, 87]" -> "95"
static wxString FirstListValue(wxString v) {
  v.Trim().Trim(false);
  v.Replace("[", ""); v.Replace("]", "");
  if (v.Contains(",")) v = v.BeforeFirst(',');
  v.Trim().Trim(false);
  if (v.length() == 1 && wxIsdigit(v[0])) v = "0" + v;
  return v;
}

// Edition 1.x concentration -> IOC v5.2 code (mirrors tools/convert_v1.py)
static wxString CleanConcV1(wxString v) {
  v = FirstListValue(v);
  if (v == "01" || v == "55") return "98";
  if (v == "02") return "01";
  if (v == "03") return "02";
  long n;
  if (v.ToLong(&n)) {
    if (n >= 1 && n <= 9) return wxString::Format("%ld", n * 10);
    if (n >= 10 && n <= 98) return wxString::Format("%02ld", n);
  }
  return "";
}

static void TokenizeNums(const wxString &txt, std::vector<double> &out) {
  wxStringTokenizer tk(txt, wxT(" \t\r\n,"), wxTOKEN_STRTOK);
  while (tk.HasMoreTokens()) {
    double v;
    if (tk.GetNextToken().ToCDouble(&v)) out.push_back(v);
  }
}

// posList/pos text -> coordinate pairs, using the file-wide axis decision
static void ParsePosText(const wxString &txt, const ParseCtx &ctx,
                         std::vector<S411Pos> &out) {
  std::vector<double> nums;
  TokenizeNums(txt, nums);
  if (nums.size() < 2) return;
  const bool swapped = (ctx.axis == Axis::LonLat);
  for (size_t i = 0; i + 1 < nums.size(); i += 2) {
    double lat = swapped ? nums[i + 1] : nums[i];
    double lon = swapped ? nums[i] : nums[i + 1];
    if (lon > 180.0) lon -= 360.0;  // 0..360 producers (AARI, NWS)
    if (std::fabs(lat) <= 90.0 && std::fabs(lon) <= 540.0)
      out.push_back({lat, lon});
  }
}

// File-wide axis detection (mirrors tools/convert_v1.py): coordinates are
// lat/lon unless the FIRST value of a pair reaches beyond +/-90 while the
// SECOND never does - only a longitude can exceed 90, so that means lon/lat.
// EPSG:4326 mandates lat/lon, but several ice services (AARI, NWS) ship
// lon/lat with 0..360 longitudes, so the axis must be detected, not assumed.
// Charts whose values never leave +/-90 on either axis (e.g. the Baltic) are
// decided by the declared CRS instead: OGC CRS84 is the one common lon/lat
// CRS, everything else (EPSG:4326, the S-100 default) means lat/lon.
// Pairs are formed per posList/pos element so that one malformed element
// cannot shift the pairing of every element after it.
struct AxisScan {
  double maxFirst = 0, maxSecond = 0;
  bool crs84 = false;
};

static void ScanPosNums(const wxXmlNode *n, AxisScan &s) {
  for (wxXmlAttribute *a = n->GetAttributes(); a; a = a->GetNext())
    if (a->GetName().AfterLast(':') == "srsName" &&
        a->GetValue().Contains("CRS84"))
      s.crs84 = true;
  wxString loc = Local(n);
  if (loc == "posList" || loc == "pos") {
    std::vector<double> nums;
    TokenizeNums(n->GetNodeContent(), nums);
    for (size_t i = 0; i + 1 < nums.size(); i += 2) {
      double a = std::fabs(nums[i]), b = std::fabs(nums[i + 1]);
      if (a > s.maxFirst) s.maxFirst = a;
      if (b > s.maxSecond) s.maxSecond = b;
    }
    return;
  }
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext())
    if (IsElem(c)) ScanPosNums(c, s);
}

static Axis DetectAxis(const wxXmlNode *root) {
  AxisScan s;
  ScanPosNums(root, s);
  if (s.maxFirst > 90.0001 && s.maxSecond <= 90.0001) return Axis::LonLat;
  if (s.maxSecond > 90.0001) return Axis::LatLon;
  return s.crs84 ? Axis::LonLat : Axis::LatLon;  // both within +/-90
}

// keep paths continuous across the antimeridian (done once, at load time)
static void UnwrapLons(std::vector<S411Pos> &pts) {
  for (size_t i = 1; i < pts.size(); i++) {
    while (pts[i].lon - pts[i - 1].lon > 180.0) pts[i].lon -= 360.0;
    while (pts[i - 1].lon - pts[i].lon > 180.0) pts[i].lon += 360.0;
  }
}

// concatenate every posList/pos below n (curve segments etc.)
static void CollectPositions(const wxXmlNode *n, const ParseCtx &ctx,
                             std::vector<S411Pos> &out) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    wxString loc = Local(c);
    if (loc == "posList" || loc == "pos")
      ParsePosText(c->GetNodeContent(), ctx, out);
    else
      CollectPositions(c, ctx, out);
  }
}

// collect polygon rings: exterior rings first, then interiors (even-odd)
static void CollectRings(const wxXmlNode *n, const ParseCtx &ctx,
                         bool exterior,
                         std::vector<std::vector<S411Pos>> &ext,
                         std::vector<std::vector<S411Pos>> &intr) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    wxString loc = Local(c);
    if (loc == "exterior") {
      CollectRings(c, ctx, true, ext, intr);
    } else if (loc == "interior") {
      CollectRings(c, ctx, false, ext, intr);
    } else if (loc == "LinearRing" || loc == "Ring") {
      std::vector<S411Pos> ring;
      CollectPositions(c, ctx, ring);
      if (ring.size() >= 3) (exterior ? ext : intr).push_back(ring);
    } else {
      CollectRings(c, ctx, exterior, ext, intr);
    }
  }
}

static const wxXmlNode *FindGeometry(const wxXmlNode *n) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    wxString loc = Local(c);
    if (loc == "Point" || loc == "LineString" || loc == "Curve" ||
        loc == "CompositeCurve" || loc == "Polygon" || loc == "Surface")
      return c;
    const wxXmlNode *r = FindGeometry(c);
    if (r) return r;
  }
  return nullptr;
}

// flatten every non-geometry leaf element into name/value pairs for the
// on-click feature report (covers both editions' attribute vocabularies,
// including nested complex attributes like iceCharacteristic)
static void CollectAttrs(const wxXmlNode *n, const wxXmlNode *geom,
                         std::vector<std::pair<wxString, wxString>> &out) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c) || c == geom) continue;
    wxString loc = Local(c);
    if (loc == "boundedBy") continue;  // envelope corners are not attributes
    bool leaf = true;
    for (const wxXmlNode *g = c->GetChildren(); g; g = g->GetNext())
      if (IsElem(g)) { leaf = false; break; }
    if (!leaf) {
      CollectAttrs(c, geom, out);
      continue;
    }
    wxString v = c->GetNodeContent().Trim().Trim(false);
    if (!v.IsEmpty() && v.length() <= 120) out.push_back({loc, v});
  }
}

static void ParseFeature(const wxXmlNode *node, const wxXmlNode *geom,
                         const ParseCtx &ctx, S411Dataset &ds) {
  S411Feature f;
  f.type = ctx.v1 ? MapV1Type(Local(node)) : Local(node);
  wxString gloc = Local(geom);
  if (gloc == "Point") {
    std::vector<S411Pos> p;
    CollectPositions(geom, ctx, p);
    if (p.empty()) return;
    f.kind = S411Kind::Point;
    f.rings.push_back({p[0]});
  } else if (gloc == "LineString" || gloc == "Curve" ||
             gloc == "CompositeCurve") {
    std::vector<S411Pos> line;
    CollectPositions(geom, ctx, line);
    if (line.size() < 2) return;
    UnwrapLons(line);
    f.kind = S411Kind::Line;
    f.rings.push_back(line);
  } else {
    std::vector<std::vector<S411Pos>> ext, intr;
    CollectRings(geom, ctx, true, ext, intr);
    if (ext.empty()) return;
    f.kind = S411Kind::Area;
    for (auto &r : ext) f.rings.push_back(r);
    for (auto &r : intr) f.rings.push_back(r);
    for (auto &r : f.rings) UnwrapLons(r);
    // keep holes on the same world copy as their exterior
    if (f.rings.size() > 1) {
      double ref = f.rings[0][0].lon;
      for (size_t i = 1; i < f.rings.size(); i++) {
        double k =
            360.0 * std::floor((f.rings[i][0].lon - ref + 180.0) / 360.0);
        if (k != 0.0)
          for (auto &p : f.rings[i]) p.lon -= k;
      }
    }
    S411Triangulate(f.rings, f.tris);
  }

  if (ctx.v1) {
    const wxXmlNode *ct = FindDesc(node, "iceact");
    if (ct) f.totalConcentration = CleanConcV1(ct->GetNodeContent());
    const wxXmlNode *sod = FindDesc(node, "icesod");
    if (sod) f.stageOfDevelopment = FirstListValue(sod->GetNodeContent());
    f.fastIce = AnyDescTextContains(node, "iceflz", "08", "09");
  } else {
    const wxXmlNode *ct = FindDesc(node, "totalConcentration");
    if (ct) f.totalConcentration = ct->GetNodeContent().Trim().Trim(false);
    const wxXmlNode *sod = FindDesc(node, "stageOfDevelopment");
    if (sod) f.stageOfDevelopment = sod->GetNodeContent().Trim().Trim(false);
    f.fastIce = AnyDescTextContains(node, "formOfIce", "09", "");
  }
  CollectAttrs(node, geom, f.attrs);

  for (const auto &ring : f.rings)
    for (const auto &p : ring) {
      if (p.lat < f.latMin) f.latMin = p.lat;
      if (p.lat > f.latMax) f.latMax = p.lat;
      if (p.lon < f.lonMin) f.lonMin = p.lon;
      if (p.lon > f.lonMax) f.lonMax = p.lon;
    }
  ds.features.push_back(f);
}

// walk the dataset: any element with a gml:id and a geometry is a feature
static void Walk(const wxXmlNode *n, const ParseCtx &ctx, S411Dataset &ds) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    wxString loc = Local(c);
    if (loc == "boundedBy" || loc == "DatasetIdentificationInformation")
      continue;
    if (loc != "Dataset" && !c->GetAttribute("gml:id", "").IsEmpty()) {
      const wxXmlNode *geom = FindGeometry(c);
      if (geom) {
        if (loc != "DataCoverage")  // coverage extent is not portrayed
          ParseFeature(c, geom, ctx, ds);
        continue;  // features are not nested
      }
    }
    Walk(c, ctx, ds);
  }
}

static bool ParseDoc(wxXmlDocument &doc, const wxString &source,
                     S411Dataset &out, wxString &err) {
  const wxXmlNode *root = doc.GetRoot();
  if (!root) {
    err = wxString::Format("%s: empty document", source);
    return false;
  }
  ParseCtx ctx;
  // Edition detection from the root namespace: this only picks the attribute
  // names / feature-code vocabulary (v1 uses iceact/icesod/iceflz and short
  // feature codes). It does NOT decide the coordinate axis - that is detected
  // from the data, because both editions occur with either axis order.
  for (wxXmlAttribute *a = root->GetAttributes(); a; a = a->GetNext()) {
    if (a->GetValue().Contains("jcomm.info/ice")) { ctx.v1 = true; break; }
  }
  ctx.axis = DetectAxis(root);
  out.file = source;
  const wxXmlNode *title = FindDesc(root, "datasetTitle");
  out.title = title ? title->GetNodeContent().Trim().Trim(false)
                    : wxFileName(source).GetName();
  Walk(root, ctx, out);
  if (out.features.empty()) {
    err = wxString::Format("%s: no S-411 features with geometry found",
                           source);
    return false;
  }
  for (const auto &f : out.features) {
    if (f.latMin < out.latMin) out.latMin = f.latMin;
    if (f.latMax > out.latMax) out.latMax = f.latMax;
    if (f.lonMin < out.lonMin) out.lonMin = f.lonMin;
    if (f.lonMax > out.lonMax) out.lonMax = f.lonMax;
  }
  return true;
}

// ---------------------------------------------------------------------------
// feature picking (pure geometry, exercised by the headless tests)
// ---------------------------------------------------------------------------
static bool PointInRings(const std::vector<std::vector<S411Pos>> &rings,
                         double lat, double lon) {
  bool in = false;  // even-odd across exterior ring(s) and holes
  for (const auto &ring : rings)
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
      const S411Pos &a = ring[i], &b = ring[j];
      if ((a.lat > lat) != (b.lat > lat) &&
          lon < (b.lon - a.lon) * (lat - a.lat) / (b.lat - a.lat) + a.lon)
        in = !in;
    }
  return in;
}

// planar distance in degrees from (lat, lon) to segment a-b, longitudes
// compressed by cos(lat) so the tolerance is isotropic on the chart
static double SegDistDeg(double lat, double lon, const S411Pos &a,
                         const S411Pos &b) {
  const double k = std::cos(lat * 3.14159265358979323846 / 180.0);
  const double ax = a.lon * k, ay = a.lat, bx = b.lon * k, by = b.lat;
  const double px = lon * k, py = lat;
  const double dx = bx - ax, dy = by - ay;
  double t = (dx * (px - ax) + dy * (py - ay)) / (dx * dx + dy * dy + 1e-12);
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  const double qx = ax + t * dx, qy = ay + t * dy;
  return std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
}

size_t S411HitTest(const std::vector<S411Dataset> &data, double lat,
                   double lon, double tolDeg, std::vector<S411Hit> &out) {
  out.clear();
  for (const auto &ds : data) {
    if (!ds.visible) continue;
    for (const auto &f : ds.features) {
      if (f.rings.empty() || f.rings[0].empty()) continue;
      // bring the probe onto the feature's world copy (lons are unwrapped)
      const double c = (f.lonMin + f.lonMax) / 2.0;
      const double lonW = lon + 360.0 * std::floor((c - lon + 180.0) / 360.0);
      if (lat < f.latMin - tolDeg || lat > f.latMax + tolDeg ||
          lonW < f.lonMin - tolDeg || lonW > f.lonMax + tolDeg)
        continue;
      bool hit = false;
      if (f.kind == S411Kind::Area) {
        hit = PointInRings(f.rings, lat, lonW);
      } else if (f.kind == S411Kind::Line) {
        const auto &ln = f.rings[0];
        for (size_t i = 0; i + 1 < ln.size() && !hit; i++)
          hit = SegDistDeg(lat, lonW, ln[i], ln[i + 1]) <= tolDeg;
      } else {
        hit = SegDistDeg(lat, lonW, f.rings[0][0], f.rings[0][0]) <= tolDeg;
      }
      if (hit) out.push_back({&ds, &f});
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const S411Hit &a, const S411Hit &b) {
                     return (int)a.feature->kind < (int)b.feature->kind;
                   });  // points first, then lines, then areas
  return out.size();
}

bool LoadS411File(const wxString &path, S411Dataset &out, wxString &err) {
  wxXmlDocument doc;
  {
    wxLogNull quiet;
    if (!doc.Load(path)) {
      err = wxString::Format("%s: not a readable XML/GML file", path);
      return false;
    }
  }
  return ParseDoc(doc, path, out, err);
}

static bool IsGmlName(const wxString &name) {
  wxString l = name.Lower();
  return l.EndsWith(".gml");
}

size_t LoadS411Zip(const wxString &zipPath, std::vector<S411Dataset> &out,
                   wxString &errors) {
  wxFFileInputStream in(zipPath);
  if (!in.IsOk()) {
    errors += zipPath + ": cannot open file\n";
    return 0;
  }
  wxZipInputStream zip(in);
  size_t loaded = 0;
  bool sawGml = false;
  std::unique_ptr<wxZipEntry> entry;
  while (entry.reset(zip.GetNextEntry()), entry) {
    if (entry->IsDir() || !IsGmlName(entry->GetName())) continue;
    sawGml = true;
    if (!zip.OpenEntry(*entry)) continue;
    wxXmlDocument doc;
    bool ok;
    {
      wxLogNull quiet;
      ok = doc.Load(zip);
    }
    wxString src = zipPath + "!" + entry->GetName();
    if (!ok) {
      errors += src + ": not readable XML\n";
      continue;
    }
    S411Dataset ds;
    wxString err;
    if (ParseDoc(doc, src, ds, err)) {
      out.push_back(ds);
      loaded++;
    } else {
      errors += err + "\n";
    }
  }
  if (!sawGml) errors += zipPath + ": no .gml/.GML entries in archive\n";
  return loaded;
}

static void FindGmlFiles(const wxString &dir, wxArrayString &out,
                         int depth = 0) {
  if (depth > 16) return;  // exchange sets are shallow; cap guards link cycles
                           // (wxDirExists resolves links, bypassing NO_FOLLOW)
  wxDir d(dir);
  if (!d.IsOpened()) return;
  wxString name;
  bool more = d.GetFirst(&name, wxEmptyString,
                         wxDIR_FILES | wxDIR_DIRS | wxDIR_NO_FOLLOW);
  while (more) {
    wxString full = dir + wxFileName::GetPathSeparator() + name;
    if (wxDirExists(full))
      FindGmlFiles(full, out, depth + 1);
    else if (IsGmlName(name))
      out.Add(full);
    more = d.GetNext(&name);
  }
}

size_t LoadS411ExchangeSet(const wxString &catalogPath,
                           std::vector<S411Dataset> &out, wxString &errors) {
  // deliberately catalogue-agnostic: producers' CATALOG.XML files differ in
  // detail, but the dataset files always live below S100_ROOT - scan them.
  wxString root = wxFileName(catalogPath).GetPath();
  wxArrayString files;
  FindGmlFiles(root, files);
  if (files.IsEmpty()) {
    errors += root + ": no .gml/.GML dataset files below the catalogue\n";
    return 0;
  }
  size_t loaded = 0;
  for (size_t i = 0; i < files.GetCount(); i++) {
    S411Dataset ds;
    wxString err;
    if (LoadS411File(files[i], ds, err)) {
      out.push_back(ds);
      loaded++;
    } else {
      errors += err + "\n";
    }
  }
  return loaded;
}
