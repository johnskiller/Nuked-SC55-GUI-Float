#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "backend/pcm.h"

#include <JuceHeader.h>
#include <filesystem>
#include <dlfcn.h>

//==============================================================================
// Get the plugin's own binary path using dladdr.
// Unlike GetProcessPath() (which returns the DAW's main executable path
// when running as a plugin), this returns the actual plugin bundle path.
static std::string getPluginBinaryPath()
{
    Dl_info info;
    // Use a known function defined in this translation unit to find
    // which shared library/bundle contains our code.
    if (dladdr((const void*)&createPluginFilter, &info) != 0)
        return info.dli_fname ? std::string(info.dli_fname) : std::string();
    return {};
}

//==============================================================================
// Standard location for ROM files on macOS
static std::string getAppSupportRomDirectory()
{
    // ~/Library/Application Support/Nuked SC-55/
    const char* home = getenv("HOME");
    if (!home)
        return {};
    return (std::filesystem::path(home) / "Library" / "Application Support" / "Nuked SC-55").generic_string();
}

//==============================================================================
// Try to find a ROM directory.
// Checks (in order):
//   1. ~/Library/Application Support/Nuked SC-55/
//   2. ~/Library/Application Support/Nuked SC-55/roms/
//   3. Walking up from the plugin bundle path (bundle-relative)
//   4. Walking up from the DAW path (legacy fallback, unlikely to find ROMs)
//
// Returns the first directory that contains a complete SC-55 romset, and
// populates `searchedPaths` with every path that was checked (for diagnostics).
static std::string autoDiscoverRomDirectory(std::vector<std::string>& searchedPaths)
{
    searchedPaths.clear();

    // Helper lambda: check if a directory contains valid ROMs by running hash detection.
    // Returns the path if valid, empty string otherwise.
    auto tryDirectory = [&](const std::string& path) -> std::string {
        searchedPaths.push_back(path);
        AllRomsetInfo testInfo;
        common::RomOverrides overrides{};
        common::LoadRomsetResult testResult;
        auto err = common::LoadRomset(testInfo, path, "", false, overrides, testResult);
        if (static_cast<int>(err) == 0)
            return path;
        return {};
    };

    // --- Priority 1: macOS Application Support ---
    {
        auto appSupport = getAppSupportRomDirectory();
        if (!appSupport.empty())
        {
            searchedPaths.push_back(appSupport + " (Application Support)");
            // Check directory itself
            {
                auto found = tryDirectory(appSupport);
                if (!found.empty()) return found;
            }
            // Check roms/ subdirectory
            {
                auto found = tryDirectory(appSupport + "/roms");
                if (!found.empty()) return found;
            }
        }
    }

    // --- Priority 2: Bundle-relative (dladdr path) ---
    {
        auto pluginPath = getPluginBinaryPath();
        if (!pluginPath.empty())
        {
            searchedPaths.push_back(pluginPath + " (plugin bundle)");
            auto dir = std::filesystem::path(pluginPath).parent_path();

            // Walk up 8 levels looking for ROMs or roms/ subdirectories
            static constexpr int kMaxWalk = 8;
            for (int i = 0; i < kMaxWalk && !dir.empty(); ++i)
            {
                auto found = tryDirectory(dir.generic_string());
                if (!found.empty()) return found;

                found = tryDirectory((dir / "roms").generic_string());
                if (!found.empty()) return found;

                dir = dir.parent_path();
            }
        }
    }

    // --- Priority 3: Fallback — walk up from DAW path ---
    {
        auto dir = std::filesystem::current_path();
        static constexpr int kMaxWalk = 4;
        for (int i = 0; i < kMaxWalk && !dir.empty(); ++i)
        {
            auto found = tryDirectory(dir.generic_string());
            if (!found.empty()) return found;

            found = tryDirectory((dir / "roms").generic_string());
            if (!found.empty()) return found;

            dir = dir.parent_path();
        }
    }

    return {};
}

//==============================================================================
// Static callback — called by the emulator for each sample
void NukedSC55AudioProcessor::sampleCallback(void* userdata, const AudioFrame<int32_t>& frame)
{
    auto& self = *static_cast<NukedSC55AudioProcessor*>(userdata);

    // Normalize int32 → float
    Normalize(frame, self.mCurrentSample, self.mVolumeControl);

    // Apply gain
    const float gain = self.mGainParam ? self.mGainParam->get() : 1.0f;
    Scale(self.mCurrentSample, gain);
}

