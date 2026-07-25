# Nuked SC-55 → VST3/AU Plugin 移植方案

## 1. 项目背景

**Nuked SC-55** 是一个精确到指令级的 Roland Sound Canvas SC-55 硬音源仿真器（也支持 JV-880、SC-155）。
当前架构：C++23，CMake 构建，SDL2 桌面前端 + 独立离线渲染器。

本文件记录将其移植为 VST3/AU 插件（保留硬件面板 GUI）的设计方案和实现细节。
构建安装指南见 [`AGENTS.md`](../AGENTS.md)。

---

## 2. 原始架构

```
┌──────────────────────────────────────────────────────────┐
│  src/backend/  (nuked-sc55-backend)  ← 纯 C++ 仿真引擎   │
│  • MCU 模拟 (NEC V50)  • PCM 合成  • ROM 加载            │
│  • 音频回调: mcu_sample_callback(void*, AudioFrame<int32>)│
│  • 无平台依赖、无 GUI 依赖                                │
│  • 采样率由 PCM_GetOutputFrequency() 动态获取             │
│    SC-55mk2 oversampling: ~66207 Hz, 无: ~33103 Hz       │
├──────────────────────────────────────────────────────────┤
│  src/common/   (nuked-sc55-common)                       │
│  • ROM 加载器  • 增益计算  • 路径工具                     │
├──────────────────────────────────────────────────────────┤
│  src/standard/  (SDL2 桌面前端)  ← VST 替换的目标        │
│  • SDL 音频输出  • LCD 窗口  • RtMidi 输入               │
│  • 事件循环: 独立仿真线程 + SDL_PollEvent 主线程          │
├──────────────────────────────────────────────────────────┤
│  src/renderer/  (MIDI→WAV 离线渲染器，保留)              │
└──────────────────────────────────────────────────────────┘
```

### 核心接口 (后端的公共 API)

| 接口 | 签名 | 用途 |
|---|---|---|
| `Emulator::Init()` | `(const EMU_Options&) → bool` | 初始化仿真器 |
| `Emulator::LoadRoms()` | `(Romset, AllRomsetInfo, ...) → bool` | 加载固件 |
| `Emulator::Reset()` | `() → void` | 复位 |
| `Emulator::PostMIDI()` | `(span<uint8_t>) → void` | 输入 MIDI 数据 |
| `Emulator::SetSampleCallback()` | `(mcu_sample_callback, void*) → void` | 注册音频回调 |
| `Emulator::Step()` | `() → void` | 推进一帧仿真（触发零或多次回调） |
| `mcu_sample_callback` | `typedef void(*)(void*, const AudioFrame<int32_t>&)` | 每 sample 回调，立体声 int32 |

> **注意**：`Step()` 不保证每次调用都产出音频帧。SC-55mk2 (reg_slots=15) 约 33 步产出一帧。
> 这是 VST 音频管线设计的核心约束（见 §4.4.1）。

---

## 3. GUI 现状分析 (SDL 前端)

原始 GUI (`src/standard/lcd_sdl.h/cpp`) 由四层构成：

### 3.1 背景面板
- `data/sc55_background.bmp` (2240×466) — SC-55 前面板高清扫描图
- `data/jv880_background.bmp` (2872×400) — JV-880 前面板
- 均为 2x 尺寸用于 Retina 显示，运行时缩放到 1x

### 3.2 LCD 像素缓存
- `lcd_t.buffer[1024][1024]`，每像素 `uint32_t`
- 仿真器实时写入，`Render()` 中通过 `SDL_UpdateTexture` 上传为纹理
- 以 BGR888 格式 composite 到背景图上的 LCD 屏幕区域
  - SC-55: 目标位置 `(283, 49)` 尺寸 `370×134`
  - JV-880: 目标位置 `(174, 83)` 尺寸 `410×50`

### 3.3 按钮系统
- 两个 `SDL_Rect[32]` 数组定义按键热区
- 鼠标点击检测 → `mcu_t.button_pressed` 位掩码
- 按钮 LED 指示灯：从 BMP 特定区域裁剪 lit/unlit 两种状态图块渲染
  - SC-55: ALL/MUTE/STANDBY 三组 LED
  - JV-880: 9 组 LED (Edit, System, Rhythm, Utility, Patch Perform, Mute, Monitor, Compare, Enter)

