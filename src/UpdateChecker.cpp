// UpdateChecker.cpp — WinHTTP poll of GitHub's "/releases/latest" endpoint.
// The whole thing runs on a detached thread; we never block the main loop.
//
// JSON parsing is deliberately substring-only. The GitHub releases payload
// gives us the two fields we want (`tag_name`, `html_url`) as plain string
// values in the top-level object — pulling in a JSON dependency for that
// would be overkill. If GitHub ever changes the shape we'll just stop
// detecting updates, which is the safe failure mode.

#include "UpdateChecker.h"

#include "Common.h"
#include <windows.h>
#include <winhttp.h>
#include <thread>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "winhttp.lib")

namespace
{
    // Registry path for the throttle timestamp (sibling of Settings.cpp's keys
    // — kept inline so UpdateChecker is self-contained and doesn't need a new
    // Settings entry per addition).
    constexpr wchar_t kSettingsKey[]      = L"Software\\SRLoom";
    constexpr wchar_t kLastCheckValue[]   = L"LastUpdateCheckUnix";
    constexpr DWORD   kThrottleSeconds    = 6 * 60 * 60;   // 6 hours

    // Read the throttle timestamp; 0 if never recorded.
    DWORD ReadLastCheckUnix()
    {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &h) != ERROR_SUCCESS)
            return 0;
        DWORD v = 0, sz = sizeof(v), type = REG_DWORD;
        if (RegQueryValueExW(h, kLastCheckValue, nullptr, &type,
                             reinterpret_cast<BYTE*>(&v), &sz) != ERROR_SUCCESS
            || type != REG_DWORD)
            v = 0;
        RegCloseKey(h);
        return v;
    }

    void WriteLastCheckUnix(DWORD value)
    {
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS)
            return;
        RegSetValueExW(h, kLastCheckValue, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(h);
    }

    // Seconds since the Unix epoch (sufficient through 2106 in a DWORD).
    DWORD UnixNow()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart  = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        // FILETIME is 100-ns intervals since 1601-01-01; convert to seconds since 1970.
        constexpr ULONGLONG kEpochDiff100ns = 116444736000000000ULL;
        return static_cast<DWORD>((u.QuadPart - kEpochDiff100ns) / 10000000ULL);
    }

    // Extract the value of a string field from a tiny chunk of JSON.
    // Looks for `"<field>":"<value>"`, returning <value> with JSON escapes
    // un-handled (release tags and URLs don't contain any). Empty on miss.
    std::string ExtractStringField(const std::string& json, const char* field)
    {
        std::string needle = std::string("\"") + field + "\"";
        size_t i = json.find(needle);
        if (i == std::string::npos) return {};
        i = json.find(':', i + needle.size());
        if (i == std::string::npos) return {};
        i = json.find('"', i + 1);
        if (i == std::string::npos) return {};
        const size_t start = i + 1;
        const size_t end = json.find('"', start);
        if (end == std::string::npos) return {};
        return json.substr(start, end - start);
    }

    // Compare dotted-numeric version strings like "1.6" vs "v1.7". Returns
    // positive if a > b, negative if a < b, zero if equal. Any leading 'v' is
    // skipped; any non-numeric tail (e.g. "1.6-rc1") is ignored after the
    // numeric prefix is consumed. Unparseable components count as 0.
    int CompareVersions(const std::string& aRaw, const std::string& bRaw)
    {
        auto strip = [](const std::string& s)
        {
            return (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? s.substr(1) : s;
        };
        const std::string a = strip(aRaw);
        const std::string b = strip(bRaw);
        size_t ia = 0, ib = 0;
        while (ia < a.size() || ib < b.size())
        {
            int va = 0, vb = 0;
            while (ia < a.size() && a[ia] >= '0' && a[ia] <= '9') va = va * 10 + (a[ia++] - '0');
            while (ib < b.size() && b[ib] >= '0' && b[ib] <= '9') vb = vb * 10 + (b[ib++] - '0');
            if (va != vb) return va - vb;
            // Skip a single dot (or whatever non-digit separator) between components.
            if (ia < a.size() && a[ia] == '.') ++ia; else break;
            if (ib < b.size() && b[ib] == '.') ++ib; else break;
        }
        return 0;
    }

    // GET https://api.github.com/repos/<slug>/releases/latest into `out`.
    // Returns true on HTTP 200 with a non-empty body.
    bool FetchLatestRelease(const std::wstring& host, const std::wstring& path,
                            std::string& out)
    {
        HINTERNET hSession = WinHttpOpen(L"SRLoom-Update-Check/1.0",
                                          WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;
        // 5s connect + 10s receive caps; tiny payload, no excuse for hanging.
        WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 10000);

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                            INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE);
        if (!hRequest)
        {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // GitHub requires both a User-Agent (set via WinHttpOpen above) and
        // it's polite to ask for the v3 JSON media type explicitly.
        const wchar_t kHeaders[] = L"Accept: application/vnd.github+json\r\n";
        bool ok = false;
        if (WinHttpSendRequest(hRequest, kHeaders, (DWORD)-1L,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(hRequest, nullptr))
        {
            DWORD status = 0, sz = sizeof(status);
            if (WinHttpQueryHeaders(hRequest,
                                     WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                     WINHTTP_NO_HEADER_INDEX)
                && status == 200)
            {
                out.clear();
                std::vector<char> buf(8192);
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0)
                {
                    if (buf.size() < available) buf.resize(available);
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, buf.data(), available, &read) || read == 0)
                        break;
                    out.append(buf.data(), read);
                    // Cap at 256 KB — releases payload is ~5 KB; if we're past
                    // this we're being fed something weird, abort defensively.
                    if (out.size() > 256 * 1024) break;
                }
                ok = !out.empty();
            }
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return ok;
    }
}

