/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "s411_pi.h"

#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choicdlg.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/ffile.h>
#include <wx/gauge.h>
#include <wx/graphics.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/tokenzr.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <ctime>

#include "S411Boundary.h"
#include "S411Symbols.h"

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

// joinable worker thread running one function (dataset loading)
class S411Worker : public wxThread {
public:
  explicit S411Worker(std::function<void()> fn)
      : wxThread(wxTHREAD_JOINABLE), m_fn(std::move(fn)) {}

protected:
  ExitCode Entry() override {
    m_fn();
    return 0;
  }

private:
  std::function<void()> m_fn;
};

// polls the worker thread that loads datasets (see s411_pi::LoadPaths)
class S411LoadTimer : public wxTimer {
public:
  explicit S411LoadTimer(s411_pi *pi) : m_pi(pi) {}
  void Notify() override { m_pi->PollLoad(); }

private:
  s411_pi *m_pi;
};

// ===========================================================================
// WMO egg code for the pick report: the same drawing as the S-411 web viewer
// (spec Figure 3.1) - a tall oval tinted with the concentration colour, Ct
// on top, the Ca/Cb/Cc, Sa/Sb/Sc, Fa/Fb/Fc columns inside, So to the left
// and Sd / strips concentration to the right. Geometry in the viewer's
// 224 x 196 units, scaled to the panel.
// ===========================================================================
class S411EggPanel : public wxPanel {
public:
  explicit S411EggPanel(wxWindow *parent) : wxPanel(parent) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &S411EggPanel::OnPaint, this);
  }
  void SetEggs(std::vector<std::pair<S411Egg, wxString>> eggs) {
    m_eggs = std::move(eggs);
    const int n = (int)m_eggs.size();
    SetMinSize(n ? wxSize(n * kW + (n - 1) * 8, kH + 18) : wxSize(0, 0));
    Show(n > 0);
    Refresh();
  }

private:
  static const int kW = 224, kH = 196;

  void OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
    if (!gc) return;
    for (size_t i = 0; i < m_eggs.size(); i++)
      Draw(gc, (double)i * (kW + 8), m_eggs[i].first, m_eggs[i].second);
    delete gc;
  }

  // text centred on x with its baseline at y (viewer coordinates)
  void Text(wxGraphicsContext *gc, double ox, double x, double y,
            const wxString &t, int px, bool bold, const wxColour &c) {
    if (t.IsEmpty()) return;
    wxFont f(wxFontInfo(wxSize(0, px)).Family(wxFONTFAMILY_SWISS).Bold(bold));
    gc->SetFont(f, c);
    double w, h, desc, lead;
    gc->GetTextExtent(t, &w, &h, &desc, &lead);
    gc->DrawText(t, ox + x - w / 2, y - (h - desc));
  }

  void Draw(wxGraphicsContext *gc, double ox, const S411Egg &d,
            const wxString &label) {
    const wxColour navy(0, 0, 76), grey(74, 91, 106), side(58, 75, 90);
    const double cx = 113, cy = 98, rx = 62, ry = 86;
    gc->SetPen(wxPen(navy, 2));
    gc->SetBrush(wxBrush(wxColour(253, 248, 230)));
    gc->DrawEllipse(ox + cx - rx, cy - ry, 2 * rx, 2 * ry);
    const S411Rgb t = S411TokenRgb(
        d.ct.IsEmpty() ? wxString("ICEUNK") : S411ConcToken(d.ct),
        S411Palette::Day);
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(wxColour(t.r, t.g, t.b, 56)));  // 0.22 opacity
    gc->DrawEllipse(ox + cx - rx, cy - ry, 2 * rx, 2 * ry);
    wxPen dash(wxColour(0, 0, 76, 68), 1, wxPENSTYLE_SHORT_DASH);
    gc->SetPen(dash);
    for (double y : {50.0, 90.0, 134.0})
      gc->StrokeLine(ox + 8, y, ox + 216, y);
    for (double x : {96.0, 131.0, 166.0})
      gc->StrokeLine(ox + x, 52, ox + x, 178);

    Text(gc, ox, cx, 32, d.ct, 17, true, navy);
    const double xs[3] = {78, 113, 148};
    for (size_t i = 0; i < d.cols.size() && i < 3; i++) {
      const S411EggColumn &col = d.cols[i];
      Text(gc, ox, xs[i], 68, col.c, 14, false, wxColour(10, 26, 42));
      for (size_t k = 0; k < col.sf.size(); k++) {
        const bool big = k == 0;
        const double dy = 12.0 * k;
        Text(gc, ox, xs[i], 112 + dy, col.sf[k].first, big ? 14 : 10, big,
             big ? navy : grey);
        Text(gc, ox, xs[i], 156 + dy, col.sf[k].second, big ? 14 : 10, big,
             big ? navy : grey);
      }
      if (!col.breccia.IsEmpty())
        Text(gc, ox, xs[i], 171,
             "brc" + (col.brecciaStage.IsEmpty() ? wxString()
                                                 : ":" + col.brecciaStage),
             9, false, wxColour(138, 90, 0));
    }
    Text(gc, ox, 24, 112, d.so, 14, true, navy);
    Text(gc, ox, 190, 68, d.cs, 13, false, side);
    Text(gc, ox, 190, 112, d.sd, 13, false, side);
    Text(gc, ox, cx, kH + 12, label, 11, false, grey);
  }

  std::vector<std::pair<S411Egg, wxString>> m_eggs;
};

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
// ===========================================================================
// guided tour: a glass overlay that dims the control panel and highlights one
// control at a time with a caption and Back / Next / Skip navigation. Shown
// automatically on first use and re-invokable from the "?" button. Purely a
// wxWindow overlay, so it needs no knowledge of the dialog internals - the
// caller hands it a list of (target control, title, text) steps.
// ===========================================================================
class S411Tutorial : public wxPanel {
public:
  struct Step {
    wxWindow *target;  // control to highlight (may be null for a centred step)
    wxString title;
    wxString body;
  };

  explicit S411Tutorial(wxWindow *host)
      : wxPanel(host, wxID_ANY, wxPoint(0, 0), host->GetClientSize(),
                wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE),
        m_host(host) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_back = new wxButton(this, wxID_ANY, L"\u2190 Back");
    m_next = new wxButton(this, wxID_ANY, L"Next \u2192");
    m_skip = new wxButton(this, wxID_ANY, "Skip");
    m_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Go(-1); });
    m_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Go(+1); });
    m_skip->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Finish(); });
    Bind(wxEVT_PAINT, &S411Tutorial::OnPaint, this);
    m_host->Bind(wxEVT_SIZE, &S411Tutorial::OnHostSize, this);
    Hide();
  }

  void SetSteps(std::vector<Step> steps) { m_steps = std::move(steps); }

  void Start() {
    if (m_steps.empty()) return;
    m_idx = 0;
    Cover();
    Show();
    Raise();
    LayoutStep();
  }

private:
  void OnHostSize(wxSizeEvent &e) {
    if (IsShown()) { Cover(); LayoutStep(); }
    e.Skip();
  }
  void Cover() { SetSize(wxRect(wxPoint(0, 0), m_host->GetClientSize())); }
  void Finish() { Hide(); }
  void Go(int d) {
    m_idx += d;
    if (m_idx < 0) m_idx = 0;
    if (m_idx >= (int)m_steps.size()) { Finish(); return; }
    LayoutStep();
  }

  wxRect TargetRect() {
    const Step &s = m_steps[m_idx];
    if (!s.target || !s.target->IsShownOnScreen()) return wxRect();
    wxRect r = s.target->GetScreenRect();
    r.SetTopLeft(ScreenToClient(r.GetTopLeft()));
    return r;
  }

  std::vector<wxString> Wrap(const wxString &text, int maxW) {
    std::vector<wxString> lines;
    wxString line;
    wxStringTokenizer tok(text, " ");
    while (tok.HasMoreTokens()) {
      wxString w = tok.GetNextToken();
      wxString cand = line.IsEmpty() ? w : line + " " + w;
      if (GetTextExtent(cand).GetWidth() > maxW && !line.IsEmpty()) {
        lines.push_back(line);
        line = w;
      } else {
        line = cand;
      }
    }
    if (!line.IsEmpty()) lines.push_back(line);
    return lines;
  }

  void LayoutStep() {
    const wxSize cs = GetClientSize();
    const int pad = 12;
    const int capW = wxMin(320, cs.x - 2 * pad);
    m_lines = Wrap(m_steps[m_idx].body, capW - 2 * pad);
    const int lineH = GetCharHeight() + 3;
    const int titleH = GetCharHeight() + 8;
    const int btnH = m_next->GetBestSize().y;
    const int capH =
        pad + titleH + (int)m_lines.size() * lineH + 8 + btnH + pad;

    // place the caption below the highlighted control, else above, else centre
    const wxRect t = TargetRect();
    int cx = (cs.x - capW) / 2, cy;
    if (t.IsEmpty()) {
      cy = (cs.y - capH) / 2;
    } else {
      cx = wxMax(pad, wxMin(cs.x - capW - pad,
                            t.x + t.width / 2 - capW / 2));
      if (t.GetBottom() + 8 + capH <= cs.y - pad)
        cy = t.GetBottom() + 8;
      else
        cy = wxMax(pad, t.y - 8 - capH);
    }
    m_capBox = wxRect(cx, cy, capW, capH);

    // navigation buttons along the caption footer
    const int by = m_capBox.GetBottom() - pad - btnH + 1;
    int rx = m_capBox.GetRight() - pad;
    const bool last = m_idx + 1 >= (int)m_steps.size();
    m_next->SetLabel(last ? L"Finish" : L"Next \u2192");
    for (wxButton *btn : {m_next, m_back}) {
      const int w = btn->GetBestSize().x;
      rx -= w;
      btn->SetSize(rx, by, w, btnH);
      rx -= 6;
    }
    m_back->Enable(m_idx > 0);
    m_skip->SetSize(m_capBox.x + pad, by, m_skip->GetBestSize().x, btnH);
    m_skip->Show(!last);
    Refresh();
  }

  void OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    const wxSize cs = GetClientSize();
    wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
    if (!gc) return;
    const wxColour dim(10, 18, 30, 150);
    const wxRect t = TargetRect();
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(wxBrush(dim));
    if (t.IsEmpty()) {
      gc->DrawRectangle(0, 0, cs.x, cs.y);
    } else {
      // dim everything except the highlighted control (four surround strips)
      gc->DrawRectangle(0, 0, cs.x, t.y);
      gc->DrawRectangle(0, t.GetBottom() + 1, cs.x, cs.y - t.GetBottom() - 1);
      gc->DrawRectangle(0, t.y, t.x, t.height);
      gc->DrawRectangle(t.GetRight() + 1, t.y, cs.x - t.GetRight() - 1,
                        t.height);
      gc->SetBrush(*wxTRANSPARENT_BRUSH);
      gc->SetPen(wxPen(wxColour(255, 190, 0), 3));
      gc->DrawRoundedRectangle(t.x - 3, t.y - 3, t.width + 6, t.height + 6, 4);
    }

    // caption card
    gc->SetPen(wxPen(wxColour(0, 0, 76), 2));
    gc->SetBrush(wxBrush(wxColour(252, 250, 244)));
    gc->DrawRoundedRectangle(m_capBox.x, m_capBox.y, m_capBox.width,
                             m_capBox.height, 8);
    const int pad = 12;
    int y = m_capBox.y + pad;
    gc->SetFont(GetFont().Bold(), wxColour(0, 0, 76));
    const wxString heading = wxString::Format("%s   (%d/%d)",
                                              m_steps[m_idx].title, m_idx + 1,
                                              (int)m_steps.size());
    gc->DrawText(heading, m_capBox.x + pad, y);
    y += GetCharHeight() + 8;
    gc->SetFont(GetFont(), wxColour(28, 39, 51));
    for (const auto &ln : m_lines) {
      gc->DrawText(ln, m_capBox.x + pad, y);
      y += GetCharHeight() + 3;
    }
    delete gc;
  }

  wxWindow *m_host;
  std::vector<Step> m_steps;
  int m_idx = 0;
  wxButton *m_back, *m_next, *m_skip;
  wxRect m_capBox;
  std::vector<wxString> m_lines;
};

