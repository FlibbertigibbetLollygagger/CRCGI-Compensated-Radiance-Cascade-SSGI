// ============================================================================
// CRCGI Injector - ReShade add-on
//
// Hands the CRCGI effect what the game knows, and lets it ignore what is drawn after the 3D scene is finished.
//   Level A  nothing: the effect runs at the end of the frame like any ReShade effect.
//   Level B  A plus the camera's field of view read from the game (and, with "Force render" on, the effect rendered inside the frame).
//   Level C  B plus feeds taken from the game's own render targets: normals, albedo, motion vectors, HDR scene light, and the finished
//            frame (a copy of the image where the game's 3D rendering is done: the effect works on it and leaves UI, fog and
//            everything drawn after it untouched).
//
// A "layer" is one render-target binding interval of the frame, listed in the order seen (only layers drawn every frame by default).
// A feed is taken from a layer or from a texture picked in the profiler. Everything is saved per game next to the executable, and the
// target listing can be exported and shared (Options).
// Feeds and the profiler: Direct3D 10 / 11 / 12, Vulkan and OpenGL. Forced in-frame rendering (advanced): Direct3D 10 / 11 and
// Direct3D 12 with the add-on's own state tracker (experimental).
// ============================================================================
#include "ui.hpp"

extern "C" __declspec(dllexport) const char *NAME = "CRCGI Injector";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Gives CRCGI what the game knows (camera, normals, albedo, motion vectors, HDR light) and lets it ignore the UI and fog drawn after the 3D scene.";

static bool s_registered = false;

static void unregister_all()
{
    reshade::unregister_overlay("CRCGI Injector", draw_overlay);
    reshade::unregister_event<addon_event::reshade_present>(on_reshade_present);
    reshade::unregister_event<addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::unregister_event<addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::unregister_event<addon_event::reshade_reloaded_effects>(on_reloaded_effects);
    reshade::unregister_event<addon_event::destroy_effect_runtime>(on_destroy_runtime);
    reshade::unregister_event<addon_event::init_effect_runtime>(on_init_runtime);
    reshade::unregister_event<addon_event::unmap_buffer_region>(on_unmap_buffer_region);
    reshade::unregister_event<addon_event::map_buffer_region>(on_map_buffer_region);
    reshade::unregister_event<addon_event::update_buffer_region>(on_update_buffer_region);
    reshade::unregister_event<addon_event::clear_render_target_view>(on_clear_render_target_view);
    reshade::unregister_event<addon_event::dispatch>(on_dispatch);
    reshade::unregister_event<addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
    reshade::unregister_event<addon_event::draw_indexed>(on_draw_indexed);
    reshade::unregister_event<addon_event::draw>(on_draw);
    reshade::unregister_event<addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
    reshade::unregister_event<addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::unregister_event<addon_event::barrier>(on_barrier);
    reshade::unregister_event<addon_event::destroy_resource>(on_destroy_resource);
    reshade::unregister_event<addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::unregister_event<addon_event::init_pipeline>(on_init_pipeline);
    reshade::unregister_event<addon_event::reset_command_list>(on_reset_cmd_list);
    reshade::unregister_event<addon_event::destroy_command_list>(on_destroy_cmd_list);
    reshade::unregister_event<addon_event::init_command_list>(on_init_cmd_list);
    reshade::unregister_event<addon_event::destroy_device>(on_destroy_device);
    reshade::unregister_event<addon_event::init_device>(on_init_device);
    reshade::unregister_event<addon_event::init_resource>(on_init_resource);
    reshade::unregister_event<addon_event::init_pipeline_layout>(on_init_pipeline_layout);
    reshade::unregister_event<addon_event::destroy_pipeline_layout>(on_destroy_pipeline_layout);
    reshade::unregister_event<addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
    reshade::unregister_event<addon_event::push_constants>(on_push_constants);
    reshade::unregister_event<addon_event::push_descriptors>(on_push_descriptors);
    reshade::unregister_event<addon_event::bind_pipeline_states>(on_bind_pipeline_states);
    reshade::unregister_event<addon_event::bind_viewports>(on_bind_viewports);
    reshade::unregister_event<addon_event::bind_scissor_rects>(on_bind_scissor_rects);
    reshade::unregister_event<addon_event::begin_render_pass>(on_begin_render_pass);
    reshade::unregister_event<addon_event::end_render_pass>(on_end_render_pass);
}

