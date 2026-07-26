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
// Static callback — called by the emulator for each audio frame
// Runs synchronously inside MCU_Step(), under mEmulatorMutex.
void NukedSC55AudioProcessor::sampleCallback(void* userdata, const AudioFrame<int32_t>& frame)
{
    auto& self = *static_cast<NukedSC55AudioProcessor*>(userdata);

    // Normalize int32 → float
    AudioFrame<float> sample;
    Normalize(frame, sample, self.mVolumeControl);

    // Apply gain
    const float gain = self.mGainParam ? self.mGainParam->get() : 1.0f;
    Scale(sample, gain);

    // Push into ring buffer. Every MCU_PostSample call produces exactly one
    // frame (or two when oversampling is on — both get pushed here).
    self.mAudioRing[self.mAudioRingWrite % NukedSC55AudioProcessor::kAudioRingSize] = sample;
    self.mAudioRingWrite++;

    // Also cache last sample for any path that reads it directly (e.g.
    // stepEmulatorForUi doesn't drain the ring buffer — it just keeps
    // the MCU alive).
    self.mCurrentSample = sample;
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

    // Romset switch requires full re-init to clear previous romset state.
    if (mForceReinit)
    {
        if (mInitialized)
            mEmulator.StopLCD();
        mInitialized = false;
        mForceReinit = false;
        fprintf(stdout, "[Nuked SC-55] ensureEmulatorReady: forced re-init\n");
        fflush(stdout);
    }

    if (!mInitialized)
    {
        EMU_Options opts{};
        opts.lcd_backend   = &mLcdBackend;
        opts.rom_directory = mRomDirectory;
        opts.nvram_filename = std::filesystem::path(mRomDirectory) / "jv880_nvram.bin";

        if (!mEmulator.Init(opts))
        {
            DBG("Nuked SC-55: Emulator init failed");
            return;
        }

        mEmulator.SetSampleCallback(sampleCallback, this);
        mInitialized = true;
    }

    if (!mRomsLoaded && !mDiscoveryAttempted)
    {
        mDiscoveryAttempted = true;
        auto discovered = autoDiscoverRomDirectory(mSearchedPaths);
        if (!discovered.empty())
        {
            DBG("Nuked SC-55: Auto-discovered ROM directory: " + discovered);
            mRomDirectory = discovered;
        }
    }

    if (!mRomsLoaded && !mRomDirectory.empty())
    {
        fprintf(stdout, "[Nuked SC-55] ensureEmulatorReady: calling loadROMsImpl, romset='%s', dir='%s'\n",
                mDesiredRomset.empty() ? "(auto)" : mDesiredRomset.c_str(),
                mRomDirectory.c_str());
        fflush(stdout);
        loadROMsImpl(mRomDirectory);
    }
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

    // Boot priming must complete regardless of DAW activity — it's
    // initialization, not idle stepping. Do it before the DAW-active
    // check so intermittent processBlock calls don't starve it.
    if (mRemainingBootSteps > 0)
    {
        std::lock_guard<std::mutex> lock(mEmulatorMutex);

        static constexpr int kUiBootStepsPerFrame = 50000;
        const int steps = std::min(kUiBootStepsPerFrame, mRemainingBootSteps);
        mRemainingBootSteps -= steps;
        fprintf(stdout, "[Nuked SC-55] UI boot priming START: doing %d steps (%d remaining after)\n",
                steps, mRemainingBootSteps);
        fflush(stdout);
        for (int i = 0; i < steps; ++i)
            mEmulator.Step();
        fprintf(stdout, "[Nuked SC-55] UI boot priming END: %d steps left\n", mRemainingBootSteps);
        fflush(stdout);
        return;
    }

    // When the DAW is actively calling processBlock (within the last 50ms),
    // skip idle stepping.  This avoids both mutex contention (which
    // glitches the audio thread) AND emulator over-advancement (which
    // causes audible instability like envelope/LFO timing errors).
    if (juce::Time::getMillisecondCounter() - mLastProcessBlockTimeMs.load(std::memory_order_relaxed) < 50)
        return;

    std::lock_guard<std::mutex> lock(mEmulatorMutex);

    // Light stepping to keep MIDI/mcu alive when DAW is idle.
    static constexpr int kIdleStepsPerFrame = 2000;

    for (int i = 0; i < kIdleStepsPerFrame; ++i)
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

    // Track last processBlock time so the UI timer knows the DAW is alive.
    mLastProcessBlockTimeMs.store(juce::Time::getMillisecondCounter(),
                                  std::memory_order_relaxed);

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
    const int framesNeeded = static_cast<int>(std::ceil(static_cast<double>(numSamples) * speedRatio))
                             + static_cast<int>(juce::LagrangeInterpolator::getBaseLatency()) + 2;
    // Each MCU step advances cycles by 12; each PCM cycle processes
    // (reg_slots+1)*25 cycles (worst case ~800 for 32 voices, typical ~400
    // for SC-55mk2's 16 voices).  So steps per frame ≈ 400/12 ≈ 33.
    // Use a generous multiplier so we never run out of steps.
    const int maxSteps = framesNeeded * 80 + 10000;

    // Ensure scratch buffer is large enough
    if (mScratchBuffer.getNumSamples() < framesNeeded || mScratchBuffer.getNumChannels() < 2)
        mScratchBuffer.setSize(2, framesNeeded, false, false, true);

    auto* scratchL = mScratchBuffer.getWritePointer(0);
    auto* scratchR = mScratchBuffer.getWritePointer(1);
    int scratchWritten = 0;

    {
        std::lock_guard<std::mutex> lock(mEmulatorMutex);

        // Boot priming: MK2 firmware needs ~500k MCU steps before lcd.enable
        // is set and the boot text appears. Do this on the audio thread to
        // avoid UI→audio lock contention. Each processBlock call consumes a
        // chunk; at 64 samples/44.1kHz this completes in ~10 blocks (~15ms).
        if (mRemainingBootSteps > 0)
        {
            static constexpr int kBootStepsPerBlock = 50000;
            const int steps = std::min(kBootStepsPerBlock, mRemainingBootSteps);
            mRemainingBootSteps -= steps;
            for (int i = 0; i < steps; ++i)
                mEmulator.Step();
        }

        // Snapshot the ring-buffer write position before we start stepping.
        // Every frame pushed during stepping (via sampleCallback) will be
        // at or after this index.
        const uint32_t startWrite = mAudioRingWrite;

        for (int step = 0; step < maxSteps; ++step)
        {
            mEmulator.Step();

            // How many new frames have been pushed into the ring?
            const uint32_t newFrames = mAudioRingWrite - startWrite;
            if (newFrames >= static_cast<uint32_t>(framesNeeded))
                break;
        }

        // Drain the ring into scratch.  Under the mutex this is safe:
        // no-one can push while we read.
        const uint32_t available = mAudioRingWrite - startWrite;
        const int toCopy = std::min(static_cast<int>(available), framesNeeded);
        for (int i = 0; i < toCopy; ++i)
        {
            const auto& frame = mAudioRing[(startWrite + static_cast<uint32_t>(i)) % kAudioRingSize];
            scratchL[i] = frame.left;
            scratchR[i] = frame.right;
        }
        scratchWritten = toCopy;
    }

    // Resample from emulator rate to DAW rate.
    // scratchWritten may differ from numSamples: if emuRate > dawRate (e.g.
    // 64k→44.1k), we upsample fewer real frames into more DAW samples.
    if (scratchWritten == 0)
        return;

    if (static_cast<int>(scratchWritten) == numSamples && emuRate == mCurrentSampleRate)
    {
        // Exact match: direct copy.
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
    xml->setAttribute("romset", mDesiredRomset);
    xml->setAttribute("cardRomPath", mCardRomPath);
    xml->setAttribute("expRomPath", mExpRomPath);

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

    if (xml->hasAttribute("romset"))
    {
        mDesiredRomset = xml->getStringAttribute("romset").toStdString();
        // The constructor may have already loaded ROMs with auto-detect
        // before setStateInformation was called. If a specific romset was
        // saved, force a full re-init to load the correct one.
        if (mRomsLoaded && !mDesiredRomset.empty())
        {
            mForceReinit = true;
            mRomsLoaded = false;
        }
    }

    if (xml->hasAttribute("cardRomPath"))
        mCardRomPath = xml->getStringAttribute("cardRomPath").toStdString();

    if (xml->hasAttribute("expRomPath"))
        mExpRomPath = xml->getStringAttribute("expRomPath").toStdString();

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
        mRomDirectory.clear();
        return false;
    }

    mRomsetInfo = {};
    common::RomOverrides overrides{};

    // Auto-detect expansion/card ROMs in the ROM directory (like legacy path).
    // This ensures the firmware detects the expansion hardware at boot,
    // enabling hot-swap of waverom data later without re-init.
    if (mCardRomPath.empty())
    {
        auto cardPath = std::filesystem::path(directory) / "jv880_waverom_pcmcard.bin";
        if (std::filesystem::exists(cardPath))
            mCardRomPath = cardPath.string();
    }
    if (mExpRomPath.empty())
    {
        auto expPath = std::filesystem::path(directory) / "jv880_waverom_expansion.bin";
        if (std::filesystem::exists(expPath))
            mExpRomPath = expPath.string();
    }

    if (!mCardRomPath.empty())
        overrides[(size_t)RomLocation::WAVEROM_CARD] = mCardRomPath;
    if (!mExpRomPath.empty())
        overrides[(size_t)RomLocation::WAVEROM_EXP] = mExpRomPath;
    common::LoadRomsetResult result;

    const auto err = common::LoadRomset(mRomsetInfo, directory, mDesiredRomset, false, overrides, result);

    if (static_cast<int>(err) != 0)
    {
        fprintf(stdout, "[Nuked SC-55] LoadRomset FAILED: err=%d (%s), romset='%s', dir='%s'\n",
                static_cast<int>(err), common::ToCString(err),
                mDesiredRomset.empty() ? "(auto)" : mDesiredRomset.c_str(),
                directory.c_str());
        fflush(stdout);
        DBG("Nuked SC-55: LoadRomset failed: " + juce::String(common::ToCString(err)));
        mRomDirectory.clear();
        return false;
    }

    if (!mEmulator.LoadRoms(result.romset, mRomsetInfo, nullptr))
    {
        fprintf(stdout, "[Nuked SC-55] Emulator::LoadRoms FAILED, romset=%d\n",
                static_cast<int>(result.romset));
        fflush(stdout);
        DBG("Nuked SC-55: Emulator::LoadRoms failed");
        return false;
    }

    mLoadResult = result;
    mRomDirectory = directory;
    mRomsLoaded = true;

    fprintf(stdout, "[Nuked SC-55] ROMs loaded OK: romset=%d, dir='%s'\n",
            static_cast<int>(result.romset), directory.c_str());
    fflush(stdout);

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

void NukedSC55AudioProcessor::switchRomset(const std::string& romsetName)
{
    std::lock_guard<std::mutex> lock(mEmulatorMutex);
    mDesiredRomset = romsetName;
    mRomsLoaded = false;
    mForceReinit = true;  // force full emulator re-init to clear previous romset state
    fprintf(stdout, "[Nuked SC-55] switchRomset: romset='%s', romDir='%s'\n",
            romsetName.c_str(),
            mRomDirectory.empty() ? "(empty)" : mRomDirectory.c_str());
    fflush(stdout);
}

void NukedSC55AudioProcessor::loadCardRom(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mEmulatorMutex);
    mCardRomPath = path;
    mRomsLoaded = false;
    mForceReinit = true;
    fprintf(stdout, "[Nuked SC-55] loadCardRom: '%s'\n", path.c_str());
    fflush(stdout);
}

void NukedSC55AudioProcessor::loadExpRom(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mEmulatorMutex);
    mExpRomPath = path;
    mRomsLoaded = false;
    mForceReinit = true;
    fprintf(stdout, "[Nuked SC-55] loadExpRom: '%s'\n", path.c_str());
    fflush(stdout);
}

void NukedSC55AudioProcessor::ejectCardRom()
{
    std::lock_guard<std::mutex> lock(mEmulatorMutex);
    mCardRomPath.clear();
    mRomsLoaded = false;
    mForceReinit = true;
    fprintf(stdout, "[Nuked SC-55] ejectCardRom\n");
    fflush(stdout);
}

void NukedSC55AudioProcessor::ejectExpRom()
{
    std::lock_guard<std::mutex> lock(mEmulatorMutex);
    mExpRomPath.clear();
    mRomsLoaded = false;
    mForceReinit = true;
    fprintf(stdout, "[Nuked SC-55] ejectExpRom\n");
    fflush(stdout);
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