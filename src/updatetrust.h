// What `frostmod.exe --update` will download and what it will put in place (security report A13).
//
// The asset URL comes out of the release JSON, so before this an http:// URL there was fetched in
// the clear and the file was moved over frostmod.exe/.dll and launched with nothing checked.
// Now:
// - only https:// URLs on GitHub's own release hosts are fetched (WinHTTP then refuses any
//   redirect from https down to http by default), and
// - a downloaded file is only swapped in if Windows verifies its Authenticode signature and the
//   signer is Creste LLC, the certificate every release build is signed with
//   (.github/workflows/release-build.yml checks the same subject before attaching).
#pragma once

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <cctype>
#include <string>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace updatetrust {

// The signer's common name on every FrostMod release binary.
constexpr const wchar_t* kSigner = L"Creste LLC";

// https:// on a host GitHub serves release assets from, nothing else. A port, user info or an
// odd host spelling is refused rather than reasoned about.
inline bool IsTrustedAssetUrl(const std::string& url) {
    static const char kScheme[] = "https://";
    if (url.size() <= sizeof(kScheme) - 1) return false;
    for (size_t i = 0; i < sizeof(kScheme) - 1; ++i)
        if (std::tolower((unsigned char)url[i]) != kScheme[i]) return false;
    const size_t start = sizeof(kScheme) - 1;
    const size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos || url[end] != '/') return false;
    std::string host = url.substr(start, end - start);
    for (char& c : host) c = (char)std::tolower((unsigned char)c);
    static const char* const kHosts[] = {
        "github.com",                              // browser_download_url
        "objects.githubusercontent.com",           // where it redirects
        "release-assets.githubusercontent.com",
    };
    for (const char* h : kHosts)
        if (host == h) return true;
    return false;
}

// True only when `path` carries a valid Authenticode signature (chain to a trusted root,
// revocation checked across the whole chain) AND the leaf signer's common name is `signer`.
inline bool SignedBy(const std::wstring& path, const wchar_t* signer = kSigner) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &file;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_SAFER_FLAG;

    const LONG status = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    bool ok = false;
    if (status == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* prov = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sgnr = prov ? WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = sgnr ? WTHelperGetProvCertFromChain(sgnr, 0) : nullptr;
        if (cert && cert->pCert) {
            wchar_t name[256] = {0};
            CertGetNameStringW(cert->pCert, CERT_NAME_ATTR_TYPE, 0, (void*)szOID_COMMON_NAME,
                               name, (DWORD)(sizeof(name) / sizeof(name[0])));
            ok = std::wstring(name) == signer;
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return ok;
}

} // namespace updatetrust