// ReShade calls AddonInit / AddonUninit outside the Windows loader lock (unlike DllMain), so registration, file access and settings
// happen here. A failure leaves nothing registered behind.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    if (!reshade::register_addon(addon_module, reshade_module))
        return false;

    std::filesystem::path dir = module_directory();
    g.ini_path = dir.empty() ? std::filesystem::path() : dir / L"crcgi_injector.ini";
    load_settings();

    // Order: per-object private data first (device / command list), so every later event finds it.
    reshade::register_event<addon_event::init_device>(on_init_device);
    reshade::register_event<addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<addon_event::init_command_list>(on_init_cmd_list);
    reshade::register_event<addon_event::destroy_command_list>(on_destroy_cmd_list);
    reshade::register_event<addon_event::reset_command_list>(on_reset_cmd_list);
    reshade::register_event<addon_event::init_resource>(on_init_resource);
    reshade::register_event<addon_event::init_pipeline_layout>(on_init_pipeline_layout);
    reshade::register_event<addon_event::destroy_pipeline_layout>(on_destroy_pipeline_layout);
    reshade::register_event<addon_event::init_pipeline>(on_init_pipeline);
    reshade::register_event<addon_event::destroy_pipeline>(on_destroy_pipeline);
    reshade::register_event<addon_event::destroy_resource>(on_destroy_resource);
    reshade::register_event<addon_event::barrier>(on_barrier);
    reshade::register_event<addon_event::bind_pipeline>(on_bind_pipeline);
    reshade::register_event<addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
    reshade::register_event<addon_event::begin_render_pass>(on_begin_render_pass);
    reshade::register_event<addon_event::end_render_pass>(on_end_render_pass);
    reshade::register_event<addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
    reshade::register_event<addon_event::push_constants>(on_push_constants);
    reshade::register_event<addon_event::push_descriptors>(on_push_descriptors);
    reshade::register_event<addon_event::bind_pipeline_states>(on_bind_pipeline_states);
    reshade::register_event<addon_event::bind_viewports>(on_bind_viewports);
    reshade::register_event<addon_event::bind_scissor_rects>(on_bind_scissor_rects);
    reshade::register_event<addon_event::draw>(on_draw);
    reshade::register_event<addon_event::draw_indexed>(on_draw_indexed);
    reshade::register_event<addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
    reshade::register_event<addon_event::dispatch>(on_dispatch);
    reshade::register_event<addon_event::clear_render_target_view>(on_clear_render_target_view);
    reshade::register_event<addon_event::update_buffer_region>(on_update_buffer_region);
    reshade::register_event<addon_event::map_buffer_region>(on_map_buffer_region);
    reshade::register_event<addon_event::unmap_buffer_region>(on_unmap_buffer_region);
    reshade::register_event<addon_event::init_effect_runtime>(on_init_runtime);
    reshade::register_event<addon_event::destroy_effect_runtime>(on_destroy_runtime);
    reshade::register_event<addon_event::reshade_reloaded_effects>(on_reloaded_effects);
    reshade::register_event<addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::register_event<addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::register_event<addon_event::reshade_present>(on_reshade_present);
    reshade::register_overlay("CRCGI Injector", draw_overlay);
    s_registered = true;
    log_msg(reshade::log::level::info, std::string("Loaded, ") + (sizeof(void *) == 8 ? "64-bit" : "32-bit") + " (built against ReShade " +
            CRCGI_RESHADE_REVISION + "). Settings: " +
            (g.ini_path.empty() ? std::string("not saved") : g.ini_path.u8string()));
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    if (s_registered)
    {
        if (g.runtime != nullptr)
            restore_end_of_frame(g.runtime);         // never leave a technique switched off
        unregister_all();
        s_registered = false;
    }
    reshade::unregister_addon(addon_module, reshade_module);
}

// Kept minimal on purpose: nothing may run under the loader lock.
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
