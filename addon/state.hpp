// CRCGI Injector - shared state, helpers, settings. Header-only, included once by crcgi_injector.cpp.
#pragma once

// Include order as in ReShade's own add-on examples: Windows, then Dear ImGui (the copy bundled with the SAME ReShade revision),
// then reshade.hpp. reshade_overlay.hpp checks the ImGui version and that ImTextureID is 64 bits.
#include <Windows.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <imgui.h>
#include <reshade.hpp>

// D3D12 in-frame injection uses the add-on's OWN command-list state tracker (tracker.hpp, its own private-data GUID), which also
// records root constants and root descriptors. ReShade's examples/utils/state_tracking helper is never built in: its GUID is reused by
// ReShade Effect Shader Toggler for a different struct (both crash when loaded together), and it does not restore root
// constants / root descriptors.

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace reshade::api;
using reshade::addon_event;

extern "C" IMAGE_DOS_HEADER __ImageBase;   // this add-on's own module (linker-provided)

#ifndef CRCGI_RESHADE_REVISION
#define CRCGI_RESHADE_REVISION "unknown"   // set by CMake from the ReShade checkout used for the build
#endif

// Private data attached to ReShade objects is looked up by __uuidof(T). MSVC and clang read __declspec(uuid); MinGW GCC needs
// an explicit __CRT_UUID_DECL (used only for the cross-compile syntax check; the supported build is MSVC).
#if defined(__MINGW32__) && !defined(_MSC_VER) && !defined(__clang__)
#define CRCGI_UUID(str)
#define CRCGI_MINGW_UUID(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) __CRT_UUID_DECL(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)
#else
#define CRCGI_UUID(str) __declspec(uuid(str))
#define CRCGI_MINGW_UUID(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------------------------------------------------
enum Level : int
{
    LEVEL_A = 0,   // end of frame: plain ReShade, nothing is touched
    LEVEL_B = 1,   // inject inside the frame + exact depth + camera data
    LEVEL_C = 2,   // level B + the game's G-buffer feeds (normals, albedo, motion vectors, HDR scene light)
};

enum Role : int
{
    ROLE_NONE = 0,
    ROLE_INJECT,
    ROLE_DEPTH,
    ROLE_NORMALS,      // ROLE_NORMALS + feed index for every feed
    ROLE_ALBEDO,
    ROLE_MOTION,
    ROLE_MATERIAL,
    ROLE_HDR,
    ROLE_COUNT
};

static const char *const ROLE_LABEL[ROLE_COUNT] = { "-", "Render CRCGI here", "Depth", "Normals", "Albedo", "Motion vectors", "Material", "HDR scene light" };
static const char ROLE_ITEMS[] = "-\0Render CRCGI here\0Depth\0Normals\0Albedo\0Motion vectors\0Material\0HDR scene light\0";

// The feeds the shader can receive. NORMAL is the semantic the shader already declares; the others come with the CRCGI shader.
enum Feed : int { FEED_NORMALS = 0, FEED_ALBEDO, FEED_MOTION, FEED_MATERIAL, FEED_HDR, FEED_COUNT };
// The material feed (metalness / specular) is implemented but not offered in the overlay: the shader's matching
// switch is hidden too. Set to true to expose it again.
static constexpr bool UI_MATERIAL = false;
static const char *const FEED_SEMANTIC[FEED_COUNT] = { "NORMAL", "CRCGI_ALBEDO", "CRCGI_MOTION", "CRCGI_MATERIAL", "CRCGI_HDRSCENE" };
static const char *const FEED_LABEL[FEED_COUNT] = { "Normals", "Albedo", "Motion vectors", "Material (reflectivity)", "HDR scene light" };
// The shader's switch for each feed (category Game Integration).
static const char *const FEED_SWITCH[FEED_COUNT] = { "UseNormalTexture", "Integration_Albedo", "Integration_Motion", "Integration_Material", "Integration_HDRScene" };
static const char *const FEED_SWITCH_LABEL[FEED_COUNT] = { "Use Game Normals If Provided", "Use Game Albedo If Provided", "Use Game Motion Vectors If Provided",
                                                           "Use Game Material (reflectivity) If Provided", "Use Game HDR Scene Light If Provided" };

// What a layer probably is, from how it is drawn (Layers tab, auto-setup). A heuristic, shown as a hint.
enum LayerKind : int { LK_UNKNOWN = 0, LK_GBUFFER, LK_OPAQUE, LK_LIGHTING, LK_TRANSPARENT, LK_POST, LK_UI, LK_COUNT };
static const char *const LK_LABEL[LK_COUNT] = { "?", "G-buffer", "Geometry", "Lighting", "Transparent", "Post-process", "UI / overlay" };

// Remappable key actions (Keys tab). Repeating actions fire again while the key is held.
enum KeyAction : int
{
    KA_TOGGLE = 0, KA_LEVEL_DOWN, KA_LEVEL_UP, KA_LAYER_PREV, KA_LAYER_NEXT, KA_INJECT_HERE, KA_ROLE_PREV, KA_ROLE_NEXT,
    KA_AUTO_SETUP, KA_AUTO_FEEDS, KA_RECORD, KA_COUNT
};
static const char *const KA_LABEL[KA_COUNT] = {
    "Integration on / off", "Level down", "Level up", "Previous layer", "Next layer", "Render CRCGI at the selected layer",
    "Previous role of the selected layer", "Next role of the selected layer", "Auto-setup (injection layer + all feeds)",
    "Auto-detect feeds only (replace)", "Record the layer list again" };
