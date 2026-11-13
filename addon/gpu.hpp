// CRCGI Injector - GPU side: feed textures, injection, shader uniforms, techniques.
#pragma once
#include "tracker.hpp"

static bool explicit_states(device *d) { return d->get_api() == device_api::d3d12 || d->get_api() == device_api::vulkan; }

// ---------------------------------------------------------------------------------------------------------------------
// Techniques and uniforms of the runtime
// ---------------------------------------------------------------------------------------------------------------------
template <typename Fn>
static std::string query_string(Fn fn)
{
    size_t size = 0;
    fn(nullptr, &size);
    if (size == 0)
        return std::string();
    std::string s(size, '\0');
    fn(s.data(), &size);
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static void refresh_techniques(effect_runtime *rt)
{
    std::vector<TechInfo> list;
    rt->enumerate_techniques(nullptr, [&](effect_runtime *r, effect_technique tech) {
        TechInfo ti;
        ti.handle = tech;
        std::string tname = query_string([&](char *buf, size_t *sz) { r->get_technique_name(tech, buf, sz); });
        std::string ename = query_string([&](char *buf, size_t *sz) { r->get_technique_effect_name(tech, buf, sz); });
        ti.key = ename + "|" + tname;
        list.push_back(ti);
    });
    std::lock_guard<std::mutex> lock(g.mtx);
    g.techs = std::move(list);
    g.suppressed.clear();                       // handles are invalid after a reload (the runtime re-creates the techniques)
    if (g.techniques.empty())   // first run: pick the CRCGI technique(s) automatically
    {
        for (const TechInfo &t : g.techs)
            if (lower(t.key).find("crcgi") != std::string::npos)
                g.techniques.push_back(t.key);
        g.dirty = true;
    }
}

static void refresh_uniforms(effect_runtime *rt)
{
    std::unordered_map<std::string, effect_uniform_variable> map;
    rt->enumerate_uniform_variables(nullptr, [&](effect_runtime *r, effect_uniform_variable v) {
        std::string name = query_string([&](char *buf, size_t *sz) { r->get_uniform_variable_name(v, buf, sz); });
        if (!name.empty())
            map[name] = v;
    });
    std::lock_guard<std::mutex> lock(g.mtx);
    g.uniforms = std::move(map);
    g.uniform_last.clear();
}

static bool uniform_found(const char *name)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    return g.uniforms.count(name) != 0;
}

// Sets a shader uniform by name when it exists and its value changed (so the user's own edits are not overwritten every frame
// unless the add-on is actively driving that value). The runtime call is made without holding g.mtx.
static void set_uniform(effect_runtime *rt, const char *name, double value, int kind /*0 bool, 1 int, 2 float*/)
{
    effect_uniform_variable v = { 0 };
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        auto it = g.uniforms.find(name);
        if (it == g.uniforms.end())
            return;
        v = it->second;
        auto last = g.uniform_last.find(name);
        if (last != g.uniform_last.end() && last->second == value)
            return;
        g.uniform_last[name] = value;
    }
    if (kind == 0)
        rt->set_uniform_value_bool(v, value != 0.0);
    else if (kind == 1)
    {
        int32_t i = static_cast<int32_t>(value);
        rt->set_uniform_value_int(v, &i, 1);
    }
    else
    {
        float f = static_cast<float>(value);
        rt->set_uniform_value_float(v, &f, 1);
    }
}