### 3.4 旋钮系统
- `LCD_Knob` 结构体: 中心坐标 + 宽高 + 角度 + 范围
- `LCD_DrawKnob()`: 从背景图裁剪旋钮区域 → 旋转渲染
- SC-55: 1 个音量旋钮 → `lcd.volume` → 音频增益
- JV-880: 2 个旋钮（音量 + 编码器） → 编码器触发 `MCU_EncoderTrigger()`

### 3.5 事件处理 (HandleEvent)
- `SDL_MOUSEBUTTONDOWN/UP` → 按钮区域检测 / 旋钮拖拽开始/结束
- `SDL_MOUSEMOTION` → 旋钮角度计算（atan2）
- `SDL_MOUSEWHEEL` → 旋钮增量
- `SDL_KEYDOWN/UP` → 键盘快捷键映射到 MCU 按钮
- `SDL_WINDOWEVENT_CLOSE` → 退出请求

---

## 4. VST 移植方案

### 4.1 整体策略

**后端完全不动**，新写 `src/vst/` 替换 `src/standard/`。
用 JUCE 8 框架，原因：
- 原生 VST3/AU/AAX 支持
- `AudioProcessorEditor` 提供 DAW 内嵌编辑器窗口
- 像素级 Graphics API，完美匹配 LCD framebuffer 渲染
- PNG 可嵌入为 C 数组（`EmbeddedResources.h`）
- 内置 Lagrange 插值器用于采样率转换

### 4.2 框架：JUCE 8

| 特性 | 用途 |
|---|---|
| `juce::AudioProcessor` | `processBlock()` 替换 `FE_RunInstanceSDL` |
| `juce::AudioProcessorEditor` | DAW 管理窗口生命周期，替代 `SDL_CreateWindow` |
| `juce::Image` | 加载 PNG 做背景，像素操作渲染 LCD |
| `juce::Graphics` | `drawImage()` composite，`drawImageTransformed()` 旋钮旋转 |
| `juce::LagrangeInterpolator` | 仿真器 66kHz → DAW 44.1kHz/48kHz 重采样 |
| `juce::Timer` | 60fps LCD 刷新 + 空闲时 MCU 步进 |

### 4.3 CMake 集成

在现有 `CMakeLists.txt` 中通过 `juce_add_plugin()` 添加插件目标：

```cmake
option(NUKED_ENABLE_VST "Build VST3/AU plugin" ON)

if(NUKED_ENABLE_VST)
    set(JUCE_DIR "${CMAKE_SOURCE_DIR}/external/JUCE" CACHE PATH "Path to JUCE")
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)

    juce_add_plugin(nuked-sc55-vst
        FORMATS VST3 AU
        PRODUCT_NAME   "Nuked SC-55"
        BUNDLE_ID      "com.nukedsc55.nuked-sc55-vst"
        PLUGIN_MANUFACTURER_CODE Nukd
        PLUGIN_CODE     Sc55
        IS_SYNTH        TRUE
        NEEDS_MIDI_INPUT TRUE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD TRUE
        VERSION         0.6.3
    )

    juce_generate_juceheader(nuked-sc55-vst)

    target_sources(nuked-sc55-vst PRIVATE
        src/vst/PluginProcessor.cpp
        src/vst/PluginEditor.cpp
    )

    target_link_libraries(nuked-sc55-vst PRIVATE
        nuked-sc55-backend
        nuked-sc55-common
    )

    target_compile_features(nuked-sc55-vst PRIVATE cxx_std_23)
endif()
```

### 4.4 音频管线

**SDL 前端 (独立线程)**:
```
独立线程: while(running) { emu.Step(); }
  → 回调 mcu_sample_callback
  → Normalize + Gain → SDL ringbuffer
SDL 音频回调 → 从 ringbuffer 读取 → 输出
```

**VST (DAW 音频线程 + Ring Buffer)**:
```
DAW 音频线程:
  processBlock(outputBuffer, midiMessages):
    for each MIDI message: emu.PostMIDI(byte)
    lock(mEmulatorMutex):
      snapshot startWrite = mAudioRingWrite
      for step in 0..maxSteps:
        emu.Step()
        → 回调 sampleCallback → push 到 mAudioRing[writePos++]
        if mAudioRingWrite - startWrite >= framesNeeded: break
      drain: scratch[i] = mAudioRing[(startWrite + i) % kAudioRingSize]
    LagrangeInterpolator.process(speedRatio, scratch, out, numSamples)

UI 定时器 (60fps, 空闲时):
  stepEmulatorForUi():
    if DAW 活跃 (processBlock 距今 < 50ms): return  # 零锁竞争
    lock(mEmulatorMutex):
      for i in 0..2000: emu.Step()  # 保持 MCU/LCD 活着
```

