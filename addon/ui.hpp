// CRCGI Injector - overlay. Status on top, then tabs: Setup (quick start + checklist), G-Buffer (the game's feeds), Layers
// (the recorded layer list), Profiler (game textures with thumbnails and content analysis), Keys (remappable), Debug, Options.
// Laid out vertically: every list is a stack of cards and every widget scales with the window, so the overlay stays usable at about a
// third of a 1440p screen's width.
// The overlay edits copies of the settings and commits them under the lock, so it never races the render thread.
#pragma once
#include "events.hpp"
#include <cstdarg>

static const ImVec4 COL_OK(0.45f, 0.85f, 0.45f, 1.0f);
static const ImVec4 COL_WARN(1.0f, 0.65f, 0.3f, 1.0f);
static const ImVec4 COL_BAD(1.0f, 0.4f, 0.4f, 1.0f);
static const ImVec4 COL_DIM(0.6f, 0.6f, 0.6f, 1.0f);
static const ImVec4 COL_HEAD(0.55f, 0.75f, 1.0f, 1.0f);
static const ImVec4 LK_COLOR[LK_COUNT] = {
    ImVec4(0.6f, 0.6f, 0.6f, 1.0f), ImVec4(0.45f, 0.85f, 0.45f, 1.0f), ImVec4(0.75f, 0.75f, 0.75f, 1.0f), ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
    ImVec4(0.5f, 0.8f, 1.0f, 1.0f), ImVec4(0.85f, 0.6f, 1.0f, 1.0f), ImVec4(1.0f, 0.55f, 0.45f, 1.0f) };

// ---------------------------------------------------------------------------------------------------------------------
// Small widgets
// ---------------------------------------------------------------------------------------------------------------------
static void tooltip_text(const char *text)
{
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
}

// Tooltip for the last item.
static void tip(const char *text)
{
    if (ImGui::BeginItemTooltip())
    {
        tooltip_text(text);
        ImGui::EndTooltip();
    }
}

// A "(?)" marker on the same line with a tooltip.
static void help(const char *text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    tip(text);
}

static void section(const char *title)
{
    ImGui::Spacing();
    ImGui::TextColored(COL_HEAD, "%s", title);
    ImGui::Separator();
}

static std::string layer_name(const Pass &p, int slot)
{
    char b[96];
    std::snprintf(b, sizeof(b), "#%u.%d %s %ux%u%s", p.order, slot, fmt_name(p.rt[slot].fmt), p.rt[slot].w, p.rt[slot].h,
                  p.rt[slot].samples > 1 ? " MSAA" : "");
    return b;
}

static std::string layer_text(LayerRef ref)
{
    if (!ref.valid())
        return "none";
    std::lock_guard<std::mutex> lock(g.mtx);
    auto it = g.catalog.find(ref.key);
    if (it != g.catalog.end() && ref.slot >= 0 && ref.slot < 8)
    {
        std::string s = layer_name(it->second.p, ref.slot);
        if (it->second.last_seen + 2 < g.frame)
            s += " (not drawn now)";
        return s;
    }
    return "assigned (not drawn since the add-on loaded)";
}

static std::string tex_text(const TexSig &t)
{
    if (!t.valid())
        return "none";
    char b[96];
    std::snprintf(b, sizeof(b), "texture %s %ux%u #%u", fmt_name(static_cast<format>(t.fmt)), t.w, t.h, t.ordinal);
    return b;
}

template <typename T, typename Fn>
static bool edit_setting(T &field, Fn widget)
{
    T copy;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        copy = field;
    }
    if (widget(copy))
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        field = copy;
        g.dirty = true;
        return true;
    }
    return false;
}

static void thumb(resource_view v, uint32_t w, uint32_t h, float width)
{
    if (v.handle == 0 || w == 0 || h == 0)
        return;
    ImGui::Image(to_texture_id(v.handle), ImVec2(width, width * static_cast<float>(h) / static_cast<float>(w)));
}

static std::string bound_key(int action)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    return key_name(g.keys[action]);
}

static float s_thumb = 200.0f;   // thumbnail width (G-Buffer, Profiler), adjustable

// Width of a widget: `chars` font heights, but never more than 62% of the space the window has left (labels sit to the right).
static float fit(float chars)
{
    const float fs = ImGui::GetFontSize();
    return std::min(fs * chars, std::max(ImGui::GetContentRegionAvail().x * 0.62f, fs * 8.0f));
}