// Managed techniques, in ReShade's order. Call with g.mtx held.
static std::vector<effect_technique> resolve_techniques_locked()
{
    std::vector<effect_technique> out;
    for (const TechInfo &t : g.techs)
        if (contains(g.techniques, t.key))
            out.push_back(t.handle);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Resource states (explicit-state APIs). Only states the game itself established on this command list are used; the bound render
// targets of the current binding interval must be in the render-target state for the game's own draws to be valid.
// ---------------------------------------------------------------------------------------------------------------------
static bool tracked_state(const CmdState &cs, resource res, resource_usage &out)
{
    auto it = cs.states.find(res.handle);
    if (it == cs.states.end())
        return false;
    out = it->second;
    return true;
}

static resource_usage bound_rt_state(const CmdState &cs, resource res)
{
    resource_usage s;
    return tracked_state(cs, res, s) ? s : resource_usage::render_target;
}

// ---------------------------------------------------------------------------------------------------------------------
// Feed textures: a private copy of a game render target, kept for the shader to sample
// ---------------------------------------------------------------------------------------------------------------------
// Immediate destruction: only when the device is being destroyed (nothing is in flight any more).
static void destroy_tex(device *dev, FeedTex &t)
{
    if (t.srv.handle) dev->destroy_resource_view(t.srv);
    if (t.srv_srgb.handle && t.srv_srgb.handle != t.srv.handle) dev->destroy_resource_view(t.srv_srgb);
    if (t.res.handle) dev->destroy_resource(t.res);
    t = FeedTex();
}

// Normal destruction: a copy may still be referenced by command lists the GPU has not executed yet (D3D12 / Vulkan record ahead),
// so the texture is kept for a few frames before it is destroyed.
static void bury_tex(FeedTex &t)
{
    if (t.res.handle != 0 || t.srv.handle != 0)
    {
        Globals::Grave gr;
        gr.res = t.res;
        gr.v1 = t.srv;
        gr.v2 = (t.srv_srgb.handle != t.srv.handle) ? t.srv_srgb : resource_view{ 0 };
        std::lock_guard<std::mutex> lock(g.mtx);
        gr.frame = g.frame;
        g.graveyard.push_back(gr);
    }
    t = FeedTex();
}

static void bury_resource(resource r)
{
    if (r.handle == 0)
        return;
    Globals::Grave gr;
    gr.res = r;
    std::lock_guard<std::mutex> lock(g.mtx);
    gr.frame = g.frame;
    g.graveyard.push_back(gr);
}

static void flush_graveyard(device *dev, bool all)
{
    std::vector<Globals::Grave> due;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        for (size_t i = 0; i < g.graveyard.size();)
        {
            if (all || g.graveyard[i].frame + 8 <= g.frame)
            {
                due.push_back(g.graveyard[i]);
                g.graveyard[i] = g.graveyard.back();
                g.graveyard.pop_back();
            }
            else
                i++;
        }
    }
    for (const Globals::Grave &gr : due)
    {
        if (gr.v1.handle) dev->destroy_resource_view(gr.v1);
        if (gr.v2.handle) dev->destroy_resource_view(gr.v2);
        if (gr.res.handle) dev->destroy_resource(gr.res);
    }
}

// The copy is created TYPELESS (where the format has a typeless family) so both a linear and an sRGB view can exist; the shader
// gets the one matching what the feed means (see bind_feed).
static bool ensure_tex(device *dev, FeedTex &t, const resource_desc &src)
{
    if (t.res.handle != 0 && t.w == src.texture.width && t.h == src.texture.height && t.fmt == src.texture.format)
        return true;
    std::string keep_status = t.status;
    bury_tex(t);
    t.status = keep_status;
    const format typeless = format_to_typeless(src.texture.format);
    const resource_usage usage = resource_usage::shader_resource | resource_usage::copy_dest | resource_usage::resolve_dest;
    resource_desc d(src.texture.width, src.texture.height, 1, 1, typeless, 1, memory_heap::default_, usage);
    if (!dev->create_resource(d, nullptr, resource_usage::shader_resource, &t.res))
    {
        d.texture.format = src.texture.format;
        if (!dev->create_resource(d, nullptr, resource_usage::shader_resource, &t.res))
        {
            t = FeedTex();
            t.status = "could not create the copy texture";
            return false;
        }
    }
    if (!dev->create_resource_view(t.res, resource_usage::shader_resource, resource_view_desc(format_to_default_typed(src.texture.format, 0)), &t.srv))
        t.srv = { 0 };
    if (!dev->create_resource_view(t.res, resource_usage::shader_resource, resource_view_desc(format_to_default_typed(src.texture.format, 1)), &t.srv_srgb))
        t.srv_srgb = t.srv;
    t.w = src.texture.width;
    t.h = src.texture.height;
    t.fmt = src.texture.format;
    if (t.srv.handle == 0)
        t.status = "could not create a shader view of the copy";
    return t.srv.handle != 0;
}

