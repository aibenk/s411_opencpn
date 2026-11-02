/***************************************************************************
 * S-411 Ice Information plugin for OpenCPN
 * Exchange catalogue reader and S-100 Part 15 signature check.
 *
 * Copyright (C) 2026 by the s411_pi authors
 * License: GNU General Public License v2 or later (see LICENSE)
 ***************************************************************************/
#include "S411Catalog.h"

#include <wx/base64.h>
#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/xml/xml.h>

#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#endif
#define S411_CRYPTO_CNG 1
#elif defined(S411_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/x509.h>
#define S411_CRYPTO_OPENSSL 1
#endif

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static wxString Local(const wxXmlNode *n) {
  wxString s = n->GetName();
  int i = s.Find(':', true);
  return i == wxNOT_FOUND ? s : s.Mid(i + 1);
}

static bool IsElem(const wxXmlNode *n) {
  return n && n->GetType() == wxXML_ELEMENT_NODE;
}

static const wxXmlNode *FindDesc(const wxXmlNode *n, const wxString &loc) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    if (Local(c) == loc) return c;
    const wxXmlNode *r = FindDesc(c, loc);
    if (r) return r;
  }
  return nullptr;
}

static wxString DescText(const wxXmlNode *n, const wxString &loc) {
  const wxXmlNode *d = n ? FindDesc(n, loc) : nullptr;
  return d ? d->GetNodeContent().Trim().Trim(false) : wxString();
}

static wxString AttrLocal(const wxXmlNode *n, const wxString &loc) {
  for (wxXmlAttribute *a = n->GetAttributes(); a; a = a->GetNext())
    if (a->GetName().AfterLast(':') == loc) return a->GetValue();
  return "";
}

static bool LoadXml(const std::string &bytes, wxXmlDocument &doc) {
  wxMemoryInputStream in(bytes.data(), bytes.size());
  wxLogNull quiet;
  return doc.Load(in) && doc.GetRoot();
}

// days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant)
static long DaysFromCivil(long y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

time_t S411ParseIsoTime(const wxString &str) {
  wxString s = str;
  s.Trim().Trim(false);
  std::string a = std::string(s.ToAscii());
  // keep digits only for the date part; accept 2026-03-01 and 20260301
  int v[6] = {0, 0, 0, 0, 0, 0};
  const int width[6] = {4, 2, 2, 2, 2, 2};
  size_t i = 0;
  for (int f = 0; f < 6; f++) {
    while (i < a.size() && !isdigit((unsigned char)a[i])) {
      if (f >= 3 && a[i] != 'T' && a[i] != ':' && a[i] != ' ') break;
      i++;
    }
    int n = 0, k = 0;
    while (k < width[f] && i < a.size() && isdigit((unsigned char)a[i])) {
      n = n * 10 + (a[i] - '0');
      i++;
      k++;
    }
    if (k != width[f]) {
      if (f < 3) return 0;  // the date is mandatory
      break;                // time fields are optional
    }
    v[f] = n;
  }
  if (v[0] < 1970 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 ||
      v[3] > 23 || v[4] > 59 || v[5] > 60)
    return 0;
  return (time_t)DaysFromCivil(v[0], v[1], v[2]) * 86400 + v[3] * 3600 +
         v[4] * 60 + v[5];
}

wxString S411SigLabel(S411SigState s) {
  switch (s) {
    case S411SigState::Valid: return "signed";
    case S411SigState::Invalid: return "SIGNATURE INVALID";
    case S411SigState::Unverified: return "signed (not verified)";
    default: return "unsigned";
  }
}

const S411CatalogEntry *S411Catalog::Find(const wxString &fileName) const {
  const wxString key = fileName.AfterLast('/').AfterLast('\\').Upper();
  for (const auto &e : entries)
    if (e.fileName == key) return &e;
  return nullptr;
}

// ---------------------------------------------------------------------------
// catalogue parsing
// ---------------------------------------------------------------------------
static void CollectCerts(const wxXmlNode *n, S411Catalog &cat) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    const wxString loc = Local(c);
    if (loc == "certificate") {
      const wxString id = AttrLocal(c, "id");
      wxString b64 = c->GetNodeContent();
      b64.Replace("\n", ""); b64.Replace("\r", ""); b64.Replace(" ", "");
      if (!id.IsEmpty() && !b64.IsEmpty()) {
        cat.certs[id] = b64;
        cat.certIssuer[id] = AttrLocal(c, "issuer");
      }
    } else if (loc == "schemeAdministrator") {
      const wxString id = AttrLocal(c, "id");
      if (!id.IsEmpty() && !cat.saCertId.count(id)) cat.saCertId[id] = "";
    } else {
      CollectCerts(c, cat);
    }
  }
}

