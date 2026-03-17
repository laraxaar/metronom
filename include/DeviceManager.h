#pragma once
#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief Audio API types supported by the system.
 */
enum class AudioApiType {
    Unspecified = 0,
    DirectSound,
    ASIO,
    WASAPI,
    CoreAudio,
    ALSA,
    PulseAudio,
    JACK,
    OSS
};

/**
 * @brief Converts AudioApiType to a human-readable string.
 */
inline const char* audioApiName(AudioApiType api) {
    switch (api) {
        case AudioApiType::DirectSound: return "DirectSound";
        case AudioApiType::ASIO:        return "ASIO";
        case AudioApiType::WASAPI:      return "WASAPI";
        case AudioApiType::CoreAudio:   return "CoreAudio";
        case AudioApiType::ALSA:        return "ALSA";
        case AudioApiType::PulseAudio:  return "PulseAudio";
        case AudioApiType::JACK:        return "JACK";
        case AudioApiType::OSS:         return "OSS";
        default:                        return "Unknown";
    }
}

/**
 * @brief Extended device information with API separation and channel details.
 */
struct AudioDeviceInfo {
    std::string     name;                    ///< Device display name
    AudioApiType    api{AudioApiType::Unspecified}; ///< Which audio API this device belongs to
    std::string     apiName;                 ///< API name as string (e.g., "WASAPI")
    int             deviceId{-1};            ///< Device index within RtAudio
    uint32_t        maxInputChannels{0};     ///< Maximum number of input channels
    uint32_t        maxOutputChannels{0};    ///< Maximum number of output channels
    uint32_t        preferredSampleRate{0};  ///< Preferred/native sample rate
    std::vector<uint32_t> supportedSampleRates; ///< All supported sample rates
    bool            isDefault{false};        ///< Whether this is the default device for its type
    bool            isProbed{false};         ///< Whether the device was successfully probed
};

/**
 * @brief Manages audio device enumeration and selection using RtAudio.
 *
 * Provides device listing with separation by audio API (ASIO, WASAPI, CoreAudio, etc.),
 * per-device channel information for input channel selection,
 * and sample rate capability querying.
 *
 * Thread-safety: Enumeration methods should only be called from the main/GUI thread.
 * Configuration methods use atomic storage for thread-safe reads from the audio thread.
 */
class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    /**
     * @brief Scan all available audio APIs and enumerate devices.
     * Call this on startup and when the user requests a device list refresh.
     */
    void enumerateDevices();

    /**
     * @brief Get all discovered devices across all APIs.
     */
    const std::vector<AudioDeviceInfo>& getAllDevices() const { return m_devices; }

    /**
     * @brief Get devices filtered by a specific API type.
     */
    std::vector<AudioDeviceInfo> getDevicesByApi(AudioApiType api) const;

    /**
     * @brief Get only input-capable devices (maxInputChannels > 0).
     */
    std::vector<AudioDeviceInfo> getInputDevices() const;

    /**
     * @brief Get only output-capable devices (maxOutputChannels > 0).
     */
    std::vector<AudioDeviceInfo> getOutputDevices() const;

    /**
     * @brief Get the number of input channels for a specific device.
     * @param deviceId RtAudio device index.
     * @return Number of input channels, or 0 if device not found.
     */
    uint32_t getInputChannelCount(int deviceId) const;

    /**
     * @brief Get the number of output channels for a specific device.
     * @param deviceId RtAudio device index.
     * @return Number of output channels, or 0 if device not found.
     */
    uint32_t getOutputChannelCount(int deviceId) const;

    /**
     * @brief Find a device by its RtAudio device ID.
     * @param deviceId RtAudio device index.
     * @return Pointer to AudioDeviceInfo, or nullptr if not found.
     */
    const AudioDeviceInfo* findDevice(int deviceId) const;

    /**
     * @brief Get the default input device ID.
     */
    int getDefaultInputDeviceId() const { return m_defaultInputId; }

    /**
     * @brief Get the default output device ID.
     */
    int getDefaultOutputDeviceId() const { return m_defaultOutputId; }

    /**
     * @brief Get all available API types on this system.
     */
    std::vector<AudioApiType> getAvailableApis() const;

    /**
     * @brief Print a diagnostic listing of all devices to stdout.
     */
    void printDeviceReport() const;

private:
    std::vector<AudioDeviceInfo> m_devices;
    int m_defaultInputId{-1};
    int m_defaultOutputId{-1};

    /**
     * @brief Map RtAudio API enum to our AudioApiType.
     */
    static AudioApiType mapApiType(int rtAudioApi);
};
