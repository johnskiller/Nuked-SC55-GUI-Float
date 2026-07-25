#include "PluginEditor.h"
#include "EmbeddedResources.h"
#include "backend/lcd.h"
#include "backend/rom.h"

#include <atomic>
#include <filesystem>
#include <mutex>

//==============================================================================
// Button region: position on the 1x panel (1120×233) and corresponding MCU bit index
struct ButtonRegion
{
    int x, y, w, h;
    int bit;
};

static constexpr ButtonRegion kButtonRegions[] = {
    { 38,  36,  67, 19,  0  }, // POWER
    { 968, 38,  53, 18,  3  }, // INST_L
    { 1024, 38,  53, 18,  4  }, // INST_R
    { 754, 82,  26, 26,  5  }, // MUTE
    { 754, 35,  26, 26,  6  }, // ALL
    { 968, 178, 53, 18,  8  }, // MIDI_CH_L
    { 1024, 178, 53, 18,  9  }, // MIDI_CH_R
    { 968, 132, 53, 18,  10 }, // CHORUS_L
    { 1024, 132, 53, 18,  11 }, // CHORUS_R
    { 968, 85,  53, 18,  12 }, // PAN_L
    { 1024, 85,  53, 18,  13 }, // PAN_R
    { 903, 37,  53, 18,  14 }, // PART_R
    { 831, 178, 53, 18,  16 }, // KEY_SHIFT_L
    { 887, 178, 53, 18,  17 }, // KEY_SHIFT_R
    { 831, 132, 53, 18,  18 }, // REVERB_L
    { 887, 132, 53, 18,  19 }, // REVERB_R
    { 831, 85,  53, 18,  20 }, // LEVEL_L
    { 887, 85,  53, 18,  21 }, // LEVEL_R
    { 849, 37,  53, 18,  22 }, // PART_L
};

constexpr auto kNumButtons = sizeof(kButtonRegions) / sizeof(kButtonRegions[0]);

// JV-880 button regions — 1x panel coordinates (1436×200).
// The `bit` field is the MCU_BUTTON_* index (matches the array position).
static constexpr ButtonRegion kButtonRegionsJv880[] = {
    { 660, 129, 75, 25,  0 }, // CURSOR_L
    { 742, 129, 75, 25,  1 }, // CURSOR_R
    { 853, 129, 75, 25,  2 }, // TONE_SELECT
    { 976, 129, 75, 25,  3 }, // MUTE
    { 700,  34, 75, 75,  4 }, // DATA
    {1056, 129, 75, 25,  5 }, // MONITOR
    {1136, 129, 75, 25,  6 }, // COMPARE
    {1216, 129, 75, 25,  7 }, // ENTER
    {1216,  53, 75, 25,  8 }, // UTILITY
    {  25,  86, 60, 60,  9 }, // PREVIEW
    { 853,  53, 75, 25, 10 }, // PATCH_PERFORM
    { 976,  53, 75, 25, 11 }, // EDIT
    {1056,  53, 75, 25, 12 }, // SYSTEM
    {1136,  53, 75, 25, 13 }, // RHYTHM
};

constexpr auto kNumButtonsJv880 = sizeof(kButtonRegionsJv880) / sizeof(kButtonRegionsJv880[0]);

// Volume knob geometry and limits
static const juce::Rectangle<int> kKnobBounds     { 153, 42, 59, 59 };  // SC-55
static const juce::Rectangle<int> kKnobBoundsJv880{  23, 86, 59, 59 };  // JV-880

// JV-880 encoder dial (relative drag, 15° per step)
static const juce::Rectangle<int> kEncoderBoundsJv880{ 706, 39, 64, 64 };
static constexpr float kEncoderStep = 0.261799f;  // 15° in radians
static constexpr float kKnobMinAngle   = 0.523599f;   //  30° in radians
static constexpr float kKnobMaxAngle   = 5.75959f;    // 330° in radians
static constexpr float kKnobDefAngle   = 4.18879f;    // 240° in radians
static constexpr float kKnobAngleRange = kKnobMaxAngle - kKnobMinAngle;

// LCD screen area on the panel (1x coordinates)
static const juce::Rectangle<int> kLcdBounds     { 283, 49, 370, 134 };  // SC-55
static const juce::Rectangle<int> kLcdBoundsJv880{ 174, 83, 410,  50 };  // JV-880

//==============================================================================
// Keyboard mapping: matches SDL's button_map_sc55 / button_map_jv880
struct KeyMapping { juce::KeyPress key; int bitIndex; };

