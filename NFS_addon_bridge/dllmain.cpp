#include <windows.h>
#include <d3d9.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <vector>

#include <injector.hpp>

// Used to get this DLL module handle without depending on DllMain parameter plumbing.
extern "C" IMAGE_DOS_HEADER __ImageBase;

#if GAME_MW
#include "../includes/NFSMW_PreFEngHook.h"
#endif

// The bridge is the "producer":
// It captures depth (best-effort) and forwards it to the ReShade add-on ("consumer") via exported functions.

using PFN_NFSTweak_PushDepthSurface = void(__cdecl *)(void *d3d9_surface, unsigned int width, unsigned int height);
using PFN_NFSTweak_PushDepthBufferR32F = void(__cdecl *)(const void *data, unsigned int width, unsigned int height, unsigned int row_pitch_bytes);
using PFN_NFSTweak_RequestPreHudEffects = void(__cdecl *)();
using PFN_NFSTweak_BeginPreHudWindow = void(__cdecl *)(unsigned int token);
using PFN_NFSTweak_EndPreHudWindow = void(__cdecl *)(unsigned int token);
using PFN_NFSTweak_BeginPreHudWindowEx = void(__cdecl *)(unsigned int token, unsigned int epoch);
using PFN_NFSTweak_EndPreHudWindowEx = void(__cdecl *)(unsigned int token, unsigned int epoch);
using PFN_NFSTweak_NotifyPrecipitationChanged = void(__cdecl *)(unsigned int value);
using PFN_NFSTweak_NotifyPhaseInvalidate = void(__cdecl *)(unsigned int reason);
using PFN_NFSTweak_NotifyPhaseInvalidateEx = void(__cdecl *)(unsigned int reason, unsigned int epoch);

static PFN_NFSTweak_PushDepthSurface g_pfnPushDepthSurface = nullptr;
static PFN_NFSTweak_PushDepthBufferR32F g_pfnPushDepthBufferR32F = nullptr;
static PFN_NFSTweak_RequestPreHudEffects g_pfnRequestPreHudEffects = nullptr;
static PFN_NFSTweak_BeginPreHudWindow g_pfnBeginPreHudWindow = nullptr;
static PFN_NFSTweak_EndPreHudWindow g_pfnEndPreHudWindow = nullptr;
static PFN_NFSTweak_BeginPreHudWindowEx g_pfnBeginPreHudWindowEx = nullptr;
static PFN_NFSTweak_EndPreHudWindowEx g_pfnEndPreHudWindowEx = nullptr;
static PFN_NFSTweak_NotifyPrecipitationChanged g_pfnNotifyPrecipitationChanged = nullptr;
static PFN_NFSTweak_NotifyPhaseInvalidate g_pfnNotifyPhaseInvalidate = nullptr;
static PFN_NFSTweak_NotifyPhaseInvalidateEx g_pfnNotifyPhaseInvalidateEx = nullptr;

static std::atomic_uint64_t g_last_capture_qpc{0};
static std::atomic_uint64_t g_predisplay_call_count{0};
static std::atomic_uint64_t g_predisplay_zero_count{0};
static std::atomic_uint64_t g_predisplay_request_count{0};
static std::atomic_uint64_t g_last_zero_predisplay_call{0};
static std::atomic_uint64_t g_last_fallback_token_call{0};
static std::atomic_uint32_t g_scene_token_counter{0};
static std::atomic_uint32_t g_scene_token_active{0};
static std::atomic_uint32_t g_bridge_phase_epoch{1};
static std::atomic_bool g_precip_state_initialized{false};
static std::atomic_uint32_t g_precip_signature_last{0};
static std::atomic_uint32_t g_precip_signature_candidate{0};
static std::atomic_uint32_t g_precip_signature_streak{0};
static std::atomic_bool g_overlay_state_initialized{false};
static std::atomic_uint32_t g_overlay_state_last{0};
static std::atomic_uint64_t g_scene_pair_skip_count{0};
static std::atomic_uint64_t g_prehud_token_emit_count{0};
static std::atomic_uint64_t g_last_token_emit_qpc{0};
static std::mutex g_capture_mutex;
static std::atomic_bool g_enable_capture{false};

static IDirect3DSurface9 *g_sysmem_surface = nullptr;
static D3DFORMAT g_sysmem_format = D3DFMT_UNKNOWN;
static unsigned int g_sysmem_w = 0, g_sysmem_h = 0;