// the SA's own certificate: issued by the SA id, and not a data server cert
// that something else references as its signer
static void ResolveSaCerts(S411Catalog &cat) {
  for (auto &sa : cat.saCertId) {
    for (const auto &ci : cat.certIssuer) {
      if (ci.second != sa.first) continue;
      bool usedAsSigner = false;
      for (const auto &e : cat.entries)
        if (e.certRef == ci.first) usedAsSigner = true;
      if (cat.catalogCertRef == ci.first) usedAsSigner = true;
      if (!usedAsSigner) { sa.second = ci.first; break; }
    }
  }
}

// organisation of an ISO 19115 responsible party (never the person)
static wxString OrgName(const wxXmlNode *party) {
  if (!party) return "";
  for (const char *loc : {"organisationName", "name", "organization"}) {
    const wxXmlNode *o = FindDesc(party, loc);
    if (!o) continue;
    wxString v = DescText(o, "CharacterString");
    if (v.IsEmpty()) v = o->GetNodeContent().Trim().Trim(false);
    if (!v.IsEmpty()) return v;
  }
  return "";
}

static void ParseDiscovery(const wxXmlNode *n, S411Catalog &cat) {
  for (const wxXmlNode *c = n->GetChildren(); c; c = c->GetNext()) {
    if (!IsElem(c)) continue;
    if (Local(c) != "S100_DatasetDiscoveryMetadata") {
      ParseDiscovery(c, cat);
      continue;
    }
    S411CatalogEntry e;
    wxString fn = DescText(c, "fileName");
    e.fileName = fn.AfterLast('/').AfterLast('\\').Upper();
    e.producerCode = DescText(c, "producerCode");
    e.agency = OrgName(FindDesc(c, "producingAgency"));
    e.validFrom = S411ParseIsoTime(DescText(c, "timeInstantBegin"));
    e.validUntil = S411ParseIsoTime(DescText(c, "timeInstantEnd"));
    const wxXmlNode *sig = FindDesc(c, "S100_SE_SignatureOnData");
    if (sig) {
      e.signature = sig->GetNodeContent().Trim().Trim(false);
      e.certRef = AttrLocal(sig, "certificateRef");
    }
    if (!e.fileName.IsEmpty()) cat.entries.push_back(e);
  }
}

bool S411ParseCatalog(const std::string &xml, S411Catalog &cat) {
  wxXmlDocument doc;
  if (!LoadXml(xml, doc)) {
    // Edition 1.x catalogues are sometimes not valid UTF-8 (Latin-1 names
    // in a file declared UTF-8): still recover the issue date
    const size_t p = xml.find("Date>");
    if (p != std::string::npos && p + 13 < xml.size()) {
      cat.issued = S411ParseIsoTime(wxString(xml.substr(p + 5, 8)));
      cat.edition1 = cat.ok = cat.issued != 0;
    }
    const size_t o = xml.find("organisationName>");
    const size_t c = o == std::string::npos
                         ? o
                         : xml.find("CharacterString>", o);
    const size_t e = c == std::string::npos ? c : xml.find('<', c);
    if (e != std::string::npos) {
      const std::string name = xml.substr(c + 16, e - c - 16);
      cat.agency = wxString(name.data(), wxConvISO8859_1, name.size());
      cat.agency.Trim().Trim(false);
    }
    return cat.ok;
  }
  const wxXmlNode *root = doc.GetRoot();
  if (Local(root) != "S100_ExchangeCatalogue") return false;
  cat.ok = true;
  // S-100 5.x exchange catalogues use the .../s100/xc/... namespace; the
  // S-411 1.x sets the ice services distribute use http://www.iho.int/S100EC
  cat.edition1 = true;
  for (wxXmlAttribute *a = root->GetAttributes(); a; a = a->GetNext())
    if (a->GetValue().Lower().Contains("/s100/xc/")) cat.edition1 = false;
  CollectCerts(root, cat);
  ParseDiscovery(root, cat);
  cat.agency = OrgName(FindDesc(root, "contact"));
  if (cat.edition1) {
    // S-411 Edition 1.x: identifier/date/gco:Date = YYYYMMDD
    const wxXmlNode *id = FindDesc(root, "identifier");
    if (id) cat.issued = S411ParseIsoTime(DescText(id, "Date"));
  } else {
    const wxXmlNode *id = FindDesc(root, "identifier");
    if (id) cat.issued = S411ParseIsoTime(DescText(id, "dateTime"));
  }
  ResolveSaCerts(cat);
  return true;
}

