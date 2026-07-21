# Nuked SC-55 → VST3/AU Plugin 移植方案

## 1. 项目背景

**Nuked SC-55** 是一个精确到指令级的 Roland Sound Canvas SC-55 硬音源仿真器（也支持 JV-880、SC-155）。
当前架构：C++20，CMake 构建，SDL2 桌面前端 + 独立离线渲染器。

本文件记录将其移植为 VST3/AU 插件（保留硬件面板 GUI）的调研结论和实施方案。

---

## 2. 当前架构

```
┌──────────────────────────────────────────────────────────┐
│  src/backend/  (nuked-sc55-backend)  ← 纯 C++ 仿真引擎   │
│  • MCU 模拟 (NEC V50)  • PCM 合成  • ROM 加载            │
│  • 音频回调: mcu_sample_callback(void*, AudioFrame<int32>)│
│  • 无平台依赖、无 GUI 依赖                                │
│  • 无采样率固定 (~22kHz 无 oversampling / ~44kHz 有)      │
├──────────────────────────────────────────────────────────┤
│  src/common/   (nuked-sc55-common)                       │
│  • ROM 加载器  • 增益计算  • 路径工具                     │
├──────────────────────────────────────────────────────────┤
│  src/standard/  (SDL2 桌面前端)  ← 被替换的目标           │
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
| `Emulator::Step()` | `() → void` | 推进一帧仿真（触发一次回调） |
| `mcu_sample_callback` | `typedef void(*)(void*, const AudioFrame<int32_t>&)` | 每 sample 回调，立体声 int32 |

---

## 3. GUI 现状分析

当前 GUI (`src/standard/lcd_sdl.h/cpp`) 由四层构成：

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
用 JUCE 框架，原因：
- 原生 VST3/AU/AAX 支持
- `AudioProcessorEditor` 提供 DAW 内嵌编辑器窗口
- 像素级 Graphics API，完美匹配 LCD framebuffer 渲染
- BMP/PNG 可编译为 BinaryData
- 内置 Lagrange 插值器用于采样率转换

### 4.2 推荐框架：JUCE 7+

| 特性 | 理由 |
|---|---|
| `juce::AudioProcessor` | `processBlock()` 替换当前的 `FE_RunInstanceSDL` |
| `juce::AudioProcessorEditor` | DAW 管理窗口生命周期，替代 `SDL_CreateWindow` |
| `juce::Image` | 加载 BMP 做背景，像素操作渲染 LCD |
| `juce::Graphics` | `drawImage()` composite，`drawImageTransformed()` 旋钮旋转 |
| `juce::BinaryData` | BMP/PNG 编译进插件，无需文件路径 |
| `juce::LagrangeInterpolator` | 仿真器 44.1kHz → DAW 48kHz 重采样 |

备选方案: iPlug2 (更轻量，但社区和文档不如 JUCE)。

### 4.3 CMake 集成

在现有 `CMakeLists.txt` 中加入 JUCE 依赖：

```cmake
# 伪代码结构
add_subdirectory(JUCE)  # 或 FetchContent

add_library(nuked-sc55-vst SHARED
    src/vst/PluginProcessor.cpp
    src/vst/PluginEditor.cpp
)
target_link_libraries(nuked-sc55-vst
    PRIVATE
    nuked-sc55-backend
    nuked-sc55-common
    juce::juce_audio_plugin_client
    juce::juce_graphics
    juce::juce_gui_basics
)
set_target_properties(nuked-sc55-vst PROPERTIES
    JUCE_VST3_MANUFACTURER "Nuked"
    JUCE_VST3_MANUFACTURER_CODE "Nukd"
    JUCE_VST3_PLUGIN_CODE "SC55"
)
```

### 4.4 音频管线

**当前 (SDL 独立线程)**:
```
独立线程: while(running) { emu.Step(); }
  → 回调 mcu_sample_callback
  → Normalize + Gain → ringbuffer