static uint32_t read_overlay_state_flag()
{
#if GAME_MW
	constexpr uintptr_t k_draw_feng_bool_addr = 0x008F374C;
	__try
	{
		return (*reinterpret_cast<volatile uint32_t *>(k_draw_feng_bool_addr) != 0) ? 1u : 0u;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0u;
	}
#else
	return 0u;
#endif
}

static bool try_resolve_exports();

static void *read_ptr_safe(uintptr_t addr)
{
	__try
	{
		return *reinterpret_cast<void **>(addr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static bool is_canonical_scene_pair_bound(IDirect3DDevice9 *dev)
{
#if GAME_MW
	if (dev == nullptr)
		return false;

	// IDA-validated canonical scene surfaces used by sub_6E6E40/sub_6D0EA0/sub_6DB011/sub_6DC232:
	// SetRenderTarget(0, dword_93DAC0) + SetDepthStencilSurface(dword_93DAC4).
	constexpr uintptr_t k_scene_rt_addr = 0x0093DAC0;
	constexpr uintptr_t k_scene_ds_addr = 0x0093DAC4;
	IDirect3DSurface9 *const expected_rt = reinterpret_cast<IDirect3DSurface9 *>(read_ptr_safe(k_scene_rt_addr));
	IDirect3DSurface9 *const expected_ds = reinterpret_cast<IDirect3DSurface9 *>(read_ptr_safe(k_scene_ds_addr));
	if (expected_rt == nullptr || expected_ds == nullptr)
		return false;

	IDirect3DSurface9 *cur_rt = nullptr;
	IDirect3DSurface9 *cur_ds = nullptr;
	const HRESULT hr_rt = dev->GetRenderTarget(0, &cur_rt);
	const HRESULT hr_ds = dev->GetDepthStencilSurface(&cur_ds);
	const bool ok = SUCCEEDED(hr_rt) && SUCCEEDED(hr_ds) && cur_rt == expected_rt && cur_ds == expected_ds;
	if (cur_rt)
		cur_rt->Release();
	if (cur_ds)
		cur_ds->Release();
	return ok;
#else
	(void)dev;
	return true;
#endif
}

static bool emit_prehud_token_window(const char *origin_tag, bool require_canonical_scene_pair)
{
	if (!try_resolve_exports())
		return false;

	// Dedupe burst emits from the same frame/phase path (can happen around FE transitions).
	// Keep nominal 60-144 FPS unaffected while blocking sub-millisecond duplicates.
	LARGE_INTEGER freq = {}, now = {};
	if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&now) && freq.QuadPart > 0)
	{
		const uint64_t now_qpc = static_cast<uint64_t>(now.QuadPart);
		const uint64_t prev_qpc = g_last_token_emit_qpc.load(std::memory_order_relaxed);
		const uint64_t min_delta = static_cast<uint64_t>(freq.QuadPart / 220); // ~4.5ms
		if (prev_qpc != 0 && now_qpc > prev_qpc && (now_qpc - prev_qpc) < min_delta)
			return false;
		g_last_token_emit_qpc.store(now_qpc, std::memory_order_relaxed);
	}

	IDirect3DDevice9 *dev = *reinterpret_cast<IDirect3DDevice9 **>(NFS_D3D9_DEVICE_ADDRESS);
	if (require_canonical_scene_pair && !is_canonical_scene_pair_bound(dev))
	{
		const uint64_t skips = g_scene_pair_skip_count.fetch_add(1, std::memory_order_relaxed) + 1;
		if (skips <= 5 || (skips % 120) == 0)
		{
			char msg[192] = {};
			sprintf_s(msg, "NFS_Addon_Bridge: Skip token (%s): canonical scene RT/DS not bound.\n", origin_tag ? origin_tag : "?");
			OutputDebugStringA(msg);
		}
		return false;
	}

	const uint32_t token = g_scene_token_counter.fetch_add(1, std::memory_order_relaxed) + 1;
	const uint32_t epoch = g_bridge_phase_epoch.load(std::memory_order_relaxed);
	g_scene_token_active.store(token, std::memory_order_relaxed);
	if (g_pfnBeginPreHudWindowEx)
		g_pfnBeginPreHudWindowEx(token, epoch);
	else if (g_pfnBeginPreHudWindow)
		g_pfnBeginPreHudWindow(token);
	if (g_pfnRequestPreHudEffects)
	{
		g_pfnRequestPreHudEffects();
		g_predisplay_request_count.fetch_add(1, std::memory_order_relaxed);
	}
	if (g_pfnEndPreHudWindowEx)
		g_pfnEndPreHudWindowEx(token, epoch);
	else if (g_pfnEndPreHudWindow)
		g_pfnEndPreHudWindow(token);
	g_scene_token_active.store(0, std::memory_order_relaxed);
	const uint64_t emits = g_prehud_token_emit_count.fetch_add(1, std::memory_order_relaxed) + 1;
	if (emits <= 5 || (emits % 120) == 0)
	{
		char msg[192] = {};
		sprintf_s(msg, "NFS_Addon_Bridge: Emitted pre-HUD token (src=%s token=%u emits=%llu).\n",
			origin_tag ? origin_tag : "?", token, static_cast<unsigned long long>(emits));
		OutputDebugStringA(msg);
	}
	return true;
}

static bool try_resolve_exports()
{
	if (g_pfnPushDepthBufferR32F || g_pfnPushDepthSurface)
		return true;

	// Prefer the add-on module name. If users rename it, fall back to scanning all modules.
	HMODULE h = GetModuleHandleA("nfs_addon.addon32");
	if (h)
	{
		g_pfnPushDepthBufferR32F = reinterpret_cast<PFN_NFSTweak_PushDepthBufferR32F>(GetProcAddress(h, "NFSTweak_PushDepthBufferR32F"));
		g_pfnPushDepthSurface = reinterpret_cast<PFN_NFSTweak_PushDepthSurface>(GetProcAddress(h, "NFSTweak_PushDepthSurface"));
		g_pfnRequestPreHudEffects = reinterpret_cast<PFN_NFSTweak_RequestPreHudEffects>(GetProcAddress(h, "NFSTweak_RequestPreHudEffects"));
		g_pfnBeginPreHudWindow = reinterpret_cast<PFN_NFSTweak_BeginPreHudWindow>(GetProcAddress(h, "NFSTweak_BeginPreHudWindow"));
		g_pfnEndPreHudWindow = reinterpret_cast<PFN_NFSTweak_EndPreHudWindow>(GetProcAddress(h, "NFSTweak_EndPreHudWindow"));
		g_pfnBeginPreHudWindowEx = reinterpret_cast<PFN_NFSTweak_BeginPreHudWindowEx>(GetProcAddress(h, "NFSTweak_BeginPreHudWindowEx"));
		g_pfnEndPreHudWindowEx = reinterpret_cast<PFN_NFSTweak_EndPreHudWindowEx>(GetProcAddress(h, "NFSTweak_EndPreHudWindowEx"));
		g_pfnNotifyPrecipitationChanged = reinterpret_cast<PFN_NFSTweak_NotifyPrecipitationChanged>(GetProcAddress(h, "NFSTweak_NotifyPrecipitationChanged"));
		g_pfnNotifyPhaseInvalidate = reinterpret_cast<PFN_NFSTweak_NotifyPhaseInvalidate>(GetProcAddress(h, "NFSTweak_NotifyPhaseInvalidate"));
		g_pfnNotifyPhaseInvalidateEx = reinterpret_cast<PFN_NFSTweak_NotifyPhaseInvalidateEx>(GetProcAddress(h, "NFSTweak_NotifyPhaseInvalidateEx"));
		return (g_pfnPushDepthBufferR32F || g_pfnPushDepthSurface || g_pfnRequestPreHudEffects || g_pfnBeginPreHudWindow || g_pfnEndPreHudWindow || g_pfnBeginPreHudWindowEx || g_pfnEndPreHudWindowEx || g_pfnNotifyPrecipitationChanged || g_pfnNotifyPhaseInvalidate || g_pfnNotifyPhaseInvalidateEx);
	}

	// Scan modules (robust against renamed addon file)
	HMODULE modules[1024];
	DWORD bytes = 0;
	if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
		return false;

	const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), ARRAYSIZE(modules));
	for (DWORD i = 0; i < count; ++i)
	{
		FARPROC p = GetProcAddress(modules[i], "NFSTweak_PushDepthBufferR32F");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_PushDepthSurface");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_RequestPreHudEffects");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_BeginPreHudWindow");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_EndPreHudWindow");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_NotifyPrecipitationChanged");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_NotifyPhaseInvalidate");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_BeginPreHudWindowEx");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_EndPreHudWindowEx");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_NotifyPhaseInvalidateEx");
		if (!p)
			continue;

		g_pfnPushDepthBufferR32F = reinterpret_cast<PFN_NFSTweak_PushDepthBufferR32F>(GetProcAddress(modules[i], "NFSTweak_PushDepthBufferR32F"));
		g_pfnPushDepthSurface = reinterpret_cast<PFN_NFSTweak_PushDepthSurface>(GetProcAddress(modules[i], "NFSTweak_PushDepthSurface"));
		g_pfnRequestPreHudEffects = reinterpret_cast<PFN_NFSTweak_RequestPreHudEffects>(GetProcAddress(modules[i], "NFSTweak_RequestPreHudEffects"));
		g_pfnBeginPreHudWindow = reinterpret_cast<PFN_NFSTweak_BeginPreHudWindow>(GetProcAddress(modules[i], "NFSTweak_BeginPreHudWindow"));
		g_pfnEndPreHudWindow = reinterpret_cast<PFN_NFSTweak_EndPreHudWindow>(GetProcAddress(modules[i], "NFSTweak_EndPreHudWindow"));
		g_pfnBeginPreHudWindowEx = reinterpret_cast<PFN_NFSTweak_BeginPreHudWindowEx>(GetProcAddress(modules[i], "NFSTweak_BeginPreHudWindowEx"));
		g_pfnEndPreHudWindowEx = reinterpret_cast<PFN_NFSTweak_EndPreHudWindowEx>(GetProcAddress(modules[i], "NFSTweak_EndPreHudWindowEx"));
		g_pfnNotifyPrecipitationChanged = reinterpret_cast<PFN_NFSTweak_NotifyPrecipitationChanged>(GetProcAddress(modules[i], "NFSTweak_NotifyPrecipitationChanged"));
		g_pfnNotifyPhaseInvalidate = reinterpret_cast<PFN_NFSTweak_NotifyPhaseInvalidate>(GetProcAddress(modules[i], "NFSTweak_NotifyPhaseInvalidate"));
		g_pfnNotifyPhaseInvalidateEx = reinterpret_cast<PFN_NFSTweak_NotifyPhaseInvalidateEx>(GetProcAddress(modules[i], "NFSTweak_NotifyPhaseInvalidateEx"));
		return true;
	}

	return false;
}

