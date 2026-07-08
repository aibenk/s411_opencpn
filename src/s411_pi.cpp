/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "s411_pi.h"

#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/dcmemory.h>
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include <cmath>

#include "S411Boundary.h"

// opencpn.lib (generated from the installed exe) does not export the
// out-of-line methods of Plugin_WaypointList, so provide them here.
#include <wx/listimpl.cpp>  // NOLINT - wx idiom for WX_DEFINE_LIST
WX_DEFINE_LIST(Plugin_WaypointList);

#ifdef __WXMSW__
#include <windows.h>
#endif
#ifdef __WXOSX__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// ===========================================================================
// legend for the active display mode / palette (mirrors the web viewer)
// ===========================================================================
class S411LegendPanel : public wxPanel {
public:
  S411LegendPanel(wxWindow *parent, s411_pi *pi)
      : wxPanel(parent), m_pi(pi) {
    Bind(wxEVT_PAINT, &S411LegendPanel::OnPaint, this);
  }

  void Rebuild() {
    m_rows.clear();
    switch (m_pi->m_mode) {
      case S411Mode::StageOfDevelopment:
        m_rows = {{"ICES81", "New ice"},          {"ICES82", "Nilas, ice rind"},
                  {"ICES83", "Young ice"},        {"ICES84", "Grey ice"},
                  {"ICES85", "Grey-white ice"},   {"ICES87", "Thin first-year"},
                  {"ICES91", "Medium first-year"},{"ICES93", "Thick first-year"},
                  {"ICES95", "Old ice"},          {"ICES96", "Second-year"},
                  {"ICES97", "Multi-year"},       {"ICEUNK", "Unknown"}};
        break;
      case S411Mode::Navigational:
        m_rows = {{"ICEFRE", "Safe (ice free)"},
                  {"ICEOPW", "Open water / bergy water"},
                  {"ICEC13", "Below ice-class limit"},
                  {"ICEC46", "Caution"},
                  {"ICEC9T", "Hazardous"},
                  {"ICEFST", "Fast ice"},
                  {"ICEUNK", "Unknown"}};
        break;
      default:
        m_rows = {{"ICEFRE", "Ice free"},
                  {"ICEOPW", "Open water / bergy water"},
                  {"ICEC13", "1-3/10"},   {"ICEC46", "4-6/10"},
                  {"ICEC79", "7-8/10"},   {"ICEC9T", "9-9+/10"},
                  {"ICEC10", "10/10"},    {"ICEFST", "Fast ice"},
                  {"ICEUNK", "Unknown"}};
    }
    SetMinSize(wxSize(230, (int)m_rows.size() * 18 + 4));
    Refresh();
  }

private:
  void OnPaint(wxPaintEvent &) {
    wxPaintDC dc(this);
    int y = 2;
    for (const auto &r : m_rows) {
      S411Rgb c = S411TokenRgb(r.first, m_pi->m_palette);
      dc.SetBrush(wxBrush(wxColour(c.r, c.g, c.b)));
      dc.SetPen(wxPen(wxColour(80, 80, 80)));
      dc.DrawRectangle(2, y, 24, 14);
      dc.DrawText(r.second, 32, y - 1);
      y += 18;
    }
  }

  s411_pi *m_pi;
  std::vector<std::pair<wxString, wxString>> m_rows;
};

