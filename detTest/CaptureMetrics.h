#pragma once
// CaptureMetrics.h
//
// Stability instrumentation for detTest.
//
// detTest exists to answer one question: "is screen capture healthy on a
// hybrid-GPU laptop, for these games, over a long run?" The capture subsystem
// already exposes method/state, but nothing aggregated them over time, so a
// degradation was only visible as a scrolling wall of DEBUG_LOG lines.
//
// This module collects per-frame timing plus the discrete failure modes that the
// capture layer documents for hybrid GPUs:
//   * stale buffer    -- same frame returned repeatedly (duplicate detection)
//   * black/white     -- iGPU shared-memory starvation, or lock-screen leakage
//   * method downgrade-- DXGI -> WGC -> GDI transitions
//   * focus flapping  -- focus lost/recovered around the GPU migration window
//
// Not thread safe by design: every entry point is called from the main loop
// only, which keeps the per-frame cost a few integer ops plus two ring pushes.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

class CaptureMetrics {
public:
    // Mirrors UnifiedScreenCapture enums so this header stays free of any
    // capture-layer dependency. Values intentionally match the capture enums'
    // declaration order so the caller can static_cast between them.
    enum class Method : int { DXGI = 0, WGC = 1, GDI = 2, Unknown = 3 };
    enum class CapState : int { Idle = 0, Capturing = 1, ExceptionWait = 2, Paused = 3, Stopped = 4, Unknown = 5 };

    static const char* MethodName(Method m);
    static const char* StateName(CapState s);

    struct Config {
        bool enable = true;
        bool hud = true;
        int reportIntervalSec = 5;
        bool csv = false;
        std::string csvDir = "metrics";
    };

    // One row per main-loop iteration.
    struct FrameSample {
        double intervalMs = 0.0;   // wall-clock gap since the previous iteration
        double captureMs = 0.0;    // time spent inside CaptureFrame()
        double inferenceMs = 0.0;  // model time reported by ONNXInference (0 = none)
        Method method = Method::Unknown;
        CapState state = CapState::Unknown;
        bool captureOk = false;
        bool black = false;
        bool duplicate = false;    // pixel content identical to the previous frame
        int width = 0;
        int height = 0;
    };

    static CaptureMetrics& GetInstance();

    // Opens the CSV file when cfg.csv is set. outputDir is the directory holding
    // the executable; the CSV lands in outputDir/csvDir.
    bool Init(const Config& cfg, const std::string& outputDir);
    bool IsEnabled() const { return m_cfg.enable; }
    void Shutdown();

    // Per-frame entry point. Also drives duplicate detection and the periodic
    // console report. `frame` must be the image returned by CaptureFrame().
    void OnFrame(const FrameSample& sample, const cv::Mat& frame);

    // Discrete events.
    void NoteMethodChange(Method from, Method to);
    void NoteStateChange(CapState from, CapState to);
    void NoteFocusLost();
    void NoteFocusRecovered();
    void NoteReinit(const char* kind);
    void NoteBlackEpisode();

    // Current frames-per-second over a short sliding window. Used by the HUD
    // in place of the old per-60-frames FPS counter.
    double RecentFps() const { return m_recentFps; }

    struct Snapshot {
        double elapsedSec = 0.0;
        std::uint64_t framesTotal = 0;
        std::uint64_t framesOk = 0;
        std::uint64_t framesFailed = 0;
        std::uint64_t framesBlack = 0;
        std::uint64_t framesDuplicate = 0;
        std::uint64_t maxFailStreak = 0;
        double failRatio = 0.0;

        double intervalP50 = 0.0, intervalP95 = 0.0, intervalP99 = 0.0, intervalMax = 0.0;
        double captureAvg = 0.0, captureP95 = 0.0, captureMax = 0.0;
        double inferenceAvg = 0.0, inferenceP95 = 0.0;

        Method method = Method::Unknown;
        CapState state = CapState::Unknown;

        std::uint64_t downgrades = 0, upgrades = 0, methodChanges = 0;
        std::uint64_t focusLostCount = 0, focusLostTotalMs = 0;
        std::uint64_t forceReinitCount = 0, softReinitCount = 0;
        std::uint64_t blackEpisodes = 0;
        std::uint64_t stateToPaused = 0, stateToStopped = 0;
    };

    Snapshot GetSnapshot() const;

    // Multi-line human readable block used by both the periodic report and the
    // run summary printed at exit.
    std::string FormatReport(const char* title) const;

    // Draws the live overlay onto `target`. No-op when the HUD is disabled.
    void DrawHud(cv::Mat& target) const;

private:
    CaptureMetrics() = default;

    void PushSample(double intervalMs, double captureMs, double inferenceMs);
    static double Percentile(std::vector<double> sorted, double pct);
    void AppendCsvRow(const FrameSample& s, std::uint64_t elapsedMs);
    bool DetectDuplicate(const cv::Mat& frame);

    Config m_cfg;
    bool m_initialized = false;

    std::chrono::steady_clock::time_point m_startTime{};
    std::chrono::steady_clock::time_point m_lastFrameTime{};

    // Ring buffers powering the percentiles. Capacity is a hard cap on samples;
    // at 60 fps one minute of history is retained.
    static constexpr size_t kRingCapacity = 3600;
    std::deque<double> m_intervalRing;
    std::deque<double> m_captureRing;
    std::deque<double> m_inferenceRing;

    // Sliding FPS window (~1 s at 60 fps).
    std::deque<std::chrono::steady_clock::time_point> m_frameTimes;
    double m_recentFps = 0.0;

    std::uint64_t m_framesTotal = 0;
    std::uint64_t m_framesOk = 0;
    std::uint64_t m_framesFailed = 0;
    std::uint64_t m_framesBlack = 0;
    std::uint64_t m_framesDuplicate = 0;
    std::uint64_t m_consecutiveFail = 0;
    std::uint64_t m_maxFailStreak = 0;

    int m_currentDupStreak = 0;
    static constexpr int kDupReportStreak = 30;  // consecutive identical frames worth reporting

    double m_captureSum = 0.0;
    double m_inferenceSum = 0.0;
    double m_intervalMax = 0.0;
    double m_captureMax = 0.0;

    Method m_method = Method::Unknown;
    CapState m_state = CapState::Unknown;

    std::uint64_t m_downgrades = 0, m_upgrades = 0, m_methodChanges = 0;
    std::uint64_t m_focusLostCount = 0, m_focusLostTotalMs = 0;
    std::chrono::steady_clock::time_point m_focusLostAt{};
    bool m_focusLost = false;
    std::uint64_t m_forceReinitCount = 0, m_softReinitCount = 0;
    std::uint64_t m_blackEpisodes = 0;
    std::uint64_t m_stateToPaused = 0, m_stateToStopped = 0;

    cv::Mat m_lastSignature;
    bool m_hasSignature = false;

    std::unique_ptr<std::ofstream> m_csv;
    std::string m_csvPath;
    std::chrono::steady_clock::time_point m_lastReportTime{};
};