// A line of text that wraps at the window's edge, in the dim colour.
static void dim_wrapped(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
    ImGui::TextWrapped("%s", buf);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------------------------------------------------
static void draw_status(effect_runtime *)
{
    bool master, injected_recent, has_inject, force, scene_set;
    int level;
    device_api api;
    std::string status, settings_error;
    LayerRef inject_ref;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        inject_ref = g.role_ref[ROLE_INJECT];
        master = g.master;
        level = g.level;
        injected_recent = g.injected_frame != ~0ull && g.injected_frame + 1 >= g.frame;
        has_inject = g.role_ref[ROLE_INJECT].valid();
        force = g.force_inject;
        scene_set = g.feed[FEED_SCENE].enabled && g.feed[FEED_SCENE].assigned();
        status = g.status;
        settings_error = g.settings_error;
        api = g.api;
    }

    char lbl[96];
    std::snprintf(lbl, sizeof(lbl), "Integration on  (%s)", bound_key(KA_TOGGLE).c_str());
    edit_setting(g.master, [&](bool &v) { return ImGui::Checkbox(lbl, &v); });
    tip("Off: the add-on does nothing and ReShade runs the effect at the end of the frame as usual.");
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "%s, %s", api_name(api), sizeof(void *) == 8 ? "64-bit" : "32-bit");
    ImGui::SetNextItemWidth(fit(28.0f));
    std::snprintf(lbl, sizeof(lbl), "Level  (%s / %s)", bound_key(KA_LEVEL_DOWN).c_str(), bound_key(KA_LEVEL_UP).c_str());
    edit_setting(g.level, [&](int &v) {
        return ImGui::Combo(lbl, &v, "A  End of frame, nothing touched\0B  A + the game's camera (FOV)\0C  B + the game's G-buffer and finished frame\0");
    });
    tip("A: plain ReShade, the effect runs at the end of the frame.\n"
        "B: the camera's field of view is read from the game and written to the shader (and, with Force render on in Options, the effect "
        "is rendered inside the frame at a layer you choose).\n"
        "C: B plus the feeds: the game's normals, albedo, motion vectors, HDR scene light and the finished frame (G-Buffer tab). The "
        "effect still runs at the end of the frame; the feeds are copies taken while the game draws.");

    if (!settings_error.empty())
        ImGui::TextColored(COL_WARN, "%s", settings_error.c_str());
    if (!g_conflict.empty())
        ImGui::TextWrapped("\"%s\" is loaded. It also moves effects into the frame, so forced in-frame rendering stands down. Feeds at the end of "
                           "the frame and the profiler still work.", g_conflict.c_str());
    if (!master)
        dim_wrapped("Off: ReShade runs the effect at the end of the frame as usual.");
    else if (level == LEVEL_A)
        dim_wrapped("Level A: the effect runs at the end of the frame, nothing is changed.");
    else
    {
        if (level >= LEVEL_C)
        {
            if (!scene_set)
                dim_wrapped("No finished frame chosen: the effect works on the final image, UI included. Give the last layer before the UI the "
                            "role \"Finished frame\" in the Layers tab to leave UI and fog alone.");
            else if (feed_ready(FEED_SCENE))
                ImGui::TextColored(COL_OK, "Finished frame delivered: UI and fog drawn after it are left untouched.");
            else
                ImGui::TextColored(COL_WARN, "The finished frame was not captured in this frame (menu / loading screen?): the effect uses the final image.");
        }
        if (force)
        {
            if (!api_supports_injection(api) || !g_conflict.empty())
                dim_wrapped("Force render: %s", injection_unsupported_reason(api));
            else if (!has_inject)
                ImGui::TextColored(COL_WARN, "Force render is on, but no layer has the role \"Force render here\". The effect runs at the end of the frame.");
            else if (!status.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, COL_WARN);
                ImGui::TextWrapped("%s", status.c_str());
                ImGui::PopStyleColor();
            }
            else if (injected_recent)
                ImGui::TextColored(COL_OK, "Force render: rendering inside the frame at %s", layer_text(inject_ref).c_str());
            else
                ImGui::TextColored(COL_WARN, "Force render: the layer was not drawn in the last frame, the effect runs at the end of the frame.");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Setup: quick start, checklist, techniques, camera, shader link
// ---------------------------------------------------------------------------------------------------------------------
static void draw_camera()
{
    edit_setting(g.cam_auto, [](bool &v) { return ImGui::Checkbox("Pick the camera automatically", &v); });
    ImGui::SameLine();
    edit_setting(g.cam_apply, [](bool &v) { return ImGui::Checkbox("Write the FOV to the shader", &v); });
    if (ImGui::Button("Forget and re-scan"))
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.cams.clear();
        g.cam_valid = false;
        g.cam_source = CAM_NONE;
        g.cam_sig = 0;
        g.dirty = true;
    }
    help("Projection matrices are found in the game's constant buffers. Shadow, reflection and cube-map cameras look alike: check the FOV "
         "against the game. Persistently mapped buffers (common in Direct3D 12) are not seen; then set the shader's FOV by hand.");
    std::vector<CamCandidate> list;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        list = g.cams;
    }
    std::sort(list.begin(), list.end(), [](const CamCandidate &a, const CamCandidate &b) { return a.hits > b.hits; });
    for (size_t i = 0; i < list.size() && i < 8; i++)
    {
        const CamCandidate &c = list[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("Use"))
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            g.cam = c;
            g.cam_valid = true;
            g.cam_source = CAM_PICKED;
            g.cam_auto = false;
            g.cam_track_res = c.res;
            g.cam_track_off = c.offset;
            g.cam_track_t = c.transposed;
            g.cam_sig = c.sig;
            g.dirty = true;
        }
        ImGui::SameLine();
        ImGui::Text("FOV %.1f   aspect %.2f   near %.3g   far %s   %s   seen %u", c.fov, c.aspect, c.nearp,
                    std::isfinite(c.farp) ? std::to_string(static_cast<int>(c.farp)).c_str() : "inf", c.reversed ? "reversed" : "standard", c.hits);
        ImGui::PopID();
    }
    if (list.empty())
        ImGui::TextDisabled("None found yet. The shader's own Field of View is used.");
}

static void draw_techniques(effect_runtime *rt)
{
    std::vector<TechInfo> techs;
    std::vector<std::string> managed;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        techs = g.techs;
        managed = g.techniques;
    }
    dim_wrapped("Ticked techniques are rendered at the forced layer, if they are also enabled in ReShade.");
    for (const TechInfo &t : techs)
    {
        bool on = contains(managed, t.key);
        std::string label = t.key;
        size_t bar = label.find('|');
        if (bar != std::string::npos)
            label = label.substr(bar + 1) + "   (" + label.substr(0, bar) + ")";
        if (ImGui::Checkbox(label.c_str(), &on))
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            if (on) g.techniques.push_back(t.key);
            else g.techniques.erase(std::remove(g.techniques.begin(), g.techniques.end(), t.key), g.techniques.end());
            g.dirty = true;
        }
    }
    if (techs.empty())
        ImGui::TextDisabled("No techniques loaded.");
    if (ImGui::Button("Refresh"))
    {
        refresh_techniques(rt);
        refresh_uniforms(rt);
    }
}

static void check_line(bool ok, bool warn, const char *what, const std::string &detail)
{
    ImGui::TextColored(ok ? COL_OK : (warn ? COL_WARN : COL_DIM), ok ? "[ok]" : (warn ? "[!!]" : "[--]"));
    ImGui::SameLine();
    ImGui::Text("%s", what);
    ImGui::Indent(ImGui::GetFontSize() * 1.2f);
    dim_wrapped("%s", detail.c_str());
    ImGui::Unindent(ImGui::GetFontSize() * 1.2f);
}