class S411CtrlDialog : public wxDialog {
public:
  S411CtrlDialog(wxWindow *parent, s411_pi *pi)
      : wxDialog(parent, wxID_ANY, "S-411 Ice Information  v" S411_VERSION_STRING,
                 wxDefaultPosition,
                 wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        m_pi(pi) {
    auto *s = new wxBoxSizer(wxVERTICAL);
    const int b = 5;

    m_show = new wxCheckBox(this, wxID_ANY, "Show ice overlay");
    s->Add(m_show, 0, wxALL, b);

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    m_loadBtn = new wxButton(this, wxID_ANY, L"Load data\u2026");
    m_download = new wxButton(this, wxID_ANY, L"Current charts\u2026");
    m_download->SetToolTip(
        "Download the newest S-411 chart of every ice service (BSH Ice "
        "Portal) and load it");
    auto *clear = new wxButton(this, wxID_ANY, "Clear");
    m_help = new wxButton(this, wxID_ANY, "?", wxDefaultPosition, wxSize(28, -1));
    m_help->SetToolTip("Show the guided tour again");
    btns->Add(m_loadBtn, 0, wxRIGHT, b);
    btns->Add(m_download, 0, wxRIGHT, b);
    btns->Add(clear, 0);
    btns->AddStretchSpacer(1);
    btns->Add(m_help, 0);
    s->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, b);

    m_status = new wxStaticText(this, wxID_ANY, "");
    s->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);
    m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition,
                          wxSize(-1, 8));
    s->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, b);
    m_gauge->Hide();

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
    m_warn = new wxStaticText(this, wxID_ANY, "");
    m_warn->SetForegroundColour(wxColour(190, 0, 0));
    s->Add(m_warn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, b);

    auto *grid = new wxFlexGridSizer(5, 2, b, b);
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

    grid->Add(new wxStaticText(this, wxID_ANY, "Age limit (hours)"), 0,
              wxALIGN_CENTRE_VERTICAL);
    m_ageLimit = new wxSpinCtrl(this, wxID_ANY, "72", wxDefaultPosition,
                                wxDefaultSize, wxSP_ARROW_KEYS, 1, 720, 72);
    m_ageLimit->SetToolTip(
        "Ice information older than this is marked on the chart and in the "
        "dataset list (S-411 PS clause 8.1, default 72 h)");
    grid->Add(m_ageLimit, 0);

    s->Add(grid, 0, wxEXPAND | wxALL, b);
    m_values = new wxCheckBox(this, wxID_ANY,
                              "Show ice values (thickness cm, drift kn)");
    s->Add(m_values, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);
    m_ageBox = new wxCheckBox(this, wxID_ANY, "Show data age on the chart");
    s->Add(m_ageBox, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);

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
    m_pickStart = new wxButton(this, wxID_ANY, L"Set\u2026", wxDefaultPosition,
                               wxSize(56, -1));
    rg->Add(m_pickStart, 0);
    rg->Add(new wxStaticText(this, wxID_ANY, "End"), 0,
            wxALIGN_CENTRE_VERTICAL);
    m_endLbl = new wxStaticText(this, wxID_ANY, "(click the map)");
    rg->Add(m_endLbl, 1, wxALIGN_CENTRE_VERTICAL | wxEXPAND);
    auto *pickE = new wxButton(this, wxID_ANY, L"Set\u2026", wxDefaultPosition,
                               wxSize(56, -1));
    rg->Add(pickE, 0);
    s->Add(rg, 0, wxEXPAND | wxALL, b);

    m_avoidCaution = new wxCheckBox(
        this, wxID_ANY, "Avoid caution ice too (not just hazardous)");
    s->Add(m_avoidCaution, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);

    auto *rb = new wxBoxSizer(wxHORIZONTAL);
    m_compute = new wxButton(this, wxID_ANY, "Compute ice route");
    rb->Add(m_compute, 0, wxRIGHT, b);
    m_export = new wxButton(this, wxID_ANY, "Export no-go zones");
    rb->Add(m_export, 0);
    s->Add(rb, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);
    auto *rc = new wxBoxSizer(wxHORIZONTAL);
    m_check = new wxButton(this, wxID_ANY, "Check active route");
    m_check->SetToolTip(
        "Check OpenCPN's active (or selected) route against the displayed "
        "ice: hazardous ice, difficult ice and icebergs per leg");
    rc->Add(m_check, 0, wxRIGHT, b);
    auto *clearMarks = new wxButton(this, wxID_ANY, "Clear marks");
    rc->Add(clearMarks, 0);
    s->Add(rc, 0, wxLEFT | wxRIGHT | wxBOTTOM, b);
    m_routeStatus = new wxStaticText(this, wxID_ANY, "");
    m_routeStatus->Wrap(320);
    s->Add(m_routeStatus, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, b);

    m_pickStart->Bind(wxEVT_BUTTON,
                      [this](wxCommandEvent &) { m_pi->BeginPickPoint(0); });
    pickE->Bind(wxEVT_BUTTON,
                [this](wxCommandEvent &) { m_pi->BeginPickPoint(1); });
    m_avoidCaution->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
      m_pi->RouteParams().avoidCaution = m_avoidCaution->GetValue();
    });
    m_compute->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnCompute, this);
    m_export->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnExport, this);
    m_check->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnCheckRoute, this);
    clearMarks->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
      m_pi->ClearRouteAlerts();
      m_pi->Refresh();
    });
    m_download->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
      m_pi->DownloadCurrentCharts();
      UpdateStatus();
    });
    m_help->Bind(wxEVT_BUTTON,
                 [this](wxCommandEvent &) { StartTutorial(); });

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
    m_ageLimit->Bind(wxEVT_SPINCTRL, &S411CtrlDialog::OnAny, this);
    m_values->Bind(wxEVT_CHECKBOX, &S411CtrlDialog::OnAny, this);
    m_ageBox->Bind(wxEVT_CHECKBOX, &S411CtrlDialog::OnAny, this);
    m_loadBtn->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnLoad, this);
    clear->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnClear, this);
    m_list->Bind(wxEVT_CHECKLISTBOX, &S411CtrlDialog::OnListCheck, this);
    zoom->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnZoom, this);
    remove->Bind(wxEVT_BUTTON, &S411CtrlDialog::OnRemove, this);
    close->Bind(wxEVT_BUTTON,
                [this](wxCommandEvent &) { m_pi->CancelPick(); Hide(); });
    Bind(wxEVT_CLOSE_WINDOW,
         [this](wxCloseEvent &) { m_pi->CancelPick(); Hide(); });
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
    m_ageLimit->SetValue((int)m_pi->m_ageLimitH);
    m_values->SetValue(m_pi->m_showValues);
    m_ageBox->SetValue(m_pi->m_showAgeBox);
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
    const bool busy = m_pi->IsLoading();
    if (busy) {
      m_status->SetLabel(m_pi->LoadProgress());
      m_gauge->Pulse();
    } else {
      m_status->SetLabel(wxString::Format(
          "%zu dataset(s), %zu feature(s) loaded", m_pi->DatasetCount(),
          m_pi->FeatureCount()));
    }
    const bool layout = m_gauge->IsShown() != busy;
    m_gauge->Show(busy);
    m_loadBtn->SetLabel(busy ? L"Cancel loading" : L"Load data\u2026");
    m_download->Enable(!busy);
    if (!busy || layout) {
      RebuildList();
      m_warn->SetLabel(m_pi->Warnings());
      m_warn->Wrap(360);
      Layout();
      Fit();
    }
  }

