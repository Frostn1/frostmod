// A13: what `frostmod.exe --update` will fetch and what it will install (src/updatetrust.h).
//
// The URL rule is pure. The signature rule runs WinVerifyTrust for real: this test binary itself
// is unsigned, a copy of a Microsoft-signed file is signed by Microsoft and not by Creste LLC, and
// one changed byte breaks the signature.

#include "../src/updatetrust.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

using namespace updatetrust;

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

int main() {
    {   // URLs
        CHECK(IsTrustedAssetUrl("https://github.com/Frostn1/frostmod-releases/releases/download/v0.53.0/frostmod.exe"), "release asset");
        CHECK(IsTrustedAssetUrl("HTTPS://GitHub.com/Frostn1/frostmod/releases/download/v1/frostmod.dll"), "case-insensitive scheme and host");
        CHECK(IsTrustedAssetUrl("https://objects.githubusercontent.com/x/y"), "GitHub's asset CDN");
        CHECK(!IsTrustedAssetUrl("http://github.com/Frostn1/frostmod/releases/download/v1/frostmod.exe"), "plain http");
        CHECK(!IsTrustedAssetUrl("https://example.invalid/frostmod.exe"), "another host");
        CHECK(!IsTrustedAssetUrl("https://github.com.example.invalid/frostmod.exe"), "a suffix trick");
        CHECK(!IsTrustedAssetUrl("https://evil.invalid@github.com/x"), "user info");
        CHECK(!IsTrustedAssetUrl("https://github.com:8443/x"), "a port");
        CHECK(!IsTrustedAssetUrl("https://github.com"), "no path");
        CHECK(!IsTrustedAssetUrl("ftp://github.com/x"), "another scheme");
        CHECK(!IsTrustedAssetUrl(""), "empty");
    }

    {   // signatures
        wchar_t self[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        CHECK(!SignedBy(self), "this unsigned test binary is not Creste LLC's");
        CHECK(!SignedBy(L"C:\\definitely\\not\\here.exe"), "a missing file is not signed");

        struct Sample { std::wstring path; const wchar_t* cn; };
        wchar_t pf86[MAX_PATH] = {0}, win[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);
        GetEnvironmentVariableW(L"WINDIR", win, MAX_PATH);
        const Sample samples[] = {
            {std::wstring(pf86) + L"\\Microsoft\\Edge\\Application\\msedge.exe", L"Microsoft Corporation"},
            {std::wstring(win) + L"\\System32\\MRT.exe", L"Microsoft Windows"},
        };
        bool tried = false;
        for (const Sample& s : samples) {
            if (!Exists(s.path)) continue;
            tried = true;
            CHECK(SignedBy(s.path, s.cn), "%ls verifies as %ls", s.path.c_str(), s.cn);
            CHECK(!SignedBy(s.path), "%ls is not Creste LLC's", s.path.c_str());

            // A copy with one byte changed must fail: this is the check a tampered download meets.
            wchar_t tmpDir[MAX_PATH] = {0}, tmp[MAX_PATH] = {0};
            GetTempPathW(MAX_PATH, tmpDir);
            GetTempFileNameW(tmpDir, L"upt", 0, tmp);
            std::vector<char> bytes;
            if (FILE* in = _wfopen(s.path.c_str(), L"rb")) {
                char buf[65536];
                for (size_t n; (n = fread(buf, 1, sizeof(buf), in)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
                fclose(in);
            }
            if (bytes.size() > 4096) {
                bytes[bytes.size() / 2] ^= 0x5A;
                if (FILE* out = _wfopen(tmp, L"wb")) {
                    fwrite(bytes.data(), 1, bytes.size(), out);
                    fclose(out);
                }
                CHECK(!SignedBy(tmp, s.cn), "a tampered copy of %ls does not verify", s.path.c_str());
            }
            DeleteFileW(tmp);
            break;
        }
        if (!tried) std::printf("updatetrust_test: no Microsoft-signed sample on this machine; positive case skipped\n");
    }

    if (g_failures == 0) std::printf("updatetrust_test: all checks passed\n");
    else                 std::printf("updatetrust_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
