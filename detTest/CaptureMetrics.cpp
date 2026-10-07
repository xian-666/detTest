// CaptureMetrics.cpp
//
// Implementation notes:
//   * Every counter is touched from the main loop only, so plain integers are
//     fine and no atomics/locks are needed. The atomic<> members inherited from
//     the wider project are intentionally avoided here.
//   * Percentiles are computed by sorting a copy of the ring buffer at report
//     time (every few seconds), keeping the per-frame path allocation free
//     after the buffer is warm.

#include "CaptureMetrics.h"

#include <windows.h>   // [Run identity] GetCurrentProcessId for per-run CSV isolation
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <numeric>
#include <sstream>

const char* CaptureMetrics::MethodName(Method m) {
    switch (m) {
        case Method::DXGI:    return "DXGI";
        case Method::WGC:     return "WGC";
        case Method::GDI:     return "GDI";
        default:              return "??";
    }
}

const char* CaptureMetrics::StateName(CapState s) {
    switch (s) {
        case CapState::Idle:           return "Idle";
        case CapState::Capturing:      return "Run";
        case CapState::ExceptionWait:  return "Wait";
        case CapState::Paused:         return "Pause";
        case CapState::Stopped:        return "Stopped";
        default:                       return "??";
    }
}

CaptureMetrics& CaptureMetrics::GetInstance() {
    static CaptureMetrics instance;
    return instance;
}

bool CaptureMetrics::Init(const Config& cfg, const std::string& outputDir) {
    m_cfg = cfg;
    if (!m_cfg.enable) {
        m_initialized = true;
        return true;
    }

    m_startTime = std::chrono::steady_clock::now();
    m_lastFrameTime = m_startTime;
    m_lastReportTime = m_startTime;

    if (m_cfg.csv) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = fs::path(outputDir) / m_cfg.csvDir;
        fs::create_directories(dir, ec);
        if (ec) {
            std::cerr << "[METRICS] Failed to create CSV dir: " << dir.string()
                      << " (" << ec.message() << ")\n";
        } else {
            auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm localTm{};
            localtime_s(&localTm, &now);
            // [Run identity] Timestamp has only second resolution; append PID so that two
            // launches within the same second (or concurrent dual-instance runs) never collide
            // on the same CSV file. Combined with ios::trunc below, every run owns a pristine
            // file: detTest_metrics_20261006_142345_pid12345.csv
            std::ostringstream name;
            name << "detTest_metrics_"
                 << std::put_time(&localTm, "%Y%m%d_%H%M%S")
                 << "_pid" << static_cast<unsigned long>(GetCurrentProcessId()) << ".csv";
            fs::path full = dir / name.str();

            m_csv = std::make_unique<std::ofstream>(full, std::ios::out | std::ios::trunc);
            if (m_csv->good()) {
                m_csvPath = full.string();
                *m_csv << "elapsed_ms,interval_ms,capture_ms,inference_ms,method,state,"
                          "capture_ok,black,duplicate,width,height\n";
                std::cout << "[METRICS] CSV: " << m_csvPath << "\n";
            } else {
                std::cerr << "[METRICS] Failed to open CSV: " << full.string() << "\n";
                m_csv.reset();
            }
        }
    }

    m_initialized = true;
    return true;
}

void CaptureMetrics::Shutdown() {
    if (m_csv) {
        m_csv->flush();
        m_csv.reset();
        std::cout << "[METRICS] CSV closed: " << m_csvPath << "\n";
    }
}

double CaptureMetrics::Percentile(std::vector<double> sorted, double pct) {
    if (sorted.empty()) return 0.0;
    size_t idx = static_cast<size_t>(std::ceil(pct / 100.0 * static_cast<double>(sorted.size())));
    if (idx == 0) idx = 1;
    if (idx > sorted.size()) idx = sorted.size();
    return sorted[idx - 1];
}

