// CRCGI Injector - pass tracking, the layer catalog, layer roles, and the ReShade add-on event handlers.
#pragma once
#include "camera.hpp"
#include "profiler.hpp"

// ---------------------------------------------------------------------------------------------------------------------
// Layers = render targets of the binding intervals of a frame. The overlay shows the CATALOG: every layer recorded over many frames,
// kept when it is not drawn for a while, in a fixed order (average position in the frame, sorted once after a recording). So the list
// does not refresh every frame and a selection stays where it is.
// ---------------------------------------------------------------------------------------------------------------------
struct Row
{
    Pass p;
    int slot = 0;
    uint64_t last_seen = 0;
    uint32_t seen = 0;
    float pos = 0.0f;
    LayerKind kind = LK_UNKNOWN;
};

static bool row_useful(const Pass &p, int slot, bool only_useful)
{
    const RtSlot &r = p.rt[slot];
    if (r.res.handle == 0 || r.w == 0)
        return false;
    if (!only_useful)
        return true;
    return static_cast<uint64_t>(r.w) * r.h * 16 >= static_cast<uint64_t>(g.bb_w) * g.bb_h;   // hide shadow maps, bloom mips, ...
}

// What a layer probably is, from how it is drawn. pos = its average position in the frame (0 first .. 1 last).
static LayerKind classify(const Pass &p, float pos)
{
    const RtSlot &r = p.rt[0];
    if (r.res.handle == 0)
        return LK_UNKNOWN;
    const bool depth = p.depth.handle != 0;
    const bool blended = p.draws > 0 && p.blend_draws * 2 >= p.draws;
    if (p.rt_count >= 3 && depth && p.draws >= 8 && !blended)
        return LK_GBUFFER;
    if (p.to_back_buffer || (pos > 0.6f && blended && !depth && same_format_family(r.fmt, g.bb_fmt) && r.w == g.bb_w && r.h == g.bb_h && p.draws >= 3))
        return LK_UI;
    if (!depth && p.draws <= 4 && p.rt_count <= 2)
        return fmt_is_float(r.fmt) && pos < 0.5f ? LK_LIGHTING : LK_POST;
    if (depth && blended)
        return LK_TRANSPARENT;
    if (depth && fmt_is_float(r.fmt) && p.draws < 40 && p.zwrite_draws == 0)
        return LK_LIGHTING;
    if (depth)
        return LK_OPAQUE;
    return LK_UNKNOWN;
}

static int layer_has_role_locked(uint64_t key)
{
    if (g.role_ref[ROLE_INJECT].key == key || g.role_ref[ROLE_DEPTH].key == key)
        return 1;
    for (int i = 0; i < FEED_COUNT; i++)
        if (g.feed[i].source == SRC_LAYER && g.feed[i].ref.key == key)
            return 1;
    return 0;
}

// Display order of the catalog by the chosen sort mode. Frame position is always the last tie-breaker, so equal keys keep the
// frame's order (resolution + frame order = "the 1440p layers in order, then the 960p ones in order", ...).
static void sort_catalog_locked()
{
    struct K { uint64_t key; float pos; uint64_t area; uint32_t draws; uint32_t fmt; int kind; int role; };
    std::vector<K> v;
    v.reserve(g.catalog.size());
    for (const auto &e : g.catalog)
    {
        const LayerInfo &li = e.second;
        K k;
        k.key = e.first;
        k.pos = li.pos;
        k.area = static_cast<uint64_t>(li.p.rt[0].w) * li.p.rt[0].h;
        k.draws = li.p.draws;
        k.fmt = static_cast<uint32_t>(format_to_typeless(li.p.rt[0].fmt));
        k.kind = classify(li.p, li.pos);
        k.role = layer_has_role_locked(e.first);
        v.push_back(k);
    }
    const int mode = g.sort_mode;
    std::stable_sort(v.begin(), v.end(), [mode](const K &a, const K &b) {
        switch (mode)
        {
        case SORT_RES_FRAME: if (a.area != b.area) return a.area > b.area; break;
        case SORT_KIND_FRAME: if (a.kind != b.kind) return a.kind < b.kind; break;
        case SORT_DRAWS: if (a.draws != b.draws) return a.draws > b.draws; break;
        case SORT_FORMAT_FRAME: if (a.fmt != b.fmt) return a.fmt < b.fmt; break;
        case SORT_ROLE_FRAME: if (a.role != b.role) return a.role > b.role; break;
        default: break;
        }
        return a.pos < b.pos;
    });
    g.cat_order.clear();
    for (const auto &e : v)
        g.cat_order.push_back(e.key);
}

static void start_recording(int frames)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    g.rec_left = frames;
    g.rec_total = frames;
}

static std::vector<Row> build_rows()
{
    std::vector<Row> rows;
    std::lock_guard<std::mutex> lock(g.mtx);
    for (uint64_t key : g.cat_order)
    {
        auto it = g.catalog.find(key);
        if (it == g.catalog.end())
            continue;
        const LayerInfo &li = it->second;
        for (uint32_t s = 0; s < li.p.rt_count && s < 8; s++)
            if (row_useful(li.p, static_cast<int>(s), g.only_useful))
            {
                Row r;
                r.p = li.p;
                r.slot = static_cast<int>(s);
                r.last_seen = li.last_seen;
                r.seen = li.seen;
                r.pos = li.pos;
                r.kind = classify(li.p, li.pos);
                rows.push_back(r);
            }
    }
    return rows;
}

static int role_of(const Pass &p, int slot)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    LayerRef me;
    me.key = p.key;
    me.slot = slot;
    for (int i = 0; i < FEED_COUNT; i++)
        if (g.feed[i].source == SRC_LAYER && g.feed[i].ref == me)
            return ROLE_NORMALS + i;
    if (slot == 0 && g.role_ref[ROLE_INJECT].key == p.key) return ROLE_INJECT;
    if (slot == 0 && g.role_ref[ROLE_DEPTH].key == p.key) return ROLE_DEPTH;
    return ROLE_NONE;
}

