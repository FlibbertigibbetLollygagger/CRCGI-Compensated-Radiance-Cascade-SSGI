// CRCGI Injector - D3D12 command-list state tracker.
//
// A mid-frame render_technique records ReShade's own pipeline, root signature, descriptor heaps, root arguments, render targets and
// viewports into the GAME's command list. On D3D10/11 ReShade restores the game's state itself; on D3D12 it does not. This tracker
// records what the game bound on each command list (from ReShade's add-on events) and binds exactly that again afterwards:
//   pipeline state, graphics root signature, every root parameter (descriptor tables, root constants, root CBV / SRV / UAV),
//   descriptor heaps (restored by ReShade's bind_descriptor_tables from the tables' own heaps), primitive topology, blend factor,
//   stencil reference, viewports, scissors, render targets.
// Before injecting, tracker_check() verifies that nothing needed for the restore is unknown (for example a root descriptor whose
// GPU address ReShade could not resolve, so no event was sent). If anything is missing the layer is refused with the reason,
// instead of risking a device removal.
#pragma once
#include "state.hpp"

static bool tracking_wanted(command_list *cl)
{
    return cl->get_device()->get_api() == device_api::d3d12 && g.d3d12_inject;
}

static RootSigState *sig_for(TrackState &t, shader_stage stages)
{
    const uint32_t s = static_cast<uint32_t>(stages);
    if (s & static_cast<uint32_t>(shader_stage::all_graphics))
        return &t.gfx;
    if (s & static_cast<uint32_t>(shader_stage::all_compute))
        return &t.cmp;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------------------------------
// Parameter classes of a root signature, as far as the restore is concerned.
enum ParamClass : uint8_t { PC_OTHER = 0, PC_TABLE, PC_ROOT_DESCRIPTOR, PC_CONSTANTS, PC_STATIC };

static uint8_t classify_param(const pipeline_layout_param &p)
{
    switch (p.type)
    {
    case pipeline_layout_param_type::descriptor_table:
    case pipeline_layout_param_type::descriptor_table_with_flags:
        return PC_TABLE;
    case pipeline_layout_param_type::push_constants:
        return PC_CONSTANTS;
    case pipeline_layout_param_type::push_descriptors:
        return p.push_descriptors.type == descriptor_type::sampler ? PC_STATIC : PC_ROOT_DESCRIPTOR;
    case pipeline_layout_param_type::push_descriptors_with_ranges:
    {
        const auto &t = p.descriptor_table;              // plain descriptor_range entries (no flags / static samplers)
        if (t.count == 0 || t.ranges == nullptr)
            return PC_STATIC;
        for (uint32_t r = 0; r < t.count; r++)
            if (t.ranges[r].type == descriptor_type::sampler)
                return PC_STATIC;
        return PC_ROOT_DESCRIPTOR;
    }
    case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
    {
        // ReShade appends the root signature's STATIC SAMPLERS as one extra parameter of this type (ranges of type sampler with
        // static_samplers set). They are part of the root signature and need no restore. A real root CBV / SRV / UAV (root signature
        // 1.1+) is a single range of a buffer type.
        const auto &t = p.descriptor_table_with_flags;
        if (t.count == 0 || t.ranges == nullptr)
            return PC_STATIC;
        for (uint32_t r = 0; r < t.count; r++)
            if (t.ranges[r].type == descriptor_type::sampler || t.ranges[r].static_samplers != nullptr)
                return PC_STATIC;
        return PC_ROOT_DESCRIPTOR;
    }
    default:
        return PC_OTHER;
    }
}

static void on_init_pipeline_layout(device *dev, uint32_t count, const pipeline_layout_param *params, pipeline_layout layout)
{
    if (dev->get_api() != device_api::d3d12 || params == nullptr)
        return;
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    std::vector<uint8_t> kinds(count);
    for (uint32_t i = 0; i < count; i++)
        kinds[i] = classify_param(params[i]);
    std::unique_lock<std::shared_mutex> lock(ds->pipe_mutex);
    ds->layouts[layout.handle] = std::move(kinds);
}

static void on_destroy_pipeline_layout(device *dev, pipeline_layout layout)
{
    DevState *ds = dev->get_private_data<DevState>();
    if (ds == nullptr)
        return;
    std::unique_lock<std::shared_mutex> lock(ds->pipe_mutex);
    ds->layouts.erase(layout.handle);
}

static void on_bind_descriptor_tables(command_list *cl, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                                      const descriptor_table *tables, uint32_t, const uint32_t *)
{
    if (t_injecting || !tracking_wanted(cl))
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    RootSigState *sig = sig_for(cs->trk, stages);
    if (sig == nullptr)
        return;
    if (count == 0)
    {
        // Set*RootSignature: a different root signature makes every root argument stale; the same one keeps them.
        if (sig->layout != layout)
            sig->reset(layout);
        return;
    }
    if (sig->layout != layout)
        sig->reset(layout);
    for (uint32_t i = 0; i < count && tables != nullptr; i++)
    {
        RootParamState &p = sig->at(first + i);
        p.kind = PK_TABLE;
        p.table = tables[i];
    }
}

static void on_push_constants(command_list *cl, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first, uint32_t count,
                              const void *values)
{
    if (t_injecting || !tracking_wanted(cl) || values == nullptr)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    RootSigState *sig = sig_for(cs->trk, stages);
    if (sig == nullptr)
        return;
    if (sig->layout != layout)
        sig->reset(layout);
    RootParamState &p = sig->at(param);
    p.kind = PK_CONSTANTS;
    if (p.values.size() < first + count)
    {
        p.values.resize(first + count, 0);
        p.set.resize(first + count, 0);
    }
    std::memcpy(p.values.data() + first, values, count * sizeof(uint32_t));
    std::fill(p.set.begin() + first, p.set.begin() + first + count, static_cast<uint8_t>(1));
}

static void on_push_descriptors(command_list *cl, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update &update)
{
    if (t_injecting || !tracking_wanted(cl))
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    RootSigState *sig = sig_for(cs->trk, stages);
    if (sig == nullptr)
        return;
    if (sig->layout != layout)
        sig->reset(layout);
    RootParamState &p = sig->at(param);
    p.kind = PK_UNSUPPORTED;
    if (update.count != 1 || update.descriptors == nullptr)
        return;
    switch (update.type)
    {
    case descriptor_type::constant_buffer:
    {
        const buffer_range &br = *static_cast<const buffer_range *>(update.descriptors);
        p.kind = PK_CBV;
        p.buf = br.buffer;
        p.offset = br.offset;
        break;
    }
    case descriptor_type::buffer_shader_resource_view:
        // D3D12 sends the root SRV's GPU virtual address itself as the descriptor (see d3d12_command_list.cpp).
        p.kind = PK_SRV_VA;
        p.va = *static_cast<const uint64_t *>(update.descriptors);
        break;
    case descriptor_type::buffer_unordered_access_view:
        p.kind = PK_UAV_VA;
        p.va = *static_cast<const uint64_t *>(update.descriptors);
        break;
    default:
        break;
    }
}

static void on_bind_pipeline_states(command_list *cl, uint32_t count, const dynamic_state *states, const uint32_t *values)
{
    if (t_injecting || !tracking_wanted(cl) || states == nullptr || values == nullptr)
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    TrackState &t = cs->trk;
    for (uint32_t i = 0; i < count; i++)
    {
        switch (states[i])
        {
        case dynamic_state::primitive_topology: t.topo = values[i]; t.topo_set = true; break;
        case dynamic_state::blend_constant: t.blend = values[i]; t.blend_set = true; break;
        case dynamic_state::front_stencil_reference_value: t.stencil_front = values[i]; t.stencil_set = true; break;
        case dynamic_state::back_stencil_reference_value: t.stencil_back = values[i]; t.stencil_set = true; break;
        default: break;
        }
    }
}

static void on_bind_viewports(command_list *cl, uint32_t first, uint32_t count, const viewport *vps)
{
    if (t_injecting || !tracking_wanted(cl))
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    std::vector<viewport> &v = cs->trk.viewports;
    if (first == 0)
        v.clear();
    if (v.size() < first + count)
        v.resize(first + count);
    for (uint32_t i = 0; i < count && vps != nullptr; i++)
        v[first + i] = vps[i];
}

static void on_bind_scissor_rects(command_list *cl, uint32_t first, uint32_t count, const rect *rects)
{
    if (t_injecting || !tracking_wanted(cl))
        return;
    CmdState *cs = cl->get_private_data<CmdState>();
    if (cs == nullptr)
        return;
    std::vector<rect> &v = cs->trk.scissors;
    if (first == 0)
        v.clear();
    if (v.size() < first + count)
        v.resize(first + count);
    for (uint32_t i = 0; i < count && rects != nullptr; i++)
        v[first + i] = rects[i];
}

// ---------------------------------------------------------------------------------------------------------------------
// Check and restore
// ---------------------------------------------------------------------------------------------------------------------
static bool param_is_root_descriptor(uint8_t c) { return c == PC_ROOT_DESCRIPTOR; }
static bool param_is_table(uint8_t c) { return c == PC_TABLE; }

// Everything the restore needs is known? `why` says what is missing.
static bool sig_complete(const RootSigState &sig, DevState &ds, const char *what, bool &has_table, std::string &why)
{
    std::vector<uint8_t> kinds;
    {
        std::shared_lock<std::shared_mutex> lock(ds.pipe_mutex);
        auto it = ds.layouts.find(sig.layout.handle);
        if (it == ds.layouts.end())
        {
            why = std::string("the ") + what + " root signature was created before the add-on saw it (its parameters are unknown)";
            return false;
        }
        kinds = it->second;
    }
    has_table = false;
    for (size_t i = 0; i < kinds.size(); i++)
    {
        const RootParamState *p = i < sig.params.size() ? &sig.params[i] : nullptr;
        if (p != nullptr && p->kind == PK_UNSUPPORTED)
        {
            why = std::string(what) + " root parameter " + std::to_string(i) + " has a type the tracker cannot restore";
            return false;
        }
        if (param_is_root_descriptor(kinds[i]) && (p == nullptr || p->kind == PK_NONE))
        {
            why = std::string(what) + " root descriptor " + std::to_string(i) + " was not seen (ReShade could not resolve its GPU address)";
            return false;
        }
        if (param_is_table(kinds[i]) && p != nullptr && p->kind == PK_TABLE)
            has_table = true;
    }
    return true;
}

static bool tracker_check(const CmdState &cs, DevState &ds, std::string &why)
{
    const TrackState &t = cs.trk;
    if (t.pso.handle == 0) { why = "no pipeline state seen on this command list"; return false; }
    if (t.gfx.layout.handle == 0) { why = "no graphics root signature seen on this command list"; return false; }
    if (t.viewports.empty()) { why = "no viewport seen on this command list"; return false; }
    bool gfx_table = false;
    if (!sig_complete(t.gfx, ds, "graphics", gfx_table, why))
        return false;
    // The descriptor heaps have no add-on event. ReShade puts the game's heaps back when a table from them is bound, so at least one
    // table must be known (a draw that samples anything has one).
    if (!gfx_table) { why = "no descriptor table seen for the graphics root signature (the game's descriptor heaps could not be restored)"; return false; }
    // ReShade's render_technique pushes its constants with shader_stage::all, which also replaces the COMPUTE root signature (and its
    // mipmap generation binds a compute pipeline). If the game has a compute root signature bound on this list, it must be restorable.
    if (t.cmp.layout.handle != 0)
    {
        bool cmp_table = false;
        if (!sig_complete(t.cmp, ds, "compute", cmp_table, why))
            return false;
    }
    why.clear();
    return true;
}

static void apply_sig(command_list *cl, shader_stage stages, const RootSigState &sig)
{
    if (sig.layout.handle == 0)
        return;
    // ReShade's bind_descriptor_tables / push_constants / push_descriptors set the root signature first when it differs from the one
    // bound (ReShade's own, after the effect), and bind_descriptor_tables switches to the descriptor heaps the game's tables come from.
    // The tables are bound first, so the heaps are final before any other argument is set. Only a root signature without any
    // table / constant / CBV argument needs the explicit count = 0 call (which would otherwise re-set the heaps).
    bool any_api = false;
    for (const RootParamState &p : sig.params)
        any_api |= p.kind == PK_TABLE || p.kind == PK_CONSTANTS || p.kind == PK_CBV;
    if (!any_api)
        cl->bind_descriptor_tables(stages, sig.layout, 0, 0, nullptr);
    for (uint32_t i = 0; i < sig.params.size(); i++)
        if (sig.params[i].kind == PK_TABLE)
            cl->bind_descriptor_tables(stages, sig.layout, i, 1, &sig.params[i].table);
    auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(static_cast<uintptr_t>(cl->get_native()));
    const bool graphics = (static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::all_graphics)) != 0;
    for (uint32_t i = 0; i < sig.params.size(); i++)
    {
        const RootParamState &p = sig.params[i];
        switch (p.kind)
        {
        case PK_CONSTANTS:
            for (uint32_t a = 0; a < p.values.size();)
            {
                if (!p.set[a]) { a++; continue; }
                uint32_t b = a;
                while (b < p.values.size() && p.set[b])
                    b++;
                cl->push_constants(stages, sig.layout, i, a, b - a, p.values.data() + a);
                a = b;
            }
            break;
        case PK_CBV:
        {
            buffer_range br;
            br.buffer = p.buf;
            br.offset = p.offset;
            br.size = UINT64_MAX;
            descriptor_table_update u;
            u.count = 1;
            u.type = descriptor_type::constant_buffer;
            u.descriptors = &br;
            cl->push_descriptors(stages, sig.layout, i, u);
            break;
        }
        case PK_SRV_VA:
            if (native != nullptr)
            {
                if (graphics) native->SetGraphicsRootShaderResourceView(i, p.va);
                else native->SetComputeRootShaderResourceView(i, p.va);
            }
            break;
        case PK_UAV_VA:
            if (native != nullptr)
            {
                if (graphics) native->SetGraphicsRootUnorderedAccessView(i, p.va);
                else native->SetComputeRootUnorderedAccessView(i, p.va);
            }
            break;
        default:
            break;
        }
    }
}

