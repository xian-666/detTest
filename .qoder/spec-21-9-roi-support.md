# 21:9 超宽屏 ROI 自适应增强方案

> **⚠️ 与当前代码的差异说明**
>
> 本文档是一份开发规范文档（spec），描述了 21:9 ROI 支持的设计方案。实际代码已实现了其中大部分功能，但具体实现路径与本文档有所不同：
>
> | 文档中的 Task | 代码实现状态 |
> |---|---|
> | **Task 1**: `GetResidualWithFallback` 传递 `aspectCategory` | ✅ 已实现，见 `ROIAdaptationConfig.cpp` |
> | **Task 2**: JSON 添加 1920×1080(16:9) 和 1920×1200(16:10) 条目 | ❌ 未实现，实际采用 hybrid mode + canonicalTargets 推导机制替代 |
> | **Task 3**: canonicalTargets 解析 + 21:9 scale law | ✅ 已实现，见 `ROIAdaptationConfig.h/.cpp` 和 `roi_adaptation.json` |
> | **Task 4**: GetGameMonitorResolution 多显示器回退 | ✅ 已实现，见 `ScreenAspectDetector.hpp` |
>
> 当前代码通过 v2.3 混合模式（hybrid mode）解决了同样的 21:9 适配问题：当 specificResolutions 中的条目 aspectGroup 与物理屏幕 aspectCategory 不匹配时，几何信息保留、图像处理参数从 aspectProfile 获取，配合 canonicalTargets 比例尺推导，无需为每个分辨率单独注册条目。
>
> 本文档中的 BUG 分析（第 7-11 行）仍然准确，可以辅助理解这些问题是如何被修复的。
>
> 最后更新：2026-08-06

## Context

游戏自动化工具已具备基础的 ROI 自适应架构（两阶段变换 + JSON 残差校正 + 物理屏幕宽高比检测），但 21:9 超宽屏支持存在以下关键缺陷：

1. **BUG**: `GetResidualWithFallback` 第一步未传递 `aspectCategory`，导致添加 16:9 1920x1080 条目后，16:9 用户可能错误命中 21:9 残差
2. **BUG**: `GetResidualWithFallback` 第二步使用渲染分辨率的宽高比匹配 profile，而非物理屏幕的 `aspectCategory`，导致 21:9 物理屏+1920x1080 渲染时无法命中 21:9 的 canonical 回退
3. **缺失**: JSON 缺少 16:9 1920x1080 和 16:10 1920x1200 条目
4. **缺失**: 21:9 aspect profile 无 `canonicalAnchor`/`canonicalTargets`，未注册的 21:9 分辨率无法推导
5. **缺失**: `canonicalTargets`（绝对坐标数组）在 C++ 中从未被解析，16:10 profile 的 scale law 回退是死代码

---

## Task 1: 修复 `GetResidualWithFallback` 的 aspectCategory 传递 (关键修复)

**文件**: `btHonorWorldDet/ROIAdaptationConfig.cpp` 第 512 行

将:
```cpp
if (GetResidual(W, H, iconName, outResidual)) {
```
改为:
```cpp
if (GetResidual(W, H, iconName, outResidual, aspectCategory)) {
```

**同时修复第二步**(第 517-539 行): 当 `aspectCategory` 非空时，用物理屏幕的 aspectCategory 匹配 profile，而非从 `W/H` 计算渲染宽高比:
- 先尝试用 `aspectCategory` 字符串精确匹配 profile 的 `aspectGroup`
- 若 `aspectCategory` 为空，保持现有的 `W/H` 计算逻辑

---

## Task 2: 添加缺失的 JSON 分辨率条目

**文件**: `btHonorWorldDet/roi_adaptation.json`

### 2a. 添加 16:9 1920x1080 条目

在 `specificResolutions` 中添加（基于 honor.txt 数据 vs Stage-1 输出计算残差）:
```json
{
  "width": 1920, "height": 1080, "aspectGroup": "16:9",
  "baseTransform": { "xBias": 0, "yBias": 0, "sizeScale": 1.0 },
  "residualCorrection": {
    "Switch": { "dx": 1, "dy": 0, "dw": 0, "dh": 0 },
    "LsRs":   { "dx": 0, "dy": 0, "dw": 1, "dh": 0 },
    "LB_1":   { "dx": 0, "dy": 0, "dw": 0, "dh": 1 },
    "RB_1":   { "dx": 0, "dy": 0, "dw": 0, "dh": 1 },
    "LT_1":   { "dx": 0, "dy": 0, "dw": 0, "dh": 1 },
    "RT_1":   { "dx": 0, "dy": 0, "dw": 0, "dh": 1 },
    "LB_2":   { "dx": 0, "dy": 0, "dw": 0, "dh": 5 },
    "LT_2":   { "dx": 0, "dy": 0, "dw": 0, "dh": 5 },
    "RT_2":   { "dx": 0, "dy": 0, "dw": 0, "dh": 5 }
  }
}
```

### 2b. 添加 16:10 1920x1200 条目

