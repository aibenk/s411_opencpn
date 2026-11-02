/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * SIGRID-3 shapefile reader (WMO TD No. 1214, the ice chart exchange format
 * several ice services publish next to - or instead of - S-411).
 *
 *  .shp  polygons (types 5/15/25); parts are rings, outer rings and holes
 *        are told apart by the even-odd rule like the GML rings
 *  .dbf  the SIGRID-3 egg code: POLY_TYPE, CT (total concentration),
 *        SA (stage of development of the thickest ice), FA (its form) ...
 *  .prj  geographic (lon/lat) or polar stereographic WKT; without a .prj
 *        the coordinates must be degrees
 *
 * The codes are those of the Ice Objects Catalogue that S-411 uses, except
 * "ice free", which SIGRID-3 writes as CT 00 (S-411: 98).
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include <wx/ffile.h>
#include <wx/filename.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "S411Data.h"

namespace {

int32_t BE32(const unsigned char *p) {
  return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                   (uint32_t)p[2] << 8 | p[3]);
}
int32_t LE32(const unsigned char *p) {
  return (int32_t)((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
                   (uint32_t)p[1] << 8 | p[0]);
}
double LEDouble(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
  double d;
  std::memcpy(&d, &v, sizeof d);
  return d;
}

// DBF (dBASE III) table: field names upper-cased, values trimmed
struct Dbf {
  std::vector<wxString> names;
  std::vector<std::pair<size_t, size_t>> fields;  // offset in record, length
  size_t records = 0, headerLen = 0, recordLen = 0;
  const std::string *bytes = nullptr;

  bool Parse(const std::string &b) {
    bytes = &b;
    const unsigned char *u = (const unsigned char *)b.data();
    if (b.size() < 32) return false;
    records = (size_t)LE32(u + 4);
    headerLen = u[8] | u[9] << 8;
    recordLen = u[10] | u[11] << 8;
    size_t off = 1;  // deletion flag
    for (size_t p = 32; p + 32 <= headerLen && p < b.size() && u[p] != 0x0D;
         p += 32) {
      char name[12] = {0};
      std::memcpy(name, u + p, 11);
      names.push_back(wxString(name, wxConvISO8859_1).Upper().Trim());
      fields.push_back({off, u[p + 16]});
      off += u[p + 16];
    }
    return !names.empty() && headerLen + records * recordLen <= b.size() + 1;
  }
  wxString Get(size_t rec, size_t field) const {
    const size_t at = headerLen + rec * recordLen + fields[field].first;
    if (at + fields[field].second > bytes->size()) return "";
    return wxString(bytes->data() + at, wxConvISO8859_1, fields[field].second)
        .Trim()
        .Trim(false);
  }
  int Index(const char *name) const {
    for (size_t i = 0; i < names.size(); i++)
      if (names[i] == name) return (int)i;
    return -1;
  }
};

double WktParam(const wxString &wkt, const char *name, bool &found) {
  const wxString up = wkt.Upper();
  const int p = up.Find(wxString::Format("PARAMETER[\"%s\"", wxString(name).Upper()));
  found = p != wxNOT_FOUND;
  if (!found) return 0;
  double v = 0;
  wxString rest = wkt.Mid(p).AfterFirst(',').BeforeFirst(']');
  found = rest.Trim().Trim(false).ToCDouble(&v);
  return v;
}

// SIGRID-3 two-digit code: pad, drop nulls (-9, empty)
wxString Code(wxString v) {
  v.Trim().Trim(false);
  if (v.IsEmpty() || v.StartsWith("-")) return "";
  if (v.length() == 1 && wxIsdigit(v[0])) v = "0" + v;
  return v;
}

}  // namespace

