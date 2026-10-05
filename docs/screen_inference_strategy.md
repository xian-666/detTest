# btHonorWorldDet - 截屏推理与调度策略

> **本文档基于上游（父）工程 `btHonorWorldDet` 整理。**
>
> 当前仓库已 fork 为 **`detTest`**：游戏层（`StrategyController` / `WeaponObserver` / `GamePadDev` / `HeroStrategy`）已全部移除，主循环中的 `ResetEffects()` / `SetOff()` 调用已替换为 `CaptureMetrics` 统计钩子。
> **仍然完全有效**：第 2 章的采图调度状态机、三级降级、黑屏自愈、窗口稳定性等待——这些正是 `detTest` 要量化的对象，不要改回去。
>
> 当前工程的实际说明见 → [detTest-guide.md](./detTest-guide.md)

> 本文档基于 `UnifiedScreenCapture.h/cpp`、`ONNXInference.h/cpp`、`GameInferenceSystem.h/cpp`、`btHonorWorldDet.cpp` 最新代码整理。

---

## 1. 整体调度架构

```
┌──────────────────────────────────────────────────────────────┐
│                    主循环 (btHonorWorldDet.cpp)               │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐  ┌──────────┐  │
│  │ 窗口检测  │→ │ 焦点判断  │→ │ 黑屏检测    │→ │ 截屏采集  │  │
│  └──────────┘  └──────────┘  └────────────┘  └──────────┘  │
│                                   ↓                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                  │
│  │ 策略处理  │← │ 推理结果  │← │ 异步推理  │                 │
│  │(Strategy)│  │(tryGet)  │  │(submit)  │                 │
│  └──────────┘  └──────────┘  └──────────┘                  │
│                                    ↓                        │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                  │
│  │ 性能统计  │  │ 手柄振动  │  │ 窗口显示  │                 │
│  │ 日志输出  │  │ 控制    │  │(OpenCV)  │                  │
│  └──────────┘  └──────────┘  └──────────┘                  │
└──────────────────────────────────────────────────────────────┘
```

每个主循环迭代的完整流程：

```
帧率限制器 → 窗口检测 → 焦点判断 → [失焦处理/焦点恢复] → 截屏采集
→ 黑屏/空帧检测 → [跳过推理] → ROI裁剪 → 异步推理提交 → 结果读取
→ 策略处理 → OpenCV显示 → waitKey(1)处理窗口消息
```

---

## 2. 截屏方法优先级与自动降级

### 2.1 固定优先级顺序

```
DXGI (优先级1) → WGC (优先级2) → GDI (优先级3)
```

初始化时按优先级依次尝试，成功后锁定为该方法。

```
Initialize():
  1. TryInitializeDXGI(hWnd)    → 成功则设为 DXGI
  2. TryInitializeWGC(hWnd)     → 成功则设为 WGC（仅当 WGC 友好且 build≥22000）
  3. TryInitializeGDI()         → 成功则设为 GDI
```

### 2.2 降级触发条件

`DowngradeMethod()` 在以下条件触发时被调用：

| 条件 | 阈值 | 行为 |
|------|------|------|
| 连续采集错误 | ≥5 次 (`MAX_CONSECUTIVE_ERRORS`) | 调用 `DowngradeMethod()` |
| `DXGI_ERROR_DEVICE_REMOVED` (0x887A0005) | 1 次 | 指数退避重置 → 失败后降级 |
| `DXGI_ERROR_ACCESS_LOST` | 1 次 | 立即降级 |
| `E_ACCESSDENIED` (0x80070005) | 1 次 | 立即降级 |
| `DXGI_ERROR_INVALID_CALL` | 1 次 | 立即降级 |

### 2.3 后台降级保护（前台稳定态）

当 `m_foregroundStable = true`（游戏窗口在前台）时，`DowngradeMethod()` 不会立刻切换到下一个方法，而是先保持当前方法重新初始化最多 3 次：

```cpp
int m_foregroundReinitAttempts = 0;
static constexpr int MAX_FOREGROUND_REINIT_ATTEMPTS = 3;
```

只有 3 次重新初始化全部失败后，才执行实际的方法降级。此机制防止游戏正常运行时因短暂抖动（如场景切换、加载）而误降级。

