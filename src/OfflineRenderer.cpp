#include "OfflineRenderer.h"
#include "miniaudio.h"
#include <cmath>
#include <algorithm>

bool OfflineRenderer::exportWav(const ExportParams& params, 
                             const std::vector<float>& accentSample, 
                             const std::vector<float>& normalSample)
{
    if (params.duration <= 0 || params.sampleRate == 0 || params.filepath.empty()) return false;

    size_t totalSamples = (size_t)(params.sampleRate * params.duration);
    std::vector<float> track(totalSamples, 0.0f);

    int curBeat = 0, curSub = 0;
    double t = 0.0;
    uint32_t sr = params.sampleRate;

    // Core sequence generation logic from engi.cpp
    while (t < params.duration) {
        bool isAccent = (curBeat == 0 && curSub == 0);
        const std::vector<float>& smp = isAccent ? accentSample : normalSample;
        
        size_t startIdx = (size_t)(t * (double)sr);
        for (size_t j = 0; j < smp.size() && (startIdx + j) < totalSamples; ++j) {
            track[startIdx + j] += smp[j];
        }

        // Interval between beats (includes subdivision)
        double interval = 60.0 / (params.bpm * params.subdivision);
        t += interval;

        curSub++;
        if (curSub >= params.subdivision) {
            curSub = 0;
            curBeat = (curBeat + 1) % params.timeSigTop;
        }
    }

    // Normalization
    float peak = 0.0f;
    for (float x : track) {
        float ax = std::abs(x);
        if (ax > peak) peak = ax;
    }
    if (peak > 1.0f) {
        for (float& x : track) x /= peak;
    }

    // Write WAV via miniaudio encoder
    ma_encoder_config encCfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 1, sr);
    ma_encoder encoder;
    if (ma_encoder_init_file(params.filepath.c_str(), &encCfg, &encoder) != MA_SUCCESS) {
        return false;
    }
    ma_encoder_write_pcm_frames(&encoder, track.data(), totalSamples, nullptr);
    ma_encoder_uninit(&encoder);

    return true;
}