// Copies (or, for an MSAA albedo target, resolves) a game texture into the feed texture. `feed` = -1 for the preview / profiler.
// src_state: the state the texture is in at this point of the command list (explicit-state APIs; it is put back into exactly that).
static bool copy_into(command_list *cl, resource_usage src_state, FeedTex &t, const RtSlot &src, int feed)
{
    device *dev = cl->get_device();
    resource_desc sd = dev->get_resource_desc(src.res);
    if (sd.type != resource_type::texture_2d)
    {
        t.status = "not a 2D texture";
        return false;
    }
    const bool msaa = sd.texture.samples > 1;
    // A resolve averages the samples. That is a valid reduction for colour (albedo, the preview), not for motion vectors or packed
    // normals: those feeds are reported as unsupported rather than silently fed with averaged data.
    if (msaa && feed != FEED_ALBEDO && feed != FEED_HDR && feed != -1)
    {
        t.status = "multisampled target: not supported for this feed (a resolve would average vectors / packed values)";
        return false;
    }
    if (!ensure_tex(dev, t, sd))
        return false;
    const bool ex = explicit_states(dev);
    const resource_usage src_use = msaa ? resource_usage::resolve_source : resource_usage::copy_source;
    const resource_usage dst_use = msaa ? resource_usage::resolve_dest : resource_usage::copy_dest;
    if (ex)
    {
        cl->barrier(src.res, src_state, src_use);
        cl->barrier(t.res, resource_usage::shader_resource, dst_use);
    }
    if (msaa)
        cl->resolve_texture_region(src.res, 0, nullptr, t.res, 0, 0, 0, 0, format_to_default_typed(sd.texture.format, 0));
    else
        cl->copy_resource(src.res, t.res);
    if (ex)
    {
        cl->barrier(src.res, src_use, src_state);                  // back to exactly the state it was in
        cl->barrier(t.res, dst_use, resource_usage::shader_resource);
    }
    t.srgb_source = fmt_is_srgb(src.view_fmt) || fmt_is_srgb(src.fmt);
    t.frame = g.frame;
    t.cl = reinterpret_cast<uintptr_t>(cl);
    t.status.clear();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------------------------------------------------
static ViewPair get_rtv_pair(device *dev, resource_view bound, resource res, const resource_desc &rd, DevState &ds)
{
    std::lock_guard<std::mutex> lock(ds.view_mutex);
    auto it = ds.rtv_cache.find(bound.handle);
    if (it != ds.rtv_cache.end())
        return it->second;
    ViewPair vp;
    vp.owner = res.handle;
    vp.linear = bound;
    vp.srgb = bound;
    resource_view v = { 0 };
    if (dev->create_resource_view(res, resource_usage::render_target, resource_view_desc(format_to_default_typed(rd.texture.format, 0)), &v))
        vp.linear = v;
    resource_view s = { 0 };
    if (dev->create_resource_view(res, resource_usage::render_target, resource_view_desc(format_to_default_typed(rd.texture.format, 1)), &s))
        vp.srgb = s;
    ds.rtv_cache[bound.handle] = vp;
    return vp;
}

// A depth buffer can be sampled only if it is a single-sampled 2D texture created with shader-resource usage and a format family
// that has a colour view (D32 -> R32F, D24S8 -> R24 UNORM X8, D16 -> R16 UNORM, D32S8 -> R32F X8). Unsupported cases are reported.
static DepthSrv get_depth_srv(device *dev, resource depth_res, DevState &ds)
{
    std::lock_guard<std::mutex> lock(ds.view_mutex);
    auto it = ds.depth_srv.find(depth_res.handle);
    if (it != ds.depth_srv.end())
        return it->second;
    DepthSrv out;
    resource_desc dd = dev->get_resource_desc(depth_res);
    if (dd.type != resource_type::texture_2d)
        out.reason = "the depth buffer is not a 2D texture";
    else if (dd.texture.samples > 1)
        out.reason = "the depth buffer is multisampled (cannot be read as a plain 2D texture)";
    else if (!has_usage(dd.usage, resource_usage::shader_resource))
        out.reason = "the game created the depth buffer without shader access: it cannot be sampled";
    else
    {
        const format view_fmt = format_to_default_typed(format_to_typeless(dd.texture.format), 0);
        if (view_fmt == format::unknown || view_fmt == format_to_typeless(dd.texture.format))
            out.reason = std::string("no colour view for depth format ") + fmt_name(dd.texture.format);
        else if (!dev->create_resource_view(depth_res, resource_usage::shader_resource, resource_view_desc(view_fmt), &out.view))
        {
            out.view = { 0 };
            out.reason = std::string("the driver refused a shader view of the depth buffer (format ") + fmt_name(dd.texture.format) + ")";
        }
    }
    ds.depth_srv[depth_res.handle] = out;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------------------------------------------------
static resource choose_depth(const CmdState &cs)
{
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (g.role_ref[ROLE_DEPTH].valid())
            for (const Pass &p : g.cur)
                if (p.key == g.role_ref[ROLE_DEPTH].key && p.depth.handle != 0)
                    return p.depth;
    }
    if (cs.dsv.handle != 0)
        return cs.dsv;
    if (cs.last_dsv.handle != 0 && cs.last_dsv_frame == g.frame)
        return cs.last_dsv;
    return resource{ 0 };
}

// The view a feed is sampled through: albedo written through an sRGB view is sampled through the sRGB view (decoded to linear).
// Normals and motion vectors are linear data and always go through the plain view, never decoded.
static resource_view feed_view(const FeedTex &ft, int i)
{
    return (i == FEED_ALBEDO && ft.srgb_source) ? ft.srv_srgb : ft.srv;
}

// ReShade rewrites descriptors on every update_texture_bindings; it is called only when the bound view changes.
static void set_semantic(effect_runtime *rt, int i, resource_view v)
{
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (g.eof_bound_valid[i] && g.eof_bound[i] == v)
            return;
        g.eof_bound[i] = v;
        g.eof_bound_valid[i] = true;
    }
    rt->update_texture_bindings(FEED_SEMANTIC[i], v, v);
}