基于 honor.txt 第 130-138 行的数据:
```json
{
  "width": 1920, "height": 1200, "aspectGroup": "16:10",
  "baseTransform": { "xBias": 0, "yBias": 0, "sizeScale": 1.0 },
  "residualCorrection": {
    "Switch": { "dx": -2, "dy": 118, "dw": 4, "dh": 2 },
    "LsRs":   { "dx": 4, "dy": 119, "dw": -5, "dh": -5 },
    "LB_1":   { "dx": 0, "dy": 118, "dw": 2, "dh": 3 },
    "RB_1":   { "dx": -1, "dy": 118, "dw": 2, "dh": 3 },
    "LT_1":   { "dx": 0, "dy": 119, "dw": 2, "dh": 3 },
    "RT_1":   { "dx": -1, "dy": 119, "dw": 2, "dh": 3 },
    "LB_2":   { "dx": -2, "dy": 119, "dw": 2, "dh": -2 },
    "LT_2":   { "dx": 0, "dy": 119, "dw": 2, "dh": -2 },
    "RT_2":   { "dx": -2, "dy": 119, "dw": 2, "dh": -2 }
  }
}
```

---

## Task 3: 实现 canonicalTargets 解析 + 21:9 scale law

### 3a. C++ 侧修改

**文件**: `btHonorWorldDet/ROIAdaptationConfig.h`
- 在 `AspectProfileEntry` 中新增:
  ```cpp
  bool hasCanonicalTargets = false;
  std::unordered_map<std::string, std::array<int, 4>> canonicalTargets; // [x, y, w, h]
  ```

**文件**: `btHonorWorldDet/ROIAdaptationConfig.cpp`
- `ParseJson`: 解析 `canonicalTargets` 的 `[x, y, w, h]` 数组格式
- `GetResidualWithFallback`: 新增 `canonicalTargets` 推导路径:
  1. 若 profile 有 `hasCanonicalTargets`，从 canonicalTargets 读取 `[cx, cy, cw, ch]`
  2. 计算 `predicted = round(c_i * targetW / canonicalW)` 对 x/y/w/h
  3. 需要调用方传入 Stage-1 输出 `(x0, y0, w0, h0)` 或在函数内重算
  4. `residual = predicted - stage1_output`

**注意**: `GetResidualWithFallback` 当前无法访问 `SKILL_ANCHOR_ROIS`（定义在 common.hpp）。解决方案:
- 在 `ROIAdaptationConfig` 中新增方法 `ComputeResidualFromTargets(iconName, W, H, stage1X, stage1Y, stage1W, stage1H)` 供 `GetAdaptiveRect` 调用
- 或者将 anchor 数据定义移至 `ROIAdaptationConfig.h`

### 3b. JSON 侧修改

**文件**: `btHonorWorldDet/roi_adaptation.json`
为 21:9 profile 添加 canonicalTargets (锚点: 2560x1440):
```json
{
  "aspectGroup": "21:9",
  "targetAspect": 2.333333,
  "tolerance": 0.15,
  "canonicalAnchor": { "width": 2560, "height": 1440 },
  "canonicalTargets": {
    "Switch": [2391, 1092, 97, 97],
    "LsRs": [2327, 1266, 124, 124],
    "LB_1": [2037, 1265, 78, 78],
    "RB_1": [2190, 1265, 78, 78],
    "LT_1": [2037, 1350, 78, 78],
    "RT_1": [2190, 1350, 78, 78],
    "LB_2": [1995, 1308, 78, 78],
    "LT_2": [2101, 1308, 78, 78],
    "RT_2": [2207, 1308, 78, 78]
  },
  "scaleLaw": {
    "type": "target_relative",
    "formula": "predicted = round(canonical_target * W/canonicalW); residual = predicted - stage1"
  }
}
```

---

## Task 4: 改进 ScreenAspectDetector 多显示器回退

**文件**: `btHonorWorldDet/common/ScreenAspectDetector.hpp`

在 `Initialize()` 方法的回退路径（第 258-277 行）替换 `GetDesktopWindow()` 为多显示器感知逻辑:

1. 新增 `GetGameMonitorResolution(int& outW, int& outH)` 函数:
   - 通过 `EnumWindows` 查找游戏窗口（标题含"王者荣耀世界"）
   - 调用 `MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)` 获取所在显示器
   - 使用 `GetMonitorInfoW()` 获取该显示器原生分辨率
2. 在 INI 读取失败后，优先调用 `GetGameMonitorResolution()`
3. 保留 `GetDesktopWindow()` 作为最终回退

---

## Task 5: 验证

### 5a. 回归测试
运行现有 `regression_test.py` 确认所有已注册分辨率不受影响

### 5b. 新增测试用例
- 1920x1080 + aspectCategory="16:9" → 使用 16:9 残差（新增条目）
- 1920x1080 + aspectCategory="21:9" → 使用 21:9 残差（已有条目）
- 1920x1080 + aspectCategory="" → 使用 first-match（需确认行为合理）
- 1920x1200 + aspectCategory="16:10" → 使用新增条目
- 1720x720 (假设 21:9) → 通过 canonicalTargets 推导

### 5c. 编译验证
MSBuild 编译通过，无新增 warning

---

## 修改文件汇总

| 文件 | 变更 |
|------|------|
| `btHonorWorldDet/ROIAdaptationConfig.cpp` | 修复 BUG (aspectCategory 传递) + canonicalTargets 解析 |
| `btHonorWorldDet/ROIAdaptationConfig.h` | AspectProfileEntry 新增 canonicalTargets 字段 |
| `btHonorWorldDet/roi_adaptation.json` | 新增 3 条目 + 增强 21:9 profile |
| `btHonorWorldDet/common/ScreenAspectDetector.hpp` | 多显示器回退改进 |
| `btHonorWorldDet/common/common.hpp` | GetAdaptiveRect 适配 canonicalTargets 调用 |