bool S411LoadSigrid(const std::string &shp, const std::string &dbfBytes,
                    const std::string &prj, const wxString &source,
                    S411Dataset &out, wxString &err) {
  const unsigned char *u = (const unsigned char *)shp.data();
  if (shp.size() < 100 || BE32(u) != 9994) {
    err = source + ": not an ESRI shapefile";
    return false;
  }
  Dbf dbf;
  if (!dbf.Parse(dbfBytes)) {
    err = source + ": missing or unreadable .dbf attribute table";
    return false;
  }

  // coordinate system from the .prj (WKT)
  const wxString wkt(prj.data(), wxConvISO8859_1, prj.size());
  bool polar = false;
  double latTs = 0, lon0 = 0, fe = 0, fn = 0;
  if (wkt.Upper().Contains("PROJCS")) {
    if (!wkt.Upper().Contains("STEREOGRAPHIC")) {
      err = source + ": projection not supported (only geographic and polar "
                     "stereographic SIGRID-3 files)";
      return false;
    }
    bool f1, f2, f3, f4;
    latTs = WktParam(wkt, "standard_parallel_1", f1);
    if (!f1) latTs = WktParam(wkt, "latitude_of_origin", f1);
    lon0 = WktParam(wkt, "central_meridian", f2);
    if (!f2) lon0 = WktParam(wkt, "longitude_of_origin", f2);
    fe = WktParam(wkt, "false_easting", f3);
    fn = WktParam(wkt, "false_northing", f4);
    if (!f1 || std::fabs(latTs) < 1.0) {  // UPS-style (scale factor) aspect
      err = source + ": polar stereographic without a latitude of true scale "
                     "is not supported";
      return false;
    }
    if (wkt.Upper().Contains("SOUTH_POLE") && latTs > 0) latTs = -latTs;
    polar = true;
  }

  const int iType = dbf.Index("POLY_TYPE"), iCT = dbf.Index("CT"),
            iSA = dbf.Index("SA"), iFA = dbf.Index("FA");
  const wxString stem = wxFileName(source.AfterLast('!')).GetName();
  out.title = stem;
  out.file = source;
  out.datasetId = stem;
  out.analysisTime = S411TimeFromName(stem);
  out.timeApprox = out.analysisTime != 0;

  size_t pos = 100, rec = 0;
  bool outOfRange = false;
  while (pos + 8 <= shp.size()) {
    const size_t len = (size_t)BE32(u + pos + 4) * 2;
    const unsigned char *r = u + pos + 8;
    const size_t idx = rec++;
    pos += 8 + len;
    if (pos > shp.size() || len < 44) continue;
    const int type = LE32(r);
    if (type != 5 && type != 15 && type != 25) continue;  // polygons only
    const int nParts = LE32(r + 36), nPoints = LE32(r + 40);
    if (nParts <= 0 || nPoints <= 0 ||
        44 + 4 * (size_t)nParts + 16 * (size_t)nPoints > len)
      continue;
    if (idx >= dbf.records) break;

    const wxString pt = iType >= 0 ? dbf.Get(idx, iType).Upper() : "I";
    if (pt == "L" || pt == "N") continue;  // land, no data
    S411Feature f;
    f.type = "SeaIce";
    f.kind = S411Kind::Area;
    wxString ct = iCT >= 0 ? Code(dbf.Get(idx, iCT)) : wxString();
    if (ct == "00") ct = "98";             // SIGRID ice free -> S-411
    if (ct.IsEmpty() && pt == "W") ct = "98";
    f.totalConcentration = ct;
    if (iSA >= 0) f.stageOfDevelopment = Code(dbf.Get(idx, iSA));
    if (iFA >= 0) f.fastIce = Code(dbf.Get(idx, iFA)) == "08";
    for (size_t k = 0; k < dbf.names.size(); k++) {
      const wxString v = dbf.Get(idx, k);
      if (!v.IsEmpty() && !v.StartsWith("-9")) f.attrs.push_back({dbf.names[k], v});
    }

    const unsigned char *parts = r + 44;
    const unsigned char *pts = parts + 4 * nParts;
    for (int p = 0; p < nParts; p++) {
      const int a = LE32(parts + 4 * p);
      const int b = p + 1 < nParts ? LE32(parts + 4 * (p + 1)) : nPoints;
      if (a < 0 || b > nPoints || b - a < 3) continue;
      std::vector<S411Pos> ring;
      ring.reserve(b - a);
      for (int i = a; i < b; i++) {
        const double x = LEDouble(pts + 16 * i), y = LEDouble(pts + 16 * i + 8);
        double lat = y, lon = x;
        if (polar) S411PolarInverse(latTs, lon0, x - fe, y - fn, lat, lon);
        if (std::fabs(lat) > 90 || std::fabs(lon) > 540) {
          outOfRange = true;
          continue;
        }
        if (lon > 180.0) lon -= 360.0;
        ring.push_back({lat, lon});
      }
      if (ring.size() >= 3) f.rings.push_back(ring);
    }
    if (f.rings.empty()) continue;
    S411FinishFeature(f);
    out.features.push_back(f);
  }
  if (out.features.empty()) {
    err = source + (outOfRange ? ": coordinates are not degrees and no "
                                 "supported .prj is present"
                               : ": no SIGRID-3 ice polygons found");
    return false;
  }
  for (const auto &f : out.features) {
    out.latMin = std::min(out.latMin, f.latMin);
    out.latMax = std::max(out.latMax, f.latMax);
    out.lonMin = std::min(out.lonMin, f.lonMin);
    out.lonMax = std::max(out.lonMax, f.lonMax);
  }
  return true;
}

static bool ReadAll(const wxString &path, std::string &out) {
  wxFFile f(path, "rb");
  if (!f.IsOpened()) return false;
  const wxFileOffset n = f.Length();
  if (n < 0) return false;
  out.resize((size_t)n);
  return n == 0 || f.Read(&out[0], (size_t)n) == (size_t)n;
}

bool S411LoadSigridFile(const wxString &shpPath, S411Dataset &out,
                        wxString &err) {
  std::string shp, dbf, prj;
  if (!ReadAll(shpPath, shp)) {
    err = shpPath + ": cannot read file";
    return false;
  }
  wxFileName fn(shpPath);
  for (const char *ext : {"dbf", "DBF"}) {
    fn.SetExt(ext);
    if (dbf.empty() && fn.FileExists()) ReadAll(fn.GetFullPath(), dbf);
  }
  for (const char *ext : {"prj", "PRJ"}) {
    fn.SetExt(ext);
    if (prj.empty() && fn.FileExists()) ReadAll(fn.GetFullPath(), prj);
  }
  return S411LoadSigrid(shp, dbf, prj, shpPath, out, err);
}