// Injection point: a feed is used only if it was captured THIS frame, before this point, on this command list (or the user accepted
// feeds from other lists, whose order relative to this one is not guaranteed).
static void bind_feed(effect_runtime *rt, command_list *cl, DevState &ds, int i, bool wanted, bool cross_lists)
{
    resource_view v = { 0 };
    {
        std::lock_guard<std::mutex> lock(ds.feed_mutex);
        FeedTex &ft = ds.feed[i];
        bool ok = wanted && ft.srv.handle != 0 && ft.frame == g.frame;
        if (wanted && ok && ft.cl != reinterpret_cast<uintptr_t>(cl) && !cross_lists)
        {
            ok = false;
            ft.status = "captured on another command list: order not guaranteed (see Options)";
        }
        else if (wanted && !ok && ft.status.empty())
            ft.status = ft.srv.handle == 0 ? "not captured yet" : "not captured this frame before the injection layer";
        if (ok)
        {
            v = feed_view(ft, i);
            ft.bound_frame = g.frame;
        }
    }
    set_semantic(rt, i, v);
}

// End of the frame (every API, also without in-frame injection): the latest capture is used if it is recent (a feed whose layer or
// texture stopped being drawn, e.g. in menus, is unbound after a few frames). GPU order: the game's command lists that hold the
// copies are submitted before the present, so the end-of-frame render sees the copy of this frame.
static void bind_feeds_end_of_frame(effect_runtime *rt)
{
    DevState *ds = rt->get_device()->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    bool want[FEED_COUNT];
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        for (int i = 0; i < FEED_COUNT; i++)
            want[i] = g.master && g.level >= LEVEL_C && api_supports_feeds(g.api) && g.feed[i].enabled && g.feed[i].assigned();
    }
    bool any = false;
    for (int i = 0; i < FEED_COUNT; i++)
    {
        resource_view v = { 0 };
        {
            std::lock_guard<std::mutex> lock(ds->feed_mutex);
            FeedTex &ft = ds->feed[i];
            if (want[i] && ft.srv.handle != 0 && ft.frame != ~0ull && ft.frame + 3 >= g.frame)
            {
                v = feed_view(ft, i);
                ft.bound_frame = g.frame;
                any = true;
            }
            else if (want[i] && ft.status.empty())
                ft.status = ft.srv.handle == 0 ? "not captured yet: the assigned layer / texture was not drawn" : "not captured in the last frames";
        }
        set_semantic(rt, i, v);
    }
    if (any)
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.n_eof_feed_frames++;
    }
}

