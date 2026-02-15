// nfs_tweak_addon.cpp
// Modern ReShade add-on: NFSTweak bridge (fixed API usage + exported push function)
//
// Build notes:
//  - Link against ReShade addon headers/libs and D3D9 (for IDirect3DSurface9 usage).
//  - This file intentionally leaves GPU-copy details as TODOs — see comments below.

#include <reshade.hpp>
#include <reshade_api.hpp>
#include <reshade_events.hpp>
#include <assert.h>
#include <d3d9.h>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include "imgui.h"

using namespace reshade::api;

static void log_info(const char *msg)
{
    // Write to ReShade.log (and also to debugger output, if attached).
    reshade::log::message(reshade::log::level::info, msg);
    OutputDebugStringA(msg);
}

// exported metadata
extern "C" __declspec(dllexport) const char* NAME = "NFSTweakBridge";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "NFS depth/texture bridge + UI";

// ---------- Globals ----------
std::mutex g_push_mutex;
static std::atomic_bool g_pending_depth(false);
static std::atomic_bool g_request_pre_hud_effects(false);
static std::atomic_bool g_running_manual_effects(false);
static std::atomic_bool g_pre_hud_effects_issued_this_frame(false);
static std::atomic_bool g_auto_pre_hud_effects(true);
static std::atomic_uint32_t g_prehud_request_count(0);
static std::atomic_uint64_t g_frame_index(0);
static std::atomic_uint64_t g_request_pre_hud_frame(0);
static std::atomic_uint64_t g_last_bridge_request_frame(0);
static std::atomic_uint64_t g_beginpass_counter(0);
static std::atomic_uint64_t g_clear_counter(0);
static std::atomic_uint64_t g_render_counter(0);
static std::atomic_bool g_defer_first_qualifying_pass_after_request(true);
static std::atomic_bool g_disable_beginpass_after_fault(false);
static std::atomic_bool g_enable_vulkan_beginpass_prehud(true);
static std::atomic_int g_skip_manual_prehud_frames(0);
static resource g_prehud_locked_rt_resource = { 0 };
static resource g_prehud_locked_ds_resource = { 0 };
static std::atomic_uint64_t g_prehud_lock_last_hit_frame(0);
static std::atomic_uint64_t g_prehud_lock_miss_frames(0);
static std::atomic_bool g_seen_reload_settle(false);

static unsigned int g_last_width = 0, g_last_height = 0;

// ReShade resource handles
static resource g_custom_depth = { 0 };        // target ReShade resource holding depth (r32_float)
static resource_view g_custom_depth_view = { 0 }; // SRV (linear)
// Do not create a separate sRGB view for depth (r32_float has no sRGB variant). Bind the same SRV for both slots.
static resource_view g_custom_depth_view_srgb = { 0 }; // unused, kept for call sites; always zero
static device *g_device = nullptr;
static effect_runtime *g_runtime = nullptr;
static device_api g_device_api = device_api::d3d9;
static bool g_enabled_for_runtime = true;
static uint32_t g_width = 0, g_height = 0;
static std::atomic_bool g_enable_depth_processing(false);
static uint64_t g_last_process_qpc = 0;

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
static std::atomic_bool g_enable_vulkan_depth_bind(true);
static resource_view g_vulkan_depth_candidate_dsv = { 0 };
static resource g_vulkan_depth_candidate_res = { 0 };
static uint32_t g_vulkan_depth_candidate_w = 0;
static uint32_t g_vulkan_depth_candidate_h = 0;
static uint32_t g_vulkan_depth_candidate_samples = 1;
static format g_vulkan_depth_candidate_format = format::unknown;
static uint32_t g_vulkan_depth_candidate_score = 0;
// Prefer deterministic scene pass selection, but allow scored fallback on engines that never bind backbuffer here.
static std::atomic_bool g_require_vulkan_backbuffer_rt(false);
static std::atomic_bool g_lock_vulkan_depth(true);
static uint32_t g_vulkan_depth_last_score = 0;

// Optional resolved depth path for MSAA depth buffers.
static resource g_vulkan_depth_resolved = { 0 };
static resource_view g_vulkan_depth_resolved_srv = { 0 };
static uint32_t g_vulkan_depth_resolved_w = 0;
static uint32_t g_vulkan_depth_resolved_h = 0;
static format g_vulkan_depth_resolved_format = format::unknown;
static std::atomic_bool g_enable_vulkan_msaa_resolve(true);
static std::atomic_bool g_require_vulkan_backbuffer_match(false);

// Forward decl
static void ProcessPendingDepth();
static void try_bind_vulkan_depth(resource_view dsv, uint32_t score_hint);
static void bind_vulkan_candidate_if_good();

extern "C" __declspec(dllexport)
void NFSTweak_RequestPreHudEffects()
{
    const uint64_t frame = g_frame_index.load(std::memory_order_relaxed);
    g_request_pre_hud_frame.store(frame, std::memory_order_relaxed);
    g_request_pre_hud_effects.store(true);
    g_defer_first_qualifying_pass_after_request.store(true, std::memory_order_relaxed);
}

extern "C" __declspec(dllexport)
void NFSTweak_RenderEffectsPreHudNow()
{
    // Safe deterministic signal from bridge hook (IDA-validated FE boundary).
    // Actual render happens in the bind callback where command context is valid.
    const uint64_t frame = g_frame_index.load(std::memory_order_relaxed);
    g_prehud_request_count.fetch_add(1);
    g_last_bridge_request_frame.store(frame, std::memory_order_relaxed);
    g_request_pre_hud_frame.store(frame, std::memory_order_relaxed);
    g_request_pre_hud_effects.store(true);
    g_defer_first_qualifying_pass_after_request.store(true, std::memory_order_relaxed);
}