`sampleCallback` 是音频帧的入口点，在 `Step()` → `MCU_Step()` → `PCM_Update()`
→ `MCU_PostSample` 中被调用（已在 `mEmulatorMutex` 锁内）。完整音频链：

```
AudioFrame<int32> (PCM 原始输出)
  → Normalize(frame, sample, mVolumeControl)   # int32→float + 硬件音量
  → Scale(sample, gain)                          # VST Gain 参数 (0.0–2.0)
  → mAudioRing[writePos++].push(sample)          # 写入 ring buffer
  → mCurrentSample = sample                      # 缓存最新帧 (供 UI 直接读)
```

#### 4.4.1 核心约束：Step 次数 ≠ 音频帧数

`Emulator::Step()` 调用 `MCU_Step()`，推进 MCU 时钟 12 周期，然后调用
`PCM_Update()`。PCM 每 `(reg_slots+1)×25` 周期才产生一帧音频。
SC-55mk2 的 reg_slots=15（16 voices），所以：

- **新帧周期** ≈ 400 MCU 周期
- **step/帧** ≈ 400/12 ≈ 33

早期代码假设每步都产生音频帧，用 `ceil(numSamples × speedRatio)` 作为步数上限，
结果只攒了 ~200 帧（实际需要 ~770 帧）。插值器读到了 scratch 尾部的垃圾数据
→ 噪音/错误音高。

**修复方案：Ring Buffer + 充足步数**

1. **Ring Buffer** (`mAudioRing[131072]`)：`sampleCallback` 把每帧写入环形缓冲区，
   `processBlock` 按序读取。确保每帧都被捕获、无重复、无遗漏。
2. **步数上限** `maxSteps = framesNeeded × 80 + 10000`：保证跑够帧数。
3. **动态采样率**：`PCM_GetOutputFrequency()` 获取真实 emuRate，不硬编码。

| 变量 | 含义 | 典型值 |
|---|---|---|
| `kAudioRingSize` | 环形缓冲大小 | 131072 (≈2s @ 66kHz) |
| `maxSteps` | Step 上限 | `framesNeeded × 80 + 10000` |
| `framesNeeded` | 目标音频帧数 | `ceil(numSamples × emuRate/dawRate) + getBaseLatency() + 2` |

Step 周期计算通用：
- MCU 每步 12 周期
- PCM 每帧 `(reg_slots+1)×25` 周期
- SC-55mk2（16 voices）：~33 步/帧
- `maxSteps = framesNeeded × 80 + 10000` 是安全的上界

> **设计取舍：无独立读指针**。`processBlock` 每次进入时快照 `mAudioRingWrite`，
> 步进后只 drain 从快照到新 write 位置的帧。`stepEmulatorForUi` 产出的帧虽然
> push 进了 ring，但在 `processBlock` 恢复时**被跳过**（不是 drain 的起点）。
> 这意味着 DAW 停止调用 `processBlock` 期间（如 REAPER 后台失焦），固件内部
> demo sequencer 产出的音频无法被消费 → demo 停播。这是已知限制，非 bug
> （Ableton Live 不受影响）。

#### 4.4.2 UI 定时器与音频线程锁竞争

`PluginEditor` 启动 60Hz 定时器，每次回调跑 `stepEmulatorForUi()`。
早期代码每次跑 50000 步，在 `mEmulatorMutex` 下执行。当编辑器窗口打开时，
UI 线程和音频线程（`processBlock`）竞争同一把锁，导致 `processBlock` 被延迟
→ 音频 glitch 和不稳定。

**修复方案：DAW 活跃检测**

```cpp
// processBlock() 开头:
mLastProcessBlockTimeMs = juce::Time::getMillisecondCounter();

// stepEmulatorForUi():
if (juce::Time::getMillisecondCounter() - mLastProcessBlockTimeMs < 50)
    return;  // DAW 正在处理，彻底跳过（零锁竞争、零 emulator 时间推进）
```

- Boot 阶段保持 50000 步/帧确保快速启动
- 启动后降至 2000 步/帧
- Ring Buffer 和 `mLastProcessBlockTimeMs` 方案均平台无关，VST3/AU 通用

#### 4.4.3 Boot priming

