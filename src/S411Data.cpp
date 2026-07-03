/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * GML parser: namespace-agnostic (matches on local element names) so that
 * datasets from any producer load regardless of prefix conventions.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "S411Data.h"

#include <wx/filename.h>
#include <wx/log.h>
#include <wx/tokenzr.h>
#include <wx/xml/xml.h>

#include <cmath>

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

static bool AnyDescWithText(const wxXmlNode *n, const wxString &loc,
                            const wxString &value) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    if (Local(c) == loc && c->GetNodeContent().Trim().Trim(false) == value)
      return true;
    if (AnyDescWithText(c, loc, value)) return true;
  }
  return false;
}

// posList/pos text -> coordinate pairs; S-411 uses EPSG:4326 (lat lon),
// but some producers ship lon/lat - detected per list and swapped.
static void ParsePosText(const wxString &txt, std::vector<S411Pos> &out) {
  std::vector<double> nums;
  wxStringTokenizer tk(txt, wxT(" \t\r\n,"), wxTOKEN_STRTOK);
  while (tk.HasMoreTokens()) {
    double v;
    if (tk.GetNextToken().ToCDouble(&v)) nums.push_back(v);
  }
  if (nums.size() < 2) return;
  bool swapped = false;
  for (size_t i = 0; i + 1 < nums.size(); i += 2) {
    if (std::fabs(nums[i]) > 90.0 && std::fabs(nums[i + 1]) <= 90.0) {
      swapped = true;  // first-of-pair cannot be a latitude
      break;
    }
  }
  for (size_t i = 0; i + 1 < nums.size(); i += 2) {
    double lat = swapped ? nums[i + 1] : nums[i];
    double lon = swapped ? nums[i] : nums[i + 1];
    if (lon > 180.0) lon -= 360.0;  // 0..360 producers (AARI, NWS)
    if (std::fabs(lat) <= 90.0 && std::fabs(lon) <= 540.0)
      out.push_back({lat, lon});
  }
}

// concatenate every posList/pos below n (curve segments etc.)
static void CollectPositions(const wxXmlNode *n, std::vector<S411Pos> &out) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    wxString loc = Local(c);
    if (loc == "posList" || loc == "pos")
      ParsePosText(c->GetNodeContent(), out);
    else
      CollectPositions(c, out);
  }
}

// collect polygon rings: exterior rings first, then interiors (even-odd)
static void CollectRings(const wxXmlNode *n, bool exterior,
                         std::vector<std::vector<S411Pos>> &ext,
                         std::vector<std::vector<S411Pos>> &intr) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    wxString loc = Local(c);
    if (loc == "exterior") {
      CollectRings(c, true, ext, intr);
    } else if (loc == "interior") {
      CollectRings(c, false, ext, intr);
    } else if (loc == "LinearRing" || loc == "Ring") {
      std::vector<S411Pos> ring;
      CollectPositions(c, ring);
      if (ring.size() >= 3) (exterior ? ext : intr).push_back(ring);
    } else {
      CollectRings(c, exterior, ext, intr);
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

static void ParseFeature(const wxXmlNode *node, const wxXmlNode *geom,
                         S411Dataset &ds) {
  S411Feature f;
  f.type = Local(node);
  wxString gloc = Local(geom);
  if (gloc == "Point") {
    std::vector<S411Pos> p;
    CollectPositions(geom, p);
    if (p.empty()) return;
    f.kind = S411Kind::Point;
    f.rings.push_back({p[0]});
  } else if (gloc == "LineString" || gloc == "Curve" ||
             gloc == "CompositeCurve") {
    std::vector<S411Pos> line;
    CollectPositions(geom, line);
    if (line.size() < 2) return;
    f.kind = S411Kind::Line;
    f.rings.push_back(line);
  } else {
    std::vector<std::vector<S411Pos>> ext, intr;
    CollectRings(geom, true, ext, intr);
    if (ext.empty()) return;
    f.kind = S411Kind::Area;
    for (auto &r : ext) f.rings.push_back(r);
    for (auto &r : intr) f.rings.push_back(r);
  }
  const wxXmlNode *ct = FindDesc(node, "totalConcentration");
  if (ct) f.totalConcentration = ct->GetNodeContent().Trim().Trim(false);
  f.fastIce = AnyDescWithText(node, "formOfIce", "09");
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
static void Walk(const wxXmlNode *n, S411Dataset &ds) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    wxString loc = Local(c);
    if (loc == "boundedBy" || loc == "DatasetIdentificationInformation")
      continue;
    if (loc != "Dataset" && !c->GetAttribute("gml:id", "").IsEmpty()) {
      const wxXmlNode *geom = FindGeometry(c);
      if (geom) {
        if (loc != "DataCoverage")  // coverage extent is not portrayed
          ParseFeature(c, geom, ds);
        continue;  // features are not nested
      }
      // information types (IceReport, ...) carry no geometry - skip subtree
      // only if it is clearly an identified object without geometry? They may
      // still contain nothing renderable; keep walking is harmless.
    }
    Walk(c, ds);
  }
}

bool LoadS411File(const wxString &path, S411Dataset &out, wxString &err) {
  wxXmlDocument doc;
  {
    wxLogNull quiet;  // wxXml reports its own errors; we produce our own
    if (!doc.Load(path)) {
      err = wxString::Format("%s: not a readable XML/GML file", path);
      return false;
    }
  }
  out.file = path;
  const wxXmlNode *root = doc.GetRoot();
  if (!root) {
    err = wxString::Format("%s: empty document", path);
    return false;
  }
  const wxXmlNode *title = FindDesc(root, "datasetTitle");
  out.title = title ? title->GetNodeContent().Trim().Trim(false)
                    : wxFileName(path).GetName();
  Walk(root, out);
  if (out.features.empty()) {
    err = wxString::Format("%s: no S-411 features with geometry found", path);
    return false;
  }
  return true;
}
