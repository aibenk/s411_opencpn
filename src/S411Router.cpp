/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Ice-aware routing: severity raster (scanline fill of the S-411 polygons),
 * A* search, string-pulling smoothing, hazard-polygon extraction.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "S411Router.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

static const double KM_PER_DEG = 111.32;
static const double NM_PER_KM = 0.539957;

static double CosLat(double lat) {
  double c = std::cos(lat * 3.14159265358979323846 / 180.0);
  return c < 0.05 ? 0.05 : c;  // keep lon metric sane near the poles
}

// planar distance in km, adequate at routing-corridor scale
static double DistKm(const S411Pos &a, const S411Pos &b) {
  const double ky = (b.lat - a.lat) * KM_PER_DEG;
  const double kx =
      (b.lon - a.lon) * KM_PER_DEG * CosLat((a.lat + b.lat) / 2.0);
  return std::sqrt(kx * kx + ky * ky);
}

static double NormLon(double lon) {
  lon -= 360.0 * std::floor((lon + 180.0) / 360.0);
  return lon;
}

// ---------------------------------------------------------------------------
// severity classification
// ---------------------------------------------------------------------------
S411CellState S411RouteSeverity(const S411Feature &f,
                                const wxString &iceClass) {
  if (f.kind != S411Kind::Area) return S411CellState::Free;
  if (f.fastIce) return S411CellState::Blocked;
  if (f.type == "IcebergArea") return S411CellState::Caution;
  const wxString tok = S411NavToken(f.totalConcentration, iceClass);
  if (tok == "ICEC9T") return S411CellState::Blocked;
  if (tok == "ICEC46" || tok == "ICEUNK") return S411CellState::Caution;
  if (tok == "ICEC13") return S411CellState::Mild;
  return S411CellState::Free;  // ICEFRE / ICEOPW
}

// ---------------------------------------------------------------------------
// raster
// ---------------------------------------------------------------------------
namespace {

struct Grid {
  double lat0 = 0, lon0 = 0;     // lower-left CELL EDGE
  double dLat = 0, dLon = 0;     // cell size in degrees
  int nx = 0, ny = 0;
  std::vector<unsigned char> cell;  // S411CellState per cell

