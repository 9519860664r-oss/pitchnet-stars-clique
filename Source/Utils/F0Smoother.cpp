#include "F0Smoother.h"
#include <algorithm>
#include <cmath>

std::vector<float> F0Smoother::medianFilter(const std::vector<float>& f0, int windowSize)
{
    if (f0.empty() || windowSize < 1)
        return f0;
    
    // Ensure window size is odd
    if (windowSize % 2 == 0)
        windowSize += 1;
    
    int halfWindow = windowSize / 2;
    std::vector<float> smoothed(f0.size());
    
    // Reuse buffer across frames to avoid per-frame allocation
    std::vector<float> windowValues;
    windowValues.reserve(windowSize);
    
    for (size_t i = 0; i < f0.size(); ++i)
    {
        int start = static_cast<int>(i) - halfWindow;
        int end = static_cast<int>(i) + halfWindow;
        
        // Collect valid F0 values in window (only voiced)
        windowValues.clear();
        
        for (int j = start; j <= end; ++j)
        {
            if (j >= 0 && j < static_cast<int>(f0.size()) && f0[j] > 0.0f)
            {
                windowValues.push_back(f0[j]);
            }
        }
        
        if (!windowValues.empty())
        {
            // Use nth_element for O(n) median instead of O(n log n) sort
            size_t mid = windowValues.size() / 2;
            if (windowValues.size() % 2 == 0)
            {
                std::nth_element(windowValues.begin(), windowValues.begin() + mid, windowValues.end());
                float upper = windowValues[mid];
                std::nth_element(windowValues.begin(), windowValues.begin() + mid - 1, windowValues.begin() + mid);
                smoothed[i] = (windowValues[mid - 1] + upper) / 2.0f;
            }
            else
            {
                std::nth_element(windowValues.begin(), windowValues.begin() + mid, windowValues.end());
                smoothed[i] = windowValues[mid];
            }
        }
        else
        {
            // No voiced frames in window, keep original (or 0)
            smoothed[i] = f0[i];
        }
    }
    
    return smoothed;
}

std::vector<float> F0Smoother::smoothTransitions(const std::vector<float>& f0,
                                                  const std::vector<bool>& voicedMask,
                                                  int windowSize)
{
    if (f0.empty() || f0.size() != voicedMask.size())
        return f0;
    
    if (windowSize < 1)
        windowSize = 1;
    
    int halfWindow = windowSize / 2;
    std::vector<float> smoothed(f0.size());
    
    for (size_t i = 0; i < f0.size(); ++i)
    {
        if (!voicedMask[i] || f0[i] <= 0.0f)
        {
            smoothed[i] = f0[i];
            continue;
        }
        
        // Weighted average of nearby voiced frames
        float sum = 0.0f;
        float weightSum = 0.0f;
        
        for (int j = -halfWindow; j <= halfWindow; ++j)
        {
            int idx = static_cast<int>(i) + j;
            if (idx >= 0 && idx < static_cast<int>(f0.size()) && 
                voicedMask[idx] && f0[idx] > 0.0f)
            {
                // Gaussian-like weight (closer frames have more weight)
                float weight = std::exp(-0.5f * (j * j) / (halfWindow * halfWindow + 1.0f));
                sum += f0[idx] * weight;
                weightSum += weight;
            }
        }
        
        if (weightSum > 0.0f)
        {
            smoothed[i] = sum / weightSum;
        }
        else
        {
            smoothed[i] = f0[i];
        }
    }
    
    return smoothed;
}

std::vector<float> F0Smoother::interpolateUnvoiced(const std::vector<float>& f0,
                                                     const std::vector<bool>& voicedMask,
                                                     int maxGapFrames)
{
    if (f0.empty() || f0.size() != voicedMask.size())
        return f0;
    
    std::vector<float> interpolated = f0;
    
    // Find unvoiced gaps and interpolate small ones
    size_t gapStart = 0;
    bool inGap = false;
    
    for (size_t i = 0; i < f0.size(); ++i)
    {
        if (!voicedMask[i] && !inGap)
        {
            // Start of gap
            gapStart = i;
            inGap = true;
        }
        else if (voicedMask[i] && inGap)
        {
            // End of gap
            size_t gapEnd = i;
            size_t gapSize = gapEnd - gapStart;
            
            if (gapSize <= static_cast<size_t>(maxGapFrames) && gapStart > 0)
            {
                // Find previous and next voiced frames
                float f0Prev = 0.0f;
                float f0Next = 0.0f;
                
                // Find previous voiced frame
                for (int j = static_cast<int>(gapStart) - 1; j >= 0; --j)
                {
                    if (voicedMask[j] && f0[j] > 0.0f)
                    {
                        f0Prev = f0[j];
                        break;
                    }
                }
                
                // Find next voiced frame
                for (size_t j = gapEnd; j < f0.size(); ++j)
                {
                    if (voicedMask[j] && f0[j] > 0.0f)
                    {
                        f0Next = f0[j];
                        break;
                    }
                }
                
                // Linear interpolation if both ends are available
                if (f0Prev > 0.0f && f0Next > 0.0f)
                {
                    for (size_t j = gapStart; j < gapEnd; ++j)
                    {
                        float t = static_cast<float>(j - gapStart) / gapSize;
                        interpolated[j] = f0Prev * (1.0f - t) + f0Next * t;
                    }
                }
            }
            
            inGap = false;
        }
    }
    
    return interpolated;
}

