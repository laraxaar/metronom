#pragma once
#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief Handles offline rendering of the metronome sequence to a WAV file.
 * Refactored from legacy engi.cpp.
 */
class OfflineRenderer {
public:
    struct ExportParams {
        std::string filepath;
        double duration;
        double bpm;
        int subdivision;
        int timeSigTop;
        uint32_t sampleRate;
    };

    /**
     * @brief Render the metronome sequence to a file.
     * @param accentSample Buffer for the accent beat.
     * @param normalSample Buffer for the normal beat.
     */
    static bool exportWav(const ExportParams& params, 
                         const std::vector<float>& accentSample, 
                         const std::vector<float>& normalSample);

private:
    // Uses miniaudio encoder internally
};
