// CRCGI Injector - pass tracking, the layer catalog, layer roles, and the ReShade add-on event handlers.
//
// How the game is watched (the same set of events ReShade Effect Shader Toggler hooks): every draw, indexed draw, indirect draw and
// compute dispatch counts as work of the current render-target binding interval, pipeline binds give the shader identities, and render
// pass / clear / barrier events delimit intervals on the explicit APIs.
//
// What identifies game data from frame to frame is the STRUCTURE of the frame, never a memory address and never what happens to be drawn
// first: a layer's target layout (how many targets, their formats, their size relative to the screen, depth bound or not) does not
// change when the camera turns, when the game recreates or swaps its textures, or between players at different resolutions. Feeds are
// captured at the end of a write run (see FeedRun), so a G-buffer drawn in several intervals is captured complete.
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

// A layer is "always rendered" when it was drawn in at least 95% of the frames since it was first seen, for at least a second of
// frames, and is still drawn now. Conditional layers (menus, cut-scenes, a weather effect, something the camera sometimes sees) are
// hidden from the list unless a role is assigned to them or the option is switched off.
static bool layer_always_rendered(const LayerInfo &li)
{
    const uint64_t frame = g.frame;
    if (li.last_seen + 3 < frame || li.seen < 60)
        return false;
    const uint64_t span = frame - li.first_seen + 1;
    return static_cast<uint64_t>(li.seen) * 20 >= span * 19;
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
        if (g.only_always && !layer_has_role_locked(key) && !layer_always_rendered(li))
            continue;
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
        if (g.feed[i].source == SRC_LAYER && (g.feed[i].ref == me || (g.feed[i].layer_match(i, p.key, p.layout) && g.feed[i].ref.slot == slot)))
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
        if (g.feed[i].source == SRC_LAYER && (g.feed[i].ref == me || (g.feed[i].layer_match(i, p.key, p.layout) && g.feed[i].ref.slot == slot)))
        {
            g.feed[i].ref = LayerRef();
            g.feed[i].layout = 0;
            g.feed[i].enabled = false;
        }
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
        f.layout = p.layout;
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
        return p.depth.handle == 0 ? "screen-format colour (can be the finished frame)" : "screen-format colour";
    if (fmt_is_float(r.fmt) && (full || render_res) && slot == 0)
        return render_res ? "floating-point colour at render resolution (maybe HDR scene, before the upscaler)" : "floating-point colour (maybe HDR scene)";
    if (render_res)
        return "render resolution (upscaler input)";
    if ((t == format_to_typeless(format::r16g16_float) || t == format_to_typeless(format::r16g16_unorm)) && half)
        return "maybe motion vectors";
    return std::string();
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------------------------------------------------
// draws: the draws of the write run that ends here (0 = unknown, the Vulkan barrier path).
static void capture_feed(command_list *cl, DevState &ds, int feed, resource_usage state, const RtSlot &src, uint32_t draws, bool barrier_path)
{
    std::lock_guard<std::mutex> lock(ds.feed_mutex);
    FeedTex &ft = ds.feed[feed];
    // A later run in the same frame replaces the copy only if it continues it (the same texture, not cleared since: it holds everything
    // the earlier run drew and more) or draws more than everything captured so far (the main use of the layout, not a smaller pass
    // that happens to use the same kind of targets). Engines reuse transient memory later in the frame for unrelated passes; those
    // runs neither continue the copy nor outdraw it, so they never overwrite the G-buffer copy (the picture would flash).
    const int policy = barrier_path ? CAP_FIRST : feed_policy(feed);
    const bool same_frame = ft.frame == g.frame;
    const bool continues = policy == CAP_RUN && same_frame && ft.cap_res == src.res && !ft.tainted;
    if (same_frame && (policy == CAP_FIRST || (!continues && draws <= ft.cap_draws)))
        return;
    if (copy_into(cl, state, ft, src, feed))
    {
        ft.cap_draws = continues ? ft.cap_draws + draws : draws;
        ft.cap_res = src.res;
        ft.tainted = false;
        std::lock_guard<std::mutex> l2(g.mtx);
        if (barrier_path) g.n_captures_barrier++;
        else g.n_captures_interval++;
    }
}

// Takes the captures of one finished binding interval: profiler snapshots, feeds taken from the interval's layer or texture, and the
// preview. The command list must be outside a render pass.
static void run_captures(command_list *cl, CmdState &cs, uint32_t draws, uint32_t rt_count, const RtSlot *rt, const std::vector<CaptureTodo> &todo,
                         bool prev, int prev_slot, bool snapshots)
{
    if (snapshots)
        for (uint32_t s = 0; s < rt_count && s < 8; s++)
            if (rt[s].res.handle != 0)
                pool_capture(cl, rt[s].res, bound_rt_state(cs, rt[s].res), rt[s].view_fmt);
    if (todo.empty() && !prev)
        return;
    DevState *ds = cl->get_device()->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    for (const CaptureTodo &t : todo)
        if (t.slot >= 0 && t.slot < 8 && rt[t.slot].res.handle != 0)
            capture_feed(cl, *ds, t.feed, bound_rt_state(cs, rt[t.slot].res), rt[t.slot], draws, false);
    if (prev && prev_slot >= 0 && prev_slot < static_cast<int>(rt_count) && rt[prev_slot].res.handle != 0)
    {
        std::lock_guard<std::mutex> lock(ds->feed_mutex);
        copy_into(cl, bound_rt_state(cs, rt[prev_slot].res), ds->preview, rt[prev_slot], -1);
    }
}

// Pending captures (see PendingCapture) whose moment has come: the command list is outside a render pass again. Stale ones (another
// frame) are dropped.
static void flush_pending(command_list *cl, CmdState &cs)
{
    if (cs.pending.empty() || cs.in_render_pass)
        return;
    std::vector<PendingCapture> list;
    list.swap(cs.pending);
    for (const PendingCapture &pc : list)
        if (pc.frame == g.frame)
            run_captures(cl, cs, pc.draws, pc.rt_count, pc.rt, pc.todo, pc.prev, pc.prev_slot, pc.snapshots);
}

// ---------------------------------------------------------------------------------------------------------------------
// Feed write runs (see FeedRun)
// ---------------------------------------------------------------------------------------------------------------------
// The layout of a set of targets: count, per target its format, its size in 64ths of the screen and its sample count, and whether a
// depth buffer is bound. No memory addresses and no shaders, so it is the same in every frame, after the game recreates its targets,
// whichever texture of a pair is bound this frame, and at another screen resolution.
static uint64_t layout_of(const RtSlot *rt, uint32_t count, bool depth)
{
    struct L { uint32_t count, depth; uint32_t fmt[8], rw[8], rh[8], samples[8]; } l;
    std::memset(&l, 0, sizeof(l));
    l.count = std::min<uint32_t>(count, 8);
    l.depth = depth ? 1u : 0u;
    const uint32_t bw = g.bb_w, bh = g.bb_h;
    for (uint32_t i = 0; i < l.count; i++)
    {
        if (rt[i].res.handle == 0)
            continue;
        l.fmt[i] = static_cast<uint32_t>(rt[i].fmt);
        l.rw[i] = bw != 0 ? static_cast<uint32_t>((static_cast<uint64_t>(rt[i].w) * 64 + bw / 2) / bw) : rt[i].w;
        l.rh[i] = bh != 0 ? static_cast<uint32_t>((static_cast<uint64_t>(rt[i].h) * 64 + bh / 2) / bh) : rt[i].h;
        l.samples[i] = rt[i].samples;
    }
    const uint64_t h = fnv64(&l, sizeof(l));
    return h != 0 ? h : 1;
}

// Ends feed i's run on this command list: the texture now holds everything the run drew, copy it (inside a Direct3D 12 render pass:
// right after the pass, see PendingCapture).
static void run_end(command_list *cl, CmdState &cs, int i)
{
    FeedRun r = cs.run[i];
    cs.run[i].open = false;
    if (!r.open || r.frame != g.frame || r.src.res.handle == 0)
        return;
    if (cs.in_render_pass)
    {
        if (cl->get_device()->get_api() != device_api::d3d12)
            return;
        PendingCapture pc;
        pc.frame = g.frame;
        pc.rt_count = 1;
        pc.rt[0] = r.src;
        pc.todo.push_back({ i, 0 });
        pc.draws = r.draws;
        cs.pending.push_back(std::move(pc));
        if (cs.pending.size() > 64)
            cs.pending.erase(cs.pending.begin());
        return;
    }
    DevState *ds = cl->get_device()->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    capture_feed(cl, *ds, i, bound_rt_state(cs, r.src.res), r.src, r.draws, false);
}

static void end_all_runs(command_list *cl, CmdState &cs)
{
    for (int i = 0; i < FEED_COUNT; i++)
        if (cs.run[i].open)
            run_end(cl, cs, i);
}

// Ends the runs that write `res` (it is about to be cleared, copied or transitioned).
static void end_runs_of(command_list *cl, CmdState &cs, resource res)
{
    for (int i = 0; i < FEED_COUNT; i++)
        if (cs.run[i].open && cs.run[i].src.res == res)
            run_end(cl, cs, i);
}

// A finished binding interval that drew into feed i's texture: it extends the open run (same texture, same layout) or starts a new one.
static void run_extend(command_list *cl, CmdState &cs, int i, int slot, uint32_t draws)
{
    FeedRun &r = cs.run[i];
    const RtSlot &src = cs.rt[slot];
    if (r.open && r.frame == g.frame && r.src.res == src.res && r.layout == cs.layout && r.slot == slot)
    {
        r.draws += draws;
        return;
    }
    if (r.open)
        run_end(cl, cs, i);
    r.open = true;
    r.frame = g.frame;
    r.layout = cs.layout;
    r.slot = slot;
    r.src = src;
    r.draws = std::max<uint32_t>(draws, 1);
}

// A clear of a texture starts a new use of its memory: a capture of it taken earlier in the frame is not continued by a later run.
static void taint_feeds(device *dev, resource res)
{
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ds->feed_mutex);
    for (int i = 0; i < FEED_COUNT; i++)
        if (ds->feed[i].frame == g.frame && ds->feed[i].cap_res == res)
            ds->feed[i].tainted = true;
}

