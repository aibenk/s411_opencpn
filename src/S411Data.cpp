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
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/tokenzr.h>
#include <wx/wfstream.h>
#include <wx/xml/xml.h>
#include <wx/zipstrm.h>
#include <wx/zstream.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "S411Portrayal.h"
#include "S411Tess.h"

enum class Axis { Auto, LatLon, LonLat };

// lifted coordinate texts: (offset, length) into the raw bytes, see
// LiftCoordinates()
typedef std::vector<std::pair<size_t, size_t>> CoordTable;

// Polar stereographic CRSs (WGS 84) some ice services deliver in, e.g. the
// Argentine SHN with EPSG:3031: coordinates are metres, not degrees
struct PolarStereo {
  int epsg;
  bool south;
  double latTs, lon0;  // latitude of true scale, central meridian (deg)
};
static const PolarStereo kPolar[] = {
    {3031, true, -71.0, 0.0},    // Antarctic Polar Stereographic
    {3976, true, -70.0, 0.0},    // NSIDC Sea Ice Polar Stereographic South
    {3413, false, 70.0, -45.0},  // NSIDC Sea Ice Polar Stereographic North
    {3995, false, 71.0, 0.0},    // Arctic Polar Stereographic
};

static void PolarInverse(const PolarStereo &ps, double x, double y,
                         double &lat, double &lon);

struct ParseCtx {
  Axis axis = Axis::Auto;
  bool v1 = false;
  const PolarStereo *proj = nullptr;  // set: x/y metres in this CRS
  const std::string *raw = nullptr;   // original bytes (lifted coordinates)
  const CoordTable *coords = nullptr;
};

// inverse ellipsoidal polar stereographic (Snyder 1987, eqs. 21-38, 7-9);
// the south case is the north case with x, y, latTs and lon0 negated
void S411PolarInverse(double latTs, double lon0, double x, double y,
                      double &lat, double &lon) {
  const PolarStereo ps = {0, latTs < 0, latTs, lon0};
  PolarInverse(ps, x, y, lat, lon);
}

static void PolarInverse(const PolarStereo &ps, double x, double y,
                         double &lat, double &lon) {
  const double a = 6378137.0, f = 1 / 298.257223563;
  const double e2 = f * (2 - f), e = std::sqrt(e2);
  const double d2r = 3.14159265358979323846 / 180.0;
  const double sg = ps.south ? -1.0 : 1.0;
  x *= sg;
  y *= sg;
  const double phc = sg * ps.latTs * d2r;
  const double sc = std::sin(phc);
  const double tc = std::tan(3.14159265358979323846 / 4 - phc / 2) /
                    std::pow((1 - e * sc) / (1 + e * sc), e / 2);
  const double mc = std::cos(phc) / std::sqrt(1 - e2 * sc * sc);
  const double rho = std::sqrt(x * x + y * y);
  const double t = rho * tc / (a * mc);
  const double chi = 3.14159265358979323846 / 2 - 2 * std::atan(t);
  const double e4 = e2 * e2, e6 = e4 * e2, e8 = e4 * e4;
  const double phi =
      chi + (e2 / 2 + 5 * e4 / 24 + e6 / 12 + 13 * e8 / 360) * std::sin(2 * chi) +
      (7 * e4 / 48 + 29 * e6 / 240 + 811 * e8 / 11520) * std::sin(4 * chi) +
      (7 * e6 / 120 + 81 * e8 / 1120) * std::sin(6 * chi) +
      (4279 * e8 / 161280) * std::sin(8 * chi);
  const double lam = sg * ps.lon0 * d2r + std::atan2(x, -y);
  lat = sg * phi / d2r;
  lon = sg * lam / d2r;
  lon -= 360.0 * std::floor((lon + 180.0) / 360.0);
}

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

// Locale-independent number scanner (OpenCPN may run with a decimal-comma
// locale, so strtod is not an option); much faster than tokenizing wxStrings
static bool IsSep(char c) {
  return c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n';
}

static void FastNums(const char *p, const char *end, std::vector<double> &out) {
  static const double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
                                  1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14,
                                  1e15, 1e16, 1e17, 1e18};
  while (p < end) {
    while (p < end && IsSep(*p)) p++;
    if (p >= end) break;
    const char *start = p;
    bool neg = false;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    unsigned long long mant = 0;
    int digits = 0, scale = 0;
    bool any = false;
    for (; p < end && *p >= '0' && *p <= '9'; p++, any = true) {
      if (digits < 18) { mant = mant * 10 + (*p - '0'); digits++; }
      else scale++;
    }
    if (p < end && *p == '.') {
      for (p++; p < end && *p >= '0' && *p <= '9'; p++, any = true)
        if (digits < 18) { mant = mant * 10 + (*p - '0'); digits++; scale--; }
    }
    if (any && p < end && (*p == 'e' || *p == 'E')) {
      const char *q = p + 1;
      bool eneg = false;
      if (q < end && (*q == '-' || *q == '+')) eneg = *q++ == '-';
      int ex = 0;
      bool ed = false;
      for (; q < end && *q >= '0' && *q <= '9'; q++, ed = true)
        ex = std::min(400, ex * 10 + (*q - '0'));
      if (ed) { scale += eneg ? -ex : ex; p = q; }
    }
    if (!any || (p < end && !IsSep(*p))) {  // not a number: skip the token
      p = start;
      while (p < end && !IsSep(*p)) p++;
      continue;
    }
    double v = (double)mant;
    if (scale > 0) v *= scale <= 18 ? kPow10[scale] : std::pow(10.0, scale);
    else if (scale < 0) v /= -scale <= 18 ? kPow10[-scale] : std::pow(10.0, -scale);
    out.push_back(neg ? -v : v);
  }
}