### 2.4 WGC 黄框规避机制

```cpp
bool m_wgcFriendly = true;  // 初始信任 WGC
```

每次 WGC 采集成功后，检测帧边缘（右下角边界区域）的 HSV 色彩范围：

| 判断条件 | 公式 |
|----------|------|
| 右边界黄色检测 | `meanH ∈ [25, 45]` 且 `meanS ≥ 80` |
| 下边界黄色检测 | `bottomMeanH ∈ [25, 45]` 且 `bottomMeanS ≥ 80` |

**任意条件满足 → `m_wgcFriendly = false`**，之后：
- 初始化时**跳过 WGC**（DXGI → GDI）
- 降级时**跳过 WGC**（DXGI → GDI）

### 2.5 完整降级流程图

```
当前方法连续5次采集失败
  ↓
m_foregroundStable = true ?
  ├── 是 → 保持当前方法重试初始化（最多3次）
  │         ├── 成功 → 恢复正常采集
  │         └── 失败 → 继续降级流程
  └── 否 → 直接降级
  ↓
当前方法是 DXGI ?
  ├── 是 → m_wgcFriendly = false ?
  │         ├── 是 → 跳过 WGC，降到 GDI
  │         └── 否 → 降到 WGC
  └── 否 → 当前方法是 WGC ?
            ├── 是 → 降到 GDI
            └── 否 → 已是 GDI，进入 Stopped 状态（5秒后自动恢复）
  ↓
新方法初始化成功 → 切换完成
新方法初始化失败 → 继续降级
```

### 2.6 截屏状态机

```
                Release()
                ┌─────┐
                ↓     │
    ┌───→ Idle ←──────┤
    │       │          │
    │       │ Initialize() 成功
    │       ↓          │
    │   Capturing      │
    │       │          │
    │       │ PauseCapture()
    │       ↓          │
    │   Paused ────────┤
    │       │ ResumeCapture()
    │       │ 或定时恢复
    │       │          │
    │       │ 错误发生  │
    │       ↓          │
    │   ExceptionWait  │
    │       │          │
    │       │ ShouldRetry() = true
    │       ↓          │
    │   Capturing (重试)│
    │       │          │
    │       │ 降级失败  │
    │       ↓          │
    │   Stopped ───────┤
    │       │ 5秒后自动恢复
    └───────┘
```

- **Idle**：未初始化或已释放
- **Capturing**：正常采集帧
- **Paused**：黑屏/加载时暂停采集
- **ExceptionWait**：错误后等待重试延迟
- **Stopped**：所有方法耗尽，5秒后自动恢复

### 2.7 关键时序常量

| 常量 | Debug | Release | 说明 |
|------|-------|---------|------|
| `RETRY_DELAY_MS` | 100ms | 300ms | 失败重试延迟 |
| `CIRCUIT_BREAKER_MS` | 800ms | 800ms | 单次采集超时 |
| `DXGI→WGC 切换等待` | 200ms | 800ms | 等待 GPU 驱动恢复 |
| `WGC→DXGI 切换等待` | 300ms | 100ms | 等待 COM 资源释放 |
| `WGC→DXGI (焦点恢复)` | - | 50ms | 加速恢复 |
| `STOPPED_RECOVERY_MS` | 5000ms | 5000ms | Stopped 状态自动恢复 |

---

## 3. 焦点切换处理策略

### 3.1 失焦时的处理流

```
isGameForeground = false 触发
  ↓
1. StrategyController::ResetEffects()  — 停止手柄振动（清空队列）
2. WeaponObserver::SetOff(0/1)         — 强制关闭左右马达
3. wasRealtime = false                  — 标记退出实时模式
4. system.pauseAsyncInference()         — 暂停推理线程
5. capture.Release()                    — 释放截屏资源
6. 显示 "Waiting for game focus..."
```

### 3.2 焦点恢复时的处理流

```
s_wasNotForeground = true 触发
  ↓
1. capture.ForceReinitialize(gameWnd)   — 强制重新初始化截屏
2. system.resumeAsyncInference()        — 恢复推理线程
3. s_inferenceFailures = 0              — 重置推理失败计数器
4. s_recoveryAttempts = 0               — 重置恢复尝试计数器
```