  double CellLat(int j) const { return lat0 + (j + 0.5) * dLat; }
  double CellLon(int i) const { return lon0 + (i + 0.5) * dLon; }
  int Col(double lon) const { return (int)std::floor((lon - lon0) / dLon); }
  int Row(double lat) const { return (int)std::floor((lat - lat0) / dLat); }
  bool Inside(int i, int j) const {
    return i >= 0 && i < nx && j >= 0 && j < ny;
  }
  unsigned char &At(int i, int j) { return cell[(size_t)j * nx + i]; }
  unsigned char Get(int i, int j) const { return cell[(size_t)j * nx + i]; }
};

// world-copy shift that brings lonCentre next to refLon
double Shift(double lonCentre, double refLon) {
  return 360.0 * std::floor((lonCentre - refLon + 180.0) / 360.0);
}

// scanline-fill one feature's rings (even-odd, holes included) into the grid
void FillFeature(Grid &g, const S411Feature &f, S411CellState sev) {
  if (sev == S411CellState::Free) return;
  const double gridCentre = g.lon0 + g.nx * g.dLon / 2.0;
  const double k = Shift((f.lonMin + f.lonMax) / 2.0, gridCentre);

  int j0 = g.Row(f.latMin), j1 = g.Row(f.latMax);
  j0 = std::max(0, j0 - 1);
  j1 = std::min(g.ny - 1, j1 + 1);

  std::vector<double> xs;
  for (int j = j0; j <= j1; j++) {
    const double y = g.CellLat(j);
    xs.clear();
    for (const auto &ring : f.rings) {
      const size_t n = ring.size();
      if (n < 3) continue;
      for (size_t a = 0; a < n; a++) {
        const S411Pos &p = ring[a];
        const S411Pos &q = ring[(a + 1) % n];
        // half-open rule avoids double-counting vertices exactly on the row
        if ((p.lat <= y && q.lat > y) || (q.lat <= y && p.lat > y)) {
          const double t = (y - p.lat) / (q.lat - p.lat);
          xs.push_back(p.lon + t * (q.lon - p.lon) - k);
        }
      }
    }
    if (xs.size() < 2) continue;
    std::sort(xs.begin(), xs.end());
    for (size_t s = 0; s + 1 < xs.size(); s += 2) {
      int i0 = (int)std::ceil((xs[s] - g.lon0) / g.dLon - 0.5);
      int i1 = (int)std::floor((xs[s + 1] - g.lon0) / g.dLon - 0.5);
      i0 = std::max(0, i0);
      i1 = std::min(g.nx - 1, i1);
      for (int i = i0; i <= i1; i++)
        if ((unsigned char)sev > g.Get(i, j)) g.At(i, j) = (unsigned char)sev;
    }
  }

  // Conservative edge rasterization: the scanline above only stamps cells
  // whose CENTRE lies inside the polygon, so a Blocked barrier (or feature)
  // thinner than one grid cell could fall entirely between cell centres and
  // let A* route straight through it. Stamp every cell each ring edge passes
  // through so a non-Free feature always has a >= 1-cell footprint. This can
  // thicken a feature by at most one cell (conservative, safe).
  for (const auto &ring : f.rings) {
    const size_t n = ring.size();
    if (n < 2) continue;
    for (size_t a = 0; a < n; a++) {
      const double x0 = ring[a].lon - k, y0 = ring[a].lat;
      const double x1 = ring[(a + 1) % n].lon - k, y1 = ring[(a + 1) % n].lat;
      const int steps =
          1 + (int)std::ceil(2.0 * std::max(std::fabs(x1 - x0) / g.dLon,
                                            std::fabs(y1 - y0) / g.dLat));
      for (int s = 0; s <= steps; s++) {
        const double t = (double)s / steps;
        const int i = g.Col(x0 + t * (x1 - x0));
        const int j = g.Row(y0 + t * (y1 - y0));
        if (g.Inside(i, j) && (unsigned char)sev > g.Get(i, j))
          g.At(i, j) = (unsigned char)sev;
      }
    }
  }
}

double StateFactor(unsigned char st, const S411RouteParams &p) {
  switch ((S411CellState)st) {
    case S411CellState::Mild: return p.mildPenalty;
    case S411CellState::Caution:
      return p.avoidCaution ? -1.0 : p.cautionPenalty;
    case S411CellState::Blocked: return -1.0;  // impassable
    default: return 1.0;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// A* route computation. One attempt over a corridor sized by p's margins;
// corridorBounded is set when the failure was "no path within the corridor"
// (worth retrying with a wider margin) rather than a hard failure such as the
// destination lying in impassable ice.
// ---------------------------------------------------------------------------
static bool ComputeRouteOnce(const std::vector<S411Dataset> &data,
                             const S411Pos &start, const S411Pos &end,
                             const S411RouteParams &p,
                             const S411LandFn &crossesLand,
                             S411RouteResult &out, bool &corridorBounded) {
  out = S411RouteResult();
  corridorBounded = false;

  // search corridor
  double latMin = std::min(start.lat, end.lat);
  double latMax = std::max(start.lat, end.lat);
  double lonMin = std::min(start.lon, end.lon);
  double lonMax = std::max(start.lon, end.lon);
  const double mLat =
      std::max(p.minMarginDeg, (latMax - latMin) * p.marginFrac);
  const double mLon =
      std::max(p.minMarginDeg, (lonMax - lonMin) * p.marginFrac);
  latMin = std::max(-85.0, latMin - mLat);
  latMax = std::min(85.0, latMax + mLat);
  lonMin -= mLon;
  lonMax += mLon;

  // cell size: roughly square in kilometres, bounded by maxDim per axis
  const double midLat = (latMin + latMax) / 2.0;
  const double latKm = (latMax - latMin) * KM_PER_DEG;
  const double lonKm = (lonMax - lonMin) * KM_PER_DEG * CosLat(midLat);
  const int maxDim = std::max(32, p.maxDim);
  double cellKm = std::max(0.25, std::max(latKm, lonKm) / maxDim);

  Grid g;
  g.dLat = cellKm / KM_PER_DEG;
  g.dLon = cellKm / (KM_PER_DEG * CosLat(midLat));
  g.lat0 = latMin;
  g.lon0 = lonMin;
  g.ny = std::max(2, (int)std::ceil((latMax - latMin) / g.dLat));
  g.nx = std::max(2, (int)std::ceil((lonMax - lonMin) / g.dLon));
  g.cell.assign((size_t)g.nx * g.ny, (unsigned char)S411CellState::Free);

  for (const auto &ds : data) {
    if (!ds.visible) continue;
    for (const auto &f : ds.features)
      if (!f.superseded) FillFeature(g, f, S411RouteSeverity(f, p.iceClass));
  }

  const int si = g.Col(start.lon), sj = g.Row(start.lat);
  const int ei = g.Col(end.lon), ej = g.Row(end.lat);
  if (!g.Inside(si, sj) || !g.Inside(ei, ej)) {
    out.error = "internal error: start/end outside the search grid";
    return false;
  }
  if (StateFactor(g.Get(ei, ej), p) < 0) {
    out.error = "the destination lies in impassable ice for this ice class";
    return false;
  }
  // the ship may well START inside bad ice - allow leaving the start cell

  // A* (8-connected)
  const size_t N = (size_t)g.nx * g.ny;
  const float INF = std::numeric_limits<float>::infinity();
  std::vector<float> best(N, INF);
  std::vector<int> from(N, -1);
  typedef std::pair<float, int> QE;  // f-cost, node
  std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;

  const S411Pos endC = {g.CellLat(ej), g.CellLon(ei)};
  const int sNode = sj * g.nx + si, eNode = ej * g.nx + ei;
  best[sNode] = 0;
  open.push({(float)DistKm({g.CellLat(sj), g.CellLon(si)}, endC), sNode});

  const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  const int DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};

  while (!open.empty()) {
    const QE top = open.top();
    open.pop();
    const int node = top.second;
    if (node == eNode) break;
    const int i = node % g.nx, j = node / g.nx;
    const float gHere = best[node];
    if (top.first - DistKm({g.CellLat(j), g.CellLon(i)}, endC) >
        gHere + 1e-3)
      continue;  // stale queue entry
    const S411Pos here = {g.CellLat(j), g.CellLon(i)};
    for (int d = 0; d < 8; d++) {
      const int ni = i + DX[d], nj = j + DY[d];
      if (!g.Inside(ni, nj)) continue;
      const double factor = StateFactor(g.Get(ni, nj), p);
      if (factor < 0) continue;
      // no corner-cutting: a diagonal step may not pass between two
      // impassable cells sharing the corner
      if (DX[d] != 0 && DY[d] != 0 &&
          (StateFactor(g.Get(i + DX[d], j), p) < 0 ||
           StateFactor(g.Get(i, j + DY[d]), p) < 0))
        continue;
      const S411Pos there = {g.CellLat(nj), g.CellLon(ni)};
      if (crossesLand &&
          crossesLand(here.lat, NormLon(here.lon), there.lat,
                      NormLon(there.lon)))
        continue;
      const float ng = gHere + (float)(DistKm(here, there) * factor);
      const int nn = nj * g.nx + ni;
      if (ng < best[nn]) {
        best[nn] = ng;
        from[nn] = node;
        open.push({ng + (float)DistKm(there, endC), nn});
      }
    }
  }

  if (from[eNode] < 0 && eNode != sNode) {
    out.error =
        "no passable route found - the area is closed by ice (or land) for "
        "this ice class within the search corridor";
    corridorBounded = true;  // a wider corridor might contain a detour
    return false;
  }

  // raw cell path
  std::vector<S411Pos> path;
  for (int n = eNode; n >= 0; n = from[n]) {
    path.push_back({g.CellLat(n / g.nx), g.CellLon(n % g.nx)});
    if (n == sNode) break;
  }
  std::reverse(path.begin(), path.end());
  path.front() = start;
  path.back() = end;

  // string-pulling: replace zig-zag cell chains by the farthest straight
  // segment that keeps the same worst severity and stays off land
  unsigned char worst = (unsigned char)S411CellState::Free;
  for (const auto &w : path) {
    const int i = g.Col(w.lon), j = g.Row(w.lat);
    if (g.Inside(i, j)) worst = std::max(worst, g.Get(i, j));
  }
  const double stepKm = cellKm * 0.5;
  // exact grid traversal (Amanatides-Woo): visit EVERY cell the segment a->b
  // enters, so a straight string-pull segment can never slip through a
  // blocked cell between two point samples
  auto segmentOk = [&](const S411Pos &a, const S411Pos &b) {
    const double fx0 = (a.lon - g.lon0) / g.dLon, fy0 = (a.lat - g.lat0) / g.dLat;
    const double fx1 = (b.lon - g.lon0) / g.dLon, fy1 = (b.lat - g.lat0) / g.dLat;
    int i = (int)std::floor(fx0), j = (int)std::floor(fy0);
    const int iEnd = (int)std::floor(fx1), jEnd = (int)std::floor(fy1);
    const double dx = fx1 - fx0, dy = fy1 - fy0;
    const int stepI = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    const int stepJ = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    const double BIG = 1e30;
    const double tDeltaX = dx != 0 ? std::fabs(1.0 / dx) : BIG;
    const double tDeltaY = dy != 0 ? std::fabs(1.0 / dy) : BIG;
    double tMaxX = dx != 0
        ? ((stepI > 0 ? (i + 1 - fx0) : (fx0 - i)) * tDeltaX) : BIG;
    double tMaxY = dy != 0
        ? ((stepJ > 0 ? (j + 1 - fy0) : (fy0 - j)) * tDeltaY) : BIG;
    auto cellOk = [&](int ci, int cj) {
      if (!g.Inside(ci, cj)) return false;
      const unsigned char st = g.Get(ci, cj);
      return StateFactor(st, p) >= 0 && st <= worst;
    };
    if (!cellOk(i, j)) return false;
    int guard = 0, guardMax = g.nx + g.ny + 4;
    while ((i != iEnd || j != jEnd) && guard++ < guardMax) {
      if (tMaxX < tMaxY) { tMaxX += tDeltaX; i += stepI; }
      else { tMaxY += tDeltaY; j += stepJ; }
      if (!cellOk(i, j)) return false;
    }
    if (crossesLand &&
        crossesLand(a.lat, NormLon(a.lon), b.lat, NormLon(b.lon)))
      return false;
    return true;
  };

  std::vector<S411Pos> smooth;
  size_t i0 = 0;
  smooth.push_back(path[0]);
  while (i0 + 1 < path.size()) {
    size_t jBest = i0 + 1;
    const size_t jMax = std::min(path.size() - 1, i0 + 120);
    for (size_t j = jMax; j > i0 + 1; j--) {
      if (segmentOk(path[i0], path[j])) {
        jBest = j;
        break;
      }
    }
    smooth.push_back(path[jBest]);
    i0 = jBest;
  }

  // stats + normalized longitudes
  for (size_t k = 0; k + 1 < smooth.size(); k++) {
    const double km = DistKm(smooth[k], smooth[k + 1]);
    out.distanceNm += km * NM_PER_KM;
    const int steps = std::max(1, (int)(km / stepKm));
    int cautionSteps = 0;
    for (int s = 0; s < steps; s++) {
      const double t = (s + 0.5) / steps;
      const double lat = smooth[k].lat + (smooth[k + 1].lat - smooth[k].lat) * t;
      const double lon = smooth[k].lon + (smooth[k + 1].lon - smooth[k].lon) * t;
      const int ci = g.Col(lon), cj = g.Row(lat);
      if (g.Inside(ci, cj) &&
          g.Get(ci, cj) >= (unsigned char)S411CellState::Caution)
        cautionSteps++;
    }
    out.cautionNm += km * NM_PER_KM * cautionSteps / steps;
  }
  for (auto &w : smooth) w.lon = NormLon(w.lon);
  out.waypoints = smooth;
  return true;
}

bool S411ComputeIceRoute(const std::vector<S411Dataset> &data,
                         const S411Pos &startIn, const S411Pos &endIn,
                         const S411RouteParams &p,
                         const S411LandFn &crossesLand, S411RouteResult &out) {
  out = S411RouteResult();
  S411Pos start = startIn, end = endIn;
  if (!std::isfinite(start.lat) || !std::isfinite(start.lon) ||
      !std::isfinite(end.lat) || !std::isfinite(end.lon)) {
    out.error = "non-finite start/end coordinate";
    return false;
  }
  start.lon = NormLon(start.lon);
  end.lon = NormLon(end.lon);
  end.lon -= 360.0 * std::round((end.lon - start.lon) / 360.0);
  if (std::fabs(start.lat) > 85.0 || std::fabs(end.lat) > 85.0) {
    out.error = "positions above 85 degrees latitude are not supported";
    return false;
  }

  // Try the configured corridor; if the ONLY problem was that no detour fit
  // inside it, widen the margins and retry (a hard failure such as the
  // destination lying in impassable ice is returned immediately).
  S411RouteParams pr = p;
  for (int attempt = 0; attempt < 3; attempt++) {
    bool corridorBounded = false;
    if (ComputeRouteOnce(data, start, end, pr, crossesLand, out,
                         corridorBounded))
      return true;
    if (!corridorBounded) return false;  // hard failure - do not widen
    pr.marginFrac *= 2.0;
    pr.minMarginDeg *= 2.0;
  }
  return false;  // out.error already holds the "within the search corridor" msg
}

// ---------------------------------------------------------------------------
// Douglas-Peucker ring simplification
// ---------------------------------------------------------------------------
static double PointSegDist(const S411Pos &p, const S411Pos &a,
                           const S411Pos &b) {
  const double c = CosLat(p.lat);
  const double ax = a.lon * c, ay = a.lat;
  const double bx = b.lon * c, by = b.lat;
  const double px = p.lon * c, py = p.lat;
  const double dx = bx - ax, dy = by - ay;
  const double L2 = dx * dx + dy * dy;
  double t = L2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / L2 : 0;
  t = std::max(0.0, std::min(1.0, t));
  const double qx = ax + t * dx, qy = ay + t * dy;
  return std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
}

void S411SimplifyRing(const std::vector<S411Pos> &in, size_t maxPoints,
                      std::vector<S411Pos> &out) {
  out.clear();
  if (in.size() <= maxPoints || in.size() < 4) {
    out = in;
    return;
  }
  // iterative Douglas-Peucker by priority: repeatedly keep the point with
  // the largest deviation until maxPoints are kept
  const size_t n = in.size();
  std::vector<bool> keep(n, false);
  keep[0] = keep[n - 1] = true;
  size_t kept = 2;

  struct Span {
    double err;
    size_t a, b, worst;
    bool operator<(const Span &o) const { return err < o.err; }
  };
  auto spanOf = [&](size_t a, size_t b) {
    Span s{-1, a, b, a};
    for (size_t i = a + 1; i < b; i++) {
      const double e = PointSegDist(in[i], in[a], in[b]);
      if (e > s.err) {
        s.err = e;
        s.worst = i;
      }
    }
    return s;
  };
  std::priority_queue<Span> pq;
  if (n > 2) pq.push(spanOf(0, n - 1));
  while (!pq.empty() && kept < maxPoints) {
    Span s = pq.top();
    pq.pop();
    if (s.err <= 0 || s.b - s.a < 2) continue;
    keep[s.worst] = true;
    kept++;
    if (s.worst - s.a >= 2) pq.push(spanOf(s.a, s.worst));
    if (s.b - s.worst >= 2) pq.push(spanOf(s.worst, s.b));
  }
  for (size_t i = 0; i < n; i++)
    if (keep[i]) out.push_back(in[i]);
}

// ---------------------------------------------------------------------------
// hazard-zone extraction for boundary export
// ---------------------------------------------------------------------------
size_t S411ExtractHazards(const std::vector<S411Dataset> &data,
                          const wxString &iceClass, bool includeCaution,
                          size_t maxPoints,
                          std::vector<S411HazardZone> &out) {
  out.clear();
  for (const auto &ds : data) {
    if (!ds.visible) continue;
    for (const auto &f : ds.features) {
      if (f.kind != S411Kind::Area || f.rings.empty() || f.superseded)
        continue;
      const S411CellState sev = S411RouteSeverity(f, iceClass);
      if (sev != S411CellState::Blocked &&
          !(includeCaution && sev == S411CellState::Caution))
        continue;
      // exterior ring(s) only: holes inside a hazard stay hazard for routing
      // purposes (conservative)
      S411HazardZone z;
      z.severity = sev;
      z.source = ds.title;
      S411SimplifyRing(f.rings[0], maxPoints, z.ring);
      for (auto &pnt : z.ring) pnt.lon = NormLon(pnt.lon);
      if (z.ring.size() >= 3) out.push_back(z);
    }
  }
  return out.size();
}

// ---------------------------------------------------------------------------
// route ice check (PS clause 9.6 alerts along an existing route)
// ---------------------------------------------------------------------------
// parameter t in [0,1] along a-b where it crosses c-d, or -1
static double SegCross(const S411Pos &a, const S411Pos &b, const S411Pos &c,
                       const S411Pos &d) {
  const double rx = b.lon - a.lon, ry = b.lat - a.lat;
  const double sx = d.lon - c.lon, sy = d.lat - c.lat;
  const double den = rx * sy - ry * sx;
  if (std::fabs(den) < 1e-15) return -1;
  const double t = ((c.lon - a.lon) * sy - (c.lat - a.lat) * sx) / den;
  const double u = ((c.lon - a.lon) * ry - (c.lat - a.lat) * rx) / den;
  return (t >= 0 && t <= 1 && u >= 0 && u <= 1) ? t : -1;
}

static bool InsideRings(const std::vector<std::vector<S411Pos>> &rings,
                        const S411Pos &p) {
  bool in = false;
  for (const auto &ring : rings)
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
      const S411Pos &a = ring[i], &b = ring[j];
      if ((a.lat > p.lat) != (b.lat > p.lat) &&
          p.lon < (b.lon - a.lon) * (p.lat - a.lat) / (b.lat - a.lat) + a.lon)
        in = !in;
    }
  return in;
}

size_t S411CheckRoute(const std::vector<S411Dataset> &data,
                      const std::vector<S411Pos> &wps,
                      const wxString &iceClass, double bergTolNm,
                      std::vector<S411RouteAlert> &out) {
  out.clear();
  for (size_t leg = 0; leg + 1 < wps.size(); leg++) {
    S411Pos a = wps[leg], b = wps[leg + 1];
    b.lon = a.lon + NormLon(b.lon - a.lon);  // the short way round
    // earliest entry per alert kind on this leg
    struct Best { double t = 2; S411RouteAlert r; };
    Best best[4];
    auto note = [&](S411Alert al, double t, const S411Feature &f,
                    const S411Dataset &ds) {
      Best &bb = best[(int)al];
      if (t >= bb.t) return;
      bb.t = t;
      bb.r.leg = leg;
      bb.r.alert = al;
      bb.r.where = {a.lat + t * (b.lat - a.lat),
                    NormLon(a.lon + t * (b.lon - a.lon))};
      wxString what = f.type;
      if (f.fastIce) what += " (fast ice)";
      else if (!f.totalConcentration.IsEmpty()) {
        const wxString l = S411ConcLabel(f.totalConcentration);
        what += " " + (l.IsEmpty() ? f.totalConcentration : l);
      }
      bb.r.feature = what;
      bb.r.dataset = ds.title;
    };
    for (const auto &ds : data) {
      if (!ds.visible) continue;
      for (const auto &f : ds.features) {
        if (f.superseded || f.rings.empty() || f.rings[0].empty()) continue;
        const S411Alert al =
            f.kind == S411Kind::Area
                ? (f.type == "IcebergArea"
                       ? S411Alert::Icebergs
                       : S411AreaAlert(f.totalConcentration, f.fastIce,
                                       iceClass))
                : S411FeatureAlert(f.type, f.totalConcentration, f.fastIce,
                                   iceClass);
        if (al == S411Alert::None) continue;
        if (f.kind == S411Kind::Area && f.type != "SeaIce" &&
            f.type != "LakeIce" && f.type != "StripsAndPatches" &&
            f.type != "IcebergArea")
          continue;
        // the leg on the feature's world copy
        const double k = Shift((f.lonMin + f.lonMax) / 2.0,
                               (a.lon + b.lon) / 2.0);
        const S411Pos pa = {a.lat, a.lon + k}, pb = {b.lat, b.lon + k};
        const double tolDeg = f.kind == S411Kind::Point
                                  ? bergTolNm / NM_PER_KM / KM_PER_DEG
                                  : 0.0;
        if (std::max(pa.lat, pb.lat) < f.latMin - tolDeg ||
            std::min(pa.lat, pb.lat) > f.latMax + tolDeg ||
            std::max(pa.lon, pb.lon) <
                f.lonMin - tolDeg / CosLat(f.latMin) ||
            std::min(pa.lon, pb.lon) > f.lonMax + tolDeg / CosLat(f.latMin))
          continue;
        if (f.kind == S411Kind::Area) {
          double t = InsideRings(f.rings, pa) ? 0.0 : 2.0;
          for (const auto &ring : f.rings)
            for (size_t i = 0; t > 0 && i < ring.size(); i++) {
              const double c =
                  SegCross(pa, pb, ring[i], ring[(i + 1) % ring.size()]);
              if (c >= 0 && c < t) t = c;
            }
          if (t <= 1) note(al, t, f, ds);
        } else if (f.kind == S411Kind::Line) {
          const auto &ln = f.rings[0];
          for (size_t i = 0; i + 1 < ln.size(); i++) {
            const double c = SegCross(pa, pb, ln[i], ln[i + 1]);
            if (c >= 0) note(al, c, f, ds);
          }
        } else {
          const S411Pos &p = f.rings[0][0];
          const double d = PointSegDist(p, pa, pb) * KM_PER_DEG;  // km
          if (d * NM_PER_KM <= bergTolNm) {
            const double len = DistKm(pa, pb);
            const double t =
                len > 0 ? std::min(1.0, DistKm(pa, p) / len) : 0.0;
            note(al, t, f, ds);
          }
        }
      }
    }
    for (S411Alert al : {S411Alert::IceHazard, S411Alert::IceDifficult,
                         S411Alert::Icebergs})
      if (best[(int)al].t <= 1) out.push_back(best[(int)al].r);
  }
  return out.size();
}
