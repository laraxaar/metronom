#include "AudioEngine.h"
#include "miniaudio.h"
#include <iostream>
#include <cmath>
#include <chrono>

// Internal implementation of the audio device
struct AudioEngine::DeviceImpl {
    ma_device device;
    bool inited = false;
};

// Static callback for miniaudio
static void maAudioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* engine = static_cast<AudioEngine*>(pDevice->pUserData);
    engine->audioCallback(static_cast<const float*>(pInput), static_cast<float*>(pOutput), frameCount);
}

AudioEngine::AudioEngine() : m_device(std::make_unique<DeviceImpl>()) {
    m_params.sampleRate = 44100;
    m_params.bufferSize = 512;
}

AudioEngine::~AudioEngine() {
    stop();
    if (m_device->inited) {
        ma_device_uninit(&m_device->device);
    }
}

bool AudioEngine::initialize(const std::string& configPath) {
    // In a real scenario, we would parse the JSON here.
    // For now, we'll use default parameters.
    
    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.sampleRate = m_params.sampleRate;
    config.periodSizeInFrames = m_params.bufferSize;
    config.dataCallback = maAudioCallback;
    config.pUserData = this;
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;

    if (ma_device_init(NULL, &config, &m_device->device) != MA_SUCCESS) {
        std::cerr << "Failed to initialize audio device." << std::endl;
        return false;
    }

    m_device->inited = true;
    m_params.numOutputChannels = 2;
    m_params.numInputChannels = 1;

    m_tuner.initialize(m_params.sampleRate);

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
            // Very simplistic MIDI clock handling for now. 
            // In a real app, this would recalculate BPM based on delta time between 24 PPQN ticks.
            // Or directly drive the MetronomeCore.
        };
        m_midiSync.start();
        std::cout << "MIDI Sync initialized." << std::endl;
    }

    return true;
}

bool AudioEngine::start() {
    if (!m_device->inited) return false;
    if (ma_device_start(&m_device->device) != MA_SUCCESS) return false;
    m_running.store(true);
    return true;
}

void AudioEngine::stop() {
    if (m_running.load()) {
        ma_device_stop(&m_device->device);
        m_running.store(false);
    }
}

void AudioEngine::setBpm(double bpm) {
    m_currentBpm.store(bpm);
    m_metronomeCore.setBpm(bpm);
}

void AudioEngine::addModule(std::unique_ptr<IMetronomeModule> module) {
    if (m_device->inited) {
        module->onInitialize(m_params);
    }
    m_modules.push_back(std::move(module));
}

