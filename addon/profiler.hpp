// CRCGI Injector - G-buffer profiler and VRAM texture viewer.
//
// D3D12 and Vulkan bind resources through descriptor heaps / sets, so a binding interval alone does not say which texture is the
// albedo or the normal buffer. The profiler works on the textures themselves:
//   1. Pool: every 2D render-target / UAV texture the game creates (init_resource; on D3D12 that covers CreateCommittedResource and
//      CreatePlacedResource, on Vulkan vkCreateImage), with its description and a run-to-run stable identity (TexSig).
//   2. Usage: per frame, how often each texture is bound as a render target, in how many-target bindings, at which slot, with depth.
//   3. Snapshot: on request, one copy of each candidate is taken at a point where its state is known (the end of a binding interval in
//      which it was a render target, or right after the game's own barrier out of the render-target state) and kept for the overlay.
//      No barrier is ever placed on a guessed state.
//   4. Analysis: a 4 x 4 grid of 64 x 64 tiles of each snapshot is read back to the CPU and measured (unit-length vectors, value range,
//      two-channel small vectors, ...), which scores albedo / normals / motion vectors and suggests the normal encoding and space.
//   5. Assignment: a texture can be used directly as the albedo, normal or motion feed ("by texture"), independent of layer keys.
#pragma once
#include "gpu.hpp"

struct Analysis
{
    bool done = false;
    std::string error;                 // why it could not be analysed
    int channels = 0;
    bool srgb = false, is_float = false, is_unorm = false;
    float mean[4] = {}, var[4] = {};
    float unit_frac = 0.0f;            // fraction of pixels whose rgb is a unit vector (after the better of v and v*2-1)
    bool unit_from_unorm = false;      // the unit vectors needed v*2-1
    float z_one_sign = 0.0f;           // fraction of the dominant sign of the vector's z (view-space normals: mostly one sign)
    float hdr_frac = 0.0f;             // fraction with a channel > 1.05
    float small2_frac = 0.0f;          // two-channel: fraction of |rg| < 0.02 (UV-sized motion)
    float small4_frac = 0.0f;          // four-channel: fraction of |rg| < 0.02 (velocity in rg, something else in ba)
    float luma_mean = 0.0f;
    float zero_frac = 0.0f;
    float s_albedo = 0.0f, s_normal = 0.0f, s_motion = 0.0f, s_material = 0.0f, s_hdr = 0.0f, s_scene = 0.0f;
    std::string label;
    float score(int feed) const
    {
        switch (feed)
        {
        case FEED_NORMALS: return s_normal;
        case FEED_ALBEDO: return s_albedo;
        case FEED_MOTION: return s_motion;
        case FEED_MATERIAL: return s_material;
        case FEED_SCENE: return s_scene;
        default: return s_hdr;
        }
    }
};

struct PoolTex
{
    resource res = { 0 };
    TexSig sig;
    format fmt = format::unknown;
    format view_fmt = format::unknown;   // format of the render-target view the game last bound it with
    uint64_t seq = 0;                    // creation order
    bool is_back_buffer = false;
    // usage in the frame being recorded (cur_) and in the last finished frame (l_)
    uint32_t cur_binds = 0, cur_draws = 0, cur_mrt = 0, cur_slot = 8, cur_order = UINT32_MAX;
    bool cur_depth = false;
    uint32_t l_binds = 0, l_draws = 0, l_mrt = 0, l_slot = 8, l_order = UINT32_MAX;
    bool l_depth = false;
    // The binding interval that drew most to this texture (its "main" interval): captures of a feed taken from the texture happen
    // only there, because engines reuse transient texture memory later in the frame for unrelated passes.
    uint64_t cur_main_key = 0, l_main_key = 0;
    uint32_t cur_main_draws = 0, l_main_draws = 0;
    bool cur_main_seen = false;          // the interval that was main so far occurred again in the frame being recorded
    uint32_t main_missing = 0;           // consecutive frames in which it did not
    uint32_t cur_dispatches = 0, l_dispatches = 0;   // compute dispatches that wrote it (where the API shows the bindings)
    uint32_t cur_clears = 0;
    uint64_t last_used = ~0ull;
    // snapshot + analysis
    bool snap_want = false;
    FeedTex snap;
    resource rb = { 0 };
    int rb_state = 0;                    // 0 idle, 1 waiting for the readback copy, 2 copied (waiting for the GPU), 3 analysed
    uint64_t rb_frame = 0;
    Analysis an;
};

struct Profiler
{
    std::mutex m;
    std::unordered_map<uint64_t, PoolTex> pool;
    uint64_t seq = 0;
    int snapshot_phase = 0;              // 0 idle, 1 requested, 2 capturing this frame
    uint64_t snapshot_frame = 0;
    int snap_limit = 16;
    int filter = 1;                      // 0 all, 1 same aspect as the screen and at least half its size, 2 exactly the screen's size
    uint64_t selected = 0;
    uint64_t rr_check_frame = 0;         // render-resolution detection runs every 60 frames
    uint64_t detected_rr = 0;            // (w << 32) | h of the detected render resolution, 0 = none
};

static Profiler prof;