### 3.3 推理线程生命周期管理

```cpp
// ONNXInference.h
std::atomic<bool> m_asyncPaused{false};          // 暂停标志
std::atomic<bool> m_inferenceThreadRunning{false}; // 线程存活标志

// 暂停 — 线程保持运行，但跳过推理
pauseAsyncInference():
  m_asyncPaused = true

// 恢复
resumeAsyncInference():
  m_asyncPaused = false

// 完全停止 — 线程退出
stopAsyncInference():
  m_asyncPaused = false
  m_inferenceThreadRunning = false
  m_requestCV.notify_one()
  if (m_inferenceThread && m_inferenceThread->joinable())
      m_inferenceThread->join()
```

**暂停时的线程行为**：

```
inferenceThreadLoop():
  while (m_inferenceThreadRunning):
    wait(m_requestCV)  ← 等待新请求
    取出 request
    if (m_asyncPaused):
      清空队列中的旧请求
      m_hasAsyncResult = false
      sleep(10ms)
      continue  ← 回到 while 开头
    正常推理...
```

**关键设计**：暂停时**不销毁线程**，只跳过推理。恢复时只需设置 `m_asyncPaused = false`，线程立即开始处理新请求。避免了线程销毁/重建的竞态条件和资源竞争。

### 3.4 OpenCV 窗口焦点保护

```cpp
// 防止 OpenCV 窗口抢夺游戏前台焦点
SetWindowPos(cvWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
```

不使用 `SWP_NOACTIVATE` 会导致每次 `imshow()` 后 OpenCV 窗口被激活，游戏失焦触发 `pauseAsyncInference()` + `Release()`，游戏卡顿时反复抢焦→释放→重连，引发资源竞争崩溃。

---

## 4. 黑屏/空帧保护策略

### 4.1 主循环中的多层防护

#### 第1层：空帧检测

```cpp
if (!captureOk || preCheckFrame.empty()) {
    s_captureFailCount++;
    // 渐进退避
    int sleepMs = 10;
    if (s_captureFailCount > 100) sleepMs = 100;
    else if (s_captureFailCount > 30) sleepMs = 50;
    else if (s_captureFailCount > 10) sleepMs = 30;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    goto RENDER_FRAME;  // 跳过推理
}
```

#### 第2层：黑帧检测（主循环）

```cpp
if (meanBrightness < BLACK_SCREEN_BRIGHTNESS_THRESHOLD (2.0) && 尺寸 > 100px) {
    isAllBlack = true;
    s_blackScreenCount++;
    // 首次黑屏：停止振动
    if (s_blackScreenCount == 1) StrategyController::GetInstance().ResetEffects();
    // 暂停采集
    capture.PauseCapture();
    // 跳过推理
    goto RENDER_FRAME;
}
```

#### 第3层：帧校验（UnifiedScreenCapture 层）

```cpp
if (meanBrightness < BLACK_THRESHOLD (1.0)) → false  // 黑屏
if (meanBrightness > WHITE_THRESHOLD (250.0)) → false // 全白
```

#### 第4层：DXGI 管道保活

黑屏暂停期间，每 300 帧（约 5 秒 @60fps）`ResumeCapture()` 一次采集一帧再 `PauseCapture()`，防止 DXGI Desktop Duplication 的帧缓冲超时/stale：

```cpp
if (s_blackScreenCount % 300 == 0) {
    capture.ResumeCapture();
    cv::Mat keepAliveFrame;
    capture.CaptureFrame(keepAliveFrame);
    capture.PauseCapture();
}
```

### 4.2 黑屏退出防抖

```cpp
static constexpr int BLACK_SCREEN_EXIT_STABLE_FRAMES = 5;
```

需要连续 5 帧非黑屏才确认退出黑屏状态，防止画面亮度在临界值附近抖动时频繁切换状态。

### 4.3 黑屏时的振动保护

```cpp
// 进入黑屏
if (s_blackScreenCount == 1) {
    StrategyController::GetInstance().ResetEffects();  // 停止振动
    capture.PauseCapture();                            // 暂停采集
}

// 退出黑屏
if (s_blackScreenExitStableCount >= BLACK_SCREEN_EXIT_STABLE_FRAMES) {
    StrategyController::GetInstance().ResetEffects();  // 停止振动残留
    capture.ResumeCapture();                           // 恢复采集
}
```

