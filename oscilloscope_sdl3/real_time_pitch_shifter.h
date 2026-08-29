#include "CDSPResampler.h"
#include <cmath>
#include <vector>

class real_time_pitch_shifter {
private:
    double m_baseSampleRate;     // E.g., 44100.0
    double m_currentSemitones;   // Dynamic pitch modifier
    int m_maxTransferSamples;    // Size of your streaming blocks
    
    r8b::CDSPResampler24* m_resampler; // Pointer to allow hot-swapping

    // Calculate virtual input sample rate based on semitone shift
    double calculateVirtualInputRate(double semitones) {
        return m_baseSampleRate * std::pow(2.0, -semitones / 12.0);
    }

public:
    real_time_pitch_shifter(double sampleRate, int maxTransferSamples) 
        : m_baseSampleRate(sampleRate), m_currentSemitones(0.0), 
          m_maxTransferSamples(maxTransferSamples) 
    {
        // Initialize at normal pitch (1:1 ratio)
        m_resampler = new r8b::CDSPResampler24(m_baseSampleRate, m_baseSampleRate, m_maxTransferSamples);
    }

    ~real_time_pitch_shifter() {
        delete m_resampler;
    }

    // Call this whenever the user moves a pitch slider or an automated modulation occurs
    inline void update_pitch(double newSemitones) {
        if (m_currentSemitones == newSemitones) return;

        m_currentSemitones = newSemitones;
        double virtualInputRate = calculateVirtualInputRate(m_currentSemitones);

        // Clear out the old resampler filter state and instantly allocate the new ratio
        delete m_resampler;
        m_resampler = new r8b::CDSPResampler24(virtualInputRate, m_baseSampleRate, m_maxTransferSamples);
    }

    // Process audio streaming chunks in your real-time callback loop
    int processBuffer(const double* inputBuffer, int inputLength, double* outputBuffer) {
        double* intermediatePtr;
        
        // r8brain processes input and returns an internal pointer containing the resampled data
        int samplesAllocated = m_resampler->process(inputBuffer, inputLength, intermediatePtr);
        
        // Copy the high-quality resampled data to your device's audio output buffer
        std::copy(intermediatePtr, intermediatePtr + samplesAllocated, outputBuffer);
        
        return samplesAllocated; // Return count so hardware mixer knows how many frames to play
    }
};