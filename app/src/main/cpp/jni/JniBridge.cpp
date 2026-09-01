// JniBridge.cpp — Kotlin↔C++ boundary.
//
// This translation unit contains the JNI-attached entry points declared by
// com.rsp.timing.NativeBridge. Every function here is called only OUTSIDE
// a stimulus event (per Redline Patch 7: zero JNI calls per frame during a
// flash sequence). The render loop itself lives entirely in C++.

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <cstring>
#include "../renderer/RenderPath.h"
#include "../fiducial/Fiducial.h"
#include "../timing/TimingLog.h"

#define TAG "JniBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace rsp;
using rsp::render::RenderPath;
using rsp::render::SequenceConfig;

// Global renderer instance. Owned by the JNI layer; created in nativeInit,
// destroyed in nativeShutdown.
static RenderPath* g_renderer = nullptr;

// Cached JNI method IDs for StimulusEventRecord constructor.
struct RecordClassCache {
    jclass cls = nullptr;
    jmethodID ctor = nullptr;
    bool ready = false;
};
static RecordClassCache g_record_cache;

static void EnsureRecordCache(JNIEnv* env) {
    if (g_record_cache.ready) return;
    jclass local = env->FindClass("com/rsp/timing/StimulusEventRecord");
    if (!local) {
        LOGE("StimulusEventRecord class not found");
        return;
    }
    g_record_cache.cls = reinterpret_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    // Kotlin data-class primary ctor signature is generated; match field order in NativeBridge.kt.
    // eventIndex:Int, fiducialNonceHex:String, targetDurationMs:Double, frameCount:Int,
    // framePeriodNs:Long, achievedDurationMs:Double, scheduledFrameNumber:Long,
    // perFrameIntendedNs:LongArray, perFrameActualNs:LongArray, perFrameSource:IntArray,
    // timingDeviationNs:Long, refreshHzAtEvent:Double, brightnessAtEvent:Float,
    // verificationStatus:String
    g_record_cache.ctor = env->GetMethodID(g_record_cache.cls, "<init>",
        "(ILjava/lang/String;DIJDJ[J[J[IJDFLjava/lang/String;)V");
    if (!g_record_cache.ctor) {
        LOGE("StimulusEventRecord ctor not found");
        return;
    }
    g_record_cache.ready = true;
}

static jobject RecordToJava(JNIEnv* env, const timing::StimulusEventRecord& r) {
    EnsureRecordCache(env);
    if (!g_record_cache.ready) return nullptr;

    // Trim per-frame arrays to r.frame_count entries.
    jlongArray intended = env->NewLongArray(r.frame_count);
    jlongArray actual = env->NewLongArray(r.frame_count);
    jintArray source = env->NewIntArray(r.frame_count);
    env->SetLongArrayRegion(intended, 0, r.frame_count, r.per_frame_intended_ns);
    env->SetLongArrayRegion(actual, 0, r.frame_count, r.per_frame_actual_ns);
    env->SetIntArrayRegion(source, 0, r.frame_count, r.per_frame_source);

    jstring nonce = env->NewStringUTF(r.fiducial_nonce_hex);
    jstring status = env->NewStringUTF(r.verification_status);

    jobject obj = env->NewObject(g_record_cache.cls, g_record_cache.ctor,
        jint(r.event_index),
        nonce,
        jdouble(r.target_duration_ms),
        jint(r.frame_count),
        jlong(r.frame_period_ns),
        jdouble(r.achieved_duration_ms),
        jlong(r.scheduled_frame_number),
        intended, actual, source,
        jlong(r.timing_deviation_ns),
        jdouble(r.refresh_hz_at_event),
        jfloat(r.brightness_at_event),
        status
    );

    env->DeleteLocalRef(intended);
    env->DeleteLocalRef(actual);
    env->DeleteLocalRef(source);
    env->DeleteLocalRef(nonce);
    env->DeleteLocalRef(status);
    return obj;
}

