# WGC 间歇黑帧抖动：诊断结论与第一期加固

> 日期：2026-09-10
> 关联代码：`Screen/UnifiedScreenCapture.*`、`Screen/WGCScreenCapture.*`
> 状态：第一期加固已实现并编译通过（改动 2 + 改动 1 + 改动 3a，见下文）

---

## 1. 现场现象

用户笔记本（Intel UHD 核显 + RTX 3060 Laptop，Optimus 架构）启动辅助程序后：

- 采图方法立即进入 WGC 并**一直保持 WGC**；
- show 窗口在"黄字提示"和"正常采图画面"之间**高频来回切换**，频率约每秒数次；
- 推理几乎拿不到有效帧。

用户机器当时任务管理器关键读数：

| 指标 | 用户机器 | 开发者机器（对照组） |
|---|---|---|
| 内存 | 15.7 GB，**96%**（可用约 0.6 GB） | 32 GB，37%（可用 19.9 GB） |
| 架构 | Intel UHD 核显输出 + RTX 3060 渲染（Optimus） | 同类核显 + 独显架构 |
| 当时负载 | RTX 3060 74°C / 24%，游戏 + 推理满负荷 | 截图时 GPU 空闲 |

## 2. 根因分析链

### 2.1 "立刻进入 WGC"不是降级，是设计行为

启动宽限期强制 WGC（`UnifiedScreenCapture::InitializeWGCForced`），日志表现为
`Startup grace period: forcing WGC mode...`，目的是规避游戏窗口刚出现时的 DXGI ACCESS_LOST。

### 2.2 "一直保持 WGC"的原因：黑帧不计入错误计数

`UnifiedScreenCapture::CaptureFrame` 中，空帧/黑帧（`ValidateFrame` 判黑/判白）按设计
"跳过错误计数"直接返回 false——因为它们在健康机器上是暂态。副作用是：**黑帧永远凑不满
`MAX_CONSECUTIVE_ERRORS = 5`，既不降级、也不触发任何重建**，WGC 会话劣化后系统会
永久停留在半失效状态。

### 2.3 show 上"黄字 ↔ 正常画面"高频切换的完整循环

1. WGC 交来黑帧 → 亮度 < 2.0 → 主循环 `PauseCapture()` → show 显示黑画面 + 黄字
   **"Game Loading / Black Screen [PAUSED]"**；
2. 下一帧因 Paused 状态采图失败 → show 黄字 **"CAPTURE RETRY..."** + `ResumeCapture()`；
3. WGC 交来正常帧 → show 正常画面；
4. 再来一帧黑帧 → 回到第 1 步。

黑帧穿插在帧流中间歇出现时，上述循环以 ~10Hz 振荡，与现场现象吻合。

### 2.4 为什么这台机器 WGC 会间歇性吐黑帧

关键：**Intel UHD 核显没有独立显存**，WGC 帧池、DWM 合成缓冲、staging texture
全部从共享内存（= 系统 RAM）分配。

```
Intel UHD 无独立显存
→ 采集链路内存全部走系统 RAM
→ RAM 96% 耗尽（剩 0.6 GB）
→ D3D 分配失败 / DWM 跨适配器拷贝超时
→ WGC 间歇性交出黑帧
→ 黑帧不计错误 → 永久抖动、无自愈
```

补充：Optimus 笔记本"有独显"≠独显驱动显示。日常桌面永远走核显输出，
所以这类笔记本全部暴露在此风险下；只有 MUX 直连或台式机独显输出是纯独显链路。
纯独显机器（显存驱动显示器）采集链路走 VRAM，RAM 耗尽只会导致分页卡顿/推理变慢，
**不会出现这种黑帧抖动形态**（例外：显存也吃满时 D3D 会 fallback 到系统内存）。

## 3. 第一期加固内容

设计原则：**任何机器上的最坏表现等于改动前**——只增加日志与自愈能力，零回归。

### 3.1 改动 2：区分"从未拿到帧"与"拿到黑帧"（WGCScreenCapture）

- `WGCScreenCapture::GetFrame` 删除"全零 1920×1080 兜底帧返回 true"的逻辑，
  无任何可用帧时**返回 false**（原两个调用方均检查返回值，行为等价于原来
  兜底帧被 `ValidateFrame` 拒绝的最终结果）。
- 新增 `FrameSource`（none / new / cached）来源标记，`GetFrame` 在每条路径
  上记录本次返回帧的来源，供上游诊断日志引用。

### 3.2 改动 1：连续黑/空帧计数 + WGC 会话重建（UnifiedScreenCapture）

新增 `UnifiedScreenCapture::NoteBadFrameAndMaybeRebuildWGC()`，在 `CaptureFrame`
的空帧/黑帧分支调用；连续坏帧达到阈值后以
`StopCapture() + InitializeWGCForced()` 重建 WGC 会话（保持 WGC 模式，不重走
DXGI 路由、不触碰推理线程）。三个护栏：