`loadROMsImpl()` 完成后设置 `mRemainingBootSteps = 500000`。Timer 以
50000 步/帧消耗，总计约 10 帧（~167ms @ 60fps）完成固件启动，使首帧
LCD 不为黑屏。消耗完毕后自动切换到 idle 模式（2000 步/帧）。

#### 4.4.4 线程安全模型

三把锁保护不同资源，互不嵌套：

| 锁 | 保护对象 | 持有者 | 备注 |
|---|---|---|---|
| `mEmulatorMutex` | emulator 全状态 + audio ring | `processBlock` (audio), `stepEmulatorForUi` (UI), `stepEmulator` (UI) | `sampleCallback` 在 `Step()` 内被调，已在锁内 |
| `lcd.mutex` | `lcd.buffer` / `LCD_Data` 等 | `LCD_Render` 用 `try_lock`（抢不到丢帧）；`timerCallback` 用 `lock_guard` 读 buffer | 非阻塞，避免 UI 卡 audio |
| 无锁 | `mcu.button_pressed` | `std::atomic<uint32_t>`，mouse/key handler 写，`Step()` 读 | lock-free |

关键约束：
- `mEmulatorMutex` 持有时间 = `Step()` 循环 + ring drain，应尽量短
- `stepEmulatorForUi` 在 DAW 活跃时彻底跳过（§4.4.2），零锁竞争
- `LCD_Render` 的 `try_lock` 策略确保渲染永远不阻塞 audio 线程

### 4.5 采样率转换

```
仿真器内部采样率 = PCM_GetOutputFrequency(pcm)
                  SC-55mk2/SCB-55 oversampling: 66207 Hz, 无: 33103 Hz
                  JV-880/MK1: 64000 Hz / 32000 Hz
DAW 项目采样率    = 44100 / 48000 / 88200 / 96000
```

- `speedRatio = emuRate / dawRate`（每输出样本消耗的输入样本数）
- 2× `juce::LagrangeInterpolator` (L/R 独立)
- `framesNeeded = ceil(numSamples × speedRatio) + getBaseLatency() + 2`
- `maxSteps = framesNeeded × 80 + 10000`（安全上界）
- `LagrangeInterpolator::process(speed, in, out, numOut)` 中 `speed` =
  "每输出样本消耗的输入样本数"（input_per_output）

### 4.6 MIDI 输入

```
processBlock() 中的 MidiBuffer → 遍历 message →
  emu.PostMIDI(message.getRawData(), message.getRawMessageSize())
```
JUCE 已处理 MIDI running status，直接传原始字节。

### 4.7 ROM 加载与自动发现

`ensureEmulatorReady()` 在构造函数、`prepareToPlay()`、`timerCallback()` 中被调用，
幂等初始化。ROM 路径来源优先级：

1. **已保存状态**：`setStateInformation()` 恢复的 `mRomDirectory`（DAW preset/session）
2. **macOS Application Support**：`~/Library/Application Support/NukedSC55/` 及其
   `roms/` 子目录
3. **Bundle-relative**：`dladdr` 获取插件二进制路径，向上遍历父目录查找
4. **DAW 路径 fallback**：`GetProcessPath()` 获取 DAW 可执行文件路径，向上遍历

每级用 `common::LoadRomset()` 做 hash 检测确认有效性。找到后调
`Emulator::LoadRoms()` → `Emulator::Reset()`，并设置
`mRemainingBootSteps = 500000` 启动 boot priming（§4.4.3）。

`setRomDirectory()` 允许外部（如未来 FileChooser UI）手动设置路径后触发重新加载。

状态持久化 XML 格式：
```xml
<NukedSC55 romDirectory="/path/to/roms" gain="1.0"/>
```
`setStateInformation` 恢复 ROM 目录后立即调 `ensureEmulatorReady()` 重新加载。

### 4.8 LCD 渲染

VST 不使用 SDL/OpenGL，而是通过 `VstLCDBackend`（继承 `LCD_Backend`）提供
dummy 渲染后端：`Start()`/`Stop()`/`Render()` 均为空操作。这使得 `LCD_Render()`
仍然填充 `lcd.buffer`（像素数据），但不实际显示。

`timerCallback()` 60fps 流程：
```
stepEmulatorForUi()           # 推进 MCU（含 LCD 控制器固件）
LCD_Render(lcd)               # 固件状态 → lcd.buffer 像素
lock(lcd.mutex):              # 读 buffer（try_lock 在 Render 内已释放）
  BGR888 → ARGB 逐像素拷贝    # lcd.buffer[y][x] → mLcdImage
repaint()                     # 触发 paint() 将 mLcdImage 绘制到屏幕
```

