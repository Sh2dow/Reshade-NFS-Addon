#include <windows.h>
#include <d3d9.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
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
using PFN_NFSTweak_RenderEffectsPreHudNow = void(__cdecl *)();
using PFN_NFSTweak_NotifyPrecipitationChanged = void(__cdecl *)(unsigned int value);

static PFN_NFSTweak_PushDepthSurface g_pfnPushDepthSurface = nullptr;
static PFN_NFSTweak_PushDepthBufferR32F g_pfnPushDepthBufferR32F = nullptr;
static PFN_NFSTweak_RequestPreHudEffects g_pfnRequestPreHudEffects = nullptr;
static PFN_NFSTweak_RenderEffectsPreHudNow g_pfnRenderEffectsPreHudNow = nullptr;
static PFN_NFSTweak_NotifyPrecipitationChanged g_pfnNotifyPrecipitationChanged = nullptr;

static std::atomic_uint64_t g_last_capture_qpc{0};
static std::atomic_uint64_t g_predisplay_call_count{0};
static std::atomic_uint64_t g_predisplay_zero_count{0};
static std::atomic_uint64_t g_predisplay_request_count{0};
static std::atomic_uint64_t g_blur_call_request_count{0};
static std::mutex g_capture_mutex;
static std::atomic_bool g_enable_capture{false};
static std::atomic_bool g_mw_precip_state_initialized{false};
static std::atomic_uint32_t g_mw_precip_signature_last{0};
static std::atomic_uint32_t g_mw_precip_signature_candidate{0};
static std::atomic_uint32_t g_mw_precip_signature_streak{0};
static std::atomic_uint64_t g_mw_precip_last_emit_call{0};

static IDirect3DSurface9 *g_sysmem_surface = nullptr;
static D3DFORMAT g_sysmem_format = D3DFMT_UNKNOWN;
static unsigned int g_sysmem_w = 0, g_sysmem_h = 0;

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
		g_pfnRenderEffectsPreHudNow = reinterpret_cast<PFN_NFSTweak_RenderEffectsPreHudNow>(GetProcAddress(h, "NFSTweak_RenderEffectsPreHudNow"));
		g_pfnNotifyPrecipitationChanged = reinterpret_cast<PFN_NFSTweak_NotifyPrecipitationChanged>(GetProcAddress(h, "NFSTweak_NotifyPrecipitationChanged"));
		return (g_pfnPushDepthBufferR32F || g_pfnPushDepthSurface || g_pfnRequestPreHudEffects || g_pfnRenderEffectsPreHudNow || g_pfnNotifyPrecipitationChanged);
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
			p = GetProcAddress(modules[i], "NFSTweak_RenderEffectsPreHudNow");
		if (!p)
			p = GetProcAddress(modules[i], "NFSTweak_NotifyPrecipitationChanged");
		if (!p)
			continue;

		g_pfnPushDepthBufferR32F = reinterpret_cast<PFN_NFSTweak_PushDepthBufferR32F>(GetProcAddress(modules[i], "NFSTweak_PushDepthBufferR32F"));
		g_pfnPushDepthSurface = reinterpret_cast<PFN_NFSTweak_PushDepthSurface>(GetProcAddress(modules[i], "NFSTweak_PushDepthSurface"));
		g_pfnRequestPreHudEffects = reinterpret_cast<PFN_NFSTweak_RequestPreHudEffects>(GetProcAddress(modules[i], "NFSTweak_RequestPreHudEffects"));
		g_pfnRenderEffectsPreHudNow = reinterpret_cast<PFN_NFSTweak_RenderEffectsPreHudNow>(GetProcAddress(modules[i], "NFSTweak_RenderEffectsPreHudNow"));
		g_pfnNotifyPrecipitationChanged = reinterpret_cast<PFN_NFSTweak_NotifyPrecipitationChanged>(GetProcAddress(modules[i], "NFSTweak_NotifyPrecipitationChanged"));
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

	// Bridge-owned precipitation state source (addon does not poll these addresses).
	// Use render-flag edge detection with asymmetric debounce:
	// fast ON detection, conservative OFF detection to avoid false unlocks.
	uint32_t cur_render = 0;
	cur_render = *reinterpret_cast<volatile uint32_t *>(PRECIPITATION_DEBUG_ADDR);

	const uint64_t call_now = g_predisplay_call_count.load(std::memory_order_relaxed);
	const uint32_t signature = (cur_render != 0) ? 0x02u : 0x00u;

	const uint64_t last_emit_call = g_mw_precip_last_emit_call.load(std::memory_order_relaxed);
	constexpr uint64_t k_emit_cooldown_calls = 45;
	const bool cooldown_ok = (call_now >= last_emit_call) && ((call_now - last_emit_call) >= k_emit_cooldown_calls);
	constexpr uint32_t k_streak_on_needed = 3;
	constexpr uint32_t k_streak_off_needed = 24;

	if (!g_mw_precip_state_initialized.load(std::memory_order_relaxed))
	{
		g_mw_precip_state_initialized.store(true, std::memory_order_relaxed);
		g_mw_precip_signature_last.store(signature, std::memory_order_relaxed);
		g_mw_precip_signature_candidate.store(signature, std::memory_order_relaxed);
		g_mw_precip_signature_streak.store(0, std::memory_order_relaxed);
		g_mw_precip_last_emit_call.store(call_now, std::memory_order_relaxed);
		g_pfnNotifyPrecipitationChanged(signature);
		return;
	}

	const uint32_t committed = g_mw_precip_signature_last.load(std::memory_order_relaxed);
	if (signature == committed)
	{
		g_mw_precip_signature_candidate.store(signature, std::memory_order_relaxed);
		g_mw_precip_signature_streak.store(0, std::memory_order_relaxed);
		return;
	}

	uint32_t candidate = g_mw_precip_signature_candidate.load(std::memory_order_relaxed);
	uint32_t streak = g_mw_precip_signature_streak.load(std::memory_order_relaxed);
	if (candidate != signature)
	{
		candidate = signature;
		streak = 1;
	}
	else
	{
		streak += 1;
	}
	g_mw_precip_signature_candidate.store(candidate, std::memory_order_relaxed);
	g_mw_precip_signature_streak.store(streak, std::memory_order_relaxed);

	const uint32_t needed = (signature != 0) ? k_streak_on_needed : k_streak_off_needed;
	if (streak >= needed && cooldown_ok)
	{
		g_mw_precip_signature_last.store(signature, std::memory_order_relaxed);
		g_mw_precip_signature_streak.store(0, std::memory_order_relaxed);
		g_mw_precip_last_emit_call.store(call_now, std::memory_order_relaxed);
		g_pfnNotifyPrecipitationChanged(signature);
	}