// Renders the managed, user-enabled techniques onto the render target bound by the pass that is about to draw.
static bool inject(command_list *cl, CmdState &cs)
{
    effect_runtime *rt = g.runtime;
    if (rt == nullptr || cs.rtv[0].handle == 0 || cs.rt[0].res.handle == 0)
        return false;

    std::vector<effect_technique> techs;
    int level;
    bool depth_live, cross_lists;
    bool feed_on[FEED_COUNT];
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        techs = resolve_techniques_locked();
        level = g.level;
        depth_live = g.depth_live;
        cross_lists = g.cross_list_feeds;
        for (int i = 0; i < FEED_COUNT; i++)
            feed_on[i] = level >= LEVEL_C && g.feed[i].enabled && g.feed[i].assigned();   // a layer OR a profiler texture
    }
    // Respect the user's own technique switch in ReShade: a technique turned off there is not rendered by the add-on either.
    techs.erase(std::remove_if(techs.begin(), techs.end(), [&](effect_technique t) { return !rt->get_technique_state(t); }), techs.end());
    if (techs.empty())
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.status = "No enabled technique to render: tick the CRCGI technique under Techniques and enable it in ReShade.";
        g.n_inject_refused++;
        log_once(reshade::log::level::warning, g.status);
        return false;
    }

    const RtSlot &c0 = cs.rt[0];
    if (c0.samples > 1)
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.status = "The layer's target is multisampled: ReShade cannot render effects into it.";
        log_once(reshade::log::level::warning, g.status);
        return false;
    }
    if (c0.w != g.bb_w || c0.h != g.bb_h || !same_format_family(c0.fmt, g.bb_fmt))
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.status = std::string("The layer's target is ") + std::to_string(c0.w) + "x" + std::to_string(c0.h) + " " + fmt_name(c0.fmt) +
                   ", the back buffer is " + std::to_string(g.bb_w) + "x" + std::to_string(g.bb_h) + " " + fmt_name(g.bb_fmt) +
                   ": the effect can only be injected into a target of the back buffer's size and format family.";
        log_once(reshade::log::level::warning, g.status);
        return false;
    }

    device *dev = cl->get_device();
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return false;
    const bool ex = explicit_states(dev);
    if (cs.in_render_pass)
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.status = "The chosen layer is drawn inside a render pass (BeginRenderPass): effects cannot be rendered there. Pick another layer.";
        g.n_inject_refused++;
        log_once(reshade::log::level::warning, g.status);
        return false;
    }
    if (ex)
    {
        std::string why;
        if (!tracker_check(cs, *ds, why))
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            g.status = "D3D12: not injected at this layer, the game's state could not be fully recorded: " + why + ". Pick another layer.";
            g.tracker_report = why;
            g.n_inject_refused++;
            log_once(reshade::log::level::warning, g.status);
            return false;
        }
        std::lock_guard<std::mutex> lock(g.mtx);
        g.tracker_report = "complete: pipeline, root signature (" + std::to_string(cs.trk.gfx.params.size()) + " parameters), heaps, viewports, targets";
    }
    ViewPair vp = get_rtv_pair(dev, cs.rtv[0], c0.res, dev->get_resource_desc(c0.res), *ds);

    std::unique_lock<std::mutex> render_lock(g_runtime_render_mutex, std::try_to_lock);
    if (!render_lock.owns_lock())
        return false;                               // ReShade is rendering the end of the previous frame right now (D3D12): skip once

    // Level C: hand the captured G-buffer feeds to the shader (or clear the binding when a feed is off or not usable)
    for (int i = 0; i < FEED_COUNT; i++)
        bind_feed(rt, cl, *ds, i, feed_on[i], cross_lists);

    // Depth: the depth buffer in use at this point of the frame
    resource depth_res = { 0 };
    resource_usage depth_state = resource_usage::undefined;
    std::string depth_status;
    if (depth_live)
    {
        depth_res = choose_depth(cs);
        if (depth_res.handle == 0)
            depth_status = "no depth buffer bound before this layer in this frame";
        else
        {
            DepthSrv srv = get_depth_srv(dev, depth_res, *ds);
            if (srv.view.handle == 0)
            {
                depth_status = srv.reason;
                depth_res = { 0 };
            }
            else if (ex && !tracked_state(cs, depth_res, depth_state))
            {
                // D3D12: the state of a game resource is never guessed. Without a barrier seen on this command list the current
                // state is unknown, so the depth buffer is not touched this frame.
                depth_status = "depth buffer state unknown on this command list (no barrier seen): not bound";
                depth_res = { 0 };
            }
            else
            {
                if (ex && depth_state != resource_usage::shader_resource)
                    cl->barrier(depth_res, depth_state, resource_usage::shader_resource);
                rt->update_texture_bindings("DEPTH", srv.view, srv.view);
            }
        }
    }

    // render_technique ignores the technique's enabled state (it renders regardless), restores the D3D9/10/11/OpenGL state itself
    // and fires begin/finish_effects, which the add-on ignores while t_injecting is set. D3D12 has no such restore: the state the
    // game had bound on this command list is re-applied from the add-on's own tracker (tracker.hpp).
    t_injecting = true;
    for (effect_technique t : techs)
        rt->render_technique(t, cl, vp.linear, vp.srgb);
    if (ex)
        tracker_apply(cl, cs, *ds);
    t_injecting = false;

    if (depth_res.handle != 0 && ex && depth_state != resource_usage::shader_resource)
        cl->barrier(depth_res, resource_usage::shader_resource, depth_state);   // back to exactly the state it was in

    std::lock_guard<std::mutex> lock(g.mtx);
    if (g.injected_frame == ~0ull)
        log_msg(reshade::log::level::info, std::string("First injection inside the frame succeeded (") + api_name(g.api) + ").");
    g.n_injections++;
    if (!depth_status.empty())
        log_once(reshade::log::level::warning, "Depth: " + depth_status);
    g.injected_frame = g.frame;
    g.injected_float = fmt_is_float(c0.fmt);
    g.status.clear();
    g.depth_status = depth_status;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Duplicate-render prevention. reshade_render_technique is a notification sent AFTER a technique rendered and cannot cancel it,
