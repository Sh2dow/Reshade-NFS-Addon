extern "C" __declspec(dllexport) const char* NAME = "NFSTweakBridge";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "NFS depth/texture bridge + UI";

// ---------- Globals ----------
std::mutex g_push_mutex;
static std::atomic_bool g_pending_depth(false);
static std::atomic_bool g_show_bridge_menu(false);
static std::atomic_bool g_request_pre_hud_effects(false);
static std::atomic_bool g_scene_window_open(false);
static std::atomic_uint32_t g_scene_window_token(0);
static std::atomic_uint32_t g_scene_window_epoch(0);
static std::atomic_uint32_t g_scene_window_rendered_token(0);
static std::atomic_bool g_scene_window_close_pending(false);
static std::atomic_uint32_t g_scene_window_close_token(0);
static std::atomic_uint64_t g_scene_window_close_frame(0);
static std::atomic_bool g_running_manual_effects(false);
static std::atomic_bool g_pre_hud_effects_issued_this_frame(false);
static std::atomic_bool g_auto_pre_hud_effects(true);
static std::atomic_uint32_t g_prehud_request_count(0);
static std::atomic_uint64_t g_frame_index(0);
static std::atomic_uint64_t g_request_pre_hud_frame(0);
static std::atomic_uint64_t g_request_pre_hud_beginpass(0);
static std::atomic_uint32_t g_request_pre_hud_epoch(0);
static std::atomic_uint64_t g_last_bridge_request_frame(0);
static std::atomic_uint64_t g_beginpass_counter(0);
static std::atomic_uint64_t g_frame_beginpass_start(0);
static std::atomic_uint64_t g_clear_counter(0);
static std::atomic_uint64_t g_render_counter(0);
static std::atomic_uint64_t g_last_manual_render_beginpass(0);
static std::atomic_bool g_defer_first_qualifying_pass_after_request(true);
static std::atomic_bool g_disable_beginpass_after_fault(false);
static std::atomic_bool g_suppress_regular_post_hud_pass(true);
static std::atomic_bool g_enable_vulkan_beginpass_prehud(true);
static std::atomic_uint64_t g_bind_rt_ds_event_count(0);
static std::atomic_bool g_enable_manual_prehud_render(true);
static std::atomic_int g_skip_manual_prehud_frames(0);
enum class prehud_runtime_state : int { disabled = 0, stabilizing = 1, armed = 2, active = 3 };
static std::atomic_int g_prehud_runtime_state(static_cast<int>(prehud_runtime_state::disabled));
static std::atomic_int g_transition_settle_frames(0);
static resource g_last_scene_rt_signature = { 0 };
static resource g_last_scene_ds_signature = { 0 };
static int g_scene_signature_streak = 0;
static resource g_active_scene_ds_signature = { 0 };
static constexpr int k_prehud_streak_required = 3;
static resource g_prehud_locked_rt_resource = { 0 };
static resource g_prehud_locked_ds_resource = { 0 };
static resource g_prehud_soft_rt_resource = { 0 };
static resource g_prehud_soft_ds_resource = { 0 };
static std::atomic_uint64_t g_prehud_soft_lock_expire_frame(0);
static std::atomic_uint64_t g_prehud_lock_last_hit_frame(0);
static std::atomic_uint64_t g_prehud_lock_miss_frames(0);
static std::atomic_uint64_t g_prehud_lock_freeze_until_frame(0);
static std::atomic_bool g_seen_reload_settle(false);
static std::atomic_uint64_t g_manual_render_ready_frame(0);
static std::atomic_uint64_t g_last_reload_event_frame(0);
static std::atomic_uint64_t g_last_manual_prehud_frame(0);
static std::atomic_uint64_t g_manual_render_latch_frame(0);
static std::atomic_bool g_manual_prehud_primed(false);
static std::atomic_bool g_block_current_reshade_effects_pass(false);
static std::atomic_bool g_allow_posthud_fallback_once(false);
static std::atomic_uint32_t g_diag_nonmanual_begin_this_frame(0);
static std::atomic_uint32_t g_diag_nonmanual_blocked_this_frame(0);
static std::atomic_uint64_t g_diag_last_prehud_rtv(0);
static std::atomic_uint64_t g_diag_last_prehud_dsv(0);
static std::atomic_uintptr_t g_manual_effects_cmdlist(0);
static std::atomic_uint64_t g_manual_effects_frame(0);
static std::atomic_int g_manual_effects_budget(0);
static std::atomic_int g_prehud_bp_bucket(-1);
static std::atomic_int g_prehud_bp_tolerance(2);
static std::atomic_int g_prehud_bp_bucket_miss(0);
static std::atomic_uint64_t g_null_rtv_burst_last_frame(0);
static std::atomic_int g_null_rtv_burst_count(0);
static std::atomic_uint64_t g_manual_prehud_cooldown_until_frame(0);

