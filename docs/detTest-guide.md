# detTest — 采图稳定性测试台使用说明

`detTest` 是从 `btHonorWorldDet` fork 出来的**采图 / 推理稳定性测试工程**。  
它保留了完整的屏幕采集与推理管线，删除了全部游戏业务层，目的是在**混合显卡笔记本**上量化三款目标游戏的采图质量，并为「采图调度策略」代码的调优提供可复现的度量数据。

- 目标游戏：《王者荣耀世界》《绝区零》《孤岛惊魂6》
- 关注问题：黑帧、重复帧（stale buffer）、方法降级（DXGI→WGC→GDI）、焦点抖动、长时间运行的帧率稳定性
- 关键要求：**主循环的采图调度状态机必须原样保留**——它本身就是被测对象。任何「顺手优化」都会让测量失去意义。

---

## 1. 与父工程的差异

### 保留（核心资产，勿动）

| 模块                                                                                | 说明                                                                  |
| --------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| `Screen/UnifiedScreenCapture`                                                     | DXGI → WGC → GDI 三级降级状态机、混合显卡软重连 `SoftReinitialize`、WGC 黄框规避、黑帧自愈重建 |
| `Screen/DXGIScreenCapture`、`Screen/WGCScreenCapture`、`Screen/GpuEnvironmentProbe` | 各后端实现与 GPU 环境探针                                                     |
| `ONNXInference`                                                                   | DirectML 批量推理 + 异步流水线                                               |
| 主循环的调度状态机                                                                         | 动态帧率限制、窗口稳定性等待、黑屏进入/退出防抖、失焦暂停、重连保护期                                 |

### 删除（游戏业务层）

`GamePadDev`（HID 手柄，连带 `hidapi` 依赖）、`WeaponObserver`、`StrategyController`、`HeroStrategy/*`（13 个英雄策略）、`IObserver`，以及所有振动/扳机/按键相关的全局变量与调用点。

### 新增

| 模块                        | 说明                                                  |
| ------------------------- | --------------------------------------------------- |
| `CaptureMetrics.h/.cpp`   | 稳定性度量：帧间隔分位数、采集耗时、黑帧/重复帧/降级/失焦计数、HUD 叠加、周期报告、CSV 落盘 |
| `common/AppConfig.h/.cpp` | **UTF-8 安全**的 INI 解析（见 §4.1）                        |
| `common/TargetWindow.h`   | 目标定位：**按进程名**为主键（Toolhelp 快照 + PID 缓存），标题/类名降为可选收窄项 |
| `common/RoiIndex.hpp`     | 从 `StrategyController.h` 中抢救出来的 ROI 索引枚举            |

---

## 2. 目录与构建

```
detTest.sln
detTest\
  detTest.vcxproj          工程名 detTest，RootNamespace detTest
  detTest.cpp              主循环（原 btHonorWorldDet.cpp）
  CaptureMetrics.*
  config.ini               运行时配置
  HONOR                    王者荣耀世界的分类模型（用于验证推理管线可用）
  Screen\                  采集子系统（要求纯 ASCII，见 check_ascii.ps1）
  common\                  工具类
Component\                 日志、线程池等通用组件
```

工程原本指定工具集 `v143`。若本机只有更高版本（例如 VS2026 的 `v180`），不要改工程配置，在命令行覆盖即可：

```
MSBuild detTest\detTest.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v180
```