static void apply_role(const Pass &p, int slot, int role)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    LayerRef me;
    me.key = p.key;
    me.slot = slot;
    // a row holds one role: free whatever it held
    for (int i = 0; i < FEED_COUNT; i++)
        if (g.feed[i].source == SRC_LAYER && g.feed[i].ref == me) { g.feed[i].ref = LayerRef(); g.feed[i].enabled = false; }
    if (slot == 0)
    {
        if (g.role_ref[ROLE_INJECT].key == p.key) g.role_ref[ROLE_INJECT] = LayerRef();
        if (g.role_ref[ROLE_DEPTH].key == p.key) g.role_ref[ROLE_DEPTH] = LayerRef();
    }
    if (role == ROLE_INJECT || role == ROLE_DEPTH)
        g.role_ref[role] = LayerRef{ p.key, 0 };
    else if (role >= ROLE_NORMALS && role < ROLE_COUNT)
    {
        FeedConfig &f = g.feed[role - ROLE_NORMALS];
        f.source = SRC_LAYER;
        f.ref = me;
        f.enabled = true;
        if (g.level < LEVEL_C)
            g.level = LEVEL_C;
    }
    g.dirty = true;
}

// What a target is probably good for. Only a hint: the preview and the profiler's analysis decide.
static std::string hint_for(const Pass &p, int slot)
{
    const RtSlot &r = p.rt[slot];
    bool full = r.w == g.bb_w && r.h == g.bb_h;
    bool render_res = !full && screen_sized(r.w, r.h);
    bool half = r.w * 3 + 3 >= g.bb_w;
    format t = format_to_typeless(r.fmt);
    if (r.samples > 1)
        return "multisampled";
    if (slot == 0 && p.to_back_buffer)
        return "draws to the screen";
    if (p.rt_count >= 2 && half && p.depth.handle != 0)
    {
        if (fmt_is_srgb(r.view_fmt) || fmt_is_srgb(r.fmt))
            return "G-buffer member, sRGB (maybe albedo)";
        if (t == format_to_typeless(format::r16g16_float) || t == format_to_typeless(format::r16g16_unorm))
            return "G-buffer member, 2 channels (motion or packed normals)";
        if (t == format_to_typeless(format::r10g10b10a2_unorm) || t == format_to_typeless(format::r16g16b16a16_float) ||
            t == format_to_typeless(format::r8g8b8a8_unorm))
            return "G-buffer member (normals / albedo / material)";
        return "G-buffer member";
    }
    if (full && same_format_family(r.fmt, g.bb_fmt) && slot == 0)
        return "can be injected into";
    if (fmt_is_float(r.fmt) && (full || render_res) && slot == 0)
        return render_res ? "floating-point colour at render resolution (maybe HDR scene, before the upscaler)" : "floating-point colour (maybe HDR scene)";
    if (render_res)
        return "render resolution (upscaler input)";
    if ((t == format_to_typeless(format::r16g16_float) || t == format_to_typeless(format::r16g16_unorm)) && half)
        return "maybe motion vectors";
    return std::string();
}

// Auto-setup. Injection layer: by the policy (before the UI / post-processing / transparent effects), the first layer of that kind in
// the frame whose target can take the effect (the back buffer's size and format family, single-sampled). Feeds: the profiler's content
// analysis, replacing what was assigned before. The result is reported (Setup tab and log) so nothing happens silently.
static std::string choose_injection_layer(bool apply)
{
    std::vector<Row> rows = build_rows();
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.pos < b.pos; });
    int policy;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        policy = g.inject_policy;
    }
    const LayerKind want = policy == IP_BEFORE_POST ? LK_POST : (policy == IP_BEFORE_TRANSPARENT ? LK_TRANSPARENT : LK_UI);
    auto usable = [](const Row &r) {
        const RtSlot &s0 = r.p.rt[0];
        return r.slot == 0 && s0.samples <= 1 && s0.w == g.bb_w && s0.h == g.bb_h && same_format_family(s0.fmt, g.bb_fmt);
    };
    int pick = -1;
    const char *why = "";
    for (size_t i = 0; i < rows.size() && pick < 0; i++)
        if (rows[i].kind == want && usable(rows[i]))
        {
            pick = static_cast<int>(i);
            why = LK_LABEL[want];
        }
    if (pick < 0 && want != LK_UI)
        for (size_t i = 0; i < rows.size() && pick < 0; i++)
            if (rows[i].kind == LK_UI && usable(rows[i]))
            {
                pick = static_cast<int>(i);
                why = "UI / overlay (no usable layer of the chosen kind)";
            }
    if (pick < 0)
    {
        // Fallback: the last single-target, screen-format layer before the first layer drawn into the back buffer.
        int first_screen = -1;
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i].slot == 0 && rows[i].p.to_back_buffer) { first_screen = static_cast<int>(i); break; }
        for (size_t i = 0; i < rows.size(); i++)
            if (usable(rows[i]) && !rows[i].p.to_back_buffer && (first_screen < 0 || static_cast<int>(i) < first_screen) && rows[i].p.rt_count == 1)
            {
                pick = static_cast<int>(i);
                why = "last screen-format layer before the screen";
            }
    }
    if (pick < 0)
        return "Injection layer: no usable layer found (none has the back buffer's size and format). The effect runs at the end of the frame.";
    if (!api_supports_injection(g.api))
        return std::string("Injection layer: ") + api_name(g.api) + " renders at the end of the frame (no in-frame injection on this API).";
    if (apply)
        apply_role(rows[pick].p, 0, ROLE_INJECT);
    char b[200];
    std::snprintf(b, sizeof(b), "Injection layer: #%u %s %ux%u, before %s.", rows[pick].p.order, fmt_name(rows[pick].p.rt[0].fmt), rows[pick].p.rt[0].w,
                  rows[pick].p.rt[0].h, why);
    return b;
}