`paint()` 中额外渲染：
- **按钮 LED**：从背景图 sprite sheet 行 466+ 裁剪 lit 状态图块（ALL/MUTE/STANDBY）
- **型号徽章**：从 sprite sheet 根据 romset 绘制 MK1/MK2 徽章
- **音量旋钮**：从背景图裁剪旋钮 sprite + `AffineTransform` 旋转 + 4 条 gap-filling strip 填充旋转空洞

### 4.9 GUI 移植明细

| SDL 原始 | JUCE 实现 | 状态 |
|---|---|---|
| `SDL_Window` | `AudioProcessorEditor` | ✅ DAW 管理生命周期 |
| `SDL_Renderer` | `juce::Graphics` in `paint()` | ✅ |
| `lcd.buffer` 像素上传 | `Image::ARGB` + `BitmapData` 批量写入 | ✅ 60fps Timer |
| 背景 BMP | `sc55_background.png` 嵌入为 C 数组 (`EmbeddedResources.h`) | ✅ SC-55 only |
| `SDL_RenderCopy` (背景) | `g.drawImageAt()` | ✅ |
| `LCD_DrawKnob` (旋转 sprite) | `AffineTransform` + `drawImageTransformed()` | ✅ 从背景图 sprite 裁剪 |
| 按钮点击检测 | `mouseDown()` + `ButtonRegion[]` 区域检测 | ✅ 19 个按钮 |
| 按钮 LED | 从背景图 sprite sheet 裁剪 lit 状态 | ✅ ALL/MUTE/STANDBY |
| 旋钮拖拽 | `mouseDrag()` + `atan2` | ✅ 双击复位 |
| 键盘快捷键 | `keyPressed()` / `keyStateChanged()` | ✅ SC-55 + JV-880 |
| `LCD_VolumeChanged()` | `updateVolumeFromKnob()` → `AudioVolume` | ✅ |
| VST 参数 | `juce::AudioParameterFloat` (Gain 0.0–2.0) | ✅ |

### 4.10 VST 参数

| 参数 | 类型 | 范围 | 实现 |
|---|---|---|---|
| 增益 (Gain) | `AudioParameterFloat` | 0.0–2.0, skew 0.5 | `sampleCallback` 中 `Scale()` |
| GS Reset | 方法 | — | `triggerGsReset()` → SysEx |
| GM Reset | 方法 | — | `triggerGmReset()` → SysEx |
| ROM 目录 | XML 状态 | 路径字符串 | `getStateInformation` / `setStateInformation` |

> 音量旋钮控制 `AudioVolume`（硬件音量），Gain 参数独立 automatable。
> 两者不绑定——旋钮不改变 Gain 参数值。

---

## 5 文件结构

```
src/backend/                        ← 完全不动
src/common/                         ← 完全不动
src/renderer/                       ← 保留

data/
├── sc55_background.bmp             ← SDL 前端使用
├── sc55_background.png             ← VST 嵌入使用 (由 BMP 转换)
└── jv880_background.bmp            ← SDL 前端使用 (VST 暂未嵌入)

src/vst/                            ← VST 插件
├── PluginProcessor.h               ← juce::AudioProcessor 声明
├── PluginProcessor.cpp             ← processBlock, SRC, ROM 加载, 状态持久化
├── PluginEditor.h                  ← juce::AudioProcessorEditor 声明
├── PluginEditor.cpp                ← 面板渲染, LCD, 按钮/旋钮/键盘交互
└── EmbeddedResources.h             ← PNG 背景 + ROM 路径自动生成的 C 数组

external/JUCE/                      ← JUCE 8 (git clone, .gitignored)
```

> 没有 `LcdCanvas`、`KnobComponent`、`SrcResampler` 等独立组件文件——
> 所有逻辑直接写在 `PluginEditor.cpp` 和 `PluginProcessor.cpp` 中。

---

## 6 实现状态

### ✅ 构建基建
- JUCE 8 通过 `external/JUCE` 子目录集成
- `juce_add_plugin()` + `juce_generate_juceheader()`
- C++23, `COPY_PLUGIN_AFTER_BUILD` 自动安装
- VST3 + AU 双格式