void CaptureMetrics::PushSample(double intervalMs, double captureMs, double inferenceMs) {
    auto push = [](std::deque<double>& ring, double v) {
        if (ring.size() >= kRingCapacity) ring.pop_front();
        ring.push_back(v);
    };
    push(m_intervalRing, intervalMs);
    push(m_captureRing, captureMs);
    if (inferenceMs > 0.0) push(m_inferenceRing, inferenceMs);

    if (intervalMs > m_intervalMax) m_intervalMax = intervalMs;
    if (captureMs > m_captureMax) m_captureMax = captureMs;
}

bool CaptureMetrics::DetectDuplicate(const cv::Mat& frame) {
    if (frame.empty()) return false;

    // 8x8 grayscale average downsample is enough to spot a frozen buffer while
    // costing well under 0.1 ms at 1920x1080.
    cv::Mat sig;
    cv::resize(frame, sig, cv::Size(8, 8), 0, 0, cv::INTER_AREA);
    if (sig.channels() == 3) cv::cvtColor(sig, sig, cv::COLOR_BGR2GRAY);

    bool same = false;
    if (m_hasSignature && m_lastSignature.size() == sig.size()) {
        double maxDiff = 0.0;
        cv::Mat diff;
        cv::absdiff(sig, m_lastSignature, diff);
        cv::minMaxLoc(diff, nullptr, &maxDiff);
        same = (maxDiff <= 1.0);  // tolerance covers dithering noise
    }

    sig.copyTo(m_lastSignature);
    m_hasSignature = true;
    return same;
}

void CaptureMetrics::OnFrame(const FrameSample& sample, const cv::Mat& frame) {
    if (!m_cfg.enable) return;

    auto now = std::chrono::steady_clock::now();
    double intervalMs = std::chrono::duration<double, std::milli>(now - m_lastFrameTime).count();
    m_lastFrameTime = now;

    // Sliding ~1 s FPS window.
    m_frameTimes.push_back(now);
    while (!m_frameTimes.empty() &&
           std::chrono::duration<double>(now - m_frameTimes.front()).count() > 1.0) {
        m_frameTimes.pop_front();
    }
    double windowSec = m_frameTimes.size() >= 2
        ? std::chrono::duration<double>(now - m_frameTimes.front()).count()
        : 0.0;
    m_recentFps = (windowSec > 0.0) ? static_cast<double>(m_frameTimes.size()) / windowSec : 0.0;

    m_framesTotal++;
    if (sample.captureOk) {
        m_framesOk++;
        m_consecutiveFail = 0;
    } else {
        m_framesFailed++;
        m_consecutiveFail++;
        if (m_consecutiveFail > m_maxFailStreak) m_maxFailStreak = m_consecutiveFail;
    }
    if (sample.black) m_framesBlack++;
    if (sample.duplicate) m_framesDuplicate++;

    m_captureSum += sample.captureMs;
    m_inferenceSum += sample.inferenceMs;

    m_method = sample.method;
    m_state = sample.state;

    PushSample(intervalMs, sample.captureMs, sample.inferenceMs);

    if (m_csv) {
        std::uint64_t elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime).count());
        AppendCsvRow(sample, elapsedMs);
    }

    // The duplicate flag comes either from the caller (already computed by the
    // black-frame path) or from our own signature check on a healthy frame.
    if (!sample.duplicate && sample.captureOk && !sample.black && !frame.empty()) {
        bool dup = DetectDuplicate(frame);
        if (dup) {
            m_framesDuplicate++;
            m_currentDupStreak++;
            if (m_currentDupStreak == kDupReportStreak) {
                std::cout << "[METRICS] WARNING: " << kDupReportStreak
                          << " consecutive identical frames (possible stale buffer)\n";
            }
        } else {
            m_currentDupStreak = 0;
        }
    } else if (!sample.duplicate) {
        m_currentDupStreak = 0;
        DetectDuplicate(frame);  // keep the signature warm anyway
    } else {
        m_hasSignature = false;  // caller flagged it: resync the signature
    }

    if (m_cfg.reportIntervalSec > 0) {
        double sinceSec = std::chrono::duration<double>(now - m_lastReportTime).count();
        if (sinceSec >= m_cfg.reportIntervalSec) {
            m_lastReportTime = now;
            std::cout << FormatReport("[METRICS]") << "\n";
        }
    }
}