struct prehud_trace_entry
{
    uint64_t frame;
    uint64_t bp;
    uint64_t rtv;
    uint64_t dsv;
    uint32_t score;
    uint32_t token;
    uint32_t reason;
    uint32_t kind; // 1=render, 2=skip, 3=transition
};
static constexpr uint32_t k_prehud_trace_capacity = 512;
static prehud_trace_entry g_prehud_trace[k_prehud_trace_capacity] = {};
static std::atomic_uint32_t g_prehud_trace_write(0);

static unsigned int g_last_width = 0, g_last_height = 0;

// ReShade resource handles
static resource g_custom_depth = { 0 };        // target ReShade resource holding depth (r32_float)
static resource_view g_custom_depth_view = { 0 }; // SRV (linear)
// Do not create a separate sRGB view for depth (r32_float has no sRGB variant). Bind the same SRV for both slots.
static resource_view g_custom_depth_view_srgb = { 0 }; // unused, kept for call sites; always zero
static device *g_device = nullptr;
static effect_runtime *g_runtime = nullptr;
static std::atomic_bool g_runtime_alive(false);
static device_api g_device_api = device_api::d3d9;
static bool g_enabled_for_runtime = true;
static uint32_t g_width = 0, g_height = 0;
static std::atomic_bool g_enable_depth_processing(true);
static uint64_t g_last_process_qpc = 0;
static std::atomic_bool g_precip_signal_pending(false);
static std::atomic_uint32_t g_precip_signal_value(0);
static std::atomic_uint32_t g_last_precip_signal_value(0xFFFFFFFFu);
static std::atomic_uint64_t g_last_precip_signal_frame(0);
static std::atomic_bool g_phase_invalidate_pending(false);
static std::atomic_uint32_t g_phase_invalidate_reason(0);
static std::atomic_uint32_t g_phase_invalidate_epoch(0);
static std::atomic_uint32_t g_phase_epoch(1);
static std::atomic_bool g_require_exact_backbuffer_lock(false);

// D3D9 surface passed from ASI (we AddRef() it in push and release after processing)
static IDirect3DSurface9* g_last_depth_surface = nullptr;

// CPU depth buffer path (DXVK-safe):
// Producer provides linear depth as R32 float (one float per pixel).
static std::vector<float> g_last_depth_cpu;
static uint32_t g_last_depth_cpu_row_pitch_floats = 0; // in floats (not bytes)
static bool g_has_depth_cpu = false;

// Vulkan/DXVK path: try to bind the runtime depth-stencil resource directly as a shader resource.
static resource_view g_runtime_depth_srv = { 0 };
static resource g_runtime_depth_resource = { 0 };
static uint64_t g_last_vulkan_bind_qpc = 0;
static std::atomic_bool g_enable_vulkan_depth_bind(false);
static std::atomic_bool g_mirror_customdepth_debug(false);
static resource_view g_vulkan_depth_candidate_dsv = { 0 };
static resource g_vulkan_depth_candidate_res = { 0 };
static uint32_t g_vulkan_depth_candidate_w = 0;
static uint32_t g_vulkan_depth_candidate_h = 0;
static uint32_t g_vulkan_depth_candidate_samples = 1;
static format g_vulkan_depth_candidate_format = format::unknown;
static uint32_t g_vulkan_depth_candidate_score = 0;
// Prefer deterministic scene pass selection, but allow scored fallback on engines that never bind backbuffer here.
static std::atomic_bool g_require_vulkan_backbuffer_rt(false);
static std::atomic_bool g_lock_vulkan_depth(false);
static uint32_t g_vulkan_depth_last_score = 0;

