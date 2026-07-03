/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "s411_pi.h"

#include <wx/dcmemory.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/msgdlg.h>

#include <cmath>

#include "S411Colors.h"

#ifdef __WXMSW__
#include <windows.h>
#endif
#ifdef __WXOSX__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// ---------------------------------------------------------------------------
// plugin factory
// ---------------------------------------------------------------------------
extern "C" DECL_EXP opencpn_plugin *create_pi(void *ppimgr) {
  return new s411_pi(ppimgr);
}
extern "C" DECL_EXP void destroy_pi(opencpn_plugin *p) { delete p; }

s411_pi::s411_pi(void *ppimgr) : opencpn_plugin_116(ppimgr) { MakeIcon(); }

// ---------------------------------------------------------------------------
// identification
// ---------------------------------------------------------------------------
wxString s411_pi::GetCommonName() { return "S-411 Ice"; }

wxString s411_pi::GetShortDescription() {
  return "S-411 Ice Information overlay (IHO S-100)";
}

wxString s411_pi::GetLongDescription() {
  return "Loads S-411 'Ice Information' datasets - Edition 2.0.0 GML,\n"
         "Edition 1.x GML, complete exchange sets (CATALOG.XML) or .zip\n"
         "archives - and renders them on the chart with the WMO ice chart\n"
         "colour code: concentration fills, fast ice, ice edges and\n"
         "limits, point features.";
}

