// GlesRenderer.cpp — GLES3 + EGL fallback render path.
//
// Used when a device lacks VK_GOOGLE_display_timing but supports:
//   - EGL_ANDROID_presentation_time    (schedule)
//   - ANativeWindow_getFrameTimestamps (retrieve)
//
// Per Redline Patch 7: a device lacking BOTH the Vulkan and GLES paths is
// not clinical-tier eligible. This renderer is here so devices that have
// only one of the two can still qualify.
//
// GLSL is inlined here (rather than compiled to SPIR-V) because GLES doesn't
// consume SPIR-V; runtime glCompileShader is fine here — it happens once at
// SetSurface() before the render loop starts.

#include "RenderPath.h"
#include "../timing/FrameScheduler.h"
#include "../fiducial/Fiducial.h"

#include <android/log.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define TAG "GlesRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace rsp::render {

namespace {

constexpr int kSourceEglPt = 1;
constexpr int kSourceMissing = 3;
constexpr int kMinFutureFrames = 2;

static const char* kVertSrc = R"(#version 320 es
precision highp float;
out vec2 v_uv;
void main() {
    v_uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kFragSrc = R"(#version 320 es
precision highp float;
uniform highp uint  u_cells0;
uniform highp uint  u_cells1;
uniform highp uint  u_cells2;
uniform highp uint  u_cells3;
uniform highp float u_target;
uniform highp float u_ambient;
uniform highp float u_vp_w;
uniform highp float u_vp_h;
in vec2 v_uv;
out vec4 fragColor;
const float kCellPxW = 8.0;
const float kCellPxH = 32.0;
const float kOffL = 24.0;
const float kOffB = 24.0;
const int kCells = 104;
void main() {
    float px = v_uv.x * u_vp_w;
    float py_top = v_uv.y * u_vp_h;
    float py_bot = u_vp_h - py_top;
    float x0 = kOffL, x1 = kOffL + float(kCells) * kCellPxW;
    float y0 = kOffB, y1 = kOffB + kCellPxH;
    vec3 color = vec3(u_target);
    if (px >= x0 && px < x1 && py_bot >= y0 && py_bot < y1) {
        int ci = int(floor((px - x0) / kCellPxW));
        if (ci >= 0 && ci < kCells) {
            uint bit;
            if (ci < 32)      bit = (u_cells0 >> uint(ci)) & 1u;
            else if (ci < 64) bit = (u_cells1 >> uint(ci - 32)) & 1u;
            else if (ci < 96) bit = (u_cells2 >> uint(ci - 64)) & 1u;
            else              bit = (u_cells3 >> uint(ci - 96)) & 1u;
            color = (bit == 1u) ? vec3(1.0) : vec3(0.0);
        }
    }
    fragColor = vec4(color, 1.0);
}
)";

class GlesRendererImpl final : public RenderPath {
public:
    ~GlesRendererImpl() override { Shutdown(); }

    int Init() override {
        egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (egl_display_ == EGL_NO_DISPLAY) { LOGE("eglGetDisplay failed"); return -1; }
        EGLint major = 0, minor = 0;
        if (!eglInitialize(egl_display_, &major, &minor)) {
            LOGE("eglInitialize failed"); return -2;
        }
        // Probe extensions.
        const char* exts = eglQueryString(egl_display_, EGL_EXTENSIONS);
        if (exts) {
            has_pt_ext_ = strstr(exts, "EGL_ANDROID_presentation_time") != nullptr;
        }
        // Resolve extension function pointer explicitly (some EGL loaders don't link statically).
        egl_presentation_time_ = (PFNEGLPRESENTATIONTIMEANDROIDPROC)
            eglGetProcAddress("eglPresentationTimeANDROID");
        return 0;
    }

    bool PreflightOK() override {
        // Also need ANativeWindow_getFrameTimestamps at runtime; this is an NDK function,
        // not queryable from EGL. It's present on API 24+; our minSdk is 33 so it's guaranteed.
        return egl_display_ != EGL_NO_DISPLAY && has_pt_ext_ && egl_presentation_time_ != nullptr;
    }

