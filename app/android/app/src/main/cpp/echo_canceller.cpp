// Software echo cancellation for the kiosk's microphone.
//
// Two stages, both fed the stereo reference SoftwareEcho.kt assembles from
// everything the kiosk plays:
//
// 1. Speex DSP's multichannel MDF filter, 256 ms long, takes out the part
//    of the echo a linear filter can model. On a tablet that is only 9 to
//    11 dB: the speaker sits in the same chassis as the microphone and
//    most of what comes back is distortion no filter can predict.
//
// 2. A small recurrent network gains each STFT bin of what the filter
//    leaves. Every 10 ms it sees 48 band energies of four signals (the
//    filter's output, the microphone, the echo the filter removed and the
//    reference 130 ms back, about where the echo arrives) and answers
//    with a gain per bin: 192 inputs, a 128 wide layer, two GRU layers of
//    128 and 161 sigmoid outputs, 245k weights. It was trained on
//    recordings of nine kiosk devices (Galaxy Tab S8, S8+ and S9, Fire HD
//    10, a Rockchip wall panel, Echo Dot, Echo Show 5 and 8, Portal Go)
//    playing music and speech through their own speakers, mixed with
//    clean speech and wake words, with a loss that charges eight times
//    more for removing the nearby voice than for leaving echo behind. The
//    wake word and the stop word stay audible over music and over a long
//    answer, which is what the kiosk needs from it.
//
// The mask stands aside in two cases, and the microphone then passes
// through unchanged (one frame late, like everything else here):
// - the reference has been silent for a second and a half, so there is no
//   echo to remove;
// - the microphone carries no echo: over three seconds of playback it stays
//   within a few dB of the level it rests at with nothing playing. That is
//   a microphone path with its own canceller (a Portal, a driver that mutes
//   the microphone during playback), and a second canceller over it only
//   costs speech. The mask comes back as soon as echo shows up again. The
//   test is on the microphone, not on what the linear filter removes: an
//   Echo Dot's echo is almost all distortion the filter cannot touch, and
//   the mask is what takes it out there.
//
// A high pass filter at 60 Hz takes the rumble out of the output, as the
// previous canceller's did.

#include "echo_canceller.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include "os_support.h"
#include "kiss_fftr.h"
}