// 0 all, 1 the screen's aspect and at least a third of its width (any render scale), 2 the screen size or the upscaler's render size.
static bool pool_candidate_size(const PoolTex &t, int filter)
{
    if (t.is_back_buffer || g.bb_w == 0)
        return false;
    if (filter == 2)
        return screen_sized(t.sig.w, t.sig.h);
    if (filter == 1)
    {
        const double a = static_cast<double>(t.sig.w) / t.sig.h, b = static_cast<double>(g.bb_w) / g.bb_h;
        return std::fabs(a - b) < 0.02 * b && t.sig.w * 3 + 3 >= g.bb_w;
    }
    return true;
}

// True when `key` is the binding interval that drew most to the texture in the last frame. Unknown (no frame seen yet) = false, so a
// feed is never captured from an interval that may be unrelated reuse of the same memory.
static bool pool_is_main_interval(resource res, uint64_t key)
{
    std::lock_guard<std::mutex> lock(prof.m);
    auto it = prof.pool.find(res.handle);
    return it != prof.pool.end() && it->second.l_main_key != 0 && it->second.l_main_key == key;
}

// True once the texture's main interval has ended in the frame being recorded (barrier-based captures wait for it).
static bool pool_main_seen(resource res)
{
    std::lock_guard<std::mutex> lock(prof.m);
    auto it = prof.pool.find(res.handle);
    return it != prof.pool.end() && it->second.l_main_key != 0 && it->second.cur_main_seen;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pool maintenance
// ---------------------------------------------------------------------------------------------------------------------
// A new texture takes over a feed whose texture it is: the same description and creation order, or (after a loading screen recreated
// the game's targets) the same description while the feed's texture is gone. The saved identity is updated to the new one.
static void match_feed_textures(resource res, const TexSig &sig)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    for (int i = 0; i < FEED_COUNT; i++)
    {
        FeedConfig &f = g.feed[i];
        if (f.source != SRC_TEXTURE || !f.tex.valid())
            continue;
        const bool lost = g.feed_res[i].handle == 0;
        if (f.tex == sig || (lost && (g.feed_lost_frame[i] != 0 && f.tex.same_desc(sig))) || (lost && f.tex.same_relative(sig)))
        {
            g.feed_res[i] = res;
            if (!(f.tex == sig))
            {
                f.tex = sig;
                g.dirty = true;
            }
            g.feed_lost_frame[i] = 0;
        }
    }
}

// The screen-relative size of a texture (thousandths of the back buffer), set once the back buffer is known.
static void set_relative_size(TexSig &s)
{
    if (g.bb_w != 0 && g.bb_h != 0)
    {
        s.rw = static_cast<uint32_t>((static_cast<uint64_t>(s.w) * 1000 + g.bb_w / 2) / g.bb_w);
        s.rh = static_cast<uint32_t>((static_cast<uint64_t>(s.h) * 1000 + g.bb_h / 2) / g.bb_h);
    }
}

// Saved or imported feeds whose texture is not found yet (the pool was filled before the back buffer was known, or a listing from
// another resolution was imported): look through the live textures again. Every 60 frames at most, only while something is missing.
static void rematch_feeds()
{
    bool missing = false;
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        if (g.frame < g.rematch_frame + 60)
            return;
        for (int i = 0; i < FEED_COUNT; i++)
            missing |= g.feed[i].source == SRC_TEXTURE && g.feed[i].tex.valid() && g.feed_res[i].handle == 0;
        g.rematch_frame = g.frame;
    }
    if (!missing)
        return;
    std::vector<std::pair<resource, TexSig>> live;
    {
        std::lock_guard<std::mutex> lock(prof.m);
        for (const auto &e : prof.pool)
        {
            TexSig s = e.second.sig;
            set_relative_size(s);
            live.emplace_back(e.second.res, s);
        }
    }
    for (const auto &l : live)
        match_feed_textures(l.first, l.second);
}

static void on_init_resource(device *dev, const resource_desc &d, const subresource_data *, resource_usage, resource res)
{
    if (d.type != resource_type::texture_2d || d.texture.depth_or_layers != 1)
        return;
    if (!has_usage(d.usage, resource_usage::render_target) && !has_usage(d.usage, resource_usage::unordered_access))
        return;
    if (has_usage(d.usage, resource_usage::depth_stencil) || d.texture.width < 128 || d.texture.height < 64)
        return;
    (void)dev;
    TexSig sig;
    sig.fmt = static_cast<uint32_t>(d.texture.format);
    sig.w = d.texture.width;
    sig.h = d.texture.height;
    sig.samples = d.texture.samples;
    sig.usage = static_cast<uint32_t>(d.usage);
    {
        std::lock_guard<std::mutex> lock(prof.m);
        // The lowest creation order not taken by a live texture of the same description: a game that recreates its targets (loading
        // screens, resolution changes) gives them the same identities again, so saved assignments keep working.
        std::vector<uint32_t> used;
        for (const auto &e : prof.pool)
            if (e.second.sig.same_desc(sig))
                used.push_back(e.second.sig.ordinal);
        uint32_t ord = 0;
        while (std::find(used.begin(), used.end(), ord) != used.end())
            ord++;
        sig.ordinal = ord;
        set_relative_size(sig);
        PoolTex &t = prof.pool[res.handle];
        t = PoolTex();
        t.res = res;
        t.sig = sig;
        t.fmt = d.texture.format;
        t.seq = prof.seq++;
    }
    match_feed_textures(res, sig);
}