    void SetSurface(ANativeWindow* window) override {
        if (window == nullptr) { DestroyEglResources(); window_ = nullptr; return; }
        window_ = window;
        if (!CreateContextAndSurface()) return;
        if (!CreateProgram()) return;
        // Enable frame timestamps on the underlying ANativeWindow so getFrameTimestamps returns real data.
        ANativeWindow_enableFrameTimestamps(window_, 1);
    }

    RefreshProbe ProbeRefresh(int sample_frames) override {
        RefreshProbe rp{};
        if (!egl_surface_ || !window_) return rp;

        // Render N frames as fast as vsync allows (eglSwapBuffers blocks on vsync in
        // FIFO mode), then read back per-frame DISPLAY_PRESENT_TIME via getFrameTimestamps.
        constexpr int kMaxProbe = 512;
        if (sample_frames > kMaxProbe) sample_frames = kMaxProbe;

        int64_t frame_ids[kMaxProbe];
        int actually_rendered = 0;
        for (int i = 0; i < sample_frames; ++i) {
            RenderAmbient(0);
            frame_ids[actually_rendered] = ANativeWindow_getNextFrameId(window_);
            // eglPresentationTimeANDROID accepts 0 = "as soon as possible".
            if (egl_presentation_time_) egl_presentation_time_(egl_display_, egl_surface_, 0);
            eglSwapBuffers(egl_display_, egl_surface_);
            actually_rendered++;
        }
        // Wait ~4 frames for the pipeline to drain.
        struct timespec ts{0, 60'000'000}; nanosleep(&ts, nullptr);

        int64_t present_times[kMaxProbe];
        int retrieved = 0;
        for (int i = 0; i < actually_rendered; ++i) {
            int64_t pt = 0;
            int64_t req = 0, acq = 0, latch = 0, first = 0, last = 0, gpu = 0, dq = 0, rel = 0;
            int32_t r = ANativeWindow_getFrameTimestamps(window_, frame_ids[i],
                &req, &acq, &latch, &first, &last, &gpu, &pt, &dq, &rel);
            if (r == 0 && pt > 0) present_times[retrieved++] = pt;
        }
        if (retrieved < 3) {
            LOGE("Refresh probe: too few frames retrieved (%d)", retrieved);
            return rp;
        }
        int64_t intervals[kMaxProbe]{};
        int in = 0;
        for (int i = 1; i < retrieved; ++i) {
            int64_t d = present_times[i] - present_times[i-1];
            if (d > 0) intervals[in++] = d;
        }
        // Sort for median + p99.
        for (int i = 1; i < in; ++i) {
            int64_t v = intervals[i]; int j = i - 1;
            while (j >= 0 && intervals[j] > v) { intervals[j+1] = intervals[j]; j--; }
            intervals[j+1] = v;
        }
        int64_t median = intervals[in / 2];
        int p99i = (in * 99) / 100; if (p99i >= in) p99i = in - 1;
        rp.frame_period_ns_median = median;
        rp.measured_hz = 1e9 / static_cast<double>(median);
        rp.jitter_p99_ns = std::llabs(intervals[p99i] - median);
        frame_period_ns_ = rp.frame_period_ns_median;
        LOGI("GLES refresh probe: %.3f Hz period=%lld ns jitter_p99=%lld ns (n=%d)",
             rp.measured_hz, (long long)median, (long long)rp.jitter_p99_ns, in);
        return rp;
    }

    void ConfigureSequence(const SequenceConfig& cfg) override {
        cfg_ = cfg;
        plan_ = timing::FrameScheduler::Plan(cfg.target_duration_ms, frame_period_ns_, cfg.tolerance_ms);
    }

    void Start() override {
        if (running_.exchange(true)) return;
        stop_requested_.store(false);
        pthread_create(&thr_, nullptr, &ThreadStatic, this);
    }

    void Stop() override { stop_requested_.store(true); }
    bool IsRunning() override { return running_.load(); }

    int DrainEvents(timing::StimulusEventRecord* out, int max) override {
        return log_.Drain(out, max);
    }