static const KeyMapping kSc55KeyMap[] = {
    { juce::KeyPress('q', 0, 0),                     MCU_BUTTON_POWER },
    { juce::KeyPress('w', 0, 0),                     MCU_BUTTON_INST_ALL },
    { juce::KeyPress('e', 0, 0),                     MCU_BUTTON_INST_MUTE },
    { juce::KeyPress('r', 0, 0),                     MCU_BUTTON_PART_L },
    { juce::KeyPress('t', 0, 0),                     MCU_BUTTON_PART_R },
    { juce::KeyPress('y', 0, 0),                     MCU_BUTTON_INST_L },
    { juce::KeyPress('u', 0, 0),                     MCU_BUTTON_INST_R },
    { juce::KeyPress('i', 0, 0),                     MCU_BUTTON_KEY_SHIFT_L },
    { juce::KeyPress('o', 0, 0),                     MCU_BUTTON_KEY_SHIFT_R },
    { juce::KeyPress('p', 0, 0),                     MCU_BUTTON_LEVEL_L },
    { juce::KeyPress('[', 0, 0),                     MCU_BUTTON_LEVEL_R },
    { juce::KeyPress('a', 0, 0),                     MCU_BUTTON_MIDI_CH_L },
    { juce::KeyPress('s', 0, 0),                     MCU_BUTTON_MIDI_CH_R },
    { juce::KeyPress('d', 0, 0),                     MCU_BUTTON_PAN_L },
    { juce::KeyPress('f', 0, 0),                     MCU_BUTTON_PAN_R },
    { juce::KeyPress('g', 0, 0),                     MCU_BUTTON_REVERB_L },
    { juce::KeyPress('h', 0, 0),                     MCU_BUTTON_REVERB_R },
    { juce::KeyPress('j', 0, 0),                     MCU_BUTTON_CHORUS_L },
    { juce::KeyPress('k', 0, 0),                     MCU_BUTTON_CHORUS_R },
    { juce::KeyPress(juce::KeyPress::leftKey, 0, 0),  MCU_BUTTON_PART_L },
    { juce::KeyPress(juce::KeyPress::rightKey, 0, 0), MCU_BUTTON_PART_R },
};

static const KeyMapping kJv880KeyMap[] = {
    { juce::KeyPress('p', 0, 0),                     MCU_BUTTON_PREVIEW },
    { juce::KeyPress(juce::KeyPress::leftKey, 0, 0),  MCU_BUTTON_CURSOR_L },
    { juce::KeyPress(juce::KeyPress::rightKey, 0, 0), MCU_BUTTON_CURSOR_R },
    { juce::KeyPress(juce::KeyPress::tabKey, 0, 0),   MCU_BUTTON_DATA },
    { juce::KeyPress('q', 0, 0),                     MCU_BUTTON_TONE_SELECT },
    { juce::KeyPress('a', 0, 0),                     MCU_BUTTON_PATCH_PERFORM },
    { juce::KeyPress('w', 0, 0),                     MCU_BUTTON_EDIT },
    { juce::KeyPress('e', 0, 0),                     MCU_BUTTON_SYSTEM },
    { juce::KeyPress('r', 0, 0),                     MCU_BUTTON_RHYTHM },
    { juce::KeyPress('t', 0, 0),                     MCU_BUTTON_UTILITY },
    { juce::KeyPress('s', 0, 0),                     MCU_BUTTON_MUTE },
    { juce::KeyPress('d', 0, 0),                     MCU_BUTTON_MONITOR },
    { juce::KeyPress('f', 0, 0),                     MCU_BUTTON_COMPARE },
    { juce::KeyPress('g', 0, 0),                     MCU_BUTTON_ENTER },
};

constexpr auto kSc55KeyMapSize  = sizeof(kSc55KeyMap)  / sizeof(kSc55KeyMap[0]);
constexpr auto kJv880KeyMapSize = sizeof(kJv880KeyMap) / sizeof(kJv880KeyMap[0]);

//==============================================================================
NukedSC55AudioProcessorEditor::NukedSC55AudioProcessorEditor(NukedSC55AudioProcessor& p)
    : AudioProcessorEditor(&p), mProcessor(p)
{
    // Background will be loaded when romset is detected (see timerCallback)
    mBackground = juce::Image(juce::Image::RGB, 1120, 233, true);
    juce::Graphics g(mBackground);
    g.fillAll(juce::Colour(0xFF1a1a1a));
    g.setColour(juce::Colours::white);
    g.setFont(14.0f);
    g.drawText("Loading...", mBackground.getBounds(), juce::Justification::centred, true);

    // Create initial LCD pixel buffer image (will be resized when emulator LCD is available)
    mLcdImage = juce::Image(juce::Image::ARGB, 741, 268, true);

    setSize(1120, 233);

    mProcessor.ensureEmulatorReady();

    // Poll the LCD state at 60 fps
    startTimerHz(60);

    // Pre-cache the 1x knob sprite and strips
    cacheKnobSprites();

    // Accept keyboard focus for hotkey support
    setWantsKeyboardFocus(true);
    grabKeyboardFocus();
}