> `Screen\` 目录有纯 ASCII 约束（中文会随 MSVC 代码页损坏），提交前请跑一次 `.\check_ascii.ps1`。

---

## 3. 运行

```
# 1) 先用王者模型跑一次，确认推理管线正常
# 2) 启动目标游戏，把窗口放在前台
# 3) 启动 detTest
```

- 键位：`q` / `ESC` 退出；`s` 保存当前帧的 ROI 裁图（`ROI_Captures\`）。
- 退出时会在控制台打印整轮的 **stability summary**，这是每次测试要留档的东西。
- OpenCV 显示窗口用 `HWND_NOTOPMOST` + `SWP_NOACTIVATE` 创建，**不会抢走游戏前台焦点**。若发现它会抢焦点，请检查 `WIN_NAME` 是否被改动——焦点保护是靠 `FindWindowW(NULL, WIN_NAME)` 实现的。

---

## 4. 配置（`detTest/config.ini`）

配置文件位于源码目录，**首次运行时会自动拷贝到 exe 同目录**；之后只有源文件比运行目录的副本更新时才会覆盖，你在运行目录上的临时改动不会被静默冲掉。

### 4.1 为什么不用 `GetPrivateProfileStringW`

因为配置文件里有中文窗口标题。Windows 的 private-profile API 会按**系统 ANSI 代码页（GBK）**&#x89E3;释文件内容，UTF-8 编码的文件必然乱码，导致窗口永远找不到。  
`appcfg::IniFile` 自己读取 UTF-8 并按 UTF-8 解析，中文标题安全。改这块时不要图省事换回去。

### 4.2 `[Target]` —— 按进程名定位目标

**主定位键是 `process`（进程映像名），不是窗口标题。**
原因：标题是最不稳定的标识——启动器、反作弊外壳、UE 辅助窗口的标题都很接近，而且部分游戏的标题会随语言变化；进程名才能真正标识「是哪个游戏」。

解析流程：Toolhelp 快照枚举进程 → 按名字匹配出 PID 集合 → 在该集合拥有的可见窗口中挑最合适的（顶层 > 非最小化 > 有标题 > 面积最大）。

```ini
[Target]
process = NGR-Win64-Shipping
; title = 王者荣耀世界     ; 可选，仅在同进程多窗口时用来收窄
; class = UnrealWindow     ; 可选
```

- 三个键都是**忽略大小写的子串匹配**，只有非空的才生效。只填 `process` = 「属于该可执行文件的任意可见窗口」。
- `.exe` 后缀两边都可省略：`FarCry6` / `FarCry6.exe` / `farcry6` 都能匹配 `FarCry6.exe`。
- **未配置 `process` 时**退化为旧的标题/类名匹配（保持父工程行为）；三个键全空则回退到 `王者荣耀世界`。

三款游戏的 profile：

| 游戏 | `process` |
|------|-----------|
| 王者荣耀世界 | `NGR-Win64-Shipping` |
| 绝区零 | `ZenlessZoneZero` |
| 孤岛惊魂6 | `FarCry6` |

不确定可执行文件名？启动游戏后跑：

```
tasklist /fi "STATUS eq running" /fo csv | findstr /i "\.exe"
```

或看任务管理器的「详细信息」页。因为是子串匹配，填一个特征片段就够了。

> 启动时若配置的进程没在运行，控制台会直接打印 `WARNING: no running process matches '...'`。
> 这用来区分「配置写错了」和「游戏还没启动」——两种情况在主循环里表现完全一致，不提示会浪费大量排查时间。

两个实现要点，改这块时别绕开：
- 用 **Toolhelp 快照**而不是 `OpenProcess` 拿进程名：受保护/提权的游戏进程会拒绝打开句柄，而那恰恰是最需要找到的。
- PID 集合带 **1 秒 TTL 缓存**：`findGameWindow()` 每帧被主循环和异步推理线程调用多次，不能每帧重扫一遍进程表。
### 4.3 `[Log]`

| 键         | 默认      | 说明                                    |
| --------- | ------- | ------------------------------------- |
| `console` | `true`  | 周期稳定性报告输出到这里（父工程默认 `false`，本工程刻意打开）   |
| `text`    | `false` | `DEBUG_LOG*` 写 `\Log\` 目录。只在定位具体问题时打开 |
| `show`    | `true`  | 是否显示 OpenCV 窗口。关掉则 HUD 无从绘制           |

### 4.4 `[Metrics]`

| 键                     | 默认        | 说明                                               |
| --------------------- | --------- | ------------------------------------------------ |
| `enable`              | `true`    | 总开关，关掉后没有任何统计开销                                  |
| `hud`                 | `true`    | 画面左上角叠加 FPS / 当前方法 / 状态 / 丢帧计数                   |
| `report_interval_sec` | `5`       | 控制台周期报告间隔（秒）                                     |
| `csv`                 | `false`   | 每帧写一行 CSV                                        |
| `csv_dir`             | `metrics` | CSV 输出目录（相对 exe），文件名 `detTest_metrics_<时间戳>.csv` |

CSV 列：

```
elapsed_ms,interval_ms,capture_ms,inference_ms,method,state,capture_ok,black,duplicate,width,height
```

---

## 5. 度量含义与判读

### 5.1 周期报告样例

```
[METRICS] t=300.0s fps=59.8 | method=DXGI state=Run
  interval(ms) p50=16.7 p95=17.2 p99=24.1 max=250.3 | capture(ms) avg=4.2 p95=6.1 max=48.0 | inference(ms) avg=8.3 p95=11.0
  frames=17940 ok=17895 fail=45 (0.3%) black=12 dup=310 maxFailStreak=4
  events: downgrade=0 upgrade=0 pause=2 stopped=0 focusLost=2 (1820ms) forceReinit=1 softReinit=2 blackEpisodes=3
