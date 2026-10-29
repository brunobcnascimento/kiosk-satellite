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

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define KIOSK_NEON 1
#else
#define KIOSK_NEON 0
#endif
// Half precision weights converted by the FPU as they are read: every
// 64-bit ARM, and a 32-bit build with -mfpu=neon-fp16 (any NEON core
// from the Cortex-A9 on). Elsewhere the scalar conversion below.
#if KIOSK_NEON && (defined(__aarch64__) || (defined(__ARM_FP) && (__ARM_FP & 2)))
#define KIOSK_FP16 1
#else
#define KIOSK_FP16 0
#endif
#if defined(__arm__) && !defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

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
//
// The step is where the canceller spends its time on a small core: a
// Cortex-A53 spent 1.4 ms a frame on a scalar version, most of it
// waiting on the weights. So the weights are half precision, the dot
// products run on NEON where there is NEON, two rows at a time so each
// loaded input vector feeds two rows, and the two state copies (below)
// go through a layer together so every weight row is read once for
// both. The same core now takes 0.7 ms.
namespace {

// IEEE half <-> float, round to nearest even, subnormals kept.
inline uint16_t FloatToHalf(float value) {
    uint32_t u;
    std::memcpy(&u, &value, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    const int exponent = static_cast<int>((u >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = u & 0x7FFFFFu;
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const int shift = 14 - exponent;
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1), midpoint = 1u << (shift - 1);
        if (rest > midpoint || (rest == midpoint && (half & 1))) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1))) ++half;
    return static_cast<uint16_t>(sign | half);
}

inline float HalfToFloat(uint16_t half) {
    const uint32_t sign = (static_cast<uint32_t>(half) & 0x8000u) << 16;
    uint32_t exponent = (half >> 10) & 0x1Fu, mantissa = half & 0x3FFu;
    uint32_t u;
    if (exponent == 0) {
        if (mantissa == 0) {
            u = sign;
        } else {
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400u)) { mantissa <<= 1; --exponent; }
            u = sign | (exponent << 23) | ((mantissa & 0x3FFu) << 13);
        }
    } else if (exponent == 31) {
        u = sign | 0x7F800000u | (mantissa << 13);
    } else {
        u = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &u, 4);
    return value;
}

#if KIOSK_FP16
inline float32x4_t LoadW(const uint16_t* p) { return vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16(p))); }
#endif

#if KIOSK_NEON
inline float Sum(float32x4_t v) {
#if defined(__aarch64__)
    return vaddvq_f32(v);
#else
    float32x2_t s = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    s = vpadd_f32(s, s);
    return vget_lane_f32(s, 0);
#endif
}
#endif

// exp, sigmoid and tanh over a vector in place, four lanes at a time
// where there is NEON: the step takes 1,700 of them a frame, and libm's
// one at a time were a tenth of its cost on a Cortex-A53. The exp is
// Cephes' polynomial on the fraction after taking out the power of two,
// within 2e-7 of libm, and the reciprocal is refined twice.
#if KIOSK_NEON
inline float32x4_t ExpNeon(float32x4_t x) {
    x = vminq_f32(vmaxq_f32(x, vdupq_n_f32(-87.f)), vdupq_n_f32(87.f));
    // n = round(x * log2e), r = x - n * ln2.
    float32x4_t n = vcvtq_f32_s32(vcvtq_s32_f32(vaddq_f32(vmulq_n_f32(x, 1.44269504088896341f),
                                               vbslq_f32(vcltq_f32(x, vdupq_n_f32(0.f)), vdupq_n_f32(-0.5f), vdupq_n_f32(0.5f)))));
    float32x4_t r = vsubq_f32(x, vmulq_n_f32(n, 0.693359375f));
    r = vsubq_f32(r, vmulq_n_f32(n, -2.12194440e-4f));
    float32x4_t p = vdupq_n_f32(1.9875691500e-4f);
    p = vaddq_f32(vmulq_f32(p, r), vdupq_n_f32(1.3981999507e-3f));
    p = vaddq_f32(vmulq_f32(p, r), vdupq_n_f32(8.3334519073e-3f));
    p = vaddq_f32(vmulq_f32(p, r), vdupq_n_f32(4.1665795894e-2f));
    p = vaddq_f32(vmulq_f32(p, r), vdupq_n_f32(1.6666665459e-1f));
    p = vaddq_f32(vmulq_f32(p, r), vdupq_n_f32(5.0000001201e-1f));
    p = vaddq_f32(vaddq_f32(vmulq_f32(p, vmulq_f32(r, r)), r), vdupq_n_f32(1.f));
    const int32x4_t e = vshlq_n_s32(vaddq_s32(vcvtq_s32_f32(n), vdupq_n_s32(127)), 23);
    return vmulq_f32(p, vreinterpretq_f32_s32(e));
}

