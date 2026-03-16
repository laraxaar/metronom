#include "MidiSyncManager.h"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <mmeapi.h>

struct MidiSyncManager::Impl {
    HMIDIIN hMidiIn = nullptr;
    MidiSyncManager* parent = nullptr;
    
    static void CALLBACK MidiInProc(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
        if (wMsg == MIM_DATA) {
            auto* mgr = reinterpret_cast<MidiSyncManager*>(dwInstance);
            if (!mgr) return;

            unsigned char msg = (dwParam1 & 0xFF);
            
            // Real-Time MIDI Messages
            if (msg == 0xF8) {        // Timing Clock (24 per quarter note)
                if (mgr->onMidiClock) mgr->onMidiClock();
            } else if (msg == 0xFA) { // Start
                if (mgr->onMidiStart) mgr->onMidiStart();
            } else if (msg == 0xFC) { // Stop
                if (mgr->onMidiStop) mgr->onMidiStop();
            } else if (msg == 0xFB) { // Continue
                if (mgr->onMidiStart) mgr->onMidiStart(); // Treat as start for simplicity
            }
        }
    }
};

MidiSyncManager::MidiSyncManager() : m_impl(new Impl()) {
    m_impl->parent = this;
}

MidiSyncManager::~MidiSyncManager() {
    stop();
    delete m_impl;
}

bool MidiSyncManager::initialize() {
    if (midiInGetNumDevs() == 0) return false;

    MMRESULT res = midiInOpen(&m_impl->hMidiIn, 0, (DWORD_PTR)Impl::MidiInProc, (DWORD_PTR)this, CALLBACK_FUNCTION);
    return res == MMSYSERR_NOERROR;
}

void MidiSyncManager::start() {
    if (m_impl->hMidiIn && !m_isRunning.load()) {
        midiInStart(m_impl->hMidiIn);
        m_isRunning.store(true);
    }
}

void MidiSyncManager::stop() {
    if (m_impl->hMidiIn && m_isRunning.load()) {
        midiInStop(m_impl->hMidiIn);
        midiInClose(m_impl->hMidiIn);
        m_impl->hMidiIn = nullptr;
        m_isRunning.store(false);
    }
}

#else
// Stub for non-Windows platforms
struct MidiSyncManager::Impl {};
MidiSyncManager::MidiSyncManager() : m_impl(nullptr) {}
MidiSyncManager::~MidiSyncManager() {}
bool MidiSyncManager::initialize() { return false; }
void MidiSyncManager::start() {}
void MidiSyncManager::stop() {}
#endif