// Called from on_destroy_resource. The snapshot copy is buried (the GPU may still use it).
static void pool_forget(resource res)
{
    FeedTex snap;
    resource rb = { 0 };
    {
        std::lock_guard<std::mutex> lock(prof.m);
        auto it = prof.pool.find(res.handle);
        if (it == prof.pool.end())
            return;
        snap = it->second.snap;
        rb = it->second.rb;
        prof.pool.erase(it);
    }
    bury_tex(snap);
    bury_resource(rb);
    std::lock_guard<std::mutex> lock(g.mtx);
    for (int i = 0; i < FEED_COUNT; i++)
        if (g.feed_res[i] == res)
        {
            g.feed_res[i] = resource{ 0 };
            g.feed_lost_frame[i] = std::max<uint64_t>(g.frame, 1);   // auto re-detect / re-adoption by the next matching texture
        }
}

// Stable per-run identity of a render target for layer keys (0 if the texture is not in the pool, e.g. the back buffer).
static uint64_t pool_identity(resource res)
{
    std::lock_guard<std::mutex> lock(prof.m);
    auto it = prof.pool.find(res.handle);
    if (it == prof.pool.end())
        return 0;
    return fnv64(&it->second.sig, sizeof(TexSig));
}

// A compute dispatch that writes the texture (Direct3D 10 / 11: the bound unordered access views). The first dispatch that writes it in a
// frame is its main one unless a draw interval writes it more.
static void pool_note_compute(resource res, uint64_t key)
{
    std::lock_guard<std::mutex> lock(prof.m);
    auto it = prof.pool.find(res.handle);
    if (it == prof.pool.end())
        return;
    PoolTex &t = it->second;
    t.cur_dispatches++;
    t.last_used = g.frame;
    if (key == t.l_main_key)
        t.cur_main_seen = true;
    if (t.cur_main_key == 0)
    {
        t.cur_main_key = key;
        t.cur_main_draws = 1;
    }
}