static const bool KA_REPEAT[KA_COUNT] = { false, false, false, true, true, false, true, true, false, false, false };
struct KeyBind
{
    uint32_t key = 0;               // virtual key code, 0 = unbound
    bool ctrl = false, shift = false, alt = false;
};
static const KeyBind KA_DEFAULT[KA_COUNT] = {
    { VK_NUMPAD0 }, { VK_NUMPAD7 }, { VK_NUMPAD9 }, { VK_NUMPAD8 }, { VK_NUMPAD2 }, { VK_NUMPAD5 }, { VK_NUMPAD4 }, { VK_NUMPAD6 },
    { VK_NUMPAD1 }, { VK_NUMPAD3 }, { VK_DECIMAL } };

// Layer list sort orders.
enum SortMode : int { SORT_FRAME = 0, SORT_RES_FRAME, SORT_KIND_FRAME, SORT_DRAWS, SORT_FORMAT_FRAME, SORT_ROLE_FRAME, SORT_COUNT };
static const char SORT_ITEMS[] = "Frame order\0Resolution, then frame order\0Kind, then frame order\0Most draws first\0Format, then frame order\0Assigned roles first\0";

// Upscalers (DLSS / FSR / XeSS) render the 3D scene below the output resolution: the G-buffer is then smaller than the back buffer.
enum UpscaleMode : int { UP_OFF = 0, UP_AUTO, UP_QUALITY, UP_BALANCED, UP_PERFORMANCE, UP_ULTRA, UP_CUSTOM, UP_COUNT };
static const char UPSCALE_ITEMS[] = "Off (native resolution only)\0Auto-detect the render resolution\0Quality (66.7%)\0Balanced (58%)\0Performance (50%)\0Ultra Performance (33.3%)\0Custom percentage\0";
static float upscale_fraction(int mode, float custom_pct)
{
    switch (mode)
    {
    case UP_QUALITY: return 1.0f / 1.5f;
    case UP_BALANCED: return 0.58f;
    case UP_PERFORMANCE: return 0.5f;
    case UP_ULTRA: return 1.0f / 3.0f;
    case UP_CUSTOM: return std::clamp(custom_pct, 20.0f, 100.0f) / 100.0f;
    default: return 1.0f;
    }
}

// Where Auto-setup puts the injection layer.
enum InjectPolicy : int { IP_BEFORE_UI = 0, IP_BEFORE_POST, IP_BEFORE_TRANSPARENT, IP_COUNT };
static const char INJECT_POLICY_ITEMS[] = "Before the UI (safest, after all post-processing)\0Before post-processing (first full-screen pass after the scene)\0Before transparent effects (fog, particles, glass)\0";

// Where the camera values written to the shader come from.
enum CamSource : int { CAM_NONE = 0, CAM_DETECTED, CAM_PICKED };

// Version of the settings file format (crcgi_injector.ini next to the game's executable).
static const int SETTINGS_VERSION = 1;

// A game texture, identified across runs: its description plus its index among the live textures with the same description at
// the time it was created (games create their render targets in a fixed order).
struct TexSig
{
    uint32_t fmt = 0, w = 0, h = 0, samples = 1, usage = 0, ordinal = 0;
    bool valid() const { return w != 0 && h != 0; }
    bool same_desc(const TexSig &o) const { return fmt == o.fmt && w == o.w && h == o.h && samples == o.samples && usage == o.usage; }
    bool operator==(const TexSig &o) const { return same_desc(o) && ordinal == o.ordinal; }
};

enum FeedSource : int { SRC_LAYER = 0, SRC_TEXTURE = 1 };

struct LayerRef
{
    uint64_t key = 0;
    int slot = 0;
    bool valid() const { return key != 0; }
    bool operator==(const LayerRef &o) const { return key == o.key && slot == o.slot; }
};

struct FeedConfig
{
    int source = SRC_LAYER;    // where the feed is taken from: a layer (binding interval) or a texture picked in the profiler
    LayerRef ref;              // SRC_LAYER
    TexSig tex;                // SRC_TEXTURE
    bool enabled = false;      // the add-on provides this feed (independent of the shader's own switch)
    bool sync = false;         // also set the shader's own switch to match (on: the switch is on exactly while the feed is delivered)
    int mode = 0;              // normals: 0 RGB stored 0..1 (*2-1), 1 octahedral, 2 RGB signed as stored
    int space = 0;             // normals: 0 view space, 1 world / any space (the shader auto-aligns it to the camera)
    int units = 0;             // motion: 0 UV, 1 pixels, 2 NDC
    bool flip_y = false;
    bool invert = false;
    int channel = 0;           // material: 0 R, 1 G, 2 B, 3 A
    int meaning = 0;           // material: 0 metalness, 1 specular / reflectance, 2 roughness, 3 smoothness
    bool assigned() const { return source == SRC_TEXTURE ? tex.valid() : ref.valid(); }
};

struct RtSlot
{
    resource res = { 0 };
    uint32_t w = 0, h = 0, samples = 1;
    format fmt = format::unknown;        // resource format (may be typeless)
    format view_fmt = format::unknown;   // format of the view the game bound (tells sRGB from linear for typeless resources)
};

// A "layer" is ONE RENDER-TARGET BINDING INTERVAL of one command list: from the first draw after the game binds a set of render
// targets until it binds another set (or the frame ends). It is not necessarily a whole render pass in the game's sense: a game can
// keep a target bound across several logically different draws. `ps_changes` counts pixel-shader changes inside the interval so the
// UI can show when one row mixes several operations.
struct Pass
{
    uint64_t key = 0;
    uint32_t order = 0, draws = 0, ps = 0, vs = 0, rt_count = 0;
    uint32_t ps_changes = 0;
    uint64_t cl = 0;                     // command list that recorded it
    RtSlot rt[8];
    resource depth = { 0 };
    bool to_back_buffer = false;
    uint32_t blend_draws = 0;            // draws with blending enabled on target 0 (where the pipeline's blend state is known)
    uint32_t zwrite_draws = 0;           // draws that write depth (where known)
};