static void draw_setup(effect_runtime *rt)
{
    section("Quick start");
    ImGui::TextWrapped("1. Enable CRCGI in ReShade. It runs at the end of the frame, like any effect.");
    ImGui::TextWrapped("2. Optional (level C): let it use what the game knows. Profiler: Take snapshot, read the suggestions, press Use as. "
                       "Layers: give the last layer before the UI the role Finished frame.");
    ImGui::TextWrapped("3. Check the result in the shader's Debug views. Everything is saved per game and can be shared (Options).");

    ImGui::SetNextItemWidth(fit(26.0f));
    edit_setting(g.upscale_mode, [](int &v) { return ImGui::Combo("Upscaler", &v, UPSCALE_ITEMS); });
    help("DLSS / FSR / XeSS render the 3D scene below the output resolution, e.g. 1707 x 960 for 1440p in Quality mode, so the G-buffer is "
         "smaller than the screen. With this on, textures at the render resolution count as screen-sized in the layer list and the "
         "profiler, and can be used as feeds (the shader samples them across the whole screen).\n"
         "Auto-detect: the most-drawn size below the screen's, of the screen's aspect, drawn with depth.");
    int up;
    uint32_t rw, rh, bw, bh;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        up = g.upscale_mode;
        rw = g.rr_w;
        rh = g.rr_h;
        bw = g.bb_w;
        bh = g.bb_h;
    }
    if (up == UP_CUSTOM)
    {
        ImGui::SetNextItemWidth(fit(12.0f));
        edit_setting(g.upscale_pct, [](float &v) { return ImGui::SliderFloat("Render scale (%)", &v, 20.0f, 100.0f, "%.1f"); });
    }
    if (up != UP_OFF)
    {
        if (rw != 0)
            ImGui::TextColored(COL_DIM, "Render resolution: %u x %u (%.1f%% of %u x %u)", rw, rh, 100.0 * rw / std::max<uint32_t>(bw, 1), bw, bh);
        else
            ImGui::TextColored(COL_DIM, "Render resolution: native (no smaller scene targets found)");
    }

    // Checklist
    section("Checklist");
    bool tech_on = false;
    std::vector<effect_technique> techs;
    int level;
    bool master, cam_valid;
    CamCandidate cam;
    FeedConfig fc[FEED_COUNT];
    std::string depth_status;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        techs = resolve_techniques_locked();
        level = g.level;
        master = g.master;
        cam_valid = g.cam_valid;
        cam = g.cam;
        depth_status = g.depth_status;
        for (int i = 0; i < FEED_COUNT; i++)
            fc[i] = g.feed[i];
    }
    for (effect_technique t : techs)
        tech_on |= rt->get_technique_state(t) || [&] { std::lock_guard<std::mutex> l(g.mtx); return contains(g.suppressed, t); }();
    check_line(tech_on, true, "CRCGI technique", tech_on ? "enabled in ReShade" : "not enabled in ReShade (or not ticked under Options > Force render)");
    char cb[96];
    std::snprintf(cb, sizeof(cb), "vertical FOV %.1f", cam.fov);
    check_line(cam_valid, false, "Camera", cam_valid ? cb : "not detected: the shader's own Field of View is used");
    if (!depth_status.empty())
        check_line(false, true, "Depth", depth_status);
    for (int i = 0; i < FEED_COUNT; i++)
    {
        if (!UI_MATERIAL && i == FEED_MATERIAL)
            continue;
        std::string d;
        bool ok = false, warn = false;
        if (!fc[i].assigned())
            d = "not assigned";
        else if (!fc[i].enabled)
            d = "assigned, switched off";
        else if (!master || level < LEVEL_C)
            d = "assigned; used at level C";
        else if (feed_ready(i))
        {
            ok = true;
            d = std::string("delivered from ") + (fc[i].source == SRC_TEXTURE ? tex_text(fc[i].tex) : layer_text(fc[i].ref));
        }
        else
        {
            warn = true;
            d = "assigned but not captured (see G-Buffer tab)";
        }
        check_line(ok, warn, FEED_LABEL[i], d);
    }

    if (ImGui::CollapsingHeader("Camera"))
        draw_camera();
    if (ImGui::CollapsingHeader("Shader link"))
    {
        static const char *const NAMES[] = { "FieldOfView", "UseNormalTexture", "Integration_NormalSpace", "Integration_GeometrySource", "Integration_Albedo",
                                             "Integration_Motion", "Integration_HDRScene", "Integration_Scene",
                                             "Integration_NormalMode", "Integration_MotionUnits", "GBufferDebugView" };
        dim_wrapped("Shader controls the add-on can write (missing = the shader is not loaded or is a different one). Convert Tonemapper is never written.");
        for (const char *n : NAMES)
        {
            bool found = uniform_found(n);
            ImGui::TextColored(found ? COL_OK : COL_DIM, "%s %s", found ? "[found]  " : "[missing]", n);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// G-Buffer: the feeds
// ---------------------------------------------------------------------------------------------------------------------
static const char *const FEED_HELP[FEED_COUNT] = {
    "The game's surface normals. The shader then skips rebuilding normals from depth (Geometry Source). View-space normals are used as "
    "they are; world-space normals are aligned to the camera by the shader every frame.",
    "Base colour (not lit colour). Used for multibounce reflectance, the relit pickup and the albedo response; the 16 taps per texel of "
    "the image-based reflectance estimate are skipped.",
    "The game's motion vectors. They replace the shader's block-matching motion estimate (its 4 passes do no work) for temporal "
    "accumulation, the canvas and multibounce.",
    "The G-buffer target that holds metalness / specular / roughness (not offered in this release).",
    "The game's lighting buffer before tonemapping, bloom, fog and UI (usually RGBA16F / R11G11B10F). Rays pick up light from it: "
    "cleaner and with the real brightness range. The exposure is matched to the image automatically.",
    "A copy of the image at the point where the game's 3D rendering is finished: the layer right before the UI, before fog or before "
    "whatever should not be touched. The effect works on this copy and adds its light to the final image, so everything drawn after the "
    "copy is left alone (the shader's Use Game Finished Frame If Provided)." };

static void draw_feeds(effect_runtime *rt)
{
    DevState *ds = rt->get_device()->get_private_data<DevState>();
    int level;
    bool master;
    FeedConfig fc[FEED_COUNT];
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        level = g.level;
        master = g.master;
        for (int i = 0; i < FEED_COUNT; i++)
            fc[i] = g.feed[i];
    }
    ImGui::SetNextItemWidth(fit(8.0f));
    ImGui::SliderFloat("thumbnails", &s_thumb, 96.0f, 320.0f, "%.0f px");
    dim_wrapped("Feeds are assigned by you: from a texture in the Profiler (Use as) or from a layer (its Role in the Layers tab).");
    if (!master || level < LEVEL_C)
        ImGui::TextColored(COL_WARN, "Feeds are handed to the shader at level C (%s).", bound_key(KA_LEVEL_UP).c_str());

    // What the shader stops doing with the delivered feeds (the add-on's main performance lever).
    section("Shader work saved by the delivered feeds");
    bool rd[FEED_COUNT];
    for (int i = 0; i < FEED_COUNT; i++)
        rd[i] = master && level >= LEVEL_C && fc[i].enabled && feed_ready(i);
    int gs;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        gs = g.geom_source;
    }
    ImGui::TextColored(rd[FEED_NORMALS] && gs > 0 ? COL_OK : COL_DIM, "%s Geometry pass: depth normal rebuild + neighbour smoothing (5 rebuilt normals per pixel)%s",
                       rd[FEED_NORMALS] && gs > 0 ? "[skipped]" : "[runs]   ", rd[FEED_NORMALS] && gs == 2 ? ", everywhere" : "");
    ImGui::TextColored(rd[FEED_ALBEDO] ? COL_OK : COL_DIM, "%s Relit pickup reflectance estimate (16 taps per texel) and albedo response taps",
                       rd[FEED_ALBEDO] ? "[skipped]" : "[runs]   ");
    ImGui::TextColored(rd[FEED_MOTION] ? COL_OK : COL_DIM, "%s Motion estimation (4 block-matching passes)", rd[FEED_MOTION] ? "[skipped]" : "[runs]   ");
    ImGui::TextColored(rd[FEED_HDR] ? COL_OK : COL_DIM, "%s Light pickup from the finished image (UI, bloom, grain included)", rd[FEED_HDR] ? "[replaced]" : "[used]   ");
    ImGui::TextColored(rd[FEED_SCENE] ? COL_OK : COL_DIM, "%s The effect works on the 3D scene only; UI and fog after it stay untouched", rd[FEED_SCENE] ? "[active]  " : "[off]     ");
    ImGui::SetNextItemWidth(fit(22.0f));
    edit_setting(g.geom_source, [](int &v) {
        return ImGui::Combo("Geometry source with game normals", &v, "Rebuild from depth everywhere\0Game normals where valid (default)\0Game normals only (fastest)\0");
    });
    help("Written to the shader's Game Integration > Geometry Source while normals are delivered and their switch is driven.\n"
         "Game normals only: pixels without a game normal (sky, holes) face the camera instead of being rebuilt.");

    for (int i = 0; i < FEED_COUNT; i++)
    {
        if (!UI_MATERIAL && i == FEED_MATERIAL)
            continue;
        ImGui::PushID(i);
        FeedConfig f = fc[i];
        resource live = { 0 };
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            live = g.feed_res[i];
        }
        std::string st;
        uint64_t captured = ~0ull;
        resource_view v = { 0 };
        uint32_t w = 0, h = 0;
        if (ds != nullptr)
        {
            std::lock_guard<std::mutex> lock(ds->feed_mutex);
            st = ds->feed[i].status;
            captured = ds->feed[i].frame;
            v = ds->feed[i].srv;
            w = ds->feed[i].w;
            h = ds->feed[i].h;
        }
        const char *badge = !f.assigned() ? "not assigned" : (!f.enabled ? "off" : (rd[i] ? "delivered" : "not delivered"));
        char head[128];
        std::snprintf(head, sizeof(head), "%s   [%s]###feed%d", FEED_LABEL[i], badge, i);
        ImGui::PushStyleColor(ImGuiCol_Text, rd[i] ? COL_OK : (f.assigned() && f.enabled ? COL_WARN : ImGui::GetStyleColorVec4(ImGuiCol_Text)));
        const bool open = ImGui::CollapsingHeader(head, (i <= FEED_MOTION || i == FEED_SCENE) ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleColor();
        tip(FEED_HELP[i]);
        if (!open)
        {
            ImGui::PopID();
            continue;
        }
        bool changed = false;
        changed |= ImGui::Checkbox("Provide to the shader", &f.enabled);
        ImGui::SameLine();
        if (f.source == SRC_TEXTURE)
            ImGui::TextColored(f.tex.valid() ? (live.handle ? COL_OK : COL_WARN) : COL_DIM, "from %s%s", tex_text(f.tex).c_str(),
                               f.tex.valid() && live.handle == 0 ? " (not created by the game right now)" : "");
        else if (f.layout != 0 && !f.ref.valid() && feed_policy(i) == CAP_RUN)
            ImGui::TextColored(COL_OK, "from a shared listing: target %d of a layer layout (found when the game draws it)", f.ref.slot + 1);
        else
            ImGui::TextColored(f.ref.valid() ? COL_OK : COL_DIM, "from layer %s", layer_text(f.ref).c_str());
        if (f.source == SRC_LAYER && f.layout != 0 && feed_policy(i) == CAP_RUN)
            dim_wrapped("Follows the layer's layout (its targets' formats and sizes, not the texture's address): every pass drawn with that "
                        "layout into this target is captured once it is complete, also when the game swaps or recreates the texture.");
        if (f.assigned())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear"))
            {
                f.ref = LayerRef();
                f.layout = 0;
                f.tex = TexSig();
                f.enabled = false;
                changed = true;
            }
        }
        else
            ImGui::TextColored(COL_DIM, "Assign it in the Profiler (Use as) or with a layer's Role.");

        if (f.enabled && f.assigned())
        {
            if (level < LEVEL_C || !master)
                ImGui::TextColored(COL_DIM, "Used at level C.");
            else if (rd[i])
                ImGui::TextColored(COL_OK, "Handed to the shader (captured %llu frame(s) ago).", static_cast<unsigned long long>(captured == ~0ull ? 0 : g.frame - captured));
            else
                ImGui::TextColored(COL_WARN, "Not used: %s%s", st.empty() ? "not captured yet" : st.c_str(),
                                   captured == ~0ull ? "" : (" (last captured " + std::to_string(g.frame - captured) + " frames ago)").c_str());
        }
        if (v.handle != 0)
            thumb(v, w, h, s_thumb);

        const float cw = fit(20.0f);
        if (i == FEED_NORMALS)
        {
            ImGui::SetNextItemWidth(cw);
            changed |= ImGui::Combo("Encoding", &f.mode, "RGB stored as 0..1 (decoded *2-1)\0Octahedral (2 channels, 0..1)\0RGB signed, as stored (float / snorm)\0");
            help("How the game packs its normals. The profiler's analysis suggests one. Wrong = the shader's Normal Difference view is bright everywhere.");
            ImGui::SetNextItemWidth(cw);
            changed |= ImGui::Combo("Space", &f.space, "View space\0World / other space (the shader aligns it to the camera)\0");
            help("World space: the shader fits the rotation to camera space every frame (see its Auto-Align Status view). Use it whenever "
                 "you are not sure: it also absorbs flipped or swapped axes.");
            changed |= ImGui::Checkbox("Flip Y", &f.flip_y);
        }
        if (i == FEED_ALBEDO)
            dim_wrapped("Must be base colour, not lit colour. Written through an sRGB view = decoded to linear when sampled.");
        if (i == FEED_MOTION)
        {
            ImGui::SetNextItemWidth(cw);
            changed |= ImGui::Combo("Units", &f.units, "UV (0..1)\0Pixels\0NDC (-1..1)\0");
            changed |= ImGui::Checkbox("Flip Y", &f.flip_y);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Invert direction", &f.invert);
            help("Check with the shader's Debug View > Motion Flow while turning the camera: the colours must follow the motion.");
        }
        if (i == FEED_MATERIAL)
        {
            ImGui::SetNextItemWidth(cw * 0.5f);
            changed |= ImGui::Combo("Channel", &f.channel, "R\0G\0B\0A\0");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(cw * 0.8f);
            changed |= ImGui::Combo("Holds", &f.meaning, "Metalness\0Specular / reflectance\0Roughness\0Smoothness / gloss\0");
            help("Which channel of the material texture holds what. Check with the shader's Debug: G-Buffer & Canvas > Game Material view: "
                 "metal (armour, swords) must turn dark, cloth and stone stay white. Roughness and smoothness are accepted but have no "
                 "diffuse effect (they would need specular GI).");
        }
        if (i == FEED_HDR)
            dim_wrapped("Exposure is matched to the image automatically. Check: Debug: G-Buffer & Canvas > Game HDR Scene Light.");
        if (i == FEED_SCENE)
            dim_wrapped("Pick the last layer (or texture) that holds the finished 3D image before the UI. Check the thumbnail: it must show the "
                        "game without HUD. It must be as large as the screen and in the back buffer's format.");
        char sync_label[160];
        std::snprintf(sync_label, sizeof(sync_label), "Drive the shader's \"%s\" switch", FEED_SWITCH_LABEL[i]);
        changed |= ImGui::Checkbox(sync_label, &f.sync);
        tip("On: the add-on turns the shader's switch on exactly while this feed is delivered, and off otherwise (so the shader never "
            "reads an empty texture). Off (default): the switch is left to you, tick it in the shader once the feed looks right in its "
            "Debug: G-Buffer & Canvas views.");
        if (changed)
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            if (!f.assigned())
                g.feed_res[i] = resource{ 0 };
            g.feed[i] = f;
            g.dirty = true;
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Layers: the recorded catalog, as a sortable, filterable table
// ---------------------------------------------------------------------------------------------------------------------
static bool contains_ci(const std::string &hay, const char *needle)
{
    if (needle == nullptr || *needle == 0)
        return true;
    return lower(hay).find(lower(needle)) != std::string::npos;
}

static void draw_layers(effect_runtime *rt)
{
    int rec_left, rec_total, sort_mode;
    size_t cat_size;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        rec_left = g.rec_left;
        rec_total = std::max(g.rec_total, 1);
        cat_size = g.catalog.size();
        sort_mode = g.sort_mode;
    }
    if (ImGui::Button(rec_left > 0 ? "Recording..." : "Record again (3 s)"))
        start_recording(180);
    tip("Records the frame's layers for 3 seconds and merges them into the list. The list then stays as it is: it does not refresh every frame.");
    ImGui::SameLine();
    if (ImGui::Button("Clear and record"))
    {
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            g.catalog.clear();
            g.cat_order.clear();
        }
        start_recording(180);
    }
    ImGui::SameLine();
    if (ImGui::Button("Re-sort"))
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        sort_catalog_locked();
    }
    tip("Applies the sort again (new layers are appended at the end until then).");
    ImGui::SetNextItemWidth(fit(14.0f));
    if (ImGui::Combo("Sort", &sort_mode, SORT_ITEMS))
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.sort_mode = sort_mode;
        sort_catalog_locked();
        g.dirty = true;
    }
    help("Frame order: as drawn. Resolution, then frame order: the largest targets first (screen, then render resolution, then smaller), "
         "each group in frame order. Kind: G-buffer, geometry, lighting, transparent, post-process, UI. Ties always keep frame order.");

    edit_setting(g.only_always, [](bool &v) { return ImGui::Checkbox("Only layers drawn every frame", &v); });
    tip("Hides layers that are drawn only sometimes (menus, cut-scenes, weather, effects the camera sees now and then). A layer must be "
        "drawn in 95% of the frames since it was first seen, for at least a second. Layers with a role are always shown.");
    edit_setting(g.only_useful, [](bool &v) { return ImGui::Checkbox("Hide small targets", &v); });
    tip("Hides targets under a quarter of the screen's width (shadow maps, bloom mips, ...).");
    edit_setting(g.cat_add_new, [](bool &v) { return ImGui::Checkbox("Add new layers", &v); });
    tip("After a recording, keep adding layers that appear later (new areas, menus) at the end of the list.");
    static char filter[64] = "";
    ImGui::SetNextItemWidth(fit(16.0f));
    ImGui::InputTextWithHint("##filter", "filter: format, size, kind, note", filter, sizeof(filter));
    if (rec_left > 0)
        ImGui::ProgressBar(1.0f - static_cast<float>(rec_left) / rec_total, ImVec2(-FLT_MIN, 0.0f), "recording the frame's layers");
    dim_wrapped("%zu layers recorded. Click = preview, double-click = finished frame. Keys: %s / %s select, %s finished frame, %s / %s role.",
                cat_size, bound_key(KA_LAYER_PREV).c_str(), bound_key(KA_LAYER_NEXT).c_str(), bound_key(KA_FRAME_END_HERE).c_str(),
                bound_key(KA_ROLE_PREV).c_str(), bound_key(KA_ROLE_NEXT).c_str());

    std::vector<Row> rows = build_rows();
    uint64_t frame;
    bool scroll;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.preview_frame = g.frame;                         // tells the capture that the overlay wants the preview
        frame = g.frame;
        scroll = g.scroll_to_sel;
        g.scroll_to_sel = false;
    }
    const int sel = selected_index(rows);
    std::vector<int> shown;
    std::vector<std::string> notes(rows.size()), targets(rows.size());
    for (size_t i = 0; i < rows.size(); i++)
    {
        const Row &r = rows[i];
        targets[i] = std::string(fmt_name(r.p.rt[r.slot].fmt)) + "  " + std::to_string(r.p.rt[r.slot].w) + " x " + std::to_string(r.p.rt[r.slot].h) +
                     (r.p.rt[r.slot].samples > 1 ? "  MSAA" : "");
        std::string n = hint_for(r.p, r.slot);
        if (r.p.ps_changes > 0)
            n += (n.empty() ? "" : ", ") + std::string("mixes ") + std::to_string(r.p.ps_changes + 1) + " shaders";
        if (r.p.rt_count > 1)
            n += (n.empty() ? "" : ", ") + std::to_string(r.p.rt_count) + " targets (slot " + std::to_string(r.slot) + ")";
        if (r.last_seen + 2 < frame)
            n += (n.empty() ? "" : ", ") + std::string("not drawn now");
        notes[i] = n;
        if (contains_ci(targets[i] + " " + n + " " + LK_LABEL[r.kind], filter))
            shown.push_back(static_cast<int>(i));
    }

    // One card per layer: id, target and kind on the first line, the note under it, the role last. Scrolls inside its own window.
    const float fs = ImGui::GetFontSize();
    if (ImGui::BeginChild("layers_list", ImVec2(0.0f, fs * 24.0f), true))
    {
        int drawn = 0;
        for (size_t k = 0; k < shown.size() && drawn < 300; k++, drawn++)
        {
            const int i = shown[k];
            const Row &r = rows[i];
            const bool live = r.last_seen + 2 >= frame;
            ImGui::PushID(i);
            if (!live)
                ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
            char head[160];
            std::snprintf(head, sizeof(head), "#%u.%d   %s   %u draws", r.p.order, r.slot, targets[i].c_str(), r.p.draws);
            if (ImGui::Selectable(head, i == sel, ImGuiSelectableFlags_AllowDoubleClick))
            {
                select_row(rows, i);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    apply_role(r.p, r.slot, ROLE_SCENE);
            }
            if (scroll && i == sel)
                ImGui::SetScrollHereY(0.5f);
            if (ImGui::BeginItemTooltip())
            {
                ImGui::Text("%s", layer_name(r.p, r.slot).c_str());
                ImGui::Text("%u draws (%u blended, %u writing depth), pixel shader %08X, %s depth buffer", r.p.draws, r.p.blend_draws, r.p.zwrite_draws,
                            r.p.ps, r.p.depth.handle ? "with" : "no");
                ImGui::Text("Kind: %s.  Position in the frame: %.0f%%.  Seen in %u frames.", LK_LABEL[r.kind], r.pos * 100.0f, r.seen);
                ImGui::TextColored(COL_DIM, "Click: preview below.  Double-click: use as the finished frame.");
                ImGui::EndTooltip();
            }
            ImGui::TextColored(live ? LK_COLOR[r.kind] : COL_DIM, "%s", LK_LABEL[r.kind]);
            if (!notes[i].empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", notes[i].c_str());
            }
            int role = role_of(r.p, r.slot), nr = role;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##role", &nr, ROLE_ITEMS) && nr != role)
                apply_role(r.p, r.slot, nr);
            if (!live)
                ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::PopID();
        }
        if (shown.empty())
            ImGui::TextDisabled(rows.empty() ? "Nothing recorded yet (or no layer is drawn in every frame)." : "No layer matches the filter.");
    }
    ImGui::EndChild();

    // preview of the selected layer
    DevState *ds = rt->get_device()->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    resource_view srv = { 0 };
    uint32_t pw = 0, ph = 0;
    std::string pstatus;
    {
        std::lock_guard<std::mutex> lock(ds->feed_mutex);
        if (ds->preview.srv.handle != 0 && ds->preview.frame + 3 >= g.frame && ds->preview.w > 0)
        {
            srv = ds->preview.srv;
            pw = ds->preview.w;
            ph = ds->preview.h;
        }
        pstatus = ds->preview.status;
    }
    if (sel >= 0)
        ImGui::TextWrapped("Selected: %s  (%s)", layer_name(rows[sel].p, rows[sel].slot).c_str(), LK_LABEL[rows[sel].kind]);
    if (srv.handle != 0)
        thumb(srv, pw, ph, std::min(ImGui::GetContentRegionAvail().x, fs * 26.0f));
    else if (rt->get_device()->get_api() == device_api::vulkan)
        dim_wrapped("No live preview on Vulkan (nothing can be copied inside a render pass): the Profiler shows the textures.");
    else if (!pstatus.empty())
        dim_wrapped("No preview: %s", pstatus.c_str());
    else if (sel >= 0 && rows[sel].last_seen + 2 < frame)
        dim_wrapped("No preview: this layer is not drawn at the moment.");
}

