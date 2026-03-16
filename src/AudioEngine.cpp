#include "AudioEngine.h"
#include "miniaudio.h"
#include <iostream>
#include <cmath>

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

    // Initialize modules
    for (auto& module : m_modules) {
        module->onInitialize(m_params);
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
    // 1. Clear output buffer
    for (uint32_t i = 0; i < nFrames * m_params.numOutputChannels; ++i) {
        output[i] = 0.0f;
    }

    // 2. Process timing
    std::vector<uint32_t> beatOffsets;
    m_metronomeCore.process(nFrames, m_params.sampleRate, beatOffsets);

    // 3. Synthesize click based on offsets
    synthesizeClick(output, nFrames, beatOffsets);

    // 4. Notify modules of beats
    for (uint32_t offset : beatOffsets) {
        // Simple beat indexing (could be expanded with bar logic)
        for (auto& module : m_modules) {
            module->onBeat(0, offset); 
        }
    }

    // 5. Run module audio processing
    // Modules can modify the 'output' buffer (e.g., adding effects or their own synthesis)
    for (auto& module : m_modules) {
        module->processAudio(input, output, nFrames);
    }
}

void AudioEngine::synthesizeClick(float* buffer, uint32_t nFrames, const std::vector<uint32_t>& offsets) {
    // Simple sine-wave click for demonstration
    // In a real app, this would use a sample-based synthesizer
    const float frequency = 1000.0f;
    const float duration = 0.05f; // 50ms
    const uint32_t clickLength = static_cast<uint32_t>(m_params.sampleRate * duration);

    for (uint32_t offset : offsets) {
        for (uint32_t i = 0; i < clickLength && (offset + i) < nFrames; ++i) {
            float envelope = 1.0f - (static_cast<float>(i) / clickLength);
            float sample = std::sin(2.0f * M_PI * frequency * i / m_params.sampleRate) * envelope * 0.5f;
            
            // Mix into stereo output
            buffer[(offset + i) * 2] += sample;
            buffer[(offset + i) * 2 + 1] += sample;
        }
    }
}
