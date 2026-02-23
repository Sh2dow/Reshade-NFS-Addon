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
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_reshade_finish_effects);
        reshade::register_event<reshade::addon_event::draw>(on_draw_block_effects);
        reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed_block_effects);
        reshade::register_event<reshade::addon_event::dispatch>(on_dispatch_block_effects);
        reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect_block_effects);
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
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_reshade_finish_effects);
        reshade::unregister_event<reshade::addon_event::draw>(on_draw_block_effects);
        reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed_block_effects);
        reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch_block_effects);
        reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect_block_effects);
        reshade::unregister_addon(hModule);
    }
    return TRUE;
}