static void auto_suggest()
{
    std::string rep = choose_injection_layer(true);
    pool_request_snapshot(true, true);           // feeds: snapshot, analyse, then assign the winners (replacing earlier choices)
    std::lock_guard<std::mutex> lock(g.mtx);
    g.master = true;
    if (g.level < LEVEL_C)
        g.level = LEVEL_C;
    g.auto_report = rep + "  Feeds: analysing...";
    g.dirty = true;
    log_msg(reshade::log::level::info, "Auto-setup: " + rep);
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------------------------------------------------
static void capture_feed(command_list *cl, DevState &ds, int feed, resource_usage state, const RtSlot &src, bool barrier_path)
{
    std::lock_guard<std::mutex> lock(ds.feed_mutex);
    if (copy_into(cl, state, ds.feed[feed], src, feed))
    {
        std::lock_guard<std::mutex> l2(g.mtx);
        if (barrier_path) g.n_captures_barrier++;
        else g.n_captures_interval++;
    }
}

static void end_pass(command_list *cl, CmdState &cs)
{
    struct Todo { int feed; int slot; };
    std::vector<Todo> todo;
    bool prev = false;
    int prev_slot = 0;
    uint32_t order = 0;
    cs.pass_open = false;
    device *dev = cl->get_device();
    const bool can_copy = !cs.in_render_pass && dev->get_api() != device_api::vulkan;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (cs.pass_index >= 0 && static_cast<size_t>(cs.pass_index) < g.cur.size() && g.cur[cs.pass_index].key == cs.pass_key)
        {
            g.cur[cs.pass_index].draws = cs.draws;
            g.cur[cs.pass_index].ps_changes = cs.ps_changes;
            g.cur[cs.pass_index].blend_draws = cs.blend_draws;
            g.cur[cs.pass_index].zwrite_draws = cs.zwrite_draws;
            order = g.cur[cs.pass_index].order;
        }
        if (can_copy && g.master && g.level >= LEVEL_C && api_supports_feeds(g.api))
            for (int i = 0; i < FEED_COUNT; i++)
            {
                const FeedConfig &f = g.feed[i];
                if (!f.enabled)
                    continue;
                if (f.source == SRC_LAYER && f.ref.key == cs.pass_key && f.ref.slot >= 0 && f.ref.slot < static_cast<int>(cs.rt_count))
                    todo.push_back({ i, f.ref.slot });
                else if (f.source == SRC_TEXTURE && g.feed_res[i].handle != 0)
                    for (uint32_t s = 0; s < cs.rt_count && s < 8; s++)
                        if (cs.rt[s].res == g.feed_res[i])
                            todo.push_back({ i, static_cast<int>(s) });
            }
        if (can_copy && g.preview_key == cs.pass_key && g.frame <= g.preview_frame + 2)
        {
            prev = true;
            prev_slot = g.preview_slot;
        }
    }
    pool_note_interval(cs, order);
    if (can_copy)
        for (uint32_t s = 0; s < cs.rt_count && s < 8; s++)
            if (cs.rt[s].res.handle != 0)
                pool_capture(cl, cs.rt[s].res, bound_rt_state(cs, cs.rt[s].res), cs.rt[s].view_fmt);
    if (todo.empty() && !prev)
        return;
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    for (const Todo &t : todo)
        if (cs.rt[t.slot].res.handle != 0)
            capture_feed(cl, *ds, t.feed, bound_rt_state(cs, cs.rt[t.slot].res), cs.rt[t.slot], false);
    if (prev && prev_slot < static_cast<int>(cs.rt_count) && cs.rt[prev_slot].res.handle != 0)
    {
        std::lock_guard<std::mutex> lock(ds->feed_mutex);
        copy_into(cl, bound_rt_state(cs, cs.rt[prev_slot].res), ds->preview, cs.rt[prev_slot], -1);
    }
}