### 4.4 采集失败时的死锁预防

焦点切回后如果采集因黑屏被暂停，形成死锁：
- `PauseCapture()` 后 `CaptureFrame()` 返回 false
- 但 `ResumeCapture()` 需要 `captureOk = true` 才能触发

解决方案：

```cpp
if (s_blackScreenCount > 0 && s_captureFailCount <= 3) {
    capture.ResumeCapture();
    s_blackScreenCount = 0;
    s_blackScreenExitStableCount = 0;
    s_prevBlackCount = 0;
}
```

---

## 5. 异步推理流水线

### 5.1 流水线架构

```
主循环 (主线程)                      后台推理线程
  │                                     │
  ├─ CaptureFrame()                     │
  ├─ submitAsyncClassification() ───────┼─→ ROI 裁剪
  │                                     ├─ 图像预处理
  │                                     ├─ ONNX 推理
  │                                     └─ 结果缓存
  ├─ tryGetAsyncClassificationResult() ←┼─ 读取缓存
  ├─ 策略处理 (StrategyController)       │
  ├─ 手柄振动控制                        │
  └─ imshow() 显示                      │
```

### 5.2 ROI 裁剪与提交

```cpp
GameInferenceSystem::submitAsyncClassification(capturedFrame):
  1. 获取游戏窗口信息（hwnd, size, scale）
  2. 生成动态 ROI（根据分辨率自适应）
  3. 从原始帧中裁剪 8 个 ROI 子图:
     LsRs, LB_1, RB_1, LT_1, RT_1, LB_2, LT_2, RT_2
  4. 调用 inference.submitAsyncClassification(roiMats)
  5. 设置 m_hasPendingInference = true

ONNXInference::submitAsyncClassification(roiMats):
  1. 如果线程未运行 → 启动新线程
  2. 丢弃队列中的旧任务（只保留最新一帧）
  3. 将新请求加入队列
  4. m_requestCV.notify_one() 唤醒推理线程
```

### 5.3 推理结果读取

```cpp
GameInferenceSystem::tryGetAsyncClassificationResult():
  1. if (!m_hasPendingInference) return false
  2. inference.tryGetAsyncClassificationResult(results, metrics)
  3. 成功后将 8 个 ROI 子图拼接成 MOSAIC 图（80×80 每个）
  4. 返回分类结果和拼图
```

### 5.4 暂停/恢复对流水线的影响

| 操作 | 主循环 `submitAsyncClassification()` | 后台推理线程 | 主循环 `tryGetAsyncClassificationResult()` |
|------|--------------------------------------|--------------|------------------------------------------|
| **pauseAsyncInference()** | 正常入队（但队列被清空） | 检测到 `m_asyncPaused=true`，清空队列，`m_hasAsyncResult=false`，sleep(10ms)，continue | `m_hasAsyncResult=false`，返回无结果 |
| **resumeAsyncInference()** | 正常入队 + notify_one | `m_asyncPaused=false`，开始处理新请求 | 等待推理完成 → `m_hasAsyncResult=true` → 返回结果 |

---

## 6. DXGI/WGC 设备丢失恢复策略

### 6.1 DXGI 设备丢失恢复

```
CaptureFrameDXGI():
  GetFrame() 失败
    ↓
  NeedsReinitialization() = true ?
    ├── 是 → 获取指数退避时间 (GetBackoffAndReset())
    │        sleep(退避时间)
    │        Reset() 尝试重置设备
    │          ├── 成功 → InitializeForWindow() → GetFrame()
    │          └── 失败 → 完全重新初始化 (StopCapture + InitializeForWindow)
    └── 否 → 返回 false（由外层判断是否降级）
```

### 6.2 WGC 设备丢失恢复

```
CaptureFrameWGC():
  GetFrame() 失败
    ↓
  NeedsReinitialization() = true ?
    ├── 是 → Reset() 尝试重置
    │          ├── 成功 → GetFrame()
    │          └── 失败 → 标记为降级候选
    └── 否 → 返回 false（由外层判断是否降级）
```

