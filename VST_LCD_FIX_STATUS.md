# Nuked SC-55 VST/AU — LCD & Badge Fix Status

## What Works
- ROM loading and auto-discovery
- Audio output (processBlock stepping)
- MIDI input processing
- Panel background rendering at correct size (1120×233)
- Model badge (SC-55mkII) sprite drawing from full 2x BMP
- Volume knob rendering and interaction
- Button coordinate detection and press/release event delivery to MCU

## Fixes Attempted (current state in code)

### 1. LCD Backend (Dummy)
- Created `VstLCDBackend` class (no-op `Start/Stop/Render`) in `PluginProcessor.h`
- Set `lcd.backend = &mLcdBackend` after `Init()` in `prepareToPlay()`
- **Why**: Both `LCD_Render()` and `LCD_Write()` check `!lcd.backend` and return immediately if null. Without a non-null backend, text never renders into `lcd.buffer` and MCU firmware can't write LCD registers.

### 2. LCD Startup
- Added `mEmulator.StartLCD()` in `loadROMs()` after `Reset()`
- **Why**: Without `StartLCD()`, `lcd.width` and `lcd.height` stay at 0 permanently. `LCD_Start()` sets dimensions (741×268 for SC-55) and calls `lcd.backend->Start()`.

### 3. Timer Stepping for LCD Liveness
- Timer runs at 60fps in editor
- When DAW is idle (no concurrent audio stepping), steps emulator 50,000× per frame to keep MCU running
- **Why**: The MCU needs to execute firmware code (LCD_Write calls) to display content. `processBlock` only steps when DAW plays. Without timer stepping, LCD stays frozen/blank when stopped.

### 4. Concurrent Step Protection
- Added `std::atomic<bool> mAudioThreadStepping` flag in processor
- `processBlock()` sets flag true before its stepping loop, false after
- Timer checks flag before stepping — skips if audio thread is active
- **Why**: Prevent race condition between audio thread and GUI timer calling `emu.Step()` simultaneously.

### 5. Badge Drawing
- `mBackgroundFull` stores the full 2x BMP (2240×588) for sprite access
- Badge sprites are at rows 466+ (below the panel crop)
- Draws MK2 badge from source {1392, 466, 200, 104} → dest {696, 174, 100, 52}
- SC-55 text badge from source {1066, 516, 262, 50} → dest {533, 195, 131, 25}

### 6. Removed Button Highlight Overlay
- Removed the white semi-transparent overlay on buttons
- Standard frontend doesn't use overlays — relies on LCD and button LED sprites
- **Why**: Overlay was causing global darkening of panel (LCD, badge, GM/GS logos)

## Known Remaining Issues

### A. LCD Still Black (critical)
Despite all fixes, LCD remains black in both AU and VST3 formats.
- `lcd.backend` is set → `LCD_Render()` and `LCD_Write()` should work
- `StartLCD()` is called → dimensions should be 741×268
- Timer stepping should keep MCU running
- **Possible root causes (unconfirmed)**:
  1. Thread safety: `LCD_Write()` is called during `Step()` without locking, `LCD_Render()` uses `try_lock`. If `step()` and `LCD_Render()` race, display state may corrupt.
  2. The MCU firmware may need specific initialization sequence not triggered by simple `Step()` calls.
  3. The audio thread's `processBlock` may be interfering with the timer stepping despite the atomic flag (flag might be set true for the entire processBlock duration, not just the stepping loop).
  4. DAW-specific behavior differences between AU and VST3 audio processing callbacks.

### B. Button Events Not Reflected on LCD
- Button bits are sent to MCU via `button_pressed.fetch_or(bit)`
- MCU reads bits during I/O in `MCU_ReadP1()` during `Step()`
- LCD doesn't update (e.g., pressing PART doesn't change displayed part)
- **Possible cause**: Same as A — MCU may not be executing enough code or LCD state isn't being rendered.

### C. AU vs VST3 Behavioral Differences
- First test: AU worked briefly, VST3 didn't
- After DAW restart: both black
- Suggests timing-dependent issue (maybe initialization order or timer scheduling)

## File Structure

```cpp
src/vst/
├── PluginProcessor.h      # VstLCDBackend class, mAudioThreadStepping flag
├── PluginProcessor.cpp    # Init, StartLCD, processBlock with stepping flag
├── PluginEditor.h         # mBackgroundFull member, mProcessor reference
├── PluginEditor.cpp       # timer callback, paint (badge, LCD), button handling
└── PluginResources.h      # EmbeddedResources (background PNG)
```

## Key Code Paths

### Initialization
```
prepareToPlay()
  → mEmulator.Init(opts)         // LCD_Init (sets mcu ptr), backend=null
  → lcd.backend = &mLcdBackend   // install dummy backend
  → mInitialized = true
  → loadROMs(dir)                // Reset + StartLCD (sets 741×268)
```

### Timer (60fps)
```
timerCallback()
  → if isInitialized()
  → if !audioThreadStepping
      → Step() × 50000          // keep MCU alive when DAW idle
  → LCD_Render(lcd)             // render text to lcd.buffer
  → lock mutex → copy buffer → repaint
```

### paint()
```
1. Draw mBackground (cropped panel, 1120×233)
2. Draw volume knob
3. Draw model badge from mBackgroundFull (sprite sheet)
4. Draw LCD content from mLcdImage
5. If !areRomsLoaded: draw "No ROMs Loaded" overlay
```

## Next Steps (suggested)
1. Verify the atomic flag is working correctly — add debug logging or simplify to always-step approach
2. Try stepping 1,000,000× once on first timer callback instead of 50,000× every frame — maybe the MCU needs many more steps to reach the LCD initialization code
3. Check if `LCD_Start()` is truly being called (it depends on `StartLCD()` being called after ROMs are loaded)
4. Try removing the `isAudioThreadStepping()` check entirely and always step — if the race is benign for LCD-only access, this would confirm the flag is the blocker
5. As last resort: extract the text rendering code from `LCD_Render()` into a standalone function that doesn't require `lcd.backend`, bypassing the backend pattern entirely