std::vector<float> F0Smoother::removeOutliers(const std::vector<float>& f0,
                                               float maxJumpRatio)
{
    if (f0.empty())
        return f0;
    
    // Octave errors first: they come in short runs the single-frame jump
    // check below cannot see.
    const std::vector<float> source = repairOctaveErrors(f0);
    std::vector<float> cleaned = source;

    for (size_t i = 1; i < source.size(); ++i)
    {
        // Compare against the already-cleaned previous frame. Using the raw
        // one made a good frame that follows an outlier look like a jump and
        // get "repaired" towards the outlier.
        if (source[i] > 0.0f && cleaned[i - 1] > 0.0f)
        {
            float ratio = source[i] / cleaned[i - 1];
            if (ratio > maxJumpRatio || ratio < 1.0f / maxJumpRatio)
            {
                // Outlier detected - use previous value or interpolate
                if (i + 1 < source.size() && source[i + 1] > 0.0f)
                {
                    // Interpolate between previous and next
                    cleaned[i] = (cleaned[i - 1] + source[i + 1]) / 2.0f;
                }
                else
                {
                    // Use previous value
                    cleaned[i] = cleaned[i - 1];
                }
            }
        }
    }

    return cleaned;
}

std::vector<float> F0Smoother::repairOctaveErrors(const std::vector<float>& f0,
                                                  int contextFrames,
                                                  int maxRunFrames)
{
    // On rasp and vocal fry the detector often locks onto the subharmonic for
    // a few frames: a note at ~100 Hz reads 54 Hz, and the vocoder then sings
    // that stretch an octave down. Compare each voiced frame with the median
    // of its voiced neighbourhood; a short run sitting an octave below (or
    // above) it is folded back. A long run is a real octave leap and stays.
    const int n = static_cast<int>(f0.size());
    std::vector<float> repaired = f0;
    if (n == 0)
        return repaired;

    constexpr float kToleranceSemitones = 3.0f;
    std::vector<int> octave(static_cast<size_t>(n), 0); // -1 low, +1 high
    std::vector<float> window;
    for (int i = 0; i < n; ++i)
    {
        if (f0[static_cast<size_t>(i)] <= 0.0f)
            continue;
        window.clear();
        for (int j = std::max(0, i - contextFrames);
             j <= std::min(n - 1, i + contextFrames); ++j)
            if (f0[static_cast<size_t>(j)] > 0.0f)
                window.push_back(f0[static_cast<size_t>(j)]);
        if (window.size() < 5)
            continue;
        auto mid = window.begin() + static_cast<std::ptrdiff_t>(window.size() / 2);
        std::nth_element(window.begin(), mid, window.end());
        const float reference = *mid;
        const float semitones =
            12.0f * std::log2(f0[static_cast<size_t>(i)] / reference);
        if (std::abs(semitones + 12.0f) < kToleranceSemitones)
            octave[static_cast<size_t>(i)] = -1;
        else if (std::abs(semitones - 12.0f) < kToleranceSemitones)
            octave[static_cast<size_t>(i)] = 1;
    }

    int i = 0;
    while (i < n)
    {
        const int flag = octave[static_cast<size_t>(i)];
        if (flag == 0)
        {
            ++i;
            continue;
        }
        int end = i;
        while (end < n && octave[static_cast<size_t>(end)] == flag)
            ++end;
        if (end - i <= maxRunFrames)
            for (int k = i; k < end; ++k)
                repaired[static_cast<size_t>(k)] *= flag < 0 ? 2.0f : 0.5f;
        i = end;
    }
    return repaired;
}

std::vector<float> F0Smoother::smoothF0(const std::vector<float>& f0,
                                         const std::vector<bool>& voicedMask)
{
    if (f0.empty())
        return f0;
    
    // Chain operations, reusing buffers to avoid 4 separate full copies.
    // Step 1: Remove outliers
    std::vector<float> result = removeOutliers(f0, 1.5f);
    
    // Step 2: Median filter (returns new vector, swap into result)
    std::vector<float> temp = medianFilter(result, 5);
    std::swap(result, temp);
    
    // Step 3: Smooth transitions
    temp = smoothTransitions(result, voicedMask, 3);
    std::swap(result, temp);
    
    // Step 4: Interpolate small unvoiced gaps
    temp = interpolateUnvoiced(result, voicedMask, 5);
    
    return temp;
}

float F0Smoother::getMedian(const std::vector<float>& values, int start, int end)
{
    if (start < 0) start = 0;
    if (end >= static_cast<int>(values.size())) end = static_cast<int>(values.size()) - 1;
    if (start > end) return 0.0f;
    
    std::vector<float> window;
    window.reserve(end - start + 1);
    
    for (int i = start; i <= end; ++i)
    {
        if (values[i] > 0.0f)
        {
            window.push_back(values[i]);
        }
    }
    
    if (window.empty())
        return 0.0f;
    
    size_t mid = window.size() / 2;
    if (window.size() % 2 == 0)
    {
        std::nth_element(window.begin(), window.begin() + mid, window.end());
        float upper = window[mid];
        std::nth_element(window.begin(), window.begin() + mid - 1, window.begin() + mid);
        return (window[mid - 1] + upper) / 2.0f;
    }
    else
    {
        std::nth_element(window.begin(), window.begin() + mid, window.end());
        return window[mid];
    }
}

bool F0Smoother::isReasonableJump(float f0Prev, float f0Curr, float maxRatio)
{
    if (f0Prev <= 0.0f || f0Curr <= 0.0f)
        return true;
    
    float ratio = f0Curr / f0Prev;
    return ratio <= maxRatio && ratio >= 1.0f / maxRatio;
}