void NukedSC55AudioProcessorEditor::loadBackgroundForCurrentRomset()
{
    if (!mProcessor.areRomsLoaded())
        return;

    auto& mcu = mProcessor.getEmulator().GetMCU();
    const int newRomset = static_cast<int>(mcu.romset);

    // Already loaded for this romset
    if (newRomset == mCurrentRomset)
        return;

    const bool isFirstLoad = (mCurrentRomset == -1);
    mCurrentRomset = newRomset;

    // JV-880 has its own background; all other romsets use the SC-55 background.
    const bool isJv880 = (mcu.romset == Romset::JV880);

    // Log ROM directory once, on the first load.
    const auto& romDir = mProcessor.getRomDirectory();
    if (isFirstLoad)
    {
        fprintf(stdout, "[Nuked SC-55] ROM directory: %s\n",
                romDir.empty() ? "(not set)" : romDir.c_str());
        fflush(stdout);
    }

    // Load the 2x background PNG from the ROM directory.
    // Priority: <romset>_background.png in the ROM directory, then embedded fallback.
    // Only SC-55 has an embedded fallback; if the JV-880 file is missing, fall back
    // to the SC-55 embedded image.
    juce::Image rawBg;
    std::string bgSource;

    const char* bgFilename = isJv880 ? "jv880_background.png" : "sc55_background.png";

    if (!romDir.empty())
    {
        auto pngPath = std::filesystem::path(romDir) / bgFilename;
        if (std::filesystem::exists(pngPath))
        {
            rawBg = juce::ImageFileFormat::loadFrom(juce::File(pngPath.string()));
            if (rawBg.isValid())
                bgSource = pngPath.string();
        }
    }

    if (!rawBg.isValid())
    {
        juce::MemoryInputStream bgStream(
            _Users_john_Projects_Nuked_SC55_GUI_Float_data_sc55_background_png,
            _Users_john_Projects_Nuked_SC55_GUI_Float_data_sc55_background_png_len,
            false);
        rawBg = juce::ImageFileFormat::loadFrom(bgStream);
        bgSource = isJv880 ? "embedded SC-55 fallback (JV-880 file not found)"
                           : "embedded fallback";
    }

    fprintf(stdout, "[Nuked SC-55] Background loaded from: %s\n", bgSource.c_str());
    fflush(stdout);

    if (rawBg.isValid())
    {
        // Keep the full 2x image for sprite access (badge, LEDs).
        mBackgroundFull = rawBg;

        if (isJv880)
        {
            // JV-880: the panel is the top 2872×400 (2x); sprite sheet rows for
            // the button LEDs live below it (y=400+). Crop the panel first so the
            // sprite rows aren't squashed into the displayed background.
            auto panel = rawBg.getClippedImage(juce::Rectangle<int>(0, 0, 2872, 400));
            mBackground = panel.rescaled(1436, 200, juce::Graphics::highResamplingQuality);
            setSize(1436, 200);
        }
        else
        {
            // SC-55: crop panel area (top 2240×466), then scale to 1x (1120×233).
            // The bottom rows (466+) contain sprite sheets for model badges and lights.
            auto panel = rawBg.getClippedImage(juce::Rectangle<int>(0, 0, 2240, 466));
            mBackground = panel.rescaled(1120, 233, juce::Graphics::highResamplingQuality);
            setSize(1120, 233);
        }
    }
    else
    {
        // Fallback: solid dark background with "Nuked SC-55" text
        mBackground = juce::Image(juce::Image::RGB, 1120, 233, true);
        juce::Graphics g(mBackground);
        g.fillAll(juce::Colour(0xFF1a1a1a));

        g.setColour(juce::Colours::white);
        g.setFont(14.0f);
        g.drawText("Nuked SC-55", mBackground.getBounds(), juce::Justification::centred, true);

        g.setColour(juce::Colour(0xFF0f6fff));
        g.fillRect(283, 49, 370, 134);

        setSize(1120, 233);
    }

    // Rebuild the sprite cache for the newly loaded background.
    cacheKnobSprites();

    repaint();
}

NukedSC55AudioProcessorEditor::~NukedSC55AudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
bool NukedSC55AudioProcessorEditor::isJv880Romset() const
{
    if (mProcessor.areRomsLoaded())
        return mProcessor.getEmulator().GetMCU().romset == Romset::JV880;
    // Fall back to the last-detected romset so geometry stays correct during
    // the brief unload/reload window when switching romsets.
    return mCurrentRomset == static_cast<int>(Romset::JV880);
}

juce::Rectangle<int> NukedSC55AudioProcessorEditor::getKnobBounds() const
{
    return isJv880Romset() ? kKnobBoundsJv880 : kKnobBounds;
}

juce::Rectangle<int> NukedSC55AudioProcessorEditor::getLcdBounds() const
{
    return isJv880Romset() ? kLcdBoundsJv880 : kLcdBounds;
}

int NukedSC55AudioProcessorEditor::findButtonAt(int x, int y)
{
    const ButtonRegion* regions = isJv880Romset() ? kButtonRegionsJv880 : kButtonRegions;
    const size_t count = isJv880Romset() ? kNumButtonsJv880 : kNumButtons;

    for (size_t i = 0; i < count; ++i)
    {
        const auto& reg = regions[i];
        if (x >= reg.x && x < reg.x + reg.w && y >= reg.y && y < reg.y + reg.h)
            return reg.bit;
    }
    return -1;
}