// Coordinate text is lifted out of the document before wxXmlDocument sees
// it: wx appends every expat text chunk to the node content, which is
// quadratic for multi-megabyte posList elements (52 MB SHN chart: 90 s).
// Each posList/pos text becomes "#<index>" into this table.
static void LiftCoordinates(const std::string &in, std::string &out,
                            CoordTable &table) {
  out.clear();
  out.reserve(in.size() / 4 + 1024);
  table.clear();
  size_t i = 0, copied = 0;
  while ((i = in.find('<', i)) != std::string::npos) {
    const size_t n = i + 1;
    if (n < in.size() && (in[n] == '/' || in[n] == '?' || in[n] == '!')) {
      i = n;
      continue;
    }
    size_t nameEnd = n;
    while (nameEnd < in.size() && !isspace((unsigned char)in[nameEnd]) &&
           in[nameEnd] != '>' && in[nameEnd] != '/')
      nameEnd++;
    size_t local = n;
    for (size_t k = n; k < nameEnd; k++)
      if (in[k] == ':') local = k + 1;
    const size_t len = nameEnd - local;
    const bool coord = (len == 7 && in.compare(local, 7, "posList") == 0) ||
                       (len == 3 && in.compare(local, 3, "pos") == 0);
    const size_t gt = in.find('>', nameEnd);
    if (gt == std::string::npos) break;
    if (!coord || in[gt - 1] == '/') {
      i = gt + 1;
      continue;
    }
    const size_t lt = in.find('<', gt + 1);
    if (lt == std::string::npos) break;
    out.append(in, copied, gt + 1 - copied);
    out += "#" + std::to_string(table.size());
    table.push_back({gt + 1, lt - gt - 1});
    copied = lt;
    i = lt;
  }
  out.append(in, copied, std::string::npos);
}

static void TokenizeNums(const wxString &txt, std::vector<double> &out) {
  wxStringTokenizer tk(txt, wxT(" \t\r\n,"), wxTOKEN_STRTOK);
  while (tk.HasMoreTokens()) {
    double v;
    if (tk.GetNextToken().ToCDouble(&v)) out.push_back(v);
  }
}

// numbers of one posList/pos element (lifted "#n" or inline text)
static void NodeNums(const wxXmlNode *n, const std::string *raw,
                     const CoordTable *coords, std::vector<double> &out) {
  const wxString txt = n->GetNodeContent();
  if (raw && coords && txt.StartsWith("#")) {
    unsigned long idx;
    if (txt.Mid(1).ToULong(&idx) && idx < coords->size()) {
      const auto &e = (*coords)[idx];
      FastNums(raw->data() + e.first, raw->data() + e.first + e.second, out);
      return;
    }
  }
  TokenizeNums(txt, out);
}

// posList/pos element -> coordinate pairs, using the file-wide axis decision
static void ParsePosText(const wxXmlNode *node, const ParseCtx &ctx,
                         std::vector<S411Pos> &out) {
  std::vector<double> nums;
  NodeNums(node, ctx.raw, ctx.coords, nums);
  if (nums.size() < 2) return;
  if (ctx.proj) {  // projected metres: x (easting) first
    for (size_t i = 0; i + 1 < nums.size(); i += 2) {
      double lat, lon;
      PolarInverse(*ctx.proj, nums[i], nums[i + 1], lat, lon);
      out.push_back({lat, lon});
    }
    return;
  }
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
  const std::string *raw = nullptr;
  const CoordTable *coords = nullptr;
  double maxFirst = 0, maxSecond = 0;
  bool crs84 = false;
  const PolarStereo *proj = nullptr;
};

// "http://www.opengis.net/def/crs/EPSG/0/3031", "EPSG:3031", "urn:...:3031"
static const PolarStereo *ProjectedCrs(const wxString &srs) {
  if (!srs.Upper().Contains("EPSG")) return nullptr;
  long code = 0;
  if (!srs.AfterLast(srs.Contains("/") ? '/' : ':').ToLong(&code))
    return nullptr;
  for (const auto &p : kPolar)
    if (p.epsg == code) return &p;
  return nullptr;
}