//==============================================================================
NukedSC55AudioProcessor::NukedSC55AudioProcessor()
    : AudioProcessor (BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // Gain parameter: linear 0.0–2.0, skew 0.5 for finer low-end control
    mGainParam = new juce::AudioParameterFloat(
        "gain", "Gain",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f, 0.5f),
        1.0f,
        juce::AudioParameterFloatAttributes{}.withLabel(" gain"));
    addParameter(mGainParam);

    // Do not wait for prepareToPlay — GUI and ROM/LCD setup must work when the
    // editor opens, even if the DAW has not started the audio graph yet.
    ensureEmulatorReady();
}

NukedSC55AudioProcessor::~NukedSC55AudioProcessor()
{
    if (mInitialized)
        mEmulator.StopLCD();
}

//==============================================================================
void NukedSC55AudioProcessor::ensureEmulatorReady()
{
    std::lock_guard<std::mutex> lock(mEmulatorMutex);

    if (!mInitialized)
    {
        EMU_Options opts{};
        opts.lcd_backend   = &mLcdBackend;
        opts.rom_directory = mRomDirectory;

        if (!mEmulator.Init(opts))
        {
            DBG("Nuked SC-55: Emulator init failed");
            return;
        }

        mEmulator.SetSampleCallback(sampleCallback, this);
        mInitialized = true;
    }

    if (!mRomsLoaded && mRomDirectory.empty())
    {
        auto discovered = autoDiscoverRomDirectory(mSearchedPaths);
        if (!discovered.empty())
        {
            DBG("Nuked SC-55: Auto-discovered ROM directory: " + discovered);
            mRomDirectory = discovered;
        }
    }

    if (!mRomsLoaded && !mRomDirectory.empty())
        loadROMsImpl(mRomDirectory);
}

void NukedSC55AudioProcessor::stepEmulator(int steps)
{
    if (steps <= 0 || !mRomsLoaded)
        return;

    std::lock_guard<std::mutex> lock(mEmulatorMutex);

    for (int i = 0; i < steps; ++i)
        mEmulator.Step();
}

void NukedSC55AudioProcessor::stepEmulatorForUi()
{
    if (!mRomsLoaded)
        return;

    std::lock_guard<std::mutex> lock(mEmulatorMutex);

    static constexpr int kIdleStepsPerFrame = 50000;
    int steps = kIdleStepsPerFrame;

    if (mRemainingBootSteps > 0)
    {
        steps = std::min(kIdleStepsPerFrame, mRemainingBootSteps);
        mRemainingBootSteps -= steps;
    }

    for (int i = 0; i < steps; ++i)
        mEmulator.Step();
}

//==============================================================================
void NukedSC55AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    mCurrentSampleRate = sampleRate;

    ensureEmulatorReady();

    // Reset interpolators when sample rate changes
    mInterpolatorL.reset();
    mInterpolatorR.reset();

    ignoreUnused(samplesPerBlock);
}

void NukedSC55AudioProcessor::releaseResources()
{
}

