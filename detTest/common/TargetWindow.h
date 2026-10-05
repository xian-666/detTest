#pragma once
// TargetWindow.h
//
// Resolves which window detTest captures from.
//
// The parent project hard-coded the Honour-of-Kings *window title* inside
// findGameWindow(). detTest targets three different games, and a title is the
// least stable identifier available: launchers, anti-cheat shells and UE
// helper windows all carry similar captions, and several games localise the
// caption. So the primary key is now the **process image name**, which is what
// actually identifies the game.
//
// Matching model
// --------------
//   process   PRIMARY   substring of the owning process image name
//                       ("ZenlessZoneZero" matches ZenlessZoneZero.exe)
//   title     OPTIONAL  substring of the window caption, narrows the choice
//   class     OPTIONAL  substring of the window class name, narrows further
//
// Empty filters are ignored, so `process` alone means "any visible window of
// that executable". All comparisons are case-insensitive.
//
// Thread safety: findGameWindow() is called from the main loop *and* from the
// async inference thread (GameInferenceSystem.cpp), so the PID cache below is
// mutex-protected and never returns a reference to a temporary.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "AppConfig.h"

namespace target {

struct Spec {
    std::wstring process;    // PRIMARY: substring of the owning process image name
    std::wstring title;      // optional narrowing: window caption
    std::wstring className;  // optional narrowing: window class name

    bool Empty() const { return title.empty() && className.empty() && process.empty(); }

    std::wstring Describe() const {
        std::wstring out = L"[";
        bool first = true;
        auto append = [&](const std::wstring& s) {
            if (s.empty()) return;
            if (!first) out += L" ";
            out += s;
            first = false;
        };
        if (!process.empty())   append(L"process~'" + process + L"'");
        if (!title.empty())     append(L"title~'" + title + L"'");
        if (!className.empty()) append(L"class~'" + className + L"'");
        out += L"]";
        return out;
    }
};

// The historical title, kept only so a config.ini without a usable [Target]
// still behaves like the parent project instead of silently finding nothing.
inline const std::wstring& LegacyDefaultTitle() {
    static const std::wstring t = L"王者荣耀世界";
    return t;
}

inline Spec LoadSpec(const appcfg::IniFile& ini) {
    Spec s;
    s.process   = ini.GetString(L"Target", L"process", L"");
    s.title     = ini.GetString(L"Target", L"title",   L"");
    s.className = ini.GetString(L"Target", L"class",   L"");

    // Trim a trailing ".exe" so "ZenlessZoneZero.exe" and "ZenlessZoneZero"
    // behave identically -- they are substrings of each other anyway.
    if (!s.process.empty()) {
        std::wstring lower(s.process);
        for (auto& c : lower) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".exe") == 0) {
            s.process = s.process.substr(0, s.process.size() - 4);
        }
    }

    // Nothing usable configured -> fall back to the legacy title.
    if (s.Empty()) s.title = LegacyDefaultTitle();
    return s;
}

