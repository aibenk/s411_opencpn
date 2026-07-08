/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * ocpn_draw_pi boundary protocol: build the JSON request payloads and parse
 * the response fields. GUI-free and OpenCPN-free so it is unit-tested
 * headlessly - only the actual SendPluginMessage transport lives in the
 * plugin.
 *
 * Protocol reference (ocpn_draw_pi src/ODJSON.cpp): every request carries
 * top-level "Source" (reply channel = our plugin message id), "Msg", "Type"
 * ("Request") and "MsgId" (correlation). Ice no-go zones are exported as
 * BoundaryType "Exclusion" boundaries, which ocpn_draw creates Active, which
 * is exactly what weather_routing_pi treats as "do not cross".
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <wx/string.h>

#include "S411Router.h"

// the message_id ocpn_draw_pi listens on, and the reply channel we use
#define S411_OD_TARGET "OCPN_DRAW_PI"
#define S411_OD_SOURCE "S411_PI"
// every boundary we create is named with this prefix so we can enumerate and
// delete our own zones idempotently (ocpn_draw owns the GUIDs)
#define S411_OD_TAG "S-411 ice: "

/** Build a CreateBoundary request for one hazard zone. The exterior ring is
 *  sent as BoundaryPoints (ocpn_draw closes the ring itself, so the last
 *  point must NOT duplicate the first). Colour follows severity. */
wxString S411BuildCreateBoundary(const S411HazardZone &zone,
                                 const wxString &msgId);

/** Build a DeleteBoundary request (GUID at top level). */
wxString S411BuildDeleteBoundary(const wxString &guid);

/** Build a Version handshake request. */
wxString S411BuildVersionRequest();

/** Build a BoundaryInformation request (to enumerate existing boundaries). */
wxString S411BuildBoundaryInfoRequest();

/** Extract a top-level or "Boundary" string field's value from a JSON
 *  response body (minimal scanner; returns empty if absent). */
wxString S411JsonField(const wxString &json, const wxString &key);

/** Was CreateBoundary successful ("Created":true present)? */
bool S411JsonCreated(const wxString &json);

/** From a BoundaryInformation response, collect the GUIDs of every boundary
 *  whose Name begins with S411_OD_TAG (our own zones). */
size_t S411JsonOurBoundaryGuids(const wxString &json,
                                std::vector<wxString> &guids);

/** JSON string escape (public for tests). */
wxString S411JsonEscape(const wxString &s);