#endif
}

// The original FEManager_Render function pointer
void(__thiscall *FEManager_Render_orig)(unsigned int thisptr) = (void(__thiscall *)(unsigned int))FEMANAGER_RENDER_ADDRESS;
int(__cdecl *PreDisplay_Render_orig)(int a1) = (int(__cdecl *)(int))PREDISPLAY_RENDER_ADDRESS;
#if GAME_MW
static int(__cdecl *MW_BlurPass_orig)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) =
	(int(__cdecl *)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))0x006D3B80;
#endif

void __stdcall FEManager_Render_Hook()
{
	unsigned int thisptr = 0;
#ifndef _WIN64
	__asm { mov eax, ecx }
	__asm { mov thisptr, eax }
#endif

#if GAME_MW
	// Always try to resolve exports from the addon at this hook point.
	// This keeps pre-HUD signaling independent from depth capture path state.
	try_resolve_exports();

	// Request/execute pre-HUD effects every FE render tick.
	if (g_pfnRenderEffectsPreHudNow)
		g_pfnRenderEffectsPreHudNow();
	else if (g_pfnRequestPreHudEffects)
		g_pfnRequestPreHudEffects();

	IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)NFS_D3D9_DEVICE_ADDRESS;
	capture_and_push_depth(dev);
#endif

	FEManager_Render_orig(thisptr);
}

int __cdecl PreDisplay_Render_Hook(int a1)
{
#if GAME_MW
	g_predisplay_call_count.fetch_add(1);
	// IMPORTANT: sub_6E6E40(0) performs device SetRenderTarget/SetDepthStencilSurface calls
	// (IDA: call [ecx+94h] @ 0x6E6E6B, call [ecx+9Ch] @ 0x6E6E80).
	// So execute original first, then request pre-HUD so addon sees the updated RT/DS state.
	const int ret = PreDisplay_Render_orig(a1);
	if (a1 == 0)
	{
		g_predisplay_zero_count.fetch_add(1);
		__try
		{
			try_resolve_exports();
			pump_precipitation_signal_from_hooks();
			if (g_pfnRenderEffectsPreHudNow)
			{
				g_pfnRenderEffectsPreHudNow();
				g_predisplay_request_count.fetch_add(1);
			}
			else if (g_pfnRequestPreHudEffects)
			{
				g_pfnRequestPreHudEffects();
				g_predisplay_request_count.fetch_add(1);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			OutputDebugStringA("NFS_Addon_Bridge: PreDisplay_Render_Hook exception suppressed.\n");
		}
	}
	return ret;
#endif
	return PreDisplay_Render_orig(a1);
}

#if GAME_MW
int __cdecl MW_BlurPass_Hook(
	uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
	uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8)
{
	const int ret = MW_BlurPass_orig(a1, a2, a3, a4, a5, a6, a7, a8);

	// Anchor pre-HUD request only to strong blur variant (0x0E/0x0F).
	// The medium variant (8/7) tends to align with post-like phases and can contaminate HUD.
	const bool strong_blur_variant = (a6 == 0x0E && a7 == 0x0F);
	if (!strong_blur_variant)
		return ret;

	// Strong blur active: nudge pre-HUD request timing toward early scene phase.
	__try
	{
		try_resolve_exports();
		if (g_pfnRenderEffectsPreHudNow)
		{
			g_pfnRenderEffectsPreHudNow();
			g_blur_call_request_count.fetch_add(1, std::memory_order_relaxed);
		}
		else if (g_pfnRequestPreHudEffects)
		{
			g_pfnRequestPreHudEffects();
			g_blur_call_request_count.fetch_add(1, std::memory_order_relaxed);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		OutputDebugStringA("NFS_Addon_Bridge: MW_BlurPass_Hook exception suppressed.\n");
	}

	return ret;
}
#endif

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
				injector::MakeCALL(0x006DBE8B, MW_BlurPass_Hook, true);
				injector::MakeCALL(0x006DBEB0, MW_BlurPass_Hook, true);
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