// ===========================================================================
// control dialog: mirrors the web viewer's left panel (mode / ice class /
// palette) plus load/clear/show, per-dataset visibility list, zoom-to-data,
// fill opacity and a legend. Non-modal so it can stay open as a panel.
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

    // per-dataset visibility list + zoom/remove
    m_list = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition,
                                wxSize(-1, 90));
    s->Add(m_list, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, b);
    auto *dsBtns = new wxBoxSizer(wxHORIZONTAL);
    auto *zoom = new wxButton(this, wxID_ANY, "Zoom to data");
    auto *remove = new wxButton(this, wxID_ANY, "Remove");
    dsBtns->Add(zoom, 0, wxRIGHT, b);
    dsBtns->Add(remove, 0);
    s->Add(dsBtns, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);

    auto *grid = new wxFlexGridSizer(4, 2, b, b);
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
    m_class->Append("IA / PC4-5");
    m_class->Append("IA Super / PC1-3");
    grid->Add(m_class, 1, wxEXPAND);

    grid->Add(new wxStaticText(this, wxID_ANY, "Colour palette"), 0,
              wxALIGN_CENTRE_VERTICAL);
    m_pal = new wxChoice(this, wxID_ANY);
    m_pal->Append("Day");
    m_pal->Append("Dusk");
    m_pal->Append("Night");
    grid->Add(m_pal, 1, wxEXPAND);

    grid->Add(new wxStaticText(this, wxID_ANY, "Fill opacity"), 0,
              wxALIGN_CENTRE_VERTICAL);
    m_opacity = new wxSlider(this, wxID_ANY, 67, 10, 100);
    grid->Add(m_opacity, 1, wxEXPAND);

    s->Add(grid, 0, wxEXPAND | wxALL, b);

    m_legend = new S411LegendPanel(this, pi);
    s->Add(m_legend, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, b);

    // --- ice routing section -------------------------------------------
    s->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, b);
    s->Add(new wxStaticText(this, wxID_ANY,
                            "Ice routing (uses the 'Vessel ice class' above)"),
           0, wxLEFT | wxRIGHT | wxTOP, b);

    auto *rg = new wxFlexGridSizer(2, 3, b, b);
    rg->AddGrowableCol(1, 1);
    rg->Add(new wxStaticText(this, wxID_ANY, "Start"), 0,
            wxALIGN_CENTRE_VERTICAL);
    m_startLbl = new wxStaticText(this, wxID_ANY, "(click the map)");
    rg->Add(m_startLbl, 1, wxALIGN_CENTRE_VERTICAL | wxEXPAND);
    auto *pickS = new wxButton(this, wxID_ANY, "Set…", wxDefaultPosition,
                               wxSize(56, -1));
    rg->Add(pickS, 0);
    rg->Add(new wxStaticText(this, wxID_ANY, "End"), 0,
            wxALIGN_CENTRE_VERTICAL);
    m_endLbl = new wxStaticText(this, wxID_ANY, "(click the map)");
    rg->Add(m_endLbl, 1, wxALIGN_CENTRE_VERTICAL | wxEXPAND);
    auto *pickE = new wxButton(this, wxID_ANY, "Set…", wxDefaultPosition,
                               wxSize(56, -1));
    rg->Add(pickE, 0);
    s->Add(rg, 0, wxEXPAND | wxALL, b);

    m_avoidCaution = new wxCheckBox(
        this, wxID_ANY, "Avoid caution ice too (not just hazardous)");
    s->Add(m_avoidCaution, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);

    auto *rb = new wxBoxSizer(wxHORIZONTAL);
    auto *compute = new wxButton(this, wxID_ANY, "Compute ice route");
    rb->Add(compute, 0, wxRIGHT, b);
    m_export = new wxButton(this, wxID_ANY, "Export no-go zones");
    rb->Add(m_export, 0);
    s->Add(rb, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);
    m_routeStatus = new wxStaticText(this, wxID_ANY, "");
    m_routeStatus->Wrap(320);
    s->Add(m_routeStatus, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, b);

    pickS->Bind(wxEVT_BUTTON,
                [this](wxCommandEvent &) { m_pi->BeginPickPoint(0); });
    pickE->Bind(wxEVT_BUTTON,
                [this](wxCommandEvent &) { m_pi->BeginPickPoint(1); });
    m_avoidCaution->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
      m_pi->RouteParams().avoidCaution = m_avoidCaution->GetValue();
    });
    compute->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnCompute, this);
    m_export->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnExport, this);

    auto *close = new wxButton(this, wxID_CLOSE, "Close");
    s->Add(close, 0, wxALIGN_RIGHT | wxALL, b);

    SetSizerAndFit(s);

    FromState();
    UpdateStatus();

    m_show->Bind(wxEVT_CHECKBOX, &S411CtrlDialog::OnAny, this);
    m_mode->Bind(wxEVT_CHOICE, &S411CtrlDialog::OnAny, this);
    m_class->Bind(wxEVT_CHOICE, &S411CtrlDialog::OnAny, this);
    m_pal->Bind(wxEVT_CHOICE, &S411CtrlDialog::OnAny, this);
    m_opacity->Bind(wxEVT_SLIDER, &S411CtrlDialog::OnAny, this);
    load->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnLoad, this);
    clear->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnClear, this);
    m_list->Bind(wxEVT_CHECKLISTBOX, &S411CtrlDialog::OnListCheck, this);
    zoom->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnZoom, this);
    remove->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnRemove, this);
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
    m_opacity->SetValue(wxMax(10, wxMin(100, m_pi->m_alpha * 100 / 255)));
    m_class->Enable(m_pi->m_mode == S411Mode::Navigational);
    if (m_startLbl) m_startLbl->SetLabel(m_pi->StartLabel());
    if (m_endLbl) m_endLbl->SetLabel(m_pi->EndLabel());
    if (m_avoidCaution)
      m_avoidCaution->SetValue(m_pi->RouteParams().avoidCaution);
    if (m_export)
      m_export->SetToolTip(
          m_pi->OcpnDrawAvailable()
              ? "Export ice no-go zones as ocpn_draw boundaries for the "
                "Weather Routing plugin."
              : "Requires the ocpn_draw_pi plugin (not detected).");
    m_legend->Rebuild();
    Layout();
    Fit();
  }

  void UpdateStatus() {
    m_status->SetLabel(wxString::Format("%zu dataset(s), %zu feature(s) loaded",
                                        m_pi->DatasetCount(),
                                        m_pi->FeatureCount()));
    RebuildList();
    Layout();
  }