static bool throttle_capture(uint32_t hz)
{
	LARGE_INTEGER freq = {}, now = {};
	if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&now) || freq.QuadPart == 0)
		return true; // if QPC is unavailable, don't block

	const uint64_t now_qpc = static_cast<uint64_t>(now.QuadPart);
	const uint64_t prev = g_last_capture_qpc.load(std::memory_order_relaxed);
	const uint64_t min_delta = static_cast<uint64_t>(freq.QuadPart / hz);
	if (prev != 0 && (now_qpc - prev) < min_delta)
		return false;

	g_last_capture_qpc.store(now_qpc, std::memory_order_relaxed);
	return true;
}

static bool ensure_sysmem_surface(IDirect3DDevice9 *dev, unsigned int w, unsigned int h)
{
	if (g_sysmem_surface && w == g_sysmem_w && h == g_sysmem_h && g_sysmem_format != D3DFMT_UNKNOWN)
		return true;

	if (g_sysmem_surface)
	{
		g_sysmem_surface->Release();
		g_sysmem_surface = nullptr;
	}

	// Prefer R32F, fall back to A8R8G8B8
	g_sysmem_format = D3DFMT_R32F;
	HRESULT hr = dev->CreateOffscreenPlainSurface(w, h, g_sysmem_format, D3DPOOL_SYSTEMMEM, &g_sysmem_surface, nullptr);
	if (FAILED(hr))
	{
		g_sysmem_format = D3DFMT_A8R8G8B8;
		hr = dev->CreateOffscreenPlainSurface(w, h, g_sysmem_format, D3DPOOL_SYSTEMMEM, &g_sysmem_surface, nullptr);
		if (FAILED(hr))
		{
			g_sysmem_format = D3DFMT_UNKNOWN;
			g_sysmem_surface = nullptr;
			return false;
		}
	}

	g_sysmem_w = w;
	g_sysmem_h = h;
	return true;
}