static void ScanPosNums(const wxXmlNode *n, AxisScan &s) {
  for (wxXmlAttribute *a = n->GetAttributes(); a; a = a->GetNext())
    if (a->GetName().AfterLast(':') == "srsName") {
      if (a->GetValue().Contains("CRS84")) s.crs84 = true;
      if (!s.proj) s.proj = ProjectedCrs(a->GetValue());
    }
  wxString loc = Local(n);
  if (loc == "posList" || loc == "pos") {
    std::vector<double> nums;
    NodeNums(n, s.raw, s.coords, nums);
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

static Axis DetectAxis(const wxXmlNode *root, const ParseCtx &ctx,
                       const PolarStereo **proj = nullptr) {
  AxisScan s;
  s.raw = ctx.raw;
  s.coords = ctx.coords;
  ScanPosNums(root, s);
  if (proj) *proj = s.proj;
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

// A ring that encloses a pole cannot be closed in the lon/lat plane: after
// unwrapping, its end lies 360 deg east or west of its start (the winding
// sum of the longitude steps is +/-360). Close it along the pole latitude so
// it tessellates and hit-tests as the region it encloses (PS clause 4.5).
static bool ClosePolarRing(std::vector<S411Pos> &r) {
  if (r.size() < 3) return false;
  const double d = r.back().lon - r.front().lon;
  if (std::fabs(d) <= 180.0) return false;
  // the step from the last vertex back to the first, taken the short way
  double step = -d - 360.0 * std::floor((-d + 180.0) / 360.0);
  const S411Pos end = {r.front().lat, r.back().lon + step};
  if (std::fabs(end.lon - r.back().lon) > 1e-9 ||
      std::fabs(end.lat - r.back().lat) > 1e-9)
    r.push_back(end);
  double latSum = 0;
  for (const auto &p : r) latSum += p.lat;
  const double pole = latSum >= 0 ? 90.0 : -90.0;
  r.push_back({pole, end.lon});
  r.push_back({pole, r.front().lon});
  return true;
}

// concatenate every posList/pos below n (curve segments etc.)
static void CollectPositions(const wxXmlNode *n, const ParseCtx &ctx,
                             std::vector<S411Pos> &out) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    wxString loc = Local(c);
    if (loc == "posList" || loc == "pos")
      ParsePosText(c, ctx, out);
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

double S411MercY(double lat) {
  const double d2r = 3.14159265358979323846 / 180.0;
  lat = std::max(-89.5, std::min(89.5, lat));
  return std::log(std::tan(3.14159265358979323846 / 4 + lat * d2r / 2)) / d2r;
}

static bool IsPoleVertex(const S411Pos &p) {
  return std::fabs(p.lat) >= 90.0 - 1e-9;
}

// float Mercator copies for the OpenGL fast path (see S411Feature)
static void BuildMercator(S411Feature &f) {
  if (f.kind == S411Kind::Point) return;
  f.mx0 = (f.lonMin + f.lonMax) / 2.0;
  f.my0 = S411MercY((f.latMin + f.latMax) / 2.0);
  auto put = [&f](std::vector<float> &v, const S411Pos &p) {
    v.push_back((float)(p.lon - f.mx0));
    v.push_back((float)(S411MercY(p.lat) - f.my0));
  };
  f.mTris.reserve(f.tris.size() * 2);
  for (const auto &p : f.tris) put(f.mTris, p);
  for (const auto &ring : f.rings) {
    const bool closed = f.kind == S411Kind::Area;
    const size_t n = ring.size() + (closed ? 1 : 0);
    std::vector<float> strip;
    for (size_t i = 0; i < n; i++) {
      const S411Pos &p = ring[i % ring.size()];
      if (IsPoleVertex(p)) {  // the synthetic pole closure is not an edge
        if (strip.size() >= 4) f.mStrips.push_back(strip);
        strip.clear();
        continue;
      }
      put(strip, p);
    }
    if (strip.size() >= 4) f.mStrips.push_back(strip);
  }
}

void S411FinishFeature(S411Feature &f) {
  if (f.kind == S411Kind::Line && !f.rings.empty()) UnwrapLons(f.rings[0]);
  if (f.kind == S411Kind::Area) {
    for (auto &r : f.rings) {
      UnwrapLons(r);
      ClosePolarRing(r);
    }
    // keep the other rings on the same world copy as the first exterior
    // (centre-based, so it also works when that exterior spans 360 deg)
    if (f.rings.size() > 1) {
      auto centre = [](const std::vector<S411Pos> &r) {
        double a = r[0].lon, b = r[0].lon;
        for (const auto &p : r) { a = std::min(a, p.lon); b = std::max(b, p.lon); }
        return (a + b) / 2.0;
      };
      const double ref = centre(f.rings[0]);
      for (size_t i = 1; i < f.rings.size(); i++) {
        double k =
            360.0 * std::floor((centre(f.rings[i]) - ref + 180.0) / 360.0);
        if (k != 0.0)
          for (auto &p : f.rings[i]) p.lon -= k;
      }
    }
    S411Triangulate(f.rings, f.tris);
  }
  for (const auto &ring : f.rings)
    for (const auto &p : ring) {
      if (p.lat < f.latMin) f.latMin = p.lat;
      if (p.lat > f.latMax) f.latMax = p.lat;
      if (p.lon < f.lonMin) f.lonMin = p.lon;
      if (p.lon > f.lonMax) f.lonMax = p.lon;
    }
  BuildMercator(f);
}

// --- WMO egg code -----------------------------------------------------------
static wxString ChildText(const wxXmlNode *n, const wxString &loc) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext())
    if (IsElem(c) && Local(c) == loc)
      return c->GetNodeContent().Trim().Trim(false);
  return "";
}

// "[95, 87]" / "95" -> {"95", "87"} (Edition 1.x S-57 style lists)
static std::vector<wxString> V1List(wxString v) {
  std::vector<wxString> out;
  v.Replace("[", "");
  v.Replace("]", "");
  wxStringTokenizer tk(v, ",", wxTOKEN_STRTOK);
  while (tk.HasMoreTokens()) {
    wxString t = tk.GetNextToken().Trim().Trim(false);
    if (t.length() == 1 && wxIsdigit(t[0])) t = "0" + t;
    out.push_back(t);
  }
  return out;
}

static void ParseEgg(const wxXmlNode *node, const ParseCtx &ctx,
                     S411Feature &f) {
  S411Egg &e = f.egg;
  e.ct = f.totalConcentration;
  if (ctx.v1) {  // parallel lists iceapc=[Ca..] icesod=[Sa..] iceflz=[Fa..]
    if (e.ct == "98" || e.ct == "01" || e.ct == "02") return;
    const wxXmlNode *a = FindDesc(node, "iceapc"), *s = FindDesc(node, "icesod"),
                    *fz = FindDesc(node, "iceflz");
    const auto apc = V1List(a ? a->GetNodeContent() : wxString());
    const auto sod = V1List(s ? s->GetNodeContent() : wxString());
    const auto flz = V1List(fz ? fz->GetNodeContent() : wxString());
    const size_t n = std::min<size_t>(3, std::max({apc.size(), sod.size(),
                                                   flz.size()}));
    for (size_t i = 0; i < n; i++) {
      S411EggColumn col;
      if (i < apc.size()) col.c = CleanConcV1(apc[i]);
      const wxString st = i < sod.size() ? sod[i] : wxString();
      const wxString fm = i < flz.size() ? flz[i] : wxString();
      if (!st.IsEmpty() || !fm.IsEmpty()) col.sf.push_back({st, fm});
      if (!col.c.IsEmpty() || !col.sf.empty()) e.cols.push_back(col);
    }
    return;
  }
  for (const wxXmlNode *g = node->GetChildren(); g; g = g->GetNext()) {
    if (!IsElem(g)) continue;
    const wxString loc = Local(g);
    if (loc != "iceCharacteristic" && loc != "lakeIceCharacteristic") continue;
    if (e.cols.size() == 3) break;
    S411EggColumn col;
    col.c = ChildText(g, "partialConcentration");
    for (const wxXmlNode *d = g->GetChildren(); d; d = d->GetNext()) {
      if (!IsElem(d)) continue;
      const wxString dl = Local(d);
      if (dl != "developmentAndForm" && dl != "lakeDevelopmentAndForm") continue;
      wxString st = ChildText(d, "stageOfDevelopment");
      if (st.IsEmpty()) st = ChildText(d, "lakeIceStageOfDevelopment");
      col.sf.push_back({st, ChildText(d, "formOfIce")});
    }
    col.breccia = ChildText(g, "iceBreccia");
    col.brecciaStage = ChildText(g, "brecciaConsolidatingStage");
    e.cols.push_back(col);
  }
  e.so = ChildText(node, "olderIceStage");
  e.sd = ChildText(node, "remainingIceStage");
  e.cs = ChildText(node, "stripsPatchesConcentration");
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
    f.kind = S411Kind::Line;
    f.rings.push_back(line);
  } else {
    std::vector<std::vector<S411Pos>> ext, intr;
    CollectRings(geom, ctx, true, ext, intr);
    if (ext.empty()) return;
    f.kind = S411Kind::Area;
    for (auto &r : ext) f.rings.push_back(r);
    for (auto &r : intr) f.rings.push_back(r);
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
  if (f.kind == S411Kind::Area &&
      (f.type == "SeaIce" || f.type == "LakeIce" || f.type == "StripsAndPatches"))
    ParseEgg(node, ctx, f);

  if (f.kind == S411Kind::Point) {
    auto attr = [&f](const char *a, const char *b) {
      for (const auto &kv : f.attrs)
        if (kv.first == a || kv.first == b) return FirstListValue(kv.second);
      return wxString();
    };
    f.symbol = S411PointSymbolFor(f.type, attr("icebergSize", "icebsz"),
                                  attr("driftDirection", "icedir"));
    const char *va, *unit;
    if (S411ValueTextSpec(f.type, va, unit)) {
      wxString v;
      for (const auto &kv : f.attrs)
        if (kv.first == va || (f.type == "IceThickness" && kv.first == "icetck") ||
            (f.type == "IceDrift" && kv.first == "icespd")) {
          v = kv.second;
          break;
        }
      if (!v.IsEmpty()) f.valueText = v + " " + unit;
    }
  }

  S411FinishFeature(f);
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
        if (loc != "DataCoverage") {
          ParseFeature(c, geom, ctx, ds);
        } else {  // coverage is not portrayed, but drives precedence
          std::vector<std::vector<S411Pos>> ext, intr;
          CollectRings(geom, ctx, true, ext, intr);
          for (auto &r : ext) {
            UnwrapLons(r);
            ClosePolarRing(r);
            ds.coverage.push_back(r);
          }
        }
        continue;  // features are not nested
      }
    }
    Walk(c, ctx, ds);
  }
}