// Vulkan/DXVK: called whenever application binds render targets + depth (lets us discover the active depth buffer).
static void on_bind_render_targets_and_depth_stencil(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    if (!g_runtime || !g_device)
        return;

    static uint32_t s_bind_debug = 0;
    if (s_bind_debug++ < 6)
    {
        char msg[256] = {};
        sprintf_s(msg,
            "NFSTweakBridge: bind RT/DSV callback (cmd=%p, count=%u, rtv0=%llu, dsv=%llu, auto=%d, req=%d, issued=%d)\n",
            cmd_list, count,
            (count > 0 && rtvs) ? static_cast<unsigned long long>(rtvs[0].handle) : 0ull,
            static_cast<unsigned long long>(dsv.handle),
            g_auto_pre_hud_effects.load() ? 1 : 0,
            g_request_pre_hud_effects.load() ? 1 : 0,
            g_pre_hud_effects_issued_this_frame.load() ? 1 : 0);
        log_info(msg);
    }

    if (g_device_api != device_api::vulkan)
        return;
    static uint32_t s_seen = 0;
    if (s_seen++ < 3)
        log_info("NFSTweakBridge: bind_render_targets_and_depth_stencil (Vulkan)\n");
    // Score higher if render targets include the current back buffer.
    uint32_t score = 0;
    const resource back = g_runtime->get_current_back_buffer();
    const resource_desc back_desc = g_device->get_resource_desc(back);
    const uint32_t bb_w = back_desc.texture.width;
    const uint32_t bb_h = back_desc.texture.height;
    for (uint32_t i = 0; i < count; ++i)
    {
        const resource r = g_device->get_resource_from_view(rtvs[i]);
        if (r.handle != 0 && r.handle == back.handle)
        {
            score = 1000;
            break;
        }
        if (r.handle != 0 && bb_w != 0 && bb_h != 0)
        {
            const resource_desc rd = g_device->get_resource_desc(r);
            if (rd.type == resource_type::texture_2d && rd.texture.width == bb_w && rd.texture.height == bb_h)
                score = std::max(score, 600u);
        }
    }

    // Pre-HUD path (safe point):
    // Render on the first pass this frame that has both RT and DSV bound.
    // This avoids Vulkan begin_render_pass reentrancy issues and usually happens before HUD passes.
    if (cmd_list != nullptr &&
        count > 0 && rtvs != nullptr && rtvs[0].handle != 0 &&
        (g_auto_pre_hud_effects.load() || g_request_pre_hud_effects.load()) &&
        !g_pre_hud_effects_issued_this_frame.load() &&
        !g_running_manual_effects.exchange(true))
    {
        g_runtime->render_effects(cmd_list, rtvs[0], rtvs[0]);
        g_running_manual_effects.store(false);
        g_pre_hud_effects_issued_this_frame.store(true);
        g_request_pre_hud_effects.store(false);
        static uint64_t s_bind_render_log_count = 0;
        const uint64_t c = ++s_bind_render_log_count;
        if (c <= 3 || (c % 120) == 0)
            log_info("NFSTweakBridge: Rendered effects at pre-HUD pass (bind RT/DSV).\n");
    }

    if (dsv.handle != 0)
        try_bind_vulkan_depth(dsv, score);
}

static void try_bind_vulkan_depth(resource_view dsv, uint32_t score_hint)
{
    if (!g_runtime || !g_device)
        return;
    if (g_device_api != device_api::vulkan)
        return;
    if (!g_enable_vulkan_depth_bind.load())
        return;
    if (g_lock_vulkan_depth.load() && g_runtime_depth_resource.handle != 0)
        return;
    if (dsv.handle == 0)
        return;

    // Record candidate; actual bind happens once per frame in 'on_present' (reduces flicker and partial binds).
    const resource depth_res = g_device->get_resource_from_view(dsv);
    if (depth_res.handle == 0)
        return;

    const resource_desc res_desc = g_device->get_resource_desc(depth_res);
    if (res_desc.type != resource_type::texture_2d)
        return;

    // Choose the "best" candidate by preferring the highest score (main camera pass), then largest area.
    const uint64_t area = static_cast<uint64_t>(res_desc.texture.width) * res_desc.texture.height;
    const uint64_t best_area = static_cast<uint64_t>(g_vulkan_depth_candidate_w) * g_vulkan_depth_candidate_h;
    if (score_hint > g_vulkan_depth_candidate_score || (score_hint == g_vulkan_depth_candidate_score && area >= best_area))
    {
        g_vulkan_depth_candidate_dsv = dsv;
        g_vulkan_depth_candidate_res = depth_res;
        g_vulkan_depth_candidate_w = res_desc.texture.width;
        g_vulkan_depth_candidate_h = res_desc.texture.height;
        g_vulkan_depth_candidate_samples = res_desc.texture.samples;
        g_vulkan_depth_candidate_format = g_device->get_resource_view_desc(dsv).format;
        g_vulkan_depth_candidate_score = score_hint;
    }
}

static void bind_vulkan_candidate_if_good()
{
    if (!g_runtime || !g_device)
        return;
    if (g_device_api != device_api::vulkan)
        return;
    if (!g_enable_vulkan_depth_bind.load())
        return;
    if (g_vulkan_depth_candidate_dsv.handle == 0 || g_vulkan_depth_candidate_res.handle == 0)
        return;

    // Throttle binding to avoid thrashing descriptor updates on Vulkan.
    LARGE_INTEGER freq = {}, now = {};
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&now) && freq.QuadPart != 0)
    {
        const uint64_t now_qpc = static_cast<uint64_t>(now.QuadPart);
        if (g_last_vulkan_bind_qpc != 0)
        {
            // 15 Hz max rebinding rate
            const uint64_t min_delta = static_cast<uint64_t>(freq.QuadPart / 15);
            if (now_qpc - g_last_vulkan_bind_qpc < min_delta)
                return;
        }
        g_last_vulkan_bind_qpc = now_qpc;
    }

    const resource back = g_runtime->get_current_back_buffer();
    const resource_desc back_desc = g_device->get_resource_desc(back);
    const uint32_t bb_w = back_desc.texture.width;
    const uint32_t bb_h = back_desc.texture.height;

    if (bb_w != 0 && bb_h != 0)
    {
        if (g_require_vulkan_backbuffer_match.load())
        {
            // Require exact match to avoid binding UI/partial-res depth buffers (fixes "only upper part shown").
            if (g_vulkan_depth_candidate_w != bb_w || g_vulkan_depth_candidate_h != bb_h)
                return;
        }
    }

    // Hysteresis: avoid switching to a lower-confidence camera (helps with "wrong camera" flicker).
    // Only allow switching if the new score is significantly better than the last bound one.
    if (g_runtime_depth_resource.handle != 0 && g_vulkan_depth_candidate_score + 150 < g_vulkan_depth_last_score)
        return;

    // If MSAA, prefer resolved depth SRV if available.
    const bool is_msaa = (g_vulkan_depth_candidate_samples > 1);
    if (!is_msaa && g_runtime_depth_srv.handle != 0 && g_runtime_depth_resource.handle == g_vulkan_depth_candidate_res.handle)
        return;

    if (g_runtime_depth_srv.handle != 0)
    {
        g_device->destroy_resource_view(g_runtime_depth_srv);
        g_runtime_depth_srv = { 0 };
    }

    if (is_msaa && g_vulkan_depth_resolved_srv.handle != 0 &&
        g_vulkan_depth_resolved_w == bb_w && g_vulkan_depth_resolved_h == bb_h &&
        g_vulkan_depth_resolved_format == g_vulkan_depth_candidate_format)
    {
        g_runtime->update_texture_bindings("CUSTOMDEPTH", g_vulkan_depth_resolved_srv, g_vulkan_depth_resolved_srv);
        log_info("NFSTweakBridge: Bound RESOLVED Vulkan depth buffer as CUSTOMDEPTH.\n");
        g_vulkan_depth_last_score = g_vulkan_depth_candidate_score;

        g_vulkan_depth_candidate_dsv = { 0 };
        g_vulkan_depth_candidate_res = { 0 };
        g_vulkan_depth_candidate_w = 0;
        g_vulkan_depth_candidate_h = 0;
        g_vulkan_depth_candidate_samples = 1;
        g_vulkan_depth_candidate_format = format::unknown;
        g_vulkan_depth_candidate_score = 0;
        return;
    }

    const resource_view_desc dsv_desc = g_device->get_resource_view_desc(g_vulkan_depth_candidate_dsv);
    resource_view_desc srv_desc = dsv_desc; // preserve view type + layer/level range

    resource_view srv = { 0 };
    if (!g_device->create_resource_view(g_vulkan_depth_candidate_res, resource_usage::shader_resource, srv_desc, &srv))
    {
        char msg[256] = {};
        sprintf_s(msg, "NFSTweakBridge: Failed to create SRV for candidate depth (fmt=%u, w=%u, h=%u, samples=%u)\n",
            (unsigned)dsv_desc.format, g_vulkan_depth_candidate_w, g_vulkan_depth_candidate_h, g_vulkan_depth_candidate_samples);
        log_info(msg);
        return;
    }

    g_runtime_depth_srv = srv;
    g_runtime_depth_resource = g_vulkan_depth_candidate_res;
    g_runtime->update_texture_bindings("CUSTOMDEPTH", g_runtime_depth_srv, g_runtime_depth_srv);
    log_info("NFSTweakBridge: Bound Vulkan depth buffer as CUSTOMDEPTH.\n");
    g_vulkan_depth_last_score = g_vulkan_depth_candidate_score;

    // Reset candidate each frame so we don't stick to a stale/mismatched camera depth buffer.
    g_vulkan_depth_candidate_dsv = { 0 };
    g_vulkan_depth_candidate_res = { 0 };
    g_vulkan_depth_candidate_w = 0;
    g_vulkan_depth_candidate_h = 0;
    g_vulkan_depth_candidate_samples = 1;
    g_vulkan_depth_candidate_format = format::unknown;
    g_vulkan_depth_candidate_score = 0;
}

