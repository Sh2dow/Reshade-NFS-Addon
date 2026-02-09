Below is the **fully detailed technical plan** for implementing the **complete NFS → ReShade Depth Bridge**, including:

* D3D9 reverse engineering points
* ReShade add-on API integration
* GPU → ReShade resource copying
* HUD-skip & FE-UI layering
* Multithreading considerations
* Tooling and debugging

This describes exactly how to go from a **D3D9 ASI hook** → **ReShade add-on** → **working depth-based effects in NFS MW/UG2/Carbon**.

---

# ✔ **NFS → ReShade Depth Bridge – Full Implementation Plan**

This is a **5-stage pipeline**.

---

# **STAGE 1 — Inside the ASI (D3D9 Hook Layer)**

Your ASI controls when depth is captured and has access to the real D3D9 runtime.

## **1. Locate the real depth surface**

NFS games typically use:

* `D3DFMT_D24S8` (Most Wanted, Carbon)
* `D3DFMT_D16` (some UG1/UG2 builds)
* Occasionally a clone render target for FE menus

Use hooks:

* `IDirect3DDevice9::SetDepthStencilSurface`
* `IDirect3DDevice9::Present`
* `FEManager_Render` (your existing hook)

Store the currently bound depth surface:

```cpp
device->GetDepthStencilSurface(&g_CurrentDepthSurface);
```

Also track:

* viewport
* resolution
* FE/HUD flags (optional)

---

## **2. Push depth to ReShade Add-on**

Call the exported function from add-on:

```cpp
g_NFSTweak_PushDepthSurface(pDepthSurface, width, height);
```

This must:

* AddRef the surface
* store width, height
* make atomic flag `pending = true`

➡ This is already implemented correctly on your side.

---

## **3. Optional: Pre-FE Rendering Injection**

To draw ReShade BEFORE NFS frontend:

Use your working hook:

```cpp
g_pd3dDevice->_implicit_swapchain->on_nfs_present();
FEManager_Render(TheThis);
```

This ensures:

* ReShade shows **behind FE** (menus & race HUD)
* But ReShade UI can be drawn **above** FE if requested with `runtime->open_overlay(true)` in depth add-on

---

# **STAGE 2 — ReShade Add-on: Receiving Depth**

Your add-on already has:

* thread-safe queue
* AddRef'd IDirect3DSurface9 pointer
* flags

Key improvements:

---

## **1. Remove the texture placeholder creation**

You currently do:

```cpp
desc.texture.width = 1920;
desc.texture.height = 1080;
```

We will instead:

🔥 **Create resource only when first depth arrives**
🔥 **Recreate resource only when resolution changes**

That is already in your `ProcessPendingDepth()` design.

---

## **2. Create ReShade texture + SRVs**

Use:

```cpp
bool create_resource(
    const resource_desc &,
    const subresource_data *,
    resource_usage,
    resource *out);
```

Then 2 SRVs:

```cpp
create_resource_view(resource, format::r32_float)
create_resource_view(resource, format::r32_float)  // SRGB (dummy)
```

---

# **STAGE 3 — GPU Depth Copy Implementation (Critical Part)**

This is the only missing block.

Depth is usually:

* D24S8 (unmappable)
* D16 (also unmappable)

We must:

### **OPTION A: GPU Copy Path (fastest)**

1. Create an R32F render target:

```cpp
device->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_R32F,D3DPOOL_DEFAULT,&tmpTex);
tmpTex->GetSurfaceLevel(0,&tmpSurf);
```

2. Use pixel shader to convert depth → linear depth
   This requires a small full-screen quad:

* Set tmpSurf as render target
* Bind depth surface for sampling (if depth is bound as texture)
  → NFS MW does **NOT** expose depth as a shader resource.
  → So **this method will NOT work**.

Thus we go to:

---

### **OPTION B: CPU Fallback (universal, works with NFS MW)**

This is 100% reliable.

1. Create system-memory surface:

```cpp
device->CreateOffscreenPlainSurface(w, h, convertFormat, D3DPOOL_SYSTEMMEM, &sys, NULL);
```

where:

* `convertFormat = D3DFMT_R32F` OR `D3DFMT_A8R8G8B8`