// Re-applies the game's state after the add-on rendered into its command list (graphics and, if the game had one bound, compute: ReShade
// replaces both root signatures). tracker_check() has verified that both are complete.
static void tracker_apply(command_list *cl, const CmdState &cs, DevState &ds)
{
    const TrackState &t = cs.trk;
    if (t.pso.handle != 0)
        cl->bind_pipeline(pipeline_stage::all, t.pso);
    if (t.cmp.layout.handle != 0)
        apply_sig(cl, shader_stage::all_compute, t.cmp);
    apply_sig(cl, shader_stage::all_graphics, t.gfx);
    (void)ds;
    dynamic_state st[4];
    uint32_t val[4];
    uint32_t n = 0;
    if (t.topo_set) { st[n] = dynamic_state::primitive_topology; val[n++] = t.topo; }
    if (t.blend_set) { st[n] = dynamic_state::blend_constant; val[n++] = t.blend; }
    if (t.stencil_set)
    {
        // A front/back pair needs ID3D12GraphicsCommandList8 in ReShade's D3D12 back end (without it the pair is dropped); the usual
        // OMSetStencilRef case (both equal) is sent as one value, which maps to OMSetStencilRef.
        st[n] = dynamic_state::front_stencil_reference_value; val[n++] = t.stencil_front;
        if (t.stencil_back != t.stencil_front)
        {
            st[n] = dynamic_state::back_stencil_reference_value; val[n++] = t.stencil_back;
        }
    }
    if (n != 0)
        cl->bind_pipeline_states(n, st, val);
    if (!t.viewports.empty())
        cl->bind_viewports(0, static_cast<uint32_t>(t.viewports.size()), t.viewports.data());
    if (!t.scissors.empty())
        cl->bind_scissor_rects(0, static_cast<uint32_t>(t.scissors.size()), t.scissors.data());
    cl->bind_render_targets_and_depth_stencil(cs.rt_count, cs.rtv, cs.dsv_view);
}
