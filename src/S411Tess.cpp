/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Headless GLU tessellation: the GLU tessellator is pure CPU code and works
 * without any OpenGL context, so the same routine serves the plugin and the
 * command-line tests. The edge-flag callback forces plain GL_TRIANGLES
 * output (no fans/strips), so vertices arrive in groups of three.
 *
 * Large polygons are cut into tiles first (recursive bisection). The SGI
 * tessellator keeps the active edges of its sweep line in a linked list, so
 * its cost grows with (vertices x edges crossing the sweep): the NIC Arctic
 * extent polygon (1 exterior, 1652 holes, 191k vertices) took 60 s in one
 * piece and takes well under a second in tiles. Each ring is clipped to
 * each tile independently (Sutherland-Hodgman); with the odd winding rule
 * the degenerate edges the clip leaves along the tile border cancel out, so
 * the union of the tile triangulations is exactly the polygon.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "S411Tess.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __APPLE__
#include <OpenGL/glu.h>
#else
#include <GL/glu.h>
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

typedef void(APIENTRY *GluCb)();

// callback state, per thread (datasets load on a worker thread)
static thread_local std::deque<std::array<GLdouble, 3>> *s_store = nullptr;
static thread_local std::vector<S411Pos> *s_out = nullptr;

static void APIENTRY tBegin(GLenum) {}
static void APIENTRY tEnd(void) {}
static void APIENTRY tEdge(GLboolean) {}  // presence forces GL_TRIANGLES
static void APIENTRY tError(GLenum) {}
static void APIENTRY tVertex(void *data) {
  const GLdouble *v = (const GLdouble *)data;
  s_out->push_back({v[1], v[0]});  // y = lat, x = lon
}
static void APIENTRY tCombine(GLdouble coords[3], void *[4], GLfloat[4],
                              void **out) {
  s_store->push_back({coords[0], coords[1], 0.0});
  *out = s_store->back().data();
}