static void end_pass(command_list *cl, CmdState &cs)
{
    std::vector<CaptureTodo> todo;
    bool prev = false;
    int prev_slot = 0;
    uint32_t order = 0;
    cs.pass_open = false;
    device *dev = cl->get_device();
    const device_api api = dev->get_api();
    const bool copy_now = !cs.in_render_pass && api != device_api::vulkan;
    const bool copy_later = cs.in_render_pass && api == device_api::d3d12;   // after EndRenderPass, at the next event of this list
    std::vector<CaptureTodo> candidates;
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
        if ((copy_now || copy_later) && g.master && g.level >= LEVEL_C && api_supports_feeds(g.api))
            for (int i = 0; i < FEED_COUNT; i++)
            {
                const FeedConfig &f = g.feed[i];
                if (!f.enabled)
                    continue;
                if (f.source == SRC_LAYER && f.layer_match(i, cs.pass_key, cs.layout) && f.ref.slot >= 0 && f.ref.slot < static_cast<int>(cs.rt_count) &&
                    cs.rt[f.ref.slot].res.handle != 0)
                    todo.push_back({ i, f.ref.slot });
                else if (f.source == SRC_TEXTURE && g.feed_res[i].handle != 0)
                    for (uint32_t s = 0; s < cs.rt_count && s < 8; s++)
                        if (cs.rt[s].res == g.feed_res[i])
                            candidates.push_back({ i, static_cast<int>(s) });
        }
        if ((copy_now || copy_later) && g.preview_key == cs.pass_key && g.frame <= g.preview_frame + 2)
        {
            prev = true;
            prev_slot = g.preview_slot;
        }
    }
    // A texture feed is captured only at runs with the texture's main layout (that of the binding that drew most to it): later bindings
    // of the same memory in another role belong to unrelated passes. (Checked outside g.mtx: the profiler has its own lock.)
    for (const CaptureTodo &c : candidates)
        if (pool_is_main_interval(cs.rt[c.slot].res, cs.layout))
            todo.push_back(c);
    pool_note_interval(cs, order);
    // G-buffer feeds: the interval joins its write run, the copy is made when the run ends (run_end). The others are copied now.
    std::vector<CaptureTodo> now;
    for (const CaptureTodo &c : todo)
    {
        if (feed_policy(c.feed) == CAP_RUN)
            run_extend(cl, cs, c.feed, c.slot, cs.draws);
        else
            now.push_back(c);
    }
    if (copy_now)
        run_captures(cl, cs, cs.draws, cs.rt_count, cs.rt, now, prev, prev_slot, true);
    else if (copy_later)
    {
        PendingCapture pc;
        pc.frame = g.frame;
        pc.key = cs.pass_key;
        pc.rt_count = std::min<uint32_t>(cs.rt_count, 8);
        for (uint32_t i = 0; i < 8; i++)
            pc.rt[i] = cs.rt[i];
        pc.todo = now;
        pc.draws = cs.draws;
        pc.prev = prev;
        pc.prev_slot = prev_slot;
        pc.snapshots = true;
        cs.pending.push_back(std::move(pc));
        if (cs.pending.size() > 64)
            cs.pending.erase(cs.pending.begin());
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

    // Layer key: targets (format, size and the texture's run-to-run identity) + the occurrence of that signature in THIS recording.
    // Independent of how command lists interleave on other threads, and of the back buffer index. The shaders are NOT part of it: the
    // first draw of a G-buffer or lighting pass depends on what the camera sees, and the layer would get a new key whenever it changes.
    struct KeyData { uint32_t count, depth; uint32_t fmt[8], w[8], h[8]; uint64_t id[8]; } kd;
    std::memset(&kd, 0, sizeof(kd));
    kd.count = p.rt_count; kd.depth = cs.dsv.handle != 0;
    for (int i = 0; i < 8; i++)
    {
        kd.fmt[i] = static_cast<uint32_t>(cs.rt[i].fmt);
        kd.w[i] = cs.rt[i].w;
        kd.h[i] = cs.rt[i].h;
        kd.id[i] = cs.rt[i].res.handle != 0 ? pool_identity(cs.rt[i].res) : 0;
    }
    const uint64_t base = fnv64(&kd, sizeof(kd));
    cs.layout = layout_of(cs.rt, p.rt_count, cs.dsv.handle != 0);
    p.layout = cs.layout;
    // A run ends where its texture is drawn to in another layout or another texture takes its slot (the draw that opens this interval
    // has not run yet: the copy holds exactly what the run drew).
    for (int i = 0; i < FEED_COUNT; i++)
    {
        const FeedRun &r = cs.run[i];
        if (r.open && (r.layout != cs.layout || cs.rt[r.slot].res != r.src.res))
            run_end(cl, cs, i);
    }
    if (cs.occ_frame != g.frame)
    {
        cs.occ.clear();                          // the occurrence counter restarts every frame on every API (a list that is reused across
        cs.occ_frame = g.frame;                  // frames without a reset, or recorded in a different order, would drift otherwise)
    }
    const uint32_t occ = cs.occ[base]++;
    uint64_t key = fnv64(&occ, sizeof(occ), base);
    if (key == 0) key = 1;

    bool do_inject = false;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        p.key = key;
        p.draws = 1;                             // the interval opens at its first real draw; end_pass stores the final count (a command list that
                                                 // ends without another binding keeps this one, so its last layer is still listed)
        p.order = static_cast<uint32_t>(g.cur.size());
        p.to_back_buffer = p.rt[0].res.handle != 0 && p.rt[0].res.handle == g.bb_res.handle;
        cs.pass_index = static_cast<int>(g.cur.size());
        cs.pass_key = key;
        if (g.cur.size() < 65536)
            g.cur.push_back(p);
        g.cur_lists.insert(p.cl);
        do_inject = g.master && g.level >= LEVEL_B && g.force_inject && api_supports_injection(g.api) && g_conflict.empty() && !g.in_eof &&
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

static void sync_frame(CmdState &cs)
{
    const uint64_t frame = g.frame;
    if (cs.frame != frame)
    {
        cs.frame = frame;                        // anything still open belongs to an old frame: drop it, never use it as current
        cs.pass_open = false;
        cs.pending.clear();
        cs.dispatches = 0;
        cs.occ.clear();                          // layer keys count occurrences within one frame's recording
        cs.occ_frame = frame;
        for (FeedRun &r : cs.run)
            r.open = false;
    }
}

// A draw call of the current binding interval. Draws that draw nothing (no vertices, no instances) are not work and do not open an
// interval: engines issue them as placeholders, and they would fill the layer list with empty entries.
static void on_draw_common(command_list *cl)
{
    if (t_injecting || g.runtime == nullptr)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    sync_frame(*cs);
    flush_pending(cl, *cs);
    if (!cs->pass_open)
        begin_pass(cl, *cs);
    cs->draws++;
    if (cs->blend) cs->blend_draws++;
    if (cs->zwrite && cs->dsv.handle != 0) cs->zwrite_draws++;
}

// A compute dispatch. It belongs to no render-target interval; what matters is which textures it writes (Direct3D 10 / 11: the unordered
// access views the game bound, which the API shows). A feed taken from such a texture is copied at the next event of the list (the
// dispatch has run by then), at the first dispatch that writes it in the frame.
static void on_dispatch_common(command_list *cl)
{
    if (t_injecting || g.runtime == nullptr)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    sync_frame(*cs);
    flush_pending(cl, *cs);
    end_all_runs(cl, *cs);                       // a compute pass reads (or writes) what the runs drew: they are complete
    cs->dispatches++;
    device *dev = cl->get_device();
    if (explicit_states(dev))
        return;                                  // Direct3D 12 / Vulkan: UAVs live in descriptor heaps, their textures are not visible here
    bool want[FEED_COUNT] = {};
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (g.master && g.level >= LEVEL_C && api_supports_feeds(g.api))
            for (int i = 0; i < FEED_COUNT; i++)
                want[i] = g.feed[i].enabled && g.feed[i].source == SRC_TEXTURE && g.feed_res[i].handle != 0;
    }
    for (uint32_t u = 0; u < 16; u++)
    {
        const resource res = cs->uav[u];
        if (res.handle == 0)
            continue;
        const uint64_t id = fnv64(&res.handle, sizeof(res.handle));
        const uint32_t occ = cs->occ[id]++;
        const uint64_t key = fnv64(&occ, sizeof(occ), id) | 1;
        pool_note_compute(res, key);
        for (int i = 0; i < FEED_COUNT; i++)
        {
            if (!want[i])
                continue;
            resource live;
            {
                std::lock_guard<std::mutex> lock(g.mtx);
                live = g.feed_res[i];
            }
            if (live != res || !pool_is_main_interval(res, key))
                continue;
            resource_desc d = dev->get_resource_desc(res);
            if (d.type != resource_type::texture_2d)
                continue;
            PendingCapture pc;
            pc.frame = g.frame;
            pc.key = key;
            pc.rt_count = 1;
            pc.rt[0].res = res;
            pc.rt[0].w = d.texture.width;
            pc.rt[0].h = d.texture.height;
            pc.rt[0].samples = d.texture.samples;
            pc.rt[0].fmt = d.texture.format;
            pc.rt[0].view_fmt = d.texture.format;
            pc.todo.push_back({ i, 0 });
            pc.draws = 1;
            cs->pending.push_back(std::move(pc));
            if (cs->pending.size() > 64)
                cs->pending.erase(cs->pending.begin());
        }
    }
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
        cs->occ_frame = g.frame;
        cs->trk.clear();
        cs->pass_open = false;
        cs->in_render_pass = false;
        cs->pending.clear();
        for (resource &r : cs->uav)
            r = resource{ 0 };
        for (FeedRun &r : cs->run)
            r.open = false;
    }
}

