#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>
#include <vector>

#include "backend/audio.h"
#include "backend/emu.h"
#include "backend/lcd.h"
#include "common/rom_loader.h"

//==============================================================================
//==============================================================================
// Minimal LCD_Backend that allows LCD_Render() to populate lcd.buffer
// without actually displaying via SDL/OpenGL. The VST editor reads the
// buffer directly.
//==============================================================================
class VstLCDBackend : public LCD_Backend
{
public:
    bool Start(lcd_t&) override { return true; }
    void Stop() override {}
    void Render() override {}   // no-op: editor reads lcd.buffer directly
};

//==============================================================================
class NukedSC55AudioProcessor : public juce::AudioProcessor
{
public:
    //==============================================================================
    NukedSC55AudioProcessor();
    ~NukedSC55AudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==============================================================================
    const juce::String getName() const override { return "Nuked SC-55"; }

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    //==============================================================================
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    // Expose for editor
    //==============================================================================
    Emulator&     getEmulator()       { return mEmulator; }
    AudioVolume&  getVolumeControl()  { return mVolumeControl; }
    bool          isInitialized() const { return mInitialized; }
    bool          areRomsLoaded() const { return mRomsLoaded; }
    /// Returns true if the audio thread is currently stepping the emulator.
    /// The editor uses this to avoid concurrent stepping in the GUI timer.
    bool          isAudioThreadStepping() const { return mAudioThreadStepping.load(); }

    juce::AudioParameterFloat* getGainParam() const { return mGainParam; }

    const std::string& getRomDirectory() const { return mRomDirectory; }
    const std::vector<std::string>& getSearchedPaths() const { return mSearchedPaths; }
    void               setRomDirectory(const std::string& path);

    // Trigger resets via MIDI SysEx
    void triggerGsReset();
    void triggerGmReset();

private:
    //==============================================================================
    VstLCDBackend mLcdBackend;   // dummy backend so LCD_Render() populates buffer

    Emulator     mEmulator;
    AudioVolume  mVolumeControl;
    double       mCurrentSampleRate = 44100.0;
    bool         mInitialized = false;
    bool         mRomsLoaded = false;

    std::atomic<bool> mAudioThreadStepping{ false }; // true while processBlock steps

    //==============================================================================
    // Parameters
    //==============================================================================
    juce::AudioParameterFloat* mGainParam = nullptr;

    //==============================================================================
    // ROM loading
    //==============================================================================
    std::string                mRomDirectory;
    std::vector<std::string>   mSearchedPaths;
    AllRomsetInfo              mRomsetInfo;
    common::LoadRomsetResult   mLoadResult;

    //==============================================================================
    // Sample rate conversion (emulator @ 44.1kHz → DAW rate)
    //==============================================================================
    juce::LagrangeInterpolator mInterpolatorL;
    juce::LagrangeInterpolator mInterpolatorR;
    juce::AudioBuffer<float>   mScratchBuffer;

    //==============================================================================
    // Per-sample output collected during processBlock
    //==============================================================================
    AudioFrame<float> mCurrentSample{ 0, 0 };

    //==============================================================================
    static void sampleCallback(void* userdata, const AudioFrame<int32_t>& frame);

    bool loadROMs(const std::string& directory);

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NukedSC55AudioProcessor)
};