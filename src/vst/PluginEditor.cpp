#include "PluginEditor.h"
#include "EmbeddedResources.h"
#include "backend/lcd.h"

#include <atomic>
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

// Volume knob geometry and limits
static const juce::Rectangle<int> kKnobBounds{ 153, 42, 59, 59 };
static constexpr float kKnobMinAngle   = 0.523599f;   //  30° in radians
static constexpr float kKnobMaxAngle   = 5.75959f;    // 330° in radians
static constexpr float kKnobDefAngle   = 4.18879f;    // 240° in radians
static constexpr float kKnobAngleRange = kKnobMaxAngle - kKnobMinAngle;

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
    // Load the 2x background PNG from embedded data
    juce::MemoryInputStream bgStream(
        _Users_john_Projects_Nuked_SC55_GUI_Float_data_sc55_background_png,
        _Users_john_Projects_Nuked_SC55_GUI_Float_data_sc55_background_png_len,
        false);
    auto rawBg = juce::ImageFileFormat::loadFrom(bgStream);

    if (rawBg.isValid())
    {
        // Keep the full 2x BMP (2240×588) for sprite access (badge, LEDs).
        // The bottom rows (466+) contain sprite sheets for model badges and lights.
        mBackgroundFull = rawBg;

        // Crop to panel area (top 2240×466), then scale down to 1x (1120×233).
        auto panel = rawBg.getClippedImage(juce::Rectangle<int>(0, 0, 2240, 466));
        mBackground = panel.rescaled(1120, 233, juce::Graphics::highResamplingQuality);
    }
    else
    {
        // Fallback: solid dark background with LCD placeholder
        mBackground = juce::Image(juce::Image::RGB, 1120, 233, true);
        juce::Graphics g(mBackground);
        g.fillAll(juce::Colour(0xFF1a1a1a));

        g.setColour(juce::Colours::white);
        g.setFont(14.0f);
        g.drawText("Nuked SC-55", mBackground.getBounds(), juce::Justification::centred, true);

        g.setColour(juce::Colour(0xFF0f6fff));
        g.fillRect(283, 49, 370, 134);
    }

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

NukedSC55AudioProcessorEditor::~NukedSC55AudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
int NukedSC55AudioProcessorEditor::findButtonAt(int x, int y)
{
    for (const auto& reg : kButtonRegions)
    {
        if (x >= reg.x && x < reg.x + reg.w && y >= reg.y && y < reg.y + reg.h)
            return reg.bit;
    }
    return -1;
}

void NukedSC55AudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    const float mx = event.position.x;
    const float my = event.position.y;

    // --- Volume knob takes priority ---
    if (kKnobBounds.contains(mx, my))
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
        const float cx = kKnobBounds.getCentreX();
        const float cy = kKnobBounds.getCentreY();
        float raw = std::atan2(my - cy, mx - cx);   // 0 = right, CCW+
        raw += 4.71239f;                              // shift so 0 = top (12 o'clock)
        if (raw < 0.0f)      raw += 6.28318f;
        if (raw >= 6.28318f) raw -= 6.28318f;
        mKnobAngle = std::max(kKnobMinAngle, std::min(kKnobMaxAngle, raw));
        updateVolumeFromKnob();
        repaint();
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
    if (!mKnobDragging)
        return;

    const float cx = kKnobBounds.getCentreX();
    const float cy = kKnobBounds.getCentreY();
    float raw = std::atan2(event.position.y - cy, event.position.x - cx);
    raw += 4.71239f;                              // 0 → top (12 o'clock)
    if (raw < 0.0f)      raw += 6.28318f;
    if (raw >= 6.28318f) raw -= 6.28318f;
    mKnobAngle = std::max(kKnobMinAngle, std::min(kKnobMaxAngle, raw));
    updateVolumeFromKnob();
    repaint();
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

    if (mButtonsDown != 0)
    {
        mProcessor.getEmulator().GetMCU().button_pressed.fetch_and(~mButtonsDown);
        mButtonsDown = 0;
        repaint();
    }
}

void NukedSC55AudioProcessorEditor::mouseExit(const juce::MouseEvent&)
{
    if (mButtonsDown != 0)
    {
        mProcessor.getEmulator().GetMCU().button_pressed.fetch_and(~mButtonsDown);
        mButtonsDown = 0;
        repaint();
    }
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

    const auto& b = kKnobBounds;
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
    }

    // 3. Draw the volume knob from the background sprite (matches SDL LCD_DrawKnob)
    if (mKnobSprite.isValid())
    {
        const auto& b = kKnobBounds;

        // --- Main rotated knob sprite ---
        // SDL_RenderCopyEx rotates CW by RAD2DEG(angle) with SDL_FLIP_VERTICAL.
        // In screen coords (y-down):
        //   SDL "angle" param = CW rotation.
        //   JUCE AffineTransform::rotation() = standard-math CCW.
        //   Standard-math CCW = visually CW in screen coords.
        //   So use +mKnobAngle (NOT negated) for visual CW match.
        auto vf = juce::AffineTransform::verticalFlip(static_cast<float>(b.getHeight()));
        auto rot = juce::AffineTransform::rotation(mKnobAngle,
                                                     b.getWidth() * 0.5f,
                                                     b.getHeight() * 0.5f);
        auto trans = juce::AffineTransform::translation(static_cast<float>(b.getX()),
                                                         static_cast<float>(b.getY()));
        auto transform = vf.followedBy(rot).followedBy(trans);

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
        const float cx = static_cast<float>(kKnobBounds.getCentreX());
        const float cy = static_cast<float>(kKnobBounds.getCentreY());
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
        g.drawImage(mLcdImage,
                    283, 49, 370, 134,                                    // destination (on-panel LCD area)
                    0, 0, mLcdImage.getWidth(), mLcdImage.getHeight());   // source (full LCD buffer)
    }

    // 6. If ROMs aren't loaded yet, overlay a message on the LCD area
    if (!mProcessor.areRomsLoaded())
    {
        // Semi-transparent dark overlay over the LCD
        g.setColour(juce::Colours::black.withAlpha(0.85f));
        g.fillRect(283, 49, 370, 134);

        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        g.drawText("No ROMs Loaded", 283, 52, 370, 22, juce::Justification::centred, true);

        // Build search-paths text from the auto-discovery results
        const auto& paths = mProcessor.getSearchedPaths();
        if (!paths.empty())
        {
            std::string text = "Searched:";
            for (size_t i = 0; i < paths.size() && i < 20; ++i)
                text += "\n" + paths[i];

            g.setFont(juce::Font(juce::FontOptions(9.0f)));
            g.setColour(juce::Colour(0xFFcccccc));
            g.drawMultiLineText(text, 290, 80, 355, juce::Justification::left);
        }
    }
}

void NukedSC55AudioProcessorEditor::resized()
{
    // Fixed-size panel — no layout to adjust
}