#include "DeviceManager.h"
#include "RtAudio.h"
#include <iostream>
#include <algorithm>

DeviceManager::DeviceManager() = default;
DeviceManager::~DeviceManager() = default;

AudioApiType DeviceManager::mapApiType(int rtAudioApi) {
    switch (static_cast<RtAudio::Api>(rtAudioApi)) {
        case RtAudio::WINDOWS_DS:    return AudioApiType::DirectSound;
        case RtAudio::WINDOWS_ASIO:  return AudioApiType::ASIO;
        case RtAudio::WINDOWS_WASAPI:return AudioApiType::WASAPI;
        case RtAudio::MACOSX_CORE:   return AudioApiType::CoreAudio;
        case RtAudio::LINUX_ALSA:    return AudioApiType::ALSA;
        case RtAudio::LINUX_PULSE:   return AudioApiType::PulseAudio;
        case RtAudio::UNIX_JACK:     return AudioApiType::JACK;
        case RtAudio::LINUX_OSS:     return AudioApiType::OSS;
        default:                     return AudioApiType::Unspecified;
    }
}

void DeviceManager::enumerateDevices() {
    m_devices.clear();
    m_defaultInputId = -1;
    m_defaultOutputId = -1;

    // Get all compiled APIs
    std::vector<RtAudio::Api> compiledApis;
    RtAudio::getCompiledApi(compiledApis);

    for (auto api : compiledApis) {
        // RtAudio 6.0: constructor no longer throws, use error callback or check state
        RtAudio audioInstance(api);

        if (audioInstance.getDeviceCount() == 0) continue;

        // RtAudio 6.0: use getDeviceIds() for persistent device IDs
        std::vector<unsigned int> deviceIds = audioInstance.getDeviceIds();
        unsigned int defaultOut = audioInstance.getDefaultOutputDevice();
        unsigned int defaultIn  = audioInstance.getDefaultInputDevice();

        for (unsigned int devId : deviceIds) {
            RtAudio::DeviceInfo info = audioInstance.getDeviceInfo(devId);
            // RtAudio 6.0: 'probed' field removed; if getDeviceInfo returns
            // a valid name, the device was probed successfully.
            if (info.name.empty()) continue;

            AudioDeviceInfo dev;
            dev.name               = info.name;
            dev.api                = mapApiType(static_cast<int>(api));
            dev.apiName            = audioApiName(dev.api);
            dev.deviceId           = static_cast<int>(devId);
            dev.maxInputChannels   = info.inputChannels;
            dev.maxOutputChannels  = info.outputChannels;
            dev.preferredSampleRate= info.preferredSampleRate;
            dev.isProbed           = true;  // If we got here, device was probed

            // Collect supported sample rates
            for (unsigned int sr : info.sampleRates) {
                dev.supportedSampleRates.push_back(sr);
            }

            // Mark defaults
            if (devId == defaultOut && info.outputChannels > 0) {
                dev.isDefault = true;
                if (m_defaultOutputId == -1) {
                    m_defaultOutputId = dev.deviceId;
                }
            }
            if (devId == defaultIn && info.inputChannels > 0) {
                dev.isDefault = true;
                if (m_defaultInputId == -1) {
                    m_defaultInputId = dev.deviceId;
                }
            }

            m_devices.push_back(std::move(dev));
        }
    }
}

std::vector<AudioDeviceInfo> DeviceManager::getDevicesByApi(AudioApiType api) const {
    std::vector<AudioDeviceInfo> result;
    for (const auto& dev : m_devices) {
        if (dev.api == api) {
            result.push_back(dev);
        }
    }
    return result;
}

std::vector<AudioDeviceInfo> DeviceManager::getInputDevices() const {
    std::vector<AudioDeviceInfo> result;
    for (const auto& dev : m_devices) {
        if (dev.maxInputChannels > 0) {
            result.push_back(dev);
        }
    }
    return result;
}

std::vector<AudioDeviceInfo> DeviceManager::getOutputDevices() const {
    std::vector<AudioDeviceInfo> result;
    for (const auto& dev : m_devices) {
        if (dev.maxOutputChannels > 0) {
            result.push_back(dev);
        }
    }
    return result;
}

uint32_t DeviceManager::getInputChannelCount(int deviceId) const {
    const AudioDeviceInfo* dev = findDevice(deviceId);
    return dev ? dev->maxInputChannels : 0;
}

uint32_t DeviceManager::getOutputChannelCount(int deviceId) const {
    const AudioDeviceInfo* dev = findDevice(deviceId);
    return dev ? dev->maxOutputChannels : 0;
}

const AudioDeviceInfo* DeviceManager::findDevice(int deviceId) const {
    for (const auto& dev : m_devices) {
        if (dev.deviceId == deviceId) {
            return &dev;
        }
    }
    return nullptr;
}

std::vector<AudioApiType> DeviceManager::getAvailableApis() const {
    std::vector<AudioApiType> apis;
    for (const auto& dev : m_devices) {
        bool found = false;
        for (auto a : apis) {
            if (a == dev.api) { found = true; break; }
        }
        if (!found) apis.push_back(dev.api);
    }
    return apis;
}

void DeviceManager::printDeviceReport() const {
    std::cout << "=== Audio Device Report ===" << std::endl;
    std::cout << "Total devices found: " << m_devices.size() << std::endl;
    std::cout << "Default input ID:  " << m_defaultInputId << std::endl;
    std::cout << "Default output ID: " << m_defaultOutputId << std::endl;
    std::cout << std::endl;

    AudioApiType currentApi = AudioApiType::Unspecified;

    for (const auto& dev : m_devices) {
        if (dev.api != currentApi) {
            currentApi = dev.api;
            std::cout << "--- " << dev.apiName << " ---" << std::endl;
        }

        std::cout << "  [" << dev.deviceId << "] " << dev.name;
        if (dev.isDefault) std::cout << " (DEFAULT)";
        std::cout << std::endl;

        std::cout << "       In: " << dev.maxInputChannels
                  << " ch | Out: " << dev.maxOutputChannels << " ch"
                  << " | Pref SR: " << dev.preferredSampleRate << " Hz" << std::endl;

        if (!dev.supportedSampleRates.empty()) {
            std::cout << "       Rates: ";
            for (size_t i = 0; i < dev.supportedSampleRates.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << dev.supportedSampleRates[i];
            }
            std::cout << std::endl;
        }
    }
    std::cout << "===========================" << std::endl;
}