static void begin_pass(command_list *cl, CmdState &cs)
{
    device *dev = cl->get_device();
    Pass p;
    p.rt_count = std::min<uint32_t>(cs.rt_count, 8);
    for (uint32_t i = 0; i < 8; i++)
    {
        cs.rt[i] = RtSlot();
        if (i >= p.rt_count || cs.rtv[i].handle == 0)
            continue;
        resource res = dev->get_resource_from_view(cs.rtv[i]);
        if (res.handle == 0)
            continue;
        resource_desc d = dev->get_resource_desc(res);
        if (d.type != resource_type::texture_2d)
            continue;
        cs.rt[i].res = res;
        cs.rt[i].w = d.texture.width;
        cs.rt[i].h = d.texture.height;
        cs.rt[i].fmt = d.texture.format;
        cs.rt[i].view_fmt = dev->get_resource_view_desc(cs.rtv[i]).format;
        cs.rt[i].samples = d.texture.samples;
        p.rt[i] = cs.rt[i];
    }
    cs.dsv = resource{ 0 };
    if (cs.dsv_view.handle != 0)
        cs.dsv = dev->get_resource_from_view(cs.dsv_view);
    if (cs.dsv.handle != 0)
    {
        cs.last_dsv = cs.dsv;
        cs.last_dsv_frame = g.frame;
    }
    p.depth = cs.dsv;
    p.ps = cs.ps;
    p.vs = cs.vs;
    p.cl = reinterpret_cast<uintptr_t>(cl);
    cs.pass_ps = cs.ps;
    cs.ps_changes = 0;
    cs.blend_draws = 0;
    cs.zwrite_draws = 0;

    // Layer key: shaders + targets (format, size and the texture's run-to-run identity) + the occurrence of that signature in THIS
    // recording. Independent of how command lists interleave on other threads, and of the back buffer index.
    struct KeyData { uint32_t ps, vs, count, depth; uint32_t fmt[8], w[8], h[8]; uint64_t id[8]; } kd;
    std::memset(&kd, 0, sizeof(kd));
    kd.ps = cs.ps; kd.vs = cs.vs; kd.count = p.rt_count; kd.depth = cs.dsv.handle != 0;
    for (int i = 0; i < 8; i++)
    {
        kd.fmt[i] = static_cast<uint32_t>(cs.rt[i].fmt);
        kd.w[i] = cs.rt[i].w;
        kd.h[i] = cs.rt[i].h;
        kd.id[i] = cs.rt[i].res.handle != 0 ? pool_identity(cs.rt[i].res) : 0;
    }
    const uint64_t base = fnv64(&kd, sizeof(kd));
    if (!explicit_states(dev) && cs.occ_frame != g.frame)
    {
        cs.occ.clear();                          // D3D10/11 / OpenGL: one recording per frame
        cs.occ_frame = g.frame;
    }
    const uint32_t occ = cs.occ[base]++;
    uint64_t key = fnv64(&occ, sizeof(occ), base);
    if (key == 0) key = 1;

    bool do_inject = false;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        p.key = key;
        p.order = static_cast<uint32_t>(g.cur.size());
        p.to_back_buffer = p.rt[0].res.handle != 0 && p.rt[0].res.handle == g.bb_res.handle;
        cs.pass_index = static_cast<int>(g.cur.size());
        cs.pass_key = key;
        if (g.cur.size() < 65536)
            g.cur.push_back(p);
        g.cur_lists.insert(p.cl);
        do_inject = g.master && g.level >= LEVEL_B && api_supports_injection(g.api) && g_conflict.empty() && !g.in_eof &&
                    g.role_ref[ROLE_INJECT].key == key && g.injected_frame != g.frame;
    }
    cs.pass_open = true;
    cs.draws = 0;
    if (!do_inject)
        return;
    // D3D10/11: effects are rendered only on the immediate context. A deferred context records on a worker thread and is executed
    // later; rendering ReShade effects into it is not supported and can crash. D3D12: one injection at a time.
    if (!explicit_states(dev))
    {
        command_queue *q = g.runtime != nullptr ? g.runtime->get_command_queue() : nullptr;
        if (q == nullptr || cl != q->get_immediate_command_list())
        {
            std::lock_guard<std::mutex> lock(g.mtx);
            g.status = "The chosen layer is drawn on a deferred (worker-thread) context: the effect cannot be injected there. Pick another layer.";
            g.n_inject_refused++;
            log_once(reshade::log::level::warning, g.status);
            return;
        }
    }
    std::lock_guard<std::mutex> inject_lock(g_inject_mutex);
    inject(cl, cs);
}

static void on_draw_common(command_list *cl)
{
    if (t_injecting || g.runtime == nullptr)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    if (cs->frame != g.frame)
    {
        cs->frame = g.frame;                     // anything still open belongs to an old frame: drop it, never use it as current
        cs->pass_open = false;
    }
    if (!cs->pass_open)
        begin_pass(cl, *cs);
    cs->draws++;
    if (cs->blend) cs->blend_draws++;
    if (cs->zwrite && cs->dsv.handle != 0) cs->zwrite_draws++;
}

// ---------------------------------------------------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------------------------------------------------
static void on_init_device(device *dev)
{
    dev->create_private_data<DevState>();
    const device_api api = dev->get_api();
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.api = api;
    }
    std::string what;
    if (api_supports_injection(api))
        what = api == device_api::d3d12 ? " (in-frame injection: experimental, with the add-on's own D3D12 state tracker; G-buffer feeds available)"
                                        : " (in-frame injection and G-buffer feeds available)";
    else
        what = std::string(" - ") + injection_unsupported_reason(api);
    log_msg(reshade::log::level::info, std::string("Device created: ") + api_name(api) + what);
}

static void release_views(device *dev, DevState &ds)
{
    std::lock_guard<std::mutex> lock(ds.view_mutex);
    for (auto &d : ds.depth_srv)
        if (d.second.view.handle != 0)
            dev->destroy_resource_view(d.second.view);
    ds.depth_srv.clear();
    for (auto &v : ds.rtv_cache)
    {
        if (v.second.linear.handle != 0 && v.second.linear.handle != v.first)
            dev->destroy_resource_view(v.second.linear);
        if (v.second.srgb.handle != 0 && v.second.srgb.handle != v.first && v.second.srgb.handle != v.second.linear.handle)
            dev->destroy_resource_view(v.second.srgb);
    }
    ds.rtv_cache.clear();
}

static void on_destroy_device(device *dev)
{
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(ds->feed_mutex);
        for (int i = 0; i < FEED_COUNT; i++)
            destroy_tex(dev, ds->feed[i]);
        destroy_tex(dev, ds->preview);
    }
    pool_clear(dev);
    flush_graveyard(dev, true);
    release_views(dev, *ds);
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        for (int i = 0; i < FEED_COUNT; i++)
        {
            g.feed_res[i] = resource{ 0 };
            g.eof_bound_valid[i] = false;
        }
    }
    dev->destroy_private_data<DevState>();
}

static void on_init_cmd_list(command_list *cl) { cl->create_private_data<CmdState>(); }
static void on_destroy_cmd_list(command_list *cl) { cl->destroy_private_data<CmdState>(); }

// D3D12 / Vulkan command lists are reset and re-recorded: everything tracked for the previous recording is void.
static void on_reset_cmd_list(command_list *cl)
{
    if (CmdState *cs = cl->get_private_data<CmdState>())
    {
        cs->states.clear();
        cs->occ.clear();
        cs->trk.clear();
        cs->pass_open = false;
        cs->in_render_pass = false;
    }
}