static void on_begin_render_pass(command_list *cmd_list, uint32_t count, const render_pass_render_target_desc *rts, const render_pass_depth_stencil_desc *ds)
{
    if (g_device_api != device_api::vulkan)
        return;
    if (g_runtime == nullptr || g_device == nullptr)
        return;
    const uint64_t bp = g_beginpass_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ds == nullptr)
        return;
    // Score higher if this render pass targets the current back buffer.
    uint32_t score = 0;
    const resource back = g_runtime ? g_runtime->get_current_back_buffer() : resource{ 0 };
    const resource_desc back_desc = (g_runtime && back.handle != 0) ? g_device->get_resource_desc(back) : resource_desc{};
    const uint32_t bb_w = back_desc.texture.width;
    const uint32_t bb_h = back_desc.texture.height;
    if (back.handle != 0)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const resource r = g_device->get_resource_from_view(rts[i].view);
            if (r.handle != 0 && r.handle == back.handle)
            {
                score = 1000;
                break;
            }
            if (r.handle != 0 && bb_w != 0 && bb_h != 0)
            {
                const resource_desc rd = g_device->get_resource_desc(r);
                if (rd.type == resource_type::texture_2d && rd.texture.width == bb_w && rd.texture.height == bb_h)
                    score = std::max(score, 600u);
            }
        }
    }
    // IMPORTANT:
    // Do NOT call 'render_effects' from inside Vulkan begin_render_pass callback.
    // That can cause invalid nested render pass / command state and crash.

    // Experimental but deterministic Vulkan pre-HUD path:
    // On builds where 'bind_render_targets_and_depth_stencil' does not fire, trigger here with strict guards.
    resource_view prehud_rtv = { 0 };
    resource prehud_rtv_resource = { 0 };
    resource prehud_dsv_resource = { 0 };
    bool lock_miss_this_pass = false;
    if (ds && ds->view.handle != 0)
        prehud_dsv_resource = g_device->get_resource_from_view(ds->view);
    if (count > 0 && rts != nullptr)
    {
        // 0) If a pre-HUD RT+DS pair is locked, try to reuse it first (stability against flashing).
        if (g_prehud_locked_rt_resource.handle != 0 && g_prehud_locked_ds_resource.handle != 0)
        {
            bool found_locked_pair = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                const resource rr = g_device->get_resource_from_view(rts[i].view);
                if (rr.handle != 0 &&
                    rr.handle == g_prehud_locked_rt_resource.handle &&
                    prehud_dsv_resource.handle != 0 &&
                    prehud_dsv_resource.handle == g_prehud_locked_ds_resource.handle)
                {
                    prehud_rtv = rts[i].view;
                    prehud_rtv_resource = rr;
                    found_locked_pair = true;
                    break;
                }
            }
            // Lock miss in this pass: keep lock and wait for matching pass later in the frame.
            if (!found_locked_pair)
                lock_miss_this_pass = true;
        }

        // Locked mode: only render on the locked signature.
        if (lock_miss_this_pass)
        {
            return;
        }

        // Prefer the RT that maps to the current back buffer to avoid pass-to-pass flicker.
        if (prehud_rtv.handle == 0)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const resource rr = g_device->get_resource_from_view(rts[i].view);
                if (rr.handle != 0 && rr.handle == back.handle)
                {
                    prehud_rtv = rts[i].view;
                    prehud_rtv_resource = rr;
                    break;
                }
            }
        }
        // Fallback: pick the best-scored full-res RT in this pass when explicit backbuffer requirement is disabled.
        if (prehud_rtv.handle == 0 && !g_require_vulkan_backbuffer_rt.load() && bb_w != 0 && bb_h != 0)
        {
            uint32_t best_i = UINT32_MAX;
            uint32_t best_score = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const resource rr = g_device->get_resource_from_view(rts[i].view);
                if (rr.handle == 0)
                    continue;
                const resource_desc rd = g_device->get_resource_desc(rr);
                if (rd.type != resource_type::texture_2d)
                    continue;

                uint32_t local = 0;
                if (rd.texture.width == bb_w && rd.texture.height == bb_h)
                    local = 700;
                else if (rd.texture.width >= (bb_w * 3) / 4 && rd.texture.height >= (bb_h * 3) / 4)
                    local = 400;

                if (local > best_score)
                {
                    best_score = local;
                    best_i = i;
                }
            }

            if (best_i != UINT32_MAX)
            {
                prehud_rtv = rts[best_i].view;
                prehud_rtv_resource = g_device->get_resource_from_view(rts[best_i].view);
            }
        }
    }

    const uint64_t frame = g_frame_index.load(std::memory_order_relaxed);
    if (g_request_pre_hud_effects.load(std::memory_order_relaxed) &&
        g_request_pre_hud_frame.load(std::memory_order_relaxed) != frame)
    {
        // Drop stale request from prior frame to avoid rendering on wrong early pass.
        g_request_pre_hud_effects.store(false, std::memory_order_relaxed);
    }
    const bool wants_prehud = g_request_pre_hud_effects.load(std::memory_order_relaxed);

    // Request timing from bridge often lands right before the first scene pass candidate.
    // Defer one qualifying pass so render happens on the subsequent scene pass in the same frame.
    if (wants_prehud &&
        prehud_rtv.handle != 0 &&
        prehud_dsv_resource.handle != 0 &&
        score >= 600 &&
        g_defer_first_qualifying_pass_after_request.exchange(false, std::memory_order_relaxed))
    {
        try_bind_vulkan_depth(ds->view, score);
        return;
    }
    if (g_enable_vulkan_beginpass_prehud.load() &&
        !g_disable_beginpass_after_fault.load(std::memory_order_relaxed) &&
        cmd_list != nullptr &&
        prehud_rtv.handle != 0 &&
        prehud_rtv_resource.handle != 0 &&
        prehud_dsv_resource.handle != 0 &&
        wants_prehud &&
        g_skip_manual_prehud_frames.load() <= 0 &&
        g_runtime->get_effects_state() &&
        !g_running_manual_effects.exchange(true))
    {
        const resource_desc prehud_desc = g_device->get_resource_desc(prehud_rtv_resource);
        if (prehud_desc.type != resource_type::texture_2d || prehud_desc.texture.samples > 1)
        {
            // AA path: avoid rendering on MSAA targets (causes interlacing/artifacts).
            // Keep request pending for a later resolved single-sample scene pass.
            g_running_manual_effects.store(false);
            return;
        }

        bool render_ok = true;
        __try
        {
            g_runtime->render_effects(cmd_list, prehud_rtv, prehud_rtv);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            render_ok = false;
            g_disable_beginpass_after_fault.store(true, std::memory_order_relaxed);
            log_info("NFSTweakBridge: render_effects fault in begin_render_pass; disabling beginpass path.\n");
        }
        if (!render_ok)
        {
            g_running_manual_effects.store(false);
            g_request_pre_hud_effects.store(false);
            return;
        }
        const uint64_t rc = g_render_counter.fetch_add(1, std::memory_order_relaxed) + 1;
        g_running_manual_effects.store(false);
        g_request_pre_hud_effects.store(false);
        // Relock only on strong scene-aligned passes to reduce oscillation.
        const bool strong_scene_pass =
            (back.handle != 0 && prehud_rtv_resource.handle == back.handle) ||
            (!g_require_vulkan_backbuffer_rt.load() && score >= 1000);
        if (strong_scene_pass && prehud_rtv_resource.handle != 0)
            g_prehud_locked_rt_resource = prehud_rtv_resource;
        if (strong_scene_pass)
        {
            g_prehud_locked_ds_resource = prehud_dsv_resource;
            g_prehud_lock_last_hit_frame.store(frame, std::memory_order_relaxed);
            g_prehud_lock_miss_frames.store(0, std::memory_order_relaxed);
        }
        else
        {
            g_prehud_locked_rt_resource = { 0 };
            g_prehud_locked_ds_resource = { 0 };
            g_prehud_lock_last_hit_frame.store(0, std::memory_order_relaxed);
            g_prehud_lock_miss_frames.store(0, std::memory_order_relaxed);
        }
        if (rc <= 5 || (rc % 120) == 0)
        {
            char msg[320] = {};
            sprintf_s(msg,
                "NFSTweakBridge: Rendered effects at pre-HUD (rc=%llu frame=%llu bp=%llu rtv=%llu dsv=%llu score=%u)\n",
                static_cast<unsigned long long>(rc),
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(bp),
                static_cast<unsigned long long>(prehud_rtv_resource.handle),
                static_cast<unsigned long long>(prehud_dsv_resource.handle),
                score);
            log_info(msg);
        }
    }

    try_bind_vulkan_depth(ds->view, score);

    // If the chosen candidate is MSAA, attempt to resolve it to a non-MSAA texture we can sample reliably.
    if (g_enable_vulkan_msaa_resolve.load() && g_vulkan_depth_candidate_samples > 1 && g_vulkan_depth_candidate_res.handle != 0)
    {
        if (!g_device->check_capability(device_caps::resolve_depth_stencil))
            return;

        // Backbuffer dims already computed above.
        if (bb_w == 0 || bb_h == 0)
            return;

        if (g_require_vulkan_backbuffer_match.load() && (g_vulkan_depth_candidate_w != bb_w || g_vulkan_depth_candidate_h != bb_h))
            return;

        // (Re)create resolve target if needed
        if (g_vulkan_depth_resolved.handle == 0 || g_vulkan_depth_resolved_w != bb_w || g_vulkan_depth_resolved_h != bb_h || g_vulkan_depth_resolved_format != g_vulkan_depth_candidate_format)
        {
            if (g_vulkan_depth_resolved_srv.handle != 0)
            {
                g_device->destroy_resource_view(g_vulkan_depth_resolved_srv);
                g_vulkan_depth_resolved_srv = { 0 };
            }
            if (g_vulkan_depth_resolved.handle != 0)
            {
                g_device->destroy_resource(g_vulkan_depth_resolved);
                g_vulkan_depth_resolved = { 0 };
            }

            resource_desc desc = {};
            desc.type = resource_type::texture_2d;
            desc.texture.width = bb_w;
            desc.texture.height = bb_h;
            desc.texture.depth_or_layers = 1;
            desc.texture.levels = 1;
            desc.texture.format = g_vulkan_depth_candidate_format;
            desc.texture.samples = 1;
            desc.usage = resource_usage::resolve_dest | resource_usage::shader_resource;

            if (!g_device->create_resource(desc, nullptr, resource_usage::resolve_dest, &g_vulkan_depth_resolved))
                return;

            resource_view_desc srv_desc = resource_view_desc(resource_view_type::texture_2d, g_vulkan_depth_candidate_format, 0, 1, 0, 1);
            if (!g_device->create_resource_view(g_vulkan_depth_resolved, resource_usage::shader_resource, srv_desc, &g_vulkan_depth_resolved_srv))
            {
                g_device->destroy_resource(g_vulkan_depth_resolved);
                g_vulkan_depth_resolved = { 0 };
                return;
            }

            g_vulkan_depth_resolved_w = bb_w;
            g_vulkan_depth_resolved_h = bb_h;
            g_vulkan_depth_resolved_format = g_vulkan_depth_candidate_format;
            log_info("NFSTweakBridge: Created resolved depth target.\n");
        }

        // Insert resolve into command list
        const resource src = g_vulkan_depth_candidate_res;
        const resource dst = g_vulkan_depth_resolved;

        // Best-effort transitions (exact prior state may differ across engines).
        cmd_list->barrier(src, resource_usage::depth_stencil_write, resource_usage::resolve_source);
        cmd_list->barrier(dst, resource_usage::resolve_dest, resource_usage::resolve_dest);
        cmd_list->resolve_texture_region(src, 0, nullptr, dst, 0, 0, 0, 0, g_vulkan_depth_candidate_format);
        cmd_list->barrier(src, resource_usage::resolve_source, resource_usage::depth_stencil_write);
    }
}

