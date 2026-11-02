/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 *
 * Loads IHO S-100 based S-411 "Ice Information" datasets (Edition 2.0.0 and
 * 1.x GML, exchange sets, zips) and renders them as a chart overlay with the
 * WMO ice chart colour code. Display mode (concentration / stage of
 * development / ice-class traffic light), ice class and day/dusk/night
 * palette are switchable, matching the S-411 web viewer.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <wx/thread.h>
#include <wx/wx.h>

#include <atomic>
#include <functional>
#include <map>
#include <vector>

#include "ocpn_plugin.h"
#include "S411Data.h"
#include "S411Portrayal.h"
#include "S411Router.h"

class S411CtrlDialog;
class S411LoadTimer;
class S411Worker;
class S411EggPanel;
struct S411Painter;

class s411_pi : public opencpn_plugin_116 {
public:
  explicit s411_pi(void *ppimgr);

  // plugin identification
  int Init(void) override;
  bool DeInit(void) override;
  int GetAPIVersionMajor() override { return 1; }
  int GetAPIVersionMinor() override { return 16; }
  int GetPlugInVersionMajor() override { return 0; }
  int GetPlugInVersionMinor() override { return 8; }
  wxBitmap *GetPlugInBitmap() override;
  wxString GetCommonName() override;
  wxString GetShortDescription() override;
  wxString GetLongDescription() override;

  // toolbar + preferences
  int GetToolbarToolCount(void) override { return 1; }
  void OnToolbarToolCallback(int id) override;
  void ShowPreferencesDialog(wxWindow *parent) override;

  // overlay rendering (single and multi canvas, DC and OpenGL)
  bool RenderOverlay(wxDC &dc, PlugIn_ViewPort *vp) override;
  bool RenderGLOverlay(wxGLContext *pcontext, PlugIn_ViewPort *vp) override;
  bool RenderOverlayMultiCanvas(wxDC &dc, PlugIn_ViewPort *vp,
                                int canvasIndex) override;
  bool RenderGLOverlayMultiCanvas(wxGLContext *pcontext, PlugIn_ViewPort *vp,
                                  int canvasIndex) override;

  // feature picking: plain click (no drag) -> attribute report
  bool MouseEventHook(wxMouseEvent &event) override;

  // inter-plugin messaging (ocpn_draw_pi handshake + boundary responses)
  void SetPluginMessage(wxString &message_id, wxString &message_body) override;

  // called back by the control dialog
  /** File dialog, then load the chosen files on a worker thread. */
  void LoadFiles();
  /** Load files on a worker thread (no dialog); results arrive in PollLoad. */
  void LoadPaths(const wxArrayString &paths);
  bool IsLoading() const { return m_loading; }
  void CancelLoad() { m_cancel = true; }
  wxString LoadProgress();
  /** Called by a timer while loading: merges finished results. */
  void PollLoad();
  /** Download the newest chart of every ice service and load them (4.2). */
  void DownloadCurrentCharts();
  /** Re-apply dataset precedence after loading/visibility changes. */
  void DataChanged();
  /** One line per dataset for the list: producer, time, age, flags. */
  wxString DatasetLabel(const S411Dataset &ds) const;
  /** Warnings for the panel: overlapping producers, outdated data. */
  wxString Warnings() const;
  void ClearData();
  void SetShow(bool on);
  void Refresh();
  size_t DatasetCount() const { return m_data.size(); }
  size_t FeatureCount() const;
  std::vector<S411Dataset> &Data() { return m_data; }
  /** Jump the chart to dataset `idx`, or to the union of all visible
   *  datasets when idx < 0. */
  void ZoomToData(int idx);