// ---------------------------------------------------------------------------------------------------------------------
// Command-list state the game has bound (explicit-state APIs), re-applied after a mid-frame effect render. Root parameters are kept per
// root signature; a change of root signature resets them (as D3D12 does), a redundant set of the same one keeps them.
// ---------------------------------------------------------------------------------------------------------------------
enum ParamKind : uint8_t { PK_NONE = 0, PK_TABLE, PK_CONSTANTS, PK_CBV, PK_SRV_VA, PK_UAV_VA, PK_UNSUPPORTED };

struct RootParamState
{
    uint8_t kind = PK_NONE;
    descriptor_table table = { 0 };
    resource buf = { 0 };
    uint64_t offset = 0;
    uint64_t va = 0;
    std::vector<uint32_t> values;
    std::vector<uint8_t> set;      // which 32-bit constants were written
};

struct RootSigState
{
    pipeline_layout layout = { 0 };
    std::vector<RootParamState> params;
    void reset(pipeline_layout l) { layout = l; params.clear(); }
    RootParamState &at(uint32_t i) { if (i >= params.size()) params.resize(i + 1); return params[i]; }
};

struct TrackState
{
    RootSigState gfx, cmp;
    pipeline pso = { 0 };
    bool topo_set = false, blend_set = false, stencil_set = false;
    uint32_t topo = 0, blend = 0, stencil_front = 0, stencil_back = 0;
    std::vector<viewport> viewports;
    std::vector<rect> scissors;
    void clear() { *this = TrackState(); }
};

struct CRCGI_UUID("FB2E1E5D-B900-4C39-9EEE-3B1AD6F285FD") CmdState
{
    uint64_t frame = ~0ull;
    bool pass_open = false;
    int pass_index = -1;
    uint64_t pass_key = 0;
    uint32_t draws = 0;
    uint32_t ps = 0, vs = 0;
    uint32_t pass_ps = 0;              // pixel shader of the interval's first draw
    uint32_t ps_changes = 0;
    bool blend = false, zwrite = false;  // output-merger state of the bound pipeline(s), where known
    uint32_t blend_draws = 0, zwrite_draws = 0;
    uint32_t rt_count = 0;
    resource_view rtv[8];          // views bound by the game (resolved lazily at the first draw of the pass)
    resource_view dsv_view = { 0 };
    RtSlot rt[8];
    resource dsv = { 0 };
    resource last_dsv = { 0 };     // last depth buffer bound this frame (marker passes often bind none)
    uint64_t last_dsv_frame = ~0ull;
    // Explicit-state APIs (D3D12): the state each resource was last transitioned to by the game ON THIS COMMAND LIST.
    // Unknown = no barrier seen since the list was reset. Never guessed.
    std::unordered_map<uint64_t, resource_usage> states;
    // Layer identity: occurrence of each binding signature within this recording (reset with the list / every frame on D3D10/11),
    // so a layer keeps its key no matter how many other lists record in parallel.
    std::unordered_map<uint64_t, uint32_t> occ;
    uint64_t occ_frame = ~0ull;
    bool in_render_pass = false;   // between begin_render_pass and end_render_pass: no copies, no injection
    TrackState trk;
};
CRCGI_MINGW_UUID(CmdState, 0xfb2e1e5d, 0xb900, 0x4c39, 0x9e, 0xee, 0x3b, 0x1a, 0xd6, 0xf2, 0x85, 0xfd)

struct PipeInfo
{
    uint32_t ps = 0, vs = 0;
    bool has_blend = false, blend = false;   // blend state known / blending enabled on target 0
    bool has_ds = false, zwrite = false;     // depth-stencil state known / depth writes on
};
struct ViewPair { resource_view linear = { 0 }, srgb = { 0 }; uint64_t owner = 0; };

struct FeedTex
{
    resource res = { 0 };
    resource_view srv = { 0 }, srv_srgb = { 0 };
    uint32_t w = 0, h = 0;
    format fmt = format::unknown;
    bool srgb_source = false;     // the game wrote it through an sRGB view
    uint64_t frame = ~0ull;       // frame of the last successful capture
    uint64_t cl = 0;              // command list it was captured on
    uint64_t bound_frame = ~0ull; // frame in which the injection last handed it to the shader
    std::string status;           // why the last capture or binding did not happen (empty = fine)
};

struct MapInfo { void *ptr = nullptr; uint64_t size = 0; uint64_t offset = 0; };

struct DepthSrv { resource_view view = { 0 }; std::string reason; };

struct CRCGI_UUID("B3203B0F-C3C2-4D9D-BDC2-AB1979966DC7") DevState
{
    std::shared_mutex pipe_mutex;
    std::unordered_map<uint64_t, PipeInfo> pipes;
    std::unordered_map<uint64_t, std::vector<uint8_t>> layouts;   // pipeline layout -> parameter types (pipeline_layout_param_type)
    std::mutex view_mutex;
    std::unordered_map<uint64_t, ViewPair> rtv_cache;
    std::unordered_map<uint64_t, DepthSrv> depth_srv;
    std::mutex feed_mutex;
    FeedTex feed[FEED_COUNT];
    FeedTex preview;
    std::mutex map_mutex;
    std::unordered_map<uint64_t, MapInfo> mapped;
};
CRCGI_MINGW_UUID(DevState, 0xb3203b0f, 0xc3c2, 0x4d9d, 0xbd, 0xc2, 0xab, 0x19, 0x79, 0x96, 0x6d, 0xc7)