// Optional resolved depth path for MSAA depth buffers.
static resource g_vulkan_depth_resolved = { 0 };
static resource_view g_vulkan_depth_resolved_srv = { 0 };
static uint32_t g_vulkan_depth_resolved_w = 0;
static uint32_t g_vulkan_depth_resolved_h = 0;
static format g_vulkan_depth_resolved_format = format::unknown;
static std::atomic_bool g_enable_vulkan_msaa_resolve(false);
static std::atomic_bool g_require_vulkan_backbuffer_match(false);

// Forward decl
static void ProcessPendingDepth();
static void try_bind_vulkan_depth(resource_view dsv, uint32_t score_hint);
static void bind_vulkan_candidate_if_good();
static void reset_prehud_transition(const char *reason, int settle_frames, bool clear_lock = true)
{
    g_transition_settle_frames.store(settle_frames, std::memory_order_relaxed);
    g_prehud_runtime_state.store(static_cast<int>(prehud_runtime_state::stabilizing), std::memory_order_relaxed);
    g_skip_manual_prehud_frames.store(std::max(g_skip_manual_prehud_frames.load(std::memory_order_relaxed), 8));
    g_running_manual_effects.store(false, std::memory_order_relaxed);
    g_manual_effects_budget.store(0, std::memory_order_relaxed);
    g_manual_effects_cmdlist.store(0, std::memory_order_relaxed);
    g_manual_effects_frame.store(0, std::memory_order_relaxed);
    g_prehud_bp_bucket.store(-1, std::memory_order_relaxed);
    g_prehud_bp_bucket_miss.store(0, std::memory_order_relaxed);
    g_null_rtv_burst_last_frame.store(0, std::memory_order_relaxed);
    g_null_rtv_burst_count.store(0, std::memory_order_relaxed);
    g_manual_prehud_cooldown_until_frame.store(0, std::memory_order_relaxed);
    g_block_current_reshade_effects_pass.store(false, std::memory_order_relaxed);
    g_request_pre_hud_effects.store(false, std::memory_order_relaxed);
    g_scene_window_open.store(false, std::memory_order_relaxed);
    g_scene_window_token.store(0, std::memory_order_relaxed);
    g_scene_window_epoch.store(0, std::memory_order_relaxed);
    g_scene_window_rendered_token.store(0, std::memory_order_relaxed);
    g_scene_window_close_pending.store(false, std::memory_order_relaxed);
    g_scene_window_close_token.store(0, std::memory_order_relaxed);
    g_scene_window_close_frame.store(0, std::memory_order_relaxed);
    g_request_pre_hud_beginpass.store(0, std::memory_order_relaxed);
    g_request_pre_hud_epoch.store(0, std::memory_order_relaxed);
    g_pre_hud_effects_issued_this_frame.store(false, std::memory_order_relaxed);
    g_defer_first_qualifying_pass_after_request.store(true, std::memory_order_relaxed);
    g_prehud_bp_bucket.store(-1, std::memory_order_relaxed);
    g_prehud_bp_bucket_miss.store(0, std::memory_order_relaxed);
    // Re-open depth candidate selection after explicit phase resets.
    g_lock_vulkan_depth.store(false, std::memory_order_relaxed);
    if (clear_lock)
    {
        g_prehud_locked_rt_resource = { 0 };
        g_prehud_locked_ds_resource = { 0 };
        g_prehud_soft_rt_resource = { 0 };
        g_prehud_soft_ds_resource = { 0 };
        g_prehud_soft_lock_expire_frame.store(0, std::memory_order_relaxed);
        g_prehud_lock_last_hit_frame.store(0, std::memory_order_relaxed);
        g_prehud_lock_miss_frames.store(0, std::memory_order_relaxed);
        g_prehud_lock_freeze_until_frame.store(0, std::memory_order_relaxed);
        g_last_scene_rt_signature = { 0 };
        g_last_scene_ds_signature = { 0 };
        g_scene_signature_streak = 0;
        g_active_scene_ds_signature = { 0 };
    }
    g_manual_prehud_primed.store(false, std::memory_order_relaxed);
    g_last_manual_prehud_frame.store(0, std::memory_order_relaxed);
    g_manual_render_latch_frame.store(0, std::memory_order_relaxed);
    const uint64_t frame = g_frame_index.load(std::memory_order_relaxed);
    // Do not hard-delay manual rendering by 180 frames; that causes long startup skip storms (reason=0x400).
    // Keep a short guard tied to settle duration so rendering starts promptly once stabilized.
    const uint64_t ready_delay = static_cast<uint64_t>(std::max(4, settle_frames + 2));
    g_manual_render_ready_frame.store(frame + ready_delay, std::memory_order_relaxed);
    // Allow fallback bootstrap after transition reset; strict exact lock can starve pre-HUD on DXVK phases.
    g_require_exact_backbuffer_lock.store(false, std::memory_order_relaxed);
    if (reason != nullptr)
        log_info(reason);
}

