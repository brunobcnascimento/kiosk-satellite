// JNI bridge for SoftwareEcho.kt: the kiosk's echo canceller and noise
// suppressor (echo_canceller.cpp) over the microphone, 10 ms at a time,
// with what the kiosk plays as the reference. Both directions come from
// the capture thread.

#include <jni.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "echo_canceller.h"

namespace {

constexpr const char* kTag = "SoftwareEcho";
constexpr int kFrame = kiosk::EchoCanceller::kFrame;
constexpr int kReferenceSamples = kFrame * kiosk::EchoCanceller::kReferenceChannels;

struct Echo {
    std::unique_ptr<kiosk::EchoCanceller> canceller;
    std::unique_ptr<kiosk::NoiseSuppressor> suppressor;
    std::mutex lock;
    int16_t reference[kReferenceSamples]{};
};

Echo* from(jlong handle) { return reinterpret_cast<Echo*>(handle); }

std::vector<uint8_t> ReadAsset(JNIEnv* env, jobject assets, jstring name) {
    std::vector<uint8_t> bytes;
    AAssetManager* manager = AAssetManager_fromJava(env, assets);
    if (!manager) return bytes;
    const char* path = env->GetStringUTFChars(name, nullptr);
    AAsset* asset = AAssetManager_open(manager, path, AASSET_MODE_BUFFER);
    env->ReleaseStringUTFChars(name, path);
    if (!asset) return bytes;
    const off_t length = AAsset_getLength(asset);
    if (length > 0) {
        bytes.resize(static_cast<size_t>(length));
        if (AAsset_read(asset, bytes.data(), bytes.size()) != length) bytes.clear();
    }
    AAsset_close(asset);
    return bytes;
}

}  // namespace

// The canceller, with the mask's weights from the app's assets. Without
// the weights it still runs, as the linear filter alone.
extern "C" JNIEXPORT jlong JNICALL
Java_me_jxl_kiosk_1satellite_SoftwareEcho_nativeCreate(
    JNIEnv* env, jclass, jobject assets, jstring modelAsset) {
    auto* echo = new Echo;
    echo->canceller = std::make_unique<kiosk::EchoCanceller>();
    const std::vector<uint8_t> blob = ReadAsset(env, assets, modelAsset);
    const bool mask = !blob.empty() && echo->canceller->Load(blob.data(), blob.size());
    if (!mask) __android_log_print(ANDROID_LOG_WARN, kTag, "mask weights unavailable, linear filter only");
    __android_log_print(ANDROID_LOG_INFO, kTag, "started (16 kHz, stereo reference, mask %s)",
                        mask ? "on" : "off");
    return reinterpret_cast<jlong>(echo);
}

// One 10 ms frame of what the speaker presented, 16 kHz stereo PCM16.
extern "C" JNIEXPORT void JNICALL
Java_me_jxl_kiosk_1satellite_SoftwareEcho_nativeRender(
    JNIEnv* env, jclass, jlong handle, jbyteArray pcm, jint offset) {
    auto* echo = from(handle);
    std::lock_guard<std::mutex> guard(echo->lock);
    env->GetByteArrayRegion(pcm, offset, kReferenceSamples * 2,
                            reinterpret_cast<jbyte*>(echo->reference));
}

// One 10 ms frame of the microphone, 16 kHz PCM16, cleaned in place
// against the reference frame given just before it.
extern "C" JNIEXPORT void JNICALL
Java_me_jxl_kiosk_1satellite_SoftwareEcho_nativeCapture(
    JNIEnv* env, jclass, jlong handle, jbyteArray pcm, jint offset) {
    auto* echo = from(handle);
    int16_t block[kFrame];
    env->GetByteArrayRegion(pcm, offset, kFrame * 2, reinterpret_cast<jbyte*>(block));
    {
        std::lock_guard<std::mutex> guard(echo->lock);
        echo->canceller->Process(block, echo->reference, block);
    }
    env->SetByteArrayRegion(pcm, offset, kFrame * 2, reinterpret_cast<const jbyte*>(block));
}

// [dB removed so far, 1 while the mask works and 0 while it stands aside].
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_me_jxl_kiosk_1satellite_SoftwareEcho_nativeStats(JNIEnv* env, jclass, jlong handle) {
    auto* echo = from(handle);
    jdouble values[2];
    {
        std::lock_guard<std::mutex> guard(echo->lock);
        values[0] = echo->canceller->RemovedDb();
        values[1] = echo->canceller->MaskEngaged() ? 1.0 : 0.0;
    }
    jdoubleArray out = env->NewDoubleArray(2);
    env->SetDoubleArrayRegion(out, 0, 2, values);
    return out;
}

extern "C" JNIEXPORT void JNICALL
Java_me_jxl_kiosk_1satellite_SoftwareEcho_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    delete from(handle);
}

// The noise suppressor, over every capture frame whether or not anything
// plays: a microphone's hiss is there all the time, and the canceller only
// runs while there is an echo.
extern "C" JNIEXPORT jlong JNICALL
Java_me_jxl_kiosk_1satellite_SoftwareEcho_nativeNsCreate(JNIEnv*, jclass) {
    auto* echo = new Echo;
    echo->suppressor = std::make_unique<kiosk::NoiseSuppressor>();
    __android_log_print(ANDROID_LOG_INFO, kTag, "noise suppression started (16 kHz)");
    return reinterpret_cast<jlong>(echo);
}

// One 10 ms frame of the microphone, PCM16, suppressed in place.
extern "C" JNIEXPORT void JNICALL
Java_me_jxl_kiosk_1satellite_SoftwareEcho_nativeNsProcess(
    JNIEnv* env, jclass, jlong handle, jbyteArray pcm, jint offset) {
    auto* echo = from(handle);
    int16_t block[kFrame];
    env->GetByteArrayRegion(pcm, offset, kFrame * 2, reinterpret_cast<jbyte*>(block));
    {
        std::lock_guard<std::mutex> guard(echo->lock);
        echo->suppressor->Process(block);
    }
    env->SetByteArrayRegion(pcm, offset, kFrame * 2, reinterpret_cast<const jbyte*>(block));
}