// ---------------------------------------------------------------------------------------------------------------------
// Profiler: the game's render-target textures, as a table
// ---------------------------------------------------------------------------------------------------------------------
// One score as a label and a bar that fills the card's width (the scores stack vertically instead of sitting side by side).
static void score_row(const char *name, float v, bool suggested)
{
    const float fs = ImGui::GetFontSize();
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", v);
    ImGui::TextColored(suggested ? COL_OK : COL_DIM, "%-11s", name);
    ImGui::SameLine(fs * 7.5f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, v >= 0.6f ? COL_OK : (v > 0.0f ? COL_WARN : COL_DIM));
    ImGui::ProgressBar(std::clamp(v, 0.0f, 1.0f), ImVec2(-FLT_MIN, fs * 0.85f), b);
    ImGui::PopStyleColor();
}

static void draw_profiler()
{
    const float fs = ImGui::GetFontSize();
    if (ImGui::Button("Take snapshot"))
        pool_request_snapshot();
    tip("Copies each candidate texture once (where its state is known) and analyses 16 tiles of it. The scores are suggestions: nothing is "
        "assigned until you press Use as.");
    ImGui::SameLine();
    if (ImGui::Button("Release snapshots"))
        pool_release_snapshots();
    tip("Frees the video memory of the snapshot copies.");

    static bool show_unused = false;
    static int order = 1;
    int filter, limit;
    {
        std::lock_guard<std::mutex> lock(prof.m);
        filter = prof.filter;
        limit = prof.snap_limit;
    }
    ImGui::SetNextItemWidth(fit(17.0f));
    if (ImGui::Combo("Size", &filter, "All render targets\0Screen aspect, a third of its size or more\0Screen or render (upscaler) size\0"))
    {
        std::lock_guard<std::mutex> lock(prof.m);
        prof.filter = filter;
    }
    ImGui::SetNextItemWidth(fit(9.0f));
    ImGui::Combo("Order", &order, "Frame order\0Best score\0Largest first\0");
    ImGui::SetNextItemWidth(fit(9.0f));
    if (ImGui::SliderInt("Snapshots", &limit, 4, 48))
    {
        std::lock_guard<std::mutex> lock(prof.m);
        prof.snap_limit = limit;
    }
    tip("How many textures (most drawn first) are copied per snapshot.");
    ImGui::Checkbox("Show unused", &show_unused);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fit(7.0f));
    ImGui::SliderFloat("##thumb", &s_thumb, 96.0f, 320.0f, "%.0f px");

    std::vector<PoolTex> items;
    uint64_t vram = 0;
    int phase;
    bool busy = false;
    {
        std::lock_guard<std::mutex> lock(prof.m);
        phase = prof.snapshot_phase;
        for (const auto &e : prof.pool)
        {
            const PoolTex &t = e.second;
            if (t.snap.res.handle != 0)
                vram += static_cast<uint64_t>(t.snap.w) * t.snap.h * std::max<uint32_t>(texel_size(t.fmt), 4);
            busy |= t.rb_state == 1 || t.rb_state == 2;
            if (!pool_candidate_size(t, filter))
                continue;
            if (!show_unused && t.l_binds == 0 && t.l_dispatches == 0 && t.snap.res.handle == 0)
                continue;
            items.push_back(t);
        }
    }
    auto best_score = [](const PoolTex &t) {
        float b = 0.0f;
        for (int f = 0; f < FEED_COUNT; f++)
            b = std::max(b, t.an.score(f));
        return b;
    };
    std::sort(items.begin(), items.end(), [&](const PoolTex &a, const PoolTex &b) {
        if (order == 1 && best_score(a) != best_score(b)) return best_score(a) > best_score(b);
        if (order == 2 && a.sig.w * a.sig.h != b.sig.w * b.sig.h) return a.sig.w * a.sig.h > b.sig.w * b.sig.h;
        if ((a.l_binds > 0) != (b.l_binds > 0)) return a.l_binds > 0;
        return a.l_order != b.l_order ? a.l_order < b.l_order : a.seq < b.seq;
    });
    ImGui::TextColored(COL_DIM, "%zu textures, snapshots use %.1f MB.%s", items.size(), vram / (1024.0 * 1024.0),
                       phase ? "  Capturing..." : (busy ? "  Analysing..." : ""));
    help("Textures come from the game's own creation calls, so this also works where descriptor heaps hide the bindings (Direct3D 12, "
         "Vulkan). Copies are taken where the texture's state is known: after a binding in which it was a render target (Direct3D 12 "
         "also right after a render pass ended), or on Vulkan right after the game's own barrier out of that state. Games that place "
         "several targets in the same memory (aliasing) are handled: the copy is taken at the texture's main binding.");

    Suggestion best[FEED_COUNT];
    profiler_suggestions(best);
    FeedConfig fc[FEED_COUNT];
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        for (int i = 0; i < FEED_COUNT; i++)
            fc[i] = g.feed[i];
    }
    if (!items.empty() && phase == 0 && !busy)
    {
        bool any = false;
        for (int f = 0; f < FEED_COUNT; f++)
            any |= best[f].have && !(!UI_MATERIAL && f == FEED_MATERIAL);
        if (!any)
            dim_wrapped("No clear suggestion yet. Take a snapshot while the scene is on screen.");
    }

    const float cw = ImGui::GetContentRegionAvail().x;
    if (ImGui::BeginChild("pool_list", ImVec2(cw, fs * 34.0f), true))
    {
        for (size_t i = 0; i < items.size(); i++)
        {
            const PoolTex &t = items[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextColored(COL_HEAD, "%s   %u x %u   #%u%s", fmt_name(t.view_fmt != format::unknown ? t.view_fmt : t.fmt), t.sig.w, t.sig.h, t.sig.ordinal,
                               t.sig.samples > 1 ? "   MSAA" : "");
            if (t.sig.w != g.bb_w && screen_sized(t.sig.w, t.sig.h))
            {
                ImGui::SameLine();
                ImGui::TextColored(COL_DIM, "render res.");
            }
            for (int f = 0; f < FEED_COUNT; f++)
                if (fc[f].source == SRC_TEXTURE && fc[f].tex == t.sig)
                    ImGui::TextColored(COL_OK, "assigned: %s", FEED_LABEL[f]);
            if (t.snap.srv.handle != 0)
                thumb(t.snap.srv, t.snap.w, t.snap.h, std::min(s_thumb, ImGui::GetContentRegionAvail().x));
            else
                ImGui::TextDisabled("(no snapshot)");
            if (t.l_binds > 0)
                dim_wrapped("%u binding(s), %u draws, up to %u targets at once, slot %u%s", t.l_binds, t.l_draws, t.l_mrt, t.l_slot, t.l_depth ? ", with depth" : "");
            else if (t.l_dispatches > 0)
                dim_wrapped("written by %u compute dispatch(es)", t.l_dispatches);
            else
                dim_wrapped("not drawn to in the last frame");
            if (t.an.done && t.an.error.empty())
            {
                ImGui::TextWrapped("%s", t.an.label.c_str());
                for (int f = 0; f < FEED_COUNT; f++)
                {
                    if (!UI_MATERIAL && f == FEED_MATERIAL)
                        continue;
                    const float v = t.an.score(f);
                    if (v <= 0.0f && !(best[f].have && best[f].res == t.res))
                        continue;                                           // only the scores that matter for this texture
                    score_row(FEED_LABEL[f], v, best[f].have && best[f].res == t.res);
                }
                dim_wrapped("unit vectors %.0f%%, above 1: %.0f%%, mean brightness %.2f", t.an.unit_frac * 100.0f, t.an.hdr_frac * 100.0f, t.an.luma_mean);
            }
            else if (!t.an.error.empty())
                ImGui::TextColored(COL_WARN, "%s", t.an.error.c_str());
            else if (t.rb_state == 1 || t.rb_state == 2)
                ImGui::TextColored(COL_DIM, "analysing...");
            else
                ImGui::TextDisabled("not analysed (Take snapshot)");
            // Use as: buttons that wrap to the next line when the window is narrow
            const float x_end = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            bool first = true;
            for (int f = 0; f < FEED_COUNT; f++)
            {
                if (!UI_MATERIAL && f == FEED_MATERIAL)
                    continue;
                const float w = ImGui::CalcTextSize(FEED_LABEL[f]).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                if (!first && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w < x_end)
                    ImGui::SameLine();
                first = false;
                if (ImGui::Button(FEED_LABEL[f]))
                    assign_texture_feed(f, t);
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (items.empty())
            ImGui::TextDisabled("No textures in this size class were drawn to in the last frame. Try \"All render targets\".");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------------------------------------------------
static void draw_keys(effect_runtime *rt)
{
    int capture;
    KeyBind keys[KA_COUNT];
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        capture = g.key_capture;
        for (int i = 0; i < KA_COUNT; i++)
            keys[i] = g.keys[i];
    }
    if (capture >= 0)
    {
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            g.key_capture_frame = g.frame;
        }
        // Waiting for a key: Escape cancels, Backspace / Delete unbinds. Mouse buttons and lone modifiers are ignored.
        const uint32_t k = rt->last_key_pressed();
        const bool modifier = k == VK_CONTROL || k == VK_SHIFT || k == VK_MENU || k == VK_LCONTROL || k == VK_RCONTROL || k == VK_LSHIFT ||
                              k == VK_RSHIFT || k == VK_LMENU || k == VK_RMENU || k == VK_LWIN || k == VK_RWIN;
        if (k > 0x06 && !modifier)
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            if (k == VK_ESCAPE) {}
            else if (k == VK_BACK || k == VK_DELETE)
                g.keys[capture] = KeyBind();
            else
            {
                KeyBind b;
                b.key = k;
                b.ctrl = rt->is_key_down(VK_CONTROL);
                b.shift = rt->is_key_down(VK_SHIFT);
                b.alt = rt->is_key_down(VK_MENU);
                g.keys[capture] = b;
            }
            g.key_capture = -1;
            g.dirty = true;
        }
        ImGui::TextColored(COL_WARN, "Press a key (with Ctrl / Shift / Alt if you like) for \"%s\".  Esc = cancel, Backspace = unbind.", KA_LABEL[capture]);
    }
    else
        ImGui::TextColored(COL_DIM, "Click a key to change it. All keys work with the overlay closed. Avoid ReShade's own overlay key.");

    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
        ImGui::TableHeadersRow();
        for (int a = 0; a < KA_COUNT; a++)
        {
            ImGui::TableNextRow();
            ImGui::PushID(a);
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s%s", KA_LABEL[a], KA_REPEAT[a] ? "  (repeats while held)" : "");
            ImGui::TableSetColumnIndex(1);
            const std::string name = capture == a ? std::string("press a key...") : key_name(keys[a]);
            if (ImGui::Button(name.c_str(), ImVec2(-FLT_MIN, 0.0f)))
            {
                std::lock_guard<std::mutex> lock(g.mtx);
                g.key_capture = a;
                g.key_capture_frame = g.frame;
            }
            // the same key on two actions
            for (int b = 0; b < KA_COUNT; b++)
                if (b != a && keys[b].key != 0 && keys[b].key == keys[a].key && keys[b].ctrl == keys[a].ctrl && keys[b].shift == keys[a].shift &&
                    keys[b].alt == keys[a].alt)
                {
                    tip("This key is also bound to another action: both run.");
                    break;
                }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("default"))
            {
                std::lock_guard<std::mutex> lock(g.mtx);
                g.keys[a] = KA_DEFAULT[a];
                g.dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset all keys"))
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        for (int i = 0; i < KA_COUNT; i++)
            g.keys[i] = KA_DEFAULT[i];
        g.key_capture = -1;
        g.dirty = true;
    }
    ImGui::SetNextItemWidth(fit(12.0f));
    edit_setting(g.repeat_delay_ms, [](int &v) { return ImGui::SliderInt("Hold delay before repeating (ms)", &v, 100, 1500); });
    ImGui::SetNextItemWidth(fit(12.0f));
    edit_setting(g.repeat_rate_ms, [](int &v) { return ImGui::SliderInt("Repeat interval (ms)", &v, 15, 500); });
    tip("While a repeating key is held: the layer selection moves one row per interval.");
}

// ---------------------------------------------------------------------------------------------------------------------
// Debug, Options
// ---------------------------------------------------------------------------------------------------------------------
static void draw_debug(effect_runtime *rt)
{
    edit_setting(g.verbose_log, [](bool &v) { return ImGui::Checkbox("Verbose log (ReShade.log)", &v); });
    uint64_t ninj, nref, nci, ncb, neof, frame, inj_frame;
    std::string trk, status;
    size_t cat;
    int rec;
    uint32_t lists;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        ninj = g.n_injections; nref = g.n_inject_refused; nci = g.n_captures_interval; ncb = g.n_captures_barrier; neof = g.n_eof_feed_frames;
        frame = g.frame; inj_frame = g.injected_frame; trk = g.tracker_report; status = g.status; cat = g.catalog.size(); rec = g.rec_left;
        lists = g.last_list_count;
    }
    ImGui::TextWrapped("Frame %llu   forced renders %llu (refused %llu), last %s", static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(ninj), static_cast<unsigned long long>(nref),
                inj_frame == ~0ull ? "never" : (std::to_string(frame - inj_frame) + " frame(s) ago").c_str());
    ImGui::TextWrapped("Feed captures: %llu at binding ends, %llu at the game's barriers (Vulkan); %llu frames with feeds bound at the end of the frame",
                static_cast<unsigned long long>(nci), static_cast<unsigned long long>(ncb), static_cast<unsigned long long>(neof));
    ImGui::TextWrapped("Layer catalog: %zu layers%s, %u command list(s) in the last frame", cat, rec > 0 ? " (recording)" : "", lists);
    if (rt->get_device()->get_api() == device_api::d3d12)
        ImGui::TextWrapped("D3D12 state tracker at the forced layer: %s", trk.empty() ? "not used yet" : trk.c_str());
    if (!status.empty())
        ImGui::TextColored(COL_WARN, "Last status: %s", status.c_str());

    section("What the shader receives (the add-on's copies)");
    DevState *ds = rt->get_device()->get_private_data<DevState>();
    if (ds != nullptr)
    {
        const float w0 = std::min(ImGui::GetFontSize() * 11.0f, ImGui::GetContentRegionAvail().x * 0.48f);
        int per_row = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (w0 + ImGui::GetStyle().ItemSpacing.x)));
        for (int i = 0; i < FEED_COUNT; i++)
        {
            if (!UI_MATERIAL && i == FEED_MATERIAL)
                continue;
            resource_view v = { 0 };
            uint32_t w = 0, h = 0;
            uint64_t bound = ~0ull;
            {
                std::lock_guard<std::mutex> lock(ds->feed_mutex);
                v = ds->feed[i].srv;
                w = ds->feed[i].w;
                h = ds->feed[i].h;
                bound = ds->feed[i].bound_frame;
            }
            if (i % per_row != 0) ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(bound != ~0ull && bound + 2 >= frame ? COL_OK : COL_DIM, "%s", FEED_LABEL[i]);
            ImGui::TextColored(COL_DIM, "%s", FEED_SEMANTIC[i]);
            if (v.handle) thumb(v, w, h, w0);
            else ImGui::Dummy(ImVec2(w0, w0 * 0.56f));
            ImGui::EndGroup();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
    ImGui::TextWrapped("Other effects can read the same textures: texture2D t : NORMAL;  : CRCGI_ALBEDO;  : CRCGI_MOTION;  "
                       ": CRCGI_HDRSCENE;  : CRCGI_SCENE;. In the shader, Debug: G-Buffer & Canvas shows each input, the normal fit, the "
                       "off-screen canvas and the light that comes from it alone.");
    ImGui::PopStyleColor();
}