juce::Rectangle<int> NukedSC55AudioProcessorEditor::getRomsetMenuZone() const
{
    // Bottom-right corner hot zone — works for both SC-55 (1120×233) and
    // JV-880 (1436×200) panel sizes. Kept clear of all button regions.
    return getLocalBounds().removeFromBottom(30).removeFromRight(30);
}

void NukedSC55AudioProcessorEditor::showRomsetMenu()
{
    const auto& desired = mProcessor.getDesiredRomset();
    const auto names = GetParsableRomsetNames();
    auto* processor = &mProcessor;

    juce::PopupMenu menu;

    // Auto-detect — when active, show the detected romset in parentheses
    juce::String autoLabel { "Auto-detect" };
    if (desired.empty() && mProcessor.areRomsLoaded())
    {
        auto& mcu = mProcessor.getEmulator().GetMCU();
        autoLabel << "  (" << RomsetName(mcu.romset) << ")";
    }
    menu.addItem(1, autoLabel, true, desired.empty());

    menu.addSeparator();

    // Individual romsets — tick the one matching the desired selection
    for (size_t i = 0; i < names.size(); ++i)
    {
        const auto romset = static_cast<Romset>(i);
        const int itemId = static_cast<int>(i + 2);
        menu.addItem(itemId, RomsetName(romset), true, desired == names[i]);
    }

    // JUCE 8 disables modal loops in plugins — use the async API.
    // The processor owns the editor, so the raw pointer is safe in the callback.
    // `names` points to static data (rs_name_simple), valid for the program lifetime.
    const auto screenZone = localAreaToGlobal(getRomsetMenuZone());

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(screenZone),
        [processor, names](int chosen)
        {
            if (chosen == 0)
                return;  // dismissed without selection

            if (chosen == 1)
            {
                processor->switchRomset({});
            }
            else
            {
                const size_t idx = static_cast<size_t>(chosen - 2);
                if (idx < names.size())
                    processor->switchRomset(names[idx]);
            }
        });
}

void NukedSC55AudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    // --- Romset menu trigger (bottom-right corner) ---
    if (getRomsetMenuZone().contains(event.position.toInt()))
    {
        showRomsetMenu();
        return;
    }

    const float mx = event.position.x;
    const float my = event.position.y;

    // --- Volume knob takes priority ---
    const auto knob = getKnobBounds();
    if (knob.contains(mx, my))
    {
        mKnobDragging = true;

        // Double-click resets to default angle
        if (event.getNumberOfClicks() >= 2)
        {
            mKnobAngle = kKnobDefAngle;
            updateVolumeFromKnob();
            repaint();
            return;
        }

        // Absolute positioning: compute angle from mouse position
        const float cx = knob.getCentreX();
        const float cy = knob.getCentreY();
        float raw = std::atan2(my - cy, mx - cx);   // 0 = right, CCW+
        raw += 4.71239f;                              // shift so 0 = top (12 o'clock)
        if (raw < 0.0f)      raw += 6.28318f;
        if (raw >= 6.28318f) raw -= 6.28318f;
        mKnobAngle = std::max(kKnobMinAngle, std::min(kKnobMaxAngle, raw));
        updateVolumeFromKnob();
        repaint();
        return;
    }

    // --- JV-880 encoder (relative drag, not absolute) ---
    if (isJv880Romset() && kEncoderBoundsJv880.contains(static_cast<int>(mx), static_cast<int>(my)))
    {
        mEncoderDragging = true;
        const float cx = kEncoderBoundsJv880.getCentreX();
        const float cy = kEncoderBoundsJv880.getCentreY();
        mEncoderLastAngle = std::atan2(my - cy, mx - cx);
        mEncoderAccumDelta = 0.0f;
        return;
    }

    // --- Not the knob — test buttons ---
    const int bitIndex = findButtonAt(static_cast<int>(mx),
                                       static_cast<int>(my));
    if (bitIndex >= 0)
    {
        const uint32_t bit = 1u << bitIndex;
        mButtonsDown |= bit;
        mProcessor.getEmulator().GetMCU().button_pressed.fetch_or(bit);
        repaint();
    }
}

