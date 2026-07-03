/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "s411_pi.h"

#include <wx/choice.h>
#include <wx/dcmemory.h>
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <cmath>

#ifdef __WXMSW__
#include <windows.h>
#endif
#ifdef __WXOSX__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// ===========================================================================
// control dialog: mirrors the web viewer's left panel (mode / ice class /
// palette) plus load/clear/show. Non-modal so it can stay open as a panel.
// ===========================================================================
class S411CtrlDialog : public wxDialog {
public:
  S411CtrlDialog(wxWindow *parent, s411_pi *pi)
      : wxDialog(parent, wxID_ANY, "S-411 Ice Information", wxDefaultPosition,
                 wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        m_pi(pi) {
    auto *s = new wxBoxSizer(wxVERTICAL);
    const int b = 5;

    m_show = new wxCheckBox(this, wxID_ANY, "Show ice overlay");
    s->Add(m_show, 0, wxALL, b);

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    auto *load = new wxButton(this, wxID_ANY, "Load data…");
    auto *clear = new wxButton(this, wxID_ANY, "Clear");
    btns->Add(load, 0, wxRIGHT, b);
    btns->Add(clear, 0);
    s->Add(btns, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);

    m_status = new wxStaticText(this, wxID_ANY, "");
    s->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);

    auto *grid = new wxFlexGridSizer(3, 2, b, b);
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(this, wxID_ANY, "Display mode"), 0,
              wxALIGN_CENTRE_VERTICAL);
    m_mode = new wxChoice(this, wxID_ANY);
    m_mode->Append("Ice concentration");
    m_mode->Append("Stage of development");
    m_mode->Append("Navigational (ice-class traffic light)");
    grid->Add(m_mode, 1, wxEXPAND);

    grid->Add(new wxStaticText(this, wxID_ANY, "Vessel ice class"), 0,
              wxALIGN_CENTRE_VERTICAL);
    m_class = new wxChoice(this, wxID_ANY);
    m_class->Append("No ice class");
    m_class->Append("IC / PC7");
    m_class->Append("IB / PC6");
    m_class->Append("IA / PC5");
    m_class->Append("IA Super / PC1-3");
    grid->Add(m_class, 1, wxEXPAND);

    grid->Add(new wxStaticText(this, wxID_ANY, "Colour palette"), 0,
              wxALIGN_CENTRE_VERTICAL);
    m_pal = new wxChoice(this, wxID_ANY);
    m_pal->Append("Day");
    m_pal->Append("Dusk");
    m_pal->Append("Night");
    grid->Add(m_pal, 1, wxEXPAND);

    s->Add(grid, 0, wxEXPAND | wxALL, b);

    auto *close = new wxButton(this, wxID_CLOSE, "Close");
    s->Add(close, 0, wxALIGN_RIGHT | wxALL, b);

    SetSizerAndFit(s);

    FromState();
    UpdateStatus();

    m_show->Bind(wxEVT_CHECKBOX, &S411CtrlDialog::OnAny, this);
    m_mode->Bind(wxEVT_CHOICE, &S411CtrlDialog::OnAny, this);
    m_class->Bind(wxEVT_CHOICE, &S411CtrlDialog::OnAny, this);
    m_pal->Bind(wxEVT_CHOICE, &S411CtrlDialog::OnAny, this);
    load->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnLoad, this);
    clear->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnClear, this);
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Hide(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { Hide(); });
  }

  void FromState() {
    m_show->SetValue(m_pi->m_show);
    m_mode->SetSelection((int)m_pi->m_mode);
    static const char *cls[] = {"", "IC", "IB", "IA", "IASuper"};
    int ci = 0;
    for (int i = 0; i < 5; i++)
      if (m_pi->m_iceClass == cls[i]) ci = i;
    m_class->SetSelection(ci);
    m_pal->SetSelection((int)m_pi->m_palette);
    m_class->Enable(m_pi->m_mode == S411Mode::Navigational);
  }

  void UpdateStatus() {
    m_status->SetLabel(wxString::Format("%zu dataset(s), %zu feature(s) loaded",
                                        m_pi->DatasetCount(),
                                        m_pi->FeatureCount()));
    Layout();
  }

private:
  void ToState() {
    m_pi->m_show = m_show->GetValue();
    m_pi->m_mode = (S411Mode)m_mode->GetSelection();
    static const char *cls[] = {"", "IC", "IB", "IA", "IASuper"};
    m_pi->m_iceClass = cls[wxMax(0, m_class->GetSelection())];
    m_pi->m_palette = (S411Palette)m_pal->GetSelection();
    m_class->Enable(m_pi->m_mode == S411Mode::Navigational);
  }
  void OnAny(wxCommandEvent &) {
    ToState();
    m_pi->Refresh();
  }
  void OnLoad(wxCommandEvent &) {
    m_pi->LoadFiles();
    m_show->SetValue(m_pi->m_show);
    UpdateStatus();
    m_pi->Refresh();
  }
  void OnClear(wxCommandEvent &) {
    m_pi->ClearData();
    m_show->SetValue(false);
    UpdateStatus();
    m_pi->Refresh();
  }

  s411_pi *m_pi;
  wxCheckBox *m_show;
  wxChoice *m_mode, *m_class, *m_pal;
  wxStaticText *m_status;
};

// ===========================================================================
// plugin factory
// ===========================================================================
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
         "colour code. Switchable display mode (concentration / stage of\n"
         "development / ice-class traffic light), vessel ice class and\n"
         "day/dusk/night palette, matching the S-411 web viewer.";
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

size_t s411_pi::FeatureCount() const {
  size_t n = 0;
  for (const auto &d : m_data) n += d.features.size();
  return n;
}