void CaptureMetrics::AppendCsvRow(const FrameSample& s, std::uint64_t elapsedMs) {
    if (!m_csv || !m_csv->good()) return;
    *m_csv << elapsedMs << ','
           << std::fixed << std::setprecision(3)
           << s.intervalMs << ',' << s.captureMs << ',' << s.inferenceMs << ','
           << MethodName(s.method) << ',' << StateName(s.state) << ','
           << (s.captureOk ? 1 : 0) << ','
           << (s.black ? 1 : 0) << ','
           << (s.duplicate ? 1 : 0) << ','
           << s.width << ',' << s.height << '\n';
}

void CaptureMetrics::NoteMethodChange(Method from, Method to) {
    if (!m_cfg.enable || from == to) return;
    // Declaration order is the priority order (DXGI < WGC < GDI), so a larger
    // index means we fell back to a weaker backend.
    m_methodChanges++;
    if (static_cast<int>(to) > static_cast<int>(from)) m_downgrades++;
    else m_upgrades++;

    std::cout << "[METRICS] Capture method changed: " << MethodName(from)
              << " -> " << MethodName(to)
              << (static_cast<int>(to) > static_cast<int>(from) ? " (downgrade)" : " (upgrade)")
              << "\n";
}

void CaptureMetrics::NoteStateChange(CapState from, CapState to) {
    if (!m_cfg.enable || from == to) return;
    if (to == CapState::Paused) m_stateToPaused++;
    if (to == CapState::Stopped) m_stateToStopped++;
}

void CaptureMetrics::NoteFocusLost() {
    if (!m_cfg.enable || m_focusLost) return;
    m_focusLost = true;
    m_focusLostAt = std::chrono::steady_clock::now();
    m_focusLostCount++;
}

void CaptureMetrics::NoteFocusRecovered() {
    if (!m_cfg.enable || !m_focusLost) return;
    m_focusLost = false;
    auto lostMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_focusLostAt).count();
    m_focusLostTotalMs += static_cast<std::uint64_t>(lostMs);
    std::cout << "[METRICS] Focus recovered after " << lostMs << " ms\n";
}

void CaptureMetrics::NoteReinit(const char* kind) {
    if (!m_cfg.enable) return;
    std::string k = kind ? kind : "?";
    if (k == "soft") m_softReinitCount++;
    else m_forceReinitCount++;
}

void CaptureMetrics::NoteBlackEpisode() {
    if (!m_cfg.enable) return;
    m_blackEpisodes++;
}