SDL 音频回调 → 从 ringbuffer 读取 → 输出
```

**VST (DAW 音频线程同步)**:
```
DAW 音频线程:
  processBlock(outputBuffer, midiMessages):
    for each MIDI message:
      emu.PostMIDI(message)
    for sample = 0 to blockSize:
      emu.Step()
      → 回调写入 outputBuffer.getSample(0, sample), (1, sample)
```

这更简单——不需要中间线程或 ringbuffer。`Step()` 由 DAW 音频线程直接驱动。

### 4.5 采样率转换

```
仿真器内部采样率 = PCM_GetOutputFrequency(pcm)
                  ≈ 22050 (no oversampling) 或 44100 (oversampling)
DAW 项目采样率    = 44100 / 48000 / 88200 / 96000
```

方案：采样率锁定为 DAW 项目采样率。
- DAW 44.1kHz: 开 oversampling → 1:1，无转换
- DAW 48kHz: 需要 SRC，用 `juce::LagrangeInterpolator`
- 较长 blockSize 下也可以一次产生多个 Step

### 4.6 MIDI 输入

直接映射：
```
processBlock() 中的 MidiBuffer → 遍历 message →
  emu.PostMIDI(message.getRawData(), message.getRawDataSize())
```
注意 MIDI running status——JUCE 已经处理好，直接传原始字节即可。

### 4.7 GUI 移植明细

| 现有 (SDL) | JUCE 等效 | 要点 |
|---|---|---|
| `SDL_Window` | `AudioProcessorEditor` | DAW 管理，开窗口时调用 `editor->setVisible(true)` |
| `SDL_Renderer` | `juce::Graphics` | 在 `paint(Graphics&)` 中绘制 |
| `m_lcd->buffer` 像素上传 | `Image::ARGB` + `setPixelAt()` | 或 `Image::BitmapData` 批量写入 |
| `SDL_UpdateTexture` | 每帧调用 `repaint()` → `paint()` | 用 Timer 或 AudioProcessor 的 `updateHostDisplay()` |
| 背景 BMP | `ImageFileFormat::loadFrom(BinaryData)` + `drawImageAt()` | BMP 编译为 BinaryData |
| `SDL_RenderCopy` (背景/按钮) | `g.drawImage()` / `g.drawImageWithin()` | 注意 Retina 缩放 (`component.setScale`) |
| `LCD_DrawKnob` (旋转 sprite) | `g.addTransform(AffineTransform::rotation())` + `drawImageTransformed()` | 以旋钮中心旋转 |
| 按钮点击检测 | `mouseDown()` + `HitTest` / `Component::contains()` | 用 `Rectangle<int>` 数组做区域检测，逻辑完全照搬 |
| 旋钮拖拽 | `mouseDrag()` 计算角度差 | `atan2()` 逻辑直接复制 |
| 键盘快捷键 | `keyPressed()` / `keyStateChanged()` | SDL scancode map → JUCE `KeyPress` |
| `SDL_WINDOWEVENT_CLOSE` | `AudioProcessorEditor` 的 `closeButtonPressed()` | 不用退出 DAW，只需通知 "关闭编辑器" |
| `LCD_VolumeChanged()` | 复用原函数 | 输出增益映射为 `AudioProcessor` 参数 |
| 多 inst 路由 | 单个 VST 实例通常不需要 | VST 插件本身就是一个实例 (简化) |

### 4.8 VST 参数映射

| 参数 | 类型 | 范围 | 映射 |
|---|---|---|---|
| 音量 | float 0-1 | `AudioVolume` | `lcd.volume` + `Out_SDL_SetVolume` 逻辑 |
| GS/GM Reset | 按钮 | `EMU_SystemReset` | `PostSystemReset()` |
| Oversampling | switch | on/off | `pcm.disable_oversampling` |
| 增益 (Gain) | float dB | -24 ~ +24 | `Scale()` 系数 |

---

## 5 文件结构

```
src/backend/                        ← 完全不动
src/common/                         ← 完全不动
src/renderer/                       ← 保留

data/
├── sc55_background.bmp             ← 转为 PNG 后嵌入 BinaryData
└── jv880_background.bmp            ← 转为 PNG 后嵌入 BinaryData