void NukedSC55AudioProcessorEditor::mouseDrag(const juce::MouseEvent& event)
{
    if (mKnobDragging)
    {
        const auto knob = getKnobBounds();
        const float cx = knob.getCentreX();
        const float cy = knob.getCentreY();
        float raw = std::atan2(event.position.y - cy, event.position.x - cx);
        raw += 4.71239f;                              // 0 → top (12 o'clock)
        if (raw < 0.0f)      raw += 6.28318f;
        if (raw >= 6.28318f) raw -= 6.28318f;
        mKnobAngle = std::max(kKnobMinAngle, std::min(kKnobMaxAngle, raw));
        updateVolumeFromKnob();
        repaint();
        return;
    }

    // JV-880 encoder: relative drag, trigger MCU_EncoderTrigger per 15° step
    if (mEncoderDragging)
    {
        const float cx = kEncoderBoundsJv880.getCentreX();
        const float cy = kEncoderBoundsJv880.getCentreY();
        const float angle = std::atan2(event.position.y - cy, event.position.x - cx);

        float delta = angle - mEncoderLastAngle;
        if (delta > juce::MathConstants<float>::pi)  delta -= juce::MathConstants<float>::twoPi;
        if (delta < -juce::MathConstants<float>::pi) delta += juce::MathConstants<float>::twoPi;

        mEncoderAccumDelta += delta;
        mEncoderLastAngle = angle;

        // Trigger encoder steps (15° = kEncoderStep)
        auto& mcu = mProcessor.getEmulator().GetMCU();
        while (mEncoderAccumDelta >= kEncoderStep)
        {
            MCU_EncoderTrigger(mcu, 1);  // CW
            mEncoderAccumDelta -= kEncoderStep;
        }
        while (mEncoderAccumDelta <= -kEncoderStep)
        {
            MCU_EncoderTrigger(mcu, 0);  // CCW
            mEncoderAccumDelta += kEncoderStep;
        }
    }
}

void NukedSC55AudioProcessorEditor::updateVolumeFromKnob()
{
    float norm = (mKnobAngle - kKnobMinAngle) / kKnobAngleRange; // 0..1
    norm = std::max(0.0f, std::min(1.0f, norm));

    auto& vol = mProcessor.getVolumeControl();
    vol.volume   = norm;
    vol.volume_fp = static_cast<uint32_t>(norm * 65535.0f);
}

void NukedSC55AudioProcessorEditor::mouseUp(const juce::MouseEvent&)
{
    mKnobDragging = false;
    mEncoderDragging = false;

    if (mButtonsDown != 0)
    {
        mProcessor.getEmulator().GetMCU().button_pressed.fetch_and(~mButtonsDown);
        mButtonsDown = 0;
        repaint();
    }
}

void NukedSC55AudioProcessorEditor::mouseExit(const juce::MouseEvent&)
{
    mMenuHover = false;
    mKnobDragging = false;
    mEncoderDragging = false;

    if (mButtonsDown != 0)
    {
        mProcessor.getEmulator().GetMCU().button_pressed.fetch_and(~mButtonsDown);
        mButtonsDown = 0;
        repaint();
    }
}

void NukedSC55AudioProcessorEditor::mouseMove(const juce::MouseEvent& event)
{
    const bool inZone = getRomsetMenuZone().contains(event.position.toInt());
    if (inZone != mMenuHover)
    {
        mMenuHover = inZone;
        repaint();
    }
}

juce::MouseCursor NukedSC55AudioProcessorEditor::getMouseCursor()
{
    return mMenuHover ? juce::MouseCursor::PointingHandCursor
                      : juce::MouseCursor::NormalCursor;
}

//==============================================================================
bool NukedSC55AudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (!mProcessor.areRomsLoaded())
        return false;

    auto& mcu = mProcessor.getEmulator().GetMCU();

    // Pick the key map matching the current romset
    const KeyMapping* map;
    int mapSize;
    if (mcu.romset == Romset::JV880)
    {
        map     = kJv880KeyMap;
        mapSize = kJv880KeyMapSize;
    }
    else
    {
        map     = kSc55KeyMap;
        mapSize = kSc55KeyMapSize;
    }

    for (int i = 0; i < mapSize; ++i)
    {
        if (key == map[i].key)
        {
            const uint32_t bit = 1u << map[i].bitIndex;

            // Ignore key repeat (bit already set from a previous press)
            if (!(mKeyboardBits & bit))
            {
                mKeyboardBits |= bit;
                mcu.button_pressed.fetch_or(bit);
                repaint();
            }
            return true;
        }
    }

    // JV-880 encoder keys (separate MCU API, not button_pressed bits)
    if (mcu.romset == Romset::JV880)
    {
        if (key == juce::KeyPress(',', 0, 0))
        {
            MCU_EncoderTrigger(mcu, 0);
            return true;
        }
        if (key == juce::KeyPress('.', 0, 0))
        {
            MCU_EncoderTrigger(mcu, 1);
            return true;
        }
    }

    return false;
}

bool NukedSC55AudioProcessorEditor::keyStateChanged(bool isKeyDown)
{
    if (!isKeyDown && mKeyboardBits != 0 && mProcessor.areRomsLoaded())
    {
        auto& mcu = mProcessor.getEmulator().GetMCU();
        mcu.button_pressed.fetch_and(~mKeyboardBits);
        mKeyboardBits = 0;
        repaint();
    }
    return false;
}

void NukedSC55AudioProcessorEditor::focusGained(FocusChangeType)
{
}

void NukedSC55AudioProcessorEditor::focusLost(FocusChangeType)
{
}

