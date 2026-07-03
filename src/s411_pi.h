/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 *
 * Loads IHO S-100 based S-411 "Ice Information" Edition 2.0.0 GML datasets
 * (ice charts) and renders them as a chart overlay with the WMO ice chart
 * colour code.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <wx/wx.h>

#include <vector>

#include "ocpn_plugin.h"
#include "S411Data.h"

class s411_pi : public opencpn_plugin_116 {
public:
  explicit s411_pi(void *ppimgr);

  // plugin identification
  int Init(void) override;
  bool DeInit(void) override;
  int GetAPIVersionMajor() override { return 1; }
  int GetAPIVersionMinor() override { return 16; }
  int GetPlugInVersionMajor() override { return 0; }
  int GetPlugInVersionMinor() override { return 2; }
  wxBitmap *GetPlugInBitmap() override;
  wxString GetCommonName() override;
  wxString GetShortDescription() override;
  wxString GetLongDescription() override;

  // toolbar
  int GetToolbarToolCount(void) override { return 1; }
  void OnToolbarToolCallback(int id) override;

  // overlay rendering (single and multi canvas, DC and OpenGL)
  bool RenderOverlay(wxDC &dc, PlugIn_ViewPort *vp) override;
  bool RenderGLOverlay(wxGLContext *pcontext, PlugIn_ViewPort *vp) override;
  bool RenderOverlayMultiCanvas(wxDC &dc, PlugIn_ViewPort *vp,
                                int canvasIndex) override;
  bool RenderGLOverlayMultiCanvas(wxGLContext *pcontext, PlugIn_ViewPort *vp,
                                  int canvasIndex) override;

private:
  void MakeIcon();
  void LoadDialog();
  bool RenderDC(wxDC &dc, PlugIn_ViewPort *vp);
  bool RenderGL(PlugIn_ViewPort *vp);

  wxBitmap m_icon;
  int m_toolId = -1;
  bool m_show = false;
  wxString m_lastDir;
  std::vector<S411Dataset> m_data;
};
