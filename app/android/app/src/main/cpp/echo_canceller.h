// The kiosk's echo canceller and noise suppressor, 16 kHz, 10 ms frames.
// Plain C++ over Speex DSP; the mask's weights come from a blob the caller
// reads out of the app's assets. See echo_canceller.cpp for how it works.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct SpeexEchoState_;
struct SpeexPreprocessState_;

namespace kiosk {

class MaskModel;

class EchoCanceller {
public:
    static constexpr int kRate = 16000;
    static constexpr int kFrame = 160;
    static constexpr int kReferenceChannels = 2;

    EchoCanceller();
    ~EchoCanceller();
    EchoCanceller(const EchoCanceller&) = delete;
    EchoCanceller& operator=(const EchoCanceller&) = delete;

    // The mask's weights (assets/echo_mask.bin). Without them only the
    // linear filter runs.
    bool Load(const uint8_t* blob, size_t bytes);

    // One frame: the microphone (kFrame samples), what the speaker was
    // presenting as it was captured (kFrame stereo frames, interleaved),
    // and the cleaned microphone, which runs one frame behind the input.
    void Process(const int16_t* microphone, const int16_t* reference, int16_t* out);

    // What came off the microphone over everything processed so far, in
    // dB, and whether the mask is currently working rather than standing
    // aside for a microphone path that already cancels its own echo.
    double RemovedDb() const;
    bool MaskEngaged() const { return engaged_; }

private:
    void Transform(int which, const float* samples);
    void BandEnergies(int which, float* energies) const;

    SpeexEchoState_* speex_ = nullptr;
    void* forward_ = nullptr;
    void* inverse_ = nullptr;
    std::unique_ptr<MaskModel> model_;

    float window_[320];
    float history_[4][320];
    std::vector<float> spectra_;   // 4 x kBins complex, as r,i pairs
    float overlap_[kFrame];
    std::vector<float> referenceRing_;
    int ringAt_ = 0;
    std::vector<float> features_;
    std::vector<float> gains_;

    int quietReferenceFrames_ = 1000;
    float floor_ = 0.f;
    int floorFrames_ = 0;
    float microphoneEvidenceDb_ = 0.f;
    int evidenceFrames_ = 0;
    bool engaged_ = true;

    double highPassZ1_ = 0, highPassZ2_ = 0;
    double highPassB0_, highPassB1_, highPassB2_, highPassA1_, highPassA2_;

    double microphoneEnergy_ = 0, outputEnergy_ = 0;
};

class NoiseSuppressor {
public:
    NoiseSuppressor();
    ~NoiseSuppressor();
    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    // One 10 ms frame at 16 kHz, suppressed in place.
    void Process(int16_t* frame);

private:
    SpeexPreprocessState_* speex_ = nullptr;
};

}  // namespace kiosk