// Barriers: record the state the game put each resource in, and capture feeds / profiler snapshots right after the game itself moves
// a texture out of the render-target state (its state is then known exactly: the barrier's new state). D3D12 and Vulkan.
static void on_barrier(command_list *cl, uint32_t count, const resource *resources, const resource_usage *old_states, const resource_usage *new_states)
{
    if (t_injecting)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr || resources == nullptr || new_states == nullptr)
        return;
    for (uint32_t i = 0; i < count; i++)
        if (resources[i].handle != 0)
            cs->states[resources[i].handle] = new_states[i];
    if (old_states == nullptr || cs->in_render_pass)
        return;
    int feed_hit[FEED_COUNT];
    bool any_feed = false;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        const bool on = g.master && g.level >= LEVEL_C && api_supports_feeds(g.api);
        for (int f = 0; f < FEED_COUNT; f++)
        {
            feed_hit[f] = -1;
            if (!on || !g.feed[f].enabled || g.feed[f].source != SRC_TEXTURE || g.feed_res[f].handle == 0)
                continue;
            for (uint32_t i = 0; i < count; i++)
                if (resources[i] == g.feed_res[f] && has_usage(old_states[i], resource_usage::render_target) &&
                    !has_usage(new_states[i], resource_usage::render_target) && new_states[i] != resource_usage::undefined)
                {
                    feed_hit[f] = static_cast<int>(i);
                    any_feed = true;
                }
        }
    }
    // D3D12: the barrier event cannot tell a split barrier's BEGIN_ONLY half from a full transition, so it only supplies copies that no
    // binding end supplied this frame (games using BeginRenderPass). Vulkan pipeline barriers are never split.
    const bool d3d12 = cl->get_device()->get_api() == device_api::d3d12;
    for (uint32_t i = 0; i < count; i++)
        if (resources[i].handle != 0 && has_usage(old_states[i], resource_usage::render_target) &&
            !has_usage(new_states[i], resource_usage::render_target) && new_states[i] != resource_usage::undefined)
            pool_capture(cl, resources[i], new_states[i], format::unknown, d3d12);
    if (!any_feed)
        return;
    DevState *ds = cl->get_device()->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    device *dev = cl->get_device();
    for (int f = 0; f < FEED_COUNT; f++)
    {
        if (feed_hit[f] < 0)
            continue;
        if (d3d12)
        {
            std::lock_guard<std::mutex> lock(ds->feed_mutex);
            if (ds->feed[f].frame == g.frame)
                continue;                           // already captured at the end of a binding this frame
        }
        const resource res = resources[feed_hit[f]];
        resource_desc d = dev->get_resource_desc(res);
        RtSlot src;
        src.res = res;
        src.w = d.texture.width;
        src.h = d.texture.height;
        src.samples = d.texture.samples;
        src.fmt = d.texture.format;
        {
            std::lock_guard<std::mutex> lock(prof.m);
            auto it = prof.pool.find(res.handle);
            if (it != prof.pool.end())
                src.view_fmt = it->second.view_fmt;
        }
        capture_feed(cl, *ds, f, new_states[feed_hit[f]], src, true);
    }
}

static void on_init_pipeline(device *dev, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipe)
{
    PipeInfo pi;
    for (uint32_t i = 0; i < count; i++)
    {
        const pipeline_subobject &so = subobjects[i];
        if (so.count == 0 || so.data == nullptr)
            continue;
        if (so.type == pipeline_subobject_type::blend_state)
        {
            pi.has_blend = true;
            pi.blend = static_cast<const blend_desc *>(so.data)->blend_enable[0];
            continue;
        }
        if (so.type == pipeline_subobject_type::depth_stencil_state)
        {
            const depth_stencil_desc *dd = static_cast<const depth_stencil_desc *>(so.data);
            pi.has_ds = true;
            pi.zwrite = dd->depth_enable && dd->depth_write_mask;
            continue;
        }
        if (so.type != pipeline_subobject_type::pixel_shader && so.type != pipeline_subobject_type::vertex_shader)
            continue;
        const shader_desc *sd = static_cast<const shader_desc *>(so.data);
        if (sd->code == nullptr || sd->code_size == 0)
            continue;
        uint32_t h = crc32(sd->code, sd->code_size);
        if (so.type == pipeline_subobject_type::pixel_shader) pi.ps = h;
        else pi.vs = h;
    }
    if (pi.ps == 0 && pi.vs == 0 && !pi.has_blend && !pi.has_ds)
        return;
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    std::unique_lock<std::shared_mutex> lock(ds->pipe_mutex);
    ds->pipes[pipe.handle] = pi;
}

static void on_destroy_pipeline(device *dev, pipeline pipe)
{
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    std::unique_lock<std::shared_mutex> lock(ds->pipe_mutex);
    ds->pipes.erase(pipe.handle);
}

static void on_destroy_resource(device *dev, resource res)
{
    pool_forget(res);
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ds->view_mutex);
    auto it = ds->depth_srv.find(res.handle);
    if (it != ds->depth_srv.end())
    {
        if (it->second.view.handle != 0)
            dev->destroy_resource_view(it->second.view);
        ds->depth_srv.erase(it);
    }
    for (auto i = ds->rtv_cache.begin(); i != ds->rtv_cache.end();)
    {
        if (i->second.owner == res.handle)
        {
            if (i->second.linear.handle != 0 && i->second.linear.handle != i->first)
                dev->destroy_resource_view(i->second.linear);
            if (i->second.srgb.handle != 0 && i->second.srgb.handle != i->first && i->second.srgb.handle != i->second.linear.handle)
                dev->destroy_resource_view(i->second.srgb);
            i = ds->rtv_cache.erase(i);
        }
        else
            ++i;
    }
}