private:
  void RebuildList() {
    const int sel = m_list->GetSelection();
    m_list->Clear();
    for (const auto &ds : m_pi->Data()) {
      int i = m_list->Append(m_pi->DatasetLabel(ds));
      m_list->Check(i, ds.visible);
    }
    if (sel >= 0 && sel < (int)m_list->GetCount()) m_list->SetSelection(sel);
  }
  void ToState() {
    m_pi->m_show = m_show->GetValue();
    m_pi->m_mode = (S411Mode)m_mode->GetSelection();
    static const char *cls[] = {"", "IC", "IB", "IA", "IASuper"};
    m_pi->m_iceClass = cls[wxMax(0, m_class->GetSelection())];
    m_pi->m_palette = (S411Palette)m_pal->GetSelection();
    m_pi->m_alpha = wxMax(26, wxMin(255, m_opacity->GetValue() * 255 / 100));
    const double limit = m_ageLimit->GetValue();
    const bool limitChanged = limit != m_pi->m_ageLimitH;
    m_pi->m_ageLimitH = limit;
    m_pi->m_showValues = m_values->GetValue();
    m_pi->m_showAgeBox = m_ageBox->GetValue();
    if (limitChanged) UpdateStatus();  // list flags depend on the limit
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
    if (m_pi->IsLoading()) {
      m_pi->CancelLoad();
      return;
    }
    m_pi->LoadFiles();  // results arrive later via s411_pi::FinishLoad
    UpdateStatus();
  }
  void OnClear(wxCommandEvent &) {
    if (m_pi->IsLoading()) m_pi->CancelLoad();
    m_pi->ClearData();
    m_show->SetValue(false);
    UpdateStatus();
    m_pi->Refresh();
  }
  void OnListCheck(wxCommandEvent &e) {
    const int i = e.GetInt();
    if (i >= 0 && (size_t)i < m_pi->Data().size()) {
      S411Dataset &ds = m_pi->Data()[i];
      if (m_list->IsChecked(i) && ds.signature == S411SigState::Invalid) {
        m_list->Check(i, false);  // PS clause 11.3 rule 4
        wxMessageBox("This dataset failed the digital signature check and "
                     "must not be displayed:\n" + ds.signatureNote,
                     "S-411 signature", wxOK | wxICON_ERROR, this);
        return;
      }
      ds.visible = m_list->IsChecked(i);
      m_pi->DataChanged();
      UpdateStatus();
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
    m_pi->DataChanged();
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
  void OnCheckRoute(wxCommandEvent &) {
    const wxString r = m_pi->CheckActiveRoute();
    wxDialog dlg(this, wxID_ANY, "S-411 route ice check", wxDefaultPosition,
                 wxSize(460, 380), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto *sz = new wxBoxSizer(wxVERTICAL);
    auto *t = new wxTextCtrl(&dlg, wxID_ANY, r, wxDefaultPosition,
                             wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
    sz->Add(t, 1, wxEXPAND | wxALL, 5);
    sz->Add(dlg.CreateButtonSizer(wxOK), 0, wxALIGN_RIGHT | wxALL, 5);
    dlg.SetSizer(sz);
    dlg.ShowModal();
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
  wxSpinCtrl *m_ageLimit;
  wxCheckBox *m_values, *m_ageBox;
  wxGauge *m_gauge;
  wxStaticText *m_status, *m_warn;
  wxCheckListBox *m_list;
  S411LegendPanel *m_legend;
  wxStaticText *m_startLbl = nullptr, *m_endLbl = nullptr,
               *m_routeStatus = nullptr;
  wxCheckBox *m_avoidCaution = nullptr;
  wxButton *m_loadBtn = nullptr, *m_export = nullptr, *m_help = nullptr,
           *m_pickStart = nullptr, *m_compute = nullptr,
           *m_download = nullptr, *m_check = nullptr;
  S411Tutorial *m_tutorial = nullptr;

public:
  /** Launch the guided tour (also called automatically the first time the
   *  panel is opened). Safe to call repeatedly. */
  void StartTutorial() {
    if (!m_tutorial) m_tutorial = new S411Tutorial(this);
    m_tutorial->SetSteps({
        {nullptr,
         "Welcome to S-411 Ice",
         "This panel controls the ice overlay. This quick tour points out "
         "each control. Use Next, or Skip to leave. You can reopen it any "
         "time with the ? button."},
        {m_loadBtn, "Load ice data",
         "Open S-411 datasets: single GML files, a whole exchange set "
         "(pick its CATALOG.XML) or a .zip archive. The chart jumps to the "
         "data once it loads."},
        {m_download, "Current ice charts",
         "Downloads the newest chart of every ice service (Canada, USA, "
         "Norway, Denmark, Russia, Germany ...) and loads it. Loading runs "
         "in the background; the bar below shows progress."},
        {m_show, "Show / hide",
         "Toggle the ice overlay on the chart. Loaded data stays in memory "
         "when hidden."},
        {m_list, "Your datasets",
         "Each dataset shows its producer, analysis time, age and signature "
         "state. Outdated data, forecasts and data older than 30 days start "
         "hidden - tick to show. Where producers overlap, untick the one "
         "you do not want."},
        {m_ageLimit, "Age limit",
         "Data older than this many hours is flagged in the list and in the "
         "age box on the chart."},
        {m_mode, "Display mode",
         "Colour the ice by concentration, by stage of development, or as a "
         "navigational traffic light (green/amber/red)."},
        {m_class, "Vessel ice class",
         "In the navigational mode this sets the green/amber/red thresholds "
         "for your ship's ice class - and it drives the route planner."},
        {m_pal, "Palette",
         "Day, Dusk or Night colours. The fill-opacity slider below controls "
         "how solid the ice areas look over the chart."},
        {m_pickStart, "Plan an ice route",
         "Set a start and an end position by clicking the chart, choose a "
         "vessel ice class above, then Compute ice route: an ice-aware path "
         "is added to OpenCPN, avoiding hazardous ice and land."},
        {m_compute, "Compute / recompute",
         "Computes the route on the visible datasets. Recomputing replaces "
         "the previous ice route rather than stacking a new one."},
        {m_check, "Check a route",
         "Checks OpenCPN's active route leg by leg for hazardous ice, "
         "difficult ice and icebergs for your ice class, lists the legs and "
         "marks the positions on the chart."},
        {m_export, "Weather Routing",
         "With the ocpn_draw_pi plugin installed, export the dangerous ice "
         "as exclusion boundaries so the Weather Routing plugin avoids them "
         "too."},
        {m_help, "That's it",
         "Click any ice feature on the chart for a full attribute report. "
         "Reopen this tour with the ? button whenever you need it. Fair "
         "winds!"},
    });
    m_tutorial->Start();
  }

  /** Run the tour automatically the very first time the panel is shown. */
  void MaybeAutoTutorial() {
    if (m_pi->m_tutorialSeen) return;
    m_pi->MarkTutorialSeen();
    CallAfter(&S411CtrlDialog::StartTutorial);
  }
};

// ===========================================================================
// plugin factory
// ===========================================================================
extern "C" DECL_EXP opencpn_plugin *create_pi(void *ppimgr) {
  return new s411_pi(ppimgr);
}
extern "C" DECL_EXP void destroy_pi(opencpn_plugin *p) { delete p; }

s411_pi::s411_pi(void *ppimgr) : opencpn_plugin_117(ppimgr) { MakeIcon(); }

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
         "Shows the age of the ice information, applies the dataset\n"
         "precedence rules, verifies S-100 signatures, draws the Annex C\n"
         "symbols, checks routes for ice and downloads current charts.\n"
         "Version " S411_VERSION_STRING;
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
  m_cancel = true;  // a running load stops after its current file
  JoinLoader();
  delete m_loadTimer;
  m_loadTimer = nullptr;
  m_loading = false;
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

// ---------------------------------------------------------------------------
// loading: a worker thread parses and tessellates (the NIC Arctic chart
// still takes a few seconds), a timer on the GUI thread merges the result
// ---------------------------------------------------------------------------
void s411_pi::LoadFiles() {
  if (m_loading) return;
  wxFileDialog dlg(
      GetOCPNCanvasWindow(), "Open S-411 data", m_lastDir, "",
      "S-411 / SIGRID-3 data (*.gml;*.xml;*.zip;*.gz;CATALOG.XML;*.shp)|"
      "*.gml;*.GML;*.xml;*.XML;*.zip;*.ZIP;*.gz;*.GZ;*.shp;*.SHP|"
      "All files|*.*",
      wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() != wxID_OK) return;
  wxArrayString paths;
  dlg.GetPaths(paths);
  m_lastDir = dlg.GetDirectory();
  LoadPaths(paths);
}

void s411_pi::LoadPaths(const wxArrayString &pathsIn) {
  if (m_loading || pathsIn.IsEmpty()) return;
  JoinLoader();
  m_loading = true;
  m_loadDone = false;
  m_cancel = false;
  {
    wxMutexLocker lk(m_loadMx);
    m_loadOut.clear();
    m_loadErrors.Clear();
    m_loadMsg = "Loading...";
  }
  // wxString copies are not thread-safe to share: hand over deep copies
  std::vector<wxString> paths;
  for (const auto &p : pathsIn) paths.push_back(wxString(p.wc_str()));
  m_loader = new S411Worker([this, paths]() {
    std::vector<S411Dataset> out;
    wxString errors;
    for (size_t i = 0; i < paths.size() && !m_cancel; i++) {
      const wxFileName fn(paths[i]);
      {
        wxMutexLocker lk(m_loadMx);
        m_loadMsg = wxString::Format("Loading %zu of %zu: %s", i + 1,
                                     paths.size(), fn.GetFullName());
      }
      S411LoadPath(paths[i], out, errors);
    }
    wxMutexLocker lk(m_loadMx);
    m_loadOut = std::move(out);
    m_loadErrors = errors;
    m_loadDone = true;
  });
  if (m_loader->Run() != wxTHREAD_NO_ERROR) {  // no thread: load right here
    delete m_loader;
    m_loader = nullptr;
    std::vector<S411Dataset> out;
    wxString errors;
    for (const auto &p : paths) S411LoadPath(p, out, errors);
    m_loadOut = std::move(out);
    m_loadErrors = errors;
    FinishLoad();
    return;
  }
  if (!m_loadTimer) m_loadTimer = new S411LoadTimer(this);
  m_loadTimer->Start(200);
  if (m_dialog) m_dialog->UpdateStatus();
}

wxString s411_pi::LoadProgress() {
  wxMutexLocker lk(m_loadMx);
  return m_loadMsg;
}

void s411_pi::PollLoad() {
  if (!m_loadDone) {
    if (m_dialog) m_dialog->UpdateStatus();
    return;
  }
  m_loadTimer->Stop();
  JoinLoader();
  FinishLoad();
}

void s411_pi::JoinLoader() {
  if (!m_loader) return;
  m_loader->Wait();
  delete m_loader;
  m_loader = nullptr;
}

void s411_pi::FinishLoad() {
  std::vector<S411Dataset> got;
  wxString errors;
  {
    wxMutexLocker lk(m_loadMx);
    got.swap(m_loadOut);
    errors = m_loadErrors;
  }
  const bool cancelled = m_cancel;
  m_loading = false;
  if (cancelled) got.clear();

  // PS clause 8.1: outdated data only on request, > 30 days not by default
  const time_t now = time(nullptr);
  size_t hiddenAge = 0, hiddenSig = 0, forecasts = 0;
  const size_t before = m_data.size();
  for (auto &ds : got) {
    if (ds.signature == S411SigState::Invalid) hiddenSig++;
    else if (ds.forecastTime) forecasts++;
    const S411Age a = S411DatasetAge(ds, now, m_ageLimitH);
    if (ds.visible && (a == S411Age::Outdated || a == S411Age::Expired)) {
      ds.visible = false;
      hiddenAge++;
    }
    m_data.push_back(std::move(ds));
  }
  DataChanged();

  wxString info;
  if (hiddenAge)
    info += wxString::Format(
        "%zu dataset(s) are outdated or older than 30 days and are not "
        "displayed (S-411 PS clause 8.1). Tick them in the dataset list to "
        "show them anyway.\n",
        hiddenAge);
  if (forecasts)
    info += wxString::Format(
        "%zu forecast dataset(s) loaded but not displayed by default - tick "
        "them in the dataset list to show a forecast.\n",
        forecasts);
  if (hiddenSig)
    info += wxString::Format(
        "%zu dataset(s) failed the digital signature check and are not "
        "displayed (S-411 PS clause 11.3).\n",
        hiddenSig);
  if (!errors.IsEmpty())
    wxMessageBox(errors, "S-411: some files could not be loaded",
                 wxOK | wxICON_WARNING, GetOCPNCanvasWindow());
  if (!info.IsEmpty())
    wxMessageBox(info, "S-411 data loaded", wxOK | wxICON_INFORMATION,
                 GetOCPNCanvasWindow());
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
  if (m_dialog) {
    m_dialog->FromState();
    m_dialog->UpdateStatus();
  }
  Refresh();
}

void s411_pi::DataChanged() { S411ApplyPrecedence(m_data); }

wxString s411_pi::DatasetLabel(const S411Dataset &ds) const {
  const time_t now = time(nullptr);
  wxString s = ds.title;
  wxString meta = ds.producer;
  if (ds.analysisTime) {
    const double h = difftime(now, ds.analysisTime) / 3600.0;
    meta += (meta.IsEmpty() ? "" : ", ") +
            S411FormatUtc(ds.analysisTime, ds.timeApprox) +
            (ds.timeApprox ? "" : " UTC") +
            (h < 48 ? wxString::Format(", %.0f h old", h)
                    : wxString::Format(", %.0f days old", h / 24.0));
  } else {
    meta += (meta.IsEmpty() ? "" : ", ") + wxString("age unknown");
  }
  meta += ", " + S411SigLabel(ds.signature);
  s += "  (" + meta + ")";
  if (ds.forecastTime)
    s += " [forecast for " + S411FormatUtc(ds.forecastTime) + " UTC]";
  switch (S411DatasetAge(ds, now, m_ageLimitH)) {
    case S411Age::Outdated: s += " [OUTDATED]"; break;
    case S411Age::Expired: s += " [older than 30 days]"; break;
    case S411Age::OverLimit: s += " [older than age limit]"; break;
    default: break;
  }
  if (!ds.supersededBy.IsEmpty()) s += " [superseded by " + ds.supersededBy + "]";
  return s;
}

wxString s411_pi::Warnings() const {
  wxString w;
  const wxString ov = S411OverlappingProducers(m_data);
  if (!ov.IsEmpty())
    w += "Data of several producers overlap (" + ov +
         ") - untick the producer you do not want in an area.\n";
  size_t old = 0;
  const time_t now = time(nullptr);
  for (const auto &d : m_data) {
    if (!d.visible) continue;
    const S411Age a = S411DatasetAge(d, now, m_ageLimitH);
    if (a == S411Age::Outdated || a == S411Age::Expired ||
        a == S411Age::OverLimit)
      old++;
  }
  if (old)
    w += wxString::Format("%zu displayed dataset(s) are outdated or older "
                          "than the age limit.\n",
                          old);
  w.Trim();
  return w;
}

// ---------------------------------------------------------------------------
// current charts of the ice services (IMPROVEMENTS 4.2): the BSH Ice Portal
// lists every service's S-411 zips in one directory; the newest per service
// is offered, downloaded with OpenCPN's downloader and loaded
// ---------------------------------------------------------------------------
static const char *kIcePortal = "https://www.bsis-ice.de/IcePortal/S411/";

void s411_pi::DownloadCurrentCharts() {
  if (m_loading) return;
  wxString base = kIcePortal;
  if (wxFileConfig *cfg = GetOCPNConfigObject()) {
    cfg->SetPath("/PlugIns/S411");
    base = cfg->Read("DownloadUrl", base);
  }
  if (!base.EndsWith("/")) base += "/";
  const wxString dir = *GetpPrivateApplicationDataLocation() +
                       wxFileName::GetPathSeparator() + "s411_pi" +
                       wxFileName::GetPathSeparator() + "downloads";
  wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
  const wxString index = dir + wxFileName::GetPathSeparator() + "index.html";
  if (OCPN_downloadFile(base, index, "S-411 ice charts",
                        "Reading the list of current ice charts...",
                        wxNullBitmap, GetOCPNCanvasWindow(),
                        OCPN_DLDS_ELAPSED_TIME | OCPN_DLDS_CAN_ABORT |
                            OCPN_DLDS_AUTO_CLOSE,
                        30) != OCPN_DL_NO_ERROR) {
    wxMessageBox("Could not read " + base, "S-411 ice charts",
                 wxOK | wxICON_WARNING, GetOCPNCanvasWindow());
    return;
  }
  wxString html;
  wxFFile(index, "rb").ReadAll(&html, wxConvUTF8);

  // href="....zip" -> newest per service (the name without its date)
  struct Chart { wxString file, date; };
  std::map<wxString, Chart> newest;
  int pos = 0;
  while ((pos = html.find("href=\"", pos)) != wxNOT_FOUND) {
    pos += 6;
    const int end = html.find('"', pos);
    if (end == wxNOT_FOUND) break;
    const wxString f = html.Mid(pos, end - pos);
    pos = end;
    if (!f.Lower().EndsWith(".zip") || f.Contains("/")) continue;
    wxString key, date;
    for (size_t i = 0; i < f.length(); i++) {
      size_t n = 0;
      while (i + n < f.length() && wxIsdigit(f[i + n])) n++;
      if (n == 8 && date.IsEmpty() && f.Mid(i, 2) == "20") {
        date = f.Mid(i, 8);
        size_t t = i + 8;  // optional THHMMZ
        if (t < f.length() && f[t] == 'T') {
          while (t + 1 < f.length() && (wxIsdigit(f[t + 1]) || f[t + 1] == 'Z'))
            t++;
          date += f.Mid(i + 8, t + 1 - (i + 8));
          i = t;
        } else {
          i += 7;
        }
        key += "#";
        continue;
      }
      key += f[i];
      if (n > 1) { key += f.Mid(i + 1, n - 1); i += n - 1; }
    }
    if (date.IsEmpty()) continue;
    auto it = newest.find(key);
    if (it == newest.end() || date > it->second.date) newest[key] = {f, date};
  }
  if (newest.empty()) {
    wxMessageBox("No S-411 charts found at " + base, "S-411 ice charts",
                 wxOK | wxICON_INFORMATION, GetOCPNCanvasWindow());
    return;
  }
  wxArrayString labels, files;
  for (const auto &kv : newest) {
    const wxString &d = kv.second.date;
    labels.Add(wxString::Format("%s   (%s-%s-%s)", kv.second.file, d.Mid(0, 4),
                                d.Mid(4, 2), d.Mid(6, 2)));
    files.Add(kv.second.file);
  }
  wxMultiChoiceDialog pick(GetOCPNCanvasWindow(),
                           "Newest chart of every ice service on the BSH Ice "
                           "Portal. Select the charts to download and load:",
                           "S-411: download current ice charts", labels);
  if (pick.ShowModal() != wxID_OK) return;
  const wxArrayInt sel = pick.GetSelections();
  wxArrayString got;
  wxString failed;
  for (int i : sel) {
    const wxString out = dir + wxFileName::GetPathSeparator() + files[i];
    if (wxFileExists(out)) {  // already downloaded earlier: same content
      got.Add(out);
      continue;
    }
    const _OCPN_DLStatus st = OCPN_downloadFile(
        base + files[i], out, "S-411 ice charts", "Downloading " + files[i],
        wxNullBitmap, GetOCPNCanvasWindow(),
        OCPN_DLDS_ELAPSED_TIME | OCPN_DLDS_CAN_ABORT | OCPN_DLDS_AUTO_CLOSE |
            OCPN_DLDS_SIZE,
        120);
    if (st == OCPN_DL_NO_ERROR) {
      got.Add(out);
    } else {
      wxRemoveFile(out);  // never keep a partial download
      failed += files[i] + "\n";
      if (st == OCPN_DL_ABORTED) break;
    }
  }
  if (!failed.IsEmpty())
    wxMessageBox("Download failed:\n" + failed, "S-411 ice charts",
                 wxOK | wxICON_WARNING, GetOCPNCanvasWindow());
  LoadPaths(got);
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
  m_routeAlerts.clear();
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
  m_dialog->MaybeAutoTutorial();  // guided tour on first open
}

void s411_pi::MarkTutorialSeen() {
  m_tutorialSeen = true;
  SaveConfig();
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
      L"Position  %.4f\u00b0%s  %.4f\u00b0%s\n\n", std::fabs(lat),
      lat >= 0 ? "N" : "S", std::fabs(lon), lon >= 0 ? "E" : "W");
  for (const auto &h : hits) {
    txt += h.feature->type;
    if (h.feature->fastIce) txt += "   [fast ice]";
    txt += "\n  dataset: " + h.ds->title + "\n";
    // PS clause 4.6: producer, analysis time and age at the picked position
    txt += "  producer: " +
           (h.ds->producer.IsEmpty() ? wxString("unknown") : h.ds->producer) +
           (h.ds->agency.IsEmpty() || h.ds->agency == h.ds->producer
                ? wxString()
                : " (" + h.ds->agency + ")") +
           "\n";
    txt += "  " + S411AgeText(*h.ds, time(nullptr)) + "\n";
    txt += "  signature: " + S411SigLabel(h.ds->signature) +
           (h.ds->signatureNote.IsEmpty() ? wxString()
                                          : " - " + h.ds->signatureNote) +
           "\n";
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
    m_reportEgg = new S411EggPanel(m_report);
    s->Add(m_reportEgg, 0, wxALIGN_CENTRE_HORIZONTAL | wxTOP, 8);
    m_reportText =
        new wxTextCtrl(m_report, wxID_ANY, "", wxDefaultPosition,
                       wxDefaultSize,
                       wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    m_reportText->SetFont(wxFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                                 wxFONTWEIGHT_NORMAL));
    m_reportText->SetMinSize(wxSize(400, 260));
    s->Add(m_reportText, 1, wxEXPAND | wxALL, 5);
    auto *close = new wxButton(m_report, wxID_CLOSE, "Close");
    s->Add(close, 0, wxALIGN_RIGHT | wxALL, 5);
    m_report->SetSizer(s);
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_report->Hide(); });
    m_report->Bind(wxEVT_CLOSE_WINDOW,
                   [this](wxCloseEvent &) { m_report->Hide(); });
  }
  m_reportText->SetValue(txt);
  // WMO egg of every picked ice area that carries egg-code data (max three)
  std::vector<std::pair<S411Egg, wxString>> eggs;
  for (const auto &h : hits)
    if (h.feature->kind == S411Kind::Area && !h.feature->egg.Empty() &&
        eggs.size() < 3)
      eggs.push_back({h.feature->egg, h.feature->type});
  m_reportEgg->SetEggs(eggs);
  m_report->SetMinSize(wxSize(420, eggs.empty() ? 340 : 560));
  m_report->Layout();
  m_report->Fit();
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
  return wxString::Format(L"%.4f\u00b0%s  %.4f\u00b0%s", std::fabs(p.lat),
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
  if (r.waypoints.size() < 2)
    return "Start and end are effectively the same position - nothing to "
           "route.";

  // replace a previous ice route instead of stacking a new one each recompute
  if (!m_lastRouteGuid.IsEmpty()) {
    DeletePlugInRoute(m_lastRouteGuid);
    m_lastRouteGuid.Clear();
  }

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

static wxString DegMin(double v, bool lat) {
  const char h = lat ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W');
  v = std::fabs(v);
  const int d = (int)v;
  return wxString::Format(lat ? L"%02d\u00b0%04.1f'%c" : L"%03d\u00b0%04.1f'%c", d,
                          (v - d) * 60.0, h);
}

wxString s411_pi::CheckActiveRoute() {
  m_routeAlerts.clear();
  wxString guid = GetActiveRouteGUID();
  wxString which = "active route";
  if (guid.IsEmpty()) {
    guid = GetSelectedRouteGUID_Plugin();
    which = "selected route";
  }
  if (guid.IsEmpty()) {
    guid = m_lastRouteGuid;
    which = "last S-411 ice route";
  }
  if (guid.IsEmpty())
    return "No route to check: activate or select a route in OpenCPN first.";
  std::unique_ptr<PlugIn_Route> route = GetRoute_Plugin(guid);
  if (!route || !route->pWaypointList)
    return "The route could not be read from OpenCPN.";
  std::vector<S411Pos> wps;
  std::vector<wxString> names;
  for (auto node = route->pWaypointList->GetFirst(); node;
       node = node->GetNext()) {
    const PlugIn_Waypoint *wp = node->GetData();
    wps.push_back({wp->m_lat, wp->m_lon});
    names.push_back(wp->m_MarkName);
  }
  if (wps.size() < 2) return "The route has fewer than two waypoints.";
  bool anyVisible = false;
  for (const auto &d : m_data) anyVisible |= d.visible && !d.features.empty();
  if (!anyVisible)
    return "No ice data displayed - load and show S-411 data first.";

  std::vector<S411RouteAlert> alerts;
  S411CheckRoute(m_data, wps, m_iceClass, 1.0, alerts);
  m_routeAlerts = alerts;
  Refresh();

  wxString name = route->m_NameString.IsEmpty() ? which : route->m_NameString;
  wxString txt = wxString::Format(
      "Ice check of %s '%s'\n%zu legs, vessel ice class: %s\n"
      "Only displayed ice data is checked (S-411 PS clauses 8.1, 9.6).\n\n",
      which, name, wps.size() - 1,
      m_iceClass.IsEmpty() ? wxString("none") : m_iceClass);
  if (alerts.empty())
    return txt + "No ice alerts on this route.";
  for (const auto &a : alerts) {
    const wxString from = names[a.leg].IsEmpty()
                              ? wxString::Format("wp%zu", a.leg + 1)
                              : names[a.leg];
    const wxString to = names[a.leg + 1].IsEmpty()
                            ? wxString::Format("wp%zu", a.leg + 2)
                            : names[a.leg + 1];
    txt += wxString::Format("Leg %zu (%s -> %s): %s\n  at %s %s - %s\n  [%s]\n\n",
                            a.leg + 1, from, to, S411AlertName(a.alert),
                            DegMin(a.where.lat, true),
                            DegMin(a.where.lon, false), a.feature, a.dataset);
  }
  size_t hz = 0;
  for (const auto &a : alerts) hz += a.alert == S411Alert::IceHazard;
  txt += wxString::Format(
      "%zu alert(s), %zu of them IceHazardForVessel. The positions are "
      "marked on the chart.",
      alerts.size(), hz);
  return txt;
}

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
  cfg->Read("TutorialSeen", &m_tutorialSeen, false);
  double lim = 72;
  cfg->Read("AgeLimitHours", &lim, 72.0);
  m_ageLimitH = wxMax(1.0, wxMin(720.0, lim));
  cfg->Read("ShowValues", &m_showValues, true);
  cfg->Read("ShowAgeBox", &m_showAgeBox, true);
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
  cfg->Write("TutorialSeen", m_tutorialSeen);
  cfg->Write("AgeLimitHours", m_ageLimitH);
  cfg->Write("ShowValues", m_showValues);
  cfg->Write("ShowAgeBox", m_showAgeBox);
  cfg->Flush();
}

// ---------------------------------------------------------------------------
// geometry -> canvas (longitudes already unwrapped at load time)
// ---------------------------------------------------------------------------
static double WorldShift(double lonCentre, double clon) {
  return 360.0 * std::floor((lonCentre - clon + 180.0) / 360.0);
}

// every world copy (shift by a multiple of 360 deg) of [lonMin, lonMax]
// that meets the view - a 360 deg wide circumpolar area needs two copies
// where the view straddles its seam
static void WorldShifts(double lonMin, double lonMax, PlugIn_ViewPort *vp,
                        std::vector<double> &ks) {
  ks.clear();
  double vmin = vp->lon_min, vmax = vp->lon_max;
  if (vmax < vmin) vmax += 360.0;  // view across the antimeridian
  const double base = WorldShift((lonMin + lonMax) / 2.0, vp->clon);
  for (int i = -1; i <= 1; i++) {
    const double k = base + 360.0 * i;
    if (lonMax - k >= vmin - 0.5 && lonMin - k <= vmax + 0.5) ks.push_back(k);
  }
}

static bool LatCulled(const S411Feature &f, PlugIn_ViewPort *vp) {
  return f.latMax < vp->lat_min - 0.5 || f.latMin > vp->lat_max + 0.5;
}

// projection with the latitude kept off the Mercator singularity (pole
// closures of circumpolar rings sit at +/-90)
static wxPoint Pix(PlugIn_ViewPort *vp, double lat, double lon) {
  wxPoint p;
  GetCanvasPixLL(vp, &p, std::max(-89.5, std::min(89.5, lat)), lon);
  return p;
}

static bool IsPole(const S411Pos &p) { return std::fabs(p.lat) >= 90.0 - 1e-9; }

// a ring as one polygon (for fills)
static void RingPoly(PlugIn_ViewPort *vp, const std::vector<S411Pos> &ring,
                     double k, std::vector<wxPoint> &out) {
  out.clear();
  out.reserve(ring.size());
  for (const auto &p : ring) out.push_back(Pix(vp, p.lat, p.lon - k));
}

// a ring or line as outline strips, split at the synthetic pole closure
static void Strips(PlugIn_ViewPort *vp, const std::vector<S411Pos> &ring,
                   double k, bool closed,
                   std::vector<std::vector<wxPoint>> &out) {
  out.clear();
  std::vector<wxPoint> s;
  const size_t n = ring.size() + (closed ? 1 : 0);
  for (size_t i = 0; i < n; i++) {
    const S411Pos &p = ring[i % ring.size()];
    if (IsPole(p)) {
      if (s.size() >= 2) out.push_back(s);
      s.clear();
      continue;
    }
    s.push_back(Pix(vp, p.lat, p.lon - k));
  }
  if (s.size() >= 2) out.push_back(s);
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

// screen pixels per millimetre (symbol sizes are given in mm, Annex C)
static double PxPerMm() {
  const double mm = PlugInGetDisplaySizeMM();
  const int w = wxGetDisplaySize().x;
  const double v = mm > 50 ? w / mm : 96.0 / 25.4;
  return std::max(2.5, std::min(12.0, v));
}

std::vector<size_t> s411_pi::DrawOrder() const {
  std::vector<size_t> idx;
  for (size_t i = 0; i < m_data.size(); i++)
    if (m_data[i].visible) idx.push_back(i);
  std::stable_sort(idx.begin(), idx.end(), [this](size_t a, size_t b) {
    return m_data[a].analysisTime < m_data[b].analysisTime;
  });
  return idx;
}

// ---------------------------------------------------------------------------
// painter: the few primitives the symbol / text / marker overlay needs, so
// it is written once for both wxDC and OpenGL
// ---------------------------------------------------------------------------
struct S411Painter {
  virtual ~S411Painter() {}
  virtual void Lines(const std::vector<wxRealPoint> &p, const wxColour &c,
                     double width) = 0;
  virtual void Fill(const std::vector<wxRealPoint> &p, const wxColour &c) = 0;
  virtual void Box(const wxRect &r, const wxColour &c) = 0;  // may be translucent
  virtual void Text(const wxString &t, int x, int y, const wxColour &c) = 0;
  virtual wxSize Extent(const wxString &t) = 0;
};

static wxFont OverlayFont() {
  return wxFont(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL);
}

class S411DCPainter : public S411Painter {
public:
  explicit S411DCPainter(wxDC &dc) : m_dc(dc) { m_dc.SetFont(OverlayFont()); }
  void Lines(const std::vector<wxRealPoint> &p, const wxColour &c,
             double width) override {
    std::vector<wxPoint> q;
    for (const auto &r : p) q.push_back(wxPoint(wxRound(r.x), wxRound(r.y)));
    m_dc.SetPen(wxPen(c, std::max(1, (int)std::lround(width))));
    if (q.size() >= 2) m_dc.DrawLines((int)q.size(), q.data());
  }
  void Fill(const std::vector<wxRealPoint> &p, const wxColour &c) override {
    std::vector<wxPoint> q;
    for (const auto &r : p) q.push_back(wxPoint(wxRound(r.x), wxRound(r.y)));
    m_dc.SetPen(wxPen(c, 1));
    m_dc.SetBrush(wxBrush(c));
    if (q.size() >= 3) m_dc.DrawPolygon((int)q.size(), q.data());
  }
  void Box(const wxRect &r, const wxColour &c) override {
    m_dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    m_dc.SetBrush(wxBrush(wxColour(c.Red(), c.Green(), c.Blue())));
    m_dc.DrawRectangle(r);
  }
  void Text(const wxString &t, int x, int y, const wxColour &c) override {
    m_dc.SetTextForeground(c);
    m_dc.DrawText(t, x, y);
  }
  wxSize Extent(const wxString &t) override { return m_dc.GetTextExtent(t); }

private:
  wxDC &m_dc;
};

// OpenGL text: rendered once into an RGBA image (alpha from coverage) and
// blitted with glDrawPixels; small cache keyed by text + colour
class S411GLPainter : public S411Painter {
public:
  void Lines(const std::vector<wxRealPoint> &p, const wxColour &c,
             double width) override {
    glColor4ub(c.Red(), c.Green(), c.Blue(), c.Alpha());
    glLineWidth((GLfloat)std::max(1.0, width));
    glBegin(GL_LINE_STRIP);
    for (const auto &r : p) glVertex2d(r.x, r.y);
    glEnd();
  }
  void Fill(const std::vector<wxRealPoint> &p, const wxColour &c) override {
    glColor4ub(c.Red(), c.Green(), c.Blue(), c.Alpha());
    glBegin(GL_TRIANGLE_FAN);
    for (const auto &r : p) glVertex2d(r.x, r.y);
    glEnd();
  }
  void Box(const wxRect &r, const wxColour &c) override {
    glColor4ub(c.Red(), c.Green(), c.Blue(), c.Alpha());
    glBegin(GL_QUADS);
    glVertex2i(r.x, r.y);
    glVertex2i(r.x + r.width, r.y);
    glVertex2i(r.x + r.width, r.y + r.height);
    glVertex2i(r.x, r.y + r.height);
    glEnd();
  }
  void Text(const wxString &t, int x, int y, const wxColour &c) override {
    const Img &im = Get(t, c);
    if (im.w <= 0) return;
    glRasterPos2i(x, y);
    glPixelZoom(1.0f, -1.0f);  // image rows top-down from the raster pos
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glDrawPixels(im.w, im.h, GL_RGBA, GL_UNSIGNED_BYTE, im.rgba.data());
    glPixelZoom(1.0f, 1.0f);
  }
  wxSize Extent(const wxString &t) override {
    const Img &im = Get(t, *wxBLACK);
    return wxSize(im.w, im.h);
  }

private:
  struct Img {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
  };
  static std::map<wxString, Img> &Cache() {
    static std::map<wxString, Img> c;
    return c;
  }
  const Img &Get(const wxString &t, const wxColour &c) {
    const wxString key = c.GetAsString(wxC2S_HTML_SYNTAX) + t;
    auto &cache = Cache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 400) cache.clear();
    Img &im = cache[key];
    wxMemoryDC mdc;
    mdc.SetFont(OverlayFont());
    const wxSize sz = mdc.GetTextExtent(t);
    if (sz.x <= 0 || sz.y <= 0) return im;
    wxBitmap bmp(sz.x, sz.y, 24);
    mdc.SelectObject(bmp);
    mdc.SetBackground(*wxWHITE_BRUSH);
    mdc.Clear();
    mdc.SetTextForeground(*wxBLACK);
    mdc.DrawText(t, 0, 0);
    mdc.SelectObject(wxNullBitmap);
    const wxImage img = bmp.ConvertToImage();
    im.w = sz.x;
    im.h = sz.y;
    im.rgba.resize((size_t)im.w * im.h * 4);
    const unsigned char *src = img.GetData();
    for (int i = 0; i < im.w * im.h; i++) {
      im.rgba[i * 4 + 0] = c.Red();
      im.rgba[i * 4 + 1] = c.Green();
      im.rgba[i * 4 + 2] = c.Blue();
      im.rgba[i * 4 + 3] = (unsigned char)(255 - src[i * 3]);  // coverage
    }
    return im;
  }
};

// one Annex C symbol at screen position c, rotated clockwise by rot
static void DrawSymbol(S411Painter &p, const S411Symbol *sym, wxPoint c,
                       double ppmm, double rot, S411Palette pal) {
  if (!sym) return;
  const wxColour col = LineColour(sym->token, pal);
  const double cs = std::cos(rot), sn = std::sin(rot);
  std::vector<wxRealPoint> pts;
  for (int i = 0; i < sym->count; i++) {
    const S411SymPrim &pr = sym->prims[i];
    pts.clear();
    for (int k = 0; k < pr.n; k++) {
      const double x = pr.xy[2 * k] * ppmm, y = pr.xy[2 * k + 1] * ppmm;
      pts.push_back(wxRealPoint(c.x + x * cs - y * sn, c.y + x * sn + y * cs));
    }
    if (pr.filled)
      p.Fill(pts, col);
    else
      p.Lines(pts, col, pr.width * ppmm);
  }
}

// ICEBRGA1: iceberg symbols on a lattice v1 = (16, 0) mm, v2 = (8, 16) mm,
// anchored to the area (GlobalGeometry), drawn where the lattice point lies
// inside the area
static void DrawBergPattern(S411Painter &p, PlugIn_ViewPort *vp,
                            const S411Feature &f, double k, double ppmm,
                            S411Palette pal) {
  const S411Symbol *sym = S411FindSymbol("ICEBRG99");
  const wxPoint o = Pix(vp, f.latMax, f.lonMin - k);
  int x0 = INT_MAX, x1 = INT_MIN, y0 = INT_MAX, y1 = INT_MIN;
  for (double la : {f.latMin, f.latMax})
    for (double lo : {f.lonMin, f.lonMax, (f.lonMin + f.lonMax) / 2}) {
      const wxPoint q = Pix(vp, la, lo - k);
      x0 = std::min(x0, q.x); x1 = std::max(x1, q.x);
      y0 = std::min(y0, q.y); y1 = std::max(y1, q.y);
    }
  x0 = std::max(x0, 0); y0 = std::max(y0, 0);
  x1 = std::min(x1, vp->pix_width); y1 = std::min(y1, vp->pix_height);
  if (x0 >= x1 || y0 >= y1) return;
  const double s = 16.0 * ppmm;
  int drawn = 0;
  for (int j = (int)std::floor((y0 - o.y) / s); j * s + o.y <= y1 + s; j++) {
    const double rowX = o.x + j * 8.0 * ppmm;
    for (int i = (int)std::floor((x0 - rowX) / s); i * s + rowX <= x1 + s;
         i++) {
      const wxPoint q(wxRound(rowX + i * s), wxRound(o.y + j * s));
      if (q.x < x0 || q.x > x1 || q.y < y0 || q.y > y1) continue;
      double lat, lon;
      GetCanvasLLPix(vp, q, &lat, &lon);
      if (!S411AreaContains(f, lat, lon)) continue;
      DrawSymbol(p, sym, q, ppmm, 0, pal);
      if (++drawn > 800) return;  // guard against absurd zoom levels
    }
  }
}

void s411_pi::RenderOverlays(S411Painter &p, PlugIn_ViewPort *vp) {
  const double ppmm = PxPerMm();
  std::vector<double> ks;
  const wxColour lin = LineColour("ICELIN", m_palette);

  // point symbols (Annex C), PS 9.7 value text, iceberg area pattern
  for (size_t di : DrawOrder()) {
    for (const auto &f : m_data[di].features) {
      if (f.superseded || LatCulled(f, vp)) continue;
      const bool berg = f.kind == S411Kind::Area && f.type == "IcebergArea";
      if (f.kind != S411Kind::Point && !berg) continue;
      WorldShifts(f.lonMin, f.lonMax, vp, ks);
      for (double k : ks) {
        if (berg) {
          DrawBergPattern(p, vp, f, k, ppmm, m_palette);
          continue;
        }
        const S411Pos &pos = f.rings[0][0];
        const wxPoint c = Pix(vp, pos.lat, pos.lon - k);
        if (c.x < -60 || c.y < -60 || c.x > vp->pix_width + 60 ||
            c.y > vp->pix_height + 60)
          continue;
        double rot = 0;
        if (f.symbol.StartsWith("ICEDFT")) {  // true direction: follow north
          const wxPoint n = Pix(vp, pos.lat + 0.01, pos.lon - k);
          rot = std::atan2((double)(n.x - c.x), (double)(c.y - n.y));
        }
        const S411Symbol *sym = S411FindSymbol(f.symbol.mb_str());
        if (sym) {
          DrawSymbol(p, sym, c, ppmm, rot, m_palette);
        } else {  // point type without an Annex C symbol: small marker
          p.Fill({wxRealPoint(c.x, c.y - 6), wxRealPoint(c.x - 5, c.y + 4),
                  wxRealPoint(c.x + 5, c.y + 4)},
                 lin);
        }
        if (m_showValues && !f.valueText.IsEmpty()) {
          const wxSize e = p.Extent(f.valueText);
          p.Text(f.valueText, c.x + wxRound(4 * ppmm), c.y - e.y / 2, lin);
        }
      }
    }
  }

  // markers of the last route ice check (PS 9.6)
  for (const auto &a : m_routeAlerts) {
    const wxColour col = a.alert == S411Alert::IceHazard ? wxColour(220, 0, 0)
                       : a.alert == S411Alert::IceDifficult
                           ? wxColour(255, 140, 0)
                           : wxColour(200, 0, 200);
    const wxPoint c = Pix(vp, a.where.lat,
                          a.where.lon - WorldShift(a.where.lon, vp->clon));
    std::vector<wxRealPoint> ring;
    const double r = 3.5 * ppmm;
    for (int i = 0; i <= 24; i++)
      ring.push_back(wxRealPoint(c.x + r * std::cos(i * 3.14159265358979323846 / 12),
                                 c.y + r * std::sin(i * 3.14159265358979323846 / 12)));
    p.Lines(ring, col, 3);
    p.Text(wxString::Format("leg %zu", a.leg + 1), c.x + wxRound(r) + 3,
           c.y - wxRound(r), col);
  }

  // age of the displayed ice information (PS 8.1: always visible)
  if (!m_showAgeBox) return;
  const time_t now = time(nullptr);
  const bool dark = m_palette != S411Palette::Day;
  const wxColour fg = dark ? wxColour(230, 230, 230) : wxColour(20, 20, 20);
  const wxColour warn = dark ? wxColour(255, 110, 110) : wxColour(200, 0, 0);
  std::vector<std::pair<wxString, wxColour>> lines;
  std::vector<size_t> order = DrawOrder();
  std::reverse(order.begin(), order.end());  // most recent first
  for (size_t n = 0; n < order.size(); n++) {
    if (n == 4) {
      lines.push_back({wxString::Format("... and %zu more dataset(s)",
                                        order.size() - 4),
                       fg});
      break;
    }
    const S411Dataset &ds = m_data[order[n]];
    wxString l = (ds.producer.IsEmpty() ? ds.title : ds.producer) + ": " +
                 S411AgeText(ds, now);
    const S411Age age = S411DatasetAge(ds, now, m_ageLimitH);
    bool bad = true;
    if (age == S411Age::Outdated) l += "  - ICE INFORMATION OUTDATED";
    else if (age == S411Age::Expired) l += "  - older than 30 days";
    else if (age == S411Age::OverLimit)
      l += wxString::Format("  - older than the %g h age limit", m_ageLimitH);
    else bad = age == S411Age::Unknown;
    lines.push_back({l, bad ? warn : fg});
  }
  const wxString ov = S411OverlappingProducers(m_data);
  if (!ov.IsEmpty())
    lines.push_back({"Ice data of several producers overlap: " + ov, warn});
  if (lines.empty()) return;
  int w = 0, h = 0;
  for (const auto &l : lines) {
    const wxSize e = p.Extent(l.first);
    w = std::max(w, e.x);
    h = std::max(h, e.y);
  }
  const int pad = 5, x = 8, y = 8;
  p.Box(wxRect(x, y, w + 2 * pad, (int)lines.size() * (h + 2) + 2 * pad - 2),
        dark ? wxColour(20, 20, 30, 190) : wxColour(255, 255, 255, 200));
  for (size_t i = 0; i < lines.size(); i++)
    p.Text(lines[i].first, x + pad, y + pad + (int)i * (h + 2),
           lines[i].second);
}

// ---------------------------------------------------------------------------
// wxDC rendering (non-OpenGL mode); wxGraphicsContext for alpha fills
// ---------------------------------------------------------------------------
bool s411_pi::RenderDC(wxDC &dc, PlugIn_ViewPort *vp) {
  if (!m_show || m_data.empty()) return false;
  std::vector<wxPoint> pix;
  std::vector<std::vector<wxPoint>> strips;
  std::vector<double> ks;

#if wxUSE_GRAPHICS_CONTEXT
  wxGraphicsContext *gc = nullptr;
  wxMemoryDC *mdc = wxDynamicCast(&dc, wxMemoryDC);
  if (mdc) gc = wxGraphicsContext::Create(*mdc);
#endif

  for (size_t di : DrawOrder()) {
    const S411Dataset &ds = m_data[di];
    for (int pass = 0; pass < 2; pass++) {  // areas, then lines
      for (const auto &f : ds.features) {
        if ((pass == 0) != (f.kind == S411Kind::Area)) continue;
        if (f.kind == S411Kind::Point || f.superseded || LatCulled(f, vp))
          continue;
        WorldShifts(f.lonMin, f.lonMax, vp, ks);
        for (double k : ks) {
          if (f.kind == S411Kind::Line) {
            Strips(vp, f.rings[0], k, false, strips);
            S411LineStyle ls;
            S411LineStyleFor(f.type, ls);
            dc.SetPen(wxPen(LineColour(ls.token, m_palette),
                            wxMax(1, (int)std::lround(ls.width)),
                            DashToPen(ls.dash)));
            for (const auto &s : strips) dc.DrawLines((int)s.size(), s.data());
            continue;
          }
          const bool berg = f.type == "IcebergArea";  // pattern, no fill
          wxColour fill = s411_pi_AreaColour(f, m_mode, m_iceClass, m_palette);
#if wxUSE_GRAPHICS_CONTEXT
          if (gc) {
            if (!berg) {
              wxGraphicsPath path = gc->CreatePath();
              for (const auto &ring : f.rings) {
                RingPoly(vp, ring, k, pix);
                if (pix.size() < 3) continue;
                path.MoveToPoint(pix[0].x, pix[0].y);
                for (size_t i = 1; i < pix.size(); i++)
                  path.AddLineToPoint(pix[i].x, pix[i].y);
                path.CloseSubpath();
              }
              gc->SetBrush(wxBrush(
                  wxColour(fill.Red(), fill.Green(), fill.Blue(), m_alpha)));
              gc->FillPath(path, wxODDEVEN_RULE);
            }
            wxGraphicsPath outline = gc->CreatePath();
            for (const auto &ring : f.rings) {
              Strips(vp, ring, k, true, strips);
              for (const auto &s : strips) {
                outline.MoveToPoint(s[0].x, s[0].y);
                for (size_t i = 1; i < s.size(); i++)
                  outline.AddLineToPoint(s[i].x, s[i].y);
              }
            }
            gc->SetPen(berg ? wxPen(LineColour("ICEBRG", m_palette), 1)
                            : wxPen(wxColour(0, 0, 0, 120), 1));
            gc->StrokePath(outline);
            continue;
          }
#endif
          // plain-DC fallback (print/PDF): all rings with even-odd so holes
          // and secondary exteriors match the GC/GL paths
          std::vector<wxPoint> allPts;
          std::vector<int> counts;
          for (const auto &ring : f.rings) {
            RingPoly(vp, ring, k, pix);
            if (pix.size() < 3) continue;
            counts.push_back((int)pix.size());
            allPts.insert(allPts.end(), pix.begin(), pix.end());
          }
          if (!berg && !counts.empty()) {
            dc.SetBrush(wxBrush(fill));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawPolyPolygon((int)counts.size(), counts.data(),
                               allPts.data(), 0, 0, wxODDEVEN_RULE);
          }
          dc.SetPen(wxPen(*wxBLACK, 1));
          for (const auto &ring : f.rings) {
            Strips(vp, ring, k, true, strips);
            for (const auto &s : strips) dc.DrawLines((int)s.size(), s.data());
          }
        }
      }
    }
  }
#if wxUSE_GRAPHICS_CONTEXT
  delete gc;
#endif
  S411DCPainter painter(dc);
  RenderOverlays(painter, vp);
  return true;
}

// ---------------------------------------------------------------------------
// OpenGL rendering: areas were triangulated at load time (S411Tess)
// ---------------------------------------------------------------------------
// In Mercator the projection is affine in (lon, Mercator northing), so the
// pre-built float Mercator copies are drawn straight from vertex arrays with
// one matrix per feature - no per-vertex projection call per frame. The
// matrix is measured from OpenCPN's own projection each frame and checked
// against it at two more points; any mismatch, another projection, or a
// zoom where float precision would show falls back to the exact path.
struct S411Affine {
  double a, b, c, d, tx, ty;  // px = a*x + b*y + tx, py = c*x + d*y + ty
};

static bool MercatorAffine(PlugIn_ViewPort *vp, S411Affine &m) {
  if (vp->m_projection_type != PI_PROJECTION_MERCATOR) return false;
  const double lat0 = std::max(-80.0, std::min(80.0, vp->clat));
  const double lon0 = vp->clon;
  const double dLon = std::max(1e-4, std::fabs(vp->lon_max - vp->lon_min) / 4);
  double dLat = std::max(1e-4, std::fabs(vp->lat_max - vp->lat_min) / 4);
  const double lat1 = lat0 + (lat0 > 0 ? -dLat : dLat);
  wxPoint2DDouble p0, p1, p2;
  GetDoubleCanvasPixLL(vp, &p0, lat0, lon0);
  GetDoubleCanvasPixLL(vp, &p1, lat0, lon0 + dLon);
  GetDoubleCanvasPixLL(vp, &p2, lat1, lon0);
  const double y0 = S411MercY(lat0), y1 = S411MercY(lat1);
  if (std::fabs(y1 - y0) < 1e-9) return false;
  m.a = (p1.m_x - p0.m_x) / dLon;
  m.c = (p1.m_y - p0.m_y) / dLon;
  m.b = (p2.m_x - p0.m_x) / (y1 - y0);
  m.d = (p2.m_y - p0.m_y) / (y1 - y0);
  m.tx = p0.m_x - m.a * lon0 - m.b * y0;
  m.ty = p0.m_y - m.c * lon0 - m.d * y0;
  // float vertex pipeline: keep the rounding error well below a pixel
  if (std::hypot(m.a, m.c) > 2.0e4) return false;
  for (double f : {0.1, 0.9}) {
    const double la = vp->lat_min + f * (vp->lat_max - vp->lat_min);
    const double lo = vp->lon_min + (1 - f) * (vp->lon_max - vp->lon_min);
    wxPoint2DDouble q;
    GetDoubleCanvasPixLL(vp, &q, la, lo);
    const double y = S411MercY(la);
    const double ex = m.a * lo + m.b * y + m.tx - q.m_x;
    const double ey = m.c * lo + m.d * y + m.ty - q.m_y;
    if (std::hypot(ex, ey) > 0.75) return false;
  }
  return true;
}

// modelview for one feature: its local float coordinates -> canvas pixels
static void LoadFeatureMatrix(const S411Affine &m, const S411Feature &f,
                              double k) {
  const double ox = f.mx0 - k, oy = f.my0;
  const GLdouble mat[16] = {m.a, m.c, 0, 0, m.b, m.d, 0, 0, 0, 0, 1, 0,
                            m.a * ox + m.b * oy + m.tx,
                            m.c * ox + m.d * oy + m.ty, 0, 1};
  glPushMatrix();
  glMultMatrixd(mat);
}

bool s411_pi::RenderGL(PlugIn_ViewPort *vp) {
  if (!m_show || m_data.empty()) return false;
  std::vector<std::vector<wxPoint>> strips;
  std::vector<double> ks;

  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_ENABLE_BIT |
               GL_POLYGON_BIT | GL_PIXEL_MODE_BIT | GL_CURRENT_BIT);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_TEXTURE_2D);

  S411Affine aff;
  const bool fast = MercatorAffine(vp, aff);
  GLint matrixMode = GL_MODELVIEW;
  glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
  glMatrixMode(GL_MODELVIEW);
  if (fast) glEnableClientState(GL_VERTEX_ARRAY);

  for (size_t di : DrawOrder()) {
    const S411Dataset &ds = m_data[di];
    for (const auto &f : ds.features) {
      if (f.kind != S411Kind::Area || f.superseded || LatCulled(f, vp))
        continue;
      const bool berg = f.type == "IcebergArea";  // pattern, no fill
      const wxColour fill =
          s411_pi_AreaColour(f, m_mode, m_iceClass, m_palette);
      const wxColour edge = berg ? LineColour("ICEBRG", m_palette)
                                 : wxColour(0, 0, 0, 120);
      WorldShifts(f.lonMin, f.lonMax, vp, ks);
      for (double k : ks) {
        glLineWidth(1.0f);
        if (fast) {
          LoadFeatureMatrix(aff, f, k);
          if (!berg && !f.mTris.empty()) {
            glColor4ub(fill.Red(), fill.Green(), fill.Blue(), (GLubyte)m_alpha);
            glVertexPointer(2, GL_FLOAT, 0, f.mTris.data());
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(f.mTris.size() / 2));
          }
          glColor4ub(edge.Red(), edge.Green(), edge.Blue(), edge.Alpha());
          for (const auto &s : f.mStrips) {
            glVertexPointer(2, GL_FLOAT, 0, s.data());
            glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)(s.size() / 2));
          }
          glPopMatrix();
          continue;
        }
        if (!berg) {
          glColor4ub(fill.Red(), fill.Green(), fill.Blue(), (GLubyte)m_alpha);
          glBegin(GL_TRIANGLES);
          for (const auto &t : f.tris) {
            const wxPoint p = Pix(vp, t.lat, t.lon - k);
            glVertex2i(p.x, p.y);
          }
          glEnd();
        }
        glColor4ub(edge.Red(), edge.Green(), edge.Blue(), edge.Alpha());
        for (const auto &ring : f.rings) {
          Strips(vp, ring, k, true, strips);
          for (const auto &s : strips) {
            glBegin(GL_LINE_STRIP);
            for (const auto &p : s) glVertex2i(p.x, p.y);
            glEnd();
          }
        }
      }
    }
    for (const auto &f : ds.features) {
      if (f.kind != S411Kind::Line || f.superseded || LatCulled(f, vp))
        continue;
      S411LineStyle ls;
      S411LineStyleFor(f.type, ls);
      const wxColour lc = LineColour(ls.token, m_palette);
      glColor4ub(lc.Red(), lc.Green(), lc.Blue(), 255);
      glLineWidth((GLfloat)ls.width);
      if (ls.stipple) {
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(2, ls.stipple);
      } else {
        glDisable(GL_LINE_STIPPLE);
      }
      WorldShifts(f.lonMin, f.lonMax, vp, ks);
      for (double k : ks) {
        if (fast) {
          LoadFeatureMatrix(aff, f, k);
          for (const auto &s : f.mStrips) {
            glVertexPointer(2, GL_FLOAT, 0, s.data());
            glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)(s.size() / 2));
          }
          glPopMatrix();
          continue;
        }
        Strips(vp, f.rings[0], k, false, strips);
        for (const auto &s : strips) {
          glBegin(GL_LINE_STRIP);
          for (const auto &p : s) glVertex2i(p.x, p.y);
          glEnd();
        }
      }
    }
    glDisable(GL_LINE_STIPPLE);
  }
  if (fast) glDisableClientState(GL_VERTEX_ARRAY);
  glMatrixMode(matrixMode);

  S411GLPainter painter;
  RenderOverlays(painter, vp);
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
