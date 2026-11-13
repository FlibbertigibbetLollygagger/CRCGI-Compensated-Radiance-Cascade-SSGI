// CRCGI Injector - camera probe: finds a perspective projection matrix in the game's constant buffer writes and derives
// vertical FOV, near/far plane and depth direction from it. Heuristic: it reports what it found and the user can override it.
#pragma once
#include "state.hpp"

static const float kPi = 3.14159265358979f;

// A perspective projection in canonical form A (row i, column j):
//   [ a  0  0  0 ]
//   [ 0  b  0  0 ]      A(2,3) = +-1 (the w term), jitter may sit in A(2,0), A(2,1)
//   [ j1 j2 c  s ]      c, d give near and far
//   [ 0  0  d  0 ]
// Memory can hold it row-major or column-major, so both readings are tested.
static bool test_projection(const float *m, bool transposed, CamCandidate &out)
{
    auto A = [&](int i, int j) { return transposed ? m[j * 4 + i] : m[i * 4 + j]; };
    float a = A(0, 0), b = A(1, 1), c = A(2, 2), d = A(3, 2), s = A(2, 3);
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d))
        return false;
    float aa = std::fabs(a), bb = std::fabs(b), ss = std::fabs(s);
    if (ss < 0.999f || ss > 1.001f || aa < 0.1f || aa > 30.0f || bb < 0.1f || bb > 30.0f)
        return false;
    const float eps = 1.0e-4f * std::max(aa, bb);
    if (std::fabs(A(0, 1)) > eps || std::fabs(A(0, 2)) > eps || std::fabs(A(0, 3)) > eps) return false;
    if (std::fabs(A(1, 0)) > eps || std::fabs(A(1, 2)) > eps || std::fabs(A(1, 3)) > eps) return false;
    if (std::fabs(A(3, 0)) > eps || std::fabs(A(3, 1)) > eps || std::fabs(A(3, 3)) > eps) return false;
    if (std::fabs(A(2, 0)) > 0.05f || std::fabs(A(2, 1)) > 0.05f) return false;
    if (std::fabs(d) < 1.0e-9f)
        return false;

    out.fov = 2.0f * std::atan(1.0f / bb) * 180.0f / kPi;
    if (out.fov < 8.0f || out.fov > 160.0f)
        return false;
    out.aspect = bb / aa;

    float n, f;
    bool reversed;
    if (std::fabs(c) < 1.0e-4f)                 { reversed = true;  n = std::fabs(d); f = INFINITY; }   // infinite far, reversed
    else if (std::fabs(c - 1.0f) < 1.0e-4f)     { reversed = false; n = std::fabs(d); f = INFINITY; }   // infinite far
    else
    {
        float n0 = std::fabs(d / c), f0 = std::fabs(d / (c - 1.0f));
        reversed = n0 > f0;
        n = std::min(n0, f0);
        f = std::max(n0, f0);
    }
    if (!(n > 0.0f) || (std::isfinite(f) && f < n * 1.5f))
        return false;
    out.nearp = n;
    out.farp = f;
    out.reversed = reversed;
    out.transposed = transposed;
    out.sig = static_cast<int64_t>(std::llround(out.fov * 20.0f)) * 1000003LL +
              static_cast<int64_t>(std::llround(std::log(std::max(n, 1.0e-4f)) * 20.0f)) * 1009LL + (reversed ? 1 : 0);
    return true;
}

static void note_candidate(const CamCandidate &c)
{
    std::lock_guard<std::mutex> lock(g.mtx);
    for (CamCandidate &e : g.cams)
        if (e.sig == c.sig)
        {
            e.hits++;
            e.res = c.res;
            e.offset = c.offset;
            e.last_frame = g.frame;
            e.transposed = c.transposed;
            return;
        }
    if (g.cams.size() >= 48)
    {
        auto worst = std::min_element(g.cams.begin(), g.cams.end(), [](const CamCandidate &x, const CamCandidate &y) { return x.hits < y.hits; });
        g.cams.erase(worst);
    }
    CamCandidate n = c;
    n.hits = 1;
    n.last_frame = g.frame;
    g.cams.push_back(n);
}

// Scans a block of memory that was written to a buffer (a constant buffer update). Cheap early-outs keep this fast.
static void scan_buffer(uint64_t res, uint64_t base_offset, const void *data, uint64_t size)
{
    if (data == nullptr || size < 64 || size > 65536)
        return;
    {   // per-frame budget so a game with huge buffers cannot slow the render thread down
        std::lock_guard<std::mutex> lock(g.mtx);
        if (g.cam_budget_frame != g.frame)
        {
            g.cam_budget_frame = g.frame;
            g.cam_budget_bytes = 0;
        }
        if (g.cam_budget_bytes > (8u << 20))
            return;
        g.cam_budget_bytes += size;
    }
    const float *f = static_cast<const float *>(data);
    uint64_t count = size / 16;                    // candidate start positions, every 4 floats
    for (uint64_t i = 0; i + 4 <= count; i++)      // 4 vec4 = one matrix
    {
        const float *m = f + i * 4;
        for (int t = 0; t < 2; t++)
        {
            CamCandidate c;
            if (!test_projection(m, t == 1, c))
                continue;
            c.res = res;
            c.offset = base_offset + i * 16;
            note_candidate(c);
            std::lock_guard<std::mutex> lock(g.mtx);
            if (g.cam_valid && g.cam_track_res == res && g.cam_track_off == c.offset && g.cam_track_t == c.transposed)
            {
                g.cam = c;
                g.cam.hits = 1;
            }
            break;
        }
    }
}

// Once per frame: pick the camera. The user's earlier choice (by signature) wins. Otherwise the most frequent plausible candidate is
// used only if it is clearly the most frequent one: when another plausible candidate with a different field of view is seen almost as
// often (a shadow, reflection or second view camera), the choice is ambiguous and nothing is picked automatically.
static void choose_camera()
{
    std::lock_guard<std::mutex> lock(g.mtx);
    double bb_aspect = g.bb_h ? static_cast<double>(g.bb_w) / g.bb_h : 0.0;
    const CamCandidate *best = nullptr, *second = nullptr, *pinned = nullptr;
    for (const CamCandidate &c : g.cams)
    {
        if (c.last_frame + 20 < g.frame || c.hits < 8)
            continue;
        if (bb_aspect > 0.0 && std::fabs(c.aspect / bb_aspect - 1.0) > 0.25)
            continue;
        if (g.cam_sig != 0 && c.sig == g.cam_sig)
            pinned = &c;
        if (!best || c.hits > best->hits) { second = best; best = &c; }
        else if (!second || c.hits > second->hits) second = &c;
    }
    g.cam_ambiguous = best && second && std::fabs(best->fov - second->fov) > 1.0f && second->hits * 5 >= best->hits * 4;
    if (!g.cam_auto && g.cam_valid)
        return;                                              // the user picked one: keep it
    const CamCandidate *use = pinned ? pinned : (g.cam_ambiguous ? nullptr : best);
    if (use && (!g.cam_valid || g.cam.sig != use->sig))
    {
        g.cam = *use;
        g.cam_valid = true;
        g.cam_source = CAM_DETECTED;
        g.cam_track_res = use->res;
        g.cam_track_off = use->offset;
        g.cam_track_t = use->transposed;
        g.cam_sig = use->sig;
        g.dirty = true;
    }
}