struct TechInfo { effect_technique handle = { 0 }; std::string key; };

struct CamCandidate
{
    float fov = 0, aspect = 0, nearp = 0, farp = 0;
    bool reversed = false;
    uint32_t hits = 0;
    uint64_t res = 0, offset = 0;
    bool transposed = false;
    uint64_t last_frame = 0;
    int64_t sig = 0;              // quantised identity, stable between runs
};

// One layer of the catalog: kept across frames, so the list is stable and a layer that is not drawn in one frame does not vanish.
struct LayerInfo
{
    Pass p;
    uint64_t first_seen = 0, last_seen = 0;
    uint32_t seen = 0;
    float pos = 0.0f;              // average position in the frame, 0 = first binding, 1 = last (sort order)
};

struct Globals
{
    std::mutex mtx;
    effect_runtime *runtime = nullptr;
    std::filesystem::path ini_path;
    std::string settings_error;                 // last settings read/write problem (shown in the UI, never fatal)

    // --- configuration (saved per game) ---
    bool master = true;                         // integration on/off at run time (Numpad 0)
    int level = LEVEL_A;
    bool fallback_end = true;                   // marker not drawn this frame: render at the end of the frame instead
    bool depth_live = true;
    bool only_useful = true;                    // layer list: hide targets that cannot be used for anything
    bool cross_list_feeds = true;               // accept feeds recorded on another command list (D3D12 records G-buffer and lighting apart)
    bool d3d12_inject = true;                   // D3D12: in-frame injection with the add-on's own state tracker (experimental)
    bool verbose_log = false;
    LayerRef role_ref[ROLE_COUNT];              // inject / depth (normals/albedo/motion live in feeds)
    FeedConfig feed[FEED_COUNT];
    std::vector<std::string> techniques;        // managed techniques "effect|technique"
    bool cam_auto = true;                       // pick the camera automatically
    bool cam_apply = true;                      // write FOV to the shader
    int64_t cam_sig = 0;                        // chosen camera, quantised
    bool dirty = false;

    // --- run time ---
    device_api api = device_api::d3d11;
    uint32_t bb_w = 0, bb_h = 0;
    format bb_fmt = format::unknown;
    resource bb_res = { 0 };
    uint64_t frame = 0;
    bool in_eof = false;
    uint64_t injected_frame = ~0ull;
    bool injected_float = false;                // diagnostic only: the injection target has a floating-point format
    std::string status;
    std::string depth_status;
    std::vector<effect_technique> suppressed;   // techniques switched off for the end-of-frame render only (restored right after)
    uint32_t eof_suppressed_frames = 0;

    std::vector<Pass> cur, last;
    std::unordered_set<uint64_t> cur_lists;     // command lists that recorded passes this frame
    uint32_t last_list_count = 0;

    // layer catalog (stable list for the overlay)
    std::unordered_map<uint64_t, LayerInfo> catalog;
    std::vector<uint64_t> cat_order;            // display order, rebuilt only after a recording or on request
    int rec_left = 0;                           // frames left in the current recording
    int rec_total = 0;
    bool cat_add_new = true;                    // after a recording, keep adding layers that were not seen yet (at the end)
    int sort_mode = SORT_FRAME;                 // layer list order (saved)
    bool scroll_to_sel = false;                 // the selection was moved by a key: scroll the list to it

    // upscaler (DLSS / FSR / XeSS): textures at the render resolution count as screen-sized
    int upscale_mode = UP_AUTO;
    float upscale_pct = 66.7f;
    uint32_t rr_w = 0, rr_h = 0;                // render resolution in use (detected or from the preset), 0 = native only

    // auto-setup and auto re-detect
    int inject_policy = IP_BEFORE_UI;
    bool auto_redetect = true;                  // re-run the feed detection when an assigned texture is destroyed or goes stale
    uint64_t feed_lost_frame[FEED_COUNT] = {};  // frame in which a feed's texture was destroyed (0 = not lost)
    uint64_t redetect_frame = 0;                // last automatic re-detection
    std::string auto_report;                    // what the last auto-setup did
    int geom_source = 1;                        // shader's Geometry Source while game normals are delivered (0 rebuild, 1 game where valid, 2 game only)

    // keys
    KeyBind keys[KA_COUNT];
    int key_capture = -1;                       // action waiting for a new key in the Keys tab (-1 = none)
    uint64_t key_capture_frame = 0;             // last frame the Keys tab was drawn while waiting (a closed overlay cancels the wait)
    uint64_t typing_frame = 0;                  // last frame a text field of the overlay had the keyboard (no key actions then)
    uint64_t key_down_since[KA_COUNT] = {};     // ms tick when the key went down (0 = up)
    uint64_t key_next_repeat[KA_COUNT] = {};
    int repeat_delay_ms = 350, repeat_rate_ms = 55;

    // feeds taken from a texture: the live resource that matches each feed's TexSig (0 = not created / not matched yet)
    resource feed_res[FEED_COUNT] = {};
    resource_view eof_bound[FEED_COUNT] = {};   // what the end-of-frame render last had bound per feed semantic
    bool eof_bound_valid[FEED_COUNT] = {};

    // statistics (Debug tab)
    uint64_t n_injections = 0, n_inject_refused = 0, n_captures_interval = 0, n_captures_barrier = 0, n_eof_feed_frames = 0;
    std::string tracker_report;                 // last D3D12 tracker check at the injection layer

    int sel_pass = -1;                          // selection in the list (index into `last`)
    int sel_slot = 0;
    uint64_t preview_key = 0;
    int preview_slot = 0;
    uint64_t preview_frame = 0;                 // frame in which the overlay last asked for the preview