| 护栏 | 实现 |
|---|---|
| ① 只在 WGC + Capturing 状态计数；合法黑屏不喂计数器 | `Paused`/`ExceptionWait` 在分发前就 return，天然不经过计数；`ResumeCapture()` 显式清零计数，主循环过图黑屏暂停永远不会触发重建 |
| ② 退避 + 熔断 | 阈值指数退避 30 → 60 → 120 帧；最多重建 5 次（`BLACK_STREAK_REBUILD_LIMIT`），之后只记日志；连续 600 有效帧（约 20s+）后自动重新武装 |
| ③ 与主循环黑屏状态机解耦、异常吞噬 | 重建只发生在采集层内部，复用 `InitializeForWindow` 现成的"先 Stop 再重启"模式（`m_processedMutex`/`m_frameMutex` + 全进程 VEH 兜底），不引入新崩溃面 |

成功帧会同时清零坏帧计数并累计好帧计数。

### 3.3 改动 3a：适配器一致性观测（只记日志，不切换）

- `WGCScreenCapture::InitializeD3D11Device` 记录默认设备实际落在哪个物理适配器
  （LUID / VendorId / DeviceId / 描述），每次初始化打一行；
- 新增 `CheckAdapterConsistency()`：每帧做廉价的 `MonitorFromWindow` 跟踪，
  重型 DXGI 枚举只在初始化、显示器变更或每 300 帧（约 5s）执行一次，
  对比"WGC 设备适配器"与"窗口所在显示器适配器"是否一致；
- **只写日志，绝不改变采图行为**。健康匹配每次初始化只打一行；
  不一致期间每 ~5s 重复记录，保证现场日志始终留有证据。

## 4. 新增日志标记与远程排查指南

日志文件在程序目录 `\Log\`（需开启 DEBUG 日志）。

### 4.1 关键字速查

| 关键字 | 含义 |
|---|---|
| `[UnifiedCapture][Diag] Empty frame received ... src=` | 空帧；`src=none` 表示从未收到任何 WGC 帧 |
| `[UnifiedCapture][Diag] Frame validation failed ... brightness= src=` | 黑/白帧被拒；brightness 是平均亮度，src 指明来源 |
| `[UnifiedCapture][SelfHeal] WGC bad-frame streak` | 坏帧连续计数推进（含当前阈值与已用重建次数） |
| `[UnifiedCapture][SelfHeal] ... rebuilding WGC session` | 触发 WGC 会话重建 |
| `[UnifiedCapture][SelfHeal] WGC session rebuilt OK / FAILED` | 重建结果 |
| `[UnifiedCapture][SelfHeal] ... rebuild budget re-armed` | 连续好帧后熔断重新武装 |
| `[WGCScreenCapture] Device adapter: ...` | WGC D3D 设备所在物理适配器 |
| `[WGCScreenCapture][Diag-3a] Adapter consistency: ... MISMATCH=` | 设备适配器 vs 显示器适配器一致性 |

### 4.2 判读表

| 日志组合 | 结论 |
|---|---|
| `brightness=0.00~1.00`，`src=new` 与 `src=cached` 交替出现 | WGC 实际交来黑帧（内存型/DWM 拷贝型），不是"帧率低" |
| `src=none` 大量出现 | WGC 从未吐帧：帧池/回调链路问题，重点查 `Frame callback exception` |
| `Diag-3a ... MISMATCH=1` | 设备适配器与显示器适配器不一致 → 每帧跨适配器拷贝，二期 3b 的直接证据 |
| `SelfHeal` 重建后恢复、`rebuild budget re-armed` 出现 | 会话级损坏（重建可治），非环境问题 |
| 5 次重建后仍抖动、熔断后只记日志 | 环境问题（内存/驱动），重建不可治 → 引导用户释放内存、查驱动/采集类软件冲突 |
| 空帧+黑帧高频交替，`Diag` 行 `diagFails` 快速增长 | 即本案例的"黄字抖动"形态 |

### 4.3 本案例（用户机器）的现场处置顺序

1. 关闭吃内存的后台程序，把 RAM 压到 85% 以下再跑——大概率直接恢复；
2. 检查 Xbox Game Bar / OBS / 其他采集软件（占用 Desktop Duplication 会导致
   DXGI 初始化失败，永远回不去 DXGI）；
3. 若仍有问题，取回 `\Log\`，按 4.2 判读；Intel 驱动版本（本机 32.0.101.7084）
   与正常机器做对比。

## 5. 行为变化与风险评估

- **改动 2**：与改动前最终行为等价（原兜底黑帧也会被 `ValidateFrame` 拒绝），
  仅日志更有区分度。唯一例外：EnhancedRT（MUX 机器）下"从未吐帧的 WGC"现在会
  计入 `m_consecutiveErrors` 并最终降级 GDI——这本身是正确的自愈方向。
- **改动 1**：复用现有重入路径 + VEH/try-catch，无新崩溃面；风险在行为回退，
  已由三个护栏覆盖（过图黑屏不触发重建、退避+熔断防重建风暴、Resume 解耦）。
- **改动 3a**：纯观测，零行为变化。

## 6. 二期计划（按证据决定，未实施）

- **3b 真切换**：若日志证实 `MISMATCH=1` 与黑帧强相关，把 WGC 帧池设备显式创建在
  窗口所在显示器的适配器上（`MonitorFromWindow` + `EnumAdapters1` 匹配，
  `D3D_DRIVER_TYPE_UNKNOWN`），保留"匹配失败回退默认适配器"兜底。
- 内存压力场景下的推理性能耗优化（ONNX arena 限制、马赛克克隆释放）。
- 是否把"持续黑帧"纳入 DXGI 升级条件（结合 3a 证据）。
