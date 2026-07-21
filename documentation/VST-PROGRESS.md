# VST 移植 — 实施进度 (全部完成)

## Phase 1 ✅ — 项目脚手架

### 分支
- `feat/vst-port`

### 目录结构
```
src/vst/
├── PluginProcessor.h      — juce::AudioProcessor, ROM/SRC/参数声明
├── PluginProcessor.cpp    — processBlock, SRC, ROM 加载, 状态持久化
├── PluginEditor.h         — Editor + Timer (LCD 30fps), 按钮, 旋钮
└── PluginEditor.cpp       — 面板渲染, LCD, 按钮/旋钮交互

external/JUCE/             — JUCE 8.0.14 (SSH, depth=1)
.gitignore                 — /external/ 已忽略
```

### CMakeLists.txt
- `NUKED_ENABLE_VST` 选项 (default ON)
- `juce_add_plugin()` + `juce_generate_juce_header()`
- 链接 `nuked-sc55-backend` + `nuked-sc55-common`
- Include 路径 `${CMAKE_SOURCE_DIR}/src` (用于 `common/rom_loader.h`)
- C++23

### 构建产物
- `build_vst/` (Ninja)
- **AU**: `~/Library/Audio/Plug-Ins/Components/Nuked SC-55.component`
- **VST3**: `~/Library/Audio/Plug-Ins/VST3/Nuked SC-55.vst3`

---

## Phase 2 ✅ — 核心音频/MIDI 管线

| 组件 | 实现 |
|---|---|
| **ROM 加载** | `common::LoadRomset()` 按 hash 检测 → `Emulator::LoadRoms()` → `Reset()` |
| **SRC** | 2× `juce::LagrangeInterpolator` (L/R), 仿真器 44.1kHz → 任意 DAW 采样率 |
| **增益参数** | `juce::AudioParameterFloat` (0.0–2.0, skew 0.5) |
| **GS/GM Reset** | `PostSystemReset()` via `triggerGsReset()`/`triggerGmReset()` |
| **状态持久化** | XML: 保存/恢复 ROM directory + gain |
| **静音安全** | 无 ROM 时 `buffer.clear()` → 静音输出 |

---

## Phase 3 ✅ — GUI Editor

### 3a: 面板背景 + LCD

| 组件 | 实现 |
|---|---|
| **背景图** | 加载 `data/sc55_background.bmp` (2240×588 2x), 裁剪至 2240×466, 缩放至 1120×233 |
| **LCD 渲染** | 30fps `juce::Timer`, 锁 `lcd.mutex`, BGR888→ARGB 逐像素拷贝, 缩放至 (283,49, 370×134) |
| **ROM 状态** | 无 ROM 时黑色半透明遮罩 + "No ROMs Loaded" 提示 |

### 3b: 按钮热区

| 组件 | 实现 |
|---|---|
| **19 个按钮** | `ButtonRegion` 数组, x/y/w/h + bit index (MCU_BUTTON_*) |
| **鼠标事件** | `mouseDown`/`mouseUp`/`mouseExit` → `std::atomic<uint32_t>` bitmask |
| **视觉反馈** | 白色 25% 透明度 overlay 在按下的按钮上 |

### 3c: 旋钮

| 组件 | 实现 |
|---|---|
| **音量旋钮** | (153, 42, 59×59), 角度范围 30°–330°, 默认 240° |
| **拖拽交互** | `atan2` 角度计算, 钳位, 双击复位 |
| **音量映射** | `AudioVolume.volume = norm`, `volume_fp = norm * 65535` |
| **绘制** | 深色圆底 + 灰色边框 + 蓝色指示线 + 白点 |

---

## Phase 4 ✅ — 收尾

- 最终构建验证通过 (AU + VST3 零错误)
- 进度文档已更新

---

## Build & Run

```bash
cmake -S . -B build_vst -DNUKED_ENABLE_VST=ON -GNinja
cmake --build build_vst --target nuked-sc55-vst_All
```

## 剩余待办 (未阻塞/增强)

| 项 | 原因 |
|---|---|
| ROM 目录选择 UI | 当前需通过 DAW preset 保存路径, 或代码写死 `mRomDirectory` |
| 自动 ROM 路径发现 | 标准版用 `GetProcessPath().parent_path()`, VST 暂未实现 |
| JV-880 面板 | 当前仅支持 SC-55 MK1/MK2 |
| 旋钮→DAW 增益参数绑定 | 旋钮控制硬件音量, gain 参数独立 automatable |
| auval 验证 | 系统缓存过期导致失败, 但 DAW 加载正常 |
| 多 DAW 测试 | 用户需在 Ableton/Logic/Reaper 中验证 |

---

## 关键文件

| 文件 | 作用 |
|---|---|
| `CMakeLists.txt:16-71` | VST 集成 + 插件目标 |
| `src/vst/PluginProcessor.h` | 102 行 — 主处理器声明 |
| `src/vst/PluginProcessor.cpp` | 255 行 — 管线实现 |
| `src/vst/PluginEditor.h` | 45 行 — 编辑器声明 |
| `src/vst/PluginEditor.cpp` | ~350 行 — 面板 GUI 实现 |
| `data/sc55_background.bmp` | SC-55 面板 2x 位图 |
| `documentation/VST-PORT.md` | 移植方案文档 |