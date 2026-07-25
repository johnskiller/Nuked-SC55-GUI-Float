#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <mutex>
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

    /// Initialise emulator + auto-discover/load ROMs (safe to call repeatedly).
    void ensureEmulatorReady();

    /// Step the emulator under the shared lock (GUI + audio thread).
    void stepEmulator(int steps);

    /// Timer-driven stepping: keeps MCU alive when DAW is idle + boot priming.
    void stepEmulatorForUi();

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
    bool         mRomsLoaded  = false;
    bool         mDiscoveryAttempted = false;
    std::mutex   mEmulatorMutex;
    int          mRemainingBootSteps = 0;

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
    // Sample rate conversion (emulator native rate → DAW rate)
    //==============================================================================
    juce::LagrangeInterpolator mInterpolatorL;
    juce::LagrangeInterpolator mInterpolatorR;
    juce::AudioBuffer<float>   mScratchBuffer;

    //==============================================================================
    // Audio ring buffer — producer (sampleCallback) and consumer (processBlock)
    // both run under mEmulatorMutex, so no atomics needed.
    //==============================================================================
    static constexpr int kAudioRingSize = 131072;  // 128k frames ≈ 2s @ 64k
    AudioFrame<float>    mAudioRing[kAudioRingSize];
    uint32_t             mAudioRingWrite = 0;

    // Temporary frame for normalize/scale in callback
    AudioFrame<float>    mCurrentSample{ 0, 0 };

    // Timestamp of the most recent processBlock call (millisecond counter).
    // Used by stepEmulatorForUi() to detect DAW activity and skip stepping.
    std::atomic<uint32_t> mLastProcessBlockTimeMs{0};

    //==============================================================================
    static void sampleCallback(void* userdata, const AudioFrame<int32_t>& frame);

    bool loadROMsImpl(const std::string& directory);

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NukedSC55AudioProcessor)
};