namespace kiosk {

namespace {

constexpr int kFft = 320;
constexpr int kHop = EchoCanceller::kFrame;
constexpr int kBins = kFft / 2 + 1;
constexpr int kSignals = 4;

// Speex's filter runs 12 dB below full scale, where it was tuned and
// tested, and the output is scaled back up.
constexpr float kHeadroom = 0.251189f;
constexpr int kFilterMs = 256;

// Where the echo of a reference sample shows up in the microphone, in
// frames, for the mask's reference feature: 110 to 135 ms on the devices
// measured.
constexpr int kReferenceDelayFrames = 13;

// No reference for this long and the microphone goes through as it is.
constexpr int kQuietReferenceFrames = 150;
constexpr float kQuietReferencePower = 1e-7f;

// The back-off: the microphone's level over frames with an audible
// reference (above -45 dBFS), averaged in dB over three seconds so a
// loud start does not hide a quiet present for half a minute. Echo shows as
// that level standing 6 dB over the floor the microphone rests at once the
// reference has been quiet for a third of a second, and at least at
// -60 dBFS, below which it does not matter; the mask then works, and
// stands aside again once the level falls within 3 dB of the floor or
// under -66 dBFS. The floor follows a drop quickly and a rise slowly (a
// voice in a pause must not lift it), never reads below -80 dBFS (a capture that starts with digital silence
// must not pin it at zero) and counts only once a second of quiet has
// been seen; before that only the fixed levels apply.
constexpr float kActiveReferencePower = 3.2e-5f;
constexpr int kEvidenceFrames = 300;
constexpr float kEvidenceAlpha = 1.f / kEvidenceFrames;
constexpr int kFloorAfterQuietFrames = 30;
constexpr int kFloorFrames = 100;
constexpr float kFloorFallAlpha = 0.1f;
// The floor rises at most 2 dB a second (0.02 dB a frame) so a voice in a
// pause cannot lift it to its own level, as a fraction of the gap did.
constexpr float kFloorRisePerFrame = 1.0046f;
constexpr float kFloorLeast = 1e-8f;
constexpr float kEngageOverFloorDb = 6.f;
constexpr float kReleaseOverFloorDb = 3.f;
constexpr float kEngageLeastDb = -60.f;
constexpr float kReleaseLeastDb = -66.f;
constexpr float kSilenceDb = -100.f;

float PowerDb(float power) { return power > 1e-10f ? 10.f * std::log10(power) : kSilenceDb; }

constexpr double kHighPassHz = 60.0;
constexpr double kPi = 3.14159265358979323846;

float Sigmoid(float x) { return 1.f / (1.f + std::exp(-x)); }

}  // namespace

// The mask network and its weights, in the layout res/export.py writes:
// a header of six floats [bands, hidden, layers, inputs, bins, outputs],
// the band matrix (bins x bands), the input layer, each GRU layer's
// weight_ih, bias_ih, weight_hh, bias_hh in PyTorch's r, z, n order, and
// the output layer.
class MaskModel {
public:
    bool Load(const uint8_t* blob, size_t bytes) {
        const float* at = reinterpret_cast<const float*>(blob);
        const float* end = at + bytes / sizeof(float);
        if (end - at < 6) return false;
        bands = static_cast<int>(at[0]);
        hidden = static_cast<int>(at[1]);
        layers = static_cast<int>(at[2]);
        inputs = static_cast<int>(at[3]);
        const int bins = static_cast<int>(at[4]);
        outputs = static_cast<int>(at[5]);
        at += 6;
        if (bins != kBins || bands <= 0 || hidden <= 0 || layers <= 0 || inputs != kSignals) return false;
        if (outputs != kBins && outputs != bands) return false;
        auto take = [&](std::vector<float>& into, size_t count) {
            if (static_cast<size_t>(end - at) < count) return false;
            into.assign(at, at + count);
            at += count;
            return true;
        };
        bool ok = take(bandMatrix, static_cast<size_t>(kBins) * bands) &&
                  take(inputWeight, static_cast<size_t>(hidden) * bands * inputs) &&
                  take(inputBias, hidden);
        weightIh.resize(layers); biasIh.resize(layers); weightHh.resize(layers); biasHh.resize(layers);
        for (int l = 0; l < layers && ok; ++l) {
            ok = take(weightIh[l], static_cast<size_t>(3) * hidden * hidden) && take(biasIh[l], 3 * hidden) &&
                 take(weightHh[l], static_cast<size_t>(3) * hidden * hidden) && take(biasHh[l], 3 * hidden);
        }
        ok = ok && take(outputWeight, static_cast<size_t>(outputs) * hidden) && take(outputBias, outputs);
        if (!ok || at != end) return false;
        for (Run& run : runs) run.state.assign(static_cast<size_t>(layers) * hidden, 0.f);
        runs[0].age = 0;
        runs[1].age = kHorizon / 2;
        features.resize(hidden);
        x.resize(hidden);
        gate.resize(3 * hidden);
        candidate.resize(hidden);
        return true;
    }

    // features: bands x inputs log energies; gains: one per output.
    //
    // The network was trained on windows of kHorizon frames, each from a
    // zero state, and its state is not stable past that: run on for
    // minutes it drifts into taking out the start of what the user says.
    // So two copies of the state run side by side, each starting over
    // every kHorizon frames, half a horizon apart, and the gains come
    // from the one that has seen more, with a short crossfade where they
    // swap. That keeps every frame inside what the network was trained on
    // with at least half a window of context behind it.
    void Step(const float* features, float* gains) {
        const int in = bands * inputs;
        for (int i = 0; i < hidden; ++i) {
            float a = inputBias[i];
            const float* w = &inputWeight[static_cast<size_t>(i) * in];
            for (int k = 0; k < in; ++k) a += w[k] * features[k];
            this->features[i] = a > 0 ? a : 0;
        }
        std::fill(gains, gains + outputs, 0.f);
        for (Run& run : runs) {
            if (run.age >= kHorizon) {
                std::fill(run.state.begin(), run.state.end(), 0.f);
                run.age = 0;
            }
            std::memcpy(x.data(), this->features.data(), hidden * sizeof(float));
            for (int l = 0; l < layers; ++l) {
                float* h = &run.state[static_cast<size_t>(l) * hidden];
                for (int j = 0; j < 3 * hidden; ++j) {
                    float a = biasIh[l][j], b = biasHh[l][j];
                    const float* wi = &weightIh[l][static_cast<size_t>(j) * hidden];
                    const float* wh = &weightHh[l][static_cast<size_t>(j) * hidden];
                    for (int k = 0; k < hidden; ++k) {
                        a += wi[k] * x[k];
                        b += wh[k] * h[k];
                    }
                    if (j < 2 * hidden) {
                        gate[j] = Sigmoid(a + b);
                    } else {
                        gate[j] = a;
                        candidate[j - 2 * hidden] = b;
                    }
                }
                for (int i = 0; i < hidden; ++i) {
                    const float r = gate[i], z = gate[hidden + i];
                    const float n = std::tanh(gate[2 * hidden + i] + r * candidate[i]);
                    h[i] = (1 - z) * n + z * h[i];
                }
                std::memcpy(x.data(), h, hidden * sizeof(float));
            }
            const float weight = Weight(run.age++);
            if (weight <= 0.f) continue;
            for (int i = 0; i < outputs; ++i) {
                float a = outputBias[i];
                const float* w = &outputWeight[static_cast<size_t>(i) * hidden];
                for (int k = 0; k < hidden; ++k) a += w[k] * x[k];
                gains[i] += weight * Sigmoid(a);
            }
        }
    }