// ---------------------------------------------------------------------------
// lifecycle / toolbar
// ---------------------------------------------------------------------------
int s411_pi::Init(void) {
  LoadConfig();
  m_toolId = InsertPlugInTool("", &m_icon, &m_icon, wxITEM_CHECK, "S-411 ice",
                              "S-411 ice information: load and portrayal",
                              nullptr, -1, 0, this);
  return WANTS_OVERLAY_CALLBACK | WANTS_OPENGL_OVERLAY_CALLBACK |
         INSTALLS_TOOLBAR_TOOL | WANTS_TOOLBAR_CALLBACK | WANTS_PREFERENCES;
}

bool s411_pi::DeInit(void) {
  SaveConfig();
  if (m_dialog) {
    m_dialog->Destroy();
    m_dialog = nullptr;
  }
  if (m_toolId != -1) RemovePlugInTool(m_toolId);
  m_toolId = -1;
  m_data.clear();
  return true;
}

void s411_pi::LoadFiles() {
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
  if (!errors.IsEmpty())
    wxMessageBox(errors, "S-411: some files could not be loaded",
                 wxOK | wxICON_WARNING, GetOCPNCanvasWindow());
  if (m_data.size() > before) m_show = true;  // show as soon as data arrives
}

void s411_pi::ClearData() {
  m_data.clear();
  m_show = false;
}

void s411_pi::SetShow(bool on) { m_show = on; }

void s411_pi::Refresh() {
  if (m_toolId != -1) SetToolbarItemState(m_toolId, m_show);
  RequestRefresh(GetOCPNCanvasWindow());
}

void s411_pi::ShowControl() {
  if (!m_dialog) m_dialog = new S411CtrlDialog(GetOCPNCanvasWindow(), this);
  m_dialog->FromState();
  m_dialog->UpdateStatus();
  m_dialog->Show();
  m_dialog->Raise();
}

void s411_pi::OnToolbarToolCallback(int id) {
  if (id != m_toolId) return;
  // Never load anything automatically - just open the control panel.
  // Data is loaded only when the user clicks "Load data..." there.
  ShowControl();
}

void s411_pi::ShowPreferencesDialog(wxWindow *parent) { ShowControl(); }

// ---------------------------------------------------------------------------
// configuration persistence
// ---------------------------------------------------------------------------
void s411_pi::LoadConfig() {
  wxFileConfig *cfg = GetOCPNConfigObject();
  if (!cfg) return;
  cfg->SetPath("/PlugIns/S411");
  long v;
  v = cfg->Read("Mode", 0L); m_mode = (S411Mode)wxMin(2, wxMax(0, (int)v));
  v = cfg->Read("Palette", 0L); m_palette = (S411Palette)wxMin(2, wxMax(0, (int)v));
  m_iceClass = cfg->Read("IceClass", wxString(""));
  m_lastDir = cfg->Read("LastDir", wxString(""));
}

void s411_pi::SaveConfig() {
  wxFileConfig *cfg = GetOCPNConfigObject();
  if (!cfg) return;
  cfg->SetPath("/PlugIns/S411");
  cfg->Write("Mode", (long)m_mode);
  cfg->Write("Palette", (long)m_palette);
  cfg->Write("IceClass", m_iceClass);
  cfg->Write("LastDir", m_lastDir);
  cfg->Flush();
}

// ---------------------------------------------------------------------------
// geometry -> canvas (longitudes already unwrapped at load time)
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

// fill colour for an area feature under the current mode/class/palette
wxColour s411_pi_AreaColour(const S411Feature &f, S411Mode mode,
                            const wxString &iceClass, S411Palette pal) {
  wxString tok = S411AreaTokenFor(f.totalConcentration, f.stageOfDevelopment,
                                  f.fastIce, mode, iceClass);
  S411Rgb c = S411TokenRgb(tok, pal);
  return wxColour(c.r, c.g, c.b);
}

static wxColour LineColour(const char *token, S411Palette pal) {
  S411Rgb c = S411TokenRgb(token, pal);
  return wxColour(c.r, c.g, c.b);
}

static wxPenStyle DashToPen(S411Dash d) {
  switch (d) {
    case S411Dash::LongDash: return wxPENSTYLE_LONG_DASH;
    case S411Dash::ShortDash: return wxPENSTYLE_SHORT_DASH;
    case S411Dash::Dot: return wxPENSTYLE_DOT;
    case S411Dash::DotDash: return wxPENSTYLE_DOT_DASH;
    default: return wxPENSTYLE_SOLID;
  }
}

// ---------------------------------------------------------------------------
// wxDC rendering (non-OpenGL mode); wxGraphicsContext for alpha fills
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
          wxColour fill = s411_pi_AreaColour(f, m_mode, m_iceClass, m_palette);
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
          dc.SetPen(wxPen(LineColour(ls.token, m_palette),
                          wxMax(1, (int)std::lround(ls.width)),
                          DashToPen(ls.dash)));
          dc.DrawLines((int)pix.size(), pix.data());
        } else {
          wxPoint p = PointToPix(vp, f.rings[0][0]);
          dc.SetBrush(wxBrush(LineColour("ICELIN", m_palette)));
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
// OpenGL rendering: areas were triangulated at load time (S411Tess)
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
      wxColour fill = s411_pi_AreaColour(f, m_mode, m_iceClass, m_palette);
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
      wxColour lc = LineColour(ls.token, m_palette);
      glColor4ub(lc.Red(), lc.Green(), lc.Blue(), 255);
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
    wxColour pc = LineColour("ICELIN", m_palette);
    for (const auto &f : ds.features) {
      if (f.kind != S411Kind::Point || Culled(f, vp)) continue;
      wxPoint p = PointToPix(vp, f.rings[0][0]);
      glColor4ub(pc.Red(), pc.Green(), pc.Blue(), 255);
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