bool S411ParseCatalogSign(const std::string &xml, S411Catalog &cat) {
  wxXmlDocument doc;
  if (!LoadXml(xml, doc)) return false;
  const wxXmlNode *root = doc.GetRoot();
  if (Local(root) != "StandaloneDigitalSignature") return false;
  CollectCerts(root, cat);
  const wxXmlNode *sig = FindDesc(root, "digitalSignature");
  if (!sig) return false;
  cat.catalogSignature = sig->GetNodeContent().Trim().Trim(false);
  cat.catalogCertRef = AttrLocal(sig, "certificateRef");
  ResolveSaCerts(cat);
  return !cat.catalogSignature.IsEmpty();
}

// ---------------------------------------------------------------------------
// signature verification
// ---------------------------------------------------------------------------
static std::string B64(const wxString &s) {
  wxMemoryBuffer b = wxBase64Decode(s, wxBase64DecodeMode_SkipWS);
  return std::string((const char *)b.GetData(), b.GetDataLen());
}

#if defined(S411_CRYPTO_CNG)
// DER SEQUENCE { INTEGER r, INTEGER s } -> fixed width r || s (P-384: 48+48)
static bool DerToRaw(const std::string &der, unsigned char raw[96]) {
  size_t i = 0;
  auto len = [&](size_t &n) -> bool {
    if (i >= der.size()) return false;
    unsigned char b = (unsigned char)der[i++];
    if (b < 0x80) { n = b; return true; }
    int k = b & 0x7f;
    if (k > 2) return false;
    n = 0;
    while (k--) {
      if (i >= der.size()) return false;
      n = (n << 8) | (unsigned char)der[i++];
    }
    return true;
  };
  size_t n;
  if (i >= der.size() || der[i++] != 0x30 || !len(n)) return false;
  memset(raw, 0, 96);
  for (int part = 0; part < 2; part++) {
    if (i >= der.size() || der[i++] != 0x02 || !len(n) || i + n > der.size())
      return false;
    size_t off = i, cnt = n;
    while (cnt > 48 && der[off] == 0) { off++; cnt--; }
    if (cnt > 48) return false;
    memcpy(raw + part * 48 + (48 - cnt), der.data() + off, cnt);
    i += n;
  }
  return true;
}

static wxString CertName(PCCERT_CONTEXT c) {
  wchar_t buf[256];
  CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, buf, 256);
  return wxString(buf);
}

