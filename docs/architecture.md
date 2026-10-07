# btHonorWorldDet 架构设计文档

> **本文档描述的是上游（父）工程 `btHonorWorldDet` 的整体架构。**
>
> 当前仓库已 fork 为 **`detTest`** —— 一个只保留「采图 + 图像推理」、**剥离全部游戏/手柄逻辑**的采集稳定性测试台。
> 其中游戏层（`GamePadDev` / `WeaponObserver` / `StrategyController` / `HeroStrategy`）**已不再存在于本仓库**，保留原文仅供理解 `UnifiedScreenCapture` 子系统的设计背景。
>
> **仍然完全有效**：第 2 章之后的采图子系统、三级降级状态机、Hybrid GPU 探测与自愈逻辑——这些正是 `detTest` 要测试的对象，不要改回去。
>
> 当前工程的实际说明见 → [detTest-guide.md](./detTest-guide.md)

## 1. 系统概述

btHonorWorldDet 是一个基于计算机视觉的王者荣耀世界游戏手柄振动控制系统。系统通过截取游戏窗口画面、识别 UI 元素状态（技能按键状态）、匹配英雄策略，向手柄 HID 设备发送扳机振动指令，实现与游戏画面同步的扳机反馈。

### 核心技术栈

| 层次 | 技术选型 | 说明 |
|------|----------|------|
| 屏幕截取 | DXGI → WGC → GDI | 按优先级自动降级，含增强兼容路径 |
| 模型推理 | ONNX Runtime + DirectML | GPU 加速分类模型，批处理推理 |
| 手柄通信 | HID Raw Input (单通道) | XInput 已完全移除，仅 HID 读写分离 |
| 振动控制 | KP70 HID 协议 | 独立 Check 线程 + 保活机制 |
| 英雄策略 | Strategy 模式 | HeroStrategy 插件体系 |

---

## 2. 模块层次架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                         main (btHonorWorldDet.cpp)                  │
│  • 帧率控制 (60FPS limiter)                                          │
│  • 游戏窗口前台检测 (GetForegroundWindow / GA_ROOT)                  │
│  • 截取/推理/显示 主循环                                              │
│  • OpenCV 可视化窗口 (HWND_NOTOPMOST + SWP_NOACTIVATE)              │
└─────────────────────────────────────────────────────────────────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    ▼              ▼              ▼
         ┌────────────────┐ ┌─────────────┐ ┌──────────────────┐
         │  Screen        │ │  Game       │ │  Input           │
         │  Capture       │ │  Inference   │ │  Controller      │
         │  (Unified)     │ │  System      │ │  (HID only)      │
         └────────┬───────┘ └──────┬──────┘ └────────┬─────────┘
                  │                │                  │
         ┌────────▼────────┐       │         ┌────────▼──────────┐
         │ DXGI / WGC / GDI│       │         │  WeaponObserver   │
         │ + EnhancedRT    │       │         │  (命令队列 + 1ms   │
         └─────────────────┘       │         │   Check 线程)     │
                                  │         │         │          │
                           ┌──────▼──────┐  │         ▼          │
                           │  ROI        │  │ ┌────────────────┐  │
                           │  Adaptation │  │ │ GamePadDev     │  │
                           │  Config    │◄─┘ │ (HID 设备管理)  │  │
                           │  (JSON v2.3)│    │ 读: Usage 0x01  │  │
                           └─────────────┘    │ 写: Usage 0xFF  │  │
                                  ▲           └───────┬────────┘  │
                           ┌──────┴──────┐             │           │
                           │  ONNX       │             │           │
                           │  Inference  │             │           │
                           │  (DirectML) │             │           │
                           │  EXTENDED   │             │           │
                           └─────────────┘             │           │
                                                       ▼           │
                                            ┌────────────────────┐  │
                                            │  StrategyController│  │
                                            │  • 识别英雄        │  │
                                            │  • 状态稳定滤波    │  │
                                            │  • 振动时序控制    │  │
                                            └─────────┬──────────┘  │
                                                      │              │
                                            ┌─────────▼──────────┐  │
                                            │  HeroStrategy     │  │
                                            │  (13 个英雄插件)   │  │
                                            │  SunBin / Kai /   │◄─┘
                                            │  LongYa / ...     │
                                            └────────────────────┘