private:
  void RebuildList() {
    m_list->Clear();
    for (const auto &ds : m_pi->Data()) {
      int i = m_list->Append(wxString::Format(
          "%s  (%zu features)", ds.title, ds.features.size()));
      m_list->Check(i, ds.visible);
    }
  }
  void ToState() {
    m_pi->m_show = m_show->GetValue();
    m_pi->m_mode = (S411Mode)m_mode->GetSelection();
    static const char *cls[] = {"", "IC", "IB", "IA", "IASuper"};
    m_pi->m_iceClass = cls[wxMax(0, m_class->GetSelection())];
    m_pi->m_palette = (S411Palette)m_pal->GetSelection();
    m_pi->m_alpha = wxMax(26, wxMin(255, m_opacity->GetValue() * 255 / 100));
    m_class->Enable(m_pi->m_mode == S411Mode::Navigational);
  }
  void OnAny(wxCommandEvent &) {
    S411Mode before = m_pi->m_mode;
    S411Palette palBefore = m_pi->m_palette;
    ToState();
    if (m_pi->m_mode != before || m_pi->m_palette != palBefore) {
      m_legend->Rebuild();
      Layout();
      Fit();
    }
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
  void OnListCheck(wxCommandEvent &e) {
    const int i = e.GetInt();
    if (i >= 0 && (size_t)i < m_pi->Data().size()) {
      m_pi->Data()[i].visible = m_list->IsChecked(i);
      m_pi->Refresh();
    }
  }
  void OnZoom(wxCommandEvent &) {
    m_pi->ZoomToData(m_list->GetSelection());  // wxNOT_FOUND -> all visible
  }
  void OnRemove(wxCommandEvent &) {
    const int i = m_list->GetSelection();
    if (i < 0 || (size_t)i >= m_pi->Data().size()) return;
    m_pi->Data().erase(m_pi->Data().begin() + i);
    if (m_pi->Data().empty()) {
      m_pi->m_show = false;
      m_show->SetValue(false);
    }
    UpdateStatus();
    m_pi->Refresh();
  }
  void OnCompute(wxCommandEvent &) {
    bool ok = false;
    m_routeStatus->SetLabel(m_pi->ComputeRoute(ok));
    m_routeStatus->Wrap(320);
    Layout();
    Fit();
  }
  void OnExport(wxCommandEvent &) {
    m_routeStatus->SetLabel(m_pi->ExportHazards());
    m_routeStatus->Wrap(320);
    Layout();
    Fit();
  }

  s411_pi *m_pi;
  wxCheckBox *m_show;
  wxChoice *m_mode, *m_class, *m_pal;
  wxSlider *m_opacity;
  wxStaticText *m_status;
  wxCheckListBox *m_list;
  S411LegendPanel *m_legend;
  wxStaticText *m_startLbl = nullptr, *m_endLbl = nullptr,
               *m_routeStatus = nullptr;
  wxCheckBox *m_avoidCaution = nullptr;
  wxButton *m_export = nullptr;
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
         "day/dusk/night palette, matching the S-411 web viewer.\n"
         "Click an ice feature for its attribute report; the control panel\n"
         "offers a legend, per-dataset visibility, zoom-to-data and fill\n"
         "opacity.";
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
  // ocpn_draw_pi detection happens later, not here: this plugin is not yet
  // registered for messaging until Init() RETURNS the capability flags, so a
  // handshake now could not receive a reply. Availability is learned from the
  // OCPN_DRAW_PI_READY_FOR_REQUESTS broadcast (SetPluginMessage) and from an
  // on-demand Version handshake in ExportHazards().
  return WANTS_OVERLAY_CALLBACK | WANTS_OPENGL_OVERLAY_CALLBACK |
         INSTALLS_TOOLBAR_TOOL | WANTS_TOOLBAR_CALLBACK | WANTS_PREFERENCES |
         WANTS_MOUSE_EVENTS | WANTS_PLUGIN_MESSAGING;
}