  // --- routing (called back by the routing panel) ------------------------
  /** Arm map-click capture for start (which==0) or end (which==1). */
  void BeginPickPoint(int which);
  /** Disarm map-click capture (called when the panel is hidden/closed) so an
   *  ordinary chart click is never silently stolen to set a route point. */
  void CancelPick() { m_pickMode = 0; }
  bool HaveStart() const { return m_haveStart; }
  bool HaveEnd() const { return m_haveEnd; }
  wxString StartLabel() const;
  wxString EndLabel() const;
  /** Compute the ice route and add it to OpenCPN. Returns a status string;
   *  ok is set true on success. */
  wxString ComputeRoute(bool &ok);
  /** (Re)export ice hazard zones as ocpn_draw boundaries. Returns a status
   *  string. */
  wxString ExportHazards();
  /** Delete every boundary this plugin created. */
  void ClearHazards();
  /** Check OpenCPN's active (else selected, else last ice) route against
   *  the ice alerts of PS clause 9.6; marks the hits on the chart and
   *  returns a report. */
  wxString CheckActiveRoute();
  void ClearRouteAlerts() { m_routeAlerts.clear(); }
  bool OcpnDrawAvailable() const { return m_odAvailable; }
  S411RouteParams &RouteParams() { return m_route; }

  // guided tour: shown once automatically, re-invokable from the panel's "?"
  bool m_tutorialSeen = false;
  void MarkTutorialSeen();

  // portrayal state (read/written by the control dialog)
  bool m_show = false;
  S411Mode m_mode = S411Mode::Concentration;
  wxString m_iceClass;                 // "", IC, IB, IA, IASuper
  S411Palette m_palette = S411Palette::Day;
  int m_alpha = 170;                   // area fill opacity, 26..255
  double m_ageLimitH = 72;             // PS 8.1 mariner age limit (hours)
  bool m_showValues = true;            // PS 9.7 value texts (cm, kn)
  bool m_showAgeBox = true;            // data age box on the chart

private:
  void MakeIcon();
  void ShowControl();
  bool RenderDC(wxDC &dc, PlugIn_ViewPort *vp);
  bool RenderGL(PlugIn_ViewPort *vp);
  // shared by both back ends: symbols, value text, iceberg pattern, data
  // age box and route alert markers
  void RenderOverlays(S411Painter &p, PlugIn_ViewPort *vp);
  /** Indices of the visible datasets, oldest analysis first (the most
   *  recent analysis is drawn on top, PS clause 4.6). */
  std::vector<size_t> DrawOrder() const;
  void FinishLoad();
  void JoinLoader();
  void LoadConfig();
  void SaveConfig();
  void ShowPickReport(const std::vector<S411Hit> &hits, double lat,
                      double lon);
  void JumpToBox(double latMin, double latMax, double lonMin, double lonMax);
  // ocpn_draw messaging
  wxString SendOcpnDraw(const wxString &json);   // returns the reply body
  void DeleteOurBoundaries();

  wxBitmap m_icon;
  int m_toolId = -1;
  wxString m_lastDir;
  std::vector<S411Dataset> m_data;
  S411CtrlDialog *m_dialog = nullptr;
  wxDialog *m_report = nullptr;        // non-modal pick report
  wxTextCtrl *m_reportText = nullptr;
  S411EggPanel *m_reportEgg = nullptr;  // WMO egg code of the picked areas
  PlugIn_ViewPort m_lastVp;            // viewport of the last render pass
  bool m_haveVp = false;
  wxPoint m_downPos;                   // click-vs-drag discrimination
  bool m_downValid = false;

  // routing state
  S411RouteParams m_route;
  S411Pos m_start{0, 0}, m_end{0, 0};
  bool m_haveStart = false, m_haveEnd = false;
  int m_pickMode = 0;                  // 0 none, 1 start, 2 end
  wxString m_lastRouteGuid;
  bool m_odAvailable = false;
  wxString m_odReply;                  // reply captured during SendOcpnDraw
  bool m_capturingReply = false;
  std::vector<S411RouteAlert> m_routeAlerts;  // markers of the last check

  // background loading. wxThread/wxMutex, NOT std::thread/std::mutex: the
  // plugin runs on OpenCPN's bundled msvcp140.dll (14.12, VS 2017), in which
  // a std::mutex compiled with VS 2022 crashes (constexpr mutex change)
  S411Worker *m_loader = nullptr;      // joinable; deleted after Wait()
  std::atomic<bool> m_loading{false}, m_loadDone{false}, m_cancel{false};
  wxMutex m_loadMx;                    // guards the three fields below
  std::vector<S411Dataset> m_loadOut;
  wxString m_loadErrors, m_loadMsg;
  S411LoadTimer *m_loadTimer = nullptr;
};