// Usage statistics of one finished binding interval.
static void pool_note_interval(const CmdState &cs, uint32_t order)
{
    std::lock_guard<std::mutex> lock(prof.m);
    for (uint32_t s = 0; s < cs.rt_count && s < 8; s++)
    {
        if (cs.rt[s].res.handle == 0)
            continue;
        auto it = prof.pool.find(cs.rt[s].res.handle);
        if (it == prof.pool.end())
            continue;
        PoolTex &t = it->second;
        t.cur_binds++;
        t.cur_draws += cs.draws;
        if (cs.pass_key == t.l_main_key)
            t.cur_main_seen = true;
        if (cs.draws > t.cur_main_draws)
        {
            t.cur_main_draws = cs.draws;
            t.cur_main_key = cs.pass_key;
        }
        t.cur_mrt = std::max(t.cur_mrt, cs.rt_count);
        t.cur_slot = std::min(t.cur_slot, s);
        t.cur_depth |= cs.dsv.handle != 0;
        t.cur_order = std::min(t.cur_order, order);
        if (cs.rt[s].view_fmt != format::unknown)
            t.view_fmt = cs.rt[s].view_fmt;
        t.last_used = g.frame;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Snapshot capture (called where the texture's state is known)
// ---------------------------------------------------------------------------------------------------------------------
// barrier_path: Vulkan, where nothing can be copied inside a render pass: the copy is taken right after the game's own barrier out of the
// render-target state, but only once the texture's main binding has ended in this frame (later reuse of the memory is not captured)
// and only if no capture was taken this frame yet. Direct3D 12 never captures at barriers: a split barrier's BEGIN_ONLY half cannot be
// told from a full transition, and a copy between its halves is invalid.
static void pool_capture(command_list *cl, resource res, resource_usage state, format view_fmt, bool barrier_path = false)
{
    std::lock_guard<std::mutex> lock(prof.m);
    if (prof.snapshot_phase != 2)
        return;
    auto it = prof.pool.find(res.handle);
    if (it == prof.pool.end() || !it->second.snap_want)
        return;
    PoolTex &t = it->second;
    const bool have_this_frame = t.snap.frame != ~0ull && t.snap.frame >= prof.snapshot_frame;
    if (have_this_frame)
        return;                                              // one capture per texture in the snapshot frame
    if (barrier_path && !(t.l_main_key != 0 && t.cur_main_seen))
        return;
    RtSlot src;
    src.res = res;
    src.w = t.sig.w;
    src.h = t.sig.h;
    src.samples = t.sig.samples;
    src.fmt = t.fmt;
    src.view_fmt = view_fmt != format::unknown ? view_fmt : t.view_fmt;
    if (copy_into(cl, state, t.snap, src, -1))
        t.rb_state = 0;
}

// Frame boundary: roll the usage counters and advance the snapshot state machine.
static void pool_end_frame()
{
    std::lock_guard<std::mutex> lock(prof.m);
    for (auto &e : prof.pool)
    {
        PoolTex &t = e.second;
        t.l_binds = t.cur_binds; t.l_draws = t.cur_draws; t.l_mrt = t.cur_mrt; t.l_slot = t.cur_slot; t.l_depth = t.cur_depth; t.l_order = t.cur_order;
        t.l_dispatches = t.cur_dispatches;
        // The main binding only changes when the old one is gone for half a second or the new one draws more than twice as much, so a
        // game that culls differently from frame to frame cannot make the capture point jump between two bindings.
        if (t.cur_main_key != 0)
        {
            if (t.l_main_key == 0 || t.cur_main_key == t.l_main_key)
            {
                t.l_main_key = t.cur_main_key;
                t.l_main_draws = t.cur_main_draws;
                t.main_missing = 0;
            }
            else
            {
                t.main_missing = t.cur_main_seen ? 0 : t.main_missing + 1;
                if (t.main_missing >= 30 || t.cur_main_draws > t.l_main_draws * 2)
                {
                    t.l_main_key = t.cur_main_key;
                    t.l_main_draws = t.cur_main_draws;
                    t.main_missing = 0;
                }
            }
        }
        t.cur_main_key = 0;
        t.cur_main_draws = 0;
        t.cur_main_seen = false;
        t.cur_dispatches = 0;
        t.cur_clears = 0;
        t.cur_binds = t.cur_draws = t.cur_mrt = 0;
        t.cur_slot = 8;
        t.cur_order = UINT32_MAX;
        t.cur_depth = false;
        if (t.res.handle == g.bb_res.handle)
            t.is_back_buffer = true;
    }
    if (g.frame >= prof.rr_check_frame + 60)
    {
        // Render resolution of an upscaler: the most-drawn size below the screen's, of the screen's aspect, drawn with depth.
        prof.rr_check_frame = g.frame;
        std::unordered_map<uint64_t, uint64_t> draws;
        for (const auto &e : prof.pool)
        {
            const PoolTex &t = e.second;
            if (t.l_binds == 0 || !t.l_depth || t.is_back_buffer || t.sig.w >= g.bb_w || t.sig.w * 3 + 3 < g.bb_w)
                continue;
            const double a = static_cast<double>(t.sig.w) / t.sig.h, b = static_cast<double>(g.bb_w) / std::max<uint32_t>(g.bb_h, 1);
            if (std::fabs(a - b) < 0.01 * b)
                draws[(static_cast<uint64_t>(t.sig.w) << 32) | t.sig.h] += t.l_draws;
        }
        uint64_t best = 0, best_draws = 0;
        for (const auto &d : draws)
            if (d.second > best_draws) { best = d.first; best_draws = d.second; }
        prof.detected_rr = best_draws >= 20 ? best : 0;
    }
    if (prof.snapshot_phase == 2)
    {
        // The capture frame is over: every candidate that got a copy is queued for the readback.
        for (auto &e : prof.pool)
        {
            PoolTex &t = e.second;
            if (t.snap_want && t.snap.frame != ~0ull && t.snap.frame >= prof.snapshot_frame && t.snap.srv.handle != 0)
            {
                t.rb_state = 1;
                t.an = Analysis();
            }
            else if (t.snap_want)
                t.an.error = t.snap.status.empty() ? "not captured: its state was never known in that frame" : t.snap.status;
            t.snap_want = false;
        }
        prof.snapshot_phase = 0;
    }
    if (prof.snapshot_phase == 1)
    {
        // Candidates: used as a render target in the last frame, of a useful size, most-drawn first.
        std::vector<PoolTex *> c;
        for (auto &e : prof.pool)
            if ((e.second.l_binds > 0 || e.second.l_dispatches > 0) && pool_candidate_size(e.second, prof.filter) && e.second.sig.samples == 1)
                c.push_back(&e.second);
        std::sort(c.begin(), c.end(), [](const PoolTex *a, const PoolTex *b) { return a->l_draws > b->l_draws; });
        if (c.size() > static_cast<size_t>(prof.snap_limit))
            c.resize(prof.snap_limit);
        for (PoolTex *t : c)
            t->snap_want = true;
        prof.snapshot_phase = 2;
        prof.snapshot_frame = g.frame + 1;   // the frame that starts now (g.frame is incremented right after this); a capture in it has frame == snapshot_frame
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Readback and analysis (runtime thread, ReShade's own command list: only the add-on's own textures are touched)
// ---------------------------------------------------------------------------------------------------------------------
static float half_to_float(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
    return s ? -v : v;
}

static float f11_to_float(uint32_t v, int mbits)   // unsigned 11 / 10 bit float (5-bit exponent)
{
    const uint32_t e = (v >> mbits) & 0x1F, m = v & ((1u << mbits) - 1);
    if (e == 0) return std::ldexp(static_cast<float>(m), -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(static_cast<float>(m | (1u << mbits)), static_cast<int>(e) - 15 - mbits);
}

// Decodes one texel to float4. Returns the channel count, 0 if the format is not supported.
static int decode_texel(format f, const uint8_t *p, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
    auto un8 = [](uint8_t v) { return v / 255.0f; };
    auto sn8 = [](uint8_t v) { return std::max(static_cast<int8_t>(v) / 127.0f, -1.0f); };
    auto un16 = [&](int i) { uint16_t v; std::memcpy(&v, p + 2 * i, 2); return v / 65535.0f; };
    auto sn16 = [&](int i) { int16_t v; std::memcpy(&v, p + 2 * i, 2); return std::max(v / 32767.0f, -1.0f); };
    auto hf = [&](int i) { uint16_t v; std::memcpy(&v, p + 2 * i, 2); return half_to_float(v); };
    auto f32 = [&](int i) { float v; std::memcpy(&v, p + 4 * i, 4); return v; };
    switch (f)
    {
    case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb: case format::r8g8b8x8_unorm: case format::r8g8b8x8_unorm_srgb:
        for (int i = 0; i < 4; i++) out[i] = un8(p[i]);
        return 4;
    case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb: case format::b8g8r8x8_unorm: case format::b8g8r8x8_unorm_srgb:
        out[0] = un8(p[2]); out[1] = un8(p[1]); out[2] = un8(p[0]); out[3] = un8(p[3]);
        return 4;
    case format::r8g8b8a8_snorm:
        for (int i = 0; i < 4; i++) out[i] = sn8(p[i]);
        return 4;
    case format::r10g10b10a2_unorm:
    {
        uint32_t v; std::memcpy(&v, p, 4);
        out[0] = (v & 0x3FF) / 1023.0f; out[1] = ((v >> 10) & 0x3FF) / 1023.0f; out[2] = ((v >> 20) & 0x3FF) / 1023.0f; out[3] = (v >> 30) / 3.0f;
        return 4;
    }
    case format::r11g11b10_float:
    {
        uint32_t v; std::memcpy(&v, p, 4);
        out[0] = f11_to_float(v & 0x7FF, 6); out[1] = f11_to_float((v >> 11) & 0x7FF, 6); out[2] = f11_to_float(v >> 22, 5);
        return 3;
    }
    case format::r16g16b16a16_float: for (int i = 0; i < 4; i++) out[i] = hf(i); return 4;
    case format::r16g16b16a16_unorm: for (int i = 0; i < 4; i++) out[i] = un16(i); return 4;
    case format::r16g16b16a16_snorm: for (int i = 0; i < 4; i++) out[i] = sn16(i); return 4;
    case format::r32g32b32a32_float: for (int i = 0; i < 4; i++) out[i] = f32(i); return 4;
    case format::r16g16_float: out[0] = hf(0); out[1] = hf(1); return 2;
    case format::r16g16_unorm: out[0] = un16(0); out[1] = un16(1); return 2;
    case format::r16g16_snorm: out[0] = sn16(0); out[1] = sn16(1); return 2;
    case format::r32g32_float: out[0] = f32(0); out[1] = f32(1); return 2;
    case format::r8g8_unorm: out[0] = un8(p[0]); out[1] = un8(p[1]); return 2;
    case format::r8g8_snorm: out[0] = sn8(p[0]); out[1] = sn8(p[1]); return 2;
    case format::r16_float: out[0] = hf(0); return 1;
    case format::r16_unorm: out[0] = un16(0); return 1;
    case format::r32_float: out[0] = f32(0); return 1;
    case format::r8_unorm: out[0] = un8(p[0]); return 1;
    default: return 0;
    }
}

static uint32_t texel_size(format f)
{
    switch (format_to_typeless(f))
    {
    case format::r8g8b8a8_typeless: case format::b8g8r8a8_typeless: case format::b8g8r8x8_typeless: case format::r10g10b10a2_typeless:
    case format::r16g16_typeless: case format::r32_typeless:
        return 4;
    case format::r16g16b16a16_typeless: case format::r32g32_typeless: return 8;
    case format::r32g32b32a32_typeless: return 16;
    case format::r8g8_typeless: case format::r16_typeless: return 2;
    case format::r8_typeless: return 1;
    default: break;
    }
    if (f == format::r11g11b10_float) return 4;
    return 0;
}

static const uint32_t RB_TILE = 64, RB_GRID = 4, RB_SIZE = RB_TILE * RB_GRID;

static void analyse(PoolTex &t, const uint8_t *data, uint32_t row_pitch)
{
    Analysis &a = t.an;
    a = Analysis();
    format f = t.view_fmt != format::unknown ? t.view_fmt : format_to_default_typed(t.fmt, 0);
    if (format_to_typeless(f) == f)
        f = format_to_default_typed(f, 0);
    const uint32_t ts = texel_size(t.fmt);
    float px[4];
    if (ts == 0 || decode_texel(f, data, px) == 0)
    {
        a.done = true;
        a.error = std::string("format not analysed (") + fmt_name(f) + ")";
        return;
    }
    a.srgb = fmt_is_srgb(f);
    a.is_float = fmt_is_float(f) || f == format::r16_float || f == format::r32_float;
    a.is_unorm = !a.is_float && f != format::r8g8b8a8_snorm && f != format::r16g16b16a16_snorm && f != format::r16g16_snorm && f != format::r8g8_snorm;
    double sum[4] = {}, sq[4] = {};
    uint64_t n = 0, unit_raw = 0, unit_unorm = 0, zpos_raw = 0, zpos_unorm = 0, nz = 0, hdr = 0, small2 = 0, small4 = 0, zero = 0;
    for (uint32_t y = 0; y < RB_SIZE; y++)
    {
        const uint8_t *row = data + static_cast<size_t>(y) * row_pitch;
        for (uint32_t x = 0; x < RB_SIZE; x++)
        {
            const int ch = decode_texel(f, row + static_cast<size_t>(x) * ts, px);
            a.channels = ch;
            bool bad = false;
            for (int i = 0; i < 4; i++)
                if (!std::isfinite(px[i])) bad = true;
            if (bad)
                continue;
            n++;
            for (int i = 0; i < 4; i++) { sum[i] += px[i]; sq[i] += static_cast<double>(px[i]) * px[i]; }
            if (std::fabs(px[0]) + std::fabs(px[1]) + std::fabs(px[2]) < 1.0e-6f) { zero++; continue; }
            if (std::max(px[0], std::max(px[1], px[2])) > 1.05f) hdr++;
            if (ch >= 3)
            {
                const float lr = std::sqrt(px[0] * px[0] + px[1] * px[1] + px[2] * px[2]);
                const float u0 = px[0] * 2 - 1, u1 = px[1] * 2 - 1, u2 = px[2] * 2 - 1;
                const float lu = std::sqrt(u0 * u0 + u1 * u1 + u2 * u2);
                nz++;
                if (std::fabs(lr - 1.0f) < 0.1f) unit_raw++;
                if (std::fabs(lu - 1.0f) < 0.1f) unit_unorm++;
                if (px[2] > 0.0f) zpos_raw++;
                if (u2 > 0.0f) zpos_unorm++;
            }
            if (ch == 2 && std::sqrt(px[0] * px[0] + px[1] * px[1]) < 0.02f) small2++;
            if (ch == 4 && std::sqrt(px[0] * px[0] + px[1] * px[1]) < 0.02f) small4++;
        }
    }
    a.done = true;
    if (n == 0)
    {
        a.error = "no finite pixels";
        return;
    }
    for (int i = 0; i < 4; i++)
    {
        a.mean[i] = static_cast<float>(sum[i] / n);
        a.var[i] = static_cast<float>(std::max(sq[i] / n - (sum[i] / n) * (sum[i] / n), 0.0));
    }
    a.zero_frac = static_cast<float>(zero) / n;
    a.hdr_frac = static_cast<float>(hdr) / n;
    a.small2_frac = static_cast<float>(small2) / n;
    a.small4_frac = static_cast<float>(small4) / n;
    if (nz > 0)
    {
        const bool use_unorm = a.is_unorm && unit_unorm >= unit_raw;
        a.unit_from_unorm = use_unorm;
        a.unit_frac = static_cast<float>(use_unorm ? unit_unorm : unit_raw) / nz;
        const float zp = static_cast<float>(use_unorm ? zpos_unorm : zpos_raw) / nz;
        a.z_one_sign = std::max(zp, 1.0f - zp);
    }
    const float var3 = a.var[0] + a.var[1] + a.var[2];
    const float lum = 0.2126f * a.mean[0] + 0.7152f * a.mean[1] + 0.0722f * a.mean[2];
    const bool lively = var3 > 1.0e-4f && a.zero_frac < 0.97f;

    // Normals: unit-length vectors in most covered pixels, with variety.
    if (a.channels >= 3 && lively)
        a.s_normal = std::clamp((a.unit_frac - 0.25f) / 0.6f, 0.0f, 1.0f);
    // Octahedral normals in two channels: values spread over the square (not small like motion).
    if (a.channels == 2 && lively && a.small2_frac < 0.3f && var3 > 0.02f)
        a.s_normal = std::max(a.s_normal, 0.35f);
    // Albedo: three or more channels in 0..1, not unit vectors, plausible mean, not HDR. sRGB views and 8-bit storage are typical;
    // a non-sRGB RGBA8 or R11G11B10 target is just as common (the game decodes the sRGB itself), so 4-byte storage alone clears the
    // threshold unless something else speaks against it.
    if (a.channels >= 3 && lively && a.hdr_frac < 0.005f)
    {
        float s = 0.5f;
        s += a.srgb ? 0.2f : 0.0f;
        s += (texel_size(t.fmt) == 4) ? 0.12f : 0.0f;                       // RGBA8 / RGB10A2 / RG16 / R11G11B10: compact colour storage
        s -= std::clamp((a.unit_frac - 0.3f) * 1.5f, 0.0f, 0.6f);
        s -= (lum < 0.02f || lum > 0.9f) ? 0.3f : 0.0f;
        a.s_albedo = std::clamp(s, 0.0f, 1.0f);
    }
    // Motion vectors: two channels, mostly tiny vectors (UV units) or a float format with a narrow distribution. Four channels:
    // velocity in two of them (xy) and something else in the other two (depth, a mask): the xy pair must be small, the target must be
    // float or signed, and it must not look like colour or like unit vectors.
    if (a.channels == 2)
    {
        float s = 0.3f + 0.6f * a.small2_frac;
        if (a.is_float) s += 0.1f;
        a.s_motion = std::clamp(s, 0.0f, 1.0f);
    }
    else if (a.channels == 4 && (a.is_float || !a.is_unorm) && a.small4_frac > 0.5f && a.hdr_frac < 0.02f && a.unit_frac < 0.3f)
    {
        float s = 0.25f + 0.55f * a.small4_frac;
        if (a.is_float) s += 0.1f;
        a.s_motion = std::clamp(s, 0.0f, 0.95f);
    }
    a.luma_mean = lum;
    // Usage bonus: textures written together with others in one many-target binding with depth are G-buffer members.
    if (t.l_mrt >= 2 && t.l_depth)
    {
        a.s_albedo = std::min(1.0f, a.s_albedo + (a.s_albedo > 0 ? 0.1f : 0.0f) + (t.l_slot == 0 && a.s_albedo > 0 ? 0.05f : 0.0f));
        a.s_normal = std::min(1.0f, a.s_normal + (a.s_normal > 0 ? 0.1f : 0.0f));
    }
    // HDR scene light: a floating-point colour target (the lighting before tonemapping). Values above 1 are typical outdoors and raise
    // the score, but dark, indoor or overcast scenes keep everything inside 0..1, so they are a bonus, not a requirement.
    if (a.channels >= 3 && a.is_float && lively && a.unit_frac < 0.5f && lum > 0.0005f && lum < 50.0f)
    {
        float s = 0.45f;
        s += a.hdr_frac > 0.003f ? 0.1f + std::min(a.hdr_frac * 4.0f, 0.2f) : 0.0f;
        s += t.l_mrt <= 1 ? 0.1f : 0.0f;                                    // written alone (lighting / composite), not a G-buffer member
        s += t.l_draws >= 50 ? 0.1f : 0.0f;                                 // forward / transparent draws land in it
        s += (t.sig.w == g.bb_w && t.sig.h == g.bb_h) || screen_sized(t.sig.w, t.sig.h) ? 0.05f : 0.0f;
        a.s_hdr = std::clamp(s, 0.0f, 1.0f);
    }
    // Finished frame: a screen-sized colour target in the back buffer's format family, drawn without depth by few draws, plausibly lit.
    // Which of the late targets is the last one before the UI cannot be told from the contents: the layer list (Kind, position) helps.
    if (a.channels >= 3 && lively && t.sig.w == g.bb_w && t.sig.h == g.bb_h && same_format_family(t.fmt, g.bb_fmt) && a.unit_frac < 0.5f &&
        t.l_mrt <= 1 && !t.l_depth)
    {
        float s = 0.55f;
        s += (lum > 0.02f && lum < 0.9f) ? 0.1f : 0.0f;
        s += t.l_order != UINT32_MAX ? 0.05f : 0.0f;
        a.s_scene = std::clamp(s, 0.0f, 0.8f);
    }
    // Material (metalness / specular / roughness): an 8-bit member of a many-target G-buffer that is neither albedo nor normals.
    // Which channel means what cannot be told from the contents: it is offered as a candidate, never assigned automatically.
    if (t.l_mrt >= 3 && t.l_depth && a.channels >= 3 && !a.is_float && lively && a.s_normal < 0.6f && !a.srgb)
        a.s_material = 0.5f;
    if (a.s_hdr >= 0.6f)
        a.label = "HDR scene light (lighting before tonemapping)";
    else if (a.s_scene >= 0.6f && a.s_albedo < 0.6f && a.s_normal < 0.6f)
        a.label = "screen-format colour (maybe the finished frame)";
    else if (a.hdr_frac > 0.01f && a.channels >= 3)
        a.label = "lit colour / HDR (not albedo)";
    else if (a.s_normal >= 0.6f && a.s_normal >= a.s_albedo)
        a.label = std::string("normals (") + (a.channels == 2 ? "octahedral" : (a.unit_from_unorm ? "RGB *2-1" : "RGB signed")) + ", " +
                  (a.z_one_sign > 0.9f ? "probably view space" : "probably world space") + ")";
    else if (a.s_albedo >= 0.6f)
        a.label = "albedo / base colour";
    else if (a.s_motion >= 0.6f)
        a.label = a.channels == 4 ? "motion vectors (xy of four channels)" : "motion vectors";
    else if (a.s_material > 0.0f)
        a.label = "G-buffer member: maybe material (metalness / specular / roughness)";
    else if (a.channels == 1)
        a.label = "single channel (depth, AO, mask, ...)";
    else
        a.label = "unclear";
}

static void pool_readback_step(effect_runtime *rt)
{
    device *dev = rt->get_device();
    command_queue *q = rt->get_command_queue();
    if (q == nullptr)
        return;
    std::vector<std::pair<uint64_t, resource>> to_bury;
    bool flushed = false;
    std::lock_guard<std::mutex> lock(prof.m);
    for (auto &e : prof.pool)
    {
        PoolTex &t = e.second;
        if (t.rb_state == 1)
        {
            if (t.snap.res.handle == 0 || texel_size(t.fmt) == 0 || t.sig.w < RB_SIZE || t.sig.h < RB_SIZE)
            {
                t.an = Analysis();
                t.an.done = true;
                t.an.error = texel_size(t.fmt) == 0 ? std::string("format not analysed (") + fmt_name(t.fmt) + ")" : "too small to analyse";
                t.rb_state = 3;
                continue;
            }
            const format tl = format_to_typeless(t.fmt) != format::unknown ? format_to_typeless(t.fmt) : t.fmt;
            resource_desc d(RB_SIZE, RB_SIZE, 1, 1, tl, 1, memory_heap::readback, resource_usage::copy_dest);
            if (!dev->create_resource(d, nullptr, resource_usage::copy_dest, &t.rb))
            {
                d.texture.format = t.fmt;
                if (!dev->create_resource(d, nullptr, resource_usage::copy_dest, &t.rb))
                {
                    t.rb = { 0 };
                    t.an = Analysis();
                    t.an.done = true;
                    t.an.error = "the driver refused a readback texture";
                    t.rb_state = 3;
                    continue;
                }
            }
            command_list *cl = q->get_immediate_command_list();
            const bool ex = explicit_states(dev);
            if (ex)
                cl->barrier(t.snap.res, resource_usage::shader_resource, resource_usage::copy_source);
            for (uint32_t gy = 0; gy < RB_GRID; gy++)
                for (uint32_t gx = 0; gx < RB_GRID; gx++)
                {
                    const uint32_t sx = std::min((gx * 2 + 1) * t.sig.w / (RB_GRID * 2) - RB_TILE / 2, t.sig.w - RB_TILE);
                    const uint32_t sy = std::min((gy * 2 + 1) * t.sig.h / (RB_GRID * 2) - RB_TILE / 2, t.sig.h - RB_TILE);
                    const subresource_box sb = { static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), 0,
                                                 static_cast<uint32_t>(sx + RB_TILE), static_cast<uint32_t>(sy + RB_TILE), 1 };
                    const subresource_box db = { static_cast<uint32_t>(gx * RB_TILE), static_cast<uint32_t>(gy * RB_TILE), 0,
                                                 static_cast<uint32_t>((gx + 1) * RB_TILE), static_cast<uint32_t>((gy + 1) * RB_TILE), 1 };
                    cl->copy_texture_region(t.snap.res, 0, &sb, t.rb, 0, &db, filter_mode::min_mag_mip_point);
                }
            if (ex)
                cl->barrier(t.snap.res, resource_usage::copy_source, resource_usage::shader_resource);
            flushed = true;
            t.rb_frame = g.frame;
            t.rb_state = 2;
        }
        else if (t.rb_state == 2 && g.frame >= t.rb_frame + 6)
        {
            subresource_data sd = {};
            if (dev->map_texture_region(t.rb, 0, nullptr, map_access::read_only, &sd) && sd.data != nullptr)
            {
                analyse(t, static_cast<const uint8_t *>(sd.data), sd.row_pitch);
                dev->unmap_texture_region(t.rb, 0);
            }
            else
            {
                t.an = Analysis();
                t.an.done = true;
                t.an.error = "the readback texture could not be mapped";
            }
            to_bury.emplace_back(e.first, t.rb);
            t.rb = { 0 };
            t.rb_state = 3;
        }
    }
    if (flushed)
        q->flush_immediate_command_list();
    for (auto &b : to_bury)
        bury_resource(b.second);
}

// ---------------------------------------------------------------------------------------------------------------------
// Assignment
// ---------------------------------------------------------------------------------------------------------------------
static void assign_texture_feed(int feed, const PoolTex &t)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    FeedConfig &f = g.feed[feed];
    f.source = SRC_TEXTURE;
    f.tex = t.sig;
    set_relative_size(f.tex);                    // so a shared listing finds it again at another screen resolution
    f.enabled = true;
    if (feed == FEED_NORMALS && t.an.done && t.an.error.empty())
    {
        f.mode = t.an.channels == 2 ? 1 : (t.an.unit_from_unorm ? 0 : 2);
        f.space = t.an.z_one_sign > 0.9f ? 0 : 1;   // not clearly view space: let the shader auto-align it
    }
    g.feed_res[feed] = t.res;
    g.feed_lost_frame[feed] = 0;
    if (g.level < LEVEL_C)
        g.level = LEVEL_C;
    g.dirty = true;
}

// The best-scoring analysed texture per feed, as suggestions for the Profiler tab (the user assigns; nothing is assigned here).
struct Suggestion { bool have = false; float score = 0.0f; TexSig sig; format fmt = format::unknown; resource res = { 0 }; };

static void profiler_suggestions(Suggestion out[FEED_COUNT])
{
    std::lock_guard<std::mutex> lock(prof.m);
    uint32_t best_draws[FEED_COUNT] = {};
    for (const auto &e : prof.pool)
    {
        const PoolTex &t = e.second;
        if (!t.an.done || !t.an.error.empty())
            continue;
        for (int i = 0; i < FEED_COUNT; i++)
        {
            const float sc = t.an.score(i);
            const float need = i == FEED_MATERIAL ? 0.45f : 0.6f;
            // ties: the texture drawn to most wins (the main target, not a copy)
            if (sc >= need && (sc > out[i].score || (sc == out[i].score && out[i].have && t.l_draws > best_draws[i])))
            {
                out[i].have = true;
                out[i].score = sc;
                out[i].sig = t.sig;
                out[i].fmt = t.fmt;
                out[i].res = t.res;
                best_draws[i] = t.l_draws;
            }
        }
    }
}

static void pool_present(effect_runtime *rt)
{
    pool_readback_step(rt);
    rematch_feeds();
}

// Snapshot, then analysis: the Profiler tab shows the scores. Nothing is assigned.
static void pool_request_snapshot()
{
    std::lock_guard<std::mutex> lock(prof.m);
    if (prof.snapshot_phase == 0)
        prof.snapshot_phase = 1;
}

static void pool_release_snapshots()
{
    std::vector<FeedTex> snaps;
    {
        std::lock_guard<std::mutex> lock(prof.m);
        for (auto &e : prof.pool)
        {
            if (e.second.rb_state == 1 || e.second.rb_state == 2)
                continue;                   // still in use by a readback
            if (e.second.snap.res.handle != 0)
                snaps.push_back(e.second.snap);
            e.second.snap = FeedTex();
        }
    }
    for (FeedTex &s : snaps)
        bury_tex(s);
}

static void pool_clear(device *dev)
{
    std::lock_guard<std::mutex> lock(prof.m);
    for (auto &e : prof.pool)
    {
        destroy_tex(dev, e.second.snap);
        if (e.second.rb.handle != 0)
            dev->destroy_resource(e.second.rb);
    }
    prof.pool.clear();
}