inline float32x4_t ReciprocalNeon(float32x4_t d) {
    float32x4_t r = vrecpeq_f32(d);
    r = vmulq_f32(r, vrecpsq_f32(d, r));
    r = vmulq_f32(r, vrecpsq_f32(d, r));
    return r;
}

inline float32x4_t SigmoidNeon(float32x4_t x) {
    return ReciprocalNeon(vaddq_f32(vdupq_n_f32(1.f), ExpNeon(vnegq_f32(x))));
}
#endif

inline void SigmoidN(float* v, int n) {
    int k = 0;
#if KIOSK_NEON
    for (; k + 4 <= n; k += 4) vst1q_f32(v + k, SigmoidNeon(vld1q_f32(v + k)));
#endif
    for (; k < n; ++k) v[k] = Sigmoid(v[k]);
}

inline void TanhN(float* v, int n) {
    int k = 0;
#if KIOSK_NEON
    for (; k + 4 <= n; k += 4) {
        const float32x4_t s = SigmoidNeon(vmulq_n_f32(vld1q_f32(v + k), 2.f));
        vst1q_f32(v + k, vsubq_f32(vmulq_n_f32(s, 2.f), vdupq_n_f32(1.f)));
    }
#endif
    for (; k < n; ++k) v[k] = std::tanh(v[k]);
}

// Two weight rows against one vector: out0 = w0 . x, out1 = w1 . x.
inline void Dot2(bool fast, const uint16_t* w0, const uint16_t* w1, const float* x, int n, float& out0, float& out1) {
    int k = 0;
    float s0 = 0.f, s1 = 0.f;
#if KIOSK_FP16
    if (fast) {
        float32x4_t a0 = vdupq_n_f32(0.f), a1 = vdupq_n_f32(0.f);
        for (; k + 4 <= n; k += 4) {
            const float32x4_t xv = vld1q_f32(x + k);
            a0 = vmlaq_f32(a0, LoadW(w0 + k), xv);
            a1 = vmlaq_f32(a1, LoadW(w1 + k), xv);
        }
        s0 = Sum(a0);
        s1 = Sum(a1);
    }
#else
    (void)fast;
#endif
    for (; k < n; ++k) {
        s0 += HalfToFloat(w0[k]) * x[k];
        s1 += HalfToFloat(w1[k]) * x[k];
    }
    out0 = s0;
    out1 = s1;
}

// Two gate rows of a GRU layer for both runs at once: the input rows wi0,
// wi1 against the runs' inputs x0 and x1, the hidden rows wh0, wh1
// against their states h0 and h1. out = {wi0.x0, wi0.x1, wh0.h0, wh0.h1,
// wi1.x0, wi1.x1, wh1.h0, wh1.h1}. With kShared the runs share one input
// (the first layer reads the same features) and x1 is not read.
template <bool kShared>
inline void Gates2(bool fast, const uint16_t* wi0, const uint16_t* wi1, const uint16_t* wh0, const uint16_t* wh1,
                   const float* x0, const float* x1, const float* h0, const float* h1, int n, float* out) {
    int k = 0;
    float s[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
#if KIOSK_FP16
    if (fast) {
        float32x4_t a0 = vdupq_n_f32(0.f), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
        for (; k + 4 <= n; k += 4) {
            const float32x4_t vx0 = vld1q_f32(x0 + k);
            const float32x4_t vh0 = vld1q_f32(h0 + k), vh1 = vld1q_f32(h1 + k);
            const float32x4_t i0 = LoadW(wi0 + k), i1 = LoadW(wi1 + k);
            const float32x4_t g0 = LoadW(wh0 + k), g1 = LoadW(wh1 + k);
            a0 = vmlaq_f32(a0, i0, vx0);
            a4 = vmlaq_f32(a4, i1, vx0);
            if (!kShared) {
                const float32x4_t vx1 = vld1q_f32(x1 + k);
                a1 = vmlaq_f32(a1, i0, vx1);
                a5 = vmlaq_f32(a5, i1, vx1);
            }
            a2 = vmlaq_f32(a2, g0, vh0);
            a3 = vmlaq_f32(a3, g0, vh1);
            a6 = vmlaq_f32(a6, g1, vh0);
            a7 = vmlaq_f32(a7, g1, vh1);
        }
        s[0] = Sum(a0); s[1] = Sum(a1); s[2] = Sum(a2); s[3] = Sum(a3);
        s[4] = Sum(a4); s[5] = Sum(a5); s[6] = Sum(a6); s[7] = Sum(a7);
    }
#else
    (void)fast;
#endif
    for (; k < n; ++k) {
        const float i0 = HalfToFloat(wi0[k]), i1 = HalfToFloat(wi1[k]), g0 = HalfToFloat(wh0[k]), g1 = HalfToFloat(wh1[k]);
        s[0] += i0 * x0[k];
        s[4] += i1 * x0[k];
        if (!kShared) {
            s[1] += i0 * x1[k];
            s[5] += i1 * x1[k];
        }
        s[2] += g0 * h0[k];
        s[3] += g0 * h1[k];
        s[6] += g1 * h0[k];
        s[7] += g1 * h1[k];
    }
    if (kShared) {
        s[1] = s[0];
        s[5] = s[4];
    }
    std::memcpy(out, s, sizeof(s));
}

}  // namespace

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
        // The weight matrices are kept in half precision: half the bytes
        // the step reads every frame, and the gains come out within
        // 0.1 dB of single precision on captured conversations.
        auto takeHalf = [&](std::vector<uint16_t>& into, size_t count) {
            if (static_cast<size_t>(end - at) < count) return false;
            into.resize(count);
            for (size_t i = 0; i < count; ++i) into[i] = FloatToHalf(at[i]);
            at += count;
            return true;
        };
        bool ok = take(bandMatrix, static_cast<size_t>(kBins) * bands) &&
                  takeHalf(inputWeight, static_cast<size_t>(hidden) * bands * inputs) &&
                  take(inputBias, hidden);
        weightIh.resize(layers); biasIh.resize(layers); weightHh.resize(layers); biasHh.resize(layers);
        for (int l = 0; l < layers && ok; ++l) {
            ok = takeHalf(weightIh[l], static_cast<size_t>(3) * hidden * hidden) && take(biasIh[l], 3 * hidden) &&
                 takeHalf(weightHh[l], static_cast<size_t>(3) * hidden * hidden) && take(biasHh[l], 3 * hidden);
        }
        ok = ok && takeHalf(outputWeight, static_cast<size_t>(outputs) * hidden) && take(outputBias, outputs);
        if (!ok || at != end) return false;
        fast = KIOSK_FP16 != 0;