// Shareable target listings: crcgi_targets*.ini next to the game's executable.
static std::string s_share_msg;

static std::string exe_stem()
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string name = exe;
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

static void draw_share()
{
    std::string author, notes;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        author = g.profile_author;
        notes = g.profile_notes;
    }
    char abuf[128], nbuf[420];
    std::snprintf(abuf, sizeof(abuf), "%s", author.c_str());
    std::snprintf(nbuf, sizeof(nbuf), "%s", notes.c_str());
    ImGui::SetNextItemWidth(fit(18.0f));
    if (ImGui::InputText("Author", abuf, sizeof(abuf)))
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.profile_author = abuf;
        g.dirty = true;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##notes", "notes shown to whoever imports this listing (game version, API, what was checked)", nbuf, sizeof(nbuf)))
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.profile_notes = nbuf;
        g.dirty = true;
    }
    const std::filesystem::path dir = module_directory();
    if (ImGui::Button("Export the target listing"))
    {
        std::string err;
        std::lock_guard<std::mutex> lock(g.mtx);
        const std::filesystem::path file = dir / (L"crcgi_targets_" + std::filesystem::path(exe_stem()).wstring() + L".ini");
        s_share_msg = write_file_locked(file, true, err) ? "Written: " + file.filename().u8string() : err;
    }
    tip("Writes crcgi_targets_<game>.ini next to the executable: the feeds (textures by format, usage, creation order and size relative to "
        "the screen, so they are found again at another screen resolution), the level, the upscaler setting and your notes. No keys, no "
        "overlay settings, no forced-render layer. Share it together with a preset.");
    dim_wrapped("To use a listing someone else made: copy their crcgi_targets*.ini next to the game's executable, press Import.");
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!dir.empty())
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::filesystem::path f = it->path();
            if (f.extension() == L".ini" && f.filename().wstring().rfind(L"crcgi_targets", 0) == 0)
                files.push_back(f);
            if (files.size() >= 16)
                break;
        }
    for (size_t i = 0; i < files.size(); i++)
    {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button("Import"))
        {
            std::string err;
            std::lock_guard<std::mutex> lock(g.mtx);
            if (read_config_locked(files[i], true, err))
            {
                g.dirty = true;
                if (g.level < LEVEL_C) g.level = LEVEL_C;
                s_share_msg = "Imported " + files[i].filename().u8string() + ". The textures are matched as the game creates them.";
            }
            else
                s_share_msg = err;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(files[i].filename().u8string().c_str());
        ImGui::PopID();
    }
    if (files.empty())
        ImGui::TextDisabled("No crcgi_targets*.ini found next to the executable.");
    if (!s_share_msg.empty())
        dim_wrapped("%s", s_share_msg.c_str());
}