### 6.3 VEH 异常保护

```cpp
// 拦截以下 SEH 异常，防止进程崩溃：
0xC000027B  → STATUS_STOWED_EXCEPTION (WinRT fatal)
0x80070005  → E_ACCESSDENIED
0x887A0005  → DXGI_ERROR_DEVICE_REMOVED
0xC0000005  → ACCESS_VIOLATION

// 处理方式：
1. UnifiedScreenCapture::GetInstance().Release()  — 释放截屏资源
2. return EXCEPTION_CONTINUE_EXECUTION            — 继续执行

// 不拦截（让正常 try/catch 处理）：
0x40080201  → WinRT 状态传播异常
```

```cpp
// 全局未处理异常过滤器（最后防线）
LONG WINAPI GlobalExceptionFilter(PEXCEPTION_POINTERS pExceptionInfo) {
    UnifiedScreenCapture::GetInstance().Release();
    return EXCEPTION_CONTINUE_SEARCH;  // 允许生成 crash dump
}
```

---

## 7. Windows 版本兼容性

### 7.1 各方法系统要求

| 方法 | 最低要求 | 说明 |
|------|---------|------|
| DXGI | Windows 8+ | Desktop Duplication API，全局采集 |
| WGC | **Windows 11 (build ≥ 22000)** | 使用 `RtlGetVersion` 精确检测，绕过兼容层 |
| GDI | 所有版本 | 最终保底方案，性能最差 |

### 7.2 版本检测代码

```cpp
// 精确检测（RtlGetVersion 直接读取系统信息，不受兼容层影响）
bool UnifiedScreenCapture::IsRealWindows10BuildOrGreater(DWORD build) {
    HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionPtr fxPtr = (RtlGetVersionPtr)::GetProcAddress(hMod, "RtlGetVersion");
    if (fxPtr != nullptr) {
        RTL_OSVERSIONINFOW rovi = { sizeof(rovi) };
        if (fxPtr(&rovi) == 0) {
            if (rovi.dwMajorVersion > 10) return true;
            if (rovi.dwMajorVersion < 10) return false;
            return rovi.dwBuildNumber >= build;
        }
    }
    return false;
}

// 使用
bool bSupportWGC = IsRealWindows10BuildOrGreater(22000) && m_wgcFriendly;
```

### 7.3 启动时 WGC 强制模式

游戏窗口首次出现时，DXGI 可能尚未就绪（`ACCESS_LOST`）。提供 `InitializeWGCForced()` 方法启动时跳过 DXGI，直接使用 WGC：

```cpp
InitializeWGCForced():  // 启动时强制 WGC，跳过 DXGI
  1. TryInitializeWGC(hWnd) → 成功则设为 WGC
  2. 失败 → TryInitializeDXGI(hWnd) → 成功则设为 DXGI
```

---

## 8. 性能优化要点

### 8.1 动态帧率限制器

```cpp
// 目标 FPS 范围: 20-60
// 使用指数移动平均 (EMA) 平滑处理时间
s_smoothedProcessTimeUs = (s_smoothedProcessTimeUs * 7 + actualInterval) / 8;
// 根据上一帧实际间隔动态调整 sleep 时间
if (sinceLast < targetFrameTimeUs) {
    sleepUs = targetFrameTimeUs - sinceLast;
    if (sleepUs > 2000)  // 只 sleep 超过 2μs 的等待
        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs - 1000));
}
```

### 8.2 推理队列优化

```cpp
// 每次提交时丢弃旧任务，只保留最新一帧
ONNXInference::submitAsyncClassification():
  while (!m_requestQueue.empty()) {
      m_requestQueue.pop();
  }
```

防止后台线程堆积大量未处理的推理请求，保证推理线程始终处理最新帧。

### 8.3 推理失败渐进恢复

渐进式恢复机制（仅当持续失败时才触发）：

| 失败次数 | 操作 |
|----------|------|
| 15-25（全屏模式） | `ForceReinitialize()` |
| 25-35 | `ForceReinitialize()` 或重置窗口绑定 |
| 35-45 | 全屏且已重试2次 → `ResetInferenceSession()` |
| ≥45 | 重置所有计数器 + `ForceReinitialize()` |