static bool on_clear_depth_stencil_view(command_list *, resource_view dsv, const float *, const uint8_t *, uint32_t, const rect *)
{
    if (g_device_api != device_api::vulkan)
        return false;
    if (g_runtime == nullptr || g_device == nullptr)
        return false;
    g_clear_counter.fetch_add(1, std::memory_order_relaxed);
    // Low score: without RT context we may capture non-main-camera depth (mirror/reflection/shadow).
    try_bind_vulkan_depth(dsv, 0);
    return false; // do not block clear
}

static void on_reshade_reloaded_effects(effect_runtime *runtime)
{
    if (runtime != g_runtime)
        return;
    g_disable_beginpass_after_fault.store(false, std::memory_order_relaxed);
    // Some setups emit this repeatedly; only apply a settle reset once to avoid lock thrash/flicker.
    if (!g_seen_reload_settle.exchange(true, std::memory_order_relaxed))
    {
        g_skip_manual_prehud_frames.store(8);
        g_running_manual_effects.store(false);
        g_pre_hud_effects_issued_this_frame.store(false);
        g_prehud_locked_rt_resource = { 0 };
        g_prehud_locked_ds_resource = { 0 };
        g_prehud_lock_last_hit_frame.store(0, std::memory_order_relaxed);
        g_prehud_lock_miss_frames.store(0, std::memory_order_relaxed);
        log_info("NFSTweakBridge: Effects reloaded, delaying manual pre-HUD pass (initial settle).\n");
    }
}

