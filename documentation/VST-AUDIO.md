# VST3/AU Audio Fixes — Reference Memo

## 背景

移植 Nuked SC-55 为 VST3/AU 插件时，音频渲染管道遇到两个关键 bug。

## Bug 1: Step 次数 ≠ 音频帧数

### 根因

`Emulator::Step()` 调用 `MCU_Step()`，后者推进 MCU 时钟 12 周期，
然后调用 `PCM_Update()`。PCM 每 `(reg_slots+1)×25` 周期才产生一帧音频。
SC-55mk2 的 reg_slots=15（16 voices），所以：

- **新帧周期** ≈ 400 MCU 周期
- **step/帧** ≈ 400/12 ≈ 33

原始代码假设每步都产生音频帧，用了 `inputNeeded = ceil(numSamples * speedRatio)` 作为步数上限，
结果只攒了 ~200 帧（实际需要 ~770 帧）。插值器读到了 scratch 尾部的垃圾数据 → 噪音/错误音高。

### 修复

1. **Ring Buffer 替代 `mCurrentSample`**：`sampleCallback` 把每帧写入环形缓冲区，
   `processBlock` 按序读取。确保每帧都被捕获、无重复、无遗漏。

2. **步数上限改为 `framesNeeded × 80 + 10000`**：保证跑够帧数。

3. **使用 `PCM_GetOutputFrequency()`** 获取真实 emulator 输出频率（~66207 Hz），
   而不是硬编码 44100。

### 关键数据流

```
sampleCallback (MCU_PostSample 调用)
  ↓ push 到 mAudioRing[writePos++]
processBlock:
  snapshot startWrite = mAudioRingWrite
  for step in 0..maxSteps:
    Step()
    if mAudioRingWrite - startWrite >= framesNeeded: break
  drain: scratch[i] = mAudioRing[(startWrite + i) % kAudioRingSize]
  LagrangeInterpolator.process(speedRatio, scratch, out, numSamples)
```

### 要设置的变量

| 变量 | 含义 | 典型值 |
|---|---|---|
| `kAudioRingSize` | 环形缓冲大小 | 131072 |
| `maxSteps` | Step 上限 | `framesNeeded × 80 + 10000` |
| `framesNeeded` | 目标音频帧数 | `ceil(numSamples × emuRate/dawRate) + 6` |

## Bug 2: UI 定时器线程干扰音频

### 根因

`PluginEditor` 启动 60Hz 定时器，每次回调跑 `stepEmulatorForUi()`。
原始代码每次跑 50000 步，在 `mEmulatorMutex` 下执行。

当编辑器窗口打开时，UI 线程和音频线程（`processBlock`）竞争同一把锁，
导致 `processBlock` 被延迟 → 音频 glitch 和不稳定。

### 修复

1. **DAW 活跃检测**：`processBlock` 开头记录 `mLastProcessBlockTimeMs`。
   `stepEmulatorForUi()` 检查该时间戳，若上次 `processBlock` 距今 < 50ms，
   则**彻底跳过**步进（零锁竞争、零 emulator 时间推进）。

2. **减少空闲步进**：boot 阶段保持 50000 步/帧确保快速启动；
   启动后降至 2000 步/帧。

### 检测逻辑

```cpp
// stepEmulatorForUi():
if (juce::Time::getMillisecondCounter() - mLastProcessBlockTimeMs < 50)
    return;  // DAW 正在处理，跳过

// processBlock():
mLastProcessBlockTimeMs = juce::Time::getMillisecondCounter();
```

## AU 适配注意事项

1. **Ring Buffer 方案是平台无关的**，直接复用即可。

2. **`mAudioRing` 大小 131072 帧 ≈ 2s @ 64kHz**，足够覆盖 UI 步进盈余。
   AU 的 `processBlock` 调用方式类似 VST3，buffer size 由宿主决定。

3. **`mLastProcessBlockTimeMs` 检测逻辑**适用于任何调用 `processBlock` 的宿主。

4. Step 周期计算通用：
   - MCU 每步 12 周期
   - PCM 每帧 `(reg_slots+1)×25` 周期
   - 对于 SC-55mk2（16 voices）：~33 步/帧
   - `maxSteps = framesNeeded × 80 + 10000` 是安全的上界

5. **`PCM_GetOutputFrequency()` 返回值**：
   - SC-55mk2/SCB-55: 66207（有 oversampling）/ 33103（无）
   - JV-880/MK1: 64000 / 32000

6. **LagrangeInterpolator 的 speedRatio** = `emuRate / dawRate`。
   以 JUCE 的 `LagrangeInterpolator::process(speed, in, out, numOut)` 为例，
   `speed` 含义是"每输出样本消耗的输入样本数"（input_per_output）。

## 验证方法

1. 在两个以上 DAW 中测试（Reaper + Live 已验证）
2. 打开/关闭编辑器窗口均无异常
3. 频谱分析确认基频准确（A4 = 440Hz，C4 = 261.6Hz）
4. 不同 buffer size（64/128/256/512/1024）均正常