    std::vector<TechInfo> techs;
    std::unordered_map<std::string, effect_uniform_variable> uniforms;
    std::unordered_map<std::string, double> uniform_last;

    // camera probe
    std::vector<CamCandidate> cams;
    CamCandidate cam;                           // the camera in use
    bool cam_valid = false;
    CamSource cam_source = CAM_NONE;
    bool cam_ambiguous = false;                 // several plausible cameras: not chosen automatically
    uint64_t cam_track_res = 0, cam_track_off = 0;
    bool cam_track_t = false;
    uint64_t cam_budget_frame = ~0ull;
    uint64_t cam_budget_bytes = 0;

    // D3D12 deferred destruction: resources the add-on created are destroyed only after the GPU can no longer use them
    struct Grave { resource res = { 0 }; resource_view v1 = { 0 }, v2 = { 0 }; uint64_t frame = 0; };
    std::vector<Grave> graveyard;
};

static Globals g;
static thread_local bool t_injecting = false;   // set while the add-on itself renders (its own nested events are ignored)
static std::mutex g_inject_mutex;               // one injection at a time (D3D12 lists can be recorded on several threads)
// ReShade's effect runtime is not safe for two renders at once. On D3D12 a game worker thread can inject while the present thread runs
// ReShade's end-of-frame render: that render holds this mutex (reshade_begin_effects .. reshade_finish_effects), an injection only
// try-locks it and is skipped for that frame when it is busy (the effect then renders at the end of the frame as usual).
static std::mutex g_runtime_render_mutex;
static std::atomic<bool> g_eof_holding{ false };
static std::string g_conflict;                  // name of a loaded add-on that also renders effects mid-frame (empty = none)

// Writes to ReShade.log (next to the game executable), so a failure report says what the add-on was doing. ReShade prefixes each
// line with the add-on's name itself.
static void log_msg(reshade::log::level lv, const std::string &msg)
{
    reshade::log::message(lv, msg.c_str());
}

// Logs a message once per distinct text (status lines can repeat every frame).
static void log_once(reshade::log::level lv, const std::string &msg)
{
    static std::mutex m;
    static std::unordered_set<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(m);
        if (seen.size() > 256 || !seen.insert(msg).second)
            return;
    }
    log_msg(lv, msg);
}

// Other add-ons that move effects into the frame themselves. Two add-ons managing the same technique fight every frame (both
// switch it on/off and render it mid-frame), so the injector stands down when one is loaded. Found by the NAME every ReShade add-on
// exports, so a renamed file is still recognized.
static std::string detect_conflicting_addon()
{
    static const char *const KNOWN[] = { "Reshade Effect Shader Toggler", "ReShade Effect Shader Toggler", "Shader Toggler" };
    std::string found;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return found;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32FirstW(snap, &me); ok && found.empty(); ok = Module32NextW(snap, &me))
    {
        const auto name = reinterpret_cast<const char *const *>(GetProcAddress(me.hModule, "NAME"));
        if (name == nullptr || *name == nullptr || me.hModule == reinterpret_cast<HMODULE>(&__ImageBase))
            continue;
        for (const char *k : KNOWN)
            if (std::strstr(*name, k) != nullptr)
            {
                found = *name;
                break;
            }
    }
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t crc32(const void *data, size_t size)
{
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
    });
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint64_t fnv64(const void *data, size_t size, uint64_t h = 1469598103934665603ull)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; i++)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static std::string hex16(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof(b), "%016llX", static_cast<unsigned long long>(v));
    return b;
}

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

template <typename T>
static bool contains(const std::vector<T> &v, const T &x) { return std::find(v.begin(), v.end(), x) != v.end(); }

static bool has_usage(resource_usage set, resource_usage bit)
{
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(bit)) == static_cast<uint32_t>(bit);
}

// ImGui texture id from a ReShade view handle, whichever way the bundled ImGui defines ImTextureID (integer since 1.91.4, a
// pointer before). reshade_overlay.hpp already asserts that it is 64 bits wide.
// (A template, so the branch that does not apply is never instantiated: on 32-bit builds ImTextureID is a 64-bit integer while
// uintptr_t is 32 bits, and the pointer cast would not compile.)
template <typename T = ImTextureID>
static T to_texture_id(uint64_t handle)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(static_cast<uintptr_t>(handle));
    else
        return static_cast<T>(handle);
}

static const char *fmt_name(format f)
{
    switch (f)
    {
    case format::r8g8b8a8_unorm: return "RGBA8";
    case format::r8g8b8a8_unorm_srgb: return "RGBA8 sRGB";
    case format::r8g8b8a8_typeless: return "RGBA8 typeless";
    case format::b8g8r8a8_unorm: return "BGRA8";
    case format::b8g8r8a8_unorm_srgb: return "BGRA8 sRGB";
    case format::b8g8r8a8_typeless: return "BGRA8 typeless";
    case format::r10g10b10a2_unorm: return "RGB10A2";
    case format::r11g11b10_float: return "R11G11B10F";
    case format::r16g16_float: return "RG16F";
    case format::r16g16_unorm: return "RG16";
    case format::r16g16b16a16_float: return "RGBA16F";
    case format::r16g16b16a16_unorm: return "RGBA16";
    case format::r32g32_float: return "RG32F";
    case format::r32g32b32a32_float: return "RGBA32F";
    case format::r8g8_unorm: return "RG8";
    case format::r8_unorm: return "R8";
    case format::r16_float: return "R16F";
    case format::r32_float: return "R32F";
    default: break;
    }
    static thread_local char buf[24];
    std::snprintf(buf, sizeof(buf), "fmt %u", static_cast<unsigned>(f));
    return buf;
}

