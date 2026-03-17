#include "AudioEngine.h"

// Define NOMINMAX to prevent windows.h from breaking std::min and std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RtAudio.h"
#include <iostream>
#include <cmath>
#include <chrono>
#include "TrainingModules.h"

// =====================================================================
// dr_wav for WAV file loading (single-file, header-only library)
// =====================================================================
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

// =====================================================================
// RtAudio Static Callback
// =====================================================================
static int rtAudioCallback(
    void* outputBuffer,
    void* inputBuffer,
    unsigned int nFrames,
    double /*streamTime*/,
    RtAudioStreamStatus status,
    void* userData)
{
    if (status) {
        std::cerr << "[AudioEngine] Stream over/underflow detected." << std::endl;
    }

    auto* engine = static_cast<AudioEngine*>(userData);
    engine->audioCallback(
        static_cast<const float*>(inputBuffer),
        static_cast<float*>(outputBuffer),
        static_cast<uint32_t>(nFrames));

    return 0; // 0 = continue streaming
}

// =====================================================================
// Constructor / Destructor
// =====================================================================

AudioEngine::AudioEngine()
    : m_inputProcessor(131072)  // ~2.7 sec ring buffer @ 48kHz
{
    m_params.sampleRate = 48000;  // Default to 48kHz for bass accuracy
    m_params.bufferSize = 256;    // Low latency default
    m_requestedSampleRate = 48000;
    m_metronomeEngine.setSampleRate(m_params.sampleRate);
    m_metronomeEngine.setBpm(m_currentBpm.load(std::memory_order_relaxed));
}

AudioEngine::~AudioEngine() {
    stop();
    m_tunerWorker.stop();
    if (m_rtAudio && m_rtAudio->isStreamOpen()) {
        m_rtAudio->closeStream();
    }
}

// =====================================================================
// Initialization
// =====================================================================

bool AudioEngine::initialize(const std::string& configPath) {
    // Enumerate all audio devices
    m_deviceManager.enumerateDevices();
    m_deviceManager.printDeviceReport();

    // Create RtAudio instance (RtAudio 6.0: no exceptions, use error checking)
    m_rtAudio = std::make_unique<RtAudio>();

    unsigned int deviceCount = m_rtAudio->getDeviceCount();
    if (deviceCount == 0) {
        std::cerr << "[AudioEngine] No audio devices found." << std::endl;
        return false;
    }

    // Determine input/output device IDs
    unsigned int outputDevId = (m_selectedOutputDevice >= 0)
        ? static_cast<unsigned int>(m_selectedOutputDevice)
        : m_rtAudio->getDefaultOutputDevice();

    unsigned int inputDevId = (m_selectedInputDevice >= 0)
        ? static_cast<unsigned int>(m_selectedInputDevice)
        : m_rtAudio->getDefaultInputDevice();

    // Get device info to determine channel counts
    RtAudio::DeviceInfo outInfo = m_rtAudio->getDeviceInfo(outputDevId);
    RtAudio::DeviceInfo inInfo  = m_rtAudio->getDeviceInfo(inputDevId);

    // Configure stream parameters
    RtAudio::StreamParameters outputParams;
    outputParams.deviceId     = outputDevId;
    outputParams.nChannels    = (std::min)(2u, outInfo.outputChannels);
    outputParams.firstChannel = 0;

    RtAudio::StreamParameters inputParams;
    inputParams.deviceId     = inputDevId;
    inputParams.nChannels    = (std::min)(inInfo.inputChannels, inInfo.inputChannels); // All input channels
    inputParams.firstChannel = 0;

    // Sample rate selection: prefer requested, fallback to device preferred, then 48000
    uint32_t sampleRate = m_requestedSampleRate;

    // Verify the sample rate is supported
    bool srSupported = false;
    for (unsigned int sr : outInfo.sampleRates) {
        if (sr == sampleRate) { srSupported = true; break; }
    }
    if (!srSupported) {
        sampleRate = outInfo.preferredSampleRate;
        if (sampleRate == 0) sampleRate = 48000;
        std::cout << "[AudioEngine] Requested SR not supported, using " << sampleRate << " Hz" << std::endl;
    }

    m_params.sampleRate = sampleRate;
    m_params.numOutputChannels = outputParams.nChannels;
    m_params.numInputChannels = inputParams.nChannels;

    // Update rhythm engine sample rate
    m_metronomeEngine.setSampleRate(sampleRate);

    unsigned int bufferFrames = m_params.bufferSize;

    // Open duplex stream
    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_MINIMIZE_LATENCY | RTAUDIO_SCHEDULE_REALTIME;

    // RtAudio 6.0: openStream returns RtAudioErrorType, no exceptions
    if (m_rtAudio->openStream(
            &outputParams,
            &inputParams,
            RTAUDIO_FLOAT32,
            sampleRate,
            &bufferFrames,
            &rtAudioCallback,
            this,
            &options) != RTAUDIO_NO_ERROR)
    {
        std::cerr << "[AudioEngine] Failed to open stream." << std::endl;
        return false;
    }

    // Update actual buffer size (RtAudio may change it)
    m_params.bufferSize = bufferFrames;

    std::cout << "[AudioEngine] Stream opened: "
              << sampleRate << " Hz, "
              << bufferFrames << " frames, "
              << "In: " << inputParams.nChannels << " ch (device " << inputDevId << "), "
              << "Out: " << outputParams.nChannels << " ch (device " << outputDevId << ")"
              << std::endl;

    // Initialize input processor
    m_inputProcessor.initialize(sampleRate, inputParams.nChannels);

    // Initialize tuner and worker thread
    m_tuner.initialize(sampleRate);
    m_tunerWorker.initialize(&m_inputProcessor, &m_tuner, sampleRate);
    m_tunerWorker.start();

    // Initialize modules
    for (auto& module : m_modules) {
        module->onInitialize(m_params);
    }

    // Initialize MIDI Sync
    if (m_midiSync.initialize()) {
        m_midiSync.onMidiStart = [this]() {
            this->start();
        };
        m_midiSync.onMidiStop = [this]() {
            this->stop();
        };
        m_midiSync.onMidiClock = [this]() {
            // Simplistic MIDI clock handling for now.
        };
        m_midiSync.start();
        std::cout << "[AudioEngine] MIDI Sync initialized." << std::endl;
    }

    return true;
}