// ---------------------------------------------------------------------------
// Case-insensitive wide substring search. Avoids pulling in <cctype> locale
// rules, which differ per thread locale in ways we do not want here.
// ---------------------------------------------------------------------------
inline bool ContainsFold(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            wchar_t a = haystack[i + j], b = needle[j];
            if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
            if (a != b) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Process name -> PIDs, via a Toolhelp snapshot.
//
// Snapshot beats OpenProcess+QueryFullProcessImageNameW here because it needs
// no handle on the target: protected/elevated games refuse OpenProcess, and
// those are exactly the ones we must still be able to find.
// ---------------------------------------------------------------------------
inline std::vector<DWORD> EnumeratePids(const std::wstring& needle) {
    std::vector<DWORD> pids;
    if (needle.empty()) return pids;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring exe(pe.szExeFile);
            // Strip ".exe" from the snapshot entry as well, so a needle of
            // "ZenlessZoneZero.exe" cannot fail on an exact-match boundary.
            std::wstring base = exe;
            if (base.size() > 4) {
                std::wstring tail = base.substr(base.size() - 4);
                for (auto& c : tail) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                if (tail == L".exe") base = base.substr(0, base.size() - 4);
            }
            if (ContainsFold(base, needle) || ContainsFold(exe, needle)) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

// ---------------------------------------------------------------------------
// PID cache.
//
// findGameWindow() runs several times per frame from two threads. Re-snapshot
// the process table at that rate and it stops being free. Process sets change
// on the order of seconds, so a short TTL is plenty and keeps the cost at a
// couple of integer comparisons per call.
// ---------------------------------------------------------------------------
class PidCache {
public:
    static constexpr int kTtlMs = 1000;

    // Returns a stable reference valid until the next call on any thread with a
    // different needle, or until the TTL expires. Callers must not store it.
    std::shared_ptr<const std::vector<DWORD>> Get(const std::wstring& needle) {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto now = std::chrono::steady_clock::now();
        if (m_pids && m_key == needle &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_at).count() < kTtlMs) {
            return m_pids;
        }
        m_key = needle;
        m_pids = std::make_shared<const std::vector<DWORD>>(EnumeratePids(needle));
        m_at = now;
        return m_pids;
    }

private:
    std::mutex m_mutex;
    std::wstring m_key;
    std::shared_ptr<const std::vector<DWORD>> m_pids;
    std::chrono::steady_clock::time_point m_at{};
};

inline PidCache& Pids() {
    static PidCache c;
    return c;
}

// ---------------------------------------------------------------------------
// Window helpers
// ---------------------------------------------------------------------------
inline std::wstring WindowCaption(HWND hwnd) {
    if (!hwnd) return L"";
    wchar_t buf[512] = { 0 };
    if (GetWindowTextW(hwnd, buf, 512) == 0) return L"";
    return std::wstring(buf);
}

inline std::wstring WindowClassName(HWND hwnd) {
    if (!hwnd) return L"";
    wchar_t buf[512] = { 0 };
    if (GetClassNameW(hwnd, buf, 512) == 0) return L"";
    return std::wstring(buf);
}

inline long long WindowArea(HWND hwnd) {
    RECT rc = { 0, 0, 0, 0 };
    if (!GetWindowRect(hwnd, &rc)) return 0;
    long long w = static_cast<long long>(rc.right) - rc.left;
    long long h = static_cast<long long>(rc.bottom) - rc.top;
    return (w > 0 && h > 0) ? w * h : 0;
}

// Image base name of the process owning hwnd, or L"" when unavailable.
inline std::wstring ProcessNameOf(HWND hwnd) {
    if (!hwnd) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return L"";

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";

    wchar_t buf[MAX_PATH] = { 0 };
    DWORD len = static_cast<DWORD>(MAX_PATH);
    std::wstring full;
    if (QueryFullProcessImageNameW(h, 0, buf, &len) && len > 0) {
        full.assign(buf, len);
    }
    CloseHandle(h);

    if (full.empty()) return L"";
    size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? full : full.substr(slash + 1);
}

// Applies only the optional narrowing filters (title / class).
inline bool PassesFilters(HWND hwnd, const Spec& spec) {
    if (!spec.title.empty() && !ContainsFold(WindowCaption(hwnd), spec.title)) return false;
    if (!spec.className.empty() && !ContainsFold(WindowClassName(hwnd), spec.className)) return false;
    return true;
}

// Candidate ranking. Games own several top-level windows (launcher, crash
// handler, UE helper shells); the renderer is the visible one covering the
// screen, so rank by: top-level first, then restored, then captioned, then
// largest area.
struct RankKey {
    int notTopLevel = 0;
    int iconic = 0;
    int noCaption = 0;
    long long negArea = 0;
    HWND hwnd = nullptr;
};

inline RankKey Rank(HWND hwnd) {
    RankKey k;
    k.notTopLevel = (GetParent(hwnd) != NULL) ? 1 : 0;
    k.iconic = IsIconic(hwnd) ? 1 : 0;
    k.noCaption = WindowCaption(hwnd).empty() ? 1 : 0;
    k.negArea = -WindowArea(hwnd);
    k.hwnd = hwnd;
    return k;
}

inline bool RankLess(const RankKey& a, const RankKey& b) {
    if (a.notTopLevel != b.notTopLevel) return a.notTopLevel < b.notTopLevel;
    if (a.iconic != b.iconic) return a.iconic < b.iconic;
    if (a.noCaption != b.noCaption) return a.noCaption < b.noCaption;
    if (a.negArea != b.negArea) return a.negArea < b.negArea;
    return reinterpret_cast<std::uintptr_t>(a.hwnd) < reinterpret_cast<std::uintptr_t>(b.hwnd);
}

inline HWND PickBest(const std::vector<HWND>& hits) {
    if (hits.empty()) return nullptr;
    std::vector<RankKey> keys;
    keys.reserve(hits.size());
    for (HWND h : hits) keys.push_back(Rank(h));
    std::sort(keys.begin(), keys.end(), RankLess);
    return keys.front().hwnd;
}

// Usable as a capture source?
inline bool IsCandidate(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!IsWindowVisible(hwnd)) return false;
    if (WindowArea(hwnd) <= 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Process-driven enumeration (the primary path)
// ---------------------------------------------------------------------------
struct PidEnumCtx {
    const std::vector<DWORD>* pids;
    const Spec* spec;
    std::vector<HWND> hits;
};

inline BOOL CALLBACK EnumProcByPid(HWND hwnd, LPARAM lParam) {
    PidEnumCtx* ctx = reinterpret_cast<PidEnumCtx*>(lParam);
    if (!IsCandidate(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    if (std::find(ctx->pids->begin(), ctx->pids->end(), pid) == ctx->pids->end()) return TRUE;
    if (!PassesFilters(hwnd, *(ctx->spec))) return TRUE;

    ctx->hits.push_back(hwnd);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Filter-driven enumeration (fallback when no process is configured)
// ---------------------------------------------------------------------------
struct EnumCtx {
    const Spec* spec;
    std::vector<HWND> hits;
};

inline BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam) {
    EnumCtx* ctx = reinterpret_cast<EnumCtx*>(lParam);
    if (!IsCandidate(hwnd)) return TRUE;
    if (!PassesFilters(hwnd, *(ctx->spec))) return TRUE;
    ctx->hits.push_back(hwnd);
    return TRUE;
}

inline HWND Find(const Spec& spec) {
    if (spec.Empty()) return nullptr;

    std::vector<HWND> hits;

    if (!spec.process.empty()) {
        auto pids = Pids().Get(spec.process);

        // Fast path: what the user is actually interacting with. Skips the full
        // enumeration and, more importantly, keeps the tool pointed at the
        // window the user cares about when a game owns several.
        HWND fg = GetForegroundWindow();
        if (fg) {
            HWND root = GetAncestor(fg, GA_ROOT);
            HWND cand[2] = { root, fg };
            for (HWND h : cand) {
                if (!IsCandidate(h)) continue;
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == 0) continue;
                if (std::find(pids->begin(), pids->end(), pid) == pids->end()) continue;
                if (!PassesFilters(h, spec)) continue;
                return h;
            }
        }

        if (!pids->empty()) {
            PidEnumCtx ctx{ pids.get(), &spec, {} };
            EnumWindows(EnumProcByPid, reinterpret_cast<LPARAM>(&ctx));
            hits = std::move(ctx.hits);
        }
    } else {
        // No process configured: legacy title/class matching.
        HWND fg = GetForegroundWindow();
        if (fg) {
            HWND root = GetAncestor(fg, GA_ROOT);
            if (IsCandidate(root) && PassesFilters(root, spec)) return root;
            if (IsCandidate(fg) && PassesFilters(fg, spec)) return fg;
        }

        EnumCtx ctx{ &spec, {} };
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
        hits = std::move(ctx.hits);
    }

    return PickBest(hits);
}

// True when at least one running process matches the spec's process filter.
// Used for startup diagnostics so a typo in config.ini is reported as
// "process not running" instead of the vaguer "window not found".
inline bool ProcessRunning(const Spec& spec) {
    if (spec.process.empty()) return false;
    return !Pids().Get(spec.process)->empty();
}

inline std::wstring DescribeWindow(HWND hwnd) {
    if (!hwnd) return L"<null>";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    wchar_t buf[640];
    std::swprintf(buf, 640, L"hwnd=0x%p pid=%lu exe=%ls title='%ls'",
        static_cast<void*>(hwnd),
        static_cast<unsigned long>(pid),
        ProcessNameOf(hwnd).c_str(),
        WindowCaption(hwnd).c_str());
    return std::wstring(buf);
}

// The spec used by every target::Find() call. main() overwrites it once at
// startup from config.ini.
inline Spec& ActiveSpec() {
    static Spec s = [] {
        Spec def;
        def.title = LegacyDefaultTitle();
        return def;
    }();
    return s;
}

}  // namespace target