static S411SigState VerifyImpl(const std::string &data, const std::string &sig,
                               const std::string &dsDer,
                               const std::string &saDer, wxString &note) {
  PCCERT_CONTEXT ds = CertCreateCertificateContext(
      X509_ASN_ENCODING, (const BYTE *)dsDer.data(), (DWORD)dsDer.size());
  PCCERT_CONTEXT sa = CertCreateCertificateContext(
      X509_ASN_ENCODING, (const BYTE *)saDer.data(), (DWORD)saDer.size());
  S411SigState st = S411SigState::Invalid;
  BCRYPT_KEY_HANDLE key = nullptr;
  BCRYPT_ALG_HANDLE alg = nullptr;
  unsigned char hash[48], raw[96];
  if (!ds || !sa) {
    note = "certificate not readable";
    goto done;
  }
  note = "signer: " + CertName(ds) + ", issuer: " + CertName(sa);
  if (!CryptVerifyCertificateSignatureEx(
          0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT,
          (void *)ds, CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)sa, 0,
          nullptr)) {
    note += " - certificate not issued by the scheme administrator";
    goto done;
  }
  if (!CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING,
                                   &ds->pCertInfo->SubjectPublicKeyInfo, 0,
                                   nullptr, &key)) {
    note += " - unsupported key";
    goto done;
  }
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA384_ALGORITHM, nullptr,
                                  0) != 0 ||
      BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(),
                 hash, sizeof hash) != 0) {
    note += " - hashing failed";
    goto done;
  }
  if (!DerToRaw(sig, raw)) {
    note += " - malformed signature";
    goto done;
  }
  if (BCryptVerifySignature(key, nullptr, hash, sizeof hash, raw, sizeof raw,
                            0) == 0)
    st = S411SigState::Valid;
  else
    note += " - data does not match the signature";
done:
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  if (key) BCryptDestroyKey(key);
  if (ds) CertFreeCertificateContext(ds);
  if (sa) CertFreeCertificateContext(sa);
  return st;
}
#elif defined(S411_CRYPTO_OPENSSL)
static wxString CertName(X509 *c) {
  char buf[256];
  X509_NAME *n = X509_get_subject_name(c);
  int k = X509_NAME_get_text_by_NID(n, NID_commonName, buf, sizeof buf);
  return k > 0 ? wxString(buf, wxConvUTF8) : wxString("?");
}

static S411SigState VerifyImpl(const std::string &data, const std::string &sig,
                               const std::string &dsDer,
                               const std::string &saDer, wxString &note) {
  const unsigned char *p = (const unsigned char *)dsDer.data();
  X509 *ds = d2i_X509(nullptr, &p, (long)dsDer.size());
  p = (const unsigned char *)saDer.data();
  X509 *sa = d2i_X509(nullptr, &p, (long)saDer.size());
  S411SigState st = S411SigState::Invalid;
  if (!ds || !sa) {
    note = "certificate not readable";
  } else {
    note = "signer: " + CertName(ds) + ", issuer: " + CertName(sa);
    EVP_PKEY *saKey = X509_get_pubkey(sa);
    EVP_PKEY *dsKey = X509_get_pubkey(ds);
    if (!saKey || X509_verify(ds, saKey) != 1) {
      note += " - certificate not issued by the scheme administrator";
    } else if (dsKey) {
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha384(), nullptr, dsKey) ==
              1 &&
          EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1 &&
          EVP_DigestVerifyFinal(ctx, (const unsigned char *)sig.data(),
                                sig.size()) == 1)
        st = S411SigState::Valid;
      else
        note += " - data does not match the signature";
      EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(saKey);
    EVP_PKEY_free(dsKey);
  }
  X509_free(ds);
  X509_free(sa);
  return st;
}
#endif

S411SigState S411VerifyData(const S411Catalog &cat, const std::string &data,
                            const wxString &sigB64, const wxString &certRef,
                            wxString &note) {
  note.Clear();
  if (sigB64.IsEmpty()) return S411SigState::None;
  auto ci = cat.certs.find(certRef);
  auto ii = cat.certIssuer.find(certRef);
  if (ci == cat.certs.end() || ii == cat.certIssuer.end()) {
    note = "signing certificate '" + certRef + "' missing from the catalogue";
    return S411SigState::Invalid;
  }
  auto sa = cat.saCertId.find(ii->second);
  if (sa == cat.saCertId.end() || sa->second.IsEmpty() ||
      !cat.certs.count(sa->second)) {
    note = "scheme administrator certificate missing from the catalogue";
    return S411SigState::Invalid;
  }
#if defined(S411_CRYPTO_CNG) || defined(S411_CRYPTO_OPENSSL)
  S411SigState st = VerifyImpl(data, B64(sigB64), B64(ci->second),
                               B64(cat.certs.at(sa->second)), note);
  if (note.Upper().Contains("TEST"))
    note += " (TEST PKI - not for navigation)";
  return st;
#else
  (void)data;
  note = "no crypto library in this build";
  return S411SigState::Unverified;
#endif
}