// =====================================================================
// Start / Stop
// =====================================================================

bool AudioEngine::start() {
    if (!m_rtAudio || !m_rtAudio->isStreamOpen()) return false;

    // RtAudio 6.0: startStream returns error type
    if (m_rtAudio->startStream() != RTAUDIO_NO_ERROR) {
        std::cerr << "[AudioEngine] Failed to start stream." << std::endl;
        return false;
    }

    m_running.store(true);
    return true;
}

void AudioEngine::stop() {
    if (m_running.load()) {
        m_tunerWorker.stop();
        if (m_rtAudio && m_rtAudio->isStreamRunning()) {
            m_rtAudio->stopStream();
        }
        m_running.store(false);
    }
}

// =====================================================================
// Stream Reconfiguration
// =====================================================================

bool AudioEngine::reopenStream() {
    bool wasRunning = m_running.load();
    stop();

    if (m_rtAudio && m_rtAudio->isStreamOpen()) {
        m_rtAudio->closeStream();
    }

    m_inputProcessor.reset();

    if (!initialize("")) return false;
    if (wasRunning) return start();
    return true;
}

// =====================================================================
// Device Lists (Legacy API — delegates to DeviceManager)
// =====================================================================

std::vector<AudioEngine::DeviceInfo> AudioEngine::listPlaybackDevices() {
    std::vector<DeviceInfo> result;
    auto outDevs = m_deviceManager.getOutputDevices();
    for (const auto& dev : outDevs) {
        DeviceInfo info;
        info.name = dev.name;
        info.index = dev.deviceId;
        info.apiName = dev.apiName;
        info.maxInputChannels = dev.maxInputChannels;
        info.maxOutputChannels = dev.maxOutputChannels;
        info.preferredSampleRate = dev.preferredSampleRate;
        result.push_back(info);
    }
    return result;
}

std::vector<AudioEngine::DeviceInfo> AudioEngine::listCaptureDevices() {
    std::vector<DeviceInfo> result;
    auto inDevs = m_deviceManager.getInputDevices();
    for (const auto& dev : inDevs) {
        DeviceInfo info;
        info.name = dev.name;
        info.index = dev.deviceId;
        info.apiName = dev.apiName;
        info.maxInputChannels = dev.maxInputChannels;
        info.maxOutputChannels = dev.maxOutputChannels;
        info.preferredSampleRate = dev.preferredSampleRate;
        result.push_back(info);
    }
    return result;
}

