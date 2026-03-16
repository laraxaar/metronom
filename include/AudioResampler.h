#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>

/**
 * @brief Simple linear resampler.
 * Refactored from legacy engi.cpp.
 */
class AudioResampler {
public:
    static std::vector<float> resample(const float* data, size_t len, uint32_t srcSr, uint32_t dstSr) {
        if (srcSr == dstSr || len == 0 || data == nullptr) {
            return std::vector<float>(data, data + len);
        }

        double ratio = static_cast<double>(dstSr) / srcSr;
        size_t outLen = static_cast<size_t>(len * ratio);
        std::vector<float> out(outLen);

        for (size_t i = 0; i < outLen; ++i) {
            double srcIdx = i / ratio;
            size_t idx0 = static_cast<size_t>(srcIdx);
            size_t idx1 = std::min(idx0 + 1, len - 1);
            double frac = srcIdx - idx0;
            out[i] = static_cast<float>((1.0 - frac) * data[idx0] + frac * data[idx1]);
        }
        return out;
    }
};
