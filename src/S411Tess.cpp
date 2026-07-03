/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Headless GLU tessellation: the GLU tessellator is pure CPU code and works
 * without any OpenGL context, so the same routine serves the plugin and the
 * command-line tests. The edge-flag callback forces plain GL_TRIANGLES
 * output (no fans/strips), so vertices arrive in groups of three.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "S411Tess.h"

#include <array>
#include <deque>

#ifdef _WIN32
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

// callback state (loading runs on one thread only)
static std::deque<std::array<GLdouble, 3>> *s_store = nullptr;
static std::vector<S411Pos> *s_out = nullptr;

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

void S411Triangulate(const std::vector<std::vector<S411Pos>> &rings,
                     std::vector<S411Pos> &trisOut) {
  trisOut.clear();
  std::deque<std::array<GLdouble, 3>> store;
  s_store = &store;
  s_out = &trisOut;

  GLUtesselator *tess = gluNewTess();
  if (!tess) return;
  gluTessCallback(tess, GLU_TESS_BEGIN, (GluCb)tBegin);
  gluTessCallback(tess, GLU_TESS_END, (GluCb)tEnd);
  gluTessCallback(tess, GLU_TESS_EDGE_FLAG, (GluCb)tEdge);
  gluTessCallback(tess, GLU_TESS_VERTEX, (GluCb)tVertex);
  gluTessCallback(tess, GLU_TESS_COMBINE, (GluCb)tCombine);
  gluTessCallback(tess, GLU_TESS_ERROR, (GluCb)tError);
  gluTessProperty(tess, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);

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

  trisOut.resize(trisOut.size() - trisOut.size() % 3);  // safety
  s_store = nullptr;
  s_out = nullptr;
}