// so the runtime's own end-of-frame render is prevented differently: right inside it (reshade_begin_effects, which the runtime
// sends before it walks its technique list), the managed techniques that are enabled are switched off, and exactly those are
// switched back on in reshade_finish_effects. The user's switches are therefore unchanged before and after every frame, and the
// preset is not rewritten. Side effect: ReShade's statistics clear the timing of a technique whenever it is switched off.
// ---------------------------------------------------------------------------------------------------------------------
static void suppress_end_of_frame(effect_runtime *rt)
{
    std::vector<effect_technique> techs;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (!g.suppressed.empty())
            return;
        const bool active = g.master && g.level >= LEVEL_B && g.role_ref[ROLE_INJECT].valid() && api_supports_injection(g.api) && g_conflict.empty();
        const bool injected = g.injected_frame == g.frame;
        if (!active || (!injected && g.fallback_end))
            return;                                 // render at the end of the frame as usual (exactly once)
        techs = resolve_techniques_locked();
    }
    std::vector<effect_technique> off;
    for (effect_technique t : techs)
        if (rt->get_technique_state(t))
        {
            rt->set_technique_state(t, false);
            if (!rt->get_technique_state(t))        // another add-on may veto the change
                off.push_back(t);
        }
    std::lock_guard<std::mutex> lock(g.mtx);
    g.suppressed = std::move(off);
    if (!g.suppressed.empty())
        g.eof_suppressed_frames++;
}