// Analysis date from a file name when the data carries none (Edition 1.x):
// "..._20260701..." / "...20260622T1800Z..." or the S-100 dataset name
// pattern 411PPPPxxxYYMMDD.
time_t S411TimeFromName(const wxString &stem) {
  const std::string s = std::string(stem.ToAscii());
  for (size_t i = 0; i + 8 <= s.size(); i++) {
    if (s.compare(i, 2, "20") != 0) continue;
    if (i > 0 && isdigit((unsigned char)s[i - 1])) continue;
    bool digits = true;
    for (size_t k = 0; k < 8; k++)
      digits = digits && isdigit((unsigned char)s[i + k]);
    if (!digits || (i + 8 < s.size() && isdigit((unsigned char)s[i + 8])))
      continue;
    std::string iso = s.substr(i, 8);
    if (i + 13 <= s.size() && s[i + 8] == 'T' &&
        isdigit((unsigned char)s[i + 9]) && isdigit((unsigned char)s[i + 12]))
      iso += "T" + s.substr(i + 9, 2) + ":" + s.substr(i + 11, 2) + ":00";
    const time_t t = S411ParseIsoTime(wxString(iso));
    if (t) return t;
  }
  if (s.size() >= 13 && s.compare(0, 3, "411") == 0) {
    const std::string tail = s.substr(s.size() - 6);
    bool digits = true;
    for (char c : tail) digits = digits && isdigit((unsigned char)c);
    if (digits) return S411ParseIsoTime(wxString("20" + tail));
  }
  return 0;
}