namespace srw::UpdateChecker
{
    void StartAsync(HWND hwnd, UINT notifyMsg, bool force)
    {
        if (!hwnd) return;

        // Throttle: skip if we checked within the last kThrottleSeconds --
        // unless this is a user-forced check, where the whole point is to
        // refresh on demand.
        if (!force)
        {
            const DWORD now  = UnixNow();
            const DWORD last = ReadLastCheckUnix();
            if (last != 0 && now > last && (now - last) < kThrottleSeconds)
            {
                Log("UpdateChecker: skipping (last check %u s ago, throttle %u s)",
                    (unsigned)(now - last), (unsigned)kThrottleSeconds);
                return;
            }
        }

        std::thread([hwnd, notifyMsg, force]()
        {
            // Helper: post (or drop, transferring ownership to the message queue).
            auto post = [&](ReleaseInfo* info) {
                if (!PostMessageW(hwnd, notifyMsg, reinterpret_cast<WPARAM>(info), 0))
                    delete info;
            };

            // Build the path "/repos/<slug>/releases/latest" -> wide for WinHTTP.
            std::string pathA = std::string("/repos/") + kRepoSlug + "/releases/latest";
            std::wstring path(pathA.begin(), pathA.end());

            std::string body;
            if (!FetchLatestRelease(L"api.github.com", path, body))
            {
                Log("UpdateChecker: fetch failed");
                if (force) post(new ReleaseInfo{ReleaseInfo::Failed, {}, {}});
                return;
            }
            // Record the check timestamp regardless of result so we don't
            // re-hit the API on every launch even if no release exists yet.
            WriteLastCheckUnix(UnixNow());

            const std::string tag = ExtractStringField(body, "tag_name");
            const std::string url = ExtractStringField(body, "html_url");
            if (tag.empty())
            {
                Log("UpdateChecker: no tag_name in response (len=%zu)", body.size());
                if (force) post(new ReleaseInfo{ReleaseInfo::Failed, {}, {}});
                return;
            }

            const int cmp = CompareVersions(tag, kAppVersion);
            Log("UpdateChecker: current=%s latest=%s cmp=%d", kAppVersion, tag.c_str(), cmp);
            if (cmp <= 0)
            {
                // Up to date (or somehow ahead) -- silent unless forced.
                if (force) post(new ReleaseInfo{ReleaseInfo::UpToDate, tag, url});
                return;
            }

            // A newer release exists -- always post.
            post(new ReleaseInfo{ReleaseInfo::Available, tag, url});
        }).detach();
    }
}