static void on_bind_pipeline(command_list *cl, pipeline_stage stages, pipeline pipe)
{
    if (t_injecting)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    DevState *ds = cl->get_device()->get_private_data<DevState>();
    if (cs == nullptr || ds == nullptr)
        return;
    if (stages == pipeline_stage::all && tracking_wanted(cl))
        cs->trk.pso = pipe;                       // D3D12 SetPipelineState
    PipeInfo pi;
    {
        std::shared_lock<std::shared_mutex> lock(ds->pipe_mutex);
        auto it = ds->pipes.find(pipe.handle);
        if (it != ds->pipes.end())
            pi = it->second;
    }
    if (static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::pixel_shader))
    {
        if (cs->pass_open && cs->frame == g.frame && pi.ps != cs->ps)
            cs->ps_changes++;                     // the binding interval now mixes several operations
        cs->ps = pi.ps;
    }
    if (static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::vertex_shader))
        cs->vs = pi.vs;
    if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::output_merger)) && (pi.has_blend || stages == pipeline_stage::all))
        cs->blend = pi.blend;                     // D3D10/11 blend state object, D3D12 / Vulkan pipeline
    if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::depth_stencil)) && (pi.has_ds || stages == pipeline_stage::all))
        cs->zwrite = pi.zwrite;
}

static void set_bindings(command_list *cl, CmdState &cs, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    if (cs.pass_open && cs.frame == g.frame)
        end_pass(cl, cs);          // the previous binding interval ends here: capture feeds / preview from its targets
    cs.pass_open = false;
    cs.rt_count = std::min<uint32_t>(count, 8);
    for (uint32_t i = 0; i < 8; i++)
        cs.rtv[i] = (i < cs.rt_count && rtvs != nullptr) ? rtvs[i] : resource_view{ 0 };
    cs.dsv_view = dsv;
}

static void on_bind_render_targets(command_list *cl, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    if (t_injecting)
        return;                                    // the add-on re-applying the game's own state after an injection
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    set_bindings(cl, *cs, count, rtvs, dsv);
}

// D3D12 BeginRenderPass / Vulkan render passes: a binding interval too, but nothing may be copied or rendered inside it.
static bool on_begin_render_pass(command_list *cl, uint32_t count, const render_pass_render_target_desc *rts, const render_pass_depth_stencil_desc *ds, render_pass_flags)
{
    if (t_injecting)
        return false;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return false;
    resource_view v[8] = {};
    const uint32_t n = std::min<uint32_t>(count, 8);
    for (uint32_t i = 0; i < n && rts != nullptr; i++)
        v[i] = rts[i].view;
    set_bindings(cl, *cs, n, v, ds != nullptr ? ds->view : resource_view{ 0 });
    cs->in_render_pass = true;
    return false;
}

static bool on_end_render_pass(command_list *cl)
{
    if (t_injecting)
        return false;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return false;
    if (cs->pass_open && cs->frame == g.frame)
        end_pass(cl, *cs);                         // still inside the render pass (the event comes first): records, copies nothing
    cs->pass_open = false;
    cs->rt_count = 0;
    cs->in_render_pass = false;
    return false;
}

static bool on_draw(command_list *cl, uint32_t, uint32_t, uint32_t, uint32_t) { on_draw_common(cl); return false; }
static bool on_draw_indexed(command_list *cl, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { on_draw_common(cl); return false; }

// Camera probe: constant buffer writes
static bool on_update_buffer_region(device *, const void *data, resource dest, uint64_t offset, uint64_t size)
{
    scan_buffer(dest.handle, offset, data, size);
    return false;
}

static void on_map_buffer_region(device *dev, resource res, uint64_t offset, uint64_t size, map_access access, void **data)
{
    if (access == map_access::read_only || data == nullptr || *data == nullptr)
        return;
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ds->map_mutex);
    MapInfo mi;
    mi.ptr = *data;
    mi.size = size;
    mi.offset = offset;
    ds->mapped[res.handle] = mi;
}

static void on_unmap_buffer_region(device *dev, resource res)
{
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    MapInfo mi;
    {
        std::lock_guard<std::mutex> lock(ds->map_mutex);
        auto it = ds->mapped.find(res.handle);
        if (it == ds->mapped.end())
            return;
        mi = it->second;
        ds->mapped.erase(it);
    }
    scan_buffer(res.handle, mi.offset, mi.ptr, mi.size);
}

static void update_back_buffer_info(effect_runtime *rt)
{
    resource bb = rt->get_current_back_buffer();
    if (bb.handle == 0)
        return;
    resource_desc bd = rt->get_device()->get_resource_desc(bb);
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.bb_res = bb;
        g.bb_w = bd.texture.width;
        g.bb_h = bd.texture.height;
        g.bb_fmt = bd.texture.format;
    }
    // Back buffers are excluded from the profiler and from layer identities (their index changes every frame).
    const uint32_t n = rt->get_back_buffer_count();
    std::lock_guard<std::mutex> lock(prof.m);
    for (uint32_t i = 0; i < n; i++)
    {
        auto it = prof.pool.find(rt->get_back_buffer(i).handle);
        if (it != prof.pool.end())
            it->second.is_back_buffer = true;
    }
}