// =====================================================================
// Device / Channel / Sample Rate Configuration
// =====================================================================

void AudioEngine::setInputDevice(int deviceId) {
    m_selectedInputDevice = deviceId;
}

void AudioEngine::setOutputDevice(int deviceId) {
    m_selectedOutputDevice = deviceId;
}

void AudioEngine::setInputChannel(uint32_t channel) {
    m_inputProcessor.setInputChannel(channel);
}

void AudioEngine::setSampleRate(uint32_t sampleRate) {
    m_requestedSampleRate = sampleRate;
}

// =====================================================================
// BPM and Modules
// =====================================================================

void AudioEngine::setBpm(double bpm) {
    m_currentBpm.store(bpm);
    m_metronomeCore.setBpm(bpm);
    m_metronomeEngine.setBpm(bpm);
}

void AudioEngine::addModule(std::unique_ptr<IMetronomeModule> module) {
    if (m_rtAudio && m_rtAudio->isStreamOpen()) {
        module->onInitialize(m_params);
    }
    m_modules.push_back(std::move(module));
}

void AudioEngine::clearModules() {
    m_modules.clear();
}

// =====================================================================
// Audio Callback (REAL-TIME SAFE — no locks, no allocations)
// =====================================================================

void AudioEngine::audioCallback(const float* input, float* output, uint32_t nFrames) {
    auto startTime = std::chrono::high_resolution_clock::now();
    uint32_t ch = m_params.numOutputChannels;

    // 1. Clear output buffer
    for (uint32_t i = 0; i < nFrames * ch; ++i) {
        output[i] = 0.0f;
    }

    // 1b. Drain leftover click tail from previous buffer
    if (!m_clickTail.empty()) {
        uint32_t tailLen = static_cast<uint32_t>(m_clickTail.size());
        uint32_t toDrain = (std::min)(nFrames, tailLen);
        for (uint32_t i = 0; i < toDrain; ++i) {
            for (uint32_t c = 0; c < ch; ++c) {
                output[i * ch + c] += m_clickTail[i];
            }
        }
        m_clickTail.clear();
    }

    // 2. Process timing (sample-accurate, no allocations)
    MetronomeEngine::Event events[MetronomeEngine::MAX_EVENTS];
    const size_t numEvents = m_metronomeEngine.processBlock(
        nFrames, events, MetronomeEngine::MAX_EVENTS);

    // 3. Synthesize click based on events (only if click is enabled)
    if (m_clickEnabled.load(std::memory_order_relaxed) && numEvents > 0) {
        synthesizeClick(output, nFrames, events, numEvents);
    }

    // 4. Process Polyrhythms
    std::vector<std::pair<uint32_t, std::vector<float>>> polyTriggers;
    m_polyEngine.process(nFrames, m_totalFrames.load(), polyTriggers);
    
    // Mix and peak detection loop
    float blockOutPeak = 0.0f;
    bool isMon = m_monitoring.load();
    float monG = m_monitorGain.load();

    for (uint32_t i = 0; i < nFrames; ++i) {
        float mix = 0.0f;
        
        // Add polyrhythm triggers if any
        for (const auto& trig : polyTriggers) {
            if (trig.first == i) {
                if (!trig.second.empty()) mix += trig.second[0]; 
            }
        }

        // Output Passthrough (Monitoring) — use first input channel for monitoring
        if (isMon && input) {
            mix += input[i * m_params.numInputChannels] * monG;
        }

        // Apply mix to output
        for (uint32_t c = 0; c < ch; ++c) {
            output[i * ch + c] += mix;
            float absOut = std::abs(output[i * ch + c]);
            if (absOut > blockOutPeak) blockOutPeak = absOut;
        }
    }

    // Update output peak
    float oldOut = m_outputPeak.load();
    if (blockOutPeak > oldOut) m_outputPeak.store(blockOutPeak);
    else m_outputPeak.store(oldOut * 0.95f);

    // 5. Push input to InputProcessor pipeline (DC filter + RingBuffer)
    //    This handles channel extraction, DC offset removal, and ring buffer storage.
    //    Tuner runs in TunerWorker thread — NOT here in the callback.
    if (input) {
        m_inputProcessor.pushSamples(input, nFrames);

        // Legacy input analysis (onset detection, free play tracking)
        processInputAnalysis(input, nFrames);

        // Notify TunerWorker that new data is available (lock-free)
        m_tunerWorker.notifyNewData();
    }

    // 6. Update UI Controller
    if (m_uiController) {
        VisualData data;
        if (input) {
            data.waveform.assign(input, input + (std::min)(nFrames, 512u));
        }
        data.currentFrequency = m_tuner.getResult().currentFreq.load();
        data.confidence = m_tuner.getResult().confidence.load();
        data.peakInput = m_inputProcessor.getPeakLevel();
        data.peakOutput = m_outputPeak.load();
        data.cpuLoad = m_cpuLoad.load();
        data.currentBeatIndex = m_metronomeEngine.getCurrentStep();
        // Grid snapshot for UI
        {
            auto snap = m_grid.getSnapshot();
            data.beatPattern.clear();
            for (int i = 0; i < snap.numSteps; ++i) {
                data.beatPattern.push_back(snap.states[i]);
            }
        }
        
        auto devs = m_coach.getRecentDeviations();
        data.hitDeviations.clear();
        data.hitDeviations.reserve(devs.size());
        for (double d : devs) {
            data.hitDeviations.push_back(static_cast<float>(d));
        }

        m_uiController->postVisualData(data);
        
        // Dequeue custom tuning requests
        if (auto optTuning = m_uiController->getLatestCustomTuning()) {
            setCustomTuning(*optTuning);
        }

        // Dequeue training module load requests
        if (auto optMod = m_uiController->getLatestActiveModule()) {
            clearModules();
            int modType = *optMod;
            if (modType == 1) addModule(std::make_unique<BpmLadder>());
            else if (modType == 2) addModule(std::make_unique<RandomSilence>());
            else if (modType == 3) addModule(std::make_unique<HumanMetronomeTest>());
            else if (modType == 4) addModule(std::make_unique<RhythmBoss>());
        }

        // Dequeue parameter updates
        while (auto optParam = m_uiController->getNextModuleParam()) {
            for (auto& module : m_modules) {
                module->setParameter(optParam->id, optParam->value);
            }
        }
    }

    // 7. Notify modules of beats (tick events)
    for (size_t i = 0; i < numEvents; ++i) {
        const uint32_t offset = events[i].sampleOffset;
        for (auto& module : m_modules) {
            module->onBeat(0, offset);
        }
    }

    // 8. Run module audio processing
    for (auto& module : m_modules) {
        module->processAudio(input, output, nFrames);
        double bpmOverride = module->getBpmOverride();
        if (bpmOverride > 0.0) {
            setBpm(bpmOverride);
        }
    }

    m_totalFrames.fetch_add(nFrames);

    // Calculate CPU Load
    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = endTime - startTime;
    double bufferDuration = static_cast<double>(nFrames) / m_params.sampleRate;
    float load = static_cast<float>((elapsed.count() / bufferDuration) * 100.0);
    
    // Smooth the CPU load for UI
    m_cpuLoad.store(m_cpuLoad.load() * 0.9f + load * 0.1f);
}