2. Copy depth → sysmem:

```cpp
device->GetRenderTargetData(g_last_depth_surface, sys);
```

This works even on D24S8, because D3D9 driver internally does depth → color conversion.

3. Lock sysmem:

```cpp
sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
```

4. Convert pixel data → float depth
   If the converted format is `A8R8G8B8`, normalize manually:

```cpp
float depth = ((pixel >> 16) & 0xFF) / 255.0f;
```

Best choice is `D3DFMT_R32F`, if driver supports it.

5. Upload to ReShade texture:

```cpp
reshade::api::subresource_data sub = {};
sub.data = depthFloatsPtr;
sub.row_pitch = row_pitch;
sub.slice_pitch = row_pitch * height;

device->update_texture_region(sub, g_custom_depth, 0, nullptr);
```

Quick validation: `shaders/ShowCustomDepth.fx` renders `CUSTOMDEPTH` as grayscale so you can verify the pipe end-to-end.

---

## **Why CPU fallback is required**

* NFS MW does NOT expose depth as a texture
* Cannot bind depth as pixel shader input
* No shared handles in D3D9
* D24S8 is untyped and cannot be directly copied to float texture

Every Depth3D/ASUS/Helix mod uses CPU fallback for D3D9 depth-based injection.

This is the only universally correct path.

---

# **STAGE 4 — Binding to ReShade Effects**

In `on_present()`:

```cpp
runtime->update_texture_bindings("CUSTOMDEPTH", g_custom_depth_view, g_custom_depth_view_srgb);
```

Your shader must contain:

```hlsl
texture DepthTex : CUSTOMDEPTH
<
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
>;
```

AND you should expose it as:

```hlsl
float LinearDepth = DepthTex.Sample(sampler, uv).r;
```

Now all ReShade effects read the injected depth.

---

# **DXVK Support (Important)**

If you run NFS MW through DXVK, the `IDirect3DSurface9` readback path (`GetRenderTargetData`) can stall or return invalid data.
Use the DXVK-safe CPU upload API exported by the add-on instead:

```cpp
// Exported by the add-on:
// void NFSTweak_PushDepthBufferR32F(const void* data, unsigned int width, unsigned int height, unsigned int row_pitch_bytes);
//
// 'data' is linear depth as float32 (R32F), one float per pixel.
NFSTweak_PushDepthBufferR32F(depth_ptr, width, height, row_pitch_bytes);
```

# **STAGE 5 — UI & Debug Tools**

## **1. Add ImGui Panel (proper method)**

Use:

```cpp
register_event<reshade_overlay>(on_overlay_ui);
```

This displays in the ReShade overlay.

---

## **2. Add Depth Debugging FX**

I will generate this file for you if you want:

* visualize depth grayscale
* visualize heatmap
* display near/far
* display max depth per frame

---

## **3. Add hotkeys**

Use ReShade’s input API or game hook keys.

---

# **STAGE 6 — Threading & Safety**

### **ASI Thread → Add-on Thread**

* ASI runs on game thread
* ReShade add-on runs in ReShade’s device/command context
* sync using mutex + atomic

### **Ensure surface lifetime**

* AddRef in ASI
* Release in add-on after finished copying

---

# **STAGE 7 — FE / HUD Layering**

MW’s FE/HUD overlay requires care:

### **To draw ReShade *behind* FE**

Use your existing hook calling:

```cpp
_implicit_swapchain->on_nfs_present();
```

### **To draw ReShade UI *above* FE**

Call once:

```cpp
runtime->open_overlay(true, input_source::keyboard);
```

### **To hide FE when needed**

Use known addresses to detect FE state (already in your mod).

---

# **STAGE 8 — Final Verification**

1. Depth debug view
2. Depth responds to near/far changes
3. ReShade UI depth toggle works
4. Race HUD does not block ReShade
5. FE menus still show correctly

---

# **STAGE 9 — Optional GPU Acceleration (Later)**

If you later add **fake depth exporter** into MW’s shaders (via ASM patch):

* export depth into color target
* then use GPU-only path
* remove CPU fallback
* huge performance boost

But not needed for functioning version.

---
