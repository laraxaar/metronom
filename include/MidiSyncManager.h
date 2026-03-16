#pragma once
#include <atomic>
#include <functional>

/**
 * @brief Interfaces with the OS to receive MIDI Clock, Start, and Stop events.
 * Currently uses Windows Multimedia API (mmeapi.h) under the hood.
 */
class MidiSyncManager {
public:
    MidiSyncManager();
    ~MidiSyncManager();

    /**
     * @brief Open the first available MIDI input device.
     */
    bool initialize();

    /**
     * @brief Start listening for MIDI events.
     */
    void start();

    /**
     * @brief Stop listening.
     */
    void stop();

    // Callbacks for the MetronomeEngine
    std::function<void()> onMidiStart;
    std::function<void()> onMidiStop;
    std::function<void()> onMidiClock; // 24 ticks per quarter note

    bool isRunning() const { return m_isRunning.load(); }

private:
    struct Impl;
    Impl* m_impl;
    std::atomic<bool> m_isRunning{false};
};