// =====================================================================
// Instrument & Tuning Configuration
// =====================================================================

void AudioEngine::setInstrumentMode(PreciseTuner::Mode mode) {
    m_tuner.setMode(mode);
    
    // Adjust onset detector for specific instruments
    if (mode == PreciseTuner::Mode::GuitarStandard || mode == PreciseTuner::Mode::GuitarDropD) {
        m_onset.baseThreshold = 0.08;
        m_onset.debounceMinSec = 0.04;
    } else if (mode == PreciseTuner::Mode::BassStandard || mode == PreciseTuner::Mode::Bass5String) {
        m_onset.baseThreshold = 0.12;
        m_onset.debounceMinSec = 0.06;
    } else if (mode == PreciseTuner::Mode::Custom) {
        m_onset.baseThreshold = 0.08;
        m_onset.debounceMinSec = 0.04;
    }
}

void AudioEngine::setCustomTuning(const std::vector<float>& frequencies) {
    m_tuner.setCustomTuning(frequencies);
    m_onset.baseThreshold = 0.10;
    m_onset.debounceMinSec = 0.05;
}

void AudioEngine::setGrooveShift(double delayMs) {
    m_grooveShiftFrames.store(static_cast<int64_t>(delayMs * m_params.sampleRate / 1000.0));
}