void s411_pi::MakeIcon() {
  m_icon.Create(32, 32);
  wxMemoryDC dc(m_icon);
  dc.SetBackground(wxBrush(wxColour(0, 0, 76)));  // IHO navy
  dc.Clear();
  dc.SetPen(wxPen(wxColour(241, 234, 202), 2));   // IHO cream
  dc.SetBrush(*wxTRANSPARENT_BRUSH);
  dc.DrawRoundedRectangle(1, 1, 30, 30, 5);
  dc.SetTextForeground(wxColour(241, 234, 202));
  wxFont f(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
  dc.SetFont(f);
  wxSize ts = dc.GetTextExtent("411");
  dc.DrawText("411", (32 - ts.x) / 2, (32 - ts.y) / 2);
  dc.SelectObject(wxNullBitmap);
}

wxBitmap *s411_pi::GetPlugInBitmap() { return &m_icon; }

// ---------------------------------------------------------------------------
// lifecycle / toolbar
// ---------------------------------------------------------------------------
int s411_pi::Init(void) {
  m_toolId = InsertPlugInTool("", &m_icon, &m_icon, wxITEM_CHECK, "S-411 ice",
                              "Load and display S-411 ice information",
                              nullptr, -1, 0, this);
  return WANTS_OVERLAY_CALLBACK | WANTS_OPENGL_OVERLAY_CALLBACK |
         INSTALLS_TOOLBAR_TOOL | WANTS_TOOLBAR_CALLBACK;
}

bool s411_pi::DeInit(void) {
  if (m_toolId != -1) RemovePlugInTool(m_toolId);
  m_toolId = -1;
  m_data.clear();
  return true;
}

void s411_pi::LoadDialog() {
  wxFileDialog dlg(
      GetOCPNCanvasWindow(), "Open S-411 data", m_lastDir, "",
      "S-411 data (*.gml;*.xml;*.zip;CATALOG.XML)|"
      "*.gml;*.GML;*.xml;*.XML;*.zip;*.ZIP|"
      "All files|*.*",
      wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() != wxID_OK) return;
  wxArrayString paths;
  dlg.GetPaths(paths);
  m_lastDir = dlg.GetDirectory();

  wxString errors;
  size_t before = m_data.size();
  for (size_t i = 0; i < paths.GetCount(); i++) {
    wxFileName fn(paths[i]);
    wxString ext = fn.GetExt().Lower();
    if (ext == "zip") {
      LoadS411Zip(paths[i], m_data, errors);
    } else if (fn.GetFullName().Upper() == "CATALOG.XML") {
      LoadS411ExchangeSet(paths[i], m_data, errors);
    } else {
      S411Dataset ds;
      wxString err;
      if (LoadS411File(paths[i], ds, err))
        m_data.push_back(ds);
      else
        errors += err + "\n";
    }
  }
  size_t nsets = m_data.size() - before, nfeat = 0;
  for (size_t i = before; i < m_data.size(); i++)
    nfeat += m_data[i].features.size();
  if (!errors.IsEmpty())
    wxMessageBox(errors, "S-411: some files could not be loaded",
                 wxOK | wxICON_WARNING, GetOCPNCanvasWindow());
  if (nsets)
    wxLogMessage("s411_pi: loaded %zu dataset(s), %zu feature(s)", nsets,
                 nfeat);
}

void s411_pi::OnToolbarToolCallback(int id) {
  if (id != m_toolId) return;
  if (!m_show) {
    // activating always offers to add data; Cancel keeps what is loaded
    LoadDialog();
    m_show = !m_data.empty();
    if (!m_show)
      wxMessageBox("No S-411 dataset loaded.", "S-411 ice",
                   wxOK | wxICON_INFORMATION, GetOCPNCanvasWindow());
  } else {
    m_show = false;  // hide; loaded data is kept for the next activation
  }
  SetToolbarItemState(m_toolId, m_show);
  RequestRefresh(GetOCPNCanvasWindow());
}

// ---------------------------------------------------------------------------
// geometry -> canvas. Longitudes are already unwrapped at load time; here a
// whole ring/point is only shifted onto the world copy nearest the viewport
// centre so charts crossing the antimeridian render correctly.
// ---------------------------------------------------------------------------
static double WorldShift(double lonCentre, double clon) {
  return 360.0 * std::floor((lonCentre - clon + 180.0) / 360.0);
}

static void RingToPix(PlugIn_ViewPort *vp, const std::vector<S411Pos> &ring,
                      std::vector<wxPoint> &out) {
  out.clear();
  if (ring.empty()) return;
  double lmin = ring[0].lon, lmax = ring[0].lon;
  for (const auto &p : ring) {
    if (p.lon < lmin) lmin = p.lon;
    if (p.lon > lmax) lmax = p.lon;
  }
  const double k = WorldShift((lmin + lmax) / 2.0, vp->clon);
  out.reserve(ring.size());
  for (const auto &p : ring) {
    wxPoint px;
    GetCanvasPixLL(vp, &px, p.lat, p.lon - k);
    out.push_back(px);
  }
}

static wxPoint PointToPix(PlugIn_ViewPort *vp, const S411Pos &pos) {
  wxPoint px;
  GetCanvasPixLL(vp, &px, pos.lat, pos.lon - WorldShift(pos.lon, vp->clon));
  return px;
}

static bool Culled(const S411Feature &f, PlugIn_ViewPort *vp) {
  if (f.latMax < vp->lat_min - 0.5 || f.latMin > vp->lat_max + 0.5)
    return true;
  const double c = (f.lonMin + f.lonMax) / 2.0;
  const double half = (f.lonMax - f.lonMin) / 2.0;
  const double cs = c - WorldShift(c, vp->clon);
  if (cs + half < vp->lon_min - 0.5 || cs - half > vp->lon_max + 0.5)
    return true;
  return false;
}

// ---------------------------------------------------------------------------
// wxDC rendering (non-OpenGL mode); uses wxGraphicsContext for alpha fills
// ---------------------------------------------------------------------------
bool s411_pi::RenderDC(wxDC &dc, PlugIn_ViewPort *vp) {
  if (!m_show || m_data.empty()) return false;
  std::vector<wxPoint> pix;

#if wxUSE_GRAPHICS_CONTEXT
  wxGraphicsContext *gc = nullptr;
  wxMemoryDC *mdc = wxDynamicCast(&dc, wxMemoryDC);
  if (mdc) gc = wxGraphicsContext::Create(*mdc);
#endif

  for (const auto &ds : m_data) {
    for (int pass = 0; pass < 3; pass++) {
      for (const auto &f : ds.features) {
        if ((int)f.kind != (pass == 0 ? (int)S411Kind::Area
                          : pass == 1 ? (int)S411Kind::Line
                                      : (int)S411Kind::Point))
          continue;
        if (Culled(f, vp)) continue;

        if (f.kind == S411Kind::Area) {
          bool berg = f.type == "IcebergArea";
          wxColour fill = S411AreaColour(f.totalConcentration, f.fastIce);
#if wxUSE_GRAPHICS_CONTEXT
          if (gc) {
            wxGraphicsPath path = gc->CreatePath();
            for (const auto &ring : f.rings) {
              RingToPix(vp, ring, pix);
              if (pix.size() < 3) continue;
              path.MoveToPoint(pix[0].x, pix[0].y);
              for (size_t i = 1; i < pix.size(); i++)
                path.AddLineToPoint(pix[i].x, pix[i].y);
              path.CloseSubpath();
            }
            gc->SetBrush(wxBrush(berg
                ? wxColour(0, 0, 0, 60)
                : wxColour(fill.Red(), fill.Green(), fill.Blue(), 170)));
            gc->FillPath(path, wxODDEVEN_RULE);
            gc->SetPen(wxPen(wxColour(0, 0, 0, 120), 1));
            gc->StrokePath(path);
            continue;
          }
#endif
          RingToPix(vp, f.rings[0], pix);
          if (pix.size() < 3) continue;
          dc.SetBrush(wxBrush(berg ? wxColour(200, 200, 200) : fill));
          dc.SetPen(wxPen(*wxBLACK, 1));
          dc.DrawPolygon((int)pix.size(), pix.data());
        } else if (f.kind == S411Kind::Line) {
          RingToPix(vp, f.rings[0], pix);
          if (pix.size() < 2) continue;
          S411LineStyle ls;
          S411LineStyleFor(f.type, ls);
          dc.SetPen(wxPen(*wxBLACK, wxMax(1, (int)std::lround(ls.width)),
                          ls.style));
          dc.DrawLines((int)pix.size(), pix.data());
        } else {
          wxPoint p = PointToPix(vp, f.rings[0][0]);
          dc.SetBrush(*wxBLACK_BRUSH);
          dc.SetPen(wxPen(*wxWHITE, 1));
          wxPoint tri[3] = {wxPoint(p.x, p.y - 6), wxPoint(p.x - 5, p.y + 4),
                            wxPoint(p.x + 5, p.y + 4)};
          dc.DrawPolygon(3, tri);
        }
      }
    }
  }
#if wxUSE_GRAPHICS_CONTEXT
  delete gc;
#endif
  return true;
}

// ---------------------------------------------------------------------------
// OpenGL rendering: areas were triangulated at load time (S411Tess), so per
// frame this only transforms vertices - no tessellation cost while panning.
// ---------------------------------------------------------------------------
bool s411_pi::RenderGL(PlugIn_ViewPort *vp) {
  if (!m_show || m_data.empty()) return false;
  std::vector<wxPoint> pix;

  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_ENABLE_BIT |
               GL_POLYGON_BIT);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_TEXTURE_2D);

  for (const auto &ds : m_data) {
    for (const auto &f : ds.features) {
      if (f.kind != S411Kind::Area || f.tris.empty() || Culled(f, vp))
        continue;
      bool berg = f.type == "IcebergArea";
      wxColour fill = S411AreaColour(f.totalConcentration, f.fastIce);
      if (berg) glColor4ub(0, 0, 0, 60);
      else glColor4ub(fill.Red(), fill.Green(), fill.Blue(), 170);

      const double k = WorldShift((f.lonMin + f.lonMax) / 2.0, vp->clon);
      glBegin(GL_TRIANGLES);
      for (const auto &t : f.tris) {
        wxPoint p;
        GetCanvasPixLL(vp, &p, t.lat, t.lon - k);
        glVertex2i(p.x, p.y);
      }
      glEnd();

      glColor4ub(0, 0, 0, 120);   // outline
      glLineWidth(1.0f);
      for (const auto &ring : f.rings) {
        RingToPix(vp, ring, pix);
        if (pix.size() < 3) continue;
        glBegin(GL_LINE_LOOP);
        for (const auto &p : pix) glVertex2i(p.x, p.y);
        glEnd();
      }
    }
    for (const auto &f : ds.features) {
      if (f.kind != S411Kind::Line || Culled(f, vp)) continue;
      RingToPix(vp, f.rings[0], pix);
      if (pix.size() < 2) continue;
      S411LineStyle ls;
      S411LineStyleFor(f.type, ls);
      glColor4ub(0, 0, 0, 255);
      glLineWidth((GLfloat)ls.width);
      if (ls.stipple) {
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(2, ls.stipple);
      } else {
        glDisable(GL_LINE_STIPPLE);
      }
      glBegin(GL_LINE_STRIP);
      for (const auto &p : pix) glVertex2i(p.x, p.y);
      glEnd();
    }
    glDisable(GL_LINE_STIPPLE);
    for (const auto &f : ds.features) {
      if (f.kind != S411Kind::Point || Culled(f, vp)) continue;
      wxPoint p = PointToPix(vp, f.rings[0][0]);
      glColor4ub(0, 0, 0, 255);
      glBegin(GL_TRIANGLES);
      glVertex2i(p.x, p.y - 6);
      glVertex2i(p.x - 5, p.y + 4);
      glVertex2i(p.x + 5, p.y + 4);
      glEnd();
    }
  }
  glPopAttrib();
  return true;
}

// ---------------------------------------------------------------------------
// overlay entry points
// ---------------------------------------------------------------------------
bool s411_pi::RenderOverlay(wxDC &dc, PlugIn_ViewPort *vp) {
  return RenderDC(dc, vp);
}
bool s411_pi::RenderGLOverlay(wxGLContext *, PlugIn_ViewPort *vp) {
  return RenderGL(vp);
}
bool s411_pi::RenderOverlayMultiCanvas(wxDC &dc, PlugIn_ViewPort *vp, int) {
  return RenderDC(dc, vp);
}
bool s411_pi::RenderGLOverlayMultiCanvas(wxGLContext *, PlugIn_ViewPort *vp,
                                         int) {
  return RenderGL(vp);
}