static bool fmt_is_float(format f)
{
    switch (f)
    {
    case format::r11g11b10_float: case format::r16g16b16a16_float: case format::r32g32b32a32_float:
    case format::r16g16_float: case format::r32g32_float: return true;
    default: return false;
    }
}

static bool fmt_is_srgb(format f) { return f != format::unknown && format_to_default_typed(f, 1) == f && format_to_default_typed(f, 0) != f; }

static bool same_format_family(format a, format b) { return format_to_typeless(a) == format_to_typeless(b); }

// Screen-sized: the back buffer's size, or (with an upscaler) the render resolution, or with Auto any size of the screen's aspect
// between a third and all of it. Call with g.mtx held or from the render thread (plain reads of a few integers).
static bool screen_sized(uint32_t w, uint32_t h, bool allow_render_res = true)
{
    if (g.bb_w == 0 || w == 0 || h == 0)
        return false;
    if (w == g.bb_w && h == g.bb_h)
        return true;
    if (!allow_render_res || g.upscale_mode == UP_OFF)
        return false;
    if (g.rr_w != 0 && (w == g.rr_w || w + 1 == g.rr_w || w == g.rr_w + 1) && (h == g.rr_h || h + 1 == g.rr_h || h == g.rr_h + 1))
        return true;
    if (g.upscale_mode == UP_AUTO)
    {
        const double a = static_cast<double>(w) / h, b = static_cast<double>(g.bb_w) / g.bb_h;
        return std::fabs(a - b) < 0.01 * b && w * 3 + 3 >= g.bb_w && w <= g.bb_w;
    }
    return false;
}

// Readable names for virtual key codes (Keys tab).
static std::string key_name(const KeyBind &k)
{
    if (k.key == 0)
        return "(none)";
    std::string s;
    if (k.ctrl) s += "Ctrl + ";
    if (k.shift) s += "Shift + ";
    if (k.alt) s += "Alt + ";
    const uint32_t c = k.key;
    char b[32];
    if (c >= VK_NUMPAD0 && c <= VK_NUMPAD9) { std::snprintf(b, sizeof(b), "Numpad %u", c - VK_NUMPAD0); return s + b; }
    if (c >= VK_F1 && c <= VK_F24) { std::snprintf(b, sizeof(b), "F%u", c - VK_F1 + 1); return s + b; }
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) { b[0] = static_cast<char>(c); b[1] = 0; return s + b; }
    switch (c)
    {
    case VK_DECIMAL: return s + "Numpad .";
    case VK_ADD: return s + "Numpad +";
    case VK_SUBTRACT: return s + "Numpad -";
    case VK_MULTIPLY: return s + "Numpad *";
    case VK_DIVIDE: return s + "Numpad /";
    case VK_INSERT: return s + "Insert";
    case VK_DELETE: return s + "Delete";
    case VK_HOME: return s + "Home";
    case VK_END: return s + "End";
    case VK_PRIOR: return s + "Page Up";
    case VK_NEXT: return s + "Page Down";
    case VK_UP: return s + "Up";
    case VK_DOWN: return s + "Down";
    case VK_LEFT: return s + "Left";
    case VK_RIGHT: return s + "Right";
    case VK_SPACE: return s + "Space";
    case VK_RETURN: return s + "Enter";
    case VK_TAB: return s + "Tab";
    case VK_PAUSE: return s + "Pause";
    case VK_SCROLL: return s + "Scroll Lock";
    case VK_OEM_PLUS: return s + "=";
    case VK_OEM_MINUS: return s + "-";
    case VK_OEM_COMMA: return s + ",";
    case VK_OEM_PERIOD: return s + ".";
    case VK_OEM_1: return s + ";";
    case VK_OEM_2: return s + "/";
    case VK_OEM_3: return s + "`";
    case VK_OEM_4: return s + "[";
    case VK_OEM_5: return s + "\\";
    case VK_OEM_6: return s + "]";
    case VK_OEM_7: return s + "'";
    default: break;
    }
    std::snprintf(b, sizeof(b), "key 0x%02X", c);
    return s + b;
}

// The graphics APIs this add-on can inject into. ReShade itself supports more, but a Vulkan or OpenGL render pass cannot be
// interrupted at a binding change, and CRCGI needs DirectX 10 or later; D3D9 and both of those stay at level A.
// D3D12 is disabled for now: after a mid-frame render_technique, ReShade's state tracker re-binds render targets, pipelines and
// descriptor tables but NOT root constants / root descriptors (push_constants / push_descriptors). The game's next draws would then
// read unbound root parameters, which removes the device (crash). The D3D12 code paths stay in place for when that is handled.
// In-frame injection: D3D10/11 (ReShade restores the state itself) and D3D12 (the add-on's own tracker restores it; experimental,
// can be switched off). Vulkan and OpenGL: not yet. D3D9 is not supported (CRCGI needs D3D10+).
static bool api_supports_injection(device_api api)
{
    return api == device_api::d3d10 || api == device_api::d3d11 || (api == device_api::d3d12 && g.d3d12_inject);
}

// Material feeds (copies of game textures, bound at the injection point or at the end of the frame) work on every API except D3D9.
static bool api_supports_feeds(device_api api)
{
    return api == device_api::d3d10 || api == device_api::d3d11 || api == device_api::d3d12 || api == device_api::vulkan || api == device_api::opengl;
}