    void Shutdown() override {
        if (running_.load()) { Stop(); pthread_join(thr_, nullptr); }
        DestroyEglResources();
        if (egl_display_ != EGL_NO_DISPLAY) {
            eglTerminate(egl_display_);
            egl_display_ = EGL_NO_DISPLAY;
        }
    }

private:
    bool CreateContextAndSurface() {
        // GLES3 + 8/8/8/8 + no depth
        EGLint cfg_attrs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
            EGL_NONE
        };
        EGLConfig cfg;
        EGLint n_cfg = 0;
        if (!eglChooseConfig(egl_display_, cfg_attrs, &cfg, 1, &n_cfg) || n_cfg == 0) {
            LOGE("eglChooseConfig failed");
            return false;
        }
        EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        egl_context_ = eglCreateContext(egl_display_, cfg, EGL_NO_CONTEXT, ctx_attrs);
        if (egl_context_ == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return false; }
        egl_surface_ = eglCreateWindowSurface(egl_display_, cfg, (EGLNativeWindowType)window_, nullptr);
        if (egl_surface_ == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); return false; }
        if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
            LOGE("eglMakeCurrent failed"); return false;
        }
        // Query surface size for the fragment shader viewport uniform.
        EGLint w = 0, h = 0;
        eglQuerySurface(egl_display_, egl_surface_, EGL_WIDTH,  &w);
        eglQuerySurface(egl_display_, egl_surface_, EGL_HEIGHT, &h);
        vp_w_ = static_cast<float>(w);
        vp_h_ = static_cast<float>(h);
        return true;
    }

    static GLuint Compile(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            GLsizei ln = 0;
            glGetShaderInfoLog(s, sizeof(log), &ln, log);
            LOGE("Shader compile failed: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    bool CreateProgram() {
        GLuint v = Compile(GL_VERTEX_SHADER, kVertSrc);
        GLuint f = Compile(GL_FRAGMENT_SHADER, kFragSrc);
        if (!v || !f) return false;
        program_ = glCreateProgram();
        glAttachShader(program_, v);
        glAttachShader(program_, f);
        glLinkProgram(program_);
        GLint ok = 0;
        glGetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024]; GLsizei ln = 0;
            glGetProgramInfoLog(program_, sizeof(log), &ln, log);
            LOGE("Program link failed: %s", log);
            return false;
        }
        glDeleteShader(v); glDeleteShader(f);
        u_cells0_ = glGetUniformLocation(program_, "u_cells0");
        u_cells1_ = glGetUniformLocation(program_, "u_cells1");
        u_cells2_ = glGetUniformLocation(program_, "u_cells2");
        u_cells3_ = glGetUniformLocation(program_, "u_cells3");
        u_target_ = glGetUniformLocation(program_, "u_target");
        u_ambient_ = glGetUniformLocation(program_, "u_ambient");
        u_vp_w_ = glGetUniformLocation(program_, "u_vp_w");
        u_vp_h_ = glGetUniformLocation(program_, "u_vp_h");
        // A dummy VAO is required in core profile.
        glGenVertexArrays(1, &vao_);
        return true;
    }

    void DestroyEglResources() {
        if (egl_display_ == EGL_NO_DISPLAY) return;
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
        if (program_) { glDeleteProgram(program_); program_ = 0; }
        if (egl_context_ != EGL_NO_CONTEXT) { eglDestroyContext(egl_display_, egl_context_); egl_context_ = EGL_NO_CONTEXT; }
        if (egl_surface_ != EGL_NO_SURFACE) { eglDestroySurface(egl_display_, egl_surface_); egl_surface_ = EGL_NO_SURFACE; }
    }

    // ---------------- Render thread ----------------

    static void* ThreadStatic(void* self) {
        static_cast<GlesRendererImpl*>(self)->ThreadMain();
        return nullptr;
    }

    void ThreadMain() {
        setpriority(PRIO_PROCESS, static_cast<int>(syscall(SYS_gettid)), -19);
        // NB: the EGL context was made current on the ORIGINAL calling thread by
        // CreateContextAndSurface. GLES contexts are per-thread. We must
        // re-make-current on this render thread.
        if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
            LOGE("Render-thread eglMakeCurrent failed");
            running_.store(false);
            return;
        }

        uint32_t global_frame_counter = 0;
        int64_t now = NowNanos();
        int64_t next_present_ns = now + int64_t(kMinFutureFrames) * frame_period_ns_;
        int64_t next_present_id = kMinFutureFrames;

        for (int ev = 0; ev < cfg_.count && !stop_requested_.load(); ++ev) {
            for (int i = 0; i < cfg_.inter_event_frames && !stop_requested_.load(); ++i) {
                PresentOneFrame(FrameKind::kAmbient, global_frame_counter, next_present_id, next_present_ns);
                global_frame_counter++;
                next_present_id++;
                next_present_ns += frame_period_ns_;
            }
            if (stop_requested_.load()) break;

            timing::StimulusEventRecord& b = log_.Building();
            b = timing::StimulusEventRecord{};
            b.event_index = ev;
            b.target_duration_ms = cfg_.target_duration_ms;
            b.frame_count = plan_.frame_count;
            b.frame_period_ns = plan_.frame_period_ns;
            b.achieved_duration_ms = plan_.achieved_duration_ms;
            b.timing_deviation_ns = static_cast<int64_t>(plan_.deviation_from_target_ms * 1'000'000.0);
            b.refresh_hz_at_event = 1e9 / static_cast<double>(frame_period_ns_);
            b.brightness_at_event = cfg_.brightness_at_event;
            b.scheduled_frame_number = next_present_id;
            for (int i = 0; i < plan_.frame_count; ++i) b.per_frame_source[i] = kSourceMissing;
            EncodeFiducialNonce(b.fiducial_nonce_hex, MakeEventNonce(ev));

            // Remember frame IDs used for the target frames — retrieval binds to these.
            int64_t target_frame_ids[16] = {0};
            for (int i = 0; i < plan_.frame_count && !stop_requested_.load(); ++i) {
                b.per_frame_intended_ns[i] = next_present_ns;
                target_frame_ids[i] = PresentOneFrame(FrameKind::kTarget, global_frame_counter,
                                                     next_present_id, next_present_ns);
                global_frame_counter++;
                next_present_id++;
                next_present_ns += frame_period_ns_;
            }
            if (stop_requested_.load()) break;

            // Pad frames for retrieval latency.
            for (int i = 0; i < 4 && !stop_requested_.load(); ++i) {
                PresentOneFrame(FrameKind::kAmbient, global_frame_counter, next_present_id, next_present_ns);
                global_frame_counter++;
                next_present_id++;
                next_present_ns += frame_period_ns_;
            }

            // Retrieve DISPLAY_PRESENT_TIME per target frame ID.
            for (int i = 0; i < plan_.frame_count; ++i) {
                int64_t pt = 0, req = 0, acq = 0, latch = 0, first = 0, last = 0, gpu = 0, dq = 0, rel = 0;
                int32_t r = ANativeWindow_getFrameTimestamps(window_, target_frame_ids[i],
                    &req, &acq, &latch, &first, &last, &gpu, &pt, &dq, &rel);
                if (r == 0 && pt > 0) {
                    b.per_frame_actual_ns[i] = pt;
                    b.per_frame_source[i] = kSourceEglPt;
                }
            }
            FinalizeEventStatus(b);
            log_.PushCompletedEvent(b);
        }
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        running_.store(false);
    }

    enum class FrameKind { kAmbient, kTarget };

    // Returns the frame ID used for this present, so callers can retrieve its timestamp later.
    int64_t PresentOneFrame(FrameKind kind, uint32_t counter, int64_t present_id, int64_t desired_present_ns) {
        // Fiducial cells for this frame
        uint8_t cells[fiducial::kCells];
        fiducial::FrameType ft = (kind == FrameKind::kTarget) ? fiducial::FRAME_TARGET : fiducial::FRAME_AMBIENT;
        fiducial::ComputeCells(cfg_.session_nonce, counter, ft, cells);
        uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        for (int i = 0; i < fiducial::kCells; ++i) {
            uint32_t bit = cells[i] & 1u;
            if      (i < 32) c0 |= (bit << i);
            else if (i < 64) c1 |= (bit << (i - 32));
            else if (i < 96) c2 |= (bit << (i - 64));
            else             c3 |= (bit << (i - 96));
        }
        glViewport(0, 0, (GLsizei)vp_w_, (GLsizei)vp_h_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program_);
        glUniform1ui(u_cells0_, c0);
        glUniform1ui(u_cells1_, c1);
        glUniform1ui(u_cells2_, c2);
        glUniform1ui(u_cells3_, c3);
        glUniform1f(u_target_,  (kind == FrameKind::kTarget) ? 0.7f : 0.05f);
        glUniform1f(u_ambient_, 0.05f);
        glUniform1f(u_vp_w_, vp_w_);
        glUniform1f(u_vp_h_, vp_h_);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // Get the frame ID BEFORE swap so we can correlate the getFrameTimestamps result.
        int64_t frame_id = ANativeWindow_getNextFrameId(window_);
        // Schedule the frame with the desired presentation time.
        if (egl_presentation_time_) {
            egl_presentation_time_(egl_display_, egl_surface_, (EGLnsecsANDROID)desired_present_ns);
        }
        eglSwapBuffers(egl_display_, egl_surface_);
        (void)present_id;  // GLES path uses ANativeWindow frame ID, not the numeric present_id
        return frame_id;
    }

    void FinalizeEventStatus(timing::StimulusEventRecord& b) {
        bool any_missing = false;
        int64_t worst_dev = 0;
        for (int i = 0; i < b.frame_count; ++i) {
            if (b.per_frame_source[i] == kSourceMissing) { any_missing = true; continue; }
            int64_t dev = b.per_frame_actual_ns[i] - b.per_frame_intended_ns[i];
            if (std::llabs(dev) > std::llabs(worst_dev)) worst_dev = dev;
        }
        if (any_missing) { strncpy(b.verification_status, "missing_pt", sizeof(b.verification_status)); return; }
        int64_t tol_ns = static_cast<int64_t>(cfg_.tolerance_ms * 1'000'000.0);
        if (std::llabs(worst_dev) > tol_ns || std::llabs(b.timing_deviation_ns) > tol_ns) {
            strncpy(b.verification_status, "failed", sizeof(b.verification_status));
        } else if (std::llabs(worst_dev) > tol_ns / 2) {
            strncpy(b.verification_status, "deviation", sizeof(b.verification_status));
        } else {
            strncpy(b.verification_status, "verified", sizeof(b.verification_status));
        }
    }

    void RenderAmbient(uint32_t counter) {
        // Draw one ambient frame WITHOUT swapping — used only inside ProbeRefresh where we
        // want a tight schedule-swap-schedule-swap loop.
        (void)counter;
        glViewport(0, 0, (GLsizei)vp_w_, (GLsizei)vp_h_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // Not drawing the fiducial in the probe — a raw clear is enough for the compositor
        // to schedule vsync-aligned present events.
    }

    static int64_t NowNanos() {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

    uint64_t MakeEventNonce(int idx) {
        return cfg_.session_nonce ^ (uint64_t(idx) * 0x9E3779B97F4A7C15ULL);
    }

    void EncodeFiducialNonce(char* out, uint64_t nonce) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 16; ++i) out[15 - i] = hex[(nonce >> (i * 4)) & 0xF];
        out[16] = 0;
    }

    // ---- State ----
    ANativeWindow* window_ = nullptr;
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    bool has_pt_ext_ = false;
    PFNEGLPRESENTATIONTIMEANDROIDPROC egl_presentation_time_ = nullptr;

    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLint u_cells0_ = -1, u_cells1_ = -1, u_cells2_ = -1, u_cells3_ = -1;
    GLint u_target_ = -1, u_ambient_ = -1, u_vp_w_ = -1, u_vp_h_ = -1;
    float vp_w_ = 0.0f, vp_h_ = 0.0f;

    int64_t frame_period_ns_ = 8'333'333;
    SequenceConfig cfg_{};
    timing::FramePlan plan_{};
    timing::TimingLog log_;

    pthread_t thr_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};

}  // namespace

RenderPath* CreateGlesRenderer() {
    return new GlesRendererImpl();
}

}  // namespace rsp::render