void AudioEngine::setBeatPattern(const std::vector<int>& pattern) {
    // Legacy compatibility: map ints to grid states
    for (size_t i = 0; i < pattern.size() && i < (size_t)m_grid.getNumSteps(); ++i) {
        m_grid.setStepState(static_cast<int>(i), static_cast<StepState>(pattern[i]));
        // Mirror into the new engine grid (StepState enums differ)
        MetronomeEngine::StepState st = MetronomeEngine::StepState::NORMAL;
        switch (static_cast<StepState>(pattern[i])) {
            case StepState::Accent: st = MetronomeEngine::StepState::ACCENT; break;
            case StepState::Normal: st = MetronomeEngine::StepState::NORMAL; break;
            case StepState::Muted:  st = MetronomeEngine::StepState::MUTE; break;
        }
        m_metronomeEngine.setStepState(static_cast<int>(i), st);
    }
}

void AudioEngine::cycleGridStep(int stepIndex) {
    m_grid.cycleStep(stepIndex);
    // Mirror into the new engine grid
    StepState s = m_grid.getStepState(stepIndex);
    MetronomeEngine::StepState st = MetronomeEngine::StepState::NORMAL;
    switch (s) {
        case StepState::Accent: st = MetronomeEngine::StepState::ACCENT; break;
        case StepState::Normal: st = MetronomeEngine::StepState::NORMAL; break;
        case StepState::Muted:  st = MetronomeEngine::StepState::MUTE; break;
    }
    m_metronomeEngine.setStepState(stepIndex, st);
}

void AudioEngine::setGridSubdivision(int subdiv) {
    Subdivision s = Subdivision::Quarter;
    if (subdiv == 8) s = Subdivision::Eighth;
    else if (subdiv == 12) s = Subdivision::Triplet;
    else if (subdiv == 16) s = Subdivision::Sixteenth;
    else if (subdiv == 20) s = Subdivision::Quintuplet;
    m_grid.setSubdivision(s);
    // Update MetronomeCore subdivision to match
    m_metronomeCore.setSubdivision(subdiv / 4);
    // Update new engine subdivision parts: 4->1, 8->2, 12->3, 16->4, 20->5
    m_metronomeEngine.setSubdivisionParts(subdiv / 4);
}

void AudioEngine::setMonitoring(bool enabled, float gain) {
    m_monitoring.store(enabled);
    m_monitorGain.store(gain);
}

void AudioEngine::setTuning(int instrument, int tuning) {
    std::vector<float> freqs;
    if (instrument == 0 || instrument == 1) { // 6-string Guitar
        freqs = { 82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f };
        if (tuning == 1) freqs[0] = 73.42f; // Drop D
        else if (tuning == 2) {
            for(auto& f : freqs) f *= 0.94387f; 
        }
    } else if (instrument == 2) { // 4-string Bass
        freqs = { 41.20f, 55.00f, 73.42f, 98.00f };
        if (tuning == 1) freqs[0] = 36.71f; // Drop D
    }
    m_tuner.setCustomTuning(freqs);
}

// =====================================================================
// Input Analysis (Legacy)
// =====================================================================

void AudioEngine::processInputAnalysis(const float* input, uint32_t nFrames) {
    uint32_t winSize = (std::max)(1u, (m_params.sampleRate / 100));
    // Note: peak level is now tracked by InputProcessor

    for (uint32_t i = 0; i < nFrames; i += winSize) {
        uint32_t n = (std::min)(nFrames - i, winSize);
        double curSec = 0.0;

        bool onset = m_onset.processWindow(input + i, n, m_params.sampleRate, m_currentBpm.load(), curSec);
        
        if (onset) {
            if (m_freePlay.active.load()) {
                m_freePlay.recordOnset(curSec);
            }
            double bps = m_currentBpm.load() / 60.0;
            double beatIntervalSecs = 1.0 / bps;
            double beatPhase = std::fmod(curSec, beatIntervalSecs);
            double deviation = beatPhase;
            if (deviation > beatIntervalSecs / 2.0) {
                deviation -= beatIntervalSecs;
            }
            m_coach.recordHit(deviation * 1000.0);
        }
    }

    // Update accuracy analysis
    auto currentBeatTime = m_metronomeCore.getTotalElapsedSec();
}