// Before a command list is closed (Direct3D 12 Close, Direct3D 11 FinishCommandList, Vulkan vkEndCommandBuffer): the last binding
// interval of the list ends here, and with it any open feed run. Engines often close a list right after the G-buffer pass; without this
// the last interval of such a list would never be captured.
static void on_close_cmd_list(command_list *cl)
{
    if (t_injecting)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr || cs->frame != g.frame)
        return;
    if (cl->get_device()->get_api() == device_api::vulkan)
        return;                                   // Vulkan captures at barriers only (end_pass records, copies nothing there)
    if (cs->pass_open)
        end_pass(cl, *cs);
    cs->pass_open = false;
    if (!cs->in_render_pass)
    {
        end_all_runs(cl, *cs);
        flush_pending(cl, *cs);
    }
}

// Barriers: record the state the game put each resource in. Vulkan only: also capture feeds / profiler snapshots right after the game
// itself moves a texture out of the render-target state (its state is then known exactly: the barrier's new state), because nothing can
// be copied inside a render pass there. Direct3D 12 never copies here: the event cannot tell a split barrier's BEGIN_ONLY half from a
// full transition, and a copy (and the barriers around it) between the two halves is invalid. Direct3D 12 captures happen at the
// end of a binding interval, or at the next event after a render pass ended (flush_pending).
static void on_barrier(command_list *cl, uint32_t count, const resource *resources, const resource_usage *old_states, const resource_usage *new_states)
{
    if (t_injecting)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr || resources == nullptr || new_states == nullptr)
        return;
    sync_frame(*cs);
    flush_pending(cl, *cs);                       // before the new states are recorded: the resources are still where they were
    if (!cs->in_render_pass)
        for (uint32_t i = 0; i < count; i++)
            if (resources[i].handle != 0)
                end_runs_of(cl, *cs, resources[i]);   // the game moves the texture on: its run is complete
    for (uint32_t i = 0; i < count; i++)
        if (resources[i].handle != 0)
            cs->states[resources[i].handle] = new_states[i];
    if (old_states == nullptr || cs->in_render_pass)
        return;
    device *dev = cl->get_device();
    if (dev->get_api() != device_api::vulkan)
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
    for (uint32_t i = 0; i < count; i++)
        if (resources[i].handle != 0 && has_usage(old_states[i], resource_usage::render_target) &&
            !has_usage(new_states[i], resource_usage::render_target) && new_states[i] != resource_usage::undefined)
            pool_capture(cl, resources[i], new_states[i], format::unknown, true);
    if (!any_feed)
        return;
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    for (int f = 0; f < FEED_COUNT; f++)
    {
        if (feed_hit[f] < 0)
            continue;
        const resource res = resources[feed_hit[f]];
        if (!pool_main_seen(res))
            continue;                                // the texture's main binding has not ended yet in this frame: this is not its picture
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
        capture_feed(cl, *ds, f, new_states[feed_hit[f]], src, 0, true);
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
    flush_pending(cl, cs);
    if (cs.pass_open && cs.frame == g.frame)
        end_pass(cl, cs);          // the previous binding interval ends here: capture feeds / preview from its targets
    cs.pass_open = false;
    cs.rt_count = std::min<uint32_t>(count, 8);
    for (uint32_t i = 0; i < 8; i++)
        cs.rtv[i] = (i < cs.rt_count && rtvs != nullptr) ? rtvs[i] : resource_view{ 0 };
    cs.dsv_view = dsv;
    // An open run continues only while its texture stays bound in its slot (rebinding the same G-buffer between draws is common); the
    // layout is checked at the next draw (begin_pass).
    bool any_open = false;
    for (const FeedRun &r : cs.run)
        any_open |= r.open;
    if (!any_open)
        return;
    device *dev = cl->get_device();
    for (int i = 0; i < FEED_COUNT; i++)
    {
        const FeedRun &r = cs.run[i];
        if (!r.open)
            continue;
        const resource now = (static_cast<uint32_t>(r.slot) < cs.rt_count && cs.rtv[r.slot].handle != 0) ? dev->get_resource_from_view(cs.rtv[r.slot]) : resource{ 0 };
        if (now != r.src.res)
            run_end(cl, cs, i);
    }
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

static bool on_draw(command_list *cl, uint32_t vertex_count, uint32_t instance_count, uint32_t, uint32_t)
{
    if (vertex_count != 0 && instance_count != 0)
        on_draw_common(cl);
    return false;
}

static bool on_draw_indexed(command_list *cl, uint32_t index_count, uint32_t instance_count, uint32_t, int32_t, uint32_t)
{
    if (index_count != 0 && instance_count != 0)
        on_draw_common(cl);
    return false;
}

// GPU-driven geometry: the draw / dispatch arguments live in a buffer the CPU never sees, so the call counts as one unit of work of
// its kind (a draw_count of 0 is a draw that may draw nothing, and is ignored).
static bool on_draw_or_dispatch_indirect(command_list *cl, indirect_command type, resource, uint64_t, uint32_t draw_count, uint32_t)
{
    switch (type)
    {
    case indirect_command::draw:
    case indirect_command::draw_indexed:
        if (draw_count != 0)
            on_draw_common(cl);
        break;
    case indirect_command::dispatch:
        on_dispatch_common(cl);
        break;
    default:
        break;                                    // mesh shaders and ray tracing do not render into the binding interval's targets here
    }
    return false;
}

static bool on_dispatch(command_list *cl, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
    if (group_count_x != 0 && group_count_y != 0 && group_count_z != 0)
        on_dispatch_common(cl);
    return false;
}

// A clear of a bound render target starts a new use of that memory: the interval so far ends (it captures what it drew), the next draw
// opens a new interval with the same targets. Without this, a game that clears and redraws the same target in one binding would be
// recorded as a single layer.
static bool on_clear_render_target_view(command_list *cl, resource_view rtv, const float[4], uint32_t, const rect *)
{
    if (t_injecting || g.runtime == nullptr)
        return false;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return false;
    sync_frame(*cs);
    flush_pending(cl, *cs);
    if (cs->pass_open && !cs->in_render_pass)
        for (uint32_t i = 0; i < cs->rt_count && i < 8; i++)
            if (cs->rtv[i] == rtv)
            {
                end_pass(cl, *cs);                        // pass_open becomes false: the next draw opens a new interval
                break;
            }
    if (rtv.handle != 0)
    {
        device *dev = cl->get_device();
        const resource res = dev->get_resource_from_view(rtv);
        if (res.handle != 0)
        {
            if (!cs->in_render_pass)
                end_runs_of(cl, *cs, res);              // its run is complete: copied before the clear runs
            taint_feeds(dev, res);                      // and a later run on it is a new use, not a continuation
        }
    }
    return false;
}

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
                    if (cs->frame == g.frame)
                    {
                        if (cs->pass_open)
                            end_pass(imm, *cs);
                        cs->pass_open = false;
                        end_all_runs(imm, *cs);
                    }
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
            if (p.draws == 0)
                continue;                        // an interval without a real draw is not a layer
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
        if ((g.frame % 600) == 599)
        {
            // Layers not drawn for ten seconds and without a role are dropped (a menu that was open once, a cut-scene): the catalog
            // stays a list of what the game does now.
            bool removed = false;
            for (auto it = g.catalog.begin(); it != g.catalog.end();)
            {
                const uint64_t key = it->first;
                if (it->second.last_seen + 600 < g.frame && !layer_has_role_locked(key))
                {
                    it = g.catalog.erase(it);
                    removed = true;
                }
                else
                    ++it;
            }
            if (removed)
                g.cat_order.erase(std::remove_if(g.cat_order.begin(), g.cat_order.end(), [](uint64_t k) { return g.catalog.count(k) == 0; }),
                                  g.cat_order.end());
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
    if (fired[KA_RECORD])
        start_recording(180);
    if (fired[KA_LAYER_PREV] || fired[KA_LAYER_NEXT] || fired[KA_FRAME_END_HERE] || fired[KA_ROLE_PREV] || fired[KA_ROLE_NEXT])
    {
        std::vector<Row> rows = build_rows();
        int idx = selected_index(rows);
        if (fired[KA_LAYER_PREV]) select_row(rows, idx < 0 ? 0 : idx - 1);
        if (fired[KA_LAYER_NEXT]) select_row(rows, idx < 0 ? 0 : idx + 1);
        if (fired[KA_LAYER_PREV] || fired[KA_LAYER_NEXT]) { std::lock_guard<std::mutex> l(g.mtx); g.scroll_to_sel = true; }
        idx = selected_index(rows);
        if (idx >= 0)
        {
            if (fired[KA_FRAME_END_HERE])
                apply_role(rows[idx].p, rows[idx].slot, ROLE_SCENE);
            if (fired[KA_ROLE_PREV] || fired[KA_ROLE_NEXT])
            {
                int r = role_of(rows[idx].p, rows[idx].slot);
                r = (r + (fired[KA_ROLE_NEXT] ? 1 : ROLE_COUNT - 1)) % ROLE_COUNT;
                apply_role(rows[idx].p, rows[idx].slot, r);
            }
        }
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

    apply_shader_link(rt);

    std::lock_guard<std::mutex> lock(g.mtx);
    if (g.dirty)
    {
        g.dirty = false;
        save_settings_locked();
    }
}