static const char *injection_unsupported_reason(device_api api)
{
    if (api == device_api::d3d12)
        return "Direct3D 12 in-frame injection is switched off (Options). The effect runs at the end of the frame; the G-buffer feeds still work.";
    if (api == device_api::vulkan)
        return "Vulkan (also DirectX 9 / 10 / 11 games through DXVK): the effect runs at the end of the frame. Material feeds, the profiler, "
               "the layer list and the camera work. In-frame injection is not available: after an effect the game's Vulkan state (and "
               "DXVK's cached state) cannot be restored safely.";
    if (api == device_api::opengl)
        return "in-frame injection is not available on OpenGL. The effect runs at the end of the frame; the G-buffer feeds (level C) still work.";
    return "this API is not supported (CRCGI needs Direct3D 10 or later).";
}

static const char *api_name(device_api api)
{
    switch (api)
    {
    case device_api::d3d9: return "Direct3D 9";
    case device_api::d3d10: return "Direct3D 10";
    case device_api::d3d11: return "Direct3D 11";
    case device_api::d3d12: return "Direct3D 12";
    case device_api::opengl: return "OpenGL";
    case device_api::vulkan: return "Vulkan";
    default: return "unknown API";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings (next to the game executable: one file per game). A missing, unreadable or malformed file falls back to the defaults;
// a write failure is reported in the UI and never disables the add-on.
// ---------------------------------------------------------------------------------------------------------------------
static std::filesystem::path module_directory()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 8; attempt++)
    {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return std::filesystem::path();                         // error: no settings file, defaults only
        if (n < buf.size())
        {
            buf.resize(n);
            return std::filesystem::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);                                 // truncated (long path): grow and retry
    }
    return std::filesystem::path();
}

static void write_ref(std::ofstream &f, const char *name, const LayerRef &r)
{
    f << name << "=" << hex16(r.key) << ":" << r.slot << "\n";
}

static LayerRef parse_ref(const std::string &v)
{
    LayerRef r;
    size_t c = v.find(':');
    r.key = std::strtoull(v.substr(0, c).c_str(), nullptr, 16);
    r.slot = c == std::string::npos ? 0 : std::atoi(v.substr(c + 1).c_str());
    if (r.slot < 0 || r.slot >= 8)
        r = LayerRef();
    return r;
}

// Call with g.mtx held.
static void save_settings_locked()
{
    if (g.ini_path.empty())
        return;
    std::filesystem::path tmp = g.ini_path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f)
        {
            g.settings_error = "Cannot write the settings file next to the game executable (read-only folder?). Settings are not saved.";
            return;
        }
        f << "[General]\n";
        f << "Version=" << SETTINGS_VERSION << "\nMaster=" << g.master << "\nLevel=" << g.level << "\nFallbackEnd=" << g.fallback_end
          << "\nDepthLive=" << g.depth_live << "\nOnlyUseful=" << g.only_useful << "\nCrossListFeeds=" << g.cross_list_feeds
          << "\nD3D12Inject=" << g.d3d12_inject << "\nVerboseLog=" << g.verbose_log << "\nSortMode=" << g.sort_mode
          << "\nUpscaleMode=" << g.upscale_mode << "\nUpscalePct=" << g.upscale_pct << "\nInjectPolicy=" << g.inject_policy
          << "\nAutoRedetect=" << g.auto_redetect << "\nGeomSource=" << g.geom_source << "\nAddNewLayers=" << g.cat_add_new
          << "\nRepeatDelay=" << g.repeat_delay_ms << "\nRepeatRate=" << g.repeat_rate_ms << "\n";
        write_ref(f, "Inject", g.role_ref[ROLE_INJECT]);
        write_ref(f, "Depth", g.role_ref[ROLE_DEPTH]);
        f << "Techniques=";
        for (size_t i = 0; i < g.techniques.size(); i++)
            f << (i ? ";" : "") << g.techniques[i];
        f << "\n\n[Camera]\nAuto=" << g.cam_auto << "\nApply=" << g.cam_apply << "\nSig=" << g.cam_sig << "\n\n";
        for (int i = 0; i < FEED_COUNT; i++)
        {
            const FeedConfig &c = g.feed[i];
            f << "[Feed" << i << "]\n";
            write_ref(f, "Ref", c.ref);
            f << "Source=" << c.source << "\nTex=" << c.tex.fmt << ":" << c.tex.w << ":" << c.tex.h << ":" << c.tex.samples << ":" << c.tex.usage
              << ":" << c.tex.ordinal << "\nEnabled=" << c.enabled << "\nSync=" << c.sync << "\nMode=" << c.mode << "\nSpace=" << c.space
              << "\nUnits=" << c.units << "\nFlipY=" << c.flip_y << "\nInvert=" << c.invert << "\nChannel=" << c.channel
              << "\nMeaning=" << c.meaning << "\n\n";
        }
        f << "[Keys]\n";
        for (int i = 0; i < KA_COUNT; i++)
            f << "Key" << i << "=" << g.keys[i].key << ":" << g.keys[i].ctrl << ":" << g.keys[i].shift << ":" << g.keys[i].alt << "\n";
        f.flush();
        if (!f)
        {
            g.settings_error = "Writing the settings file failed (disk full?). Settings are not saved.";
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g.ini_path, ec);                   // replaces the old file in one step
    if (ec)
    {
        g.settings_error = "Cannot replace the settings file: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return;
    }
    g.settings_error.clear();
}