void AudioEngine::audioCallback(const float* input, float* output, uint32_t nFrames) {
    auto startTime = std::chrono::high_resolution_clock::now();

    // 1. Clear output buffer
    for (uint32_t i = 0; i < nFrames * m_params.numOutputChannels; ++i) {
        output[i] = 0.0f;
    }

    // 2. Process timing
    std::vector<uint32_t> beatOffsets;
    m_metronomeCore.process(nFrames, m_params.sampleRate, beatOffsets);

    // 3. Synthesize click based on offsets
    synthesizeClick(output, nFrames, beatOffsets);

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
                // Simplistic: just play the first sample for now
                if (!trig.second.empty()) mix += trig.second[0]; 
            }
        }

        // Output Passthrough (Monitoring)
        if (isMon && input) {
            mix += input[i] * monG;
        }

        // Apply mix to output
        for (uint32_t c = 0; c < m_params.numOutputChannels; ++c) {
            output[i * m_params.numOutputChannels + c] += mix;
            float absOut = std::abs(output[i * m_params.numOutputChannels + c]);
            if (absOut > blockOutPeak) blockOutPeak = absOut;
        }
    }

    // Update peaks
    float oldOut = m_outputPeak.load();
    if (blockOutPeak > oldOut) m_outputPeak.store(blockOutPeak);
    else m_outputPeak.store(oldOut * 0.95f);

    // 5. Input Analysis (Legacy Refactored)
    if (input) {
        processInputAnalysis(input, nFrames);
        m_tuner.process(input, nFrames);
    }

    // 6. Update UI Controller
    if (m_uiController) {
        VisualData data;
        data.waveform.assign(input, input + std::min(nFrames, 512u)); 
        data.currentFrequency = m_tuner.getFrequency();
        data.confidence = m_tuner.getConfidence();
        data.peakInput = m_inputPeak.load();
        data.peakOutput = m_outputPeak.load();
        data.cpuLoad = m_cpuLoad.load();
        
        auto devs = m_coach.getRecentDeviations();
        data.hitDeviations.assign(devs.begin(), devs.end());

        m_uiController->postVisualData(data);
    }

    // 7. Notify modules of beats
    for (uint32_t offset : beatOffsets) {
        for (auto& module : m_modules) {
            module->onBeat(0, offset); 
        }
    }

    // 8. Run module audio processing
    for (auto& module : m_modules) {
        module->processAudio(input, output, nFrames);
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

void AudioEngine::setInstrumentMode(PreciseTuner::Mode mode) {
    m_tuner.setMode(mode);
    
    // Adjust onset detector for specific instruments to prevent false double-triggers
    if (mode == PreciseTuner::Mode::GuitarStandard || mode == PreciseTuner::Mode::GuitarDropD) {
        m_onset.baseThreshold = 0.08;
        m_onset.debounceMinSec = 0.04;
    } else if (mode == PreciseTuner::Mode::BassStandard || mode == PreciseTuner::Mode::Bass5String) {
        m_onset.baseThreshold = 0.12; // Bass has more low-frequency rumble, higher threshold
        m_onset.debounceMinSec = 0.06;
    } else if (mode == PreciseTuner::Mode::Custom) {
        // Assume default
        m_onset.baseThreshold = 0.08;
        m_onset.debounceMinSec = 0.04;
    }
}

void AudioEngine::setGrooveShift(double delayMs) {
    m_grooveShiftFrames.store(static_cast<int64_t>(delayMs * m_params.sampleRate / 1000.0));
}

void AudioEngine::setBeatPattern(const std::vector<int>& pattern) {
    std::lock_guard<std::mutex> lock(m_patternMutex);
    m_beatPattern = pattern;
}

void AudioEngine::setMonitoring(bool enabled, float gain) {
    m_monitoring.store(enabled);
    m_monitorGain.store(gain);
}

void AudioEngine::processInputAnalysis(const float* input, uint32_t nFrames) {
    // Legacy Smart Onset Detection logic applied in windows
    uint32_t winSize = std::max(1u, (m_params.sampleRate / 100)); // 10ms
    float blockPeak = 0.0f;

    for (uint32_t i = 0; i < nFrames; i += winSize) {
        uint32_t n = std::min(nFrames - i, winSize);
        double curSec = 0.0; // In a real app, track total elapsed seconds
        
        // Track peak
        for(uint32_t j = 0; j < n; ++j) {
            float v = std::abs(input[i+j]);
            if (v > blockPeak) blockPeak = v;
        }

        bool onset = m_onset.processWindow(input + i, n, m_params.sampleRate, m_currentBpm.load(), curSec);
        
        if (onset) {
            if (m_freePlay.active.load()) {
                m_freePlay.recordOnset(curSec);
            }
            
            // Temporary simple deviation logic (distance to nearest beat modulo BPM interval)
            double bps = m_currentBpm.load() / 60.0;
            double beatIntervalSecs = 1.0 / bps;
            double beatPhase = std::fmod(curSec, beatIntervalSecs);
            double deviation = beatPhase;
            if (deviation > beatIntervalSecs / 2.0) {
                deviation -= beatIntervalSecs; // Convert to - (early) or + (late)
            }
            m_coach.recordHit(deviation * 1000.0); // pass to coach in ms
        }
    }

    // Peak decay
    float oldPeak = m_inputPeak.load();
    if (blockPeak > oldPeak) m_inputPeak.store(blockPeak);
    else m_inputPeak.store(oldPeak * 0.95f);
}

void AudioEngine::synthesizeClick(float* buffer, uint32_t nFrames, const std::vector<uint32_t>& offsets) {
    // Drum/Bass/Hi-Gain friendly click synthesis:
    // A sharp transient with a dual-tone sine pair (2kHz + 1kHz) cuts through heavy mixes.
    const float f1 = 2000.0f; // High-pitched attack
    const float f2 = 1000.0f; // Body
    const float duration = 0.02f; // Very short 20ms click avoids mud
    const uint32_t clickLength = static_cast<uint32_t>(m_params.sampleRate * duration);

    for (uint32_t offset : offsets) {
        for (uint32_t i = 0; i < clickLength && (offset + i) < nFrames; ++i) {
            float t = static_cast<float>(i) / m_params.sampleRate;
            
            // Fast exponential decay envelope for a "snap"
            float envelope = std::exp(-t * 200.0f); 
            
            float s1 = std::sin(2.0f * M_PI * f1 * t);
            float s2 = std::sin(2.0f * M_PI * f2 * t);
            
            // Tiny bit of noise burst at the very beginning (first 5ms) for extra clarity
            float noise = ((rand() % 1000) / 1000.0f - 0.5f) * 0.15f * std::exp(-t * 800.0f);
            
            float sample = (s1 * 0.6f + s2 * 0.4f + noise) * envelope * 0.8f;
            
            // Mix into stereo output
            buffer[(offset + i) * 2] += sample;
            buffer[(offset + i) * 2 + 1] += sample;
        }
    }
}