#if defined(__arm__) && !defined(__aarch64__)
        // A 32-bit core without the half precision conversions (none
        // newer than a Cortex-A8) takes the scalar path.
        fast = fast && (getauxval(AT_HWCAP) & HWCAP_HALF) != 0;
#endif
        // The band matrix is a filterbank: almost every entry is zero, so
        // the energies and the gains per bin are summed over its nonzero
        // entries only.
        bandTerms.clear();
        for (int b = 0; b < kBins; ++b) {
            for (int k = 0; k < bands; ++k) {
                const float w = bandMatrix[static_cast<size_t>(b) * bands + k];
                if (w != 0.f) bandTerms.push_back({b, k, w});
            }
        }
        for (Run& run : runs) {
            run.state.assign(static_cast<size_t>(layers) * hidden, 0.f);
            run.gate.assign(static_cast<size_t>(3) * hidden, 0.f);
            run.candidate.assign(hidden, 0.f);
        }
        runs[0].age = 0;
        runs[1].age = kHorizon / 2;
        features.resize(hidden + 1);   // one spare slot for an odd width's last row pair
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
        float* f = this->features.data();
        int i = 0;
        for (; i + 2 <= hidden; i += 2) {
            Dot2(fast, &inputWeight[static_cast<size_t>(i) * in], &inputWeight[static_cast<size_t>(i + 1) * in], features, in,
                 f[i], f[i + 1]);
        }
        if (i < hidden) Dot2(fast, &inputWeight[static_cast<size_t>(i) * in], &inputWeight[static_cast<size_t>(i) * in], features, in, f[i], f[hidden]);
        for (int k = 0; k < hidden; ++k) {
            const float a = f[k] + inputBias[k];
            f[k] = a > 0 ? a : 0;
        }
        for (Run& run : runs) {
            if (run.age >= kHorizon) {
                std::fill(run.state.begin(), run.state.end(), 0.f);
                run.age = 0;
            }
        }
        // Both runs through each layer together.
        for (int l = 0; l < layers; ++l) {
            const float* x0 = l == 0 ? f : &runs[0].state[static_cast<size_t>(l - 1) * hidden];
            const float* x1 = l == 0 ? f : &runs[1].state[static_cast<size_t>(l - 1) * hidden];
            float* h0 = &runs[0].state[static_cast<size_t>(l) * hidden];
            float* h1 = &runs[1].state[static_cast<size_t>(l) * hidden];
            const uint16_t* wi = weightIh[l].data();
            const uint16_t* wh = weightHh[l].data();
            const float* bi = biasIh[l].data();
            const float* bh = biasHh[l].data();
            const int rows = 3 * hidden;
            float d[8];
            for (int j = 0; j < rows; j += 2) {
                const int j1 = j + 1 < rows ? j + 1 : j;
                const uint16_t* wi0 = wi + static_cast<size_t>(j) * hidden;
                const uint16_t* wi1 = wi + static_cast<size_t>(j1) * hidden;
                const uint16_t* wh0 = wh + static_cast<size_t>(j) * hidden;
                const uint16_t* wh1 = wh + static_cast<size_t>(j1) * hidden;
                if (l == 0) Gates2<true>(fast, wi0, wi1, wh0, wh1, x0, x1, h0, h1, hidden, d);
                else Gates2<false>(fast, wi0, wi1, wh0, wh1, x0, x1, h0, h1, hidden, d);
                for (int r = 0; r < 2; ++r) {
                    Run& run = runs[r];
                    Gate(run, j, bi[j] + d[r], bh[j] + d[2 + r]);
                    if (j1 != j) Gate(run, j1, bi[j1] + d[4 + r], bh[j1] + d[6 + r]);
                }
            }
            for (int r = 0; r < 2; ++r) {
                Run& run = runs[r];
                float* h = r == 0 ? h0 : h1;
                float* g = run.gate.data();
                SigmoidN(g, 2 * hidden);
                float* n = g + 2 * hidden;
                for (int k = 0; k < hidden; ++k) n[k] += g[k] * run.candidate[k];
                TanhN(n, hidden);
                for (int k = 0; k < hidden; ++k) {
                    const float z = g[hidden + k];
                    h[k] = (1 - z) * n[k] + z * h[k];
                }
            }
        }
        std::fill(gains, gains + outputs, 0.f);
        for (Run& run : runs) {
            const float weight = Weight(run.age++);
            if (weight <= 0.f) continue;
            const float* x = &run.state[static_cast<size_t>(layers - 1) * hidden];
            float* out = run.gate.data();   // scratch: outputs <= kBins < 3 * hidden
            int o = 0;
            for (; o + 2 <= outputs; o += 2) {
                Dot2(fast, &outputWeight[static_cast<size_t>(o) * hidden], &outputWeight[static_cast<size_t>(o + 1) * hidden], x, hidden, out[o], out[o + 1]);
            }
            if (o < outputs) {
                float b;
                Dot2(fast, &outputWeight[static_cast<size_t>(o) * hidden], &outputWeight[static_cast<size_t>(o) * hidden], x, hidden, out[o], b);
            }
            for (int i = 0; i < outputs; ++i) out[i] += outputBias[i];
            SigmoidN(out, outputs);
            for (int i = 0; i < outputs; ++i) gains[i] += weight * out[i];
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

    // The band matrix's nonzero entries: bin, band, weight.
    struct BandTerm {
        int bin, band;
        float weight;
    };
    std::vector<BandTerm> bandTerms;

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
        std::vector<float> gate, candidate;
        int age = 0;
    };
    Run runs[2];

    // Row j's gate from its input and hidden sums: r and z are sigmoids of
    // the sum, n keeps the two apart until the reset gate is known.
    void Gate(Run& run, int j, float a, float b) const {
        if (j < 2 * hidden) {
            run.gate[j] = a + b;
        } else {
            run.gate[j] = a;
            run.candidate[j - 2 * hidden] = b;
        }
    }

    std::vector<uint16_t> inputWeight, outputWeight;
    std::vector<float> inputBias, outputBias;
    std::vector<std::vector<uint16_t>> weightIh, weightHh;
    std::vector<std::vector<float>> biasIh, biasHh;
    std::vector<float> features;
    bool fast = false;
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
    float power[kBins];
    for (int b = 0; b < kBins; ++b) power[b] = spectrum[2 * b] * spectrum[2 * b] + spectrum[2 * b + 1] * spectrum[2 * b + 1];
    for (int k = 0; k < bands; ++k) energies[k] = 0.f;
    for (const MaskModel::BandTerm& t : model_->bandTerms) energies[t.band] += power[t.bin] * t.weight;
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
    float binGain[kBins];
    if (!masking) {
        std::fill(binGain, binGain + kBins, 1.f);
    } else if (model_->outputs == kBins) {
        std::memcpy(binGain, gains_.data(), sizeof(binGain));
    } else {
        std::fill(binGain, binGain + kBins, 0.f);
        for (const MaskModel::BandTerm& t : model_->bandTerms) binGain[t.bin] += gains_[t.band] * t.weight;
    }
    for (int b = 0; b < kBins; ++b) {
        masked[b].r = source[2 * b] * binGain[b];
        masked[b].i = source[2 * b + 1] * binGain[b];
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