static void capture_and_push_depth(IDirect3DDevice9 *dev)
{
	if (!dev)
		return;
	if (!try_resolve_exports())
		return;

	// Toggle capture with F10 (edge triggered)
	static SHORT prev = 0;
	const SHORT cur = GetAsyncKeyState(VK_F10);
	if ((cur & 0x1) != 0 && (prev & 0x1) == 0)
	{
		const bool next = !g_enable_capture.load(std::memory_order_relaxed);
		g_enable_capture.store(next, std::memory_order_relaxed);
		OutputDebugStringA(next ? "NFS_Addon_Bridge: Depth capture enabled (F10)\n"
		                        : "NFS_Addon_Bridge: Depth capture disabled (F10)\n");
	}
	prev = cur;

	if (!g_enable_capture.load(std::memory_order_relaxed))
		return;

	// Keep this conservative to avoid stalls on DXVK.
	if (!throttle_capture(10))
		return;

	IDirect3DSurface9 *depth_surface = nullptr;
	if (FAILED(dev->GetDepthStencilSurface(&depth_surface)) || !depth_surface)
		return;

	D3DSURFACE_DESC desc = {};
	if (FAILED(depth_surface->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
	{
		depth_surface->Release();
		return;
	}

	// Best-effort: try to read back via GetRenderTargetData into a sysmem surface.
	// This is not guaranteed for real depth-stencil surfaces, but on DXVK this is often the only practical path.
	std::lock_guard<std::mutex> lock(g_capture_mutex);
	if (!ensure_sysmem_surface(dev, desc.Width, desc.Height))
	{
		depth_surface->Release();
		return;
	}

	HRESULT hr = dev->GetRenderTargetData(depth_surface, g_sysmem_surface);
	depth_surface->Release();
	if (FAILED(hr))
		return;

	D3DLOCKED_RECT lr = {};
	hr = g_sysmem_surface->LockRect(&lr, nullptr, D3DLOCK_READONLY);
	if (FAILED(hr) || !lr.pBits)
	{
		g_sysmem_surface->UnlockRect();
		return;
	}

	// Convert to tight R32F buffer and push.
	std::vector<float> depth(static_cast<size_t>(desc.Width) * static_cast<size_t>(desc.Height));

	if (g_sysmem_format == D3DFMT_R32F)
	{
		for (UINT y = 0; y < desc.Height; ++y)
		{
			const auto *src = reinterpret_cast<const float *>(reinterpret_cast<const uint8_t *>(lr.pBits) + y * lr.Pitch);
			memcpy(depth.data() + static_cast<size_t>(y) * desc.Width, src, static_cast<size_t>(desc.Width) * sizeof(float));
		}
	}
	else if (g_sysmem_format == D3DFMT_A8R8G8B8)
	{
		for (UINT y = 0; y < desc.Height; ++y)
		{
			const auto *row = reinterpret_cast<const uint32_t *>(reinterpret_cast<const uint8_t *>(lr.pBits) + y * lr.Pitch);
			for (UINT x = 0; x < desc.Width; ++x)
			{
				const uint32_t pixel = row[x];
				depth[static_cast<size_t>(y) * desc.Width + x] = static_cast<float>(((pixel >> 16) & 0xFF) / 255.0f);
			}
		}
	}

	g_sysmem_surface->UnlockRect();

	if (g_pfnPushDepthBufferR32F)
	{
		g_pfnPushDepthBufferR32F(depth.data(), desc.Width, desc.Height, desc.Width * sizeof(float));
	}
	else if (g_pfnPushDepthSurface)
	{
		// Fallback: no CPU-buffer export available, push the surface itself.
		// This may stall in the add-on under DXVK, but keeps compatibility.
		// Note: Need a valid surface again, so just skip in this mode.
	}
}

static void pump_precipitation_signal_from_hooks()
{
#if GAME_MW
	if (!try_resolve_exports() || g_pfnNotifyPrecipitationChanged == nullptr)
		return;

	const uint32_t cur = (*reinterpret_cast<volatile uint32_t *>(PRECIPITATION_DEBUG_ADDR) != 0) ? 0x02u : 0x00u;
	if (!g_precip_state_initialized.load(std::memory_order_relaxed))
	{
		g_precip_state_initialized.store(true, std::memory_order_relaxed);
		g_precip_signature_last.store(cur, std::memory_order_relaxed);
		g_precip_signature_candidate.store(cur, std::memory_order_relaxed);
		g_precip_signature_streak.store(0, std::memory_order_relaxed);
		g_pfnNotifyPrecipitationChanged(cur);
		return;
	}

	const uint32_t committed = g_precip_signature_last.load(std::memory_order_relaxed);
	if (cur == committed)
	{
		g_precip_signature_candidate.store(cur, std::memory_order_relaxed);
		g_precip_signature_streak.store(0, std::memory_order_relaxed);
		return;
	}

	uint32_t candidate = g_precip_signature_candidate.load(std::memory_order_relaxed);
	uint32_t streak = g_precip_signature_streak.load(std::memory_order_relaxed);
	if (candidate != cur)
	{
		candidate = cur;
		streak = 1;
	}
	else
	{
		++streak;
	}
	g_precip_signature_candidate.store(candidate, std::memory_order_relaxed);
	g_precip_signature_streak.store(streak, std::memory_order_relaxed);

	const uint32_t needed = (cur != 0) ? 3u : 18u;
	if (streak >= needed)
	{
		g_precip_signature_last.store(cur, std::memory_order_relaxed);
		g_precip_signature_streak.store(0, std::memory_order_relaxed);
		g_pfnNotifyPrecipitationChanged(cur);
	}
#endif
}

static void pump_overlay_invalidate_signal()
{
#if GAME_MW
	if (!try_resolve_exports())
		return;

	const uint32_t cur = read_overlay_state_flag();
	if (!g_overlay_state_initialized.load(std::memory_order_relaxed))
	{
		g_overlay_state_initialized.store(true, std::memory_order_relaxed);
		g_overlay_state_last.store(cur, std::memory_order_relaxed);
		return;
	}

	const uint32_t prev = g_overlay_state_last.load(std::memory_order_relaxed);
	if (cur == prev)
		return;

	g_overlay_state_last.store(cur, std::memory_order_relaxed);
	// reason: 1=overlay_enter, 2=overlay_exit
	const uint32_t next_epoch = g_bridge_phase_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
	if (g_pfnNotifyPhaseInvalidateEx)
		g_pfnNotifyPhaseInvalidateEx(cur ? 1u : 2u, next_epoch);
	else if (g_pfnNotifyPhaseInvalidate)
		g_pfnNotifyPhaseInvalidate(cur ? 1u : 2u);
#endif
}

// The original FEManager_Render function pointer
void(__thiscall *FEManager_Render_orig)(unsigned int thisptr) = (void(__thiscall *)(unsigned int))FEMANAGER_RENDER_ADDRESS;
int(__cdecl *PreDisplay_Render_orig)(int a1) = (int(__cdecl *)(int))PREDISPLAY_RENDER_ADDRESS;
static int(__thiscall *MW_Sub516F70_orig)(void *self) = (int(__thiscall *)(void *))0x00516F70;

int __fastcall MW_Sub516F70_Hook(void *self, void *)
{
#if GAME_MW
	(void)self;
	__try
	{
		// Deterministic pre-HUD request point from IDA:
		// sub_6E6E40 + 0x390 calls sub_516F70 right before FE/HUD rendering.
		emit_prehud_token_window("sub_516F70", true);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		OutputDebugStringA("NFS_Addon_Bridge: MW_Sub516F70_Hook token emit exception suppressed.\n");
	}
#endif
	return MW_Sub516F70_orig(self);
}

void __stdcall FEManager_Render_Hook()
{
	unsigned int thisptr = 0;
#ifndef _WIN64
	__asm { mov eax, ecx }
	__asm { mov thisptr, eax }
#endif

#if GAME_MW
	// Always try to resolve exports from the addon at this hook point.
	// This keeps bridge->addon communication available for depth/capture paths.
	try_resolve_exports();

	IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)NFS_D3D9_DEVICE_ADDRESS;
	capture_and_push_depth(dev);
#endif

	FEManager_Render_orig(thisptr);
}

int __cdecl PreDisplay_Render_Hook(int a1)
{
#if GAME_MW
	const uint64_t call_now = g_predisplay_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
	pump_precipitation_signal_from_hooks();
	pump_overlay_invalidate_signal();
	const int ret = PreDisplay_Render_orig(a1);
	// Trigger exactly on sub_6E6E40(0), which is the second display-phase call in eDisplayFrame.
	// This is a stronger pre-HUD boundary than FEManager::Render helper internals.
	if (a1 == 0)
	{
		g_predisplay_zero_count.fetch_add(1);
		g_last_zero_predisplay_call.store(call_now, std::memory_order_relaxed);
		// Deterministic mode: token emission is anchored at sub_516F70 hook point.
	}
	else
	{
		// Deterministic mode: no fallback token emission from non-canonical display phases.
		// Tokens are emitted only from a1==0 when canonical scene RT/DS is bound.
	}
	return ret;
#endif
	return PreDisplay_Render_orig(a1);
}

// Provide a stub for optional hook symbol when headers declare it but no TU defines it in this build.
#if defined(HAS_COPS) && !defined(GAME_UC)
void __stdcall FECareerRecord_AdjustHeatOnEventWin_Hook() {}
#endif

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls(hModule);

		// Do not install hooks from inside DllMain (loader-lock sensitive; can hang with DXVK).
		// Defer hook installation to a background thread.
		auto init_thread = [](LPVOID) -> DWORD {
			Sleep(2000); // let the game/DXVK initialize

			__try
			{
#ifdef NFS_MULTITHREAD
				// Left as-is if you rely on these elsewhere.
				injector::MakeJMP(FEMANAGER_RENDER_HOOKADDR1, ReShade_EntryPoint, true);
				injector::MakeCALL(MAINSERVICE_HOOK_ADDR, MainService_Hook, true);
#else
				injector::MakeCALL(PREDISPLAY_HOOKADDR1, PreDisplay_Render_Hook, true);
				injector::MakeCALL(PREDISPLAY_HOOKADDR2, PreDisplay_Render_Hook, true);
#ifdef GAME_MW
				// End pre-HUD token window exactly at FE/HUD callsite in eDisplayFrame.
				injector::MakeCALL(0x006E71D0, MW_Sub516F70_Hook, true);
#endif
#endif

#ifdef GAME_MW
				injector::MakeNOP(GAMEFLOW_UNLOADTRACK_FIX, 5, true);
#endif
#ifdef GAME_CARBON
				injector::MakeCALL(INFINITENOS_HOOK, EasterEggCheck_Hook, true);
#endif
#ifdef GAME_PS
				injector::MakeJMP(AICONTROL_CAVE_ADDR, ToggleAIControlCave, true);
				injector::MakeJMP(INFINITENOS_CAVE_ADDR, InfiniteNOSCave, true);
				injector::MakeJMP(GAMESPEED_CAVE_ADDR, GameSpeedCave, true);
				injector::MakeJMP(DRAWWORLD_CAVE_ADDR, DrawWorldCave, true);
				injector::WriteMemory<char>(SKIPFE_PLAYERCAR_DEHARDCODE_PATCH_ADDR, 0xA1, true);
				injector::WriteMemory<int>(SKIPFE_PLAYERCAR_DEHARDCODE_PATCH_ADDR + 1, SKIPFE_PLAYERCAR_ADDR, true);
#endif
#ifdef GAME_UC
				injector::MakeJMP(NFSUC_MOTIONBLUR_HOOK_ADDR, MotionBlur_EntryPoint, true);
				injector::MakeJMP(INFINITENOS_CAVE_ADDR, InfiniteNOSCave, true);
				injector::MakeJMP(AICONTROL_CAVE_ADDR, ToggleAIControlCave, true);
#endif
#ifdef GAME_UG2
				injector::MakeCALL(SETRAIN_HOOK_ADDR, SetRainBase_Custom, true);
#endif
#ifdef HAS_COPS
#ifndef GAME_UC
				injector::MakeCALL(HEATONEVENTWIN_HOOK_ADDR, FECareerRecord_AdjustHeatOnEventWin_Hook, true);
#endif
#endif
				OutputDebugStringA("NFS_Addon_Bridge: Hooks installed (eDisplayFrame pre-HUD).\n");
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				OutputDebugStringA("NFS_Addon_Bridge: Hook install crashed; bridge disabled.\n");
			}

			return 0;
		};

		HANDLE hThread = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
		if (hThread)
			CloseHandle(hThread);
		break;
	}

	case DLL_PROCESS_DETACH:
		if (g_sysmem_surface)
		{
			g_sysmem_surface->Release();
			g_sysmem_surface = nullptr;
		}
		break;
	}

	return TRUE;
}
