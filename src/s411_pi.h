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

#include <wx/wx.h>

#include <vector>

#include "ocpn_plugin.h"
#include "S411Data.h"
#include "S411Portrayal.h"
#include "S411Router.h"

class S411CtrlDialog;

class s411_pi : public opencpn_plugin_116 {
public:
  explicit s411_pi(void *ppimgr);

  // plugin identification
  int Init(void) override;
  bool DeInit(void) override;
  int GetAPIVersionMajor() override { return 1; }
  int GetAPIVersionMinor() override { return 16; }
  int GetPlugInVersionMajor() override { return 0; }
  int GetPlugInVersionMinor() override { return 5; }
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
  void LoadFiles();
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
  bool OcpnDrawAvailable() const { return m_odAvailable; }
  S411RouteParams &RouteParams() { return m_route; }

  // portrayal state (read/written by the control dialog)
  bool m_show = false;
  S411Mode m_mode = S411Mode::Concentration;
  wxString m_iceClass;                 // "", IC, IB, IA, IASuper
  S411Palette m_palette = S411Palette::Day;
  int m_alpha = 170;                   // area fill opacity, 26..255

private:
  void MakeIcon();
  void ShowControl();
  bool RenderDC(wxDC &dc, PlugIn_ViewPort *vp);
  bool RenderGL(PlugIn_ViewPort *vp);
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
};