src/vst/                            ← 新建
├── CMakeLists.txt                  ← JUCE 构建集成
├── PluginProcessor.h               ← juce::AudioProcessor
├── PluginProcessor.cpp             ← processBlock(), 参数管理, ROM 初始化
├── PluginEditor.h                  ← juce::AudioProcessorEditor
├── PluginEditor.cpp                ← paint(), 鼠标事件, 键盘事件
├── LcdCanvas.h                     ← LCD 像素渲染逻辑 (原 lcd_sdl 的渲染部分)
├── LcdCanvas.cpp
├── KnobComponent.h                 ← 旋钮组件 (可选，也可以直接在 Editor 中处理)
├── KnobComponent.cpp
├── SrcResampler.h                  ← 采样率转换 (封装 LagrangeInterpolator)
├── SrcResampler.cpp
├── BinaryData/                     ← JUCE 自动生成 (资源文件)
│   ├── sc55_background.png
│   └── jv880_background.png
└── JuceLibraryCode/                ← JUCE 自动生成 (Projucer 或 CMake)
```

---

## 6 实施步骤 (按依赖顺序)

### Phase 1: 构建基建
1. 引入 JUCE (via FetchContent 或 git submodule)
2. 在 CMakeLists.txt 中添加 `nuked-sc55-vst` 目标
3. 确认 SDL 前端可独立编译（不影响 VST 目标）

### Phase 2: 核心音频/MIDI 管线
4. 实现 `PluginProcessor`:
   - `processBlock()`: MIDI → `PostMIDI()`, 循环 `Step()` + 收集音频
   - 采样率转换 (LagrangeInterpolator)
   - 参数管理 (juce::AudioParameterFloat/Float)
5. 实现 ROM 初始化流程:
   - 插件的 `initialize()` 或首次 `prepareToPlay()` 时加载 ROM
   - ROM 路径参数化

### Phase 3: GUI 移植
6. 实现 `PluginEditor`:
   - 背景图加载与绘制
   - LCD buffer 像素渲染
   - 按钮热区 + 点击处理
   - 旋钮绘制 + 拖拽交互
   - 键盘快捷键
7. 绑定 VST 参数与 GUI 控件

### Phase 4: 收尾
8. 测试多采样率 (44.1k/48k/96k)
9. 测试多平台 (macOS AU + VST3, Windows VST3)
10. 文档 + 许可声明（MAME 非商业许可）

---

## 7 风险和注意事项

| 风险 | 影响 | 缓解 |
|---|---|---|
| **MAME 非商业许可** | 不能销售/商业使用 | 个人/开源项目无影响。商业需换基础代码 |
| **实时性能** | `Step()` 含 MCU 仿真 + PCM 合成 | 现有 SDL 前端已证明实时可行。Profile bad case |
| **采样率差异** | 44.1kHz 仿真 → 48kHz 宿主 | Lagrange 插值。或内部锁 44.1kHz（限制使用场景） |
| **ROM 版权** | SC-55 firmware dump 有版权 | 用户自备 ROM。插件不分发。 |
| **JUCE 许可** | JUCE 6/7: GPLv3 或商业许可 | GPLv3 与 MAME-nc 兼容需确认。或改用 iPlug2 |
| **多实例/MIDI 路由** | 当前前端支持 16 实例轮询 | VST 实例本身是单实例。如需多实例可用多个插件轨道 |
| **LCD 刷新率** | SDL 是 15ms 循环，VST 用 Timer | 用 `Timer` 或 DAW 的 `postUpdate()` |

---

## 8 关键结论

1. **技术上完全可行**。后端音频/MIDI 接口 (`Step()` + `PostMIDI()` + `mcu_sample_callback`) 天然适配 VST 插件架构。
2. **GUI 保留可行且合理**。SC-55 的设备面板本身就是软音源 GUI 的理想形态。JUCE 可以完整复现 SDL 的 bitmap+旋钮+按钮 渲染方式。
3. **管线反而更简单**。VST 的 `processBlock()` 同步驱动 `Step()`，无需独立仿真线程和 ringbuffer。
4. **唯一硬限制是 MAME 许可**。发布的插件不能收费、不能用于商业音乐制作。