bool s411_pi::DeInit(void) {
  SaveConfig();
  if (m_dialog) {
    m_dialog->Destroy();
    m_dialog = nullptr;
  }
  if (m_report) {
    m_report->Destroy();
    m_report = nullptr;
    m_reportText = nullptr;
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
  if (m_data.size() > before) {
    m_show = true;  // show as soon as data arrives
    // jump to the freshly loaded data so it is never invisibly off-screen
    double la = 90, lA = -90, lo = 999, lO = -999;
    for (size_t i = before; i < m_data.size(); i++) {
      la = wxMin(la, m_data[i].latMin);
      lA = wxMax(lA, m_data[i].latMax);
      lo = wxMin(lo, m_data[i].lonMin);
      lO = wxMax(lO, m_data[i].lonMax);
    }
    JumpToBox(la, lA, lo, lO);
  }
}

// jump the chart so the given lat/lon box fills ~80% of the canvas
void s411_pi::JumpToBox(double latMin, double latMax, double lonMin,
                        double lonMax) {
  if (latMin > latMax || lonMin > lonMax) return;
  const double lat = (latMin + latMax) / 2.0;
  double lon = (lonMin + lonMax) / 2.0;
  lon -= 360.0 * std::floor((lon + 180.0) / 360.0);  // back to [-180, 180)
  const double spanLat = wxMax(0.2, latMax - latMin);
  const double spanLon = wxMax(0.2, lonMax - lonMin);
  const double mPerDegLat = 111320.0;
  const double mPerDegLon =
      wxMax(1000.0, 111320.0 * std::cos(lat * 3.14159265358979323846 / 180.0));
  const double pixW = m_haveVp ? m_lastVp.pix_width : 1000.0;
  const double pixH = m_haveVp ? m_lastVp.pix_height : 700.0;
  double ppm = 0.8 * wxMin(pixW / (spanLon * mPerDegLon),
                           pixH / (spanLat * mPerDegLat));
  ppm = wxMax(1e-5, wxMin(0.05, ppm));
  JumpToPosition(lat, lon, ppm);
}

void s411_pi::ZoomToData(int idx) {
  double la = 90, lA = -90, lo = 999, lO = -999;
  for (size_t i = 0; i < m_data.size(); i++) {
    if (idx >= 0 ? (size_t)idx != i : !m_data[i].visible) continue;
    la = wxMin(la, m_data[i].latMin);
    lA = wxMax(lA, m_data[i].latMax);
    lo = wxMin(lo, m_data[i].lonMin);
    lO = wxMax(lO, m_data[i].lonMax);
  }
  JumpToBox(la, lA, lo, lO);
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
// feature picking: a plain click (no drag) on the overlay opens a report
// ---------------------------------------------------------------------------
bool s411_pi::MouseEventHook(wxMouseEvent &ev) {
  if (!m_haveVp) return false;
  const bool picking = m_pickMode != 0;
  if (!m_show && !picking) return false;
  if (ev.LeftDown()) {
    m_downPos = ev.GetPosition();
    m_downValid = true;
    return false;  // never eat the down event - panning must keep working
  }
  if (!ev.LeftUp() || !m_downValid) return false;
  m_downValid = false;
  const wxPoint up = ev.GetPosition();
  if (abs(up.x - m_downPos.x) > 3 || abs(up.y - m_downPos.y) > 3)
    return false;  // that was a drag/pan, not a click

  // routing: a click sets the armed start/end position
  if (picking) {
    double plat, plon;
    GetCanvasLLPix(&m_lastVp, up, &plat, &plon);
    if (m_pickMode == 1) { m_start = {plat, plon}; m_haveStart = true; }
    else { m_end = {plat, plon}; m_haveEnd = true; }
    m_pickMode = 0;
    if (m_dialog) m_dialog->FromState();  // refresh the routing labels
    return true;  // consumed
  }

  if (m_data.empty()) return false;
  double lat, lon, latR, lonR;
  GetCanvasLLPix(&m_lastVp, up, &lat, &lon);
  GetCanvasLLPix(&m_lastVp, wxPoint(up.x + 8, up.y), &latR, &lonR);
  const double tol = std::fabs(lonR - lon);  // ~8 px expressed in degrees
  std::vector<S411Hit> hits;
  if (!S411HitTest(m_data, lat, lon, tol, hits)) return false;
  ShowPickReport(hits, lat, lon);
  return true;  // consumed - the click hit an ice feature
}

void s411_pi::ShowPickReport(const std::vector<S411Hit> &hits, double lat,
                             double lon) {
  wxString txt = wxString::Format(
      "Position  %.4f°%s  %.4f°%s\n\n", std::fabs(lat),
      lat >= 0 ? "N" : "S", std::fabs(lon), lon >= 0 ? "E" : "W");
  for (const auto &h : hits) {
    txt += h.feature->type;
    if (h.feature->fastIce) txt += "   [fast ice]";
    txt += "\n  dataset: " + h.ds->title + "\n";
    for (const auto &kv : h.feature->attrs) {
      txt += "  " + kv.first + " = " + kv.second;
      const wxString n = kv.first.Lower();
      wxString lbl;
      if (n.Contains("concentration") || n == "iceact")
        lbl = S411ConcLabel(kv.second);
      else if (n.Contains("stageofdevelopment") || n.Contains("icestage") ||
               n == "icesod")
        lbl = S411SodLabel(kv.second);
      if (!lbl.IsEmpty()) txt += "   (" + lbl + ")";
      txt += "\n";
    }
    if (h.feature->attrs.empty()) txt += "  (no attributes)\n";
    txt += "\n";
  }
  if (!m_report) {
    m_report = new wxDialog(GetOCPNCanvasWindow(), wxID_ANY,
                            "S-411 feature report", wxDefaultPosition,
                            wxSize(400, 340),
                            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto *s = new wxBoxSizer(wxVERTICAL);
    m_reportText =
        new wxTextCtrl(m_report, wxID_ANY, "", wxDefaultPosition,
                       wxDefaultSize,
                       wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    m_reportText->SetFont(wxFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                                 wxFONTWEIGHT_NORMAL));
    s->Add(m_reportText, 1, wxEXPAND | wxALL, 5);
    auto *close = new wxButton(m_report, wxID_CLOSE, "Close");
    s->Add(close, 0, wxALIGN_RIGHT | wxALL, 5);
    m_report->SetSizer(s);
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_report->Hide(); });
    m_report->Bind(wxEVT_CLOSE_WINDOW,
                   [this](wxCloseEvent &) { m_report->Hide(); });
  }
  m_reportText->SetValue(txt);
  m_report->Show();
  m_report->Raise();
}

// ---------------------------------------------------------------------------
// inter-plugin messaging (ocpn_draw_pi)
// ---------------------------------------------------------------------------
void s411_pi::SetPluginMessage(wxString &id, wxString &body) {
  if (id == "OCPN_DRAW_PI_READY_FOR_REQUESTS") {
    m_odAvailable = body.Upper().Contains("TRUE");
    return;
  }
  // synchronous reply to one of our requests (delivered on our Source id)
  if (id == S411_OD_SOURCE && m_capturingReply) m_odReply = body;
}

wxString s411_pi::SendOcpnDraw(const wxString &json) {
  wxString tgt = S411_OD_TARGET, body = json;
  m_odReply.Clear();
  m_capturingReply = true;
  SendPluginMessage(tgt, body);  // reply arrives synchronously in SetPluginMessage
  m_capturingReply = false;
  return m_odReply;
}

void s411_pi::DeleteOurBoundaries() {
  const wxString info = SendOcpnDraw(S411BuildBoundaryInfoRequest());
  std::vector<wxString> guids;
  S411JsonOurBoundaryGuids(info, guids);
  for (const auto &g : guids) SendOcpnDraw(S411BuildDeleteBoundary(g));
}

// ---------------------------------------------------------------------------
// routing
// ---------------------------------------------------------------------------
static wxString PosLabel(const S411Pos &p) {
  return wxString::Format("%.4f°%s  %.4f°%s", std::fabs(p.lat),
                          p.lat >= 0 ? "N" : "S", std::fabs(p.lon),
                          p.lon >= 0 ? "E" : "W");
}
wxString s411_pi::StartLabel() const {
  return m_haveStart ? PosLabel(m_start) : "(click the map)";
}
wxString s411_pi::EndLabel() const {
  return m_haveEnd ? PosLabel(m_end) : "(click the map)";
}

void s411_pi::BeginPickPoint(int which) {
  m_pickMode = which == 0 ? 1 : 2;
  wxMessageBox(wxString::Format("Click the %s position on the chart.",
                                which == 0 ? "START" : "END"),
               "S-411 route", wxOK | wxICON_INFORMATION,
               GetOCPNCanvasWindow());
}

wxString s411_pi::ComputeRoute(bool &ok) {
  ok = false;
  if (!m_haveStart || !m_haveEnd)
    return "Set a start and an end position first (click the map).";
  bool anyVisible = false;
  for (const auto &d : m_data) anyVisible |= d.visible && !d.features.empty();
  if (!anyVisible)
    return "No ice data loaded - load an S-411 dataset before routing.";

  m_route.iceClass = m_iceClass;
  S411RouteResult r;
  const S411LandFn land = [](double la1, double lo1, double la2, double lo2) {
    return PlugIn_GSHHS_CrossesLand(la1, lo1, la2, lo2);
  };
  if (!S411ComputeIceRoute(m_data, m_start, m_end, m_route, land, r))
    return "No route: " + r.error;

  // hand the polyline to OpenCPN's route manager
  PlugIn_Route route;  // its ctor allocates pWaypointList; its dtor frees it
  route.m_NameString = wxString::Format(
      "S-411 ice route (%s)",
      m_iceClass.IsEmpty() ? "no ice class" : m_iceClass);
  route.m_StartString = "S-411 start";
  route.m_EndString = "S-411 end";
  route.m_GUID = GetNewGUID();
  for (size_t i = 0; i < r.waypoints.size(); i++) {
    const bool ends = (i == 0 || i + 1 == r.waypoints.size());
    auto *wp = new PlugIn_Waypoint(
        r.waypoints[i].lat, r.waypoints[i].lon,
        ends ? "activepoint" : "diamond",
        i == 0 ? "Start"
               : (i + 1 == r.waypoints.size()
                      ? "End"
                      : wxString::Format("wp%zu", i)),
        GetNewGUID());
    route.pWaypointList->Append(wp);
  }
  const bool added = AddPlugInRoute(&route, true);
  // AddPlugInRoute copies the data into OpenCPN's own route objects, so free
  // the PlugIn_Waypoint objects we allocated (the list has auto-delete off;
  // ~PlugIn_Route frees the list itself - do not delete it here)
  for (auto node = route.pWaypointList->GetFirst(); node;
       node = node->GetNext())
    delete node->GetData();
  route.pWaypointList->Clear();
  if (!added) return "OpenCPN refused to add the route.";
  m_lastRouteGuid = route.m_GUID;
  ok = true;
  return wxString::Format(
      "Route added: %.0f NM total, %.0f NM through caution ice, %zu legs.",
      r.distanceNm, r.cautionNm, r.waypoints.size() - 1);
}

wxString s411_pi::ExportHazards() {
  // refresh availability (ocpn_draw may have started after us)
  if (!m_odAvailable) {
    if (SendOcpnDraw(S411BuildVersionRequest()).Find("\"Major\"") != wxNOT_FOUND)
      m_odAvailable = true;
  }
  if (!m_odAvailable)
    return "ocpn_draw_pi is not available - install and enable it to export "
           "ice no-go zones for the Weather Routing plugin.";

  DeleteOurBoundaries();  // idempotent re-export

  std::vector<S411HazardZone> zones;
  S411ExtractHazards(m_data, m_iceClass, m_route.avoidCaution, 60, zones);
  if (zones.empty())
    return "No hazardous ice zones for this ice class - nothing to export.";

  size_t created = 0, n = 0;
  for (const auto &z : zones) {
    const wxString reply =
        SendOcpnDraw(S411BuildCreateBoundary(z, wxString::Format("ice-%zu", n++)));
    if (S411JsonCreated(reply)) created++;
  }
  return wxString::Format(
      "Exported %zu of %zu ice no-go zone(s) as ocpn_draw boundaries.\n"
      "Enable 'Detect Boundary' in the Weather Routing configuration to "
      "avoid them.",
      created, zones.size());
}

void s411_pi::ClearHazards() { DeleteOurBoundaries(); }

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
  v = cfg->Read("FillAlpha", 170L);
  m_alpha = (int)wxMax(26, wxMin(255, v));
  bool avoid = false;
  cfg->Read("RouteAvoidCaution", &avoid, false);
  m_route.avoidCaution = avoid;
  double pen = 4.0;
  cfg->Read("RouteCautionPenalty", &pen, 4.0);
  m_route.cautionPenalty = wxMax(1.0, wxMin(50.0, pen));
}