// ---------- Helper: create or resize the ReShade depth resource ----------
static bool create_or_resize_depth_resource(device *dev, uint32_t width, uint32_t height, resource &out_res, resource_view &srv_out, resource_view &srv_out_srgb)
{
    if (!dev) return false;

    resource_desc desc = {};
    desc.type = resource_type::texture_2d;
    desc.texture.width = width;
    desc.texture.height = height;
    desc.texture.depth_or_layers = 1;
    desc.texture.levels = 1;
    // use r32_float for depth storage (shaders expect this)
    desc.texture.format = format::r32_float;
    // Needs copy_dest for update_texture_region, and shader_resource for sampling.
    desc.usage = resource_usage::shader_resource | resource_usage::copy_dest;

    resource tmp = {};
    // modern API: create_resource returns bool and fills out tmp
    if (!dev->create_resource(desc, nullptr, resource_usage::copy_dest, &tmp))
    {
        OutputDebugStringA("NFSTweakBridge: create_resource failed\n");
        return false;
    }

    // create SRV (linear)
    resource_view srv = {};
    
    if (!dev->create_resource_view(tmp, resource_usage::shader_resource,
        resource_view_desc(format::r32_float), &srv))
    {
        dev->destroy_resource(tmp);
        OutputDebugStringA("NFSTweakBridge: create_resource_view (linear) failed\n");
        return false;
    }

    // destroy old if present
    if (out_res.handle != 0)
        dev->destroy_resource(out_res);
    if (srv_out.handle != 0)
        dev->destroy_resource_view(srv_out);

    out_res = tmp;
    srv_out = srv;
    srv_out_srgb = { 0 };
    return true;
}

// ---------- Exported API: called by ASI (your dllmain.cpp already resolves this) ----------
extern "C" __declspec(dllexport)
void NFSTweak_PushDepthSurface(void* d3d9_surface_ptr, unsigned int width, unsigned int height)
{
    if (!d3d9_surface_ptr) return;
    if (width == 0 || height == 0) return;

    IDirect3DSurface9* pSrc = reinterpret_cast<IDirect3DSurface9*>(d3d9_surface_ptr);
    if (!pSrc) return;

    // Take lock + AddRef
    std::lock_guard<std::mutex> lock(g_push_mutex);

    pSrc->AddRef(); // ensure the surface remains valid for processing

    // Drop/replace previous
    if (g_last_depth_surface)
        g_last_depth_surface->Release();

    g_last_depth_surface = pSrc;
    g_last_width = width;
    g_last_height = height;
    g_pending_depth.store(true);
}

// DXVK-safe API: push CPU depth buffer (R32 float)
// - 'data' points to the first row
// - 'row_pitch_bytes' is the stride in bytes between rows
extern "C" __declspec(dllexport)
void NFSTweak_PushDepthBufferR32F(const void* data, unsigned int width, unsigned int height, unsigned int row_pitch_bytes)
{
    if (!data || width == 0 || height == 0 || row_pitch_bytes == 0)
        return;
    if ((row_pitch_bytes % sizeof(float)) != 0)
        return;

    const uint32_t row_pitch_floats = row_pitch_bytes / sizeof(float);
    if (row_pitch_floats < width)
        return;

    std::lock_guard<std::mutex> lock(g_push_mutex);

    // Drop any surface-based pending work (avoid mixing paths)
    if (g_last_depth_surface)
    {
        g_last_depth_surface->Release();
        g_last_depth_surface = nullptr;
    }

    g_last_width = width;
    g_last_height = height;
    g_last_depth_cpu_row_pitch_floats = row_pitch_floats;

    // Copy into our own buffer so producer can reuse/free immediately
    g_last_depth_cpu.resize(static_cast<size_t>(row_pitch_floats) * static_cast<size_t>(height));
    memcpy(g_last_depth_cpu.data(), data, static_cast<size_t>(row_pitch_bytes) * static_cast<size_t>(height));
    g_has_depth_cpu = true;

    g_pending_depth.store(true);
}

