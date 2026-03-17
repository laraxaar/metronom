#include "AudioEngine.h"

// Define NOMINMAX to prevent windows.h from breaking std::min and std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RtAudio.h"
#include <iostream>
#include <cmath>
#include <chrono>
#include <cstdio>
#include "TrainingModules.h"

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
    , m_accuracyAnalyzer(m_coach)
    , m_audioMixer(m_mixer)
{
    m_params.sampleRate = 48000;  // Default to 48kHz for bass accuracy
    m_params.bufferSize = 256;    // Low latency default
    m_requestedSampleRate = 48000;
    m_metronomeEngine.setSampleRate(m_params.sampleRate);
    m_metronomeEngine.setBpm(m_currentBpm.load(std::memory_order_relaxed));
    m_audioMixer.setSampleRate(m_params.sampleRate);

    // Wire training modules (instances live inside AudioEngine)
    m_metronomeEngine.clearTrainingModules();
    m_metronomeEngine.addTrainingModule(&m_trainLadder);
    m_metronomeEngine.addTrainingModule(&m_trainSilence);
    m_metronomeEngine.addTrainingModule(&m_trainDisappear);
    m_metronomeEngine.addTrainingModule(&m_trainGroove);
    m_metronomeEngine.addTrainingModule(&m_trainDrunken);
    m_metronomeEngine.addTrainingModule(&m_trainBoss);

    // Defaults (disabled groove shift, enabled others with conservative params)
    m_trainLadder.setEnabled(false);
    m_trainSilence.setEnabled(false);
    m_trainDisappear.setEnabled(false);
    m_trainGroove.setEnabled(false);
    m_trainDrunken.setEnabled(false);
    m_trainBoss.setEnabled(false);
    m_trainLadderEnabled.store(false, std::memory_order_relaxed);
    m_trainSilenceEnabled.store(false, std::memory_order_relaxed);
    m_trainDisappearEnabled.store(false, std::memory_order_relaxed);
    m_trainGrooveEnabled.store(false, std::memory_order_relaxed);
    m_trainDrunkenEnabled.store(false, std::memory_order_relaxed);
    m_trainBossEnabled.store(false, std::memory_order_relaxed);

    m_trainLadder.reset(m_currentBpm.load(std::memory_order_relaxed));
    m_trainLadder.setMeasuresPerStep(m_ladderBars.load(std::memory_order_relaxed));
    m_trainLadder.setBpmIncrement(m_ladderInc.load(std::memory_order_relaxed));

    m_trainSilence.setProbability(m_silenceProb.load(std::memory_order_relaxed));
    m_trainGroove.setMaxShiftMs(m_grooveMaxShiftMs.load(std::memory_order_relaxed));
    m_trainDisappear.setVisibleBars(m_disappearVis.load(std::memory_order_relaxed));
    m_trainDisappear.setHiddenBars(m_disappearHid.load(std::memory_order_relaxed));
    m_trainDrunken.setLevel(m_drunkenLevel.load(std::memory_order_relaxed));
    m_trainBoss.setLevel(m_bossLevel.load(std::memory_order_relaxed));
    m_trainBoss.setBaseBpm(m_currentBpm.load(std::memory_order_relaxed));
    m_trainBoss.setFlashTarget(&m_bossFlash);
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
    m_accuracyAnalyzer.setSampleRate(sampleRate);
    m_audioMixer.setSampleRate(sampleRate);
    m_liveTempo.setSampleRate(sampleRate);

    // Pre-allocate onset scratch window (~10ms at current SR)
    const uint32_t winSize = (std::max)(1u, (sampleRate / 100));
    m_onsetWindow.assign(winSize, 0.0f);

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

void AudioEngine::setBufferSize(uint32_t bufferFrames) {
    // Takes effect on next reopenStream()
    if (bufferFrames < 32) bufferFrames = 32;
    if (bufferFrames > 4096) bufferFrames = 4096;
    m_params.bufferSize = bufferFrames;
}

// =====================================================================
// BPM and Modules
// =====================================================================

void AudioEngine::setBpm(double bpm) {
    m_currentBpm.store(bpm);
    m_metronomeCore.setBpm(bpm);
    m_metronomeEngine.setBpm(bpm);
    m_trainLadder.reset(bpm);
    m_trainBoss.setBaseBpm(bpm);
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

void AudioEngine::addPolyrhythm(int ratio) {
    // Adds a single polyrhythm voice: 'ratio' hits across current bar (timeSigTop beats)
    const double bpm = m_currentBpm.load(std::memory_order_relaxed);
    const int top = (std::max)(1, m_grid.getBeatsPerBar());
    const uint64_t curFrame = m_totalFrames.load(std::memory_order_relaxed);

    // Use cowbell sample if loaded; otherwise PolyEngine will synth a tone (empty vector)
    std::vector<float> sample; // empty = use mixer fallback
    m_polyEngine.addVoice(ratio, bpm, top, curFrame, sample, m_params.sampleRate);
}

void AudioEngine::setPolyrhythmEnabled(bool enabled) {
    m_polyEnabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        m_polyEngine.clear();
        m_metronomeEngine.setGridBEnabled(false);
    }
}

void AudioEngine::setPolyrhythmRatio(int x, int y) {
    if (x < 1) x = 1;
    if (x > 32) x = 32;
    if (y < 1) y = 1;
    if (y > 32) y = 32;
    m_polyX.store(x, std::memory_order_relaxed);
    m_polyY.store(y, std::memory_order_relaxed);
    m_polyEnabled.store(true, std::memory_order_relaxed);

    // Configure dual grid in MetronomeEngine: A is the bar grid (Y beats), B is X clicks across bar.
    m_metronomeEngine.setBeatsPerBar(y);
    m_metronomeEngine.setGridBEnabled(true);
    m_metronomeEngine.setGridBClicksPerBar(x);
}

void AudioEngine::clearPolyrhythm() {
    m_polyEnabled.store(false, std::memory_order_relaxed);
    m_polyX.store(0, std::memory_order_relaxed);
    m_polyY.store(0, std::memory_order_relaxed);
    m_polyEngine.clear();
    m_metronomeEngine.setGridBEnabled(false);
    m_metronomeEngine.setGridBClicksPerBar(0);
}

// =====================================================================
// Audio Callback (REAL-TIME SAFE — no locks, no allocations)
// =====================================================================

void AudioEngine::audioCallback(const float* input, float* output, uint32_t nFrames) {
    auto startTime = std::chrono::high_resolution_clock::now();
    uint32_t outCh = m_params.numOutputChannels;

    // 2. Process timing (sample-accurate, no allocations)
    MetronomeEngine::Event events[MetronomeEngine::MAX_EVENTS];
    const size_t numEvents = m_metronomeEngine.processBlock(
        nFrames, events, MetronomeEngine::MAX_EVENTS);

    // Accuracy: feed ideal tick times (even if click is muted)
    const uint64_t blockStart = m_totalFrames.load(std::memory_order_relaxed);
    // Only Grid A drives accuracy scoring
    MetronomeEngine::Event aEvents[MetronomeEngine::MAX_EVENTS];
    size_t aN = 0;
    for (size_t i = 0; i < numEvents && aN < MetronomeEngine::MAX_EVENTS; ++i) {
        if (events[i].grid == MetronomeEngine::GridId::A) aEvents[aN++] = events[i];
    }
    m_accuracyAnalyzer.onTicks(blockStart, aEvents, aN);

    // 3. Process Polyrhythms
    std::vector<std::pair<uint32_t, std::vector<float>>> polyTriggers;
    m_polyEngine.process(nFrames, m_totalFrames.load(), polyTriggers);

    // 4. Final mixdown (click events + polyrhythm + monitoring + meters)
    // If click is disabled, still pass events so analyzer stays aligned,
    // but set velocities to 0 for actual audio output.
    if (!m_clickEnabled.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < numEvents; ++i) {
            events[i].velocity = 0.0f;
        }
    }
    m_audioMixer.processBlock(
        input,
        m_params.numInputChannels,
        output,
        outCh,
        nFrames,
        events,
        numEvents,
        polyTriggers,
        m_currentBpm.load(std::memory_order_relaxed)
    );

    // Mirror master peak into legacy outputPeak (UI)
    m_outputPeak.store(m_mixer.masterPeak.load(std::memory_order_relaxed), std::memory_order_relaxed);

    // Live Coaching update (trend-based)
    {
        // Compute mean/jitter over last 8..16 deviations (ms)
        const auto& devs = m_coach.getRecentDeviations();
        int n = (int)devs.size();
        if (n > 16) n = 16;
        if (n >= 8) {
            double sum = 0.0;
            for (int i = (int)devs.size() - n; i < (int)devs.size(); ++i) sum += devs[(size_t)i];
            const double mean = sum / (double)n;
            double var = 0.0;
            for (int i = (int)devs.size() - n; i < (int)devs.size(); ++i) {
                double d = devs[(size_t)i] - mean;
                var += d * d;
            }
            var /= (double)n;
            const double jitter = std::sqrt(var);

            const double elapsedSec = static_cast<double>(m_totalFrames.load(std::memory_order_relaxed))
                                    / static_cast<double>(m_params.sampleRate);
            const float stab = m_liveTempo.getStabilityPercent();
            auto out = m_liveCoach.update(mean, jitter, stab, elapsedSec);

            // seqlock write
            uint32_t s = m_coachSeq.load(std::memory_order_relaxed);
            m_coachSeq.store(s + 1, std::memory_order_release); // odd
            std::snprintf(m_coachText, sizeof(m_coachText), "%s", out.message ? out.message : "");
            m_flowActive.store(out.flow, std::memory_order_relaxed);
            m_coachSeq.store(s + 2, std::memory_order_release); // even
        }
    }

    // Rhythm Boss: 3 minutes survive, accuracy >= 70% required
    if (m_trainBossEnabled.load(std::memory_order_relaxed) && !m_bossGameOver.load(std::memory_order_relaxed)) {
        const uint64_t now = m_totalFrames.load(std::memory_order_relaxed);
        const uint64_t durFrames = static_cast<uint64_t>(m_params.sampleRate) * 180ull;
        const float acc = m_coach.getAccuracyPercent();
        if (acc > 0.0f && acc < 70.0f) {
            m_bossGameOver.store(true, std::memory_order_relaxed);
            m_trainBossEnabled.store(false, std::memory_order_relaxed);
            m_trainBoss.setEnabled(false);
        } else if (now > m_bossStartFrame && (now - m_bossStartFrame) >= durFrames) {
            // Victory: disable boss
            m_trainBossEnabled.store(false, std::memory_order_relaxed);
            m_trainBoss.setEnabled(false);
        }
        // Decay flash overlay
        float f = m_bossFlash.load(std::memory_order_relaxed);
        f *= 0.90f;
        if (f < 0.01f) f = 0.0f;
        m_bossFlash.store(f, std::memory_order_relaxed);
    }

    // 5. Push input to InputProcessor pipeline (DC filter + RingBuffer)
    //    This handles channel extraction, DC offset removal, and ring buffer storage.
    //    Tuner runs in TunerWorker thread — NOT here in the callback.
    if (input) {
        m_inputProcessor.pushSamples(input, nFrames);

        // Input analysis (onset detection, free play tracking, accuracy)
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
    m_metronomeEngine.setBeatsPerBar(m_grid.getBeatsPerBar());
    m_metronomeEngine.setSubdivisionParts(subdiv / 4);

    // Copy current grid states into engine (keeps UI/editor in sync with audio thread grid)
    const int steps = m_grid.getNumSteps();
    for (int i = 0; i < steps; ++i) {
        const StepState uiState = m_grid.getStepState(i);
        MetronomeEngine::StepState st = MetronomeEngine::StepState::NORMAL;
        switch (uiState) {
            case StepState::Accent: st = MetronomeEngine::StepState::ACCENT; break;
            case StepState::Normal: st = MetronomeEngine::StepState::NORMAL; break;
            case StepState::Muted:  st = MetronomeEngine::StepState::MUTE; break;
        }
        m_metronomeEngine.setStepState(i, st);
    }
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
    const uint32_t sr = m_params.sampleRate;
    const uint32_t winSize = (std::max)(1u, (sr / 100)); // ~10ms windows

    // Absolute block start sample (for correct timestamps)
    const uint64_t blockStart = m_totalFrames.load(std::memory_order_relaxed);
    const uint32_t totalCh = (std::max)(1u, m_params.numInputChannels);

    for (uint32_t i = 0; i < nFrames; i += winSize) {
        const uint32_t n = (std::min)(nFrames - i, winSize);
        const double curSec = static_cast<double>(blockStart + i) / static_cast<double>(sr);

        // Build mono window from interleaved input (channel 0 for now)
        if (m_onsetWindow.size() < n) {
            // Should not happen with pre-allocation; guard without realloc in RT.
            continue;
        }
        for (uint32_t j = 0; j < n; ++j) {
            m_onsetWindow[j] = input[(i + j) * totalCh];
        }

        const bool onset = m_onset.processWindow(
            m_onsetWindow.data(), static_cast<int>(n), sr, m_currentBpm.load(std::memory_order_relaxed), curSec);

        if (!onset) continue;

        // Free play tracking uses real timestamps.
        if (m_freePlay.active.load(std::memory_order_relaxed)) {
            m_freePlay.recordOnset(curSec);
        }

        // Live BPM tracking (Free Play)
        m_liveTempo.onOnsetFrame(blockStart + i);

        // Accuracy: compare onset time to ideal tick times (sample-accurate).
        m_accuracyAnalyzer.onOnset(blockStart + i);
    }
}

// =====================================================================
// Training + Human test control
// =====================================================================

void AudioEngine::setTrainingEnabled(TrainingModuleId id, bool enabled) {
    switch (id) {
        case TrainingModuleId::Ladder:
            m_trainLadderEnabled.store(enabled, std::memory_order_relaxed);
            m_trainLadder.setEnabled(enabled);
            break;
        case TrainingModuleId::RandomSilence:
            m_trainSilenceEnabled.store(enabled, std::memory_order_relaxed);
            m_trainSilence.setEnabled(enabled);
            break;
        case TrainingModuleId::Disappearing:
            m_trainDisappearEnabled.store(enabled, std::memory_order_relaxed);
            m_trainDisappear.setEnabled(enabled);
            break;
        case TrainingModuleId::GrooveShift:
            m_trainGrooveEnabled.store(enabled, std::memory_order_relaxed);
            m_trainGroove.setEnabled(enabled);
            break;
        case TrainingModuleId::DrunkenDrummer:
            m_trainDrunkenEnabled.store(enabled, std::memory_order_relaxed);
            m_trainDrunken.setEnabled(enabled);
            break;
        case TrainingModuleId::RhythmBoss:
            m_trainBossEnabled.store(enabled, std::memory_order_relaxed);
            m_trainBoss.setEnabled(enabled);
            m_bossGameOver.store(false, std::memory_order_relaxed);
            m_bossStartFrame = m_totalFrames.load(std::memory_order_relaxed);
            m_trainBoss.reset();
            break;
        default: break;
    }
}

void AudioEngine::adjustTrainingParam(TrainingParamId id, float delta) {
    switch (id) {
        case TrainingParamId::LadderMeasuresPerStep: {
            int v = m_ladderBars.load(std::memory_order_relaxed) + static_cast<int>(delta);
            if (v < 1) v = 1;
            if (v > 64) v = 64;
            m_ladderBars.store(v, std::memory_order_relaxed);
            m_trainLadder.setMeasuresPerStep(v);
            break;
        }
        case TrainingParamId::LadderBpmIncrement: {
            float v = m_ladderInc.load(std::memory_order_relaxed) + delta;
            v = (std::max)(0.1f, (std::min)(50.0f, v));
            m_ladderInc.store(v, std::memory_order_relaxed);
            m_trainLadder.setBpmIncrement(v);
            break;
        }
        case TrainingParamId::SilenceProbability: {
            float v = m_silenceProb.load(std::memory_order_relaxed) + delta;
            v = (std::max)(0.0f, (std::min)(1.0f, v));
            m_silenceProb.store(v, std::memory_order_relaxed);
            m_trainSilence.setProbability(v);
            break;
        }
        case TrainingParamId::GrooveMaxShiftMs: {
            float v = m_grooveMaxShiftMs.load(std::memory_order_relaxed) + delta;
            v = (std::max)(0.0f, (std::min)(20.0f, v));
            m_grooveMaxShiftMs.store(v, std::memory_order_relaxed);
            m_trainGroove.setMaxShiftMs(v);
            break;
        }
        case TrainingParamId::DisappearVisibleBars: {
            int v = m_disappearVis.load(std::memory_order_relaxed) + static_cast<int>(delta);
            if (v < 1) v = 1;
            if (v > 64) v = 64;
            m_disappearVis.store(v, std::memory_order_relaxed);
            m_trainDisappear.setVisibleBars(v);
            break;
        }
        case TrainingParamId::DisappearHiddenBars: {
            int v = m_disappearHid.load(std::memory_order_relaxed) + static_cast<int>(delta);
            if (v < 1) v = 1;
            if (v > 64) v = 64;
            m_disappearHid.store(v, std::memory_order_relaxed);
            m_trainDisappear.setHiddenBars(v);
            break;
        }
        case TrainingParamId::DrunkenLevel: {
            float v = m_drunkenLevel.load(std::memory_order_relaxed) + delta;
            v = (std::max)(0.0f, (std::min)(1.0f, v));
            m_drunkenLevel.store(v, std::memory_order_relaxed);
            m_trainDrunken.setLevel(v);
            break;
        }
        case TrainingParamId::BossLevel: {
            float v = m_bossLevel.load(std::memory_order_relaxed) + delta;
            v = (std::max)(0.0f, (std::min)(1.0f, v));
            m_bossLevel.store(v, std::memory_order_relaxed);
            m_trainBoss.setLevel(v);
            break;
        }
        default:
            break;
    }
}

void AudioEngine::setHumanTestEnabled(bool enabled) {
    m_humanTestEnabled.store(enabled, std::memory_order_relaxed);
    m_accuracyAnalyzer.setHumanTestEnabled(enabled);
    // Human test typically implies silent click, but keep engine running.
    if (enabled) {
        setClickEnabled(false);
        m_coach.reset(m_currentBpm.load(std::memory_order_relaxed));
        m_accuracyAnalyzer.reset();
    }
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
    // Backward-compatible API: 0=normal,1=accent
    if (type == 0) return m_audioMixer.loadWavSample(AudioMixer::SampleId::ClickNormal, path);
    if (type == 1) return m_audioMixer.loadWavSample(AudioMixer::SampleId::ClickAccent, path);
    return false;
}

bool AudioEngine::loadMixerSample(int sampleId, const std::string& path) {
    if (sampleId < 0 || sampleId > 4) return false;
    return m_audioMixer.loadWavSample(static_cast<AudioMixer::SampleId>(sampleId), path);
}