static void draw_options(effect_runtime *rt)
{
    if (ImGui::CollapsingHeader("Share this game's target listing", ImGuiTreeNodeFlags_DefaultOpen))
        draw_share();
    edit_setting(g.cross_list_feeds, [](bool &v) { return ImGui::Checkbox("Accept feeds recorded on another command list", &v); });
    tip("Direct3D 12 games record the G-buffer and the lighting on different command lists. On: their copies are used anyway (the GPU "
        "order is the game's submission order, which is G-buffer first in practice).");
    if (ImGui::CollapsingHeader("Force render (advanced)"))
    {
        dim_wrapped("The default is to let ReShade run the effect at the end of the frame and to use the finished-frame feed to leave the UI "
                    "and fog alone. Force render instead makes the add-on render the effect INSIDE the frame, right before the layer with the "
                    "role \"Force render here\" (Layers tab), with the depth buffer in use there. It changes the game's command stream, so "
                    "it is the one place where a game can misbehave: use it only when the default cannot do what you need.");
        edit_setting(g.force_inject, [](bool &v) { return ImGui::Checkbox("Force render at the chosen layer", &v); });
        edit_setting(g.fallback_end, [](bool &v) { return ImGui::Checkbox("Fall back to the end of the frame when the layer is not drawn", &v); });
        tip("Menus and loading screens often skip the layer. On: the effect then runs at the end of the frame. Off: not at all.");
        edit_setting(g.depth_live, [](bool &v) { return ImGui::Checkbox("Hand the depth buffer in use at the layer to the shader", &v); });
        edit_setting(g.d3d12_inject, [](bool &v) { return ImGui::Checkbox("Direct3D 12: allow it (experimental)", &v); });
        tip("The add-on records the game's Direct3D 12 state and restores it after the effect. If a game crashes as soon as a layer is "
            "chosen, switch this off. Layers drawn inside a render pass, or whose state cannot be fully recorded, are refused with the reason.");
        draw_techniques(rt);
    }
    if (ImGui::CollapsingHeader("Performance"))
    {
        dim_wrapped("What the add-on does for performance: it hands the shader the game's normals, albedo and motion vectors, which "
                    "removes the normal rebuild, the reflectance estimate and the motion estimation (G-Buffer tab, 'Shader work "
                    "saved'). In the shader, Shadow Contact Bias 0 skips the contact passes, and Far Cascades: "
                    "Half-Rate Update halves the cost of the long cascades.");
    }
    if (ImGui::CollapsingHeader("Build"))
        dim_wrapped("CRCGI Injector " CRCGI_ADDON_VERSION ", built against ReShade revision %s (ImGui %s), %s.", CRCGI_RESHADE_REVISION, IMGUI_VERSION,
                    sizeof(void *) == 8 ? "64-bit" : "32-bit");
}

static void draw_overlay(effect_runtime *rt)
{
    draw_status(rt);
    ImGui::Separator();
    if (ImGui::BeginTabBar("crcgi_tabs"))
    {
        if (ImGui::BeginTabItem("Setup")) { draw_setup(rt); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("G-Buffer")) { draw_feeds(rt); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Layers")) { draw_layers(rt); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Profiler")) { draw_profiler(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Keys")) { draw_keys(rt); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Debug")) { draw_debug(rt); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Options")) { draw_options(rt); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if (ImGui::GetIO().WantTextInput)
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.typing_frame = g.frame;                   // typing into the layer filter must not move the selection
    }
}