static void load_settings()
{
    std::lock_guard<std::mutex> lock(g.mtx);
    if (g.ini_path.empty())
    {
        g.settings_error = "The game's folder could not be determined: defaults are used and settings are not saved.";
        return;
    }
    for (int i = 0; i < KA_COUNT; i++)
        g.keys[i] = KA_DEFAULT[i];                                  // defaults first; the file overrides what it has
    std::ifstream f(g.ini_path);
    if (!f)
        return;                                                     // first run: defaults
    std::string line, section;
    int version = 1;
    bool cross_list = g.cross_list_feeds;
    bool sync_seen[FEED_COUNT] = {}, sync_val[FEED_COUNT] = {};
    while (std::getline(f, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        auto b = [&] { return v == "1"; };
        if (section == "General")
        {
            if (k == "Version") version = std::atoi(v.c_str());
            else if (k == "Master") g.master = b();
            else if (k == "Level") g.level = std::clamp(std::atoi(v.c_str()), 0, 2);
            else if (k == "FallbackEnd") g.fallback_end = b();
            else if (k == "DepthLive") g.depth_live = b();
            else if (k == "OnlyUseful") g.only_useful = b();
            else if (k == "CrossListFeeds") cross_list = b();
            else if (k == "D3D12Inject") g.d3d12_inject = b();
            else if (k == "VerboseLog") g.verbose_log = b();
            else if (k == "SortMode") g.sort_mode = std::clamp(std::atoi(v.c_str()), 0, SORT_COUNT - 1);
            else if (k == "UpscaleMode") g.upscale_mode = std::clamp(std::atoi(v.c_str()), 0, UP_COUNT - 1);
            else if (k == "UpscalePct") g.upscale_pct = std::clamp(static_cast<float>(std::atof(v.c_str())), 20.0f, 100.0f);
            else if (k == "InjectPolicy") g.inject_policy = std::clamp(std::atoi(v.c_str()), 0, IP_COUNT - 1);
            else if (k == "AutoRedetect") g.auto_redetect = b();
            else if (k == "GeomSource") g.geom_source = std::clamp(std::atoi(v.c_str()), 0, 2);
            else if (k == "AddNewLayers") g.cat_add_new = b();
            else if (k == "RepeatDelay") g.repeat_delay_ms = std::clamp(std::atoi(v.c_str()), 100, 1500);
            else if (k == "RepeatRate") g.repeat_rate_ms = std::clamp(std::atoi(v.c_str()), 15, 500);
            else if (k == "AutoTonemap") {}                         // version 1 setting: removed, ignored on purpose
            else if (k == "Inject") g.role_ref[ROLE_INJECT] = parse_ref(v);
            else if (k == "Depth") g.role_ref[ROLE_DEPTH] = parse_ref(v);
            else if (k == "Techniques") g.techniques = split(v, ';');
        }
        else if (section == "Camera")
        {
            if (k == "Auto") g.cam_auto = b();
            else if (k == "Apply") g.cam_apply = b();
            else if (k == "Sig") g.cam_sig = std::strtoll(v.c_str(), nullptr, 10);
        }
        else if (section.rfind("Feed", 0) == 0 && section.size() == 5 && std::isdigit(static_cast<unsigned char>(section[4])))
        {
            int i = section[4] - '0';
            if (i < 0 || i >= FEED_COUNT)
                continue;
            FeedConfig &c = g.feed[i];
            if (k == "Ref") c.ref = parse_ref(v);
            else if (k == "Enabled") c.enabled = b();
            else if (k == "Sync") { sync_seen[i] = true; sync_val[i] = b(); }
            else if (k == "Source") c.source = std::clamp(std::atoi(v.c_str()), 0, 1);
            else if (k == "Tex")
            {
                std::vector<std::string> t = split(v, ':');
                if (t.size() == 6)
                {
                    c.tex.fmt = static_cast<uint32_t>(std::strtoul(t[0].c_str(), nullptr, 10));
                    c.tex.w = static_cast<uint32_t>(std::strtoul(t[1].c_str(), nullptr, 10));
                    c.tex.h = static_cast<uint32_t>(std::strtoul(t[2].c_str(), nullptr, 10));
                    c.tex.samples = static_cast<uint32_t>(std::strtoul(t[3].c_str(), nullptr, 10));
                    c.tex.usage = static_cast<uint32_t>(std::strtoul(t[4].c_str(), nullptr, 10));
                    c.tex.ordinal = static_cast<uint32_t>(std::strtoul(t[5].c_str(), nullptr, 10));
                }
            }
            else if (k == "Space") c.space = std::clamp(std::atoi(v.c_str()), 0, 1);
            else if (k == "Mode") c.mode = std::clamp(std::atoi(v.c_str()), 0, 2);
            else if (k == "Units") c.units = std::clamp(std::atoi(v.c_str()), 0, 2);
            else if (k == "FlipY") c.flip_y = b();
            else if (k == "Invert") c.invert = b();
            else if (k == "Channel") c.channel = std::clamp(std::atoi(v.c_str()), 0, 3);
            else if (k == "Meaning") c.meaning = std::clamp(std::atoi(v.c_str()), 0, 3);
        }
        else if (section == "Keys" && k.rfind("Key", 0) == 0)
        {
            const int i = std::atoi(k.c_str() + 3);
            std::vector<std::string> t = split(v, ':');
            if (i >= 0 && i < KA_COUNT && t.size() == 4)
            {
                g.keys[i].key = static_cast<uint32_t>(std::strtoul(t[0].c_str(), nullptr, 10)) & 0xFF;
                g.keys[i].ctrl = t[1] == "1";
                g.keys[i].shift = t[2] == "1";
                g.keys[i].alt = t[3] == "1";
            }
        }
    }
    for (int i = 0; i < FEED_COUNT; i++)
        g.feed[i].sync = sync_seen[i] ? sync_val[i] : false;        // the shader's switches stay yours unless 'Drive the shader's switch' is ticked
    g.cross_list_feeds = cross_list;
    if (version < SETTINGS_VERSION)
        g.dirty = true;                                             // rewrite in the current format on the next frame
}