// The frame boundary is reshade_present (sent once per present, after the effects). Binding intervals still open on the immediate
// command list (D3D9/10/11: the game's own context) are finalized here, so the last interval of the frame is recorded and its feeds
// and preview captured. Deferred / D3D12 lists that never bind again this frame are dropped by their frame stamp instead.
static void end_frame(effect_runtime *rt)
{
    if (rt != nullptr && !explicit_states(rt->get_device()))
        if (command_queue *q = rt->get_command_queue())
            if (command_list *imm = q->get_immediate_command_list())
                if (CmdState *cs = imm->get_private_data<CmdState>())
                    if (cs->pass_open && cs->frame == g.frame)
                        end_pass(imm, *cs);
    pool_end_frame();
    uint64_t detected_rr;
    {
        std::lock_guard<std::mutex> lock(prof.m);
        detected_rr = prof.detected_rr;
    }
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        // Upscaler render resolution: detected (Auto), from the preset's fraction, or none.
        if (g.upscale_mode == UP_AUTO)
        {
            g.rr_w = static_cast<uint32_t>(detected_rr >> 32);
            g.rr_h = static_cast<uint32_t>(detected_rr & 0xFFFFFFFFu);
        }
        else if (g.upscale_mode == UP_OFF)
            g.rr_w = g.rr_h = 0;
        else
        {
            const float f = upscale_fraction(g.upscale_mode, g.upscale_pct);
            g.rr_w = static_cast<uint32_t>(std::lround(g.bb_w * f));
            g.rr_h = static_cast<uint32_t>(std::lround(g.bb_h * f));
        }
        // Catalog: merge this frame's layers. New layers are added while recording (and afterwards at the end of the list, unless
        // that is switched off); a layer that is assigned a role is always kept.
        const float total = static_cast<float>(std::max<size_t>(g.cur.size(), 1));
        for (const Pass &p : g.cur)
        {
            auto it = g.catalog.find(p.key);
            if (it == g.catalog.end())
            {
                bool assigned = g.role_ref[ROLE_INJECT].key == p.key || g.role_ref[ROLE_DEPTH].key == p.key;
                for (int i = 0; i < FEED_COUNT; i++)
                    assigned |= g.feed[i].source == SRC_LAYER && g.feed[i].ref.key == p.key;
                if (!(g.rec_left > 0 || g.cat_add_new || assigned) || g.catalog.size() >= 8192)
                    continue;
                it = g.catalog.emplace(p.key, LayerInfo()).first;
                it->second.first_seen = g.frame;
                it->second.pos = p.order / total;
                if (g.rec_left == 0)
                    g.cat_order.push_back(p.key);
            }
            LayerInfo &li = it->second;
            li.p = p;
            li.last_seen = g.frame;
            li.pos = li.seen == 0 ? p.order / total : 0.92f * li.pos + 0.08f * (p.order / total);
            li.seen++;
        }
        if (g.rec_left > 0)
        {
            g.rec_left--;
            if (g.rec_left == 0 || (g.rec_left % 15) == 0)
                sort_catalog_locked();
        }
        g.last = std::move(g.cur);
        g.cur.clear();
        g.last_list_count = static_cast<uint32_t>(g.cur_lists.size());
        g.cur_lists.clear();
        g.frame++;
        g.in_eof = false;
    }
    choose_camera();
}

// begin/finish_effects are also sent around the add-on's own render_technique calls (ignored via t_injecting) and around other
// add-ons' mid-frame renders (ignored: only the runtime's render into the current back buffer is the end-of-frame render).
static bool is_end_of_frame_render(effect_runtime *rt, resource_view rtv)
{
    return rtv.handle != 0 && rt->get_device()->get_resource_from_view(rtv) == rt->get_current_back_buffer();
}

static void on_begin_effects(effect_runtime *rt, command_list *, resource_view rtv, resource_view)
{
    if (t_injecting || !is_end_of_frame_render(rt, rtv))
        return;
    if (!g_eof_holding.load())
    {
        g_runtime_render_mutex.lock();              // waits for an injection running on a worker thread (D3D12) to finish
        g_eof_holding = true;
    }
    update_back_buffer_info(rt);
    bool injected;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.in_eof = true;
        injected = g.injected_frame == g.frame;
    }
    if (!injected)
        bind_feeds_end_of_frame(rt);   // level C without (or without a successful) in-frame injection: feeds at the end of the frame
    suppress_end_of_frame(rt);
}

static void release_eof_lock()
{
    if (g_eof_holding.exchange(false))
        g_runtime_render_mutex.unlock();
}

static void on_finish_effects(effect_runtime *rt, command_list *, resource_view rtv, resource_view)
{
    if (t_injecting || !is_end_of_frame_render(rt, rtv))
        return;
    restore_end_of_frame(rt);
    release_eof_lock();
}

static void on_reloaded_effects(effect_runtime *rt)
{
    refresh_techniques(rt);
    refresh_uniforms(rt);
    std::lock_guard<std::mutex> lock(g.mtx);
    for (int i = 0; i < FEED_COUNT; i++)
        g.eof_bound_valid[i] = false;               // a reload drops ReShade's semantic bindings
}

static void on_init_runtime(effect_runtime *rt)
{
    g_conflict = detect_conflicting_addon();
    if (!g_conflict.empty())
        log_msg(reshade::log::level::warning, "\"" + g_conflict + "\" is loaded and also renders effects inside the frame. "
                "The injector stands down (no in-frame rendering, no technique switching) so the two do not fight over the same technique. "
                "Material feeds at the end of the frame and the profiler still work.");
    g.runtime = rt;
    update_back_buffer_info(rt);
    refresh_techniques(rt);
    refresh_uniforms(rt);
    start_recording(180);                           // first layer list: 3 seconds at 60 fps
}

static void on_destroy_runtime(effect_runtime *rt)
{
    if (g.runtime == rt)
    {
        restore_end_of_frame(rt);
        g.runtime = nullptr;
    }
}

// Selection and keyboard
static void select_row(const std::vector<Row> &rows, int index)
{
    if (rows.empty())
        return;
    index = std::clamp(index, 0, static_cast<int>(rows.size()) - 1);
    std::lock_guard<std::mutex> lock(g.mtx);
    g.preview_key = rows[index].p.key;
    g.preview_slot = rows[index].slot;
    g.sel_pass = index;
    g.sel_slot = rows[index].slot;
}

static int selected_index(const std::vector<Row> &rows)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].p.key == g.preview_key && rows[i].slot == g.preview_slot)
            return static_cast<int>(i);
    return -1;
}

