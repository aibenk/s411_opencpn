/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * ocpn_draw_pi boundary JSON builders + response scanners.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "S411Boundary.h"

wxString S411JsonEscape(const wxString &s) {
  wxString o;
  o.reserve(s.length() + 4);
  for (wxUniChar c : s) {
    switch ((int)c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((int)c < 0x20)
          o += wxString::Format("\\u%04x", (int)c);
        else
          o += c;
    }
  }
  return o;
}

static wxString Num(double v) {
  // fixed 6 decimals, trimmed - avoids locale/exponent surprises in the JSON
  wxString s = wxString::FromCDouble(v, 6);
  return s;
}

wxString S411BuildCreateBoundary(const S411HazardZone &zone,
                                 const wxString &msgId) {
  const bool blocked = zone.severity == S411CellState::Blocked;
  const char *colour = blocked ? "#C80000" : "#FFB400";  // red / amber
  const char *sev = blocked ? "hazardous" : "caution";

  wxString name = wxString(S411_OD_TAG) + sev;
  if (!zone.source.IsEmpty()) name += " (" + zone.source + ")";

  // drop a trailing vertex that duplicates the first: ocpn_draw closes the
  // ring itself, and a duplicated closing point is a contract violation
  size_t n = zone.ring.size();
  if (n >= 2 && zone.ring.front().lat == zone.ring.back().lat &&
      zone.ring.front().lon == zone.ring.back().lon)
    n--;
  wxString pts;
  for (size_t i = 0; i < n; i++) {
    if (i) pts += ",";
    pts += "{\"Lat\":" + Num(zone.ring[i].lat) +
           ",\"Lon\":" + Num(zone.ring[i].lon) +
           ",\"BoundaryPointType\":\"Exclusion\"}";
  }

  wxString j = "{";
  j += "\"Source\":\"" S411_OD_SOURCE "\",";
  j += "\"Type\":\"Request\",";
  j += "\"Msg\":\"CreateBoundary\",";
  j += "\"MsgId\":\"" + S411JsonEscape(msgId) + "\",";
  j += "\"Boundary\":{";
  j += "\"BoundaryName\":\"" + S411JsonEscape(name) + "\",";
  j += "\"BoundaryType\":\"Exclusion\",";
  j += "\"visible\":true,";
  j += "\"lineColour\":\"" + wxString(colour) + "\",";
  j += "\"fillColour\":\"" + wxString(colour) + "\",";
  j += "\"BoundaryPoints\":[" + pts + "]";
  j += "}}";
  return j;
}

wxString S411BuildDeleteBoundary(const wxString &guid) {
  return "{\"Source\":\"" S411_OD_SOURCE "\",\"Type\":\"Request\","
         "\"Msg\":\"DeleteBoundary\",\"MsgId\":\"del\",\"GUID\":\"" +
         S411JsonEscape(guid) + "\"}";
}

wxString S411BuildVersionRequest() {
  return "{\"Source\":\"" S411_OD_SOURCE "\",\"Type\":\"Request\","
         "\"Msg\":\"Version\",\"MsgId\":\"version\"}";
}

wxString S411BuildBoundaryInfoRequest() {
  return "{\"Source\":\"" S411_OD_SOURCE "\",\"Type\":\"Request\","
         "\"Msg\":\"BoundaryInformation\",\"MsgId\":\"info\"}";
}

// --- minimal response scanning (fixed-format ocpn_draw replies) ------------
// find the exact key token "<key>" (with the closing quote) so that "GUID"
// never matches inside "BoundaryGUID" etc.
static int FindKey(const wxString &json, const wxString &key, int from) {
  const wxString needle = "\"" + key + "\"";
  return json.find(needle, from);
}

wxString S411JsonField(const wxString &json, const wxString &key) {
  int p = FindKey(json, key, 0);
  if (p == wxNOT_FOUND) return "";
  p += (int)key.length() + 2;  // past the closing quote of the key
  // skip whitespace and the colon
  while (p < (int)json.length() &&
         (json[p] == ' ' || json[p] == ':' || json[p] == '\t' ||
          json[p] == '\n' || json[p] == '\r'))
    p++;
  if (p >= (int)json.length()) return "";
  if (json[p] == '"') {
    p++;
    wxString out;
    while (p < (int)json.length() && json[p] != '"') {
      if (json[p] == '\\' && p + 1 < (int)json.length()) p++;
      out += json[p++];
    }
    return out;
  }
  // bare token (number / bool)
  wxString out;
  while (p < (int)json.length() && json[p] != ',' && json[p] != '}' &&
         json[p] != ']' && json[p] != ' ' && json[p] != '\n' &&
         json[p] != '\r' && json[p] != '\t')
    out += json[p++];
  return out;
}

bool S411JsonCreated(const wxString &json) {
  return S411JsonField(json, "Created").Lower() == "true";
}

size_t S411JsonOurBoundaryGuids(const wxString &json,
                                std::vector<wxString> &guids) {
  // For every "GUID", read the Name/BoundaryName from the SAME flat JSON
  // object (bounded by the enclosing { }), independent of key order, and keep
  // it only when that object's own name carries our tag. This never lets one
  // boundary's name leak onto another boundary's GUID - deleting a user's
  // boundary would be data loss.
  const size_t before = guids.size();
  int p = 0;
  while (p < (int)json.length()) {
    const int nGuid = FindKey(json, "GUID", p);
    if (nGuid == wxNOT_FOUND) break;
    int objStart = json.rfind('{', nGuid);
    int objEnd = json.find('}', nGuid);
    if (objStart == wxNOT_FOUND) objStart = 0;
    if (objEnd == wxNOT_FOUND) objEnd = (int)json.length() - 1;
    const wxString seg = json.Mid(objStart, objEnd - objStart + 1);
    wxString name = S411JsonField(seg, "BoundaryName");
    if (name.IsEmpty()) name = S411JsonField(seg, "Name");
    const wxString guid = S411JsonField(seg, "GUID");
    if (!guid.IsEmpty() && name.StartsWith(S411_OD_TAG))
      guids.push_back(guid);
    p = objEnd + 1;  // continue past this object
  }
  return guids.size() - before;
}
