#pragma once
#include <chrono>
#include <functional>
#include <mutex>

/**
 * @brief 边缘检测器 - 用于检测状态变化（上升沿/下降沿）
 * @note 线程安全：所有公共方法都使用互斥锁保护
 */
class EdgeDetector {
private:
    bool m_prevState;
    int m_debounceMs;
    std::chrono::steady_clock::time_point m_lastEdgeTime;
    mutable std::mutex m_mutex;

public:
    /**
     * @brief 构造函数
     * @param initialState 初始状态
     * @param debounceMs 防抖时间（毫秒），0 表示不防抖
     */
    explicit EdgeDetector(bool initialState = false, int debounceMs = 0)
        : m_prevState(initialState),
          m_debounceMs(debounceMs),
          m_lastEdgeTime(std::chrono::steady_clock::now()) {}
    
    /**
     * @brief 检测上升沿（从不按到按下）
     * @param currentState 当前状态
     * @return 是否检测到上升沿
     */
    bool detectRisingEdge(bool currentState) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool result = (!m_prevState && currentState);
        m_prevState = currentState;
        return result;
    }
    
    /**
     * @brief 检测下降沿（从按下到不按）
     * @param currentState 当前状态
     * @return 是否检测到下降沿
     */
    bool detectFallingEdge(bool currentState) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool result = (m_prevState && !currentState);
        return result;
    }
    
    /**
     * @brief 检测变化（上升沿或下降沿）
     * @param currentState 当前状态
     * @return 是否检测到状态变化
     */
    bool detectChange(bool currentState) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool result = (m_prevState != currentState);
        return result;
    }
    
    /**
     * @brief 更新状态（必须在检测后调用）
     * @param currentState 当前状态
     */
    void updateState(bool currentState) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_prevState = currentState;
    }
    
    /**
     * @brief 同时检测上升沿和下降沿
     * @param currentState 当前状态
     * @param[out] risingEdge 是否检测到上升沿
     * @param[out] fallingEdge 是否检测到下降沿
     */
    void detectBothEdges(bool currentState, bool& risingEdge, bool& fallingEdge) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool rawRising = (!m_prevState && currentState);
        bool rawFalling = (m_prevState && !currentState);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastEdgeTime).count();
        bool withinDebounce = (m_debounceMs > 0 && elapsed < m_debounceMs);
        risingEdge = (rawRising && !withinDebounce);
        fallingEdge = (rawFalling && !withinDebounce);
        // Edge re-trigger guard: refresh m_lastEdgeTime whenever a RAW edge
        // occurs, even if it is suppressed by the debounce window. Otherwise
        // a rawRising inside the window that was suppressed would let the
        // next rawRising (after the original debounce expires) pass through
        // immediately, breaking the intended "one rising per debounce window"
        // contract used by all HeroStrategy triggers.
        if (rawRising || rawFalling) {
            m_lastEdgeTime = now;
        }
        m_prevState = currentState;
    }
    
    /**
     * @brief 重置检测器状态
     * @param newState 新的状态值
     * @note 线程安全：使用互斥锁保护
     */
    void reset(bool newState = false) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_prevState = newState;
        m_lastEdgeTime = std::chrono::steady_clock::now();
    }
    
    /**
     * @brief 获取上一次的状态
     * @return 上一次的状态值
     */
    bool getPreviousState() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_prevState;
    }
};

/**
 * @brief 带回调的边缘检测器
 */
class CallbackEdgeDetector {
private:
    EdgeDetector m_detector;
    std::function<void()> m_risingCallback;
    std::function<void()> m_fallingCallback;
    
public:
    /**
     * @brief 构造函数
     * @param initialState 初始状态
     * @param risingCallback 上升沿回调函数
     * @param fallingCallback 下降沿回调函数
     */
    explicit CallbackEdgeDetector(
        bool initialState = false,
        std::function<void()> risingCallback = nullptr,
        std::function<void()> fallingCallback = nullptr
    ) : m_detector(initialState), m_risingCallback(risingCallback), m_fallingCallback(fallingCallback) {}
    
    /**
     * @brief 更新状态并触发回调
     * @param currentState 当前状态
     */
    void update(bool currentState) {
        if (m_detector.detectRisingEdge(currentState) && m_risingCallback) {
            m_risingCallback();
        }
        
        if (m_detector.detectFallingEdge(currentState) && m_fallingCallback) {
            m_fallingCallback();
        }
    }
    
    /**
     * @brief 设置上升沿回调
     * @param callback 回调函数
     */
    void setRisingCallback(std::function<void()> callback) {
        m_risingCallback = callback;
    }
    
    /**
     * @brief 设置下降沿回调
     * @param callback 回调函数
     */
    void setFallingCallback(std::function<void()> callback) {
        m_fallingCallback = callback;
    }
    
    /**
     * @brief 重置检测器
     * @param newState 新的状态值
     */
    void reset(bool newState = false) {
        m_detector.reset(newState);
    }
};