extern "C" __declspec(dllexport)
void NFSTweak_RequestPreHudEffects()
{
    if (!g_runtime_alive.load(std::memory_order_relaxed))
        return;

    const prehud_runtime_state state = static_cast<prehud_runtime_state>(g_prehud_runtime_state.load(std::memory_order_relaxed));
    if (state == prehud_runtime_state::disabled || state == prehud_runtime_state::stabilizing)
        return;
    const uint64_t bp_now = g_beginpass_counter.load(std::memory_order_relaxed);
    const uint64_t bp_frame_start = g_frame_beginpass_start.load(std::memory_order_relaxed);
    // Ignore late requests in current frame (typically blur/HUD tail on same RT).
    if (bp_now > bp_frame_start && (bp_now - bp_frame_start) > 128)
        return;

    const uint64_t frame = g_frame_index.load(std::memory_order_relaxed);
    if (g_request_pre_hud_effects.load(std::memory_order_relaxed) &&
        g_request_pre_hud_frame.load(std::memory_order_relaxed) == frame)
    {
        // Keep earliest in-frame request anchor; ignore duplicate later requests.
        return;
    }
    g_request_pre_hud_frame.store(frame, std::memory_order_relaxed);
    g_request_pre_hud_beginpass.store(bp_now, std::memory_order_relaxed);
    g_request_pre_hud_epoch.store(g_phase_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_request_pre_hud_effects.store(true);
    g_defer_first_qualifying_pass_after_request.store(true, std::memory_order_relaxed);
}

extern "C" __declspec(dllexport)
void NFSTweak_BeginPreHudWindowEx(unsigned int token, unsigned int epoch)
{
    if (!g_runtime_alive.load(std::memory_order_relaxed) || token == 0)
        return;
    if (epoch != 0 && epoch != g_phase_epoch.load(std::memory_order_relaxed))
        return;

    const uint64_t frame = g_frame_index.load(std::memory_order_relaxed);
    // FE overlays (e.g. music player) can emit multiple begin-window signals in one frame.
    // Keep the first request anchor in-frame, but always refresh token/window so it never goes stale.
    if (g_pre_hud_effects_issued_this_frame.load(std::memory_order_relaxed))
        return;

    g_scene_window_token.store(token, std::memory_order_relaxed);
    g_scene_window_open.store(true, std::memory_order_relaxed);
    g_scene_window_epoch.store(g_phase_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);

    const uint64_t bp_now = g_beginpass_counter.load(std::memory_order_relaxed);
    g_prehud_request_count.fetch_add(1, std::memory_order_relaxed);
    g_last_bridge_request_frame.store(frame, std::memory_order_relaxed);
    if (g_request_pre_hud_effects.load(std::memory_order_relaxed) &&
        g_request_pre_hud_frame.load(std::memory_order_relaxed) == frame)
        return;
    g_request_pre_hud_frame.store(frame, std::memory_order_relaxed);
    g_request_pre_hud_beginpass.store(bp_now, std::memory_order_relaxed);
    g_request_pre_hud_epoch.store(g_phase_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_request_pre_hud_effects.store(true, std::memory_order_relaxed);
    g_defer_first_qualifying_pass_after_request.store(true, std::memory_order_relaxed);
}

extern "C" __declspec(dllexport)
void NFSTweak_BeginPreHudWindow(unsigned int token)
{
    NFSTweak_BeginPreHudWindowEx(token, g_phase_epoch.load(std::memory_order_relaxed));
}

extern "C" __declspec(dllexport)
void NFSTweak_EndPreHudWindowEx(unsigned int token, unsigned int epoch)
{
    if (!g_runtime_alive.load(std::memory_order_relaxed) || token == 0)
        return;
    if (epoch != 0 && epoch != g_phase_epoch.load(std::memory_order_relaxed))
        return;

    if (g_scene_window_token.load(std::memory_order_relaxed) == token)
    {
        // Keep token window open until consumed by a successful pre-HUD render
        // (or replaced by the next Begin token). Immediate close can starve pre-HUD
        // on runtimes where the qualifying pass arrives later than bridge hook timing.
        g_scene_window_close_pending.store(false, std::memory_order_relaxed);
        g_scene_window_close_token.store(0, std::memory_order_relaxed);
        g_scene_window_close_frame.store(0, std::memory_order_relaxed);
    }
}

extern "C" __declspec(dllexport)
void NFSTweak_EndPreHudWindow(unsigned int token)
{
    NFSTweak_EndPreHudWindowEx(token, g_phase_epoch.load(std::memory_order_relaxed));
}

extern "C" __declspec(dllexport)
void NFSTweak_NotifyPrecipitationChanged(unsigned int value)
{
    if (!g_runtime_alive.load(std::memory_order_relaxed))
        return;

    g_precip_signal_value.store(value, std::memory_order_relaxed);
    g_precip_signal_pending.store(true, std::memory_order_relaxed);
}

extern "C" __declspec(dllexport)
void NFSTweak_NotifyPhaseInvalidate(unsigned int reason)
{
    if (!g_runtime_alive.load(std::memory_order_relaxed))
        return;

    g_phase_invalidate_reason.store(reason, std::memory_order_relaxed);
    g_phase_invalidate_epoch.store(0, std::memory_order_relaxed);
    g_phase_invalidate_pending.store(true, std::memory_order_relaxed);
}

extern "C" __declspec(dllexport)
void NFSTweak_NotifyPhaseInvalidateEx(unsigned int reason, unsigned int epoch)
{
    if (!g_runtime_alive.load(std::memory_order_relaxed))
        return;

    g_phase_invalidate_reason.store(reason, std::memory_order_relaxed);
    g_phase_invalidate_epoch.store(epoch, std::memory_order_relaxed);
    g_phase_invalidate_pending.store(true, std::memory_order_relaxed);
}

extern "C" __declspec(dllexport)
void NFSTweak_RenderEffectsPreHudNow()
{
    if (!g_runtime_alive.load(std::memory_order_relaxed))
        return;

    const prehud_runtime_state state = static_cast<prehud_runtime_state>(g_prehud_runtime_state.load(std::memory_order_relaxed));
    if (state == prehud_runtime_state::disabled || state == prehud_runtime_state::stabilizing)
        return;
    const uint64_t bp_now = g_beginpass_counter.load(std::memory_order_relaxed);
    const uint64_t bp_frame_start = g_frame_beginpass_start.load(std::memory_order_relaxed);
    // Ignore late requests in current frame (typically blur/HUD tail on same RT).
    if (bp_now > bp_frame_start && (bp_now - bp_frame_start) > 128)
        return;

    // Safe deterministic signal from bridge hook (IDA-validated FE boundary).
    // Actual render happens in the bind callback where command context is valid.
    const uint64_t frame = g_frame_index.load(std::memory_order_relaxed);
    if (g_request_pre_hud_effects.load(std::memory_order_relaxed) &&
        g_request_pre_hud_frame.load(std::memory_order_relaxed) == frame)
    {
        // Keep earliest in-frame request anchor; ignore duplicate later requests.
        return;
    }
    g_prehud_request_count.fetch_add(1);
    g_last_bridge_request_frame.store(frame, std::memory_order_relaxed);
    g_request_pre_hud_frame.store(frame, std::memory_order_relaxed);
    g_request_pre_hud_beginpass.store(bp_now, std::memory_order_relaxed);
    g_request_pre_hud_epoch.store(g_phase_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_request_pre_hud_effects.store(true);
    g_defer_first_qualifying_pass_after_request.store(true, std::memory_order_relaxed);
}

// Vulkan/DXVK: called whenever application binds render targets + depth (lets us discover the active depth buffer).