//==============================================================================
void NukedSC55AudioProcessorEditor::timerCallback()
{
    mProcessor.ensureEmulatorReady();

    // Check if romset changed and reload background if needed
    if (mProcessor.areRomsLoaded())
        loadBackgroundForCurrentRomset();

    if (!mProcessor.areRomsLoaded())
        return;

    auto& lcd = mProcessor.getEmulator().GetLCD();

    // Keep the MCU running when the DAW is idle (SDL uses a dedicated thread).
    mProcessor.stepEmulatorForUi();

    LCD_Render(lcd);

    // Lock to safely read the rendered buffer
    const std::lock_guard<std::mutex> lock(lcd.mutex);

    const int w = static_cast<int>(lcd.width);
    const int h = static_cast<int>(lcd.height);

    if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
        return;

    // Resize our local image if the LCD dimensions have changed
    if (mLcdImage.getWidth() != w || mLcdImage.getHeight() != h)
        mLcdImage = juce::Image(juce::Image::ARGB, w, h, true);

    // Copy BGR888 pixels from the emulator LCD buffer into our ARGB image
    {
        juce::Image::BitmapData data(mLcdImage, juce::Image::BitmapData::readWrite);

        for (int y = 0; y < h && y < data.height; ++y)
        {
            auto* pixelRow = reinterpret_cast<juce::PixelARGB*>(data.getLinePointer(y));

            for (int x = 0; x < w && x < data.width; ++x)
            {
                const uint32_t bgr = lcd.buffer[y][x];

                // BGR888 stored as 0x00BBGGRR
                const auto r = static_cast<uint8_t>(bgr & 0xFF);
                const auto g = static_cast<uint8_t>((bgr >> 8) & 0xFF);
                const auto b = static_cast<uint8_t>((bgr >> 16) & 0xFF);

                // Alpha = 255 (opaque). Premultiplied == non-premultiplied at full opacity.
                pixelRow[x].setARGB(255, r, g, b);
            }
        }
    }

    repaint();
}

//==============================================================================
// Pre-render the 1x knob sprite and gap strips from the 2x background.
// Called once in the constructor so paint() doesn't rescale every frame.
void NukedSC55AudioProcessorEditor::cacheKnobSprites()
{
    if (!mBackgroundFull.isValid())
        return;

    // Romset-correct knob geometry (falls back to SC-55 when ROMs aren't
    // loaded yet; cacheKnobSprites() is also called from the constructor,
    // but the early return above guards that case).
    const auto b = getKnobBounds();
    constexpr float sin45 = 0.7071067811865476f;
    const int s = b.getHeight() - static_cast<int>(std::floor(b.getHeight() * sin45));

    // 1x knob sprite
    {
        auto knob2x = mBackgroundFull.getClippedImage({
            b.getX() * 2,
            b.getY() * 2,
            b.getWidth() * 2,
            b.getHeight() * 2
        });
        mKnobSprite = knob2x.rescaled(b.getWidth(), b.getHeight(),
                                       juce::Graphics::highResamplingQuality);
    }

    if (s <= 0)
        return;

    // Top strip
    {
        auto s2x = mBackgroundFull.getClippedImage({
            b.getX() * 2,
            b.getY() * 2 - s * 2,
            b.getWidth() * 2,
            s * 2
        });
        mKnobStripTop = s2x.rescaled(b.getWidth(), s,
                                      juce::Graphics::highResamplingQuality);
    }

    // Bottom strip
    {
        auto s2x = mBackgroundFull.getClippedImage({
            b.getX() * 2,
            b.getY() * 2 + b.getHeight() * 2,
            b.getWidth() * 2,
            s * 2
        });
        mKnobStripBot = s2x.rescaled(b.getWidth(), s,
                                      juce::Graphics::highResamplingQuality);
    }

    // Left strip
    {
        auto s2x = mBackgroundFull.getClippedImage({
            b.getX() * 2 - s * 2,
            b.getY() * 2,
            s * 2,
            b.getHeight() * 2
        });
        mKnobStripLeft = s2x.rescaled(s, b.getHeight(),
                                       juce::Graphics::highResamplingQuality);
    }

    // Right strip
    {
        auto s2x = mBackgroundFull.getClippedImage({
            b.getX() * 2 + b.getWidth() * 2,
            b.getY() * 2,
            s * 2,
            b.getHeight() * 2
        });
        mKnobStripRight = s2x.rescaled(s, b.getHeight(),
                                        juce::Graphics::highResamplingQuality);
    }
}