// one GLU pass over a set of rings, appending triangles to trisOut
static void TessRings(const std::vector<std::vector<S411Pos>> &rings,
                      std::vector<S411Pos> &trisOut) {
  std::deque<std::array<GLdouble, 3>> store;
  s_store = &store;
  s_out = &trisOut;
  const size_t before = trisOut.size();

  GLUtesselator *tess = gluNewTess();
  if (!tess) return;
  gluTessCallback(tess, GLU_TESS_BEGIN, (GluCb)tBegin);
  gluTessCallback(tess, GLU_TESS_END, (GluCb)tEnd);
  gluTessCallback(tess, GLU_TESS_EDGE_FLAG, (GluCb)tEdge);
  gluTessCallback(tess, GLU_TESS_VERTEX, (GluCb)tVertex);
  gluTessCallback(tess, GLU_TESS_COMBINE, (GluCb)tCombine);
  gluTessCallback(tess, GLU_TESS_ERROR, (GluCb)tError);
  gluTessProperty(tess, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
  gluTessNormal(tess, 0, 0, 1);  // planar input: skip the normal estimation

  gluTessBeginPolygon(tess, nullptr);
  for (const auto &ring : rings) {
    if (ring.size() < 3) continue;
    gluTessBeginContour(tess);
    for (const auto &p : ring) {
      store.push_back({p.lon, p.lat, 0.0});
      gluTessVertex(tess, store.back().data(), store.back().data());
    }
    gluTessEndContour(tess);
  }
  gluTessEndPolygon(tess);
  gluDeleteTess(tess);

  // safety: drop a trailing partial triangle of this pass
  trisOut.resize(before + (trisOut.size() - before) / 3 * 3);
  s_store = nullptr;
  s_out = nullptr;
}

// Sutherland-Hodgman clip of one ring against one axis-aligned half plane
// (axis 0 = lon, 1 = lat; keep values >= v when keepGreater)
static void ClipHalf(const std::vector<S411Pos> &in, int axis, double v,
                     bool keepGreater, std::vector<S411Pos> &out) {
  out.clear();
  if (in.empty()) return;
  auto coord = [axis](const S411Pos &p) { return axis ? p.lat : p.lon; };
  auto inside = [&](const S411Pos &p) {
    return keepGreater ? coord(p) >= v : coord(p) <= v;
  };
  S411Pos prev = in.back();
  bool prevIn = inside(prev);
  for (const S411Pos &cur : in) {
    const bool curIn = inside(cur);
    if (curIn != prevIn) {
      const double t = (v - coord(prev)) / (coord(cur) - coord(prev));
      S411Pos x = {prev.lat + t * (cur.lat - prev.lat),
                   prev.lon + t * (cur.lon - prev.lon)};
      if (axis) x.lat = v; else x.lon = v;  // exact on the tile border
      out.push_back(x);
    }
    if (curIn) out.push_back(cur);
    prev = cur;
    prevIn = curIn;
  }
}

// vertices per tile the tiler aims for; GLU is fast below a few thousand
static const size_t kTileTarget = 3000;

namespace {
struct Box {
  double x0, x1, y0, y1;
};

Box RingBox(const std::vector<S411Pos> &r) {
  Box b = {1e9, -1e9, 1e9, -1e9};
  for (const auto &p : r) {
    b.x0 = std::min(b.x0, p.lon); b.x1 = std::max(b.x1, p.lon);
    b.y0 = std::min(b.y0, p.lat); b.y1 = std::max(b.y1, p.lat);
  }
  return b;
}

// Recursive bisection: halve the box along its longer side, clip every ring
// to each half (one half-plane per level, so each vertex is touched once per
// level) and recurse until a part is small enough for one GLU pass. Dense
// coastlines simply recurse deeper.
void TessSplit(std::vector<std::vector<S411Pos>> &rings, const Box &box,
               int depth, std::vector<S411Pos> &trisOut) {
  size_t total = 0;
  for (const auto &r : rings) total += r.size();
  if (total <= kTileTarget || depth >= 24) {
    TessRings(rings, trisOut);
    return;
  }
  const int axis = (box.x1 - box.x0) >= (box.y1 - box.y0) ? 0 : 1;
  const double mid = axis ? (box.y0 + box.y1) / 2 : (box.x0 + box.x1) / 2;
  for (int side = 0; side < 2; side++) {
    Box half = box;
    if (axis) (side ? half.y0 : half.y1) = mid;
    else (side ? half.x0 : half.x1) = mid;
    std::vector<std::vector<S411Pos>> part;
    for (const auto &r : rings) {
      const Box b = RingBox(r);
      const double lo = axis ? b.y0 : b.x0, hi = axis ? b.y1 : b.x1;
      if (side ? hi < mid : lo > mid) continue;           // wholly outside
      if (side ? lo >= mid : hi <= mid) {                 // wholly inside
        part.push_back(r);
        continue;
      }
      std::vector<S411Pos> c;
      ClipHalf(r, axis, mid, side == 1, c);
      const Box cb = RingBox(c);
      if (c.size() >= 3 && cb.x1 > cb.x0 && cb.y1 > cb.y0)
        part.push_back(std::move(c));
    }
    if (!part.empty()) TessSplit(part, half, depth + 1, trisOut);
  }
}
}  // namespace

void S411Triangulate(const std::vector<std::vector<S411Pos>> &rings,
                     std::vector<S411Pos> &trisOut) {
  trisOut.clear();
  size_t total = 0;
  Box box = {1e9, -1e9, 1e9, -1e9};
  for (const auto &r : rings) {
    total += r.size();
    const Box b = RingBox(r);
    box.x0 = std::min(box.x0, b.x0); box.x1 = std::max(box.x1, b.x1);
    box.y0 = std::min(box.y0, b.y0); box.y1 = std::max(box.y1, b.y1);
  }
  if (total <= 2 * kTileTarget || box.x1 <= box.x0 || box.y1 <= box.y0) {
    TessRings(rings, trisOut);
    return;
  }
  std::vector<std::vector<S411Pos>> work;
  for (const auto &r : rings)
    if (r.size() >= 3) work.push_back(r);
  TessSplit(work, box, 0, trisOut);
}