### ✅ 核心音频/MIDI 管线
- `processBlock()`: MIDI → `PostMIDI()`, 循环 `Step()` + ring buffer 收集音频
- 采样率转换: 2× `LagrangeInterpolator`, `PCM_GetOutputFrequency()` 动态获取 emuRate
- ROM 加载: `common::LoadRomset()` 按 hash 检测 → `Emulator::LoadRoms()` → `Reset()`
- ROM 路径自动发现: 从插件二进制路径向上搜索 (`autoDiscoverRomDirectory()`)
- 增益参数: `AudioParameterFloat` (0.0–2.0, skew 0.5)
- GS/GM Reset: `PostSystemReset()` via `triggerGsReset()` / `triggerGmReset()`
- 状态持久化: XML 保存/恢复 ROM directory + gain
- 静音安全: 无 ROM 时 `buffer.clear()`

### ✅ GUI Editor
- 面板背景: PNG 嵌入, 2x→1x 缩放 (2240×466 → 1120×233)
- LCD: 60fps Timer, 锁 `lcd.mutex`, BGR888→ARGB 逐像素拷贝
- 无 ROM 时: 黑色半透明遮罩 + 提示文字
- 19 个按钮热区: `ButtonRegion` 数组 + `mouseDown/Up/Exit` → `button_pressed` atomic
- 按钮 LED: ALL/MUTE/STANDBY 从背景图 sprite sheet 裁剪
- 音量旋钮: 从背景图 sprite 裁剪, `AffineTransform` 旋转, 拖拽 + 双击复位
- 键盘热键: SC-55 (21 映射) + JV-880 (14 映射 + 编码器), `setWantsKeyboardFocus(true)`

### ✅ 音频 bug 修复
- Ring buffer 替代单帧缓存 (§4.4.1)
- UI 定时器 DAW 活跃检测, 避免锁竞争 (§4.4.2)

### ✅ 多 DAW 验证
- REAPER: VST3 正常
- Ableton Live: VST3 正常
- 打开/关闭编辑器窗口均无异常
- 频谱分析确认基频准确（A4 = 440Hz，C4 = 261.6Hz）
- 不同 buffer size（64/128/256/512/1024）均正常

---

## 7 风险和注意事项

| 风险 | 影响 | 缓解 |
|---|---|---|
| **MAME 非商业许可** | 不能销售/商业使用 | 个人/开源项目无影响。商业需换基础代码 |
| **实时性能** | `Step()` 含 MCU 仿真 + PCM 合成 | 现有 SDL 前端已证明实时可行 |
| **采样率差异** | emuRate ~66kHz → DAW 44.1/48kHz | `LagrangeInterpolator` 重采样 |
| **ROM 版权** | SC-55 firmware dump 有版权 | 用户自备 ROM。插件不分发。 |
| **JUCE 许可** | JUCE 8: GPLv3 或商业许可 | GPLv3 与 MAME-nc 兼容需确认 |
| **REAPER 后台行为** | DAW 失焦时停止调 `processBlock`, 固件 demo 停播 | DAW 行为差异, 非 bug。Ableton Live 不受影响 |

---

## 8 剩余待办

| 项 | 说明 |
|---|---|
| ROM 目录选择 UI | 当前依赖自动发现或 DAW preset 保存路径, 无 FileChooser 对话框 |
| JV-880 面板背景 | 键盘热键已支持 JV-880, 但面板背景仅 SC-55。需嵌入 `jv880_background.png` |
| auval 验证 | `auval -v aufx Nukd Sc55` 需确认 AU type code (`aufx` vs `aumu`) |
| 多平台测试 | macOS AU + VST3 已验证, Windows/Linux 未测 |

---

## 9 关键文件

| 文件 | 行数 | 作用 |
|---|---|---|
| `CMakeLists.txt` | — | VST 集成 + 插件目标 (约 50 行 VST 相关) |
| `src/vst/PluginProcessor.h` | 143 | `AudioProcessor` 声明: ring buffer, SRC, ROM, 参数 |
| `src/vst/PluginProcessor.cpp` | 482 | `processBlock`, 采样率转换, ROM 加载, 状态持久化 |
| `src/vst/PluginEditor.h` | 60 | `AudioProcessorEditor` 声明: 事件, Timer, 旋钮, 键盘 |
| `src/vst/PluginEditor.cpp` | 638 | 面板渲染, LCD, 按钮/旋钮/键盘交互, sprite 缓存 |
| `src/vst/EmbeddedResources.h` | ~12k | PNG 背景数据 (C 数组, 自动生成) |
| `data/sc55_background.png` | — | SC-55 面板 2x 位图 (由 BMP 转换) |