// ---------- Process pending depth during present (ReShade thread/context) ----------
static void ProcessPendingDepth()
{
    // Called from present() where g_runtime and device are valid
    if (!g_runtime || !g_device) return;
    if (!g_enabled_for_runtime) return;
    if (!g_pending_depth.load()) return;
    if (!g_enable_depth_processing.load()) return;

    // Throttle CPU readback to avoid hard stalls if the producer pushes every frame.
    // This is intentionally conservative: it keeps the game responsive while debugging.
    LARGE_INTEGER freq = {}, now = {};
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&now) && freq.QuadPart != 0)
    {
        const uint64_t now_qpc = static_cast<uint64_t>(now.QuadPart);
        if (g_last_process_qpc != 0)
        {
            // 15 Hz max processing rate
            const uint64_t min_delta = static_cast<uint64_t>(freq.QuadPart / 15);
            if (now_qpc - g_last_process_qpc < min_delta)
                return;
        }
        g_last_process_qpc = now_qpc;
    }

    // Lock and grab current payload (surface or CPU buffer)
    std::lock_guard<std::mutex> lock(g_push_mutex);

    if (!g_last_depth_surface && !g_has_depth_cpu)
    {
        g_pending_depth.store(false);
        return;
    }

    // Fast path: CPU buffer upload (DXVK-safe)
    if (g_has_depth_cpu)
    {
        // Ensure resource size matches
        if (g_custom_depth.handle == 0 || g_last_width != g_width || g_last_height != g_height)
        {
            if (!create_or_resize_depth_resource(g_device, g_last_width, g_last_height, g_custom_depth, g_custom_depth_view, g_custom_depth_view_srgb))
            {
                OutputDebugStringA("NFSTweakBridge: failed to create/resize g_custom_depth (CPU path)\n");
                g_has_depth_cpu = false;
                g_last_depth_cpu.clear();
                g_pending_depth.store(false);
                return;
            }
            g_width = g_last_width;
            g_height = g_last_height;
        }

        // If row pitch != width, repack to tight rows
        const float* src = g_last_depth_cpu.data();
        uint32_t src_pitch = g_last_depth_cpu_row_pitch_floats;
        std::vector<float> tight;
        if (src_pitch != g_last_width)
        {
            tight.resize(static_cast<size_t>(g_last_width) * static_cast<size_t>(g_last_height));
            for (uint32_t y = 0; y < g_last_height; ++y)
                memcpy(tight.data() + static_cast<size_t>(y) * g_last_width, src + static_cast<size_t>(y) * src_pitch, static_cast<size_t>(g_last_width) * sizeof(float));
            src = tight.data();
            src_pitch = g_last_width;
        }

        subresource_data sub_data = {};
        sub_data.data = const_cast<float*>(src);
        sub_data.row_pitch = static_cast<uint32_t>(src_pitch * sizeof(float));
        sub_data.slice_pitch = static_cast<uint32_t>(g_last_width * g_last_height * sizeof(float));

        g_device->update_texture_region(sub_data, g_custom_depth, 0, nullptr);

        // Consume
        g_has_depth_cpu = false;
        g_last_depth_cpu.clear();
        g_pending_depth.store(false);
        OutputDebugStringA("NFSTweakBridge: Depth buffer uploaded via CPU path.\n");
        return;
    }

    // If resource size doesn't match, recreate
    if (g_custom_depth.handle == 0 || g_last_width != g_width || g_last_height != g_height)
    {
        if (!create_or_resize_depth_resource(g_device, g_last_width, g_last_height, g_custom_depth, g_custom_depth_view, g_custom_depth_view_srgb))
        {
            // failed to create resource; drop pending
            OutputDebugStringA("NFSTweakBridge: failed to create/resize g_custom_depth\n");
            g_pending_depth.store(false);
            // release surface
            g_last_depth_surface->Release();
            g_last_depth_surface = nullptr;
            return;
        }
        g_width = g_last_width;
        g_height = g_last_height;
    }

    // =========
    // TODO: Implement fast GPU-side copy from the incoming IDirect3DSurface9* (g_last_depth_surface)
    // into the ReShade resource (g_custom_depth). This is the recommended approach for performance.
    //
    // Suggested approach (best-effort outline):
    // 1) Get IDirect3DDevice9* from g_last_depth_surface via GetDevice().
    // 2) Create an intermediate IDirect3DTexture9 (D3DPOOL_DEFAULT) with format R32F or a 32-bit format,
    //    same dimensions as depth (or engine's depth). Use CreateTexture(..., D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT).
    // 3) Use pDev->GetRenderTargetData(srcDepthSurface, pSysMemSurface) or StretchRect if copying between RTs.
    //    Note: GetRenderTargetData requires a DEFAULT->SYSTEMMEM copy; StretchRect works between DEFAULT resources.
    // 4) If you can create a shared/native handle that ReShade can import, pass that handle to ReShade.
    // 5) Otherwise, read back to CPU once (GetRenderTargetData -> LockRect), convert to r32 floats, and then upload into
    //    ReShade resource using g_device->update_texture_region(...) or a command list copy.
    //
    // The exact D3D9 code depends on the engine's depth format. Depth surfaces are often D24S8/D16 formats which are not
    // directly lockable; you'll likely need to copy to a render target (if the game writes linear depth to a texture) or use
    // IDirect3DDevice9::GetRenderTargetData to copy to a SYSTEMMEM surface and then LockRect.
    //
    // If you want, I can implement a concrete GPU-copy + CPU-fallback version for these common formats (D24S8, D16, R32F).
    // =========

    // Get D3D9 device from the surface
    IDirect3DDevice9* d3d9_device = nullptr;
    g_last_depth_surface->GetDevice(&d3d9_device);
    if (!d3d9_device)
    {
        OutputDebugStringA("NFSTweakBridge: Failed to get D3D9 device from surface\n");
        g_last_depth_surface->Release();
        g_last_depth_surface = nullptr;
        g_pending_depth.store(false);
        return;
    }

    D3DSURFACE_DESC src_desc = {};
    if (SUCCEEDED(g_last_depth_surface->GetDesc(&src_desc)))
    {
        // Most games use depth-stencil formats here (D24S8/D16). Those often cannot be read back via GetRenderTargetData.
        if (src_desc.Format == D3DFMT_D16 || src_desc.Format == D3DFMT_D24X8 || src_desc.Format == D3DFMT_D24S8 ||
            src_desc.Format == D3DFMT_D24X4S4 || src_desc.Format == D3DFMT_D32 || src_desc.Format == D3DFMT_D32F_LOCKABLE ||
            src_desc.Format == D3DFMT_D15S1)
        {
            OutputDebugStringA("NFSTweakBridge: Incoming surface looks like a depth-stencil format; CPU readback may fail (prefer capturing a lockable color/linear-depth surface instead).\n");
        }
    }

    IDirect3DSurface9* sysmem_surface = nullptr;
    D3DFORMAT sysmem_format = D3DFMT_R32F; // Prefer R32F
    HRESULT hr = d3d9_device->CreateOffscreenPlainSurface(g_last_width, g_last_height, sysmem_format, D3DPOOL_SYSTEMMEM, &sysmem_surface, NULL);

    if (FAILED(hr))
    {
        // Fallback to A8R8G8B8 if R32F is not supported for offscreen plain surfaces
        sysmem_format = D3DFMT_A8R8G8B8;
        hr = d3d9_device->CreateOffscreenPlainSurface(g_last_width, g_last_height, sysmem_format, D3DPOOL_SYSTEMMEM, &sysmem_surface, NULL);
        if (FAILED(hr))
        {
            OutputDebugStringA("NFSTweakBridge: Failed to create offscreen plain surface with R32F or A8R8G8B8.\n");
            d3d9_device->Release();
            g_last_depth_surface->Release();
            g_last_depth_surface = nullptr;
            g_pending_depth.store(false);
            return;
        }
    }

    hr = d3d9_device->GetRenderTargetData(g_last_depth_surface, sysmem_surface);
    if (FAILED(hr))
    {
        char msg[256];
        sprintf_s(msg, "NFSTweakBridge: GetRenderTargetData failed (hr=0x%08X, srcFormat=%d)\n", (unsigned)hr, (int)src_desc.Format);
        OutputDebugStringA(msg);
        sysmem_surface->Release();
        d3d9_device->Release();
        g_last_depth_surface->Release();
        g_last_depth_surface = nullptr;
        g_pending_depth.store(false);
        return;
    }

    D3DLOCKED_RECT locked_rect;
    hr = sysmem_surface->LockRect(&locked_rect, NULL, D3DLOCK_READONLY);
    if (FAILED(hr))
    {
        OutputDebugStringA("NFSTweakBridge: LockRect failed\n");
        sysmem_surface->Release();
        d3d9_device->Release();
        g_last_depth_surface->Release();
        g_last_depth_surface = nullptr;
        g_pending_depth.store(false);
        return;
    }

    // If resource size doesn't match, recreate
    if (g_custom_depth.handle == 0 || g_last_width != g_width || g_last_height != g_height)
    {
        if (!create_or_resize_depth_resource(g_device, g_last_width, g_last_height, g_custom_depth, g_custom_depth_view, g_custom_depth_view_srgb))
        {
            OutputDebugStringA("NFSTweakBridge: failed to create/resize g_custom_depth\n");
            sysmem_surface->UnlockRect();
            sysmem_surface->Release();
            d3d9_device->Release();
            g_last_depth_surface->Release();
            g_last_depth_surface = nullptr;
            g_pending_depth.store(false);
            return;
        }
        g_width = g_last_width;
        g_height = g_last_height;
    }

    // Copy to a buffer and then update resource
    std::vector<float> depth_data(g_last_width * g_last_height);
    for (unsigned int y = 0; y < g_last_height; ++y)
    {
        if (sysmem_format == D3DFMT_R32F)
        {
            // Directly copy float data
            memcpy(depth_data.data() + y * g_last_width, (float*)((uint8_t*)locked_rect.pBits + y * locked_rect.Pitch), g_last_width * sizeof(float));
        }
        else if (sysmem_format == D3DFMT_A8R8G8B8)
        {
            // Convert A8R8G8B8 to float depth (assuming depth is in the red channel)
            uint32_t* row = (uint32_t*)((uint8_t*)locked_rect.pBits + y * locked_rect.Pitch);
            for (unsigned int x = 0; x < g_last_width; ++x)
            {
                uint32_t pixel = row[x];
                depth_data[y * g_last_width + x] = static_cast<float>(((pixel >> 16) & 0xFF) / 255.0f);
            }
        }
        else
        {
            OutputDebugStringA("NFSTweakBridge: Unsupported system memory format for depth conversion.\n");
            sysmem_surface->UnlockRect();
            sysmem_surface->Release();
            d3d9_device->Release();
            g_last_depth_surface->Release();
            g_last_depth_surface = nullptr;
            g_pending_depth.store(false);
            return;
        }
    }

    sysmem_surface->UnlockRect();
    sysmem_surface->Release();

    // Update ReShade resource
    subresource_data sub_data = {};
    sub_data.data = depth_data.data();
    sub_data.row_pitch = g_last_width * sizeof(float);
    sub_data.slice_pitch = g_last_width * g_last_height * sizeof(float);

    // ReShade API: upload CPU data into the texture subresource
    g_device->update_texture_region(
        sub_data,
        g_custom_depth,
        0,      // subresource
        nullptr // entire subresource
    );

    // Release D3D9 device and surface
    d3d9_device->Release();
    g_last_depth_surface->Release();
    g_last_depth_surface = nullptr;
    g_pending_depth.store(false);

    OutputDebugStringA("NFSTweakBridge: Depth surface successfully processed and updated ReShade resource.\n");


    // After a successful copy, bind the resource for shaders:
    // runtime->update_texture_bindings("CUSTOMDEPTH", g_custom_depth_view, g_custom_depth_view);
    // NOTE: Must call update_texture_bindings *after* you have created resource views for g_custom_depth.
}