static bool ParseDoc(wxXmlDocument &doc, const wxString &source,
                     S411Dataset &out, wxString &err,
                     const std::string *raw = nullptr,
                     const CoordTable *coords = nullptr) {
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
  ctx.raw = raw;
  ctx.coords = coords;
  ctx.axis = DetectAxis(root, ctx, &ctx.proj);
  out.file = source;
  const wxXmlNode *title = FindDesc(root, "datasetTitle");
  const wxString stem = wxFileName(source.AfterLast('!')).GetName();
  out.title = title ? title->GetNodeContent().Trim().Trim(false) : stem;
  const wxXmlNode *dsid = FindDesc(root, "datasetFileIdentifier");
  out.datasetId = dsid ? dsid->GetNodeContent().Trim().Trim(false) : stem;
  // S-100 Part 17-4.3 dataset names: "411" + 4-character producer code
  if (out.datasetId.StartsWith("411") && out.datasetId.length() >= 7)
    out.producer = out.datasetId.Mid(3, 4);
  const wxXmlNode *at = FindDesc(root, "analysisDateTime");
  if (!at) at = FindDesc(root, "reportDateTime");  // ice report datasets
  if (at) out.analysisTime = S411ParseIsoTime(at->GetNodeContent());
  const wxXmlNode *ft = FindDesc(root, "forecastDateTime");
  if (ft) out.forecastTime = S411ParseIsoTime(ft->GetNodeContent());
  if (!out.analysisTime) {
    out.analysisTime = S411TimeFromName(stem);
    out.timeApprox = out.analysisTime != 0;
  }
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

bool S411AreaContains(const S411Feature &f, double lat, double lon) {
  if (f.kind != S411Kind::Area || f.rings.empty()) return false;
  const double c = (f.lonMin + f.lonMax) / 2.0;
  const double lonW = lon + 360.0 * std::floor((c - lon + 180.0) / 360.0);
  if (lat < f.latMin || lat > f.latMax || lonW < f.lonMin || lonW > f.lonMax)
    return false;
  return PointInRings(f.rings, lat, lonW);
}

size_t S411HitTest(const std::vector<S411Dataset> &data, double lat,
                   double lon, double tolDeg, std::vector<S411Hit> &out) {
  out.clear();
  for (const auto &ds : data) {
    if (!ds.visible) continue;
    for (const auto &f : ds.features) {
      if (f.rings.empty() || f.rings[0].empty() || f.superseded) continue;
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

// ---------------------------------------------------------------------------
// loading: every dataset is read into memory first, so that its exact bytes
// can be checked against the exchange catalogue signature (S-100 Part 15)
// ---------------------------------------------------------------------------
namespace {
struct CatCtx {
  S411Catalog cat;
  S411SigState catSig = S411SigState::None;  // CATALOG.SIGN over CATALOG.XML
  wxString catNote;
};
}  // namespace

static bool ReadFile(const wxString &path, std::string &out) {
  wxFFile f(path, "rb");
  if (!f.IsOpened()) return false;
  const wxFileOffset n = f.Length();
  if (n < 0) return false;
  out.resize((size_t)n);
  return n == 0 || f.Read(&out[0], (size_t)n) == (size_t)n;
}

static void PrepareCatalog(const std::string &catBytes,
                           const std::string *signBytes, CatCtx &cc) {
  if (!S411ParseCatalog(catBytes, cc.cat)) return;
  if (signBytes && S411ParseCatalogSign(*signBytes, cc.cat))
    cc.catSig = S411VerifyData(cc.cat, catBytes, cc.cat.catalogSignature,
                               cc.cat.catalogCertRef, cc.catNote);
}

// join one parsed dataset with its catalogue entry
static void ApplyCatalog(const CatCtx *cc, const wxString &source,
                         const std::string &bytes, S411Dataset &ds) {
  if (!cc || !cc->cat.ok) return;
  const S411Catalog &cat = cc->cat;
  if (cat.edition1) {
    if (!cat.agency.IsEmpty() && ds.agency.IsEmpty()) ds.agency = cat.agency;
    if (ds.producer.IsEmpty()) ds.producer = cat.agency;
    if (!ds.analysisTime && cat.issued) {  // a name with a time is better
      ds.analysisTime = cat.issued;
      ds.timeApprox = true;
    }
    return;
  }
  const S411CatalogEntry *e = cat.Find(source.AfterLast('!'));
  if (!e) return;
  if (!e->producerCode.IsEmpty()) ds.producer = e->producerCode;
  if (!e->agency.IsEmpty()) ds.agency = e->agency;
  if (e->validUntil) ds.validUntil = e->validUntil;
  if (!ds.analysisTime && e->validFrom) ds.analysisTime = e->validFrom;
  ds.signature = S411VerifyData(cat, bytes, e->signature, e->certRef,
                                ds.signatureNote);
  // a broken catalogue signature invalidates everything it lists
  if (cc->catSig == S411SigState::Invalid) {
    ds.signature = S411SigState::Invalid;
    ds.signatureNote = "CATALOG.SIGN does not verify: " + cc->catNote;
  }
  // PS clause 11.3 rule 4: data whose signature fails is not displayed
  if (ds.signature == S411SigState::Invalid) ds.visible = false;
}

static bool LoadBytes(const std::string &bytes, const wxString &source,
                      const CatCtx *cc, S411Dataset &out, wxString &err) {
  wxXmlDocument doc;
  std::string slim;
  CoordTable coords;
  LiftCoordinates(bytes, slim, coords);
  {
    wxLogNull quiet;
    wxMemoryInputStream in(slim.data(), slim.size());
    if (!doc.Load(in)) {
      err = wxString::Format("%s: not a readable XML/GML file", source);
      return false;
    }
  }
  if (!ParseDoc(doc, source, out, err, &bytes, &coords)) return false;
  ApplyCatalog(cc, source, bytes, out);
  // forecasts are not displayed by default (PS clause 4.6, Table 4-2)
  if (out.forecastTime) out.visible = false;
  return true;
}

// the exchange catalogue a loose dataset file belongs to, if any: walk up
// to four directories looking for CATALOG.XML (5.2) or catalogue.xml (1.x)
static bool FindCatalogFor(const wxString &path, CatCtx &cc) {
  wxFileName dir(path);
  dir.SetFullName("");
  for (int up = 0; up < 5 && dir.GetDirCount() > 0; up++) {
    for (const char *name : {"CATALOG.XML", "catalogue.xml"}) {
      wxFileName c(dir.GetPath(), name);
      std::string bytes, sign;
      if (c.FileExists() && ReadFile(c.GetFullPath(), bytes)) {
        wxFileName sg(dir.GetPath(), "CATALOG.SIGN");
        const bool haveSign =
            sg.FileExists() && ReadFile(sg.GetFullPath(), sign);
        PrepareCatalog(bytes, haveSign ? &sign : nullptr, cc);
        if (cc.cat.ok) return true;
      }
    }
    dir.RemoveLastDir();
  }
  return false;
}

static bool IsGzipBytes(const std::string &b) {
  return b.size() >= 2 && (unsigned char)b[0] == 0x1f &&
         (unsigned char)b[1] == 0x8b;
}

static bool IsZipBytes(const std::string &b) {
  return b.size() >= 4 && b.compare(0, 4, "PK\x03\x04") == 0;
}

// gzip (.gz) -> plain bytes; false when the stream is broken
static bool Gunzip(const std::string &in, std::string &out) {
  wxMemoryInputStream mem(in.data(), in.size());
  wxZlibInputStream z(mem, wxZLIB_GZIP);
  out.clear();
  char buf[65536];
  while (true) {
    z.Read(buf, sizeof buf);
    const size_t n = z.LastRead();
    if (!n) break;
    out.append(buf, n);
  }
  return z.GetLastError() == wxSTREAM_EOF || z.GetLastError() == wxSTREAM_NO_ERROR;
}

// "x.gml.gz" -> "x.gml"
static wxString StripGz(const wxString &name) {
  return name.Lower().EndsWith(".gz") ? name.Left(name.length() - 3) : name;
}

bool LoadS411File(const wxString &path, S411Dataset &out, wxString &err) {
  std::string bytes;
  if (!ReadFile(path, bytes)) {
    err = wxString::Format("%s: cannot read file", path);
    return false;
  }
  if (IsGzipBytes(bytes)) {
    std::string plain;
    if (!Gunzip(bytes, plain)) {
      err = wxString::Format("%s: broken gzip file", path);
      return false;
    }
    bytes.swap(plain);
  }
  CatCtx cc;
  const bool haveCat = FindCatalogFor(path, cc);
  return LoadBytes(bytes, StripGz(path), haveCat ? &cc : nullptr, out, err);
}

static bool IsGmlName(const wxString &name) {
  wxString l = name.Lower();
  return l.EndsWith(".gml");
}

static bool IsCatalogName(const wxString &base) {
  return base.Upper() == "CATALOG.XML" || base.Lower() == "catalogue.xml";
}

static size_t LoadZipBytes(const std::string &zipBytes, const wxString &zipPath,
                           std::vector<S411Dataset> &out, wxString &errors,
                           int depth) {
  wxMemoryInputStream in(zipBytes.data(), zipBytes.size());
  // read every relevant entry first: catalogues may come after the data
  wxZipInputStream zip(in);
  std::map<wxString, std::string> files;  // entry path ('/' separated)
  std::vector<wxString> gmls, shps, nested;
  std::unique_ptr<wxZipEntry> entry;
  while (entry.reset(zip.GetNextEntry()), entry) {
    if (entry->IsDir()) continue;
    wxString name = entry->GetName(wxPATH_UNIX);
    const bool gz = name.Lower().EndsWith(".gz");
    if (gz) name = StripGz(name);  // x.gml.gz is handled as x.gml
    const wxString base = name.AfterLast('/');
    const bool gml = IsGmlName(base);
    const wxString ext = base.AfterLast('.').Lower();
    const bool shpPart = ext == "shp" || ext == "dbf" || ext == "prj";
    if (!gml && !shpPart && ext != "zip" && !IsCatalogName(base) &&
        base.Upper() != "CATALOG.SIGN")
      continue;
    if (!zip.OpenEntry(*entry)) continue;
    std::string data;
    char buf[65536];
    while (true) {
      zip.Read(buf, sizeof buf);
      const size_t n = zip.LastRead();
      if (!n) break;
      data.append(buf, n);
    }
    if (gz) {
      std::string plain;
      if (!Gunzip(data, plain)) {
        errors += zipPath + "!" + name + ".gz: broken gzip data\n";
        continue;
      }
      data.swap(plain);
    }
    files[name] = data;
    if (gml) gmls.push_back(name);
    if (ext == "shp") shps.push_back(name);
    if (ext == "zip") nested.push_back(name);
  }
  if (gmls.empty() && shps.empty() && nested.empty()) {
    errors += zipPath + ": no .gml/.GML or SIGRID-3 .shp entries in archive\n";
    return 0;
  }

  // nearest catalogue up the directory tree of each dataset entry
  std::map<wxString, CatCtx> cats;  // directory -> its catalogue
  auto catalogFor = [&](const wxString &gml) -> const CatCtx * {
    wxString dir = gml.Contains("/") ? gml.BeforeLast('/') : wxString();
    for (int up = 0; up < 5; up++) {
      const wxString pre = dir.IsEmpty() ? wxString() : dir + "/";
      auto have = cats.find(dir);
      if (have != cats.end() && have->second.cat.ok) return &have->second;
      for (const auto &kv : files) {
        if (!kv.first.StartsWith(pre)) continue;
        const wxString rest = kv.first.Mid(pre.length());
        if (rest.Contains("/") || !IsCatalogName(rest)) continue;
        CatCtx &cc = cats[dir];
        const std::string *sign = nullptr;
        auto s = files.find(pre + "CATALOG.SIGN");
        if (s != files.end()) sign = &s->second;
        PrepareCatalog(kv.second, sign, cc);
        if (cc.cat.ok) return &cc;
      }
      if (dir.IsEmpty()) break;
      dir = dir.Contains("/") ? dir.BeforeLast('/') : wxString();
    }
    return nullptr;
  };

  size_t loaded = 0;
  for (const auto &g : gmls) {
    S411Dataset ds;
    wxString err;
    if (LoadBytes(files[g], zipPath + "!" + g, catalogFor(g), ds, err)) {
      out.push_back(ds);
      loaded++;
    } else {
      errors += err + "\n";
    }
  }
  // SIGRID-3 shapefiles: .shp with its .dbf (and .prj) of the same name
  for (const auto &shp : shps) {
    const wxString stem = shp.BeforeLast('.').Lower();
    const std::string empty;
    const std::string *dbf = &empty, *prj = &empty;
    for (const auto &kv : files) {
      if (kv.first.BeforeLast('.').Lower() != stem) continue;
      const wxString e = kv.first.AfterLast('.').Lower();
      if (e == "dbf") dbf = &kv.second;
      if (e == "prj") prj = &kv.second;
    }
    S411Dataset ds;
    wxString err;
    if (S411LoadSigrid(files[shp], *dbf, *prj, zipPath + "!" + shp, ds, err)) {
      out.push_back(ds);
      loaded++;
    } else {
      errors += err + "\n";
    }
  }
  // zips inside the zip (e.g. a season of daily exchange sets)
  for (const auto &z : nested) {
    if (depth >= 3) {
      errors += zipPath + "!" + z + ": archives nested too deeply\n";
      continue;
    }
    loaded += LoadZipBytes(files[z], zipPath + "!" + z, out, errors, depth + 1);
  }
  return loaded;
}

size_t LoadS411Zip(const wxString &zipPath, std::vector<S411Dataset> &out,
                   wxString &errors) {
  std::string bytes;
  if (!ReadFile(zipPath, bytes)) {
    errors += zipPath + ": cannot open file\n";
    return 0;
  }
  if (IsGzipBytes(bytes)) {  // .zip.gz
    std::string plain;
    if (!Gunzip(bytes, plain)) {
      errors += zipPath + ": broken gzip file\n";
      return 0;
    }
    bytes.swap(plain);
  }
  return LoadZipBytes(bytes, zipPath, out, errors, 0);
}

size_t S411LoadPath(const wxString &path, std::vector<S411Dataset> &out,
                    wxString &errors) {
  const wxFileName fn(path);
  if (fn.GetFullName().Upper() == "CATALOG.XML")
    return LoadS411ExchangeSet(path, out, errors);
  wxString err;
  S411Dataset ds;
  if (fn.GetExt().Lower() == "shp") {
    if (S411LoadSigridFile(path, ds, err)) {
      out.push_back(std::move(ds));
      return 1;
    }
    errors += err + "\n";
    return 0;
  }
  // archives are recognised by their content, whatever the extension
  std::string head;
  {
    wxFFile f(path, "rb");
    if (!f.IsOpened()) {
      errors += path + ": cannot open file\n";
      return 0;
    }
    char b[4];
    head.assign(b, f.Read(b, sizeof b));
  }
  bool zip = IsZipBytes(head);
  if (IsGzipBytes(head)) {  // gzip: a zip or a single dataset inside?
    std::string all, plain;
    if (ReadFile(path, all) && Gunzip(all, plain)) zip = IsZipBytes(plain);
  }
  if (zip) return LoadS411Zip(path, out, errors);
  if (LoadS411File(path, ds, err)) {
    out.push_back(std::move(ds));
    return 1;
  }
  errors += err + "\n";
  return 0;
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
  // deliberately catalogue-agnostic for FINDING the data: producers'
  // CATALOG.XML files differ in detail, but the dataset files always live
  // below S100_ROOT - scan them, then join each with its catalogue entry.
  wxString root = wxFileName(catalogPath).GetPath();
  wxArrayString files;
  FindGmlFiles(root, files);
  if (files.IsEmpty()) {
    errors += root + ": no .gml/.GML dataset files below the catalogue\n";
    return 0;
  }
  CatCtx cc;
  std::string catBytes, sign;
  if (ReadFile(catalogPath, catBytes)) {
    const bool haveSign = ReadFile(
        root + wxFileName::GetPathSeparator() + "CATALOG.SIGN", sign);
    PrepareCatalog(catBytes, haveSign ? &sign : nullptr, cc);
  }
  size_t loaded = 0;
  for (size_t i = 0; i < files.GetCount(); i++) {
    S411Dataset ds;
    wxString err;
    std::string bytes;
    if (!ReadFile(files[i], bytes)) {
      errors += files[i] + ": cannot read file\n";
      continue;
    }
    if (LoadBytes(bytes, files[i], cc.cat.ok ? &cc : nullptr, ds, err)) {
      out.push_back(ds);
      loaded++;
    } else {
      errors += err + "\n";
    }
  }
  return loaded;
}

// ---------------------------------------------------------------------------
// precedence of overlapping datasets (PS clause 4.6) and age (clause 8.1)
// ---------------------------------------------------------------------------
static double CopyNear(double lon, double centre) {
  return lon + 360.0 * std::floor((centre - lon + 180.0) / 360.0);
}

static bool InCoverage(const S411Dataset &ds, double lat, double lon) {
  if (ds.coverage.empty()) {
    const double w = CopyNear(lon, (ds.lonMin + ds.lonMax) / 2);
    return lat >= ds.latMin && lat <= ds.latMax && w >= ds.lonMin &&
           w <= ds.lonMax;
  }
  for (const auto &ring : ds.coverage) {
    double a = ring[0].lon, b = ring[0].lon;
    for (const auto &p : ring) {
      a = std::min(a, p.lon);
      b = std::max(b, p.lon);
    }
    if (PointInRings({ring}, lat, CopyNear(lon, (a + b) / 2))) return true;
  }
  return false;
}

static bool IsShownAnalysis(const S411Dataset &d) {
  return d.visible && !d.forecastTime && d.analysisTime &&
         d.signature != S411SigState::Invalid;
}

void S411ApplyPrecedence(std::vector<S411Dataset> &data) {
  for (auto &d : data) {
    d.supersededBy.Clear();
    for (auto &f : d.features) f.superseded = false;
  }
  for (auto &older : data) {
    if (!IsShownAnalysis(older) || older.producer.IsEmpty()) continue;
    size_t hidden = 0;
    for (const auto &newer : data) {
      if (&newer == &older || !IsShownAnalysis(newer) ||
          newer.producer != older.producer ||
          newer.analysisTime <= older.analysisTime)
        continue;
      if (newer.latMax < older.latMin || newer.latMin > older.latMax) continue;
      for (auto &f : older.features) {
        if (f.superseded) continue;
        // inside the newer coverage: box corners, edge midpoints and centre
        const double la[3] = {f.latMin, f.latMax, (f.latMin + f.latMax) / 2};
        const double lo[3] = {f.lonMin, f.lonMax, (f.lonMin + f.lonMax) / 2};
        bool inside = true;
        for (int i = 0; i < 3 && inside; i++)
          for (int j = 0; j < 3 && inside; j++)
            inside = InCoverage(newer, la[i], lo[j]);
        if (inside) {
          f.superseded = true;
          if (++hidden == older.features.size())
            older.supersededBy = newer.title;
        }
      }
    }
  }
}

wxString S411OverlappingProducers(const std::vector<S411Dataset> &data) {
  std::vector<wxString> prods;
  for (size_t i = 0; i < data.size(); i++) {
    const S411Dataset &a = data[i];
    if (!a.visible || !a.supersededBy.IsEmpty() || a.features.empty())
      continue;
    for (size_t j = i + 1; j < data.size(); j++) {
      const S411Dataset &b = data[j];
      if (!b.visible || !b.supersededBy.IsEmpty() || b.features.empty() ||
          a.producer == b.producer)
        continue;
      if (a.latMax < b.latMin || a.latMin > b.latMax) continue;
      const double sh =
          360.0 * std::floor(((b.lonMin + b.lonMax) / 2 -
                              (a.lonMin + a.lonMax) / 2 + 180.0) / 360.0);
      if (b.lonMax - sh < a.lonMin || b.lonMin - sh > a.lonMax) continue;
      for (const wxString &p : {a.producer, b.producer}) {
        const wxString n = p.IsEmpty() ? wxString("unknown producer") : p;
        if (std::find(prods.begin(), prods.end(), n) == prods.end())
          prods.push_back(n);
      }
    }
  }
  std::sort(prods.begin(), prods.end());
  wxString out;
  for (const auto &p : prods) out += (out.IsEmpty() ? "" : ", ") + p;
  return out;
}

S411Age S411DatasetAge(const S411Dataset &ds, time_t now, double limitHours) {
  if (!ds.analysisTime) return S411Age::Unknown;
  const double ageH = difftime(now, ds.analysisTime) / 3600.0;
  if (ageH > 30 * 24) return S411Age::Expired;
  const time_t until = ds.forecastTime ? ds.forecastTime : ds.validUntil;
  if (until && now > until) return S411Age::Outdated;
  if (ageH > limitHours) return S411Age::OverLimit;
  return S411Age::Current;
}

wxString S411FormatUtc(time_t t, bool dateOnly) {
  static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  // civil from days (H. Hinnant) instead of gmtime: thread-safe, portable
  long z = (long)(t / 86400);
  long sec = (long)(t % 86400);
  if (sec < 0) { sec += 86400; z--; }
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const long y = (long)yoe + era * 400 + (m <= 2);
  if (dateOnly) return wxString::Format("%u %s %ld", d, mon[m - 1], y);
  return wxString::Format("%u %s %ld %02ld:%02ld", d, mon[m - 1], y,
                          sec / 3600, (sec / 60) % 60);
}

wxString S411AgeText(const S411Dataset &ds, time_t now) {
  if (!ds.analysisTime) return "age unknown";
  const double h = difftime(now, ds.analysisTime) / 3600.0;
  wxString age;
  if (h < 0)
    age = "issued in the future";
  else if (h < 48)
    age = wxString::Format("%.0f h old", h);
  else
    age = wxString::Format("%.0f days old", h / 24.0);
  wxString s = "Ice analysis " +
               S411FormatUtc(ds.analysisTime, ds.timeApprox) +
               (ds.timeApprox ? "" : " UTC") + " - " + age;
  if (ds.forecastTime)
    s = "Forecast for " + S411FormatUtc(ds.forecastTime) + " UTC (" + s + ")";
  return s;
}