extern "C" {

JNIEXPORT jint JNICALL
Java_com_rsp_timing_NativeBridge_nativeInit(JNIEnv* env, jobject, jboolean useVulkan) {
    if (g_renderer != nullptr) {
        LOGE("Renderer already initialized");
        return -100;
    }
    g_renderer = useVulkan ? render::CreateVulkanRenderer() : render::CreateGlesRenderer();
    int rc = g_renderer->Init();
    if (rc != 0) {
        delete g_renderer;
        g_renderer = nullptr;
        return rc;
    }
    if (!g_renderer->PreflightOK()) {
        LOGE("Renderer preflight FAILED (extension missing?)");
        delete g_renderer;
        g_renderer = nullptr;
        return -101;
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_rsp_timing_NativeBridge_nativeSetSurface(JNIEnv* env, jobject, jobject surface) {
    if (!g_renderer) return;
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    g_renderer->SetSurface(window);
    // Note: ANativeWindow_fromSurface increments the ref count. The renderer owns it
    // from here until SetSurface(null) or Shutdown.
}

JNIEXPORT jdoubleArray JNICALL
Java_com_rsp_timing_NativeBridge_nativeProbeRefresh(JNIEnv* env, jobject, jint sampleFrames) {
    if (!g_renderer) {
        jdoubleArray a = env->NewDoubleArray(2);
        return a;
    }
    auto probe = g_renderer->ProbeRefresh(sampleFrames);
    jdoubleArray a = env->NewDoubleArray(2);
    double buf[2] = { probe.measured_hz, static_cast<double>(probe.jitter_p99_ns) };
    env->SetDoubleArrayRegion(a, 0, 2, buf);
    return a;
}

JNIEXPORT void JNICALL
Java_com_rsp_timing_NativeBridge_nativeConfigureSequence(JNIEnv* env, jobject,
    jdouble targetDurationMs, jint count, jint interEventFrames, jstring nonceHex)
{
    if (!g_renderer) return;
    SequenceConfig cfg{};
    cfg.target_duration_ms = targetDurationMs;
    cfg.count = count;
    cfg.inter_event_frames = interEventFrames;
    const char* nhex = env->GetStringUTFChars(nonceHex, nullptr);
    cfg.session_nonce = fiducial::ParseNonceHex(nhex);
    env->ReleaseStringUTFChars(nonceHex, nhex);
    cfg.tolerance_ms = 2.0;  // spec §5.2.2 default
    cfg.brightness_at_event = 0.5f;
    g_renderer->ConfigureSequence(cfg);
}

JNIEXPORT void JNICALL
Java_com_rsp_timing_NativeBridge_nativeStart(JNIEnv*, jobject) {
    if (g_renderer) g_renderer->Start();
}

JNIEXPORT void JNICALL
Java_com_rsp_timing_NativeBridge_nativeStop(JNIEnv*, jobject) {
    if (g_renderer) g_renderer->Stop();
}

JNIEXPORT jboolean JNICALL
Java_com_rsp_timing_NativeBridge_nativeIsRunning(JNIEnv*, jobject) {
    return g_renderer ? g_renderer->IsRunning() : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_rsp_timing_NativeBridge_nativeDrainCompletedEvents(JNIEnv* env, jobject) {
    if (!g_renderer) {
        EnsureRecordCache(env);
        return env->NewObjectArray(0, g_record_cache.cls, nullptr);
    }
    timing::StimulusEventRecord buf[64];
    int n = g_renderer->DrainEvents(buf, 64);
    EnsureRecordCache(env);
    jobjectArray arr = env->NewObjectArray(n, g_record_cache.cls, nullptr);
    for (int i = 0; i < n; ++i) {
        jobject obj = RecordToJava(env, buf[i]);
        env->SetObjectArrayElement(arr, i, obj);
        env->DeleteLocalRef(obj);
    }
    return arr;
}

JNIEXPORT void JNICALL
Java_com_rsp_timing_NativeBridge_nativeIngestPresentTimestamp(JNIEnv*, jobject,
    jlong frameNumber, jlong actualPresentNs, jint source)
{
    // Currently unused: Vulkan renderer pulls VkPastPresentationTimingGOOGLE
    // internally. Reserved for the GLES/NDK path where ANativeWindow_getFrameTimestamps
    // is called from Kotlin. Kept as a JNI declaration so we don't paint ourselves
    // into a corner.
    (void)frameNumber; (void)actualPresentNs; (void)source;
}

JNIEXPORT void JNICALL
Java_com_rsp_timing_NativeBridge_nativeShutdown(JNIEnv*, jobject) {
    if (g_renderer) {
        g_renderer->Shutdown();
        delete g_renderer;
        g_renderer = nullptr;
    }
}

}  // extern "C"