//==============================================================================
void NukedSC55AudioProcessorEditor::paint(juce::Graphics& g)
{
    // 1. Draw the panel background image
    g.drawImageAt(mBackground, 0, 0);

    // 2. Button LEDs (ALL / MUTE / STANDBY) from sprite sheet rows 466+
    if (mProcessor.areRomsLoaded() && mBackgroundFull.isValid())
    {
        auto& mcu = mProcessor.getEmulator().GetMCU();
        auto& lcd = mProcessor.getEmulator().GetLCD();

        if (mcu.romset == Romset::MK1 || mcu.romset == Romset::MK2)
        {
            const uint32_t buttonEnable = lcd.button_enable.load();

            // ALL / MUTE share the same lit sprite tile in the 2x sheet.
            if ((buttonEnable & 1) != 0 || (buttonEnable & 2) != 0)
            {
                constexpr int sx = 0, sy = 466, sw = 52, sh = 52;
                constexpr int dw = 26, dh = 26;

                if ((buttonEnable & 1) != 0) // ALL
                    g.drawImage(mBackgroundFull, 754, 35, dw, dh, sx, sy, sw, sh);

                if ((buttonEnable & 2) != 0) // MUTE
                    g.drawImage(mBackgroundFull, 754, 82, dw, dh, sx, sy, sw, sh);
            }

            if ((buttonEnable & 4) != 0) // STANDBY
                g.drawImage(mBackgroundFull, 118, 42, 10, 10, 0, 518, 20, 20);
        }
        else if (mcu.romset == Romset::JV880)
        {
            const uint32_t buttonEnable = lcd.button_enable.load();

            // Sprite sheet (2x) layout for JV-880: button LED tiles start at
            // y=400 (unlit) / y=450 (lit), each 150×50. The panel occupies the
            // top 2872×400. mBackgroundFull holds the full 2x image.
            //
            // button_enable bit → button region mapping (matches SDL frontend):
            //   bit 0 (1):   MIDI Message LED (drawn separately, top-right)
            //   bit 1 (2):   EDIT
            //   bit 2 (4):   SYSTEM
            //   bit 3 (8):   RHYTHM
            //   bit 4 (16):  UTILITY
            //   bit 5 (32):  PATCH_PERFORM
            //   bit 6 (64):  MUTE
            //   bit 7 (128): MONITOR
            //   bit 8 (256): COMPARE
            //   bit 9 (512): ENTER

            // MIDI Message LED: source {150, 400|408, 40, 8} → dest {1355, 26, 20, 4}
            {
                const int sy = 400 + 8 * static_cast<int>((buttonEnable & 1) != 0);
                g.drawImage(mBackgroundFull, 1355, 26, 20, 4, 150, sy, 40, 8);
            }

            // 9 button LEDs — each drawn from the 150×50 (2x) tile scaled to the
            // button region (75×25 at 1x). Unlit (y=400) and lit (y=450) states
            // are always drawn so the LED appearance matches the hardware.
            struct Jv880Led { uint32_t bit; int regionIdx; };
            static constexpr Jv880Led jv880Leds[] = {
                {   2u, MCU_BUTTON_EDIT          },
                {   4u, MCU_BUTTON_SYSTEM        },
                {   8u, MCU_BUTTON_RHYTHM        },
                {  16u, MCU_BUTTON_UTILITY       },
                {  32u, MCU_BUTTON_PATCH_PERFORM },
                {  64u, MCU_BUTTON_MUTE          },
                { 128u, MCU_BUTTON_MONITOR       },
                { 256u, MCU_BUTTON_COMPARE       },
                { 512u, MCU_BUTTON_ENTER         },
            };

            for (const auto& led : jv880Leds)
            {
                const int sy = 400 + 50 * static_cast<int>((buttonEnable & led.bit) != 0);
                const auto& r = kButtonRegionsJv880[led.regionIdx];
                g.drawImage(mBackgroundFull, r.x, r.y, r.w, r.h, 0, sy, 150, 50);
            }
        }
    }

    // 3. Draw the volume knob from the background sprite (matches SDL LCD_DrawKnob)
    if (mKnobSprite.isValid())
    {
        const auto b = getKnobBounds();

        // --- Main rotated knob sprite ---
        // SDL_RenderCopyEx rotates CW by RAD2DEG(angle).
        // SC-55 uses SDL_FLIP_VERTICAL; JV-880 does NOT.
        // In screen coords (y-down), standard-math CCW = visually CW.
        auto rot = juce::AffineTransform::rotation(mKnobAngle,
                                                     b.getWidth() * 0.5f,
                                                     b.getHeight() * 0.5f);
        auto trans = juce::AffineTransform::translation(static_cast<float>(b.getX()),
                                                         static_cast<float>(b.getY()));
        auto transform = isJv880Romset()
            ? rot.followedBy(trans)
            : juce::AffineTransform::verticalFlip(static_cast<float>(b.getHeight()))
                  .followedBy(rot).followedBy(trans);

        g.drawImageTransformed(mKnobSprite, transform);

        // --- Gap-filling strips ---
        if (mKnobStripTop.isValid())
        {
            g.drawImageAt(mKnobStripTop,    b.getX(), b.getY() - 18);
            g.drawImageAt(mKnobStripBot,    b.getX(), b.getY() + b.getHeight());
            g.drawImageAt(mKnobStripLeft,   b.getX() - 18, b.getY());
            g.drawImageAt(mKnobStripRight,  b.getX() + b.getWidth(), b.getY());
        }

        // Sprite already carries the knob's physical indicator → no extra line/dot.
    }
    else
    {
        // Fallback: simple knob when background image is not available
        const auto knobBounds = getKnobBounds();
        const float cx = static_cast<float>(knobBounds.getCentreX());
        const float cy = static_cast<float>(knobBounds.getCentreY());
        const float radius = 26.0f;
        const float indicatorLen = 18.0f;

        // Dark circular base
        g.setColour(juce::Colour(0xFF2a2a2a));
        g.fillEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

        // Thin border ring
        g.setColour(juce::Colour(0xFF666666));
        g.drawEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.8f);

        // Indicator line
        constexpr float kAngleOffset = 4.71239f;
        const float ix = cx + std::cos(mKnobAngle - kAngleOffset) * indicatorLen;
        const float iy = cy + std::sin(mKnobAngle - kAngleOffset) * indicatorLen;
        g.setColour(juce::Colour(0xFF88ccff));
        g.drawLine(cx, cy, ix, iy, 2.5f);

        // Center dot
        g.fillEllipse(cx - 3.5f, cy - 3.5f, 7.0f, 7.0f);
    }

    // 4. Draw model badge from the sprite sheet (rows 466+ of the 2x BMP)
    if (mProcessor.areRomsLoaded() && mBackgroundFull.isValid())
    {
        auto& mcu = mProcessor.getEmulator().GetMCU();

        // Determine badge type from romset
        int badgeType = -1;
        if (mcu.romset == Romset::MK2)
            badgeType = 3;
        else if (mcu.romset == Romset::MK1)
            badgeType = 1; // generic MK1

        if (badgeType == 3)
        {
            // SC-55mkII: draw BOTH text badge AND large badge
            // Text badge: source {1066, 566, 262, 50} → dest {533, 195, 131, 25}
            int sx = 804 + 262 * (badgeType & 1);
            int sy = 466 + 50 * (badgeType >> 1);
            g.drawImage(mBackgroundFull,
                        533, 195, 131, 25,
                        sx, sy, 262, 50);
            // Large badge: source {1392, 466, 200, 104} → dest {696, 174, 100, 52}
            g.drawImage(mBackgroundFull,
                        696, 174, 100, 52,
                        1392, 466, 200, 104);
        }
        else if (badgeType >= 0)
        {
            // SC-55 text badge: source varies, dest {533, 195, 131, 25}
            int sx = 804 + 262 * (badgeType & 1);
            int sy = 466 + 50 * (badgeType >> 1);
            g.drawImage(mBackgroundFull,
                        533, 195, 131, 25,
                        sx, sy, 262, 50);
        }
    }

    // 5. Draw the LCD content into the LCD area on the panel
    //    (drawn last so it renders on top of the panel, like the standard frontend)
    if (mProcessor.isInitialized() && mLcdImage.getWidth() > 1 && mLcdImage.getHeight() > 1)
    {
        const auto lcdBounds = getLcdBounds();
        g.drawImage(mLcdImage,
                    lcdBounds.getX(), lcdBounds.getY(), lcdBounds.getWidth(), lcdBounds.getHeight(), // destination (on-panel LCD area)
                    0, 0, mLcdImage.getWidth(), mLcdImage.getHeight());                              // source (full LCD buffer)
    }

    // 6. If ROMs aren't loaded yet, overlay a message on the LCD area
    if (!mProcessor.areRomsLoaded())
    {
        const auto lcdBounds = getLcdBounds();
        const int lx = lcdBounds.getX();
        const int ly = lcdBounds.getY();
        const int lw = lcdBounds.getWidth();

        // Semi-transparent dark overlay over the LCD
        g.setColour(juce::Colours::black.withAlpha(0.85f));
        g.fillRect(lcdBounds);

        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        g.drawText("No ROMs Loaded", lx, ly + 3, lw, 22, juce::Justification::centred, true);

        // Build search-paths text from the auto-discovery results
        const auto& paths = mProcessor.getSearchedPaths();
        if (!paths.empty())
        {
            std::string text = "Searched:";
            for (size_t i = 0; i < paths.size() && i < 20; ++i)
                text += "\n" + paths[i];

            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.setColour(juce::Colour(0xFFcccccc));
            g.drawMultiLineText(text, lx + 7, ly + 31, lw - 15, juce::Justification::left);
        }
    }

    // 7. Romset menu indicator (bottom-right corner) — subtle three-dot
    //    overflow glyph that brightens on hover. Drawn last so it's always
    //    visible, even during the "No ROMs Loaded" state.
    {
        const auto zone = getRomsetMenuZone().toFloat().reduced(2.0f);

        if (mMenuHover)
        {
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRoundedRectangle(zone, 5.0f);
        }

        const float alpha = mMenuHover ? 0.85f : 0.22f;
        g.setColour(juce::Colours::white.withAlpha(alpha));

        const float dotSize = 4.0f;
        const float gap = 6.0f;
        const float totalW = dotSize * 3.0f + gap * 2.0f;
        const float startX = zone.getCentreX() - totalW * 0.5f;
        const float y = zone.getCentreY() - dotSize * 0.5f;

        for (int i = 0; i < 3; ++i)
            g.fillEllipse(startX + i * (dotSize + gap), y, dotSize, dotSize);
    }
}

void NukedSC55AudioProcessorEditor::resized()
{
    // Fixed-size panel — no layout to adjust
}