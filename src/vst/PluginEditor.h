#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

//==============================================================================
class NukedSC55AudioProcessorEditor : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit NukedSC55AudioProcessorEditor(NukedSC55AudioProcessor&);
    ~NukedSC55AudioProcessorEditor() override;

    //==============================================================================
    void paint(juce::Graphics&) override;
    void resized() override;

    //==============================================================================
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;

    juce::MouseCursor getMouseCursor() override;

    //==============================================================================
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;

private:
    //==============================================================================
    void timerCallback() override;

    int findButtonAt(int x, int y);
    void updateVolumeFromKnob();

    // Romset-aware geometry helpers
    bool isJv880Romset() const;
    juce::Rectangle<int> getKnobBounds() const;
    juce::Rectangle<int> getLcdBounds() const;

    NukedSC55AudioProcessor& mProcessor;

    juce::Image mBackground;      // 1x cropped panel (1120×233)
    juce::Image mBackgroundFull;  // full 2x BMP (2240×588) for sprite access
    juce::Image mLcdImage;

    int mCurrentRomset = -1;  // -1 = unknown, tracks Romset enum value

    uint32_t mButtonsDown = 0;

    // Volume knob state
    float mKnobAngle = 4.18879f;         // 240° (SDL default), 0 = top, CW+
    bool  mKnobDragging = false;

    // JV-880 encoder (relative drag, 15° per step)
    bool  mEncoderDragging = false;
    float mEncoderLastAngle = 0.0f;
    float mEncoderAccumDelta = 0.0f;

    // Keyboard-pressed button bits (tracked separately from mouse for clean release)
    uint32_t mKeyboardBits = 0;

    // Cached 1x knob sprite and gap-filling strips (created once from mBackgroundFull)
    juce::Image mKnobSprite;
    juce::Image mKnobStripTop, mKnobStripBot, mKnobStripLeft, mKnobStripRight;

    void cacheKnobSprites();
    void loadBackgroundForCurrentRomset();

    // Romset selection menu — unobtrusive corner trigger
    juce::Rectangle<int> getRomsetMenuZone() const;
    void showRomsetMenu();
    bool mMenuHover = false;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NukedSC55AudioProcessorEditor)
};