// =====================================================================
// Practice Scale
// =====================================================================

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void AudioEngine::setPracticeScale(float scale) {
    m_practiceScale = scale;
    double baseBpm = m_currentBpm.load();
    m_metronomeCore.setBpm(baseBpm * scale);
}

// =====================================================================
// WAV Sample Loading (using dr_wav instead of miniaudio)
// =====================================================================

bool AudioEngine::loadWavSample(int type, const std::string& path) {
    unsigned int channels;
    unsigned int sampleRate;
    drwav_uint64 totalFrameCount;

    float* pSampleData = drwav_open_file_and_read_pcm_frames_f32(
        path.c_str(), &channels, &sampleRate, &totalFrameCount, nullptr);

    if (pSampleData == nullptr) {
        std::cerr << "[AudioEngine] Failed to load WAV: " << path << std::endl;
        return false;
    }

    // Convert to mono if stereo (average channels)
    std::vector<float> monoBuffer;
    if (channels == 1) {
        monoBuffer.assign(pSampleData, pSampleData + totalFrameCount);
    } else {
        monoBuffer.resize(static_cast<size_t>(totalFrameCount));
        for (drwav_uint64 i = 0; i < totalFrameCount; ++i) {
            float sum = 0.0f;
            for (unsigned int c = 0; c < channels; ++c) {
                sum += pSampleData[i * channels + c];
            }
            monoBuffer[static_cast<size_t>(i)] = sum / static_cast<float>(channels);
        }
    }

    drwav_free(pSampleData, nullptr);

    // TODO: Resample if sampleRate != m_params.sampleRate

    if (type == 0) m_normalSample = std::move(monoBuffer);
    else m_accentSample = std::move(monoBuffer);

    std::cout << "[AudioEngine] Loaded WAV: " << path 
              << " (" << totalFrameCount << " frames, " << channels << " ch, " << sampleRate << " Hz)" 
              << std::endl;

    return true;
}

// =====================================================================
// Click Synthesis
// =====================================================================

void AudioEngine::synthesizeClick(float* buffer, uint32_t nFrames, const std::vector<uint32_t>& offsets, const std::vector<int>& indices) {
    const uint32_t ch = m_params.numOutputChannels;
    
    for (size_t k = 0; k < offsets.size(); ++k) {
        uint32_t offset = offsets[k];
        int beatIdx = indices[k];
        int patternType = (beatIdx >= 0 && beatIdx < (int)m_beatPattern.size()) ? m_beatPattern[beatIdx] : 1;
        
        if (patternType == 0) continue;

        const std::vector<float>& sample = (patternType == 2 && !m_accentSample.empty()) ? m_accentSample : m_normalSample;
        
        if (!sample.empty()) {
            // Use WAV sample
            for (uint32_t i = 0; i < (uint32_t)sample.size() && (offset + i) < nFrames; ++i) {
                for (uint32_t c = 0; c < ch; ++c) {
                    buffer[(offset + i) * ch + c] += sample[i];
                }
            }
        } else {
            // Fallback to synthesized click
            const float sr = static_cast<float>(m_params.sampleRate);
            const float bpm = static_cast<float>(m_currentBpm.load());
            float durMs = (bpm > 400.0f) ? 5.0f : 15.0f;
            uint32_t clickLen = static_cast<uint32_t>(sr * durMs * 0.001f);
            float vol = (patternType == 2) ? 0.95f : 0.75f;
            float freq = (patternType == 2) ? 2000.0f : 1200.0f;

            for (uint32_t i = 0; i < clickLen && (offset + i) < nFrames; ++i) {
                float t = static_cast<float>(i) / sr;
                float phase = static_cast<float>(i) / static_cast<float>(clickLen);
                float envelope = std::pow(0.5f * (1.0f - std::cos(2.0f * 3.14159f * phase)), 0.5f) * std::exp(-phase * 8.0f);
                float s = std::sin(2.0f * 3.14159f * freq * t) * envelope * vol;
                for (uint32_t c = 0; c < ch; ++c) {
                    buffer[(offset + i) * ch + c] += s;
                }
            }
        }
    }
}
