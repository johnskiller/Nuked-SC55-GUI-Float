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

private:
    //==============================================================================
    void timerCallback() override;

    static int findButtonAt(int x, int y);
    void updateVolumeFromKnob();

    NukedSC55AudioProcessor& mProcessor;

    juce::Image mBackground;      // 1x cropped panel (1120×233)
    juce::Image mBackgroundFull;  // full 2x BMP (2240×588) for sprite access
    juce::Image mLcdImage;

    uint32_t mButtonsDown = 0;

    // Volume knob state
    float mKnobAngle = 4.18879f;         // 240° (SDL default), 0 = top, CW+
    bool  mKnobDragging = false;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NukedSC55AudioProcessorEditor)
};