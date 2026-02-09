#include <windows.h>
#include <d3d9.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
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

static PFN_NFSTweak_PushDepthSurface g_pfnPushDepthSurface = nullptr;
static PFN_NFSTweak_PushDepthBufferR32F g_pfnPushDepthBufferR32F = nullptr;

static std::atomic_uint64_t g_last_capture_qpc{0};
static std::mutex g_capture_mutex;
static std::atomic_bool g_enable_capture{false};

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
		return (g_pfnPushDepthBufferR32F || g_pfnPushDepthSurface);
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
			continue;

		g_pfnPushDepthBufferR32F = reinterpret_cast<PFN_NFSTweak_PushDepthBufferR32F>(GetProcAddress(modules[i], "NFSTweak_PushDepthBufferR32F"));
		g_pfnPushDepthSurface = reinterpret_cast<PFN_NFSTweak_PushDepthSurface>(GetProcAddress(modules[i], "NFSTweak_PushDepthSurface"));
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

// The original FEManager_Render function pointer
void(__thiscall *FEManager_Render_orig)(unsigned int thisptr) = (void(__thiscall *)(unsigned int))FEMANAGER_RENDER_ADDRESS;

void __stdcall FEManager_Render_Hook()
{
	unsigned int thisptr = 0;
#ifndef _WIN64
	__asm { mov eax, ecx }
	__asm { mov thisptr, eax }
#endif

#if GAME_MW
	IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)NFS_D3D9_DEVICE_ADDRESS;
	capture_and_push_depth(dev);
#endif

	FEManager_Render_orig(thisptr);
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
				injector::MakeCALL(FEMANAGER_RENDER_HOOKADDR1, FEManager_Render_Hook, true);
				injector::MakeCALL(FEMANAGER_RENDER_HOOKADDR2, FEManager_Render_Hook, true);
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
				OutputDebugStringA("NFS_Addon_Bridge: Hooks installed.\n");
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