// True when the action's key combination was pressed this frame, or (repeating actions) is being held past the repeat delay.
static bool key_fired(effect_runtime *rt, int a, uint64_t now)
{
    KeyBind k;
    int delay, rate;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        k = g.keys[a];
        delay = g.repeat_delay_ms;
        rate = g.repeat_rate_ms;
        if (g.key_capture >= 0 || g.typing_frame + 1 >= g.frame)
            return false;                           // the Keys tab waits for a new key, or a text field is being typed into
    }
    if (k.key == 0)
        return false;
    const bool mods = rt->is_key_down(VK_CONTROL) == k.ctrl && rt->is_key_down(VK_SHIFT) == k.shift && rt->is_key_down(VK_MENU) == k.alt;
    const bool down = mods && rt->is_key_down(k.key);
    std::lock_guard<std::mutex> lock(g.mtx);
    if (!down)
    {
        g.key_down_since[a] = 0;
        return false;
    }
    if (g.key_down_since[a] == 0)                   // went down: fire once, then repeat after the delay while held
    {
        g.key_down_since[a] = now;
        g.key_next_repeat[a] = now + static_cast<uint64_t>(delay);
        return true;
    }
    if (KA_REPEAT[a] && now >= g.key_next_repeat[a])
    {
        g.key_next_repeat[a] = now + static_cast<uint64_t>(rate);
        return true;
    }
    return false;
}

static void handle_keys(effect_runtime *rt)
{
    const uint64_t now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (g.key_capture >= 0 && g.frame > g.key_capture_frame + 2)
            g.key_capture = -1;                     // the overlay was closed while the Keys tab waited for a key
    }
    bool fired[KA_COUNT];
    bool any = false;
    for (int a = 0; a < KA_COUNT; a++)
        any |= (fired[a] = key_fired(rt, a, now));
    if (!any)
        return;
    if (fired[KA_TOGGLE]) { std::lock_guard<std::mutex> l(g.mtx); g.master = !g.master; g.dirty = true; }
    if (fired[KA_LEVEL_DOWN]) { std::lock_guard<std::mutex> l(g.mtx); g.level = std::max(0, g.level - 1); g.dirty = true; }
    if (fired[KA_LEVEL_UP]) { std::lock_guard<std::mutex> l(g.mtx); g.level = std::min(2, g.level + 1); g.dirty = true; }
    if (fired[KA_AUTO_SETUP])
        auto_suggest();
    if (fired[KA_AUTO_FEEDS])
    {
        pool_request_snapshot(true, true);
        std::lock_guard<std::mutex> l(g.mtx);
        if (g.level < LEVEL_C) g.level = LEVEL_C;
        g.dirty = true;
    }
    if (fired[KA_RECORD])
        start_recording(180);
    if (fired[KA_LAYER_PREV] || fired[KA_LAYER_NEXT] || fired[KA_INJECT_HERE] || fired[KA_ROLE_PREV] || fired[KA_ROLE_NEXT])
    {
        std::vector<Row> rows = build_rows();
        int idx = selected_index(rows);
        if (fired[KA_LAYER_PREV]) select_row(rows, idx < 0 ? 0 : idx - 1);
        if (fired[KA_LAYER_NEXT]) select_row(rows, idx < 0 ? 0 : idx + 1);
        if (fired[KA_LAYER_PREV] || fired[KA_LAYER_NEXT]) { std::lock_guard<std::mutex> l(g.mtx); g.scroll_to_sel = true; }
        idx = selected_index(rows);
        if (idx >= 0)
        {
            if (fired[KA_INJECT_HERE])
                apply_role(rows[idx].p, rows[idx].slot, ROLE_INJECT);
            if (fired[KA_ROLE_PREV] || fired[KA_ROLE_NEXT])
            {
                int r = role_of(rows[idx].p, rows[idx].slot);
                r = (r + (fired[KA_ROLE_NEXT] ? 1 : ROLE_COUNT - 1)) % ROLE_COUNT;
                apply_role(rows[idx].p, rows[idx].slot, r);
            }
        }
    }
}

// Auto re-detect: a feed whose texture was destroyed (loading screens recreate the G-buffer) and not re-adopted within half a second,
// or whose texture stopped being written for 3 seconds while the game renders, triggers a new detection that replaces it.
static void auto_redetect_check(effect_runtime *rt)
{
    bool want = false;
    bool check_stale[FEED_COUNT] = {};
    uint64_t frame;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (!g.auto_redetect || !g.master || g.level < LEVEL_C || g.frame < g.redetect_frame + 600)
            return;
        frame = g.frame;
        for (int i = 0; i < FEED_COUNT; i++)
        {
            const FeedConfig &f = g.feed[i];
            if (!f.enabled || f.source != SRC_TEXTURE || !f.tex.valid() || i == FEED_MATERIAL)
                continue;
            if (g.feed_res[i].handle == 0 && g.feed_lost_frame[i] != 0 && g.frame > g.feed_lost_frame[i] + 30)
                want = true;
            else if (g.feed_res[i].handle != 0)
                check_stale[i] = true;
        }
    }
    // (feed_mutex is never taken while g.mtx is held: capture_feed takes them in the other order)
    if (DevState *ds = rt->get_device()->get_private_data<DevState>())
    {
        std::lock_guard<std::mutex> l2(ds->feed_mutex);
        for (int i = 0; i < FEED_COUNT; i++)
            if (check_stale[i] && ds->feed[i].frame != ~0ull && frame > ds->feed[i].frame + 180)
                want = true;
    }
    if (want)
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.redetect_frame = g.frame;
    }
    if (want)
    {
        log_msg(reshade::log::level::info, "Auto re-detect: an assigned feed texture was lost or went stale (loading screen?); detecting again.");
        pool_request_snapshot(true, true);
    }
}

static void on_reshade_present(effect_runtime *rt)
{
    if (rt != g.runtime)
        return;
    restore_end_of_frame(rt);   // safety net: never leave a technique switched off past the frame
    release_eof_lock();         // and never keep the render lock past the frame
    end_frame(rt);
    pool_present(rt);
    flush_graveyard(rt->get_device(), false);

    handle_keys(rt);
    auto_redetect_check(rt);

    apply_shader_link(rt);

    std::lock_guard<std::mutex> lock(g.mtx);
    if (g.dirty)
    {
        g.dirty = false;
        save_settings_locked();
    }
}