static void on_overlay_ui(effect_runtime *runtime)
{
    if (ImGui::GetCurrentContext() == nullptr)
        return; // ImGui not initialized, skip drawing

    if (!ImGui::Begin("NFSTweakBridge")) {
        ImGui::End();
        return;
    }

    ImGui::Text("NFSTweakBridge active");
    ImGui::Separator();

    ImGui::Text("Depth incoming: %s", g_pending_depth.load() ? "Yes (pending)" : "No");
    ImGui::Text("PreHUD requests: %u", g_prehud_request_count.load());

    bool enabled = g_enable_depth_processing.load();
    if (ImGui::Checkbox("Enable Depth Processing (CPU readback)", &enabled))
        g_enable_depth_processing.store(enabled);

    bool auto_pre_hud = g_auto_pre_hud_effects.load();
    if (ImGui::Checkbox("Auto Pre-HUD Effects Pass", &auto_pre_hud))
        g_auto_pre_hud_effects.store(auto_pre_hud);

    if (g_device_api == device_api::vulkan)
    {
        bool vk_bind = g_enable_vulkan_depth_bind.load();
        if (ImGui::Checkbox("Enable Vulkan Depth Bind (CUSTOMDEPTH)", &vk_bind))
            g_enable_vulkan_depth_bind.store(vk_bind);

        bool vk_bp = g_enable_vulkan_beginpass_prehud.load();
        if (ImGui::Checkbox("Vulkan: BeginPass Pre-HUD (Experimental)", &vk_bp))
            g_enable_vulkan_beginpass_prehud.store(vk_bp);

        bool vk_bb_rt = g_require_vulkan_backbuffer_rt.load();
        if (ImGui::Checkbox("Vulkan: Require Backbuffer RT Pass", &vk_bb_rt))
            g_require_vulkan_backbuffer_rt.store(vk_bb_rt);

        bool vk_lock = g_lock_vulkan_depth.load();
        if (ImGui::Checkbox("Vulkan: Lock Depth Selection", &vk_lock))
            g_lock_vulkan_depth.store(vk_lock);

        ImGui::Text("Vulkan candidate: %ux%u (samples=%u score=%u)",
            g_vulkan_depth_candidate_w, g_vulkan_depth_candidate_h, g_vulkan_depth_candidate_samples, g_vulkan_depth_candidate_score);
        ImGui::Text("Vulkan last score: %u", g_vulkan_depth_last_score);
        ImGui::Text("PreHUD skip frames after reload: %d", g_skip_manual_prehud_frames.load());
        if (g_runtime && g_device)
        {
            const resource back = g_runtime->get_current_back_buffer();
            const resource_desc bd = g_device->get_resource_desc(back);
            ImGui::Text("Backbuffer: %ux%u", bd.texture.width, bd.texture.height);
        }

        bool vk_match = g_require_vulkan_backbuffer_match.load();
        if (ImGui::Checkbox("Vulkan: Require Backbuffer Match", &vk_match))
            g_require_vulkan_backbuffer_match.store(vk_match);

        bool vk_resolve = g_enable_vulkan_msaa_resolve.load();
        if (ImGui::Checkbox("Vulkan: Enable MSAA Depth Resolve", &vk_resolve))
            g_enable_vulkan_msaa_resolve.store(vk_resolve);

        ImGui::TextUnformatted("Pre-HUD pass runs from RT/DSV bind callback (Vulkan-safe path).");
    }

    if (g_width && g_height)
        ImGui::Text("Current Depth Size: %u x %u", g_width, g_height);

    if (ImGui::Button("ProcessPendingDepth"))
        ProcessPendingDepth();

    ImGui::End();
}