CaptureMetrics::Snapshot CaptureMetrics::GetSnapshot() const {
    Snapshot s;
    s.elapsedSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_startTime).count();

    s.framesTotal = m_framesTotal;
    s.framesOk = m_framesOk;
    s.framesFailed = m_framesFailed;
    s.framesBlack = m_framesBlack;
    s.framesDuplicate = m_framesDuplicate;
    s.maxFailStreak = m_maxFailStreak;
    s.failRatio = (m_framesTotal > 0)
        ? (100.0 * static_cast<double>(m_framesFailed) / static_cast<double>(m_framesTotal))
        : 0.0;

    if (!m_intervalRing.empty()) {
        std::vector<double> v(m_intervalRing.begin(), m_intervalRing.end());
        std::sort(v.begin(), v.end());
        s.intervalP50 = Percentile(v, 50);
        s.intervalP95 = Percentile(v, 95);
        s.intervalP99 = Percentile(v, 99);
        s.intervalMax = m_intervalMax;
    }
    if (!m_captureRing.empty()) {
        std::vector<double> v(m_captureRing.begin(), m_captureRing.end());
        std::sort(v.begin(), v.end());
        s.captureAvg = m_captureSum / static_cast<double>(m_framesTotal > 0 ? m_framesTotal : 1);
        s.captureP95 = Percentile(v, 95);
        s.captureMax = m_captureMax;
    }
    if (!m_inferenceRing.empty()) {
        std::vector<double> v(m_inferenceRing.begin(), m_inferenceRing.end());
        std::sort(v.begin(), v.end());
        s.inferenceAvg = m_inferenceSum / static_cast<double>(m_framesTotal > 0 ? m_framesTotal : 1);
        s.inferenceP95 = Percentile(v, 95);
    }

    s.method = m_method;
    s.state = m_state;
    s.downgrades = m_downgrades;
    s.upgrades = m_upgrades;
    s.methodChanges = m_methodChanges;
    s.focusLostCount = m_focusLostCount;
    s.focusLostTotalMs = m_focusLostTotalMs;
    s.forceReinitCount = m_forceReinitCount;
    s.softReinitCount = m_softReinitCount;
    s.blackEpisodes = m_blackEpisodes;
    s.stateToPaused = m_stateToPaused;
    s.stateToStopped = m_stateToStopped;
    return s;
}

std::string CaptureMetrics::FormatReport(const char* title) const {
    Snapshot s = GetSnapshot();
    std::ostringstream os;
    os << std::fixed << std::setprecision(1);
    os << title << " t=" << s.elapsedSec << "s"
       << " fps=" << m_recentFps
       << " | method=" << MethodName(s.method)
       << " state=" << StateName(s.state)
       << "\n  interval(ms) p50=" << s.intervalP50 << " p95=" << s.intervalP95
       << " p99=" << s.intervalP99 << " max=" << s.intervalMax
       << " | capture(ms) avg=" << s.captureAvg << " p95=" << s.captureP95
       << " max=" << s.captureMax
       << " | inference(ms) avg=" << s.inferenceAvg << " p95=" << s.inferenceP95
       << "\n  frames=" << s.framesTotal << " ok=" << s.framesOk
       << " fail=" << s.framesFailed << " (" << s.failRatio << "%)"
       << " black=" << s.framesBlack << " dup=" << s.framesDuplicate
       << " maxFailStreak=" << s.maxFailStreak
       << "\n  events: downgrade=" << s.downgrades << " upgrade=" << s.upgrades
       << " pause=" << s.stateToPaused << " stopped=" << s.stateToStopped
       << " blackEpisodes=" << s.blackEpisodes
       << " reinit(force=" << s.forceReinitCount << " soft=" << s.softReinitCount << ")"
       << " focusLost=" << s.focusLostCount << "/" << s.focusLostTotalMs << "ms";
    return os.str();
}

void CaptureMetrics::DrawHud(cv::Mat& target) const {
    if (!m_cfg.enable || !m_cfg.hud || target.empty()) return;

    Snapshot s = GetSnapshot();
    char buf[256];
    const double scale = 0.5;
    const int thick = 1;
    const cv::Scalar good(0, 255, 0);
    const cv::Scalar warn(0, 255, 255);

    // Line 1 (only): identity + liveness. The metrics HUD used to stack 5 lines from the
    // top-left and occlude the sampled image / ROI display area, so it is trimmed to this
    // single summary line. The full timing / frame-health / recovery-event stats remain
    // available in the periodic [METRICS] report and the [STATUS] heartbeat log.
    auto headColor = (s.framesTotal > 0 && s.method == Method::GDI) ? warn : good;
    std::snprintf(buf, sizeof(buf), "FPS %5.1f | %s | %s | %dx%d",
                  m_recentFps, MethodName(s.method), StateName(s.state),
                  target.cols, target.rows);
    cv::putText(target, buf, cv::Point(10, 20), cv::FONT_HERSHEY_SIMPLEX, scale, headColor, thick);
}
