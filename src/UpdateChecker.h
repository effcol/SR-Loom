// UpdateChecker.h — background poll of GitHub Releases for a newer SR Loom build.
#pragma once

#include <windows.h>
#include <string>

namespace srw
{
    // Result of an update check. Heap-allocated and handed to the main
    // thread via PostMessage; the receiver owns it and must `delete` it.
    struct ReleaseInfo
    {
        enum Status
        {
            Available,   // a release tag newer than kAppVersion was found
            UpToDate,    // latest release tag == kAppVersion (user-triggered check only)
            Failed,      // network / parse failure (user-triggered check only)
        };
        Status      status = Available;
        std::string tag;   // e.g. "v1.7" -- populated when Available or UpToDate
        std::string url;   // GitHub release page (HTML) -- populated when Available
        bool        forced = false;   // user-triggered check (always wants feedback)
    };

    namespace UpdateChecker
    {
        // Kick off an asynchronous check against the GitHub Releases API.
        // Posts `notifyMsg` to `hwnd` with `wParam = (WPARAM)(ReleaseInfo*)`
        // when the check completes.
        //
        // force=false (auto-check): respects a once-per-hour throttle stored
        // in HKCU\Software\SRLoom\LastUpdateCheckUnix to avoid hammering
        // api.github.com, and only posts when a newer release is found
        // (silent on up-to-date / failure -- we don't want to nag on launch).
        //
        // force=true (user-triggered, e.g. "Check for updates" button):
        // skips the throttle, and posts a ReleaseInfo for ALL outcomes
        // (Available / UpToDate / Failed) so the GUI can show feedback.
        //
        // Safe to call from the main thread; spawns a detached worker thread
        // so the caller never blocks on the network round-trip.
        void StartAsync(HWND hwnd, UINT notifyMsg, bool force = false);

        // The newest release seen by the LAST successful check (cached in
        // HKCU\Software\SRLoom), if it's newer than this build. Lets the GUI
        // + tray show "update available" straight away on launch, even when
        // the throttle skips the network check. False if none / up to date.
        bool CachedUpdate(std::string& tag, std::string& url);
    }
}