// ---------- ReShade lifecycle callbacks ----------
static void on_init_effect_runtime(effect_runtime *runtime)
{
    g_runtime = runtime;
    g_device = runtime->get_device();
    g_device_api = g_device ? g_device->get_api() : device_api::d3d9;
    g_seen_reload_settle.store(false, std::memory_order_relaxed);
    g_disable_beginpass_after_fault.store(false, std::memory_order_relaxed);
    log_info("NFSTweakBridge: init_effect_runtime\n");

    // Vulkan/DXVK: Do not create any placeholder resources here (this has been observed to hang on some setups).
    // Instead, try to bind the active runtime depth buffer via the 'bind_render_targets_and_depth_stencil' event.
    if (g_device_api == device_api::vulkan)
    {
        g_enabled_for_runtime = true;
        g_auto_pre_hud_effects.store(true);
        log_info("NFSTweakBridge: Vulkan runtime detected (DXVK). Using Vulkan bind hook.\n");
        return;
    }
    g_enabled_for_runtime = true;

    // Create a 1x1 placeholder and bind it immediately so effects compiling early
    // do not force ReShade to create its own placeholder for the CUSTOMDEPTH semantic.
    g_width = 0;
    g_height = 0;
    if (create_or_resize_depth_resource(g_device, 1, 1, g_custom_depth, g_custom_depth_view, g_custom_depth_view_srgb))
    {
        g_width = 1;
        g_height = 1;
        g_runtime->update_texture_bindings("CUSTOMDEPTH", g_custom_depth_view, g_custom_depth_view);
    }

}

static void on_destroy_effect_runtime(effect_runtime *runtime)
{
    // Destroy any Vulkan-bound SRV (resource belongs to app/runtime, view belongs to us).
    if (g_runtime_depth_srv.handle != 0 && g_device)
    {
        g_device->destroy_resource_view(g_runtime_depth_srv);
        g_runtime_depth_srv = { 0 };
        g_runtime_depth_resource = { 0 };
    }

    // destroy resource views + resource
    if (g_custom_depth_view.handle) g_device->destroy_resource_view(g_custom_depth_view);
    if (g_custom_depth.handle) g_device->destroy_resource(g_custom_depth);

    g_custom_depth = {};
    g_custom_depth_view = {};

    // release leftover surface if any
    if (g_last_depth_surface)
    {
        g_last_depth_surface->Release();
        g_last_depth_surface = nullptr;
        g_pending_depth.store(false);
    }

    g_runtime = nullptr;
    g_device = nullptr;
    g_device_api = device_api::d3d9;
    g_seen_reload_settle.store(false, std::memory_order_relaxed);
    g_disable_beginpass_after_fault.store(false, std::memory_order_relaxed);
    g_prehud_locked_rt_resource = { 0 };
    g_prehud_locked_ds_resource = { 0 };
    g_prehud_lock_last_hit_frame.store(0, std::memory_order_relaxed);
    g_prehud_lock_miss_frames.store(0, std::memory_order_relaxed);
}

// Present hook: run ProcessPendingDepth early in frame so ReShade effects can use it
static void on_present(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*)
{
    const uint64_t frame = g_frame_index.fetch_add(1, std::memory_order_relaxed) + 1;
    // New frame: allow one manual pre-HUD effect pass again.
    g_pre_hud_effects_issued_this_frame.store(false);
    if (g_skip_manual_prehud_frames.load() > 0)
        g_skip_manual_prehud_frames.fetch_sub(1);

    if (!g_enabled_for_runtime)
        return;

    // Vulkan path: choose/bind once per frame (reduces flicker and avoids partial binds).
    if (g_device_api == device_api::vulkan)
    {
        // Unlock stale pre-HUD signature only after sustained full-frame misses.
        if (g_prehud_locked_rt_resource.handle != 0 && g_prehud_locked_ds_resource.handle != 0)
        {
            const uint64_t last_hit = g_prehud_lock_last_hit_frame.load(std::memory_order_relaxed);
            if (last_hit != 0 && frame > last_hit)
            {
                const uint64_t miss = frame - last_hit;
                g_prehud_lock_miss_frames.store(miss, std::memory_order_relaxed);
                if (miss > 90)
                {
                    g_prehud_locked_rt_resource = { 0 };
                    g_prehud_locked_ds_resource = { 0 };
                    g_prehud_lock_last_hit_frame.store(0, std::memory_order_relaxed);
                    g_prehud_lock_miss_frames.store(0, std::memory_order_relaxed);
                    log_info("NFSTweakBridge: Released stale pre-HUD lock after frame misses.\n");
                }
            }
        }
        bind_vulkan_candidate_if_good();
        return;
    }

    ProcessPendingDepth();

    // If resource was successfully copied, bind it to a semantic so FX can use it:
    if (g_runtime && g_custom_depth_view.handle)
    {
        // Make sure the semantic string matches the one used in your FX (e.g. "CUSTOMDEPTH")
        g_runtime->update_texture_bindings("CUSTOMDEPTH", g_custom_depth_view, g_custom_depth_view);
    }
}

// ---------- DllMain: register events ----------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        if (!reshade::register_addon(hModule))
            return FALSE;

        // Register lifecycle events
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_overlay_ui);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
        reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
        reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_depth_stencil_view);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reshade_reloaded_effects);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_event<reshade::addon_event::present>(on_present);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(on_overlay_ui);
        reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
        reshade::unregister_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
        reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_depth_stencil_view);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reshade_reloaded_effects);
        reshade::unregister_addon(hModule);
    }
    return TRUE;
}