```

---

## 3. 核心模块详解

### 3.1 屏幕截取子系统 (Screen/)

#### 截取优先级链

```
DXGI (Desktop Duplication API)
    ↓ 失败 5 次或 ACCESS_LOST
WGC  (Windows Graphics Capture)
    ↓ 失败 5 次或 WinRT 异常
GDI  (传统 GDI BitBlt)  ← 最低优先级
```

#### 兼容分流架构 (v2.4+)

Initialize() 入口新增无损分流判断：

```
Initialize(HWND, bool)
    │
    ├── compat_mode=force_legacy ──► InitializeLegacy() [原逻辑逐行平移]
    │
    ├── compat_mode=force_enhanced ──► InitializeEnhanced() [五处增强措施]
    │
    └── compat_mode=auto (默认)
           │
           ├── GpuEnvironmentProbe::Detect() [纯只读: WMI/SetupAPI/注册表]
           │      → m_isHybridGPU = (isHybridWithMux || isAmdApuHybrid)
           │
           ├── DetectDisplayGPUType(hWnd) [目标窗口所在显示器由哪块 GPU 驱动]
           │      ├── 显示器接 iGPU (Optimus 内屏) → InitializeLegacy()  ← 常规混显本走这条
           │      ├── 显示器接 dGPU (MUX 直连)     → InitializeEnhanced() [含 EnhancedCaptureRuntime]
           │      └── 未知(-1) → 回退 NeedsEnhancedCompat(env) 判定
           │
           └── 说明：不再按「MUX/HDR/VRR/Intel Arc」等高风险特征把 iGPU 输出升级到 Enhanced
```

> 曾有「iGPU 输出也按高风险特征升级到 Enhanced」的版本，实测对 NGR-Win64（无边框）等 iGPU-DDA 本就稳定的游戏反而引发 WGC `DEVICE_REMOVED` 抖动回归，**已回退为纯按显示器归属选路**。

#### compat_mode 配置

通过环境变量 `BT_CAPTURE_COMPAT_MODE` 控制，无需配置文件：

| 值 | 行为 |
|------|------|
| `auto` (默认) | 自动探针判定，高风险走增强路径 |
| `force_enhanced` | 强制增强路径，跳过探针 |
| `force_legacy` | 强制遗留路径，等价于修改前行为 |

#### 增强路径五处措施 (EnhancedCaptureRuntime)

1. **HDR / 像素格式安全转换**：运行时检测 cv::Mat 像素格式，非 BGR8 时自动转换
2. **DDA 帧活性检测**：内容哈希排除 UI 静态区域掩码，连续 >=3 帧相同判定假死，主动重建会话
3. **显示模式变更主动监听**：每 60 帧轻量轮询 EnumDisplaySettings，变更即重建
4. **跨适配器纹理安全拷贝**：检测到 iGPU/dGPU 分离时，经 Staging Texture + 共享句柄传输
5. **GDI 智能降频与补偿**：动态采集间隔，低帧率置信度标记供下游插值补偿

#### 环境探针 (GpuEnvironmentProbe)

纯只读检测，仅使用以下 API（严禁 DXGI/D3D/WGC/GDI）：
- SetupAPI 枚举显示适配器
- EnumDisplayDevices 确定主屏所在适配器
- 注册表读取 HDR/VRR 状态
- GetDpiForMonitor 读取 DPI 缩放

#### 状态机

```
Idle ──[Initialize]──► Capturing
                         │                    ▲
                         │ 错误累计≥5次        │
                         ▼                    │
                    ExceptionWait            │
                         │                    │
              ┌──────────┴──────────┐           │
              ▼                     ▼           │
         Capturing            Paused           │
    (游戏加载/CG黑屏时暂停)     (亮度<1.0)       │
              │                     │           │
              └─────────────────────┘           │
                         │                    │
                    [ResumeCapture]            │
                         │                    │
                         ▼                    │
                      Capturing ───────────────┘