static void restore_end_of_frame(effect_runtime *rt)
{
    std::vector<effect_technique> off;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        off.swap(g.suppressed);
    }
    for (effect_technique t : off)
        rt->set_technique_state(t, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Per-frame link to the shader: camera and feed switches. The add-on never writes ConvertTonemapper: the shader's own tonemapper
// conversion control stays entirely in the user's hands, whatever the target format is.
// ---------------------------------------------------------------------------------------------------------------------
static bool feed_ready(int i)
{
    effect_runtime *rt = g.runtime;
    if (!rt)
        return false;
    DevState *ds = rt->get_device()->get_private_data<DevState>();
    if (ds == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(ds->feed_mutex);
    return ds->feed[i].bound_frame != ~0ull && ds->feed[i].bound_frame + 2 >= g.frame;   // handed to the shader in the last frames
}

static void apply_shader_link(effect_runtime *rt)
{
    bool active;
    int level;
    CamCandidate cam;
    bool cam_valid, cam_apply;
    FeedConfig feeds[FEED_COUNT];
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        active = g.master && api_supports_feeds(g.api);
        level = g.level;
        cam = g.cam;
        cam_valid = g.cam_valid;
        cam_apply = g.cam_apply;
        for (int i = 0; i < FEED_COUNT; i++)
            feeds[i] = g.feed[i];
    }
    int geom_source;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        geom_source = g.geom_source;
    }
    if (!active || level < LEVEL_B)
    {
        // Level A / off: the shader is left as the user set it, except that a switch the add-on drives is turned off again (no feed
        // is delivered any more, so a switch left on would read an empty texture).
        for (int i = 0; i < FEED_COUNT; i++)
            if (feeds[i].sync)
                set_uniform(rt, FEED_SWITCH[i], 0, 0);
        return;
    }

    if (cam_apply && cam_valid)
    {
        set_uniform(rt, "FieldOfView", cam.fov, 2);
        set_uniform(rt, "Integration_Near", cam.nearp, 2);
        set_uniform(rt, "Integration_Far", std::isfinite(cam.farp) ? cam.farp : 1.0e9, 2);
        set_uniform(rt, "Integration_Reversed", cam.reversed ? 1 : 0, 0);
    }

    for (int i = 0; i < FEED_COUNT; i++)
        if (feeds[i].sync)
            set_uniform(rt, FEED_SWITCH[i], (level >= LEVEL_C && feeds[i].enabled && feed_ready(i)) ? 1 : 0, 0);
    if (level >= LEVEL_C)
    {
        if (feeds[FEED_NORMALS].sync)
        {
            set_uniform(rt, "Integration_NormalSpace", feeds[FEED_NORMALS].space, 1);
            set_uniform(rt, "Integration_GeometrySource", geom_source, 1);   // skip the depth rebuild where game normals exist
        }
        set_uniform(rt, "Integration_NormalMode", feeds[FEED_NORMALS].mode, 1);
        set_uniform(rt, "Integration_NormalFlipY", feeds[FEED_NORMALS].flip_y ? 1 : 0, 0);
        set_uniform(rt, "Integration_MotionUnits", feeds[FEED_MOTION].units, 1);
        set_uniform(rt, "Integration_MotionFlipY", feeds[FEED_MOTION].flip_y ? 1 : 0, 0);
        set_uniform(rt, "Integration_MotionInvert", feeds[FEED_MOTION].invert ? 1 : 0, 0);
        set_uniform(rt, "Integration_MaterialChannel", feeds[FEED_MATERIAL].channel, 1);
        set_uniform(rt, "Integration_MaterialMeaning", feeds[FEED_MATERIAL].meaning, 1);
    }
}