void s411_pi::SaveConfig() {
  wxFileConfig *cfg = GetOCPNConfigObject();
  if (!cfg) return;
  cfg->SetPath("/PlugIns/S411");
  cfg->Write("Mode", (long)m_mode);
  cfg->Write("Palette", (long)m_palette);
  cfg->Write("IceClass", m_iceClass);
  cfg->Write("LastDir", m_lastDir);
  cfg->Write("FillAlpha", (long)m_alpha);
  cfg->Write("RouteAvoidCaution", m_route.avoidCaution);
  cfg->Write("RouteCautionPenalty", m_route.cautionPenalty);
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
    if (!ds.visible) continue;
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
                ? wxColour(0, 0, 0, wxMin(60, m_alpha))
                : wxColour(fill.Red(), fill.Green(), fill.Blue(), m_alpha)));
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
    if (!ds.visible) continue;
    for (const auto &f : ds.features) {
      if (f.kind != S411Kind::Area || f.tris.empty() || Culled(f, vp))
        continue;
      bool berg = f.type == "IcebergArea";
      wxColour fill = s411_pi_AreaColour(f, m_mode, m_iceClass, m_palette);
      if (berg) glColor4ub(0, 0, 0, (GLubyte)wxMin(60, m_alpha));
      else glColor4ub(fill.Red(), fill.Green(), fill.Blue(), (GLubyte)m_alpha);

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
  m_lastVp = *vp;
  m_haveVp = true;
  return RenderDC(dc, vp);
}
bool s411_pi::RenderGLOverlay(wxGLContext *, PlugIn_ViewPort *vp) {
  m_lastVp = *vp;
  m_haveVp = true;
  return RenderGL(vp);
}
bool s411_pi::RenderOverlayMultiCanvas(wxDC &dc, PlugIn_ViewPort *vp, int) {
  m_lastVp = *vp;
  m_haveVp = true;
  return RenderDC(dc, vp);
}
bool s411_pi::RenderGLOverlayMultiCanvas(wxGLContext *, PlugIn_ViewPort *vp,
                                         int) {
  m_lastVp = *vp;
  m_haveVp = true;
  return RenderGL(vp);
}