---

## 9. 配置常量汇总

| 常量 | 所属文件 | 值 | 说明 |
|------|---------|-----|------|
| `MAX_CONSECUTIVE_ERRORS` | `UnifiedScreenCapture.h` | 5 | 降级触发阈值 |
| `RETRY_DELAY_MS` (Debug) | `UnifiedScreenCapture.h` | 100ms | 失败重试延迟 |
| `RETRY_DELAY_MS` (Release) | `UnifiedScreenCapture.h` | 300ms | 失败重试延迟 |
| `CIRCUIT_BREAKER_MS` | `UnifiedScreenCapture.h` | 800ms | 单次采集超时 |
| `STOPPED_RECOVERY_MS` | `UnifiedScreenCapture.h` | 5000ms | Stopped 状态自动恢复周期 |
| `BLACK_THRESHOLD` | `UnifiedScreenCapture.h` | 1.0 | 黑屏判定亮度阈值 |
| `WHITE_THRESHOLD` | `UnifiedScreenCapture.h` | 250.0 | 全白判定亮度阈值 |
| `MAX_FOREGROUND_REINIT_ATTEMPTS` | `UnifiedScreenCapture.h` | 3 | 前台稳定时的重试次数 |
| `WGC_YELLOW_FRAME_THRESHOLD` | `UnifiedScreenCapture.h` | 10 | WGC 黄框检测阈值 |
| `DXGI_UPGRADE_COOLDOWN` | `UnifiedScreenCapture.h` | 5 min | DXGI 升级冷却期 |
| `AUTO_UPGRADE_INTERVAL_FRAMES` | `UnifiedScreenCapture.h` | 300 帧 | WGC→DXGI 自动升级间隔 |
| `BLACK_SCREEN_BRIGHTNESS_THRESHOLD` | `btHonorWorldDet.cpp` | 2.0 | 主循环黑屏判定亮度阈值 |
| `BLACK_SCREEN_EXIT_STABLE_FRAMES` | `btHonorWorldDet.cpp` | 5 | 黑屏退出防抖帧数 |
| `kGameHwndStableFrames` | `btHonorWorldDet.cpp` | 15 | 窗口稳定帧数 |
| `RECOVERY_PROTECTION_PERIOD` | `btHonorWorldDet.cpp` | 500ms | 推理恢复保护期 |

---

## 10. 启动流程

```
main()
  │
  ├─ VEH 异常处理器安装
  ├─ 全局未处理异常过滤器安装
  ├─ timeBeginPeriod(1) — 高精度定时器
  │
  ├─ DEBUG_LOG_INIT()
  ├─ ShowConsole(true)
  ├─ screen_aspect::GetAspectCategory() — 屏幕比例检测
  │
  ├─ UnifiedScreenCapture::GetInstance() — 预创建截屏单例
  ├─ WeaponObserver::GetInstance()       — 预创建振动控制单例
  │
  ├─ ThreadPool(10).init()
  │   ├─ DetectDevice()  — HID 手柄检测
  │   └─ HeroNotify()    — 英雄策略通知
  ├─ WeaponObserver::StartCheckThread()   — 振动控制线程
  │
  ├─ GameInferenceSystem(modelPath, ...)  — 推理系统初始化
  ├─ HeroStrategyFactory                  — 策略工厂
  │
  └─ 进入主循环
```

---

## 11. 退出流程

```
while(true) 循环退出
  │
  ├─ g_ExitReader = true — 通知后台线程退出
  │
  ├─ WeaponObserver::SetOffDirect(0/1) — 直接写入 HID 关闭振动
  ├─ Sleep(50ms)
  ├─ WeaponObserver::StopCheckThread()  — 关闭振动控制线程
  ├─ Sleep(50ms)
  ├─ WeaponObserver::SetOffDirect(0/1) — 再次确认振动关闭
  ├─ GamePadDev::StopEnum()             — 停止设备枚举
  ├─ GamePadDev::Close()                — 关闭 HID 设备
  │
  ├─ cv::destroyAllWindows()            — 关闭 OpenCV 窗口
  │
  ├─ timeEndPeriod(1)                   — 恢复调度器精度
  └─ return 0