```

#### 混合显卡优化

- **DXGI 自动绑核显**：`InitializeForWindow` 默认按 `MonitorFromWindow` 选中「驱动目标显示器的适配器」建桌面复制——Optimus 内屏由 iGPU 驱动即**自动绑核显**，iGPU 的 DDA 经 DWM 合成帧拿到 dGPU 渲染内容。`TryInitializeOnDiscreteGPU` / `SetHybridGPUMode`（dGPU-first）已成为 **dead code**（Optimus 下独显无输出，必失败）。
- **`BT_CAPTURE_PIN_IGPU=1`**（环境变量）：显式把 D3D 设备 + 复制硬绑到 iGPU/APU，绕过 monitor-match；失败软回退默认路径。用于 MUX/迁移可能挑错卡的机器。
- **黑/白/空帧不计错误**：`ValidateFrame` 判否的帧不累计 `m_consecutiveErrors`，因此既不降级也不触发 DXGI 重建——WGC「半失效停走」与 FC6「MPO 陈旧帧卡死」都无法靠降级/重建自愈（详见 `wgc-blackframe-flapping-diagnosis-and-hardening.md`）。
- **SoftReinitialize**：焦点恢复时优先保留现有 capture session，采集 warmup 帧自我验证，避免破坏性 Release+Initialize
- **长切出改进**：动态 warmup 帧数（<10s=30帧, 10-120s=60帧, >120s=120帧），多阶段尝试（最多 3 轮），窗口大小变化检测
- **IsFrameAlive**：同时检测纯黑/纯白 + 帧间差异（防止 stale buffer）
- **WGC 黄框规避**：HSV 色相检测 + Hysteresis（连续 K 帧才判定），避免焦点切换误判
- **DXGI 升级黑名单**：全屏独占失败后 5 分钟冷却期，避免每 5 秒卡顿

#### 防崩溃机制

- `AddVectoredExceptionHandler`：捕获 GraphicsCapture.dll 的 WinRT 异常 (0xC000027B)
- `SetUnhandledExceptionFilter`：全局异常过滤器，记录崩溃阶段 (`g_crashStage`)
- 异常后调用 `UnifiedScreenCapture::GetInstance().Release()` 释放 GPU 资源
- `abort_breadcrumb.txt`：记录崩溃前的最后阶段用于诊断

---

### 3.2 推理子系统 (GameInferenceSystem / ONNXInference)

#### 两类模型

| 模型 | 用途 | 输入尺寸 | 推理路径 |
|------|------|----------|----------|
| 分类模型 (HONOR.onnx) | ROI 图像分类（8 个按钮状态） | 96×96 | DirectML GPU 批量推理 |
| 检测模型 (可选，nullptr 时禁用) | 子弹数检测 | 64×64 | DirectML GPU 推理 |

#### 屏幕比例检测 (ScreenAspectDetector.hpp)

**问题**：游戏渲染分辨率可能与物理屏幕分辨率不一致（如 21:9 带鱼屏渲染 1920×1080），导致 ROI 绝对坐标偏移。

**检测优先级**：
```
1. GetGameMonitorResolution()     ← 游戏窗口所在显示器的物理分辨率（最可靠）
2. DetectPhysicalResolution()     ← 读 GameUserSettings.ini 的 ResolutionSizeX/Y
3. GetDesktopWindow() rect       ← 桌面窗口尺寸（多显示器不可靠）
4. 硬编码 16:9 (1920×1080)       ← 最终兜底
```

**宽高比分类**：21:9 (容差 0.15) → 16:9 (0.05) → 16:10 (0.05) → 4:3 → 5:4

#### ROI 动态生成 (GameInferenceSystem.cpp)

```cpp
// 流程
gameW/gameH (窗口客户区) ──► generateDynamicROIs()
                                     │
                        ┌────────────▼────────────┐
                        │ roi::GetAdaptiveRect()   │
                        │   阶段 1: 16:9 基准变换   │
                        │     x' = round(x * W / 1920) │
                        │     y' = round(y * H / 1080) │
                        │   阶段 2: 最小值裁剪       │
                        │     w' = max(w', min(w, W - x'))
                        │   阶段 3: 居中微调          │
                        │     x'' = x' + xBias        │
                        │   阶段 4: JSON 残差修正     │
                        │     dx/dy/dw/dh 加权叠加   │
                        └────────────────────────────┘
```

#### ROI 自适应配置 (ROIAdaptationConfig, roi_adaptation.json)

v2.3 混合模式 (Hybrid Mode) 核心逻辑：当 specificResolutions 中的条目 aspectGroup 与物理屏幕 aspectCategory 不匹配时（如 1920×1200 注册为 16:10 但物理屏幕是 21:9）：

- **几何信息**（ROI 坐标 dx/dy/dw/dh）：**保留** specificResolutions 中的值
- **图像处理参数**（sizeScale/xBias/yBias）：**丢弃** specificResolutions 中的值，改用匹配物理屏幕 aspectCategory 的 aspectProfile 参数

```
Lookup 顺序 (GetResidualWithFallback):
  1. exact (W, H, aspectCategory) in specificResolutions
       └─► 若 aspectGroup 不匹配 → 几何保留 + 图像参数用 aspectProfile (混合模式)
  2. aspect profile 匹配 (aspectCategory 或 targetAspect)
       └─► 若有 canonicalResiduals + scaleLaw → 按比例尺推导
  3. zero residual (回退到 canonicalTargets)
```

#### 推理执行 (ONNXInference.cpp)

```
输入图像 → LetterBox 预处理 → resize(INTER_LINEAR) → 归一化 [0,1]
                                                    → 填充 tensor (预分配缓冲区)
                                                    → ONNX Session 推理 (DirectML EP)
                                                    → Softmax 后处理
                                                    → 返回 {class_id, confidence, class_name}
```

- **批处理**：`runBatchClassificationWithMetrics` 一次性处理 8 个 ROI，避免逐个推理的反复 kernel 启动开销
- **预分配 tensor**：`m_batchTensorValues` 每帧复用，避免堆分配
- **单线程执行**：`SetIntraOpNumThreads(1)` + `ORT_SEQUENTIAL` 模式
- **图优化等级**：`ORT_ENABLE_EXTENDED`（跨 GPU 架构数值稳定，避免分类漂移）
- **异步流水线**：后台推理线程 + 队列，支持 `submitAsyncClassification` / `tryGetAsyncClassificationResult`
- **焦点恢复优化**：`pauseAsyncInference` / `resumeAsyncInference`，暂停时跳过推理但不销毁线程

---

### 3.3 手柄输入子系统 (GamePadDev / WeaponObserver)

#### 单通道 HID 输入读取 (XInput 已完全移除)

```
主循环通过全局变量读取 HID 状态
    │
    └── HID Raw Input (独立线程 33ms 固定轮询)
           └─► HidReadLoop 线程读取 UsagePage=0x01 / Usage=0x05 设备
           └─► 解析扳机值 → g_LeftTriggerValue / g_RightTriggerValue
           └─► 解析肩键状态 → g_lbPressed / g_rbPressed
           └─► 记录上升沿时间戳 → g_lastLbRisingMs / g_lastRbRisingMs
```

**读写分离设计**：
- 读设备：`UsagePage=0x01, Usage=0x05`（独立 `m_ptrHidReadDev`）
- 写设备：`UsagePage=0xFF, Usage=0x03`（独立 `m_ptrHidDev`）
- 独立 `m_readMutex` / `m_writeMutex` 互斥锁

#### 命令队列与 Check 线程

```
WeaponObserver 命令入队 (生产者: 主循环 / HeroStrategy)
    │
    └─► moodycamel::BlockingReaderWriterQueue<TriggerCommand>
              │
              ├── m_queueLeftTrigger   (LT)
              └── m_queueRightTrigger  (RT)

独立 Check 线程 (1ms 轮询，消费者)
    │
    ├── DequeueCommand()
    ├── 与 m_lastLeftCmd / m_lastRightCmd 去重 (2s 窗口)
    ├── 肩键保活: 每 3s 重复发送非零命令 (kActiveKeepAliveMs)
    ├── 空闲保活: 每 3s 发送历史命令防止超时 (kShoulderIdleKeepAliveIntervalMs)
    └── GamePadDev::WriteReport() → HID 硬件
```

#### 设备白名单 (GamePadDev.h)

| PID (HEX) | 设备类型 | 协议 |
|-----------|----------|------|
| 512F | KP70 | 有线 KP70 HID |
| 5130 | KP70_Dongle | 无线接收器 |
| 515B | KP40D | 有线 KP40D |
| 515C | KP40D_Dongle | 无线接收器 |
| 515F | KP70B | 有线 KP70B |
| 5160 | KP70B_Dongle | 无线接收器 |
| 516D | KP70A1 | 有线 KP70A1 |
| 516E | KP70A1_Dongle | 无线接收器 |
| 5173 | KP70C | 有线 KP70C |
| 5174 | KP70C_Dongle | 无线接收器 |
| 518A | KP70C1 | 有线 KP70C1 |
| 518B | KP70C1_Dongle | 无线接收器 |
| 518C | KP70C2 | 有线 KP70C2 |
| 518D | KP70C2_Dongle | 无线接收器 |

#### 肩键控制协议

- `SendShoulderEffect`：标准肩键效果（Quick/Soft/Heavy），使用 0x76 协议
- `SendCustomStepEffect`：自定义多步震动效果，支持 LB/RB 各 10 步强度配置
- 肩键保活机制：独立 LR 保活缓存通道，每 3s 维持非零命令
- 空闲保活：独立空闲保活缓存，固件超时 5s 前重发

---

### 3.4 策略控制子系统 (StrategyController / HeroStrategy)

#### 分类结果 → 扳机状态映射

```
8 个 ROI 的 ClassificationResult[]
      │
      ├── ROI0 LsRs     → 英雄识别
      ├── ROI1 LB_1     → LT_1 默认触发
      ├── ROI2 RB_1     → RT_1 默认触发
      ├── ROI3 LT_1     → 预留（普通模式）
      ├── ROI4 RT_1     → 预留（普通模式）
      ├── ROI5 LB_2     → 二次触发状态
      ├── ROI6 LT_2     → 二次触发状态
      └── ROI7 RT_2     → 二次触发状态

TriggerState: INACTIVE(0) → DEFAULT(1) → SECONDARY(2)
      └─► 去抖滤波后生效（激活阈值 1 帧，关闭阈值 8 帧）
```

#### 英雄识别与预设

```
LsRs ROI 分类结果
      │
      └──► m_hero StableCount ≥ 3 → 确认英雄
                  │
                  ├── Preset 1 (LB_1=待命 / RT_1=施法)
                  └── Preset 2 (LT_2=待命 / RT_2=施法)
                             ↑
                     StableCount ≥ 6 切换预设
```

#### 13 个英雄策略插件

| 英雄 | 策略特点 |
|------|----------|
| SunBin (孙膑) | 二技能持续，待命/施法态 |
| Kai (凯) | 大招，待命/施法态 |
| LongYa (龙牙) | 二次触发 |
| MengYa (蒙讶) | 技能状态管理 |
| ColdSpring (冷春) | 特殊技能序列 |
| DongfangYao (东方曜) | 持续技能 |
| ChildofOriginStream (元流之子) | 双形态切换 |
| Jialuo (伽罗) | 远程状态 |
| LubanDaShi (鲁班大师) | 技能连招 |
| SnowRabbit (雪兔) | 特殊机制 |
| SunShangxian (孙尚香) | 翻滚状态 |
| WangZhaojun (王昭君) | 冰冻控制 |
| HuaMulan (花木兰) | 重剑/轻剑双形态切换，含 LB 物理按键边缘检测 |

每个策略实现 `IHeroStrategy::applyEffects(state, preset)` 接口，返回是否接管振动逻辑。

#### 振动控制与保活机制

```
主循环 (分类推理)               Check 线程 (1ms 轮询)
        │                              │
        ├──► SetTrigger() 入队 ──────►│
        │    (TriggerCommand 队列)      │
        │                              ├──► Dequeue + 去重
        │                              ├──► WriteReport() → HID
        │                              └──► KeepAlive 每 3s 发送
        │                              │
        ▼                              ▼
 HeroStrategy::applyEffects     实际振动发送 (时序保护)
```

**时序保证**：`ResetEffects()` 必须先于 `SetOff()` 调用——`ForceReset()` 清空队列并重置 Check 线程缓存，确保 Off 指令不被丢弃。

**安全关闭序列**（解决手柄重启后异常振动）：
1. `SetOffDirect(0/1)` ← 直接写 HID，绕过队列
2. `StopCheckThread()`
3. `sleep(50ms)` ← 等待 pending HID 写完成
4. `SetOffDirect(0/1)` ← 再次确认 Off
5. `StopEnum()` ← 信号 ThrdFunc 停止
6. `Close()` ← 关闭 HID 设备

---

### 3.5 主循环流程 (btHonorWorldDet.cpp main)

```
┌──────────────────────────────────────────────────────────────┐
│  启动阶段                                                     │
│  ├── 安装 VEH 异常处理器 + 全局未处理异常过滤器                 │
│  ├── 预初始化 ScreenAspectDetector (读取 INI 确定物理屏幕比例)  │
│  ├── 创建单例: UnifiedScreenCapture / WeaponObserver          │
│  ├── 提交线程池: DetectDevice / HeroNotify / GetControllerInputState │
│  ├── 启动独立 Check 线程 (WeaponObserver)                      │
│  └── 启动独立 HID 读取线程 (GamePadDev::HidReadLoop, 33ms)    │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│  主循环 (60 FPS 动态节流)                                      │
│                                                               │
│  [窗口检测]                                                   │
│  findGameWindow() ──► 前台窗口检测 ──► 稳定性等待 15 帧        │
│                            │                                  │
│                      非前台 ──► "Waiting for game focus..."   │
│                            │                                  │
│                      是前台 ──► (SetForegroundStable 已空操作)  │
│                                       │                       │
│  [截取阶段]                           │                       │
│  UpdateTargetWindow() ──► CaptureFrame()                       │
│                            │          │                       │
│                            ├─ EnhancedRT 激活 → 增强采集路径   │
│                            │          │                       │
│                            ├─ 非增强路径 → Legacy 采集         │
│                            │          │                       │
│                      黑屏检测 (亮度 < 1.0) ──► PauseCapture()  │
│                            │                                  │
│                      正常画面                                  │
│  [推理阶段]                           │                       │
│  inferFromGameClassification(preCheckFrame)                    │
│       │                                                      │
│       ├──► ROI 截取 + 批量分类推理                            │
│       ├──► 结果稳定检测 (帧间一致性检查)                       │
│       └──► ProcessClassifications()                           │
│  [策略阶段]                           │                       │
│  StrategyController ──► HeroStrategy ──► SetTrigger() 入队    │
│  [显示阶段]                           │                       │
│  OpenCV imshow (HWND_NOTOPMOST + SWP_NOACTIVATE)              │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│  安全关闭序列 (解决手柄重启后异常振动)                          │
│                                                               │
│  1. SetOffDirect(0/1)   ← 直接写 HID，绕过队列                │
│  2. StopCheckThread()                                        │
│  3. sleep(50ms)         ← 等待 pending HID 写完成             │
│  4. SetOffDirect(0/1)   ← 再次确认 Off                        │
│  5. StopEnum()          ← 信号 ThrdFunc 停止                  │
│  6. Close()             ← 关闭 HID 设备                      │
└──────────────────────────────────────────────────────────────┘
```

---

## 4. 关键工程约束

> 以下约束来自 project_memory，涉及跨机器一致性、稳定性和正确性，修改前需充分理解其背景。

| 约束 | 说明 | 影响文件 |
|------|------|---------|
| WGC 初始化前必须检查前台窗口 | 非前台时 WGC 抛不可捕获 WinRT 异常 (0xC000027B) | btHonorWorldDet.cpp, WGCScreenCapture.cpp |
| 游戏窗口稳定性等待 ≥ 15 帧 (~250ms) | 提前 attach 导致 DWM/WGC 未就绪 | btHonorWorldDet.cpp |
| ROI 绝对坐标需同时验证 W/H 和 aspectGroup | 21:9 物理屏 + 1920×1080 渲染场景下遗漏 aspectGroup 检查导致误匹配 16:9 条目 | ROIAdaptationConfig.cpp |
| DirectML 推理用 ORT_ENABLE_EXTENDED | ORT_ENABLE_ALL 导致跨 GPU 架构数值漂移，高置信度误分类 | ONNXInference.cpp |
| 分类模型用 INTER_LINEAR 插值 | INTER_CUBIC 上采样导致 ROI0 误分类 | ONNXInference.cpp |
| OpenCV debug 窗口用 HWND_NOTOPMOST + SWP_NOACTIVATE | TOPMOST 窗口抢走游戏前台焦点 | btHonorWorldDet.cpp |
| 关闭时必须用 SetOffDirect 写 HID | 队列中的 Off 指令可能因设备关闭而丢失，手柄重启后异常振动 | btHonorWorldDet.cpp |
| ResetEffects 必须在 SetOff 之前 | ForceReset 清空队列后 SetOff 才有效 | WeaponObserver.cpp, HeroStrategyFactory.cpp |
| HID 读取用独立线程 33ms 固定轮询 | HidReadLoop 线程专用，XInput 已完全移除 | GamePadDev.cpp |
| 环境探针仅允许只读 API | 检测阶段禁止 DXGI/D3D/WGC/GDI，仅 WMI/SetupAPI/注册表/EnumDisplayDevices | GpuEnvironmentProbe.cpp |
| 增强路径新增耗时 ≤0.3ms | 基于 1080p@60fps 约束，主线程每帧额外开销 | UnifiedScreenCapture.cpp |
| WGC 初始化前置条件 | GetForegroundWindow() == gameWnd，确保游戏在前台才启动 WGC | UnifiedScreenCapture.cpp |

---

## 5. 配置文件

| 文件 | 用途 |
|------|------|
| `roi_adaptation.json` | ROI 自适应配置：各分辨率各图标的残差修正值 (v2.3 混合模式) |
| `game-launch-crash.env` | 崩溃诊断环境变量 |
| `GameUserSettings.ini` | 游戏配置文件：ResolutionSizeX/Y（屏幕比例检测备选） |
| 环境变量 `BT_CAPTURE_COMPAT_MODE` | 兼容模式配置：auto / force_enhanced / force_legacy |
| 环境变量 `BT_CAPTURE_PIN_IGPU` | =1 时把 DXGI/WGC 的 D3D 设备硬绑到 iGPU/APU（绕过 monitor-match，失败软回退） |
| 环境变量 `ONNX_FORCE_CPU` | 强制 CPU 推理诊断开关 |

---

## 6. 调试与诊断

| 机制 | 说明 |
|------|------|
| `g_crashStage` | 崩溃阶段记录 (0-903)，用于定位崩溃发生位置 |
| `abort_breadcrumb.txt` | SIGABRT / terminate 时的最后阶段记录 |
| `[ROI-SRC]` 日志 | 输出 aspect、物理分辨率、缓存命中情况 |
| `[ROI-MAP]` 日志 | 输出每帧 ROI→分类结果映射（写 Log/debug_*.log）|
| `[ROI-CONSIST]` 日志 | 检测帧间分类抖动 |
| `[FPS]` 日志 | 每 60 帧输出实际帧率、目标帧率、sleep 补偿量 |
| 独立性能日志 | 每 2 秒输出推理总耗时、推理耗时、FPS |
| `[EnhancedRT]` 日志 | 增强路径激活/格式转换/帧假死重建/显示模式变更/GDI 降频事件 |
| `[UnifiedCapture]` 日志 | 截取方法选择/降级/恢复/混合显卡诊断信息 |
| `[GPU]` 日志 | 窗口所在 GPU adapter 诊断信息 |

---

*文档版本: v2.0 | 生成日期: 2026-08-06*