//==============================================================================
void NukedSC55AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const auto numSamples = buffer.getNumSamples();

    // Clear output first (safe default when ROMs aren't loaded)
    buffer.clear();

    // Can't generate audio without ROMs
    if (!mInitialized || !mRomsLoaded)
        return;

    // Process incoming MIDI
    for (const auto& metadata : midiMessages)
    {
        auto* msgData = metadata.data;
        auto msgSize = metadata.numBytes;

        for (size_t i = 0; i < msgSize; ++i)
            mEmulator.PostMIDI(msgData[i]);
    }

    midiMessages.clear();

    //--------------------------------------------------------------------------
    // Sample rate conversion: emulator native rate → DAW project rate
    // (same source as the SDL frontend: PCM_GetOutputFrequency)
    //--------------------------------------------------------------------------
    const double emuRate = static_cast<double>(PCM_GetOutputFrequency(mEmulator.GetPCM()));
    const double speedRatio = emuRate / mCurrentSampleRate;
    const int inputNeeded = static_cast<int>(std::ceil(static_cast<double>(numSamples) * speedRatio))
                            + static_cast<int>(juce::LagrangeInterpolator::getBaseLatency()) + 2;

    // Ensure scratch buffer is large enough
    if (mScratchBuffer.getNumSamples() < inputNeeded || mScratchBuffer.getNumChannels() < 2)
        mScratchBuffer.setSize(2, inputNeeded, false, false, true);

    auto* scratchL = mScratchBuffer.getWritePointer(0);
    auto* scratchR = mScratchBuffer.getWritePointer(1);

    {
        std::lock_guard<std::mutex> lock(mEmulatorMutex);

        for (int i = 0; i < inputNeeded; ++i)
        {
            mEmulator.Step();
            scratchL[i] = mCurrentSample.left;
            scratchR[i] = mCurrentSample.right;
        }
    }

    // Resample from emulator rate to DAW rate
    if (emuRate == mCurrentSampleRate)
    {
        buffer.copyFrom(0, 0, scratchL, numSamples);
        buffer.copyFrom(1, 0, scratchR, numSamples);
    }
    else
    {
        auto* outL = buffer.getWritePointer(0);
        auto* outR = buffer.getWritePointer(1);

        mInterpolatorL.process(speedRatio, scratchL, outL, numSamples);
        mInterpolatorR.process(speedRatio, scratchR, outR, numSamples);
    }
}

//==============================================================================
juce::AudioProcessorEditor* NukedSC55AudioProcessor::createEditor()
{
    ensureEmulatorReady();
    return new NukedSC55AudioProcessorEditor(*this);
}

//==============================================================================
void NukedSC55AudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto xml = std::make_unique<juce::XmlElement>("NukedSC55");

    xml->setAttribute("romDirectory", mRomDirectory);

    if (mGainParam != nullptr)
        xml->setAttribute("gain", (double)mGainParam->get());

    copyXmlToBinary(*xml, destData);
}

void NukedSC55AudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;

    // Restore ROM directory
    if (xml->hasAttribute("romDirectory"))
    {
        const auto dir = xml->getStringAttribute("romDirectory").toStdString();
        mRomDirectory = dir;
    }

    ensureEmulatorReady();

    // Restore gain
    if (mGainParam != nullptr && xml->hasAttribute("gain"))
        *mGainParam = (float)xml->getDoubleAttribute("gain");
}

//==============================================================================
bool NukedSC55AudioProcessor::loadROMsImpl(const std::string& directory)
{
    if (directory.empty())
        return false;

    if (!std::filesystem::exists(directory))
    {
        DBG("Nuked SC-55: ROM directory does not exist: " + directory);
        return false;
    }

    mRomsetInfo = {};
    common::RomOverrides overrides{};
    common::LoadRomsetResult result;

    const auto err = common::LoadRomset(mRomsetInfo, directory, "", false, overrides, result);

    if (static_cast<int>(err) != 0)
    {
        DBG("Nuked SC-55: LoadRomset failed: " + juce::String(common::ToCString(err)));
        return false;
    }

    if (!mEmulator.LoadRoms(result.romset, mRomsetInfo, nullptr))
    {
        DBG("Nuked SC-55: Emulator::LoadRoms failed");
        return false;
    }

    mLoadResult = result;
    mRomDirectory = directory;
    mRomsLoaded = true;

    mEmulator.Reset();
    mEmulator.StartLCD();

    // MK2 firmware needs ~500k MCU steps before lcd.enable is set and the
    // boot text appears. Prime in the GUI timer so the first frame is not black.
    mRemainingBootSteps = 500000;

    DBG("Nuked SC-55: ROMs loaded successfully");
    return true;
}

void NukedSC55AudioProcessor::setRomDirectory(const std::string& path)
{
    mRomDirectory = path;
    mRomsLoaded   = false;

    ensureEmulatorReady();
}

void NukedSC55AudioProcessor::triggerGsReset()
{
    mEmulator.PostSystemReset(EMU_SystemReset::GS_RESET);
    DBG("Nuked SC-55: GS Reset triggered");
}

void NukedSC55AudioProcessor::triggerGmReset()
{
    mEmulator.PostSystemReset(EMU_SystemReset::GM_RESET);
    DBG("Nuked SC-55: GM Reset triggered");
}

//==============================================================================
// This creates new instances of the plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NukedSC55AudioProcessor();
}