    // Back to the state the network starts a window from. Called when the
    // mask comes back after standing aside: the state it froze with then
    // describes a sound that ended seconds ago, and the network was never
    // trained to continue from one. A zero state is what every training
    // window began with.
    void Restart() {
        for (Run& run : runs) std::fill(run.state.begin(), run.state.end(), 0.f);
        runs[0].age = 0;
        runs[1].age = kHorizon / 2;
    }

    int bands = 0, hidden = 0, layers = 0, inputs = 0, outputs = 0;
    std::vector<float> bandMatrix;

private:
    // The training window, in frames, and how long the two runs crossfade.
    static constexpr int kHorizon = 400;
    static constexpr int kCrossfade = 16;

    // A run's share of the gains by its age: none through the first half
    // of its window, all of them through the second, ramping in as the
    // other run ramps out just before it starts over. The two shares
    // always add up to one.
    static float Weight(int age) {
        constexpr int half = kHorizon / 2;
        if (age < half - kCrossfade) return 0.f;
        if (age < half) return static_cast<float>(age - (half - kCrossfade)) / kCrossfade;
        if (age < kHorizon - kCrossfade) return 1.f;
        return 1.f - static_cast<float>(age - (kHorizon - kCrossfade)) / kCrossfade;
    }

    struct Run {
        std::vector<float> state;
        int age = 0;
    };
    Run runs[2];

    std::vector<float> inputWeight, inputBias, outputWeight, outputBias;
    std::vector<std::vector<float>> weightIh, biasIh, weightHh, biasHh;
    std::vector<float> features, x, gate, candidate;
};

EchoCanceller::EchoCanceller() {
    const int taps = kFilterMs * kRate / 1000 / kHop * kHop;
    speex_ = speex_echo_state_init_mc(kHop, taps, 1, kReferenceChannels);
    int rate = kRate;
    speex_echo_ctl(speex_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    forward_ = kiss_fftr_alloc(kFft, 0, nullptr, nullptr);
    inverse_ = kiss_fftr_alloc(kFft, 1, nullptr, nullptr);
    for (int i = 0; i < kFft; ++i) {
        window_[i] = std::sqrt(0.5f - 0.5f * std::cos(2.f * static_cast<float>(kPi) * i / kFft));
    }
    std::memset(history_, 0, sizeof(history_));
    std::memset(overlap_, 0, sizeof(overlap_));
    spectra_.assign(static_cast<size_t>(kSignals) * kBins * 2, 0.f);
    referenceRing_.assign(static_cast<size_t>(kReferenceDelayFrames + 1) * kHop, 0.f);

    // Second order Butterworth high pass, bilinear transform.
    const double k = std::tan(kPi * kHighPassHz / kRate);
    const double norm = 1.0 / (1.0 + std::sqrt(2.0) * k + k * k);
    highPassB0_ = norm;
    highPassB1_ = -2.0 * norm;
    highPassB2_ = norm;
    highPassA1_ = 2.0 * (k * k - 1.0) * norm;
    highPassA2_ = (1.0 - std::sqrt(2.0) * k + k * k) * norm;
}

EchoCanceller::~EchoCanceller() {
    speex_echo_state_destroy(speex_);
    kiss_fftr_free(forward_);
    kiss_fftr_free(inverse_);
}

bool EchoCanceller::Load(const uint8_t* blob, size_t bytes) {
    auto model = std::make_unique<MaskModel>();
    if (!model->Load(blob, bytes)) return false;
    features_.assign(static_cast<size_t>(model->bands) * model->inputs, 0.f);
    gains_.assign(model->outputs, 1.f);
    model_ = std::move(model);
    return true;
}

void EchoCanceller::Transform(int which, const float* samples) {
    float* history = history_[which];
    std::memmove(history, history + kHop, (kFft - kHop) * sizeof(float));
    std::memcpy(history + kFft - kHop, samples, kHop * sizeof(float));
    float frame[kFft];
    for (int i = 0; i < kFft; ++i) frame[i] = history[i] * window_[i];
    kiss_fftr(static_cast<kiss_fftr_cfg>(forward_), frame,
              reinterpret_cast<kiss_fft_cpx*>(&spectra_[static_cast<size_t>(which) * kBins * 2]));
}

void EchoCanceller::BandEnergies(int which, float* energies) const {
    const int bands = model_->bands;
    const float* spectrum = &spectra_[static_cast<size_t>(which) * kBins * 2];
    for (int k = 0; k < bands; ++k) energies[k] = 0.f;
    for (int b = 0; b < kBins; ++b) {
        const float power = spectrum[2 * b] * spectrum[2 * b] + spectrum[2 * b + 1] * spectrum[2 * b + 1];
        const float* row = &model_->bandMatrix[static_cast<size_t>(b) * bands];
        for (int k = 0; k < bands; ++k) energies[k] += power * row[k];
    }
    for (int k = 0; k < bands; ++k) energies[k] = std::log10(energies[k] + 1e-9f) + 4.f;
}

void EchoCanceller::Process(const int16_t* microphone, const int16_t* reference, int16_t* out) {
    // 1. The linear filter.
    int16_t near[kHop], far[kReferenceChannels * kHop], linear[kHop];
    for (int i = 0; i < kHop; ++i) near[i] = static_cast<int16_t>(microphone[i] * kHeadroom);
    for (int i = 0; i < kReferenceChannels * kHop; ++i) far[i] = static_cast<int16_t>(reference[i] * kHeadroom);
    speex_echo_cancellation(speex_, near, far, linear);

    float lin[kHop], mic[kHop], ref[kHop], removed[kHop];
    float microphonePower = 0.f, referencePower = 0.f;
    for (int i = 0; i < kHop; ++i) {
        lin[i] = linear[i] / kHeadroom / 32768.f;
        mic[i] = microphone[i] / 32768.f;
        ref[i] = 0.5f * (reference[2 * i] + reference[2 * i + 1]) / 32768.f;
        removed[i] = mic[i] - lin[i];
        microphonePower += mic[i] * mic[i];
        referencePower += ref[i] * ref[i];
    }
    microphonePower /= kHop;
    referencePower /= kHop;

    // 2. Spectra of the four signals the mask looks at. The reference one
    // is read from the ring, a round trip to the speaker and back ago.
    std::memcpy(&referenceRing_[static_cast<size_t>(ringAt_) * kHop], ref, kHop * sizeof(float));
    ringAt_ = (ringAt_ + 1) % (kReferenceDelayFrames + 1);
    Transform(0, lin);
    Transform(1, mic);
    if (model_) {
        Transform(2, removed);
        Transform(3, &referenceRing_[static_cast<size_t>(ringAt_) * kHop]);
    }

    // 3. Whether the mask should work on this frame.
    if (referencePower < kQuietReferencePower) {
        quietReferenceFrames_ = std::min(quietReferenceFrames_ + 1, 1000);
    } else {
        quietReferenceFrames_ = 0;
    }
    if (quietReferenceFrames_ > kFloorAfterQuietFrames) {
        // The floor is the room with nothing playing, and the pause between
        // two answers is also where the user talks: the first answer of a
        // conversation was cut short by a voice 30 dB over the room, and a
        // floor that followed it made the next answer's echo look like no
        // echo at all, so the mask stood aside through it. Hence the
        // floor settles on the quietest of its first frames and afterwards
        // rises only a little per frame, however loud the pause is, while
        // it still drops at once.
        if (floorFrames_ == 0) floor_ = microphonePower;
        else if (floorFrames_ < kFloorFrames) floor_ = std::min(floor_, microphonePower);
        else if (microphonePower < floor_) floor_ += kFloorFallAlpha * (microphonePower - floor_);
        else floor_ = std::min(microphonePower, floor_ * kFloorRisePerFrame);
        if (floorFrames_ < kFloorFrames) ++floorFrames_;
    } else if (referencePower >= kActiveReferencePower) {
        const float levelDb = PowerDb(microphonePower);
        if (evidenceFrames_ == 0) microphoneEvidenceDb_ = levelDb;
        else microphoneEvidenceDb_ += kEvidenceAlpha * (levelDb - microphoneEvidenceDb_);
        if (evidenceFrames_ < kEvidenceFrames) ++evidenceFrames_;
        if (evidenceFrames_ >= kEvidenceFrames) {
            const float floorDb = floorFrames_ >= kFloorFrames ? PowerDb(std::max(floor_, kFloorLeast)) : kSilenceDb;
            const float engage = std::max(floorDb + kEngageOverFloorDb, kEngageLeastDb);
            const float release = std::max(floorDb + kReleaseOverFloorDb, kReleaseLeastDb);
            if (engaged_ && microphoneEvidenceDb_ < release) engaged_ = false;
            if (!engaged_ && microphoneEvidenceDb_ >= engage) engaged_ = true;
        }
    }
    const bool masking = model_ && engaged_ && quietReferenceFrames_ < kQuietReferenceFrames;
    // The mask only steps while it works, so across a pause in playback
    // its state stands still. Coming back, it starts the window over
    // rather than continue from a state about a sound that is gone: on a
    // captured conversation that took the first seconds of the answer
    // after a barge-in from a median of 65 to 22, and the next answer's
    // from 114 to 42.
    if (masking && !masking_) model_->Restart();
    masking_ = masking;

    // 4. The gains, on the filter's output when masking and on the
    // microphone itself otherwise.
    const float* source = &spectra_[0];
    if (masking) {
        const int bands = model_->bands;
        for (int s = 0; s < kSignals; ++s) BandEnergies(s, &features_[static_cast<size_t>(s) * bands]);
        model_->Step(features_.data(), gains_.data());
    } else {
        source = &spectra_[static_cast<size_t>(1) * kBins * 2];
    }
    kiss_fft_cpx masked[kBins];
    for (int b = 0; b < kBins; ++b) {
        float gain = 1.f;
        if (masking) {
            if (model_->outputs == kBins) {
                gain = gains_[b];
            } else {
                gain = 0.f;
                const float* row = &model_->bandMatrix[static_cast<size_t>(b) * model_->bands];
                for (int k = 0; k < model_->bands; ++k) gain += gains_[k] * row[k];
            }
        }
        masked[b].r = source[2 * b] * gain;
        masked[b].i = source[2 * b + 1] * gain;
    }

    // 5. Back to samples: overlap-add, then the high pass.
    float frame[kFft];
    kiss_fftri(static_cast<kiss_fftr_cfg>(inverse_), masked, frame);
    for (int i = 0; i < kHop; ++i) {
        const double sample = overlap_[i] + frame[i] * window_[i] / kFft;
        overlap_[i] = frame[kHop + i] * window_[kHop + i] / kFft;
        const double filtered = highPassB0_ * sample + highPassZ1_;
        highPassZ1_ = highPassB1_ * sample - highPassA1_ * filtered + highPassZ2_;
        highPassZ2_ = highPassB2_ * sample - highPassA2_ * filtered;
        const double scaled = filtered * 32768.0;
        out[i] = static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0));
        outputEnergy_ += filtered * filtered;
    }
    microphoneEnergy_ += static_cast<double>(microphonePower) * kHop;
}

double EchoCanceller::RemovedDb() const {
    if (microphoneEnergy_ <= 0 || outputEnergy_ <= 0) return 0;
    return 10.0 * std::log10(microphoneEnergy_ / outputEnergy_);
}

NoiseSuppressor::NoiseSuppressor() {
    speex_ = speex_preprocess_state_init(EchoCanceller::kFrame, EchoCanceller::kRate);
    int on = 1, off = 0;
    speex_preprocess_ctl(speex_, SPEEX_PREPROCESS_SET_DENOISE, &on);
    speex_preprocess_ctl(speex_, SPEEX_PREPROCESS_SET_AGC, &off);
    speex_preprocess_ctl(speex_, SPEEX_PREPROCESS_SET_DEREVERB, &off);
    speex_preprocess_ctl(speex_, SPEEX_PREPROCESS_SET_VAD, &off);
    // 12 dB, the moderate setting of the previous suppressor: it took an
    // Echo Show 8's hiss out of an intercom call and left the room (an air
    // conditioner's hum) as it was.
    int suppress = -12;
    speex_preprocess_ctl(speex_, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
}

NoiseSuppressor::~NoiseSuppressor() {
    speex_preprocess_state_destroy(speex_);
}

void NoiseSuppressor::Process(int16_t* frame) {
    speex_preprocess_run(speex_, frame);
}

}  // namespace kiosk
