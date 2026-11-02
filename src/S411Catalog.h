/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Exchange catalogue reader and S-100 Part 15 signature check.
 *
 *  - CATALOG.XML (S-100 5.2.0 Part 17): per dataset file the temporal
 *    extent (valid from/until), producer code and agency, and the
 *    ECDSA-384-SHA2 signature with its certificates.
 *  - catalogue.xml (S-411 Edition 1.x exchange sets as distributed by the
 *    ice services): only the issue date and the organisation.
 *  - CATALOG.SIGN: the standalone signature over CATALOG.XML.
 *
 * Signatures are verified with the platform crypto library (Windows CNG,
 * OpenSSL elsewhere when S411_HAVE_OPENSSL is defined). Without one the
 * state is reported as "unverified", never as valid.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#pragma once

#include <wx/string.h>

#include <ctime>
#include <map>
#include <string>
#include <vector>

/** Signature state of one dataset (S-100 Part 15). */
enum class S411SigState {
  None,        // no exchange catalogue / no signature (e.g. Edition 1.x)
  Valid,       // dataset and certificate chain verify
  Invalid,     // a signature or the certificate chain does not verify
  Unverified,  // signed, but no crypto library to check it with
};

struct S411CatalogEntry {
  wxString fileName;      // base name, upper case (e.g. 411DE00BAL260301.GML)
  wxString producerCode;  // e.g. DE00
  wxString agency;        // producing agency / organisation name
  time_t validFrom = 0;   // timeInstantBegin (UTC), 0 = absent
  time_t validUntil = 0;  // timeInstantEnd (UTC), 0 = absent
  wxString signature;     // base64 DER ECDSA signature, empty = unsigned
  wxString certRef;       // id of the signing certificate
};

struct S411Catalog {
  bool ok = false;        // parsed as an exchange catalogue
  bool edition1 = false;  // S-411 1.x catalogue.xml (no signatures)
  time_t issued = 0;      // catalogue date (1.x: the only time it carries)
  wxString agency;        // catalogue contact organisation
  std::map<wxString, wxString> certs;      // id -> base64 DER certificate
  std::map<wxString, wxString> certIssuer; // id -> issuer id
  std::map<wxString, wxString> saCertId;   // SA id -> its certificate id
  std::vector<S411CatalogEntry> entries;
  // CATALOG.SIGN (standalone signature over the CATALOG.XML bytes)
  wxString catalogSignature, catalogCertRef;

  const S411CatalogEntry *Find(const wxString &fileName) const;
};

/** Parse an exchange catalogue from its bytes (either edition). */
bool S411ParseCatalog(const std::string &xml, S411Catalog &out);

/** Parse CATALOG.SIGN and attach its signature to an already parsed
 *  catalogue (certificates are merged). */
bool S411ParseCatalogSign(const std::string &xml, S411Catalog &cat);

/** Verify `data` against a dataset signature listed in the catalogue:
 *  the signing certificate must be issued by the catalogue's scheme
 *  administrator certificate and the ECDSA-384-SHA2 signature must match.
 *  `note` receives a short human-readable result (signer, TEST PKI ...). */
S411SigState S411VerifyData(const S411Catalog &cat, const std::string &data,
                            const wxString &sigB64, const wxString &certRef,
                            wxString &note);

/** Parse an ISO 8601 UTC date/time ("2026-03-01T12:00:00Z", "2026-03-01",
 *  "20260301") into a time_t. Returns 0 when not parseable. */
time_t S411ParseIsoTime(const wxString &s);

/** Short label for a signature state ("signed", "unsigned" ...). */
wxString S411SigLabel(S411SigState s);
