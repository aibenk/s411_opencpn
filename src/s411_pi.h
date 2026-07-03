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
  int GetPlugInVersionMinor() override { return 3; }
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

  // called back by the control dialog
  void LoadFiles();
  void ClearData();
  void SetShow(bool on);
  void Refresh();
  size_t DatasetCount() const { return m_data.size(); }
  size_t FeatureCount() const;

  // portrayal state (read/written by the control dialog)
  bool m_show = false;
  S411Mode m_mode = S411Mode::Concentration;
  wxString m_iceClass;                 // "", IC, IB, IA, IASuper
  S411Palette m_palette = S411Palette::Day;

private:
  void MakeIcon();
  void ShowControl();
  bool RenderDC(wxDC &dc, PlugIn_ViewPort *vp);
  bool RenderGL(PlugIn_ViewPort *vp);
  void LoadConfig();
  void SaveConfig();

  wxBitmap m_icon;
  int m_toolId = -1;
  wxString m_lastDir;
  std::vector<S411Dataset> m_data;
  S411CtrlDialog *m_dialog = nullptr;
};