```

### 5.2 各指标说明

| 指标                           | 含义                                  | 健康参考                                             |
| ---------------------------- | ----------------------------------- | ------------------------------------------------ |
| `fps`                        | 近 1 秒滑动窗口 FPS                       | 贴近显示器刷新率                                         |
| `interval p50 / p95 / p99`   | 两次主循环迭代的墙钟间隔分位数                     | p99 与 p50 差距小说明抖动小                               |
| `capture avg / p95 / max`    | `CaptureFrame()` 内部耗时，即**要优化的那个数字** | DXGI 通常 < 8ms；WGC 略高；GDI 明显更高                    |
| `inference`                  | `ONNXInference` 报告的推理耗时             | 采集与推理分离后不应影响 `interval`                          |
| `fail` / `maxFailStreak`     | `CaptureFrame()` 返回失败的次数与最长连续失败     | 应力求 0；连续失败说明会话已坏                                 |
| `black` / `blackEpisodes`    | 平均亮度低于阈值的帧数 / 黑屏事件段数                | 除加载过渡外应为 0                                       |
| `dup`                        | 与上一帧像素完全一致的帧数                       | 偶发几帧正常；连续 ≥30 帧会单独告警（stale buffer 嫌疑）            |
| `downgrade` / `upgrade`      | DXGI→WGC→GDI 的降级次数 / 回升次数           | 稳定会话应为 0；频繁降级说明 GPU 迁移正在发生                       |
| `focusLost`                  | 目标窗口失焦次数与累计时长                       | 切出去过就会计数，属正常；频繁抖动需查 `UnifiedScreenCapture` 的前台锁定 |
| `forceReinit` / `softReinit` | 重建 / 软重连次数                          | `force` 上升说明会话被判定为不可恢复                           |

### 5.3 混合显卡场景的判读要点

笔记本在集显与独显之间迁移时会短时间交不出可用帧，典型症状链：

```
downgrade↑ → fail↑ / maxFailStreak↑ → black↑ → forceReinit↑ → 短暂恢复 → 再次降级
```

调优时应**同时**看三个层面的改善：降级次数下降、`capture max` 下降、`maxFailStreak` 下降。只看 FPS 平均值会被平均掉，看不出问题。

---

## 6. 测试流程建议

1. **基线**：`[Metrics] csv = true`，目标游戏跑到稳定场景，跑满 5 分钟。
2. **留档**：保存退出的 summary 与 CSV（`metrics\detTest_metrics_*.csv`）。
3. **对照**：同场景重复 3 次，确认降级/黑帧是否可复现。
4. **调优**：改采图调度策略后重跑，**只改一处变量**。
5. **对比**：重点比 `downgrade`、`capture p95/max`、`maxFailStreak`、`blackEpisodes`。

---

## 7. 已知约束

- **ROI 是写死的**：`detTest.cpp` 里的 `captureRegions` 仍是王者技能键在 1920×1080 下的 8 个位置。  
  对《绝区零》《孤岛惊魂6》来说，这些 ROI 会落在无关像素上，模型输出 `Other` 是**预期结果**——本工程要验证的是采集链路，不是标签正确性。需要真正做识别时再引入 ROI 自适应。
- **模型**：沿用的是王者的分类模型（`HONOR` 文件），同样只为验证推理管线正常。
- **屏幕比例探测**：`common/ScreenAspectDetector.hpp` 会去读 `%LOCALAPPDATA%\NGR\...\GameUserSettings.ini`（王者的 UE 配置），对另外两款游戏读不到时会回退，不影响采集，只是取不到游戏内分辨率。
- **`Component/` 目录**：历史上被 `.gitignore` 排除且未纳入版本控制，纯净 clone 会编译失败。若遇到同样情况，注意 `.gitignore` 里已相应修正。
- **混显自动绑核显（无需手动配置）**：`DXGIScreenCapture::InitializeForWindow` 默认按显示器归属（`MonitorFromWindow`）选卡，Optimus 内屏由 iGPU 驱动即自动落到核显；iGPU 的 DDA 经 DWM 合成帧能拿到独显渲染的内容。dGPU-first 路径已是 dead code。若 MUX/迁移挑错卡，可用环境变量 `BT_CAPTURE_PIN_IGPU=1` 强制硬绑核显。
- **孤岛惊魂6（混显冷启动 CG 卡死）——已知限制**：根因是 FC6 走 dGPU 渲染 + 独立翻转/MPO **绕过 DWM 合成**，iGPU 上的 DDA 只看到同一张静止的合成图（表现为“卡在开局画面、非黑”），既不是 device-lost、又是“有效但静止”的帧，**不会触发降级/重建**，无法自愈。实测：切 WGC（混显下 `deviceRemoved`）、原地重建 duplication（新复制对象仍读到同一张没更新的图）、调 device-lost 退避（会连累其它游戏 CG）均无效或有害。生产 show 必须隐藏，无法用“置顶逼 DWM 重合成”绕过；隐形置顶辅助窗有触发反外挂 overlay 指纹的风险。**结论：FC6 在这类混显本上采用「游戏启动完成后再启动 AI」（后开 AI）**——把采集 attach 错开 FC6 冷启动的设备风暴/呈现建立窗口即可正常。
- **混显冷启动宽限期与有界复探（`detTest.cpp`）**：A. 首次 attach 前，混显上等待前台目标稳定 `kHybridColdStartGraceMs`（=10000ms）再初始化，规避开局脆弱窗口（HWND 变化则重新计时）；B. 黑屏期间做“有界复探”（≤30000ms、≤2 次）。这些是 detTest 主循环层的时序规避，**不改共享采图恢复核心**。

