/*
    CRCGI v1 (Compensated Radiance Cascade Global Illumination) - screen space global illumination for ReShade
    (DirectX 10+, OpenGL, Vulkan; DirectX 9 games through DXVK, which ReShade sees as Vulkan).

    HOW IT WORKS
    - Geometry: view-space positions and normals are rebuilt from linear depth and the vertical Field of View, or taken from the
      game's G-buffer when the CRCGI Injector add-on delivers it (Game Integration).
    - Lighting: cascades of surface-oriented hemisphere probes (4 / 16 / 64 / 256 ... directions with CRCGI_BASE_TILE 2). Each traces
      only its own screen-space interval (penumbra layout, x4 per cascade; from 'Reach Applies From Cascade' on, spread up to the
      Cascade Reach) and they are merged far -> near per direction over the 4 bilinear parent probes:
          L_c(dir) = hit ? radiance : average of the 4 child directions of the parents.
      Energy Carry scales the light handed down per merge (from 'Energy Carry From Cascade' on).
    - The cascades from 'Far Cascades: Accumulate From' on keep their own history, are jittered every frame and accumulated longer
      the farther they are (up to 64 frames), which removes the static noise of the long cascades.
    - Multibounce: a hit reads last frame's GI over the penumbra of the ray that found it, times the surface's reflectance (the
      game's albedo, or Bounce Reflectance with the surface's hue), held below 0.9 per bounce so it always converges.
    - Off-screen canvas: the light and depth of what left the screen, kept with a fading confidence, marched through by rays.
    - HDR & Color Range: HDR output (scRGB / HDR10) is decoded and re-encoded; HDR Light Pickup lets bright sources keep their range.
    - Sizes are in effect texels (1 texel = CRCGI_RESOLUTION_DIVISOR screen pixels) or screen fractions, never in pixels of a
      reference resolution, so a preset looks the same at every screen resolution.

    LIMITS
    - ReShade has no camera matrices. Positions come from linear depth and the Field of View; without the add-on, motion comes from
      image matching, so it is wrong for fast motion, moving objects and lighting changes (the history is rejected there).
    - Only what is on screen (or still in the canvas) can bounce light.
    - "Sky" light is a tintable ambient layer gated by cascade visibility, not a sky capture.
*/

#include "ReShade.fxh"

// ----------------------------------------------------------------------------
// Preprocessor tunables (ReShade's "Preprocessor definitions"; the effect reloads when changed)
// ----------------------------------------------------------------------------

#ifndef CRCGI_RESOLUTION_DIVISOR
    #define CRCGI_RESOLUTION_DIVISOR 3   // cascade resolution: 1 = full, 2 = half, 3 = third (default), 4 = quarter
#endif
#ifndef CRCGI_BASE_TILE
    #define CRCGI_BASE_TILE 2            // directions per probe axis at cascade 0: 2 (default) or 4
#endif
#ifndef CRCGI_CASCADES
    #define CRCGI_CASCADES 4             // 4 (default), 5 or 6 cascades. 5 and 6 add farther cascades (more rays, more memory)
#endif
#ifndef CRCGI_CANVAS_MARGIN
    #define CRCGI_CANVAS_MARGIN 24       // off-screen canvas border on each side, in percent of the screen (0 = no canvas)
#endif

#ifndef CRCGI_SECONDARY_DIVISOR
    #define CRCGI_SECONDARY_DIVISOR 1    // multibounce map: 0 = the exact per-hit multibounce read (no map); 1 (default), 2, 3 ... = a map
                                          // rendered at the effect resolution divided by this value. Contact shadows are always full resolution
#endif

#ifndef BUFFER_COLOR_SPACE
    #define BUFFER_COLOR_SPACE 1         // set by ReShade 5.1+: 1 sRGB, 2 scRGB, 3 HDR10 PQ, 4 HLG
#endif

#define RC_HALF_W   (BUFFER_WIDTH / CRCGI_RESOLUTION_DIVISOR)
#define RC_HALF_H   (BUFFER_HEIGHT / CRCGI_RESOLUTION_DIVISOR)
#if CRCGI_SECONDARY_DIVISOR > 0
    #define SEC_W   (RC_HALF_W / CRCGI_SECONDARY_DIVISOR)
    #define SEC_H   (RC_HALF_H / CRCGI_SECONDARY_DIVISOR)
#endif
#if CRCGI_CASCADES >= 6
    #define RC_PAD  (CRCGI_BASE_TILE * 32)                          // largest probe tile (cascade 5)
#elif CRCGI_CASCADES == 5
    #define RC_PAD  (CRCGI_BASE_TILE * 16)                          // largest probe tile (cascade 4)
#else
    #define RC_PAD  (CRCGI_BASE_TILE * 8)                           // largest probe tile (cascade 3)
#endif
#define RC_TEX_W    (((RC_HALF_W + RC_PAD - 1) / RC_PAD) * RC_PAD)
#define RC_TEX_H    (((RC_HALF_H + RC_PAD - 1) / RC_PAD) * RC_PAD)
#define RC_PI       3.14159265359

static const float2 RC_SIZE      = float2(RC_HALF_W, RC_HALF_H);
static const float2 CASC_SIZE    = float2(RC_TEX_W, RC_TEX_H);
#define RC_P0_W     (RC_TEX_W / CRCGI_BASE_TILE)
#define RC_P0_H     (RC_TEX_H / CRCGI_BASE_TILE)
static const float2 P0_SIZE      = float2(RC_P0_W, RC_P0_H);
static const int2   RC_MAX_TEXEL = int2(RC_HALF_W - 1, RC_HALF_H - 1);
static const float  RES_SCALE    = BUFFER_HEIGHT / 1440.0;      // internal only: the cascade layout's travel unit (no setting uses it)
static const float  RC_DIV       = CRCGI_RESOLUTION_DIVISOR;    // full-res pixels per cascade texel
static const float3 LUMA         = float3(0.2126, 0.7152, 0.0722);

// Off-screen canvas: the cascade-resolution screen plus a border of CRCGI_CANVAS_MARGIN percent on each side.
#if CRCGI_CANVAS_MARGIN > 0
    #define CANVAS_PAD_W ((RC_HALF_W * CRCGI_CANVAS_MARGIN) / 100)
    #define CANVAS_PAD_H ((RC_HALF_H * CRCGI_CANVAS_MARGIN) / 100)
#else
    #define CANVAS_PAD_W 0
    #define CANVAS_PAD_H 0
#endif
#define CANVAS_W    (RC_HALF_W + 2 * CANVAS_PAD_W)
#define CANVAS_H    (RC_HALF_H + 2 * CANVAS_PAD_H)
static const float2 CANVAS_M     = float2(CANVAS_PAD_W, CANVAS_PAD_H) / float2(RC_HALF_W, RC_HALF_H);   // margin in screen uv
float2 ScreenToCanvas(float2 u) { return (u + CANVAS_M) / (1.0 + 2.0 * CANVAS_M); }
float2 CanvasToScreen(float2 c) { return c * (1.0 + 2.0 * CANVAS_M) - CANVAS_M; }

// Sky map: premultiplied sky colour (rgb * confidence, confidence) over the screen; mip 6 is the 1 x 1 average.
#define SKYMAP_W 64
#define SKYMAP_H 32

uniform int framecount < source = "framecount"; >;

// ============================================================================
// SECTION: Setup
// Set once per game.
// ============================================================================

uniform float FieldOfView <
    ui_category = "Setup"; ui_category_closed = false;
    ui_type = "slider"; ui_min = 30.0; ui_max = 110.0;
    ui_label = "Vertical Field of View";
    ui_tooltip = "The game's VERTICAL field of view in degrees (convert it if the game only shows a horizontal one). Positions, normals "
                 "and the paths rays take are rebuilt from depth with it.\n"
                 "Check it in Debug View > Normals: a flat wall must be one even colour. Speckled or striped normals mean the value is "
                 "wrong.\n"
                 "The add-on writes it for you while it has the game's camera.";
> = 60.000019;

uniform float SkyDepthThreshold <
    ui_category = "Setup"; ui_category_closed = false;
    ui_type = "slider"; ui_min = 0.5; ui_max = 1.0; ui_step = 0.0001;
    ui_label = "Sky Rejection Depth";
    ui_tooltip = "Linear depth (0 = camera, 1 = far plane) where the sky starts. Pixels at or beyond it get no GI, give no bounce, and "
                 "rays pass through them.\n"
                 "Check it in Debug View > Linear Depth: the sky must be clearly separate from the terrain. Too low: far terrain is "
                 "treated as sky and gets no GI. Too high (or the game's depth never reaches 1): the sky edge gets lit.\n"
                 "Games spread depth very differently. With a long draw distance (racing, flight) the horizon can sit at 0.99 or more, "
                 "so set this just above it (Ctrl+click the slider to type a value such as 0.9995).";
> = 0.970000;

// ============================================================================
// SECTION: Brightness
// How strong the GI is and how it is added to the image.
// ============================================================================

uniform float Intensity <
    ui_category = "Brightness"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 8.0;
    ui_label = "GI Intensity";
    ui_tooltip = "Master level of the GI that is added to the image. At 0 nothing is added and the picture is the game's own, so raise "
                 "it from there. Debug View > GI Only shows the light by itself (it ignores this slider and uses Debug Gain), so you can "
                 "judge the light before you add it.\n"
                 "Every other slider shapes the light, this one scales the result: set it last.";
> = 0.000000;

uniform float DarkAreaBoost <
    ui_category = "Brightness"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 4.0; ui_step = 0.01;
    ui_label = "Dark Area Boost";
    ui_tooltip = "Extra GI on surfaces that are dark compared to the rest of the screen (shadows, interiors). 0 = off. Raising it lifts "
                 "dark areas; too much turns shadows into flat grey.\n"
                 "What counts as dark is set by Dark Area Threshold, which does nothing while this is 0.";
> = 0.000000;

uniform float DarkAreaThreshold <
    ui_category = "Brightness"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 1.5; ui_step = 0.01;
    ui_label = "Dark Area Threshold";
    ui_tooltip = "What counts as dark for Dark Area Boost, relative to the average brightness of the screen (measured every frame, so it "
                 "follows day and night without retuning).\n"
                 "Lower = only the darkest surfaces get the boost. Higher = more of the screen counts as dark. Does nothing while Dark "
                 "Area Boost is 0.";
> = 1.000000;

uniform float GIHeadroomRolloff <
    ui_category = "Brightness"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Highlight Rolloff (added light only)";
    ui_tooltip = "Eases the added GI toward white instead of letting it clip in bright areas (the game's own image is never changed). 0 "
                 "= plain add, 1 = full easing. In HDR the ceiling is HDR Peak Brightness.\n"
                 "Raise it if bright walls clip when GI Intensity is high. Too high flattens bright areas. Rolloff Minimum Headroom sets "
                 "the least room it assumes.";
> = 0.740000;

uniform float HeadroomFloor <
    ui_category = "Brightness"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.001; ui_max = 0.2;
    ui_label = "Rolloff Minimum Headroom";
    ui_tooltip = "The least room Highlight Rolloff assumes above a pixel, so pixels that are already bright still receive a little GI. "
                 "Raise it if highlights look flat, lower it to protect them more.\n"
                 "Does nothing while Highlight Rolloff is 0.";
> = 0.030000;

// ============================================================================
// SECTION: Game Integration
// Inputs from the CRCGI Injector add-on (the game's own G-buffer). Each switch is driven by the add-on by default.
// ============================================================================

uniform bool UseNormalTexture <
    ui_category = "Game Integration"; ui_category_closed = true;
    ui_label = "Use Game Normals If Provided";
    ui_tooltip = "Use the game's normals (from the add-on) instead of rebuilding them from depth. Turn it on once G-Buffer / Canvas View "
                 "> Normal Difference is mostly black.\n"
                 "The add-on only switches this for you if 'Drive the shader's switch' is ticked for normals in the add-on's G-Buffer "
                 "tab. Pixels with a game normal no longer use the Geometry Reconstruction settings (see Geometry Source).";
> = 0;

uniform int Integration_NormalSpace <
    ui_category = "Game Integration"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "View space (fixed axes)\0World or any other space (auto-aligned to the camera)\0";
    ui_label = "Game Normal Space";
    ui_tooltip = "How the game stores its normals. View space: used as they are. World or any other space: every frame the shader fits "
                 "the rotation that lines the game's normals up with the normals rebuilt from depth (this also absorbs flipped or "
                 "swapped axes). It needs a view with surfaces facing at least two different ways, otherwise the last good fit is kept.\n"
                 "Check it with G-Buffer / Canvas View > Game Normal Auto-Align Status (green bar) and Normal Difference (mostly black). "
                 "Only used while game normals are on.";
> = 1;

uniform float Integration_NormalFitSmoothing <
    ui_category = "Game Integration"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.95; ui_step = 0.01;
    ui_label = "Normal Auto-Align Smoothing";
    ui_tooltip = "World space only: how much of last frame's fit is kept. 0 = fitted fresh every frame (follows fast camera turns, may "
                 "shimmer). Higher = steadier, but lags on fast turns.\n"
                 "Raise it if the Auto-Align Status view flickers.";
> = 0.250000;

uniform int Integration_GeometrySource <
    ui_category = "Game Integration"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Rebuild from depth everywhere\0Game normals where valid, rebuild elsewhere\0Game normals only (skip the depth rebuild)\0";
    ui_label = "Geometry Source (with game normals)";
    ui_tooltip = "Where the normals come from while game normals are on.\n"
                 "Rebuild from depth everywhere: the game's normals are ignored.\n"
                 "Game normals where valid: pixels with a game normal use it, sky and holes are rebuilt.\n"
                 "Game normals only: no depth rebuild at all (fastest, pixels without a game normal face the camera).\n"
                 "With either game option, the Geometry Reconstruction settings do nothing for the pixels that have a game normal.";
> = 1;

uniform bool Integration_Albedo <
    ui_category = "Game Integration"; ui_category_closed = true;
    ui_label = "Use Game Albedo If Provided";
    ui_tooltip = "Use the game's albedo (base colour) from the add-on instead of estimating it from the image. Turn it on once G-Buffer "
                 "/ Canvas View > Game Albedo looks like the scene's flat colours.\n"
                 "It replaces the estimate, so Relit Albedo Detail, Relit Detail Radius and Bounce Reflectance (estimate) do nothing "
                 "where the albedo is valid, and Surface Albedo Response uses the real colour.";
> = false;

uniform bool Integration_Motion <
    ui_category = "Game Integration"; ui_category_closed = true;
    ui_label = "Use Game Motion Vectors If Provided";
    ui_tooltip = "Use the game's motion vectors from the add-on instead of the shader's own block-matching estimate (those passes then "
                 "do no work). Turn it on once G-Buffer / Canvas View > Game Motion Vectors looks right (units, Flip Y and Invert are "
                 "set in the add-on's G-Buffer tab).\n"
                 "The motion is then exact, so Motion Mismatch Rejection does nothing.";
> = false;

uniform bool Integration_Material <
    hidden = true;
    ui_category = "Game Integration"; ui_category_closed = true;
    ui_label = "Use Game Material (reflectivity) If Provided";
    ui_tooltip = "Reserved. Game material input is not available in this release.";
> = false;

uniform bool Integration_HDRScene <
    ui_category = "Game Integration"; ui_category_closed = true;
    ui_label = "Use Game HDR Scene Light If Provided";
    ui_tooltip = "Use the game's lighting buffer (before tonemapping, bloom, fog and UI) from the add-on as the light rays pick up, "
                 "instead of the finished image: no UI, no bloom, no film grain, and the full brightness range. The exposure is matched "
                 "to the image every frame.\n"
                 "Turn it on once G-Buffer / Canvas View > Game HDR Scene Light looks like the image without UI and effects (black = not "
                 "delivered or no match). While it is on, Light Rays Pick Up After Conversion does nothing.";
> = false;

// ============================================================================
// SECTION: HDR & Color Range
// What the game outputs (SDR / HDR) and how much brightness range the light pickup keeps.
// ============================================================================

uniform int HDRInputMode <
    ui_category = "HDR & Color Range"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Auto (from the game's output format)\0SDR (sRGB)\0HDR scRGB (linear, 1.0 = 80 nits)\0HDR10 (PQ / ST 2084)\0";
    ui_label = "Game Output Signal";
    ui_tooltip = "How the game's image is encoded. Auto reads it from ReShade. Set it by hand only if Auto is wrong (a washed-out or "
                 "blown-out picture).\n"
                 "In HDR (scRGB or HDR10) the image is decoded to light, the GI is added there and it is encoded again, so highlights "
                 "keep their range. The Tonemapper Conversion settings and Output Dither are not used in HDR, and Highlight Rolloff "
                 "works up to HDR Peak Brightness.";
> = 2;

uniform float HDRPaperWhite <
    ui_category = "HDR & Color Range"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 80.0; ui_max = 500.0; ui_step = 1.0;
    ui_label = "HDR Paper White (nits)";
    ui_tooltip = "HDR only. The brightness of a white UI element in the game's HDR settings, in nits. Every threshold the shader "
                 "measures (emitters, sky, pickup) treats this as 1.0, so match the game's own paper white or UI brightness setting. A "
                 "wrong value shifts all of them.";
> = 80.000000;

uniform float HDRPeakNits <
    ui_category = "HDR & Color Range"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 400.0; ui_max = 10000.0; ui_step = 10.0;
    ui_label = "HDR Peak Brightness (nits)";
    ui_tooltip = "HDR only. The game's peak brightness setting, in nits. Highlight Rolloff lets the added light approach this instead of "
                 "paper white. Too low clips the added light early.";
> = 1000.000000;

uniform float HDRPickupBoost <
    ui_category = "HDR & Color Range"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "HDR Light Pickup";
    ui_tooltip = "Lets bright sources (lamps, fire, sunlit spots) bounce more than a white wall. An SDR image squeezes them into the top "
                 "of the range, so on their own they would give no more light than a wall. 0 = off. Raising it makes bright lights throw "
                 "richer, more directional light; too much gives fireflies (small flickering bright dots). Mid grey and darker never "
                 "change. HDR Pickup Range is the ceiling.\n"
                 "In SDR the brightening follows the Input Tonemapper (set it to the game's curve, it is used even while Convert "
                 "Tonemapper is off). In HDR output, or with Use Game HDR Scene Light, the real range above paper white is used instead. "
                 "See Debug View > Emitters.";
> = 0.600000;

uniform float HDRPickupRange <
    ui_category = "HDR & Color Range"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 16.0; ui_step = 0.1;
    ui_label = "HDR Pickup Range";
    ui_tooltip = "The ceiling for HDR Light Pickup: the most a source may be brightened (times this). Higher = stronger lamps and "
                 "highlights, more firefly risk. Does nothing while HDR Light Pickup is 0.";
> = 4.000000;

// ============================================================================
// SECTION: Tonemapper Conversion
// Optional: undo the game's tonemapper and apply another (SDR).
// ============================================================================

uniform bool ConvertTonemapper <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_label = "Convert Tonemapper";
    ui_tooltip = "Undoes the Input Tonemapper on the game's image, applies the Output Tonemapper instead, then adds the GI on top. SDR "
                 "only (ignored in HDR). Off = the GI is added to the untouched image.\n"
                 "Turn it on only if you want a different curve. If the picture changes while Output Tonemapper equals Input Tonemapper, "
                 "the input curve is wrong.\n"
                 "While it is off, everything else in this section does nothing, except that the Input Tonemapper still shapes HDR Light "
                 "Pickup.";
> = 0;

uniform int GameTonemap <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Reinhard\0ACES filmic (modern games)\0None (plain SDR)\0ACES 2.0 (modern games)\0AgX (Witcher 3 Remastered)\0Gran Turismo 7\0Uncharted 2 / Hable filmic\0ACES (Hill fit, many games)\0Uchimura (Gran Turismo Sport)\0";
    ui_label = "Input Tonemapper (the game's)";
    ui_tooltip = "The curve the game used to turn its light into the image, undone first. Nothing is detected, pick the one that matches "
                 "the game: ACES filmic or ACES 2.0 for most modern games, AgX for Witcher 3 Remastered, Uncharted 2 / Hable for many "
                 "last-generation games, ACES (Hill fit) for engines that use that fit, Uchimura for Gran Turismo Sport.\n"
                 "Used by Convert Tonemapper and by HDR Light Pickup. This curve always uses its published settings, only the Output "
                 "Tonemapper has tunables. ACES 2.0 uses simplified colour handling and the Gran Turismo 7 inverse is approximate for "
                 "saturated colours.";
> = 4;

uniform int OutputTonemap <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Reinhard\0ACES filmic (modern games)\0None (plain SDR)\0ACES 2.0 (modern games)\0AgX (Witcher 3 Remastered)\0Gran Turismo 7\0Uncharted 2 / Hable filmic\0ACES (Hill fit, many games)\0Uchimura (Gran Turismo Sport)\0";
    ui_label = "Output Tonemapper";
    ui_tooltip = "The curve applied once the Input Tonemapper is undone. Set it equal to the Input Tonemapper first: the image must come "
                 "back unchanged (apart from a clipped highlight). Then pick another curve for a different look. Only used while Convert "
                 "Tonemapper is on.\n"
                 "The tunables below this one only act on the curve picked here.";
> = 4;

uniform bool MatchMidGrey <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_label = "Match Mid-Grey";
    ui_tooltip = "Tonemappers put mid grey at very different brightness. On: the light is scaled so mid grey lands where the Input "
                 "Tonemapper put it, and only the contrast, highlight roll-off and colour of the new curve change. Off: the curve is "
                 "applied as it is and the overall brightness shifts with it. Only used while Convert Tonemapper is on.";
> = 1;

uniform bool KeepClippedWhites <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_label = "Keep Clipped Whites";
    ui_tooltip = "Pixels that are white in the game's image stay at least that white after the conversion. Some curves (AgX, ACES 2.0) "
                 "do not reach white from the light level the inverse gives a clipped pixel, which would leave clipped highlights "
                 "slightly grey. Only used while Convert Tonemapper is on.";
> = 1;

uniform bool PickupAfterConversion <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_label = "Light Rays Pick Up After Conversion";
    ui_tooltip = "Rays pick up the image after the tonemapper conversion (on) or the game's original image (off). Does nothing while "
                 "Convert Tonemapper is off, in HDR, or with Use Game HDR Scene Light. On costs one conversion per effect texel.";
> = 1;

uniform float TMDither <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.05;
    ui_label = "Output Dither (levels)";
    ui_tooltip = "Noise of this many 8-bit levels added to the final colour while Convert Tonemapper is on. Undoing and re-applying a "
                 "curve stretches dark gradients, which shows as banding. Raise it if you see banding in dark gradients, too high shows "
                 "as fine noise. 0 = off.";
> = 1.000000;

uniform float InputExposure <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = -3.0; ui_max = 3.0; ui_step = 0.05;
    ui_label = "Input Exposure (EV)";
    ui_tooltip = "How many stops brighter than standard the game fed light into its curve. It only moves where mid grey sits on the "
                 "Input Tonemapper, which shifts where the new curve's highlight roll-off starts relative to mid grey. Only matters with "
                 "Match Mid-Grey on. 0 = standard, leave it unless mid grey looks wrong.";
> = 0.000000;

uniform float OutputExposure <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = -3.0; ui_max = 3.0; ui_step = 0.05;
    ui_label = "Output Exposure (EV)";
    ui_tooltip = "Stops added to the scene light before the Output Tonemapper, on top of Match Mid-Grey. A taste control for the overall "
                 "brightness of the converted image. Only used while Convert Tonemapper is on.";
> = 0.000000;

uniform float OutputContrast <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.01;
    ui_label = "Output Contrast";
    ui_tooltip = "Contrast around mid grey before the Output Tonemapper: above 1 darks get darker and brights brighter, below 1 the "
                 "opposite. Mid grey stays where it is. 1 = off. Only used while Convert Tonemapper is on.";
> = 1.000000;

uniform float OutputSaturation <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
    ui_label = "Output Saturation";
    ui_tooltip = "Colour saturation after the Output Tonemapper, around each pixel's own luminance. 1 = off, 0 = grey. Only used while "
                 "Convert Tonemapper is on.";
> = 1.000000;

uniform float ReinhardWhite <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 100.0; ui_step = 0.5;
    ui_label = "Reinhard: White Point";
    ui_tooltip = "Only used while the Output Tonemapper is Reinhard. The scene light that maps to white. Lower = highlights clip sooner "
                 "and look punchier, 100 = plain Reinhard.";
> = 100.000000;

uniform float HableShoulder <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.03; ui_max = 0.5; ui_step = 0.005;
    ui_label = "Hable: Shoulder Strength";
    ui_tooltip = "Only used while the Output Tonemapper is Uncharted 2 / Hable. Higher = a harder, earlier highlight roll-off.";
> = 0.150000;

uniform float HableToe <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 0.5; ui_step = 0.005;
    ui_label = "Hable: Toe Strength";
    ui_tooltip = "Only used while the Output Tonemapper is Uncharted 2 / Hable. Higher = darker, more crushed shadows.";
> = 0.200000;

uniform float HableWhite <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 3.0; ui_max = 20.0; ui_step = 0.1;
    ui_label = "Hable: White Point";
    ui_tooltip = "Only used while the Output Tonemapper is Uncharted 2 / Hable. The scene light that maps to white.";
> = 11.200000;

uniform float GT7Shoulder <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 0.6; ui_step = 0.005;
    ui_label = "Gran Turismo 7: Shoulder (alpha)";
    ui_tooltip = "Only used while the Output Tonemapper is Gran Turismo 7. How softly the highlights roll off (higher = a harder "
                 "shoulder).";
> = 0.250000;

uniform float GT7LinearSection <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.2; ui_max = 0.7; ui_step = 0.005;
    ui_label = "Gran Turismo 7: Linear Section";
    ui_tooltip = "Only used while the Output Tonemapper is Gran Turismo 7. Where the straight part ends and the shoulder starts, as a "
                 "fraction of the peak. Higher = more of the picture stays linear.";
> = 0.444000;

uniform float GT7Toe <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.8; ui_max = 2.0; ui_step = 0.01;
    ui_label = "Gran Turismo 7: Toe Strength";
    ui_tooltip = "Only used while the Output Tonemapper is Gran Turismo 7. How much the shadows are pushed down (1 = none).";
> = 1.280000;

uniform float UchContrast <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.01;
    ui_label = "Uchimura: Contrast";
    ui_tooltip = "Only used while the Output Tonemapper is Uchimura. Slope of the straight part of the curve.";
> = 1.000000;

uniform float UchLinearStart <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 0.5; ui_step = 0.005;
    ui_label = "Uchimura: Linear Section Start";
    ui_tooltip = "Only used while the Output Tonemapper is Uchimura. Where the toe ends and the straight part begins.";
> = 0.220000;

uniform float UchLinearLength <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 0.8; ui_step = 0.005;
    ui_label = "Uchimura: Linear Section Length";
    ui_tooltip = "Only used while the Output Tonemapper is Uchimura. How long the straight part is before the shoulder.";
> = 0.400000;

uniform float UchToe <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 2.0; ui_step = 0.01;
    ui_label = "Uchimura: Toe (black tightness)";
    ui_tooltip = "Only used while the Output Tonemapper is Uchimura. Higher = darker, tighter shadows.";
> = 1.330000;

uniform int AgXLook <
    ui_category = "Tonemapper Conversion"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Base\0Punchy\0Golden\0";
    ui_label = "AgX: Look";
    ui_tooltip = "Only used while the Output Tonemapper is AgX. Base is the plain curve, Punchy adds contrast and saturation, Golden "
                 "adds a warm tint.";
> = 0;

// ============================================================================
// SECTION: Multibounce
// Light that bounces more than once.
// ============================================================================

uniform bool Multibounce <
    ui_category = "Multibounce"; ui_category_closed = true;
    ui_label = "Multibounce";
    ui_tooltip = "Rays also pick up the GI that last frame put on the surface they hit, in that surface's colour, so light bounces more "
                 "than once and fills rooms, corridors and caves. It settles within a few frames. Off = a single bounce.\n"
                 "Look at it in Debug View > Bounce Only, no sky. The other settings in this section do nothing while it is off.";
> = 1;

uniform float MultibounceAmount <
    ui_category = "Multibounce"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
    ui_label = "Multibounce Strength";
    ui_tooltip = "How much of last frame's GI a hit surface passes on (times its reflectance). 0 = no multibounce. 1 = physical for the "
                 "GI you see (it includes GI Intensity, so raising the intensity raises the bounce too). Above 1 rooms and caves glow. "
                 "It always settles: the loop is held below 0.9 per bounce.\n"
                 "It is scaled down by Relit Pickup Bias, because the relit pickup already passes bounce on.";
> = 1.000000;

uniform int MultibounceFromCascade <
    ui_category = "Multibounce"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1; ui_max = CRCGI_CASCADES;
    ui_label = "Multibounce From Cascade";
    ui_tooltip = "The first cascade whose rays pick up multibounce (1 = all of them). Indoors most bounce light comes from near "
                 "surfaces, so 1 is what makes it visible in rooms. Higher values keep multibounce to long-range light only.";
> = 1;

uniform float MultibounceAlbedo <
    ui_category = "Multibounce"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 0.9; ui_step = 0.01;
    ui_label = "Bounce Reflectance (estimate)";
    ui_tooltip = "How much light an average surface reflects, combined with each surface's own hue, for surfaces where the game's albedo "
                 "is not available. Higher = more bounce, lower = it settles sooner and rooms fill less. Does nothing where Use Game "
                 "Albedo provides the real albedo.";
> = 0.300000;

uniform float MultibounceSpread <
    ui_category = "Multibounce"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.25; ui_max = 4.0; ui_step = 0.01;
    ui_label = "Multibounce Spread";
    ui_tooltip = "How wide the bounce is read around a hit. It follows the width of the ray that found the hit (near hits tight, far "
                 "hits broad). 1 = matched to the ray, lower = sharper, higher = broader and slower to build up.";
> = 1.000000;

uniform float MultibounceSurfaceMatch <
    ui_category = "Multibounce"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Multibounce Surface Match";
    ui_tooltip = "Weights the bounce read by depth and normal agreement with the hit surface, so the wall of a corner is lit by bounce "
                 "and not by blur. 0 = plain read.\n"
                 "Only used with CRCGI_SECONDARY_DIVISOR 0. With the map version (any other value) the bounce is weighted by depth only "
                 "and this does nothing.";
> = 1.000000;

// ============================================================================
// SECTION: Bounce Shaping
// Contrast of the bounce strength.
// ============================================================================

uniform float BounceContrast <
    ui_category = "Bounce Shaping"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.6; ui_max = 2.0;
    ui_label = "Bounce Contrast Curve";
    ui_tooltip = "Evens out or widens the gap between weak and strong bounce, around Bounce Curve Pivot. 1 = off. Below 1 weak bounce is "
                 "lifted and strong bounce reduced (rooms look more even). Above 1 weak gets weaker and strong stronger. The overall "
                 "average stays about the same.";
> = 0.600000;

uniform float BouncePivot <
    ui_category = "Bounce Shaping"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.005; ui_max = 0.3;
    ui_label = "Bounce Curve Pivot";
    ui_tooltip = "The bounce level the contrast curve leaves unchanged. Bounce brighter than this counts as strong, dimmer as weak. Set "
                 "it near your typical bounce level (Debug View > Bounce Only, no sky). Does nothing while Bounce Contrast Curve is 1.";
> = 0.100000;

uniform float BounceMaxGain <
    ui_category = "Bounce Shaping"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 16.0;
    ui_label = "Bounce Curve Max Gain";
    ui_tooltip = "The most the contrast curve may multiply any bounce by. Caps the lift of very weak bounce, which would otherwise "
                 "amplify noise. Only matters while Bounce Contrast Curve is below 1.";
> = 5.000000;

// ============================================================================
// SECTION: Colors & Temperature
// Colour of the bounce light.
// ============================================================================

uniform float SaturationBoost <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.4;
    ui_label = "Bounce Saturation";
    ui_tooltip = "Colour saturation of the bounce light before it is added. 0 = as picked up. Raising it makes coloured surfaces tint "
                 "their surroundings more (colour bleeding). The top of the range is very strong, go in small steps.";
> = 1.000000;

uniform float BlueFilter <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Blue Light Filter";
    ui_tooltip = "Desaturates bounce light whose hue is near Blue Filter Hue (toward grey). 0 = off, 1 = removes the colour completely "
                 "inside the hue band. Use it to stop sky-blue or cold surfaces from tinting everything blue.";
> = 0.000000;

uniform float BlueFilterHue <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Blue Filter Hue";
    ui_tooltip = "The hue the blue filter targets, on the 0-1 colour wheel (0 red, 0.17 yellow, 0.33 green, 0.5 cyan, 0.67 blue, 0.83 "
                 "magenta). Does nothing while Blue Light Filter is 0.";
> = 0.690000;

uniform float BlueFilterWidth <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.02; ui_max = 0.5;
    ui_label = "Blue Filter Hue Width";
    ui_tooltip = "How far around its hue the blue filter reaches (0-0.5). It fades out smoothly toward the edge. Does nothing while Blue "
                 "Light Filter is 0.";
> = 0.180000;

uniform float OrangeFilter <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Orange Light Filter";
    ui_tooltip = "Desaturates bounce light whose hue is near Orange Filter Hue. 0 = off, 1 = full. Use it to calm warm tints from lamps, "
                 "sand or sunsets.";
> = 0.000000;

uniform float OrangeFilterHue <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Orange Filter Hue";
    ui_tooltip = "The hue the orange filter targets (0 red, 0.08 orange, 0.17 yellow). Does nothing while Orange Light Filter is 0.";
> = 0.080000;

uniform float OrangeFilterWidth <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.02; ui_max = 0.5;
    ui_label = "Orange Filter Hue Width";
    ui_tooltip = "How far around its hue the orange filter reaches (0-0.5). Does nothing while Orange Light Filter is 0.";
> = 0.340000;

uniform float GITemperature <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = -1.0; ui_max = 1.0;
    ui_label = "GI Color Temperature";
    ui_tooltip = "Shifts the GI warm (above 0) or cool (below 0). 0 = off. The size of the shift at full value is set by Temperature "
                 "Strength.";
> = 0.000000;

uniform float TemperatureStrength <
    ui_category = "Colors & Temperature"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.3;
    ui_label = "Temperature Strength";
    ui_tooltip = "How big the red / blue shift is at GI Color Temperature +1 or -1. Does nothing while GI Color Temperature is 0.";
> = 0.150000;

// ============================================================================
// SECTION: Sky
// Tintable sky light, gated by the cascades' visibility.
// ============================================================================

uniform bool ProcessSkyColor <
    ui_category = "Sky"; ui_category_closed = true;
    ui_label = "Sky Light";
    ui_tooltip = "Adds a tintable sky ambient to the GI, gated by the cascades' own visibility: probe directions that escape upward "
                 "carry sky light, blocked ones do not. Open areas get sky light, crevices do not, and nothing gets darker. Needs Sky "
                 "Light Strength above 0. Look at Debug View > Escape Visibility.";
> = 0;

uniform int SkyColorSource <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Scene Sky (sampled)\0Custom Color\0Blend Scene Sky + Custom\0";
    ui_label = "Sky Color Source";
    ui_tooltip = "Where the sky tint comes from. Scene Sky: the average hue of the sky pixels found at 6 points near the top of the "
                 "screen. Custom Color: Custom Sky Color. Blend: a mix set by Blend: Custom Amount. With no sky on screen the custom "
                 "colour is used.";
> = 0;

uniform float3 SkyCustomColor <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "color";
    ui_label = "Custom Sky Color";
    ui_tooltip = "Custom sky colour. Only its hue and saturation matter, the brightness of the sky light is set by Sky Light Strength. "
                 "Used by the Custom and Blend sources.";
> = float3(0.950178, 0.635393, 0.074391);

uniform float SkyBlend <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Blend: Custom Amount";
    ui_tooltip = "Only used by 'Blend Scene Sky + Custom'. 0 = the sampled scene sky, 1 = the custom colour.";
> = 0.000000;

uniform float SkySaturation <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0;
    ui_label = "Sky Tint Saturation";
    ui_tooltip = "Saturation of the sky tint. 0 = plain white sky light whatever the source, 1 = the sampled or custom colour as it is, "
                 "above 1 = exaggerated.";
> = 0.000000;

uniform float SkyColorInfluence <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Sky Light Strength";
    ui_tooltip = "Brightness of the sky ambient added to the GI. 0 = off. Small values are enough because it is added on top of the "
                 "bounce; too much flattens the image with a uniform glow.";
> = 0.000000;

uniform float SkyOcclusionContrast <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 4.0;
    ui_label = "Sky Occlusion Contrast";
    ui_tooltip = "How hard the cascades' sky visibility cuts the sky light. 0 = ignores visibility (a flat ambient everywhere). Higher = "
                 "sky light drops off faster as an area becomes enclosed. It only removes sky light, it never darkens the image. Look at "
                 "Debug View > Enclosure.";
> = 0.000000;

uniform float SkyVisibilityNorm <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Sky Fully-Open Threshold";
    ui_tooltip = "Sky visibility at or above this counts as fully open. Lower = more areas count as fully open, higher = only very open "
                 "areas get the full sky light. 0 = any visibility counts as fully open.";
> = 0.000000;

uniform float SkyDirLow <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = -1.0; ui_max = 1.0;
    ui_label = "Sky Direction Start";
    ui_tooltip = "Where a probe direction starts counting as sky-facing, by its upward component (-1 straight down, 0 horizon, 1 "
                 "straight up). Directions below this carry no sky light.";
> = 0.000000;

uniform float SkyDirHigh <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = -1.0; ui_max = 1.0;
    ui_label = "Sky Direction Full";
    ui_tooltip = "The upward component at which a direction counts fully as sky-facing. The weight rises smoothly between Sky Direction "
                 "Start and this value.";
> = 0.750000;

uniform float SkyEdgeFade <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.3;
    ui_label = "Sky Screen-Edge Fade";
    ui_tooltip = "Fades the sky light out within this fraction of the screen border, to hide that visibility is unknown past the edge. 0 "
                 "= no fade.";
> = 0.000000;

uniform bool SkyMapOn <
    ui_category = "Sky"; ui_category_closed = true;
    ui_label = "Directional Sky Map";
    ui_tooltip = "Builds a small map of the sky's colour across the screen, so the sky light carries the sky's gradient (a sunset side, "
                 "a blue zenith) instead of one average tint. It uses Sky Color Source and Sky Tint Saturation like the plain tint. Off "
                 "= one average tint.";
> = 0;

uniform float SkyMapBrightness <
    ui_category = "Sky"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Sky Map Brightness Variation";
    ui_tooltip = "How much of the sky's brightness gradient (not only its colour) reaches the GI. 0 = colour only, 1 = brighter parts of "
                 "the sky give proportionally more light. The average stays at Sky Light Strength either way. Only used with Directional "
                 "Sky Map on.";
> = 0.500000;

// ============================================================================
// SECTION: Off-Screen Canvas & Edges
// What rays see when they leave the screen.
// ============================================================================

uniform float ScreenEdgeExtend <
    ui_category = "Off-Screen Canvas & Edges"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Screen Edge Extension";
    ui_tooltip = "Rays that leave the screen beyond the canvas continue the nearest border pixel and pick up its light times this "
                 "(cascades 1-3 only). Keeps light from vanishing at the borders but cannot invent what is off screen. 0 = off.\n"
                 "It only acts outside the canvas, so with a large CRCGI_CANVAS_MARGIN it matters less.";
> = 0.500000;

uniform bool OffscreenCanvas <
    ui_category = "Off-Screen Canvas & Edges"; ui_category_closed = true;
    ui_label = "Off-Screen Canvas";
    ui_tooltip = "Remembers the light and depth of what recently left the screen in a border around it (CRCGI_CANVAS_MARGIN percent per "
                 "side), moved with the motion vectors. Rays that leave the screen march on through it and pick up light from it.\n"
                 "Look at G-Buffer / Canvas View > Off-Screen Light Only to see what it adds. Canvas Light Strength and Canvas Memory do "
                 "nothing while it is off.";
> = 1;

uniform float CanvasStrength <
    ui_category = "Off-Screen Canvas & Edges"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Canvas Light Strength";
    ui_tooltip = "How much a ray takes from the off-screen canvas compared with an on-screen hit (also scaled by how much that canvas "
                 "texel is still trusted, see Canvas Memory). 0 = canvas light ignored, 1 = like an on-screen hit.";
> = 1.000000;

uniform float CanvasPersistence <
    ui_category = "Off-Screen Canvas & Edges"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.8; ui_max = 0.998; ui_step = 0.001;
    ui_label = "Canvas Memory (per frame)";
    ui_tooltip = "How long light that left the screen is remembered. Each frame the trust in it is multiplied by this (the light keeps "
                 "its full colour and brightness). 0.985 = half trust after about 0.8 s at 60 fps, 0.995 = about 2.3 s. Lower = safer "
                 "when the off-screen scene changes, higher = light stays longer.";
> = 0.985000;

// ============================================================================
// SECTION: Range & Falloff
// How far the GI reaches.
// ============================================================================

uniform float GIDistanceCutoff <
    ui_category = "Range & Falloff"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 1.5;
    ui_label = "Max GI Distance (linear depth)";
    ui_tooltip = "Linear depth where the GI has fully faded out, pixels farther than this get none (look at Debug View > Linear Depth to "
                 "see where your scene falls). Games spread depth differently, so the useful value changes per game. Values above 1 "
                 "never cut anything. Lower it to save GI on far scenery.\n"
                 "Distance Fade Width sets how gradual the fade is. Fade With Receiver Distance also weakens GI with depth, use that one "
                 "for a gentle falloff and this one as a hard limit.";
> = 1.000000;

uniform float GIDistanceFadeWidth <
    ui_category = "Range & Falloff"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.5;
    ui_label = "Distance Fade Width";
    ui_tooltip = "Width of the fade before Max GI Distance, as a fraction of it. 0 = hard cut, 0.5 = fades over the last half. Wider = "
                 "more gradual loss of GI with distance.";
> = 0.370000;

uniform float ProximityCameraDistanceInfluence <
    ui_category = "Range & Falloff"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Fade With Receiver Distance";
    ui_tooltip = "Weakens the GI of surfaces far from the camera, smoothly with depth. 0 = the same strength at every distance, 1 = "
                 "nothing left at the far plane.\n"
                 "It overlaps with Max GI Distance (both fade the GI with depth): use this for a gentle falloff and Max GI Distance as a "
                 "hard limit.";
> = 0.750000;

uniform float HitDistanceFalloff <
    ui_category = "Range & Falloff"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Hit Distance Falloff";
    ui_tooltip = "Weakens light that a ray picks up far away, measured against the farthest cascade's reach, so it looks the same at "
                 "every resolution. 0 = off, which is the physical result because radiance cascades already fade light with distance. "
                 "About 0.3 is gentle, 1 is strong.\n"
                 "Fade With Receiver Distance looks at how far the receiving surface is from the camera, this one at how far the light "
                 "travelled.";
> = 0.000000;

// ============================================================================
// SECTION: Contact Shadowing
// Small-scale contact shadows / crevice light and the albedo response.
// ============================================================================

uniform float CreviceBounceBias <
    ui_category = "Contact Shadowing"; ui_category_closed = true;
    ui_type = "slider"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Crevice Light Bounce Bias";
    ui_tooltip = "What the contact pass does to places that are more enclosed than their surroundings (crevices, joints, corners, the "
                 "foot of an object). 0 = off and the pass is skipped (it costs nothing). Below 0 it acts like ambient occlusion: those "
                 "places get less light (-1 strongest). Above 0 they get more bounce light, as if the walls around them filled them with "
                 "light (1 strongest).\n"
                 "The pass always runs at full resolution, whatever CRCGI_RESOLUTION_DIVISOR is. Look at Debug View > Contact Occlusion.";
> = 0.000000;

uniform float ContactRangeTexels <
    ui_category = "Contact Shadowing"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.5; ui_max = 6.0; ui_step = 0.05;
    ui_label = "Contact Range (texels)";
    ui_tooltip = "How far the contact pass looks from every pixel, and the size of the area it compares with, in effect texels (so a "
                 "preset looks the same at every screen resolution). Small = tight joints and cracks only. Larger = shadows at the foot "
                 "of objects. Cost does not depend on it. Does nothing while Crevice Light Bounce Bias is 0.";
> = 2.000000;

uniform float ContactBiasTexels <
    ui_category = "Contact Shadowing"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.5; ui_step = 0.01;
    ui_label = "Contact Depth Bias (texels)";
    ui_tooltip = "How far behind the depth buffer a contact ray must be to count as blocked. Too low: flat surfaces shadow themselves "
                 "(speckle, stripes on slopes). Too high: shallow crevices vanish. Raise it if flat walls look dirty in Debug View > "
                 "Contact Occlusion. Does nothing while Crevice Light Bounce Bias is 0.";
> = 0.300000;

uniform float AlbedoResponse <
    ui_category = "Contact Shadowing"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Surface Albedo Response";
    ui_tooltip = "Makes the GI follow the surface's own colour. 0 = the GI is added as plain light. Higher = darker parts of a surface "
                 "(mortar, dirt, seams) receive less GI and coloured surfaces take coloured light.\n"
                 "Without game data the albedo is estimated from the image (colour relative to the surrounding surface). With Use Game "
                 "Albedo the real albedo is used (0.5 = mid reflectance = unchanged). Works with or without Crevice Light Bounce Bias.";
> = 0.250000;

// ============================================================================
// SECTION: Temporal Accumulation
// Blending with previous frames; far-cascade accumulation and jitter.
// ============================================================================

uniform bool TemporalOn <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_label = "Temporal Accumulation";
    ui_tooltip = "Blends the GI (lighting only, never the game image) with previous frames, moved with the motion and rejected where "
                 "depth or light disagree. Off = every frame stands alone and the GI is noisy. The cascades from Far Cascades: "
                 "Accumulate From Cascade onwards also keep their own longer history.\n"
                 "The other settings in this section do nothing while it is off.";
> = 1;

uniform float TemporalFrames <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 64.0; ui_step = 0.5;
    ui_label = "Frames Accumulated";
    ui_tooltip = "About how many frames the GI history represents. 1 = no accumulation. Higher = steadier light but longer trails when "
                 "the light changes. Far cascades accumulate more on top of this (see Far Cascades: Accumulation Growth), up to 64 "
                 "frames.";
> = 16.000000;

uniform float TemporalLightGuard <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Light Change Guard";
    ui_tooltip = "Drops the history where the light changed. A relative brightness difference of this value between this frame and the "
                 "history gives no blend at all, half of it gives half the blend. Lower = stricter (less ghosting, less smoothing).";
> = 1.000000;

uniform float TemporalDepthGuard <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.005; ui_max = 1.0; ui_step = 0.005;
    ui_label = "Depth Change Guard";
    ui_tooltip = "Drops the history where the surface moved: a relative depth difference of this value gives no blend. Stops light "
                 "trailing behind moving objects and in newly revealed areas. Lower = stricter.";
> = 0.600000;

uniform float MotionRejection <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 200.0; ui_step = 1.0;
    ui_label = "Motion Mismatch Rejection";
    ui_tooltip = "How much the history is distrusted where the estimated motion could not find a good match (fast motion, moving "
                 "objects). Higher = shorter trails, 0 = trust the motion everywhere.\n"
                 "With Use Game Motion Vectors the motion is exact and this does nothing.";
> = 50.000000;

uniform float TemporalJitter <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Ray Jitter Accumulation";
    ui_tooltip = "Rotates every ray's start position and direction a little each frame and lets the history average them, so thin or "
                 "sparse geometry converges instead of sliding through a fixed pattern when the camera moves. Higher = smoother after a "
                 "moment, a little noisier right after fast motion. 0 = fixed pattern.\n"
                 "Only works with Temporal Accumulation on.";
> = 1.000000;

uniform int FarAccumStart <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 2; ui_max = CRCGI_CASCADES;
    ui_label = "Far Cascades: Accumulate From Cascade";
    ui_tooltip = "Cascades from this one onwards (1 = nearest) keep their own history, are jittered every frame, and accumulate more the "
                 "farther they are. Far cascades have few, long rays, so their noise is large and static and cannot be filtered "
                 "spatially, accumulating jittered samples averages it out. Lower = steadier near-mid range, slower to react.";
> = 2;

uniform float FarAccumGrowth <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 3.0; ui_step = 0.05;
    ui_label = "Far Cascades: Accumulation Growth";
    ui_tooltip = "Each cascade from Far Cascades: Accumulate From Cascade onwards accumulates Frames Accumulated times this, once more "
                 "for every step past the start cascade, capped at 64 frames. 1 = the same as the rest of the GI. Higher = steadier far "
                 "light that reacts more slowly to lighting changes.";
> = 2.300000;

uniform float FarJitter <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Far Cascades: Jitter";
    ui_tooltip = "Moves the far cascades' probes within their tile, and their rays within their direction cell, a little every frame, so "
                 "the accumulated history covers the whole tile instead of one fixed point. This is what removes the static noise of the "
                 "far cascades. 0 = fixed pattern (the static noise stays).";
> = 1.000000;

uniform bool FarHalfRate <
    ui_category = "Temporal Accumulation"; ui_category_closed = true;
    ui_label = "Far Cascades: Half-Rate Update (faster)";
    ui_tooltip = "Performance. The accumulated far cascades trace half of their probes each frame (checkerboard) and reuse the "
                 "reprojected history for the rest, which roughly halves the cost of those cascades (the most expensive ones). Slightly "
                 "slower to react to lighting changes.";
> = false;

// ============================================================================
// SECTION: Denoise & Upsample
// Spatial smoothing at effect resolution and the edge-aware upsample to full resolution.
// ============================================================================

uniform int DenoiseMode <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Edge-Aware\0Off\0";
    ui_label = "Denoise Mode";
    ui_tooltip = "Runs at the effect's own resolution, before the upsample to full resolution (temporal accumulation smooths further on "
                 "top). Edge-Aware: blurs only along the receiver's surface, never across depth edges. Off: the raw gathered GI. The "
                 "other denoise settings do nothing while it is off. Look at Debug View > GI Before Denoise to see the raw light.";
> = 1;

uniform int DenoiseRadius <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1; ui_max = 8;
    ui_label = "Denoise Kernel Radius (cascade texels)";
    ui_tooltip = "Taps on each side of a texel, in effect texels, applied horizontally then vertically. Larger = smoother GI, softer "
                 "detail, more cost.";
> = 3;

uniform float DenoiseStep <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.5; ui_max = 3.0;
    ui_label = "Denoise Tap Spacing (texels)";
    ui_tooltip = "Distance between taps, in effect texels, before the distance shrink. Above 1 the taps skip texels, so a wide area is "
                 "covered with few taps (cheap, but can leave faint patterns). 1 = contiguous taps.";
> = 1.280000;

uniform float DenoiseVarianceGuide <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Variance Guided Denoise";
    ui_tooltip = "Estimates how noisy each texel is from its 3 x 3 neighbourhood and lets that steer the filter: in noisy regions it "
                 "keeps its full width, in clean regions it stays tight, so real lighting edges (a shadow boundary, a bright patch) "
                 "survive. A single very bright texel (a firefly) is clamped to its neighbourhood. 0 = the plain depth and normal guided "
                 "filter.";
> = 1.000000;

uniform float DenoiseDepthScale <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 20.0;
    ui_label = "Denoise Distance Shrink";
    ui_tooltip = "Makes the blur smaller on distant surfaces (fewer pixels cover them, so a smaller kernel is enough). 0 = no shrink. "
                 "Too high under-filters the distance.";
> = 3.970000;

uniform int DenoiseDepthMode <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Surface plane\0Relative depth\0Absolute depth\0";
    ui_label = "Denoise Edge Test";
    ui_tooltip = "How a tap is judged to be on the same surface as the pixel.\n"
                 "Surface plane: distance from the pixel's tangent plane (grazing floors stay smooth, objects in front or behind are "
                 "rejected).\n"
                 "Relative depth: depth difference relative to distance.\n"
                 "Absolute depth: plain depth difference (strict near, loose far).";
> = 0;

uniform float DenoiseDepthSharpness <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 8.0;
    ui_label = "Denoise Edge Sharpness";
    ui_tooltip = "Strength of the edge test chosen in Denoise Edge Test. Higher = stricter (stops at smaller depth steps, more residual "
                 "noise near edges). Lower = more blur across edges.";
> = 0.490000;

uniform float DenoiseNormalPower <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.5; ui_max = 16.0;
    ui_label = "Denoise Normal Power";
    ui_tooltip = "Taps count by their normal agreement raised to this. Higher = the blur stops at creases and corners. Lower = it blurs "
                 "across them.";
> = 1.735000;

uniform int UpsampleMode <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Edge-aware, surface plane\0Edge-aware, relative depth\0Bilinear\0";
    ui_label = "GI Upsample";
    ui_tooltip = "How the denoised GI reaches full resolution. Edge-aware: each pixel only takes light from effect texels on its own "
                 "surface, and a thin feature that matches none takes the closest in depth instead of a blend of other surfaces. Surface "
                 "plane judges by distance from the pixel's tangent plane, relative depth by the depth gap. Bilinear: plain filtering "
                 "(cheapest, halos at depth edges).";
> = 0;

uniform float UpsampleDepthSharpness <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 8.0;
    ui_label = "Upsample Edge Sharpness";
    ui_tooltip = "How strictly the edge-aware upsample sticks to the pixel's own surface. Higher = crisper edges, but thin features may "
                 "pick a single tap. Lower = softer, more halo risk. Does nothing with Bilinear.";
> = 2.000000;

uniform float UpsampleNormalPower <
    ui_category = "Denoise & Upsample"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.5; ui_max = 16.0;
    ui_label = "Upsample Normal Power";
    ui_tooltip = "Upsample taps count by their normal agreement raised to this. Higher = sharper at creases. Does nothing with Bilinear.";
> = 2.720000;

// ============================================================================
// SECTION: Expert: Light Pickup
// How a ray turns what it hits into light.
// ============================================================================

uniform float PreFilterGamma <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.6; ui_max = 1.6;
    ui_label = "Pre-Filter Gamma";
    ui_tooltip = "Gamma on the picked-up light. Above 1 favours the brighter parts of the image, below 1 evens lit and dim surfaces out. "
                 "It also changes which surfaces reach the Emitter Threshold.";
> = 1.000000;

uniform float PickupBlurTexels <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 3.0; ui_step = 0.01;
    ui_label = "Pickup Blur (texels)";
    ui_tooltip = "The image rays pick light from is softened once per frame over this radius, in effect texels. Larger = steadier bounce "
                 "and less sparkle from small bright details. Smaller = sharper but noisier. 0 = off.";
> = 0.355000;

uniform float SurfaceBounceThreshold <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.3; ui_step = 0.001;
    ui_label = "Surface Bounce Threshold";
    ui_tooltip = "The surface brightness at which a surface counts for half as a source of bounce light. Dimmer surfaces count less and "
                 "brighter ones more, smoothly, with no cutoff. Raise it to keep shadowed and dark surfaces from lighting their "
                 "surroundings. Debug View > Emitters shows what is sampled (grey = ordinary surfaces, black = nothing sampled).";
> = 0.080000;

uniform float EmitterThreshold <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 1.0;
    ui_label = "Emitter Threshold";
    ui_tooltip = "Brightness above which a surface counts as a light source (an emitter) and gets Emitter Boost. Look at Debug View > "
                 "Emitters: orange pixels are what the shader treats as emitters. Raise the threshold until only real lamps, fire and "
                 "sunlit spots are orange, lower it if lamps are not.";
> = 0.400000;

uniform float EmitterSensitivity <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 3.0;
    ui_label = "Emitter Sensitivity (transition sharpness)";
    ui_tooltip = "How abrupt the step from ordinary surface to emitter is around Emitter Threshold. Higher = a sharp edge, lower = a "
                 "gradual one (the transition is Emitter Threshold divided by this wide). It shows as the softness of the orange in "
                 "Debug View > Emitters.\n"
                 "With Emitter Boost at 1 it only decides which surfaces keep their image colour for Relit Pickup Bias.";
> = 0.100000;

uniform float EmitterBoost <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 30.0;
    ui_label = "Emitter Boost";
    ui_tooltip = "Multiplier on the light of emitters. 1 = lamps bounce like any lit wall. Higher = lamps and bright spots throw more "
                 "light (and fireflies if too high). It multiplies on top of Pickup Brightness Cap, so a low cap does not limit it. See "
                 "also HDR Light Pickup.";
> = 1.000000;

uniform float MaxSampleLuminance <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 4.0;
    ui_label = "Pickup Brightness Cap (fireflies)";
    ui_tooltip = "The most light one surface may hand to a ray, before Emitter Boost and HDR Light Pickup multiply it. Low = lamps and "
                 "lit walls bounce about equally and there are no fireflies. Higher = brighter surfaces throw proportionally more light, "
                 "and small bright lights give fireflies. In Debug View > Hit Radiance Buffer every surface that reaches this cap looks "
                 "equally bright.";
> = 0.080000;

uniform float DarkSurfacesBlock <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 3.0; ui_step = 0.01;
    ui_label = "Dark Surfaces Block Rays";
    ui_tooltip = "Makes dark surfaces shadow casters: the darker a surface, the thicker its hit band, so rays that pass behind a dark "
                 "object (armour, rock faces, shadowed bark) are blocked by it instead of picking up the bright light behind it. Dark "
                 "objects then darken their surroundings. 0 = off, higher = stronger.\n"
                 "What counts as dark is set by Surface Bounce Threshold. The thicker band is still limited by Max Hit Thickness.";
> = 0.500000;

uniform float RelitBias <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Relit Pickup Bias (geometry + lights + bounce)";
    ui_tooltip = "The game's image is lit by the game (its baked ambient, shadow maps and light probes). At 0 a ray that hits a surface "
                 "picks up exactly that. Raising it lights the surface again from what this effect knows: its reflectance (its colour "
                 "with the large-scale lighting taken out) times the light that last frame's GI says reaches that spot. A surface the "
                 "game left flat-lit but that is walled in now bounces what reaches it, and a surface the game lit from a probe that is "
                 "not there stops glowing.\n"
                 "Light sources and bright, directly lit surfaces keep their image colour (see Relit Highlight Keep). 0 = image only, 1 "
                 "= fully relit. It reduces Multibounce Strength by the same share, because the relit pickup already passes bounce on.";
> = 0.150000;

uniform float RelitReflectance <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Relit Reflectance";
    ui_tooltip = "How much of the light that reaches a relit surface it hands on. Each bounce is this much weaker than the last, so "
                 "lower values settle faster and keep rooms from filling with light, and higher values carry bounce farther. Does "
                 "nothing while Relit Pickup Bias is 0.";
> = 0.600000;

uniform float RelitDetail <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Relit Albedo Detail";
    ui_tooltip = "How much of a pixel's reflectance detail is kept (its brightness relative to the average around it, which divides out "
                 "slow baked lighting). 0 = every surface reflects the same, only its hue is kept. 1 = full detail (the edge of a baked "
                 "shadow may read as dark texture).\n"
                 "Does nothing while Relit Pickup Bias is 0, or where Use Game Albedo provides the real albedo.";
> = 0.500000;

uniform float RelitDetailRadius <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 12.0; ui_step = 0.5;
    ui_label = "Relit Detail Radius (cascade texels)";
    ui_tooltip = "How far around a pixel the baked lighting is assumed to be constant, in effect texels. Larger = broader shadows and "
                 "light pools are removed from the reflectance, fine texture blurs into them. Does nothing while Relit Pickup Bias is 0, "
                 "or with Use Game Albedo.";
> = 9.000000;

uniform float RelitKeepBright <
    ui_category = "Expert: Light Pickup"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Relit Highlight Keep";
    ui_tooltip = "Bright surfaces are mostly directly lit (sun, lamps), and direct light is in the image but not in the GI, so they keep "
                 "their image colour instead of being relit. 1 = surfaces above about 0.25-0.7 of display brightness are never relit. 0 "
                 "= only emitters are kept. Does nothing while Relit Pickup Bias is 0.";
> = 0.600000;

// ============================================================================
// SECTION: Expert: Radiance Cascades
// Cascade layout, rays and steps.
// ============================================================================

uniform int RaysPerCell <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1; ui_max = 4;
    ui_label = "Rays Per Cell (quality)";
    ui_tooltip = "Each probe direction traces this many stratified sub-rays and averages them. More = smoother, steadier cascades (the "
                 "temporal history smooths further). Cost grows linearly.";
> = 3;

uniform int StepsNear <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 3; ui_max = 12;
    ui_label = "Ray Steps: Cascades 1-2 (near)";
    ui_tooltip = "Samples per ray in cascades 1 and 2 (the short ones). Each ray only covers its own short interval, so a few are "
                 "enough. Raise it if thin geometry close to surfaces is missed. Cost grows linearly.";
> = 4;

uniform int StepsFar <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 3; ui_max = 12;
    ui_label = "Ray Steps: Cascades 3+ (far)";
    ui_tooltip = "Samples per ray in cascades 3 and up (the long ones). Long intervals need more steps to not skip thin geometry. Cost "
                 "grows linearly, and about halves with Far Cascades: Half-Rate Update.";
> = 10;

uniform float CascadeReach <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 1.5; ui_step = 0.01;
    ui_label = "Cascade Reach (screen diagonals)";
    ui_tooltip = "How far the farthest cascade reaches, as a fraction of the screen diagonal (so it is resolution independent). The "
                 "cascades from Reach Applies From Cascade onwards are spread evenly between the end of the cascade before them and this "
                 "distance, the earlier cascades keep their natural layout. Lower = the far cascades look at nearer geometry with finer "
                 "steps (steadier). Higher = light from farther across the screen. Look at Show Single Cascade.";
> = 0.300000;

uniform int ReachStartCascade <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 2; ui_max = CRCGI_CASCADES;
    ui_label = "Reach Applies From Cascade";
    ui_tooltip = "The first cascade (1 = nearest) that Cascade Reach stretches. Cascades before it keep the natural layout where each "
                 "interval is 4 times the last. Lower = more cascades share the reach (each shorter and denser). Higher = only the last "
                 "ones are stretched.";
> = 2;

uniform float CascadeOverlap <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.01;
    ui_label = "Cascade Cross-Fade";
    ui_tooltip = "Each cascade's rays continue this fraction into the next cascade's interval and fade out there while the next one "
                 "fades in. Removes rings at fixed distances from edges. Costs that fraction of extra steps. 0 = off.";
> = 0.250000;

uniform int AdaptiveRays <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Off\0Boost new areas\0Boost new areas, save on stable ones\0";
    ui_label = "Adaptive Ray Count";
    ui_tooltip = "Uses the temporal history to decide how many rays a probe needs. Where the history was just lost (an area that was "
                 "hidden last frame, fast motion) the probe traces Adaptive: Extra Rays more, so newly revealed areas clean up faster. "
                 "'Save' also traces one ray fewer where the history is valid (faster, relies on the history to average).\n"
                 "Needs Temporal Accumulation on. Off = Rays Per Cell everywhere.";
> = 2;

uniform int AdaptiveRaysExtra <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1; ui_max = 3;
    ui_label = "Adaptive: Extra Rays";
    ui_tooltip = "Rays added (up to 4 per cell) where the history was lost. Does nothing while Adaptive Ray Count is Off.";
> = 2;

uniform float StartJitterAmount <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Ray Start Jitter (draw distance vs noise)";
    ui_tooltip = "Each ray's first sample sits at a fixed per-ray offset, so neighbouring rays probe different spots and thin or sparse "
                 "far geometry is found on average. 1 = full offset (most coverage, some pattern noise). 0 = every ray samples mid-step "
                 "(no pattern noise, misses more). The pattern is fixed per pixel, so it never flickers.";
> = 1.000000;

uniform float ConeFootprint <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0;
    ui_label = "Radiance Cone Blur";
    ui_tooltip = "Rays sample a blurred copy of the light at a level that matches how wide their cone is at the hit. Softens speckle "
                 "from small bright lights. Higher = blurrier distant light. 0 = off. Max Cone Blur Mip limits how blurry it may get.";
> = 0.000000;

uniform float MaxConeMip <
    ui_category = "Expert: Radiance Cascades"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 5.0;
    ui_label = "Max Cone Blur Mip";
    ui_tooltip = "The blurriest level Radiance Cone Blur may use. Lower keeps far light sharper (and noisier). Does nothing while "
                 "Radiance Cone Blur is 0.";
> = 5.000000;

// ============================================================================
// SECTION: Expert: Merge & Gather
// How cascades are merged and read.
// ============================================================================

uniform float GatherSmoothing <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Gather Smoothing (quality)";
    ui_tooltip = "How the final per-pixel GI reads the cascade probes. 0 = bilinear over 4 probes. Above 0 = a wider tent over 16 "
                 "probes: smoother GI and less blockiness on the probe grid (still edge-aware). Costs a few more fetches per pixel.";
> = 1.000000;

uniform bool BilinearDirectionMerge <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_label = "Bilinear Direction Merge (quality)";
    ui_tooltip = "When a cascade is merged into the one below, the parent's light for a direction is read with bilinear filtering "
                 "between neighbouring direction cells instead of snapping to one cell. Smoother directions at no extra fetches. Leave "
                 "it on.";
> = 1;

uniform float ProbeDepthSharpness <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 2.0; ui_max = 64.0;
    ui_label = "Probe Edge Sharpness";
    ui_tooltip = "How strictly probes interpolate only with probes on the same surface (used by the gather and by cascade merging). "
                 "Higher = crisper silhouettes and less light leaking across edges. Lower = smoother but more bleed.";
> = 10.000000;

uniform int ProbeWeightMode <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Relative depth + normal\0Surface plane distance\0";
    ui_label = "Probe Edge Weighting";
    ui_tooltip = "How the merge, gather and structure lookup decide that a neighbouring probe lies on the same surface. Relative depth + "
                 "normal: depth gap relative to distance times normal agreement. Surface plane distance: distance from the receiver's "
                 "tangent plane, which keeps floors and walls seen at grazing angles smooth. Probe Edge Sharpness scales both.";
> = 0;

uniform float ProbeNormalPower <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.5; ui_max = 16.0;
    ui_label = "Probe Normal Agreement Power";
    ui_tooltip = "Only used with Relative depth + normal weighting. Probes count by their normal agreement raised to this. Higher = a "
                 "probe on a differently facing surface counts for much less (sharper creases).";
> = 5.000000;

uniform float MergeSharpnessFalloff <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0;
    ui_label = "Merge Sharpness Falloff";
    ui_tooltip = "Makes the merge blend more freely in the coarse far cascades than in the fine near ones: Probe Edge Sharpness is "
                 "divided by 1 plus this times the cascade number. 0 = the same sharpness at every cascade.";
> = 0.000000;

uniform float MergeHorizon <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.5;
    ui_label = "Merge Horizon Cutoff";
    ui_tooltip = "A parent probe is skipped for a direction that lies closer than this to its horizon. Avoids reading grazing directions "
                 "the parent's surface cannot see. 0 = off.";
> = 0.000000;

uniform float MergeEnergyCarry <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.5; ui_max = 1.6; ui_step = 0.005;
    ui_label = "Energy Carry (per cascade step)";
    ui_tooltip = "How much of the light a farther cascade hands down survives the merge. Each merge multiplies it by this, once per 4x "
                 "step in distance, so it compounds from the far cascades toward the near ones. 1 = unchanged. Above 1 makes up the "
                 "energy far cascades lose (missed thin geometry, interpolation, depth rejection). Below 1 favours near light. Only "
                 "light is scaled, never visibility.\n"
                 "Energy Carry From Cascade limits which merges it applies to.";
> = 1.000000;

uniform int EnergyCarryStart <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 2; ui_max = CRCGI_CASCADES;
    ui_label = "Energy Carry From Cascade";
    ui_tooltip = "Energy Carry applies to the light handed down by this cascade and every farther one (1 = nearest). 2 = every merge. "
                 "Higher = only the far cascades are boosted and near light stays as traced. Does nothing while Energy Carry is 1.";
> = 2;

uniform float MergeVisibility <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Merge Visibility Check";
    ui_tooltip = "When a cascade is merged into the one below, the straight line from the child probe to each parent probe is checked "
                 "against the depth buffer (3 taps). A parent hidden behind something (a pillar, a wall edge) is down-weighted by this "
                 "much, so light does not leak around corners. 0 = off.";
> = 1.000000;

uniform float MergeVisibilityTolerance <
    ui_category = "Expert: Merge & Gather"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.005; ui_max = 0.3; ui_step = 0.005;
    ui_label = "Merge Visibility Tolerance";
    ui_tooltip = "How far, relative to its depth, a surface must stick out in front of the line between two probes to count as blocking "
                 "it. Higher = only clear occluders count (fewer false blocks on bumpy surfaces). Lower = stricter. Does nothing while "
                 "Merge Visibility Check is 0.";
> = 0.060000;

// ============================================================================
// SECTION: Expert: Occlusion & Depth Separation
// What blocks rays and which surfaces exchange light.
// ============================================================================

uniform float DepthSeparationLimit <
    ui_category = "Expert: Occlusion & Depth Separation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 20.0;
    ui_label = "Depth Separation Limit";
    ui_tooltip = "A surface only receives bounce from something close to it in depth, measured relative to distance. At this value the "
                 "bounce has faded to zero, so a character or pole far in front of a wall no longer tints it. Lower = stricter. 20 = "
                 "effectively off.\n"
                 "Long draw distances (a road running to the horizon) need a high value. See also Far Cascade Separation Relax and "
                 "Foreground-to-Background Block.";
> = 0.050000;

uniform float DepthSeparationSoftness <
    ui_category = "Expert: Occlusion & Depth Separation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 1.0;
    ui_label = "Depth Separation Softness";
    ui_tooltip = "How gradually the bounce fades out before the Depth Separation Limit. 1 = it starts fading at 0. A small value = full "
                 "strength, then a sharp drop near the limit.";
> = 0.050000;

uniform float ForegroundBleedBlock <
    ui_category = "Expert: Occlusion & Depth Separation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Foreground-to-Background Block";
    ui_tooltip = "Extra strictness when the light comes from something closer to the camera than the surface receiving it. 0 = the same "
                 "limit both ways, 1 = the limit shrinks to 40% for foreground emitters. Stops foreground objects from painting the "
                 "background.";
> = 0.000000;

uniform float DepthSeparationFarRelax <
    ui_category = "Expert: Occlusion & Depth Separation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 4.0; ui_step = 0.01;
    ui_label = "Far Cascade Separation Relax";
    ui_tooltip = "Light that travels far across the screen usually comes from a surface at a different depth (the far end of a street or "
                 "track), so the Depth Separation Limit rejects far hits much more often than near ones and the far cascades lose "
                 "energy. This raises the limit for the far cascades: it is multiplied by 1 plus this times the cascade number. 0 = the "
                 "same limit in every cascade.";
> = 0.000000;

uniform float OcclusionTolerance <
    ui_category = "Expert: Occlusion & Depth Separation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.0005;
    ui_label = "Occlusion Tolerance";
    ui_tooltip = "A surface can only block a ray for a receiver if it is nearer than the receiver by at least this much (linear depth), "
                 "so the receiver's own plane and small bumps never shadow it. Raise it to stop self-shadow speckle on flat surfaces, "
                 "lower it for tighter contact shadows. 0 = anything nearer can block.\n"
                 "Which surfaces may block is decided here and by Occluder Depth Range. How far behind a surface a ray must be is "
                 "decided by Occlusion Depth Threshold.";
> = 0.000000;

uniform float OccluderDepthRange <
    ui_category = "Expert: Occlusion & Depth Separation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.1; ui_max = 10.0; ui_step = 0.01;
    ui_label = "Occluder Depth Range";
    ui_tooltip = "How far in front of the receiver a blocker may be, relative to the receiver's depth. Something much nearer than that "
                 "(a character in front of a wall) only overlaps on screen and blocks nothing. Above 1 anything in front can block. "
                 "Lower = only close blockers count.";
> = 0.100000;

uniform float DepthTolScaling <
    ui_category = "Expert: Occlusion & Depth Separation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Relative Depth Tolerances (long draw distance)";
    ui_tooltip = "Fixes depth checks on games with a very long far plane (racing, flight, open worlds). Occlusion Tolerance, Occlusion "
                 "Depth Threshold, Ray Depth Drift Limit and the small offsets in the depth comparisons are written in absolute linear "
                 "depth. When the whole area around the camera sits at depth 0.001-0.05 those values are bigger than the geometry there: "
                 "nothing blocks, nothing is separated, and everything near the car looks flat.\n"
                 "1 = every one of them is scaled by the depth of the surface being tested (relative to Tolerance Reference Depth), so "
                 "they hold at any distance. 0 = absolute units.";
> = 0.000000;

uniform float DepthTolReference <
    ui_category = "Expert: Occlusion & Depth Separation"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.001; ui_max = 1.0; ui_step = 0.001;
    ui_label = "Tolerance Reference Depth";
    ui_tooltip = "The linear depth at which the depth checks keep exactly the value on their own sliders while Relative Depth Tolerances "
                 "is above 0. Nearer surfaces get proportionally smaller tolerances, farther ones larger. Set it to the depth where your "
                 "slider values already look right (the typical depth around the car or the camera). Does nothing while Relative Depth "
                 "Tolerances is 0.";
> = 0.001000;

// ============================================================================
// SECTION: Expert: Hit Detection
// When a ray counts as a hit: thickness, facing, ray depth model.
// ============================================================================

uniform int HitThicknessMode <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Generic thickness (original)\0Silhouette-derived thickness\0";
    ui_label = "Hit Thickness Mode";
    ui_tooltip = "How thick the shader assumes each visible surface is. A ray hits a surface when its depth falls inside that surface's "
                 "hit band (from just in front of the surface to its thickness behind it), because a depth buffer only knows the front.\n"
                 "Generic thickness: every surface gets the same allowance, Object Thickness Cap.\n"
                 "Silhouette-derived thickness: the thickness of each object is measured from the depth buffer (it searches for the "
                 "object's two opposite edges), so thin things (poles, railings, foliage) get a thin band and big things a thick one. If "
                 "no pair of edges is found it falls back to the generic allowance.\n"
                 "Thickness Search Range, Thickness Silhouette Gap and Object Thickness Scale only act in Silhouette-derived mode. Debug "
                 "View > Object Thickness shows the measurement in either mode (red = thin). Only the fine cascades use the measurement, "
                 "the farthest cascades use the depth pyramid's range instead.";
> = 1;

uniform int SilhouetteSearchSteps <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1; ui_max = 6;
    ui_label = "Thickness Search Range";
    ui_tooltip = "Only used with Silhouette-derived thickness. How far the shader searches for an object's edges in each of four "
                 "directions: values 1 to 6 search up to 1, 2, 4, 8, 16 or 32 full-resolution pixels. Wider objects need a longer "
                 "search, which costs more depth reads. An object wider than the range is not measured and gets the generic allowance "
                 "instead.";
> = 1;

uniform float SilhouetteDepthGap <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.005; ui_max = 0.2;
    ui_label = "Thickness Silhouette Gap";
    ui_tooltip = "Only used with Silhouette-derived thickness. How far behind the object (relative to its depth, and to its own surface "
                 "plane) a pixel must be to count as the object's edge. Lower finds subtler edges but starts mistaking steep slopes and "
                 "depth noise for edges. Higher only finds clear edges.";
> = 0.040000;

uniform float ThinObjectScale <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.25; ui_max = 8.0;
    ui_label = "Object Thickness Scale";
    ui_tooltip = "Only used with Silhouette-derived thickness. Multiplies the measured thickness. Higher = thin objects get a wider hit "
                 "band (rays hit them more easily). Lower = rays pass behind them more easily.\n"
                 "The result is still limited by Object Thickness Cap, so once the measured thickness times this reaches the cap, "
                 "raising it further changes nothing.";
> = 0.425000;

uniform float ObjectThickness <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.005; ui_max = 1.0;
    ui_label = "Object Thickness Cap";
    ui_tooltip = "The thickness a surface is given, as a fraction of its own depth. Generic mode: every surface gets exactly this. "
                 "Silhouette mode: the upper limit of the measured thickness, and the fallback when no edges are found. The ray's own "
                 "step and the surface slope add on top of it, and Max Hit Thickness limits the total.\n"
                 "Raise it if rays pass through thin objects and miss them. Too high and rays hit surfaces from far behind them (dark "
                 "halos). If it is equal to or above Max Hit Thickness, Max Hit Thickness decides and this changes nothing.";
> = 0.020000;

uniform float HitFrontTolerance <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Hit Front Tolerance";
    ui_tooltip = "How far IN FRONT of a surface, as a fraction of its hit band, a ray may stop and still count as a hit. 0 = the ray "
                 "must be inside the band. Raise it if rays miss grazing surfaces (floors seen at a low angle) because they stop just "
                 "short.";
> = 0.070000;

uniform float HitThicknessCap <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.05; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Max Hit Thickness (fraction of depth)";
    ui_tooltip = "The most any hit band may be thick, as a fraction of the surface's own depth, whatever it is made of (object "
                 "thickness, the ray's step, the surface slope). It matters for the long steps of the far cascades and for anything "
                 "close to the camera, where the band can grow bigger than the surface's own depth and rays hit the character or the "
                 "grass next to the camera from far behind, which darkens everything around the player and makes the far cascades "
                 "splotchy. Lower = stricter. 1 = effectively off.\n"
                 "If Object Thickness Cap plus the ray step is already below this, this changes nothing.";
> = 0.350000;

uniform float ThicknessThreshold <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 5.0;
    ui_label = "Occlusion Depth Threshold";
    ui_tooltip = "Decides whether a ray that has gone behind a nearer surface is blocked by it (a shadow) or just passes behind it (a "
                 "thin object). The ray must be this much farther behind in linear depth (or farther than the hit band, whichever is "
                 "larger) to be blocked. 0 = everything behind a surface is blocked (strongest contact shadows). Higher = rays pass "
                 "behind more (less shadow, more light from behind objects).\n"
                 "It is in linear depth units, so the value depends on how the game spreads depth (see Relative Depth Tolerances for "
                 "long draw distances). If it is smaller than the hit band, the band decides and this changes nothing. Occlusion "
                 "Tolerance and Occluder Depth Range decide which surfaces may block at all.";
> = 0.500000;

uniform float FacingCheckIntensity <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Facing Check Intensity";
    ui_tooltip = "How much a surface that faces away from the ray is dimmed. 0 = off (back-facing hits give full light). 1 = fully "
                 "respected: the back of a lit surface bounces little light. Very thin objects fade the check out, because their normals "
                 "are unreliable. Facing Check Steepness and Facing Check Bias do nothing at 0.";
> = 1.000000;

uniform float FacingCheckSlope <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.25; ui_max = 8.0;
    ui_label = "Facing Check Steepness";
    ui_tooltip = "How sharply a hit switches between front-facing and back-facing. The facing weight is the dot product of the hit "
                 "normal and the ray, times this, plus Facing Check Bias, limited to 0-1. Higher = a sharper switch.";
> = 0.870000;

uniform float FacingCheckBias <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = -1.0; ui_max = 1.0;
    ui_label = "Facing Check Bias";
    ui_tooltip = "How much light a surface seen exactly edge-on still gives (the added term of the facing weight, see Facing Check "
                 "Steepness). Raise it so grazing hits still bounce, lower it to cut them.";
> = 0.500000;

uniform int RayDepthModel <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Slope heuristic\0Perspective-correct\0";
    ui_label = "Ray Depth Model";
    ui_tooltip = "Where in depth a ray is when it reaches a point on screen.\n"
                 "Slope heuristic: a linear guess, set by Ray Depth Slope (per texel) and limited by Ray Depth Drift Limit.\n"
                 "Perspective-correct: the real depth of the 3D ray there, from the Field of View. It needs a correct Vertical Field of "
                 "View, and rays start lifted off the surface by Perspective Ray Lift. Ray Depth Drift Limit still caps it.";
> = 1;

uniform float PerspectiveLift <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 4.0;
    ui_label = "Perspective Ray Lift (texels)";
    ui_tooltip = "Only used with Perspective-correct. How far rays start off the surface along its normal, in texels. Higher avoids rays "
                 "hitting their own surface on curved or noisy geometry (self-shadow speckle). Too high loses contact bounce.";
> = 0.000000;

uniform float RayDepthSlopeTexel <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0003; ui_max = 0.06; ui_step = 0.0001;
    ui_label = "Ray Depth Slope (per texel)";
    ui_tooltip = "Only used with the Slope heuristic. How fast a ray's expected depth changes per effect texel travelled. Higher = rays "
                 "leave the surface plane sooner.";
> = 0.003000;

uniform float RayDepthDriftLimit <
    ui_category = "Expert: Hit Detection"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.02; ui_max = 10.0;
    ui_label = "Ray Depth Drift Limit";
    ui_tooltip = "Caps how far a ray's expected depth may drift from the surface it started on, however far it travels, so long rays "
                 "cannot leave the depth range of everything on screen. Used by both ray depth models. Games spread depth differently, "
                 "so very long draw distances may need a different value (or Relative Depth Tolerances).";
> = 0.020000;

// ============================================================================
// SECTION: Expert: Geometry Reconstruction
// How normals are rebuilt from depth (not used where game normals are used).
// ============================================================================

uniform int DownsampleDepthMode <
    ui_category = "Expert: Geometry Reconstruction"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Nearest depth\0Average depth\0";
    ui_label = "Depth Downsample";
    ui_tooltip = "How the full-resolution pixels under one effect texel become one depth. Nearest: thin geometry survives and no "
                 "in-between depths are invented, the normal and colour come from that same pixel. Average: the blended depth of the "
                 "block (softer, but invents floating depths along silhouettes).";
> = 0;

uniform bool UseSmoothedNormals <
    ui_category = "Expert: Geometry Reconstruction"; ui_category_closed = true;
    ui_label = "Smoothed Normals";
    ui_tooltip = "Averages each depth-rebuilt normal with 4 neighbours for calmer GI on rough detail and noisy depth. Does nothing for "
                 "pixels that use game normals (see Geometry Source).";
> = 0;

uniform float NormalSmoothTexels <
    ui_category = "Expert: Geometry Reconstruction"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.15; ui_max = 1.5; ui_step = 0.01;
    ui_label = "Normal Smoothing Radius (texels)";
    ui_tooltip = "Distance of the 4 neighbours averaged into each rebuilt normal, in effect texels. Larger = calmer normals and rounder "
                 "corners. Does nothing for pixels that use game normals, or with Smoothed Normals off.";
> = 0.500000;

uniform float NormalSmoothEdgeStop <
    ui_category = "Expert: Geometry Reconstruction"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0;
    ui_label = "Normal Smoothing Edge Stop";
    ui_tooltip = "Leaves neighbours that are off this pixel's surface plane out of the normal average. 0 = every neighbour counts (a "
                 "twig blends with the wall behind it), 1 = full edge stop. Does nothing for pixels that use game normals, or with "
                 "Smoothed Normals off.";
> = 0.165000;

uniform float SilhouetteAngle <
    ui_category = "Expert: Geometry Reconstruction"; ui_category_closed = true;
    ui_type = "slider"; ui_min = 1.0; ui_max = 30.0;
    ui_label = "Silhouette Angle (degrees)";
    ui_tooltip = "A depth step between neighbouring pixels that is steeper than a surface this close to edge-on counts as a silhouette, "
                 "not a slope. Higher = more steps count as silhouettes. Used by the depth rebuild and the contact pass, ignored for "
                 "pixels that use game normals.";
> = 3.199999;

// ============================================================================
// SECTION: Debug
// Look inside the pipeline. Leave on Off for normal use.
// ============================================================================

uniform int DebugView <
    ui_category = "Debug"; ui_category_closed = false;
    ui_type = "combo";
    ui_items = "Off\0GI Only\0Bounce Only, no sky\0Escape Visibility\0Enclosure (1 - escape visibility)\0Normals\0Hit Radiance Buffer\0Cascade 3 Radiance\0Linear Depth\0Object Thickness (red = thin)\0GI Before Denoise\0Contact Occlusion\0Motion Flow\0Emitters (sampled light and light sources)\0";
    ui_label = "Debug View";
    ui_tooltip = "Replaces the picture with an internal buffer, to check one stage at a time. GI-type views use Debug Gain.\n"
                 "GI Only / Bounce Only: the light by itself (the first thing to look at while tuning, it ignores GI Intensity).\n"
                 "Escape Visibility / Enclosure: how open each area is (gates the sky light).\n"
                 "Normals: a flat wall must be one even colour, speckle means the Field of View or depth setup is off.\n"
                 "Linear Depth: the sky must be clearly separate from the terrain (Sky Rejection Depth).\n"
                 "Object Thickness: red = thin (Hit Thickness Mode).\n"
                 "Contact Occlusion: the contact pass (Crevice Light Bounce Bias).\n"
                 "Emitters: what the shader samples light from and what it treats as a light source (grey = sampled light, orange = "
                 "emitter, black = nothing sampled).\n"
                 "Motion Flow shows the game's vectors while Use Game Motion Vectors is on. For the game's inputs and the canvas see "
                 "G-Buffer / Canvas View.";
> = 0;

uniform float DebugGain <
    ui_category = "Debug"; ui_category_closed = false;
    ui_type = "slider"; ui_min = 1.0; ui_max = 40.0;
    ui_label = "Debug Gain";
    ui_tooltip = "Brightness multiplier for the GI-type debug views and the single-cascade view. It has no effect on the normal image.";
> = 6.000000;

uniform int CascadeDebugLevel <
    ui_category = "Debug"; ui_category_closed = false;
    ui_type = "combo";
    ui_items = "Off\0Cascade 1 (nearest)\0Cascade 2\0Cascade 3\0Cascade 4\0Cascade 5 (needs CRCGI_CASCADES 5+)\0Cascade 6 (needs CRCGI_CASCADES 6)\0";
    ui_label = "Show Single Cascade";
    ui_tooltip = "Shows ONE cascade on its own interval, with nothing merged in from the farther cascades (1 = nearest). Use it to tune "
                 "Cascade Reach, the ray steps and the far-cascade accumulation.";
> = 0;

uniform int CascadeDebugMode <
    ui_category = "Debug"; ui_category_closed = false;
    ui_type = "combo";
    ui_items = "Radiance (interval only)\0Hit Coverage (fraction of rays that hit/blocked)\0";
    ui_label = "Cascade Debug Shows";
    ui_tooltip = "What the single-cascade view shows. Radiance: the light that cascade's rays picked up (times Debug Gain). Hit "
                 "Coverage: the fraction of its rays that hit or were blocked (white = everything hit), which shows how much of the "
                 "screen the cascade actually reaches.";
> = 1;

// ============================================================================
// SECTION: Debug: G-Buffer & Canvas
// The game's inputs, the normal fit and the off-screen canvas.
// ============================================================================

uniform int GBufferDebugView <
    ui_category = "Debug: G-Buffer & Canvas"; ui_category_closed = true;
    ui_type = "combo";
    ui_items = "Off\0G-Buffer Overview (albedo | normals / motion | depth)\0Game Albedo (input)\0Normals As Used (game, or depth where missing)\0Normals Rebuilt From Depth\0Normal Difference: Game vs Depth\0Game Motion Vectors (input)\0Off-Screen Canvas (whole canvas, screen outlined)\0Off-Screen Light Only (GI from the canvas)\0Game Normal Auto-Align Status\0Game HDR Scene Light (input, exposure-matched)\0";
    ui_label = "G-Buffer / Canvas View";
    ui_tooltip = "Shows what the game hands to the shader, and the off-screen canvas. Separate from Debug View.\n"
                 "Overview: top left albedo, top right normals as used, bottom left motion, bottom right depth.\n"
                 "Normals are shown in this shader's view space (x right, y down, z away, times 0.5 plus 0.5).\n"
                 "Normal Difference: black = the game's and the depth-rebuilt normals agree, white = 90 degrees apart, red = no game "
                 "normal.\n"
                 "Off-Screen Canvas: the whole canvas scaled to the screen. The yellow frame is the visible screen, outside it is the "
                 "remembered border (blue = nothing remembered there yet).\n"
                 "Off-Screen Light Only: the GI with every on-screen hit treated as a blocker without light and no sky, so only light "
                 "found in the canvas border remains (times Debug Gain).\n"
                 "Auto-Align Status: the aligned game normals against the depth normals, with the fit confidence as a bar on top (green "
                 "= good, red = too few surface directions in view to align).\n"
                 "Game HDR Scene Light: the game's lighting buffer after the exposure match (should look like the image without UI, "
                 "bloom and post-processing, black = not delivered or no match).";
> = 0;

// ----------------------------------------------------------------------------
// Textures / samplers
// ----------------------------------------------------------------------------

// Cascade-res geometry: xy = octahedral view-space normal, z = linear depth, w = object thickness.
texture2D CRCGIV1_Geo { Width = RC_HALF_W; Height = RC_HALF_H; Format = RGBA16F; };
sampler2D sGeo { Texture = CRCGIV1_Geo; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// rgb = light a ray picks up where it hits, a = display luminance (change detection).
// Mips for the cone blur.
texture2D CRCGIV1_Rad { Width = RC_HALF_W; Height = RC_HALF_H; Format = RGBA16F; MipLevels = 6; };
sampler2D sRad { Texture = CRCGIV1_Rad; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = LINEAR; };

// The four cascades. rgb = merged radiance, a = escape visibility (1 = reached the environment,
// 0 = hit/blocked). Linear filtering is used by the bilinear direction merge.
texture2D CRCGIV1_C0 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
texture2D CRCGIV1_C1 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
texture2D CRCGIV1_C2 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
texture2D CRCGIV1_C3 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
#if CRCGI_CASCADES >= 5
    texture2D CRCGIV1_C4 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
#else
    texture2D CRCGIV1_C4 { Width = 1; Height = 1; Format = RGBA16F; };     // unused at 4 cascades
#endif
#if CRCGI_CASCADES >= 6
    texture2D CRCGIV1_C5 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
#else
    texture2D CRCGIV1_C5 { Width = 1; Height = 1; Format = RGBA16F; };     // unused below 6 cascades
#endif
sampler2D sC0 { Texture = CRCGIV1_C0; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler2D sC1 { Texture = CRCGIV1_C1; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler2D sC2 { Texture = CRCGIV1_C2; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler2D sC3 { Texture = CRCGIV1_C3; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler2D sC4 { Texture = CRCGIV1_C4; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler2D sC5 { Texture = CRCGIV1_C5; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };

// Gathered GI at cascade resolution: rgb = GI, a = escape visibility.
texture2D CRCGIV1_Raw { Width = RC_HALF_W; Height = RC_HALF_H; Format = RGBA16F; };
sampler2D sRaw { Texture = CRCGIV1_Raw; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sRawLin { Texture = CRCGIV1_Raw; };   // denoise taps between texels, debug view

// Per cascade-0 probe: rgb = mean radiance, a = mean escape visibility of its cells;
// P0B.r = sky-weighted escape visibility. Full float precision so Gather gets the same numbers.
texture2D CRCGIV1_P0A { Width = RC_P0_W; Height = RC_P0_H; Format = RGBA32F; };
texture2D CRCGIV1_P0B { Width = RC_P0_W; Height = RC_P0_H; Format = RGBA32F; };   // r = sky-facing visibility, gba = sky map colour
sampler2D sP0A { Texture = CRCGIV1_P0A; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sP0B { Texture = CRCGIV1_P0B; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// Denoised GI at cascade resolution (horizontal pass, then vertical).
texture2D CRCGIV1_DenTmp { Width = RC_HALF_W; Height = RC_HALF_H; Format = RGBA16F; };
texture2D CRCGIV1_Den { Width = RC_HALF_W; Height = RC_HALF_H; Format = RGBA16F; MipLevels = 5; };   // mips: large-scale multibounce
sampler2D sDenTmp { Texture = CRCGIV1_DenTmp; };
sampler2D sDenIn { Texture = CRCGIV1_Den; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// Temporal accumulation: rgb = GI, a = linear depth of the texel. Hist holds last frame's Den.
texture2D CRCGIV1_Hist { Width = RC_HALF_W; Height = RC_HALF_H; Format = RGBA16F; };
sampler2D sHist { Texture = CRCGIV1_Hist; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sHistLin { Texture = CRCGIV1_Hist; };
sampler2D sDen { Texture = CRCGIV1_Den; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sDenLin { Texture = CRCGIV1_Den; };

// Once per frame: sky tint (current frame only).
texture2D CRCGIV1_SkyA { Width = 1; Height = 1; Format = RGBA16F; };
sampler2D sSkyA { Texture = CRCGIV1_SkyA; };
texture2D CRCGIV1_TM { Width = 1; Height = 1; Format = RGBA32F; };       // tonemapper conversion: x = exposure gain
sampler2D sTM { Texture = CRCGIV1_TM; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture2D CRCGIV1_Lum { Width = 1; Height = 1; Format = RGBA16F; };            // screen average brightness (log average of the surfaces in view)
sampler2D sLum { Texture = CRCGIV1_Lum; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// Motion flow: luminance of this frame and of the previous one (cascade resolution, with mips), and a coarse-to-fine
// block-matching flow on their mips. Flow.xy = where the picture moved since last frame (full-res px, previous ->
// current), Flow.z = how badly the best match still disagrees (mean luminance difference).
texture2D CRCGIV1_Luma     { Width = RC_HALF_W; Height = RC_HALF_H; Format = R16F; MipLevels = 4; };
texture2D CRCGIV1_LumaPrev { Width = RC_HALF_W; Height = RC_HALF_H; Format = R16F; MipLevels = 4; };
sampler2D sLuma     { Texture = CRCGIV1_Luma; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = LINEAR; };
sampler2D sLumaPrev { Texture = CRCGIV1_LumaPrev; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = LINEAR; };
texture2D CRCGIV1_Flow3 { Width = RC_HALF_W / 8; Height = RC_HALF_H / 8; Format = RGBA16F; };
texture2D CRCGIV1_Flow2 { Width = RC_HALF_W / 4; Height = RC_HALF_H / 4; Format = RGBA16F; };
texture2D CRCGIV1_Flow1 { Width = RC_HALF_W / 2; Height = RC_HALF_H / 2; Format = RGBA16F; };
texture2D CRCGIV1_Flow0 { Width = RC_HALF_W;     Height = RC_HALF_H;     Format = RGBA16F; };
sampler2D sFlow3 { Texture = CRCGIV1_Flow3; };
sampler2D sFlow2 { Texture = CRCGIV1_Flow2; };
sampler2D sFlow1 { Texture = CRCGIV1_Flow1; };
sampler2D sFlow0 { Texture = CRCGIV1_Flow0; };

// Last frame's geometry (to validate reprojected history) and the min / max depth pyramid of this frame's geometry:
// rg = (nearest, farthest) surface depth in the footprint, sky ignored (min 1 / max 0 where there is only sky).
texture2D CRCGIV1_GeoPrev { Width = RC_HALF_W; Height = RC_HALF_H; Format = RGBA16F; };
sampler2D sGeoPrev { Texture = CRCGIV1_GeoPrev; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture2D CRCGIV1_Pyr1 { Width = RC_HALF_W / 2; Height = RC_HALF_H / 2; Format = RG16F; };
texture2D CRCGIV1_Pyr2 { Width = RC_HALF_W / 4; Height = RC_HALF_H / 4; Format = RG16F; };
texture2D CRCGIV1_Pyr3 { Width = RC_HALF_W / 8; Height = RC_HALF_H / 8; Format = RG16F; };
sampler2D sPyr1 { Texture = CRCGIV1_Pyr1; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sPyr2 { Texture = CRCGIV1_Pyr2; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sPyr3 { Texture = CRCGIV1_Pyr3; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// Per-cascade history of the accumulated cascades (levels 1 and up = cascades 2 to 6, see Far Cascades: Accumulate From).
texture2D CRCGIV1_HC1 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
texture2D CRCGIV1_HC2 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
texture2D CRCGIV1_HC3 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
#if CRCGI_CASCADES >= 5
    texture2D CRCGIV1_HC4 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
#else
    texture2D CRCGIV1_HC4 { Width = 1; Height = 1; Format = RGBA16F; };
#endif
#if CRCGI_CASCADES >= 6
    texture2D CRCGIV1_HC5 { Width = RC_TEX_W; Height = RC_TEX_H; Format = RGBA16F; };
#else
    texture2D CRCGIV1_HC5 { Width = 1; Height = 1; Format = RGBA16F; };
#endif
sampler2D sHC1 { Texture = CRCGIV1_HC1; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sHC2 { Texture = CRCGIV1_HC2; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sHC3 { Texture = CRCGIV1_HC3; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sHC4 { Texture = CRCGIV1_HC4; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sHC5 { Texture = CRCGIV1_HC5; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// Contact cascade: always full resolution (independent of CRCGI_RESOLUTION_DIVISOR), r = transmittance of the contact interval.
texture2D CRCGIV1_Contact { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R16F; };
sampler2D sContact { Texture = CRCGIV1_Contact; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
#if CRCGI_SECONDARY_DIVISOR > 0
// Multibounce map: last frame's GI reprojected onto this frame's surfaces, premultiplied by its validity (rgb) with the validity
// in alpha, mipped so a ray can read it over the penumbra of the cascade that found the hit.
texture2D CRCGIV1_MB { Width = SEC_W; Height = SEC_H; Format = RGBA16F; MipLevels = 5; };
sampler2D sMB { Texture = CRCGIV1_MB; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = LINEAR; AddressU = CLAMP; AddressV = CLAMP; };
#endif

// Off-screen canvas: rgb = the light a ray picks up there, a = linear depth (0 = nothing known yet).
texture2D CRCGIV1_Canvas     { Width = CANVAS_W; Height = CANVAS_H; Format = RGBA16F; };
texture2D CRCGIV1_CanvasPrev { Width = CANVAS_W; Height = CANVAS_H; Format = RGBA16F; };
sampler2D sCanvas     { Texture = CRCGIV1_Canvas;     MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D sCanvasPrev { Texture = CRCGIV1_CanvasPrev; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// Canvas confidence (1 = seen this frame, fading with Canvas Memory once off screen). The light itself is kept at full strength.
texture2D CRCGIV1_CanvasW     { Width = CANVAS_W; Height = CANVAS_H; Format = R16F; };
texture2D CRCGIV1_CanvasWPrev { Width = CANVAS_W; Height = CANVAS_H; Format = R16F; };
sampler2D sCanvasW     { Texture = CRCGIV1_CanvasW;     MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D sCanvasWPrev { Texture = CRCGIV1_CanvasWPrev; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };

// Sky map and its previous frame.
texture2D CRCGIV1_SkyMap     { Width = SKYMAP_W; Height = SKYMAP_H; Format = RGBA16F; MipLevels = 7; };
texture2D CRCGIV1_SkyMapPrev { Width = SKYMAP_W; Height = SKYMAP_H; Format = RGBA16F; };
sampler2D sSkyMap     { Texture = CRCGIV1_SkyMap;     MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D sSkyMapPrev { Texture = CRCGIV1_SkyMapPrev; MinFilter = POINT;  MagFilter = POINT;  MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };

// Optional real normals, only populated if another addon binds this semantic.
texture2D texNormalTex : NORMAL;
sampler2D sNormalTex { Texture = texNormalTex; };

// ============================================================================
// Game integration (CRCGI Injector add-on). The add-on binds these when level C is on; the switches below are the shader's side.
// ============================================================================
texture2D texAlbedoTex : CRCGI_ALBEDO;
sampler2D sAlbedoTex { Texture = texAlbedoTex; };
texture2D texMotionTex : CRCGI_MOTION;
sampler2D sMotionTex { Texture = texMotionTex; AddressU = CLAMP; AddressV = CLAMP; };
texture2D texMaterialTex : CRCGI_MATERIAL;
sampler2D sMaterialTex { Texture = texMaterialTex; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture2D texHDRSceneTex : CRCGI_HDRSCENE;
sampler2D sHDRSceneTex { Texture = texHDRSceneTex; AddressU = CLAMP; AddressV = CLAMP; };









// Game normal auto-align: per-tile sums of (depth normal) x (game normal)^T, then the best-fitting orthogonal matrix.
#define NFIT_W   32
#define NFIT_H   18
#define NFIT_SUB 4
texture2D CRCGIV1_NFit0 { Width = NFIT_W; Height = NFIT_H; Format = RGBA32F; };
texture2D CRCGIV1_NFit1 { Width = NFIT_W; Height = NFIT_H; Format = RGBA32F; };
texture2D CRCGIV1_NFit2 { Width = NFIT_W; Height = NFIT_H; Format = RGBA32F; };
sampler2D sNFit0 { Texture = CRCGIV1_NFit0; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sNFit1 { Texture = CRCGIV1_NFit1; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sNFit2 { Texture = CRCGIV1_NFit2; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture2D CRCGIV1_NRot     { Width = 4; Height = 1; Format = RGBA32F; };   // texels 0..2 = rotation rows, 3 = (confidence, samples, agreement, valid)
texture2D CRCGIV1_NRotPrev { Width = 4; Height = 1; Format = RGBA32F; };
sampler2D sNRot     { Texture = CRCGIV1_NRot;     MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler2D sNRotPrev { Texture = CRCGIV1_NRotPrev; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };

// Written by the add-on, hidden from the UI.
uniform int   Integration_NormalMode   < hidden = true; > = 0;      // 0 RGB stored 0..1, 1 octahedral, 2 RGB signed as stored
uniform bool  Integration_NormalFlipY  < hidden = true; > = false;
uniform int   Integration_MotionUnits  < hidden = true; > = 0;      // 0 UV, 1 pixels, 2 NDC
uniform bool  Integration_MotionFlipY  < hidden = true; > = false;
uniform bool  Integration_MotionInvert < hidden = true; > = false;
uniform float Integration_Near         < hidden = true; > = 0.1;    // camera near plane
uniform float Integration_Far          < hidden = true; > = 1000.0;
uniform bool  Integration_Reversed     < hidden = true; > = false;
uniform int   Integration_MaterialChannel < hidden = true; > = 0;   // 0 R, 1 G, 2 B, 3 A
uniform int   Integration_MaterialMeaning < hidden = true; > = 0;   // 0 metalness, 1 specular / reflectance, 2 roughness, 3 smoothness

// Share of a surface's light that is diffuse, from the game's material texture (1 without it). Roughness and smoothness have no
// diffuse effect here (they would need specular GI).
float MaterialDiffuse(float2 uv)
{
    if (!Integration_Material || Integration_MaterialMeaning >= 2)
        return 1.0;
    float4 t = tex2Dlod(sMaterialTex, float4(uv, 0, 0));
    float v = saturate((Integration_MaterialChannel == 0) ? t.r : ((Integration_MaterialChannel == 1) ? t.g : ((Integration_MaterialChannel == 2) ? t.b : t.a)));
    return (Integration_MaterialMeaning == 0) ? 1.0 - 0.85 * v : 1.0 - 0.5 * v;
}

float3 DecodeGameNormal(float4 t)
{
    float3 n;
    if (Integration_NormalMode == 1)            // octahedral, two channels in [0,1]
    {
        float2 f = t.xy * 2.0 - 1.0;
        n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
        float tt = saturate(-n.z);
        n.xy += float2(n.x >= 0.0 ? -tt : tt, n.y >= 0.0 ? -tt : tt);
    }
    else if (Integration_NormalMode == 2)       // float / snorm targets: already signed
        n = t.rgb;
    else
        n = t.rgb * 2.0 - 1.0;
    if (Integration_NormalFlipY)
        n.y = -n.y;
    return n;
}

// The game's motion vector as this shader's flow: pixels, current minus previous position (UV = current - flow / size).
float4 GameMotionFlow(float2 uv, int lod)
{
    if (lod != 0)
        return float4(0.0, 0.0, 0.0, 1.0);      // the coarser levels are only predictions for level 0
    float2 mv = tex2Dlod(sMotionTex, float4(uv, 0, 0)).xy;
    float2 size = float2(BUFFER_WIDTH, BUFFER_HEIGHT);
    if (Integration_MotionUnits == 0)      mv *= size;
    else if (Integration_MotionUnits == 2) mv *= 0.5 * size;
    if (Integration_MotionFlipY)   mv.y = -mv.y;
    if (Integration_MotionInvert)  mv = -mv;
    return float4(mv, 0.0, 1.0);                // z = 0: fully trusted
}

// ----------------------------------------------------------------------------
// Basic helpers
// ----------------------------------------------------------------------------

float GetLinearDepth(float2 uv)
{
    return ReShade::GetLinearizedDepth(uv);
}

// NaN / Inf guard, checked on the bits so no compiler can fold it away.
bool IsBad(float3 v) { return any((asuint(v) & 0x7F800000u) == 0x7F800000u); }
float3 Finite(float3 v) { return IsBad(v) ? float3(0, 0, 0) : v; }            // GI may be negative after saturation boost
float3 Clean(float3 v)  { return IsBad(v) ? float3(0, 0, 0) : max(v, 0.0); }  // light is never negative

// Scale-invariant depth distance: a gap of 0.05 means something very different at depth 0.1
// than at depth 0.8.
// Relative Depth Tolerances: 1 at Tolerance Reference Depth, proportional to the depth elsewhere. Scales the absolute
// linear-depth tolerances so they stay valid when the far plane is very far (see the Occlusion & Depth Separation settings).
float DepthTolScale(float z)
{
    return lerp(1.0, max(z, 1.0e-5) / DepthTolReference, DepthTolScaling);
}

float DepthEps(float z) { return 0.05 * DepthTolScale(z); }

float RelDepth(float a, float b)
{
    float m = min(a, b);
    return abs(a - b) / (m + 0.02 * DepthTolScale(m));
}

float3 BoostSaturation(float3 color, float boost)
{
    float lum = dot(color, LUMA);
    return lerp(lum.xxx, color, 1.0 + boost);
}

float3 ClampLuminance(float3 color, float maxLum)
{
    float lum = dot(color, LUMA);
    return (lum > maxLum && lum > 0.0001) ? color * (maxLum / lum) : color;
}

float3 RGBtoHSV(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 ApplyHueFilter(float3 color, float targetHue, float hueWidth, float strength)
{
    if (strength <= 0.001)
        return color;
    float3 hsv = RGBtoHSV(color);
    float hueDist = abs(hsv.x - targetHue);
    hueDist = min(hueDist, 1.0 - hueDist);
    float weight = (1.0 - smoothstep(0.0, hueWidth, hueDist)) * strength;
    float lum = dot(color, LUMA);
    return lerp(color, lum.xxx, weight);
}

float3 ApplyColorFilters(float3 color)
{
    color = ApplyHueFilter(color, BlueFilterHue, BlueFilterWidth, BlueFilter);
    color = ApplyHueFilter(color, OrangeFilterHue, OrangeFilterWidth, OrangeFilter);

    if (abs(GITemperature) > 0.001)
    {
        float3 warm = color * float3(1.0 + TemperatureStrength, 1.0, 1.0 - TemperatureStrength);
        float3 cool = color * float3(1.0 - TemperatureStrength, 1.0, 1.0 + TemperatureStrength);
        color = lerp(color, GITemperature > 0.0 ? warm : cool, abs(GITemperature));
    }
    return color;
}

float3 ApplyBounceCurve(float3 c)
{
    float l = dot(c, LUMA);
    if (abs(BounceContrast - 1.0) < 0.001 || l <= 1e-5)
        return c;
    float s = pow(l / BouncePivot, BounceContrast - 1.0);
    return c * clamp(s, 0.0, BounceMaxGain);
}

float GetDistanceFade(float depth)
{
    float cutoff = GIDistanceCutoff;
    float fadeStart = cutoff * (1.0 - GIDistanceFadeWidth);
    return 1.0 - smoothstep(fadeStart, cutoff, depth);
}

// Falloff of a hit: Hit Distance Falloff on the travel as a fraction of the full reach (t = 0..1), times the receiver's
// distance fade. Both are free of screen resolution and divisor.
float HitFalloff(float t, float receiverDepth)
{
    float f = 1.0 / (1.0 + 16.0 * HitDistanceFalloff * t * t);
    return f * lerp(1.0, saturate(1.0 - receiverDepth), ProximityCameraDistanceInfluence);
}

float3 SRGBToLinear(float3 c) { return (c <= 0.04045) ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4); }
float3 LinearToSRGB(float3 c) { return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055; }

// ----------------------------------------------------------------------------
// Extra game tonemappers. Each one has a forward curve for scene-linear Rec.709 light and
// an inverse that undoes it. Inputs: 1.0 = reference white paper, mid grey about 0.18.
// ----------------------------------------------------------------------------

static const float3x3 M709_TO_2020 = float3x3(0.6274039, 0.3292830, 0.0433131,
                                              0.0690973, 0.9195404, 0.0113623,
                                              0.0163914, 0.0880133, 0.8955953);
static const float3x3 M2020_TO_709 = float3x3( 1.660490989, -0.587641056, -0.072849934,
                                              -0.124550493,  1.132899890, -0.008349397,
                                              -0.018150689, -0.100578909,  1.118729598);

// --- ACES 2.0, 100 nit SDR preset ---
// The tone scale is the one of the ACES 2 Output Transform (a Michaelis-Menten based S curve). The constants
// are derived for 100 nits and reproduce the published table (scene 0.18 / 1.0 / 2.0 -> 10 / 45.757 / 63.988
// nits). The colour handling is SIMPLIFIED: the colour is scaled by the luminance gain (hue preserved), then
// desaturated at constant luminance until it fits the display. The real transform does this in a CAM16-like
// JMh space with chroma-compression and gamut-compression tables, so saturated colours will differ.
static const float AC2_M2 = 1.047103762;
static const float AC2_S2 = 0.919858254;
static const float AC2_T1 = 0.04;
static const float AC2_G  = 1.15;

float AC2_Scale(float Y)                       // display luminance (1.0 = 100 nits) for scene luminance Y
{
    float yy = max(Y, 0.0);
    float f = AC2_M2 * pow(yy / (yy + AC2_S2), AC2_G);
    return max(f * f / (f + AC2_T1), 0.0);
}

float AC2_ScaleInv(float h)                    // closed-form inverse of the tone scale
{
    h = max(h, 0.0);
    float f = 0.5 * (h + sqrt(h * (h + 4.0 * AC2_T1)));
    float q = min(pow(f / AC2_M2, 1.0 / AC2_G), 0.999999);
    return AC2_S2 * q / (1.0 - q);
}

float3 TonemapACES2(float3 x)
{
    x = max(x, 0.0);
    float Y = dot(x, LUMA);
    if (Y <= 1.0e-6)
        return float3(0, 0, 0);
    float g = AC2_Scale(Y);
    float3 c = x * (g / Y);
    float mx = max(c.r, max(c.g, c.b));
    float tt = (mx > 1.0) ? (mx - 1.0) / max(mx - g, 1.0e-5) : 0.0;
    return saturate(lerp(c, float3(g, g, g), saturate(tt)));
}

float3 InvTonemapACES2(float3 y)
{
    float Yd = max(dot(y, LUMA), 1.0e-6);
    return y * (AC2_ScaleInv(Yd) / Yd);
}

// --- AgX (base contrast, as in Blender / Filament / three.js) ---
// linear sRGB -> Rec.2020 -> inset matrix -> log2 -> sigmoid -> outset matrix -> 2.2 gamma -> linear sRGB.
// The inverse is exact up to the clipping the forward pass does (colours pushed out of gamut are lost).
static const float3x3 AGX_709_TO_2020 = float3x3(0.6274, 0.3293, 0.0433,
                                                 0.0691, 0.9195, 0.0113,
                                                 0.0164, 0.0880, 0.8956);
static const float3x3 AGX_2020_TO_709 = float3x3( 1.6605, -0.5876, -0.0728,
                                                 -0.1246,  1.1329, -0.0083,
                                                 -0.0182, -0.1006,  1.1187);
static const float3x3 AGX_INSET  = float3x3(0.856627153315983,  0.0951212405381588, 0.0482516061458583,
                                            0.137318972929847,  0.761241990602591,  0.101439036467562,
                                            0.11189821299995,   0.0767994186031903, 0.811302368396859);
static const float3x3 AGX_OUTSET = float3x3( 1.1271005818144368,  -0.11060664309660323, -0.016493938717834573,
                                            -0.1413297634984383,   1.157823702216272,   -0.016493938717834257,
                                            -0.14132976349843826, -0.11060664309660294,  1.2519364065950405);
static const float3x3 AGX_INSET_INV  = float3x3( 1.197441077, -0.144261513, -0.053179564,
                                                -0.196474626,  1.354095131, -0.157620505,
                                                -0.146557417, -0.108284059,  1.254841476);
static const float3x3 AGX_OUTSET_INV = float3x3(0.899796956, 0.087199619, 0.013003425,
                                                0.111420989, 0.875575586, 0.013003425,
                                                0.111420989, 0.087199619, 0.801379392);
static const float3x3 AGX_INV_A = float3x3( 1.660512276, -0.587704679, -0.072866367,      // inverse of AGX_709_TO_2020
                                           -0.124563464,  1.132949121, -0.008272362,
                                           -0.018167504, -0.100559587,  1.118717035);
static const float3x3 AGX_INV_B = float3x3(0.62740944,  0.329260252, 0.043271894,        // inverse of AGX_2020_TO_709
                                           0.069124839, 0.919548614, 0.011320767,
                                           0.016423358, 0.088047848, 0.895616714);
static const float AGX_MIN_EV = -12.47393;
static const float AGX_MAX_EV = 4.026069;

float AgxSig(float x)                          // 6th order approximation of the AgX base contrast curve
{
    return x * (x * (x * (x * (x * (15.5 * x - 40.14) + 31.96) - 6.868) + 0.4298) + 0.1191) - 0.00232;
}

float AgxSigDeriv(float x)
{
    return x * (x * (x * (x * (93.0 * x - 200.7) + 127.84) - 20.604) + 0.8596) + 0.1191;
}

float AgxSigInv(float tv)                      // monotonic on 0..1: bisection, then two Newton steps
{
    tv = clamp(tv, -0.0023, 0.9985);
    float lo = 0.0;
    float hi = 1.0;
    [unroll]
    for (int i = 0; i < 10; i++)
    {
        float mid = 0.5 * (lo + hi);
        if (AgxSig(mid) < tv)
            lo = mid;
        else
            hi = mid;
    }
    float x = 0.5 * (lo + hi);
    x = x - (AgxSig(x) - tv) / max(AgxSigDeriv(x), 1.0e-3);
    x = x - (AgxSig(x) - tv) / max(AgxSigDeriv(x), 1.0e-3);
    return saturate(x);
}

float3 AgxApplyLook(float3 v, int look)             // on the AgX base-contrast signal, before the outset matrix
{
    if (look == 0)
        return v;
    float3 slope = (look == 1) ? float3(1.0, 1.0, 1.0) : float3(1.0, 0.9, 0.5);
    float power = (look == 1) ? 1.35 : 0.8;
    float sat = (look == 1) ? 1.4 : 1.3;
    v = pow(max(v * slope, 0.0), power);
    float l = dot(v, LUMA);
    return l + sat * (v - l);
}

float3 TonemapAgXP(float3 x, int look)
{
    float3 v = mul(AGX_709_TO_2020, max(x, 0.0));
    v = mul(AGX_INSET, v);
    v = log2(max(v, 1.0e-10));
    v = saturate((v - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV));
    v = float3(AgxSig(v.x), AgxSig(v.y), AgxSig(v.z));
    v = AgxApplyLook(v, look);
    v = mul(AGX_OUTSET, v);
    v = pow(max(v, 0.0), 2.2);
    return saturate(mul(AGX_2020_TO_709, v));
}

float3 TonemapAgX(float3 x) { return TonemapAgXP(x, 0); }

float3 InvTonemapAgX(float3 y)
{
    float3 v = mul(AGX_INV_B, y);
    v = pow(max(v, 0.0), 1.0 / 2.2);
    v = mul(AGX_OUTSET_INV, v);
    v = float3(AgxSigInv(v.x), AgxSigInv(v.y), AgxSigInv(v.z));
    v = exp2(v * (AGX_MAX_EV - AGX_MIN_EV) + AGX_MIN_EV);
    return max(mul(AGX_INV_A, mul(AGX_INSET_INV, v)), 0.0);
}

// --- Gran Turismo 7 tone mapping, SDR (250 nit paper white), ICtCp variant ---
// A port of the published sample (Polyphony Digital, MIT License, 2025): a per-channel curve with toe, linear
// part and convergent shoulder is blended with a version that keeps the original chroma in ICtCp and only takes
// the curve's luminance, and the chroma fades out near peak. Input: scene-linear light where 1.0 = 100 nits.
// The inverse below undoes only the per-channel curve, so it is exact for neutral colours and approximate
// (a few to ~10%) for saturated ones. Constants: peak 2.5, alpha 0.25, mid 0.538, linear section 0.444, toe 1.28,
// blend 0.6, chroma fade 0.98..1.16, SDR correction 0.4.
// Copyright (c) 2025 Polyphony Digital Inc., MIT License: permission is granted to use, copy, modify and
// distribute this software provided this notice is kept; the software is provided "as is", without warranty.
static const float GT7_PEAK = 2.5;
static const float GT7_MID  = 0.538;
static const float GT7_LIN  = 0.444;
static const float GT7_TOE  = 1.280;
static const float GT7_KA   =  2.963333333;
static const float GT7_KB   = -3.373351238;
static const float GT7_KC   = -0.539568345;
static const float GT7_TARGET_I = 0.602559155;     // ICtCp intensity of a grey at the peak

float GT7_Curve(float x)
{
    if (x < 0.0)
        return 0.0;
    if (x >= GT7_LIN * GT7_PEAK)
        return GT7_KA + GT7_KB * exp(x * GT7_KC);
    float wl = smoothstep(0.0, GT7_MID, x);
    return (1.0 - wl) * GT7_MID * pow(x / GT7_MID, GT7_TOE) + wl * x;
}

float GT7_CurveP(float x, float alpha, float lin, float toe)   // tunable version (output curve)
{
    float k = (lin - 1.0) / (alpha - 1.0);
    float kA = GT7_PEAK * lin + GT7_PEAK * k;
    float kB = -GT7_PEAK * k * exp(lin / k);
    float kC = -1.0 / (k * GT7_PEAK);
    if (x < 0.0)
        return 0.0;
    if (x >= lin * GT7_PEAK)
        return kA + kB * exp(x * kC);
    float wl = smoothstep(0.0, GT7_MID, x);
    return (1.0 - wl) * GT7_MID * pow(x / GT7_MID, toe) + wl * x;
}

float GT7_CurveInv(float c)
{
    c = min(c, 0.99 * GT7_PEAK);
    float linEnd = GT7_LIN * GT7_PEAK;
    if (c >= linEnd)
        return log((c - GT7_KA) / GT7_KB) / GT7_KC;
    float lo = 0.0;
    float hi = linEnd;
    [loop]
    for (int i = 0; i < 12; i++)
    {
        float mid = 0.5 * (lo + hi);
        if (GT7_Curve(mid) < c)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

float GT7_PQEncode(float fb)                   // frame-buffer value (1.0 = 100 nits) -> PQ signal
{
    float y = max(fb, 0.0) * 0.01;
    float ym = pow(y, 0.1593017578125);
    return exp2(78.84375 * (log2(0.8359375 + 18.8515625 * ym) - log2(1.0 + 18.6875 * ym)));
}

float GT7_PQDecode(float n)                    // PQ signal -> frame-buffer value
{
    n = clamp(n, 0.0, 1.0);
    float np = pow(n, 1.0 / 78.84375);
    float l = max(np - 0.8359375, 0.0);
    l = l / (18.8515625 - 18.6875 * np);
    return pow(l, 1.0 / 0.1593017578125) * 100.0;
}

float3 GT7_RgbToICtCp(float3 rgb)              // linear Rec.2020 in
{
    float l = dot(rgb, float3(1688.0, 2146.0, 262.0)) / 4096.0;
    float m = dot(rgb, float3(683.0, 2951.0, 462.0)) / 4096.0;
    float s = dot(rgb, float3(99.0, 309.0, 3688.0)) / 4096.0;
    float lp = GT7_PQEncode(l);
    float mp = GT7_PQEncode(m);
    float sp = GT7_PQEncode(s);
    return float3((2048.0 * lp + 2048.0 * mp) / 4096.0,
                  (6610.0 * lp - 13613.0 * mp + 7003.0 * sp) / 4096.0,
                  (17933.0 * lp - 17390.0 * mp - 543.0 * sp) / 4096.0);
}

float3 GT7_ICtCpToRgb(float3 u)                // linear Rec.2020 out
{
    float l = u.x + 0.00860904 * u.y + 0.11103 * u.z;
    float m = u.x - 0.00860904 * u.y - 0.11103 * u.z;
    float s = u.x + 0.560031 * u.y - 0.320627 * u.z;
    float ll = GT7_PQDecode(l);
    float mm = GT7_PQDecode(m);
    float ss = GT7_PQDecode(s);
    return max(float3(3.43661 * ll - 2.50645 * mm + 0.0698454 * ss,
                     -0.79133 * ll + 1.9836 * mm - 0.192271 * ss,
                     -0.0259499 * ll - 0.0989137 * mm + 1.12486 * ss), 0.0);
}

float3 TonemapGT7P(float3 x, float alpha, float lin, float toe)
{
    float3 rgb = mul(M709_TO_2020, max(x, 0.0));
    float3 ucs = GT7_RgbToICtCp(rgb);
    float3 sk = float3(GT7_CurveP(rgb.r, alpha, lin, toe), GT7_CurveP(rgb.g, alpha, lin, toe), GT7_CurveP(rgb.b, alpha, lin, toe));
    float3 skU = GT7_RgbToICtCp(sk);
    float cs = 1.0 - smoothstep(0.98, 1.16, ucs.x / GT7_TARGET_I);
    float3 sc = GT7_ICtCpToRgb(float3(skU.x, ucs.y * cs, ucs.z * cs));
    float3 o = 0.4 * min(lerp(sk, sc, 0.6), GT7_PEAK);
    return saturate(mul(M2020_TO_709, o));
}

float3 TonemapGT7(float3 x) { return TonemapGT7P(x, 0.25, 0.444, 1.28); }

float3 InvTonemapGT7(float3 y)
{
    return float3(GT7_CurveInv(y.r / 0.4), GT7_CurveInv(y.g / 0.4), GT7_CurveInv(y.b / 0.4));
}

// --- Uncharted 2 / Hable filmic (exposure bias 2.0, white point 11.2) ---
// Per channel, with an exact closed-form inverse (a quadratic).
static const float HBL_A = 0.15;
static const float HBL_B = 0.50;
static const float HBL_C = 0.10;
static const float HBL_D = 0.20;
static const float HBL_E = 0.02;
static const float HBL_F = 0.30;
static const float HBL_WHITE_SCALE = 1.379064247;      // 1 / curve(11.2)

float3 TonemapHable(float3 x)
{
    float3 v = max(x, 0.0) * 2.0;
    float3 c = (v * (HBL_A * v + HBL_C * HBL_B) + HBL_D * HBL_E) / (v * (HBL_A * v + HBL_B) + HBL_D * HBL_F) - HBL_E / HBL_F;
    return saturate(c * HBL_WHITE_SCALE);
}

float HableInv(float y)
{
    y = min(y, 0.99);
    float tt = y / HBL_WHITE_SCALE + HBL_E / HBL_F;
    float qa = HBL_A * (tt - 1.0);
    float qb = HBL_B * (tt - HBL_C);
    float qc = HBL_D * (tt * HBL_F - HBL_E);
    float disc = max(qb * qb - 4.0 * qa * qc, 0.0);
    return max((-qb - sqrt(disc)) / (2.0 * min(qa, -1.0e-6)), 0.0) * 0.5;
}

// --- ACES, Hill's RRT+ODT fit (as in three.js: input scaled by 1/0.6) ---
static const float3x3 HILL_IN  = float3x3(0.59719, 0.35458, 0.04823,
                                          0.07600, 0.90834, 0.01566,
                                          0.02840, 0.13383, 0.83777);
static const float3x3 HILL_OUT = float3x3( 1.60475, -0.53108, -0.07367,
                                          -0.10208,  1.10813, -0.00605,
                                          -0.00327, -0.07276,  1.07602);
static const float3x3 HILL_IN_INV  = float3x3(1.764740972, -0.675777678, -0.088963294,
                                              -0.147027852, 1.160251512, -0.013223660,
                                              -0.036336830, -0.162436437, 1.198773267);
static const float3x3 HILL_OUT_INV = float3x3(0.643038249, 0.311186752, 0.045775457,
                                              0.059268690, 0.931436487, 0.009294916,
                                              0.005961901, 0.063929016, 0.930118384);

float3 TonemapHill(float3 x)
{
    float3 v = mul(HILL_IN, max(x, 0.0) / 0.6);
    v = (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081);
    return saturate(mul(HILL_OUT, v));
}

float HillRRTInv(float tv)                     // positive root of the per-channel rational fit
{
    tv = clamp(tv, 0.0, 0.99);
    float qa = 1.0 - 0.983729 * tv;
    float qb = 0.0245786 - 0.4329510 * tv;
    float qc = -(0.000090537 + 0.238081 * tv);
    return (-qb + sqrt(max(qb * qb - 4.0 * qa * qc, 0.0))) / (2.0 * qa);
}

float3 InvTonemapHill(float3 y)
{
    float3 tv = mul(HILL_OUT_INV, min(y, 0.99));
    float3 v = float3(HillRRTInv(tv.x), HillRRTInv(tv.y), HillRRTInv(tv.z));
    return max(mul(HILL_IN_INV, v), 0.0) * 0.6;
}

// --- Uchimura (Gran Turismo Sport), SDR: max brightness 1, no pedestal ---
// a = contrast, m = linear section start, l = linear section length, c = toe (black tightness).
float UchimuraCurve(float x, float a, float m, float l, float c)
{
    float l0 = (1.0 - m) * l / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float CP = -a / (1.0 - S1);
    x = max(x, 0.0);
    float w0 = 1.0 - smoothstep(0.0, m, x);
    float w2 = (x >= S0) ? 1.0 : 0.0;
    float w1 = 1.0 - w0 - w2;
    float T = m * pow(x / m, c);
    float L = m + a * (x - m);
    float S = 1.0 - (1.0 - S1) * exp(CP * (x - S0));
    return T * w0 + L * w1 + S * w2;
}

float3 TonemapUchimura(float3 x, float a, float m, float l, float c)
{
    return saturate(float3(UchimuraCurve(x.r, a, m, l, c), UchimuraCurve(x.g, a, m, l, c), UchimuraCurve(x.b, a, m, l, c)));
}

float UchimuraInv(float y)                     // published settings; the curve is monotonic: bisection in log space
{
    y = min(y, 0.99);
    float lo = log(1.0e-5);
    float hi = log(64.0);
    [loop]
    for (int i = 0; i < 20; i++)
    {
        float mid = 0.5 * (lo + hi);
        if (UchimuraCurve(exp(mid), 1.0, 0.22, 0.4, 1.33) < y)
            lo = mid;
        else
            hi = mid;
    }
    return exp(0.5 * (lo + hi));
}

// --- Hable with tunable shoulder (A), toe (D) and white point (W); B, C, E, F and the exposure bias stay fixed ---
float3 TonemapHableP(float3 x, float A, float D, float W)
{
    float3 v = max(x, 0.0) * 2.0;
    float3 c = (v * (A * v + HBL_C * HBL_B) + D * HBL_E) / (v * (A * v + HBL_B) + D * HBL_F) - HBL_E / HBL_F;
    float cw = (W * (A * W + HBL_C * HBL_B) + D * HBL_E) / (W * (A * W + HBL_B) + D * HBL_F) - HBL_E / HBL_F;
    return saturate(c / cw);
}

// Index: 0 Reinhard, 1 ACES filmic, 2 None, 3 ACES 2.0, 4 AgX, 5 Gran Turismo 7, 6 Hable, 7 ACES (Hill), 8 Uchimura.
// TonemapSel / InvToneVecSel: the INPUT side, always with the published settings.
float3 TonemapSel(int mode, float3 x)
{
    if (mode == 0)
        return x / (1.0 + x);
    if (mode == 1)                                                // ACES fit (Narkowicz)
        return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
    if (mode == 3)
        return TonemapACES2(x);
    if (mode == 4)
        return TonemapAgX(x);
    if (mode == 5)
        return TonemapGT7(x);
    if (mode == 6)
        return TonemapHable(x);
    if (mode == 7)
        return TonemapHill(x);
    if (mode == 8)
        return TonemapUchimura(x, 1.0, 0.22, 0.4, 1.33);
    return saturate(x);
}

// Inverse of the per-channel curves (below 1; the top 1% is clamped so it stays finite).
float InvToneScalarSel(int mode, float c)
{
    c = min(c, 0.99);
    if (mode == 0)
        return c / (1.0 - c);
    if (mode == 1)                                                // positive root of the ACES fit
    {
        float A = 2.43 * c - 2.51, B = 0.59 * c - 0.03, C = 0.14 * c;
        return (-B - sqrt(B * B - 4.0 * A * C)) / (2.0 * A);
    }
    if (mode == 6)
        return HableInv(c);
    if (mode == 8)
        return UchimuraInv(c);
    return c;
}

float3 InvToneVecSel(int mode, float3 y)
{
    if (mode <= 2 || mode == 6 || mode == 8)
        return float3(InvToneScalarSel(mode, y.r), InvToneScalarSel(mode, y.g), InvToneScalarSel(mode, y.b));
    y = min(y, 0.99);
    if (mode == 3)
        return InvTonemapACES2(y);
    if (mode == 4)
        return InvTonemapAgX(y);
    if (mode == 7)
        return InvTonemapHill(y);
    return InvTonemapGT7(y);
}

// The OUTPUT curve with its tunables.
float3 TonemapOutCurve(float3 x)
{
    int m = OutputTonemap;
    if (m == 0)                                                   // extended Reinhard
        return saturate(x * (1.0 + x / (ReinhardWhite * ReinhardWhite)) / (1.0 + x));
    if (m == 1)
        return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
    if (m == 3)
        return TonemapACES2(x);
    if (m == 4)
        return TonemapAgXP(x, AgXLook);
    if (m == 5)
        return TonemapGT7P(x, GT7Shoulder, GT7LinearSection, GT7Toe);
    if (m == 6)
        return TonemapHableP(x, HableShoulder, HableToe, HableWhite);
    if (m == 7)
        return TonemapHill(x);
    if (m == 8)
        return TonemapUchimura(x, UchContrast, UchLinearStart, UchLinearLength, UchToe);
    return saturate(x);
}


// ----------------------------------------------------------------------------
// HDR & Color Range: the game's output signal <-> the work signal. The work signal is what the whole effect measures and composites in:
// sRGB-encoded light with paper white at 1.0 (above 1 in HDR). SDR: the image itself.
// ----------------------------------------------------------------------------
int OutputSignal()
{
    if (HDRInputMode != 0)
        return HDRInputMode;
    return (BUFFER_COLOR_SPACE == 2) ? 2 : ((BUFFER_COLOR_SPACE == 3) ? 3 : 1);
}
bool SignalIsHDR() { return OutputSignal() >= 2; }

float3 PQToNits(float3 n)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(n), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1.0e-6), 1.0 / m1);
}
float3 NitsToPQ(float3 l)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(l / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
static const float3x3 BT2020_TO_709 = float3x3( 1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187);
static const float3x3 BT709_TO_2020 = float3x3( 0.6274,  0.3293,  0.0433,  0.0691, 0.9195,  0.0114,  0.0164,  0.0880, 0.8956);

float3 InToWork(float3 c)
{
    int sig = OutputSignal();
    if (sig <= 1)
        return c;
    float3 lin = (sig == 2) ? c * (80.0 / HDRPaperWhite) : mul(BT2020_TO_709, PQToNits(c)) / HDRPaperWhite;
    return LinearToSRGB(max(lin, 0.0));
}

float3 WorkToOut(float3 w)
{
    int sig = OutputSignal();
    if (sig <= 1)
        return w;
    float3 lin = SRGBToLinear(max(w, 0.0)) * HDRPaperWhite;           // nits, Rec.709 primaries
    return (sig == 2) ? lin / 80.0 : NitsToPQ(max(mul(BT709_TO_2020, lin), 0.0));
}

float3 BackBufferWork(float2 uv) { return InToWork(tex2Dlod(ReShade::BackBuffer, float4(uv, 0, 0)).rgb); }

// The brightest the composite may reach in the work signal (1 in SDR, the peak in HDR).
float WorkPeak() { return SignalIsHDR() ? LinearToSRGB(float3(HDRPeakNits / HDRPaperWhite, 0, 0)).x : 1.0; }

// ----------------------------------------------------------------------------
// Tonemapper conversion: game image (display colour) -> same scene on the Output Tonemapper.
// ----------------------------------------------------------------------------

// Once per frame: x = total gain on the scene light, y = the scene light that stays where it is under Output Contrast.
float4 TMStateCalc()
{
    float gain = 1.0;
    float pivot = 0.18;
    if (ConvertTonemapper)
    {
        float s = exp2(OutputExposure);
        gain = s;
        pivot = 0.18 * s;
        if (MatchMidGrey)
        {
            // Where the Input curve puts mid grey (moved by Input Exposure) ...
            float xm = 0.18 * exp2(InputExposure);
            float target = TonemapSel(GameTonemap, float3(xm, xm, xm)).x;
            // ... and the scene light that lands there on the (tunable) Output curve, found by bisection.
            float lo = log(1.0e-5);
            float hi = log(500.0);
            [loop]
            for (int i = 0; i < 26; i++)
            {
                float mid = 0.5 * (lo + hi);
                float v = exp(mid);
                if (TonemapOutCurve(float3(v, v, v)).x < target)
                    lo = mid;
                else
                    hi = mid;
            }
            float xo = exp(0.5 * (lo + hi));
            gain = (xo / xm) * s;
            pivot = xo * s;
        }
    }
    return float4(gain, pivot, 0.0, 1.0);
}

float3 ConvertTone(float3 c)
{
    float4 st = tex2Dlod(sTM, float4(0.5, 0.5, 0, 0));
    float3 lin = SRGBToLinear(saturate(c));
    float3 x = InvToneVecSel(GameTonemap, lin) * st.x;
    if (abs(OutputContrast - 1.0) > 1.0e-3)
        x = st.y * pow(max(x / st.y, 0.0), OutputContrast);
    float3 y = TonemapOutCurve(x);
    if (abs(OutputSaturation - 1.0) > 1.0e-3)
    {
        float l = dot(y, LUMA);
        y = max(lerp(float3(l, l, l), y, OutputSaturation), 0.0);
    }
    if (KeepClippedWhites)
    {
        float cw = smoothstep(0.93, 1.0, max(lin.r, max(lin.g, lin.b)));
        y = lerp(y, max(y, lin), cw * cw);
    }
    return LinearToSRGB(saturate(y));
}

float3 TMDitherApply(float3 c, float2 pixel)
{
    if (!ConvertTonemapper || TMDither <= 0.0 || SignalIsHDR())
        return c;
    float n1 = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float n2 = frac(52.9829189 * frac(dot(pixel + 19.19, float2(0.06711056, 0.00583715))));
    return c + (n1 + n2 - 1.0) * TMDither / 255.0;
}

// ----------------------------------------------------------------------------
// View-space geometry
// ----------------------------------------------------------------------------

float2 TanHalf()
{
    float t = tan(FieldOfView * (RC_PI / 360.0));
    return float2(t * BUFFER_ASPECT_RATIO, t);
}

float3 ViewPos(float2 uv, float z) { return float3((uv * 2.0 - 1.0) * TanHalf() * z, z); }
// Same as ViewPos with TanHalf() passed in, for loops that call it many times.
float3 ViewPosT(float2 uv, float z, float2 th) { return float3((uv * 2.0 - 1.0) * th * z, z); }
// pow(x, p) for x >= 0 with fast paths for the common exponents 1 and 2.
float PowN(float x, float p) { return (p == 1.0) ? x : ((p == 2.0) ? x * x : pow(x, p)); }
float2 ViewToUV(float3 p)          { return p.xy / (p.z * TanHalf()) * 0.5 + 0.5; }
float  PixelAngle()                { return 2.0 * TanHalf().y / float(BUFFER_HEIGHT); }   // view-plane units per full-res pixel
float  TexelSize(float z)          { return z * 2.0 * TanHalf().y / float(RC_HALF_H); }  // one cascade texel at depth z
float  TexelSizeFull(float z)      { return z * PixelAngle(); }                           // one full-res pixel at depth z

float2 OctEncode(float3 n)
{
    n = float3(n.xy, -n.z) / (abs(n.x) + abs(n.y) + abs(n.z));   // camera-facing normals land mid-square
    float2 s = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return (n.z >= 0.0) ? n.xy : (1.0 - abs(n.yx)) * s;
}

float3 OctDecode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    n = normalize(n);
    return float3(n.xy, -n.z);
}

// View-space normal at one full-res pixel. Axes: x right, y DOWN the screen, z away from the
// camera; camera-facing surfaces have negative z. The smaller one-sided depth difference is used per
// axis; a step steeper than the silhouette angle is an edge, and if even the smaller side is an edge
// (silhouette on both sides, a twig) the surface is assumed to face the camera on that axis.
float4 NRotRow(int i) { return tex2Dlod(sNRot, float4((float(i) + 0.5) / 4.0, 0.5, 0, 0)); }

// The game's normal at a pixel in this shader's view space, facing the camera. ok = false where the game has none.
float3 GameNormalView(float2 uv, float3 pc, out bool ok)
{
    ok = false;
    float3 real = DecodeGameNormal(tex2Dlod(sNormalTex, float4(uv, 0, 0)));
    if (dot(real, real) <= 0.01 || IsBad(real))
        return float3(0, 0, -1);
    if (Integration_NormalSpace == 1)
    {
        if (NRotRow(3).w < 0.5)
            return float3(0, 0, -1);               // no usable fit yet: fall back to the depth rebuild
        real = float3(dot(NRotRow(0).xyz, real), dot(NRotRow(1).xyz, real), dot(NRotRow(2).xyz, real));
    }
    real = normalize(real);
    ok = true;
    return (dot(real, pc) > 0.0) ? -real : real;
}

// Normal rebuilt from depth only. edge = a silhouette was met on at least one axis (the rebuild is a guess there).
float3 ReconNormalAt(float2 uv, float zc, float tanSil, out bool edge)
{
    float3 pc = ViewPos(uv, zc);
    float2 px = ReShade::PixelSize;
    float zl = GetLinearDepth(uv - float2(px.x, 0));
    float zr = GetLinearDepth(uv + float2(px.x, 0));
    float zu = GetLinearDepth(uv - float2(0, px.y));
    float zd = GetLinearDepth(uv + float2(0, px.y));

    float edgeLim = PixelAngle() * zc / tanSil;
    float dzx = (abs(zr - zc) < abs(zc - zl)) ? zr - zc : zc - zl;
    float dzy = (abs(zd - zc) < abs(zc - zu)) ? zd - zc : zc - zu;
    edge = abs(dzx) > edgeLim || abs(dzy) > edgeLim;
    if (abs(dzx) > edgeLim)
        dzx = 0.0;
    if (abs(dzy) > edgeLim)
        dzy = 0.0;

    float3 cr = cross(ViewPos(uv + float2(0, px.y), zc + dzy) - pc, ViewPos(uv + float2(px.x, 0), zc + dzx) - pc);
    float3 n = (dot(cr, cr) > 1e-20) ? normalize(cr) : float3(0, 0, -1);   // degenerate: face the camera
    return (dot(n, pc) > 0.0) ? -n : n;
}

float3 ViewNormalAt(float2 uv, float zc, float tanSil)
{
    float3 pc = ViewPos(uv, zc);
    if (UseNormalTexture && Integration_GeometrySource >= 1)
    {
        bool ok;
        float3 real = GameNormalView(uv, pc, ok);
        if (ok)
            return real;
    }
    bool edge;
    return ReconNormalAt(uv, zc, tanSil, edge);
}

// The normal every pass uses, optionally averaged with 4 neighbours, leaving out neighbours that
// are off this pixel's tangent plane (Edge Stop).
float3 GeometryNormal(float2 uv, float zc)
{
    if (UseNormalTexture && Integration_GeometrySource >= 1)
    {
        bool ok;
        float3 gn = GameNormalView(uv, ViewPos(uv, zc), ok);
        if (ok || Integration_GeometrySource == 2)
            return gn;                              // game normal (or camera-facing): no depth rebuild, no neighbour smoothing
    }
    float tanSil = tan(SilhouetteAngle * (RC_PI / 180.0));
    float3 n = ViewNormalAt(uv, zc, tanSil);
    if (!UseSmoothedNormals)
        return n;

    static const float2 OFFS[4] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };
    float nsr = NormalSmoothTexels * RC_DIV;              // texels -> screen pixels
    float2 o = ReShade::PixelSize * nsr;
    float3 pc = ViewPos(uv, zc);
    float foot = max(TexelSizeFull(zc) * max(nsr, 1.0), 1e-6);
    float3 sum = n;
    [unroll]
    for (int i = 0; i < 4; i++)
    {
        float2 u = uv + o * OFFS[i];
        float z = max(GetLinearDepth(u), 1e-4);
        float w = 1.0;
        if (NormalSmoothEdgeStop > 0.0)
        {
            float d = dot(ViewPos(u, z) - pc, n) / foot;
            w = lerp(1.0, exp(-d * d), NormalSmoothEdgeStop);
        }
        sum += ViewNormalAt(u, z, tanSil) * w;
    }
    return (dot(sum, sum) > 1e-12) ? normalize(sum) : n;
}

// ----------------------------------------------------------------------------
// Hemisphere / cascade geometry helpers
// ----------------------------------------------------------------------------

// Probe tile size (directions per axis) of a cascade: 2, 4, 8, 16 (x BASE_TILE / 2).
int CascadeTile(int level)
{
    return CRCGI_BASE_TILE * ((level <= 0) ? 1 : ((level == 1) ? 2 : ((level == 2) ? 4 : ((level == 3) ? 8 : ((level == 4) ? 16 : 32)))));
}

// Accumulated cascades (Far Cascades: Accumulate From): own history, per-frame jitter, longer accumulation.
int FarStartLevel() { return clamp(FarAccumStart - 1, 1, CRCGI_CASCADES - 1); }
bool CascadeAccumulates(int level) { return TemporalOn && TemporalFrames > 1.0 && level >= FarStartLevel(); }

// Shirley-Chiu concentric map, unit square -> unit disk. Cells of a finer tile nest exactly inside
// cells of a coarser one, which the merge relies on.
float2 SquareToDisk(float2 xi)
{
    float2 o = xi * 2.0 - 1.0;
    float r, theta;
    if (abs(o.x) > abs(o.y))
    {
        r = o.x;
        theta = (RC_PI * 0.25) * (o.y / ((abs(r) > 1e-6) ? r : 1e-6));
    }
    else
    {
        r = o.y;
        theta = RC_PI * 0.5 - (RC_PI * 0.25) * (o.x / ((abs(r) > 1e-6) ? r : 1e-6));
    }
    return r * float2(cos(theta), sin(theta));
}

// Exact inverse of SquareToDisk.
float2 DiskToSquare(float2 d)
{
    float r = length(d);
    if (r < 1e-6)
        return float2(0.5, 0.5);

    float phi = atan2(d.y, d.x);
    if (phi < -RC_PI * 0.25)
        phi += 2.0 * RC_PI;

    float a, b;
    if (phi < RC_PI * 0.25)
    {
        a = r;
        b = a * phi / (RC_PI * 0.25);
    }
    else if (phi < RC_PI * 0.75)
    {
        b = r;
        a = (RC_PI * 0.5 - phi) * b / (RC_PI * 0.25);
    }
    else if (phi < RC_PI * 1.25)
    {
        a = -r;
        b = a * (phi - RC_PI) / (RC_PI * 0.25);
    }
    else
    {
        b = -r;
        a = (RC_PI * 0.5 - (phi - RC_PI)) * b / (RC_PI * 0.25);
    }
    return float2(a, b) * 0.5 + 0.5;
}

// Branch-free orthonormal basis (Duff et al.). Works for every unit normal.
void BuildFrame(float3 n, out float3 t, out float3 b)
{
    float s = (n.z >= 0.0) ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float bb = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
    b = float3(bb, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted direction for a cell of the probe's direction tile.
float3 DirFromXi(float2 xi, float3 n, float3 t, float3 b)
{
    float2 d = SquareToDisk(xi);
    float lz = sqrt(max(0.0, 1.0 - dot(d, d)));
    return t * d.x + b * d.y + n * lz;
}

float4 FetchC0(int2 texel) { return tex2Dlod(sC0, float4((float2(texel) + 0.5) / CASC_SIZE, 0, 0)); }

float4 FetchGeoTexel(int2 texel)
{
    return tex2Dlod(sGeo, float4((float2(texel) + 0.5) / RC_SIZE, 0, 0));
}

// Geometry of a cascade probe. Fine cascades read the centre texel of the probe's tile. Coarse cascades (tile >= 8
// texels, probes 16+ pixels apart) take the MEDIAN-depth texel of the centre and four taps around it, so a probe does not
// jump onto a twig, a pole or a far wall when the camera moves by a pixel, which is what made the far cascades flicker.
void ProbeGeometry(int level, int2 probe, out float4 g, out float2 uvp)
{
    int N = CascadeTile(level);
    int2 pc = min(probe * N + N / 2, RC_MAX_TEXEL);
    // Far Cascades: Jitter. The accumulated cascades move their probe within the middle half of its tile every frame (the same
    // offset for the cascade pass and for the merge that reads it), so the history averages the tile instead of one point.
    if (FarJitter > 0.0 && level >= 2 && CascadeAccumulates(level))
    {
        float h = frac(52.9829189 * frac(dot(float2(probe) + float(level) * 17.0, float2(0.06711056, 0.00583715))));
        float2 r2 = frac(float2(h, frac(h * 7.31 + 0.37)) + float(framecount % 256) * float2(0.7548776662, 0.5698402910));
        pc = clamp(pc + int2(floor((r2 - 0.5) * float(N) * 0.5 * FarJitter + 0.5)), int2(0, 0), RC_MAX_TEXEL);
    }
    g = FetchGeoTexel(pc);
    uvp = (float2(pc) + 0.5) / RC_SIZE;
    if (level < 2)
        return;

    int q = N / 4;
    int2 t1 = clamp(pc + int2(q, 0), int2(0, 0), RC_MAX_TEXEL);
    int2 t2 = clamp(pc - int2(q, 0), int2(0, 0), RC_MAX_TEXEL);
    int2 t3 = clamp(pc + int2(0, q), int2(0, 0), RC_MAX_TEXEL);
    int2 t4 = clamp(pc - int2(0, q), int2(0, 0), RC_MAX_TEXEL);
    float4 g1 = FetchGeoTexel(t1);
    float4 g2 = FetchGeoTexel(t2);
    float4 g3 = FetchGeoTexel(t3);
    float4 g4 = FetchGeoTexel(t4);
    float z0 = g.z;
    int r1 = int(g.z < g1.z) + int(g2.z < g1.z) + int(g3.z < g1.z) + int(g4.z < g1.z);
    int r2 = int(g.z < g2.z) + int(g1.z < g2.z) + int(g3.z < g2.z) + int(g4.z < g2.z);
    int r3 = int(g.z < g3.z) + int(g1.z < g3.z) + int(g2.z < g3.z) + int(g4.z < g3.z);
    int r4 = int(g.z < g4.z) + int(g1.z < g4.z) + int(g2.z < g4.z) + int(g3.z < g4.z);
    int r0 = int(g1.z < z0) + int(g2.z < z0) + int(g3.z < z0) + int(g4.z < z0);
    if (r0 == 2)
        return;                                   // the centre already is the median
    if (r1 == 2)      { g = g1; uvp = (float2(t1) + 0.5) / RC_SIZE; }
    else if (r2 == 2) { g = g2; uvp = (float2(t2) + 0.5) / RC_SIZE; }
    else if (r3 == 2) { g = g3; uvp = (float2(t3) + 0.5) / RC_SIZE; }
    else if (r4 == 2) { g = g4; uvp = (float2(t4) + 0.5) / RC_SIZE; }
}

// How much a direction counts as "sky-facing" (screen-up = -y). Sideways rays that merely leave the
// screen edge get ~0, so frame borders don't glow.
float SkyDirWeight(float3 d)
{
    return smoothstep(SkyDirLow, max(SkyDirHigh, SkyDirLow + 0.01), -d.y);
}

// Weight of a neighbouring probe Q for a receiver R (normal nR, depth dR at uvR).
// Relative depth: exp(-relative depth gap x K) x dot(normals)^power.
// Surface plane: distance of Q from R's tangent plane, in units of 'foot' (the probe spacing).
float ProbeWeight(float3 nR, float dR, float2 uvR, float4 gQ, float2 uvQ, float K, float foot)
{
    if (gQ.z >= SkyDepthThreshold)
        return 0.0;
    float3 nQ = OctDecode(gQ.xy);
    if (ProbeWeightMode == 0)
        return exp(-RelDepth(gQ.z, dR) * K) * PowN(saturate(dot(nR, nQ)), ProbeNormalPower);
    float d = dot(ViewPos(uvQ, gQ.z) - ViewPos(uvR, dR), nR) / max(foot, 1e-6);
    return exp(-d * d * K * 0.25) * saturate(dot(nR, nQ) * 0.75 + 0.25);
}

// ----------------------------------------------------------------------------
// Nearest / farthest surface depth in the footprint of pyramid level 'lodp' (1..3 = 2, 4, 8 cascade texels) at uv.
float2 PyrMinMax(int lodp, float2 uv)
{
    if (lodp <= 1) return tex2Dlod(sPyr1, float4(uv, 0, 0)).rg;
    if (lodp == 2) return tex2Dlod(sPyr2, float4(uv, 0, 0)).rg;
    return tex2Dlod(sPyr3, float4(uv, 0, 0)).rg;
}

// Ray march through one cascade interval. Returns 0 = escaped, 1 = hit (hitRad valid), 2 = blocked,
// 3 = left the screen onto the continued border pixel (hitRad valid, already scaled by the extension).
// ----------------------------------------------------------------------------

// Expected depth change of a ray after 'travelPx' full-res pixels on screen (x), and whether the ray
// gets that far on screen at all (y). Slope: linear in screen travel. Perspective: depth of the 3D
// ray where it projects there: z = zO * a / (a - d * zO * D.z), d in view-plane units.
float2 RayDrift(float travelPx, float3 D, float zO, float a, float pixA)
{
    float lim = RayDepthDriftLimit * DepthTolScale(zO);
    if (RayDepthModel == 0)
        return float2(clamp(travelPx / RC_DIV * RayDepthSlopeTexel * D.z, -lim, lim), 1.0);
    float den = a - travelPx * pixA * zO * D.z;
    if (den <= 1e-6)
        return float2(0.0, 0.0);   // the ray reaches its vanishing point before this
    return float2(clamp(zO * a / den - zO, -lim, lim), 1.0);
}

// Where cascade 'level' sits along the ray, in internal travel units (screen pixels / RES_SCALE; never shown in the UI).
// The penumbra criterion wants (angular spacing x end distance) = probe spacing in every cascade. Cascade c has
//           probe spacing tile_c x RC_DIV px and sqrt(2 pi)/tile_c rad between its directions (tile_c^2 directions over the
//           hemisphere), so it may reach tile_c^2 x RC_DIV / sqrt(2 pi) px: 3.2 px for cascade 0 at the defaults, x4 per
//           cascade. Cascades 1-3 (levels 0-2) always keep that layout. The far cascades 4-6 (levels 3-5) are spread
//           geometrically from the end of cascade 3 to the Cascade Reach (the logarithmic split of cascaded shadow maps),
//           so the Reach sets where they sit and how long they are, and nothing else.
float PenumbraEnd0()
{
    return float(CRCGI_BASE_TILE * CRCGI_BASE_TILE) * RC_DIV / (2.5066283 * RES_SCALE);
}

// End distance of a cascade (0 for level -1).
float CascadeEnd(int level)
{
    float e0 = PenumbraEnd0();
    if (level < 0)
        return 0.0;
    int sL = clamp(ReachStartCascade - 1, 1, CRCGI_CASCADES - 1);   // first level stretched to the reach
    if (level < sL)
        return e0 * pow(4.0, float(level));

    float eb = e0 * pow(4.0, float(sL - 1));               // end of the last penumbra-layout cascade
    float diag = sqrt(float(BUFFER_WIDTH * BUFFER_WIDTH + BUFFER_HEIGHT * BUFFER_HEIGHT)) / RES_SCALE;
    float reach = max(CascadeReach * diag, eb * 2.0);
    float growth = pow(reach / eb, 1.0 / float(max(CRCGI_CASCADES - sL, 1)));
    return eb * pow(growth, float(level - sL + 1));
}

void CascadeInterval(int level, out float rStart, out float rLen, out float gEff)
{
    float e = CascadeEnd(level);
    rStart = CascadeEnd(level - 1);
    rLen = e - rStart;
    gEff = e / max(rStart, 1.0e-3);
}

// Path occlusion: a surface blocks the ray when it is Occlusion Tolerance nearer than the receiver and within
// Occluder Depth Range of the receiver's depth (see the Occlusion & Depth Separation settings).

// Reflectance of a hit surface for the multibounce: the game's albedo (Game Integration) where it is delivered, otherwise the hue of
// the surface's pickup light at Bounce Reflectance. Never the image brightness: a dark room has dark pixels, not dark surfaces.
float3 BounceAlbedo(float4 radS, float2 uvS)
{
    if (Integration_Albedo)
    {
        float3 a = saturate(tex2Dlod(sAlbedoTex, float4(uvS, 0, 0)).rgb);
        if (dot(a, LUMA) > 1.0e-3)
            return a * MaterialDiffuse(uvS);
    }
    float l = dot(radS.rgb, LUMA);
    float3 hue = (l > 1.0e-5) ? radS.rgb / l : float3(1, 1, 1);
    hue /= max(1.0, max(hue.r, max(hue.g, hue.b)));       // keep the hue, no channel above 1
    return hue * MultibounceAlbedo;
}

// Strength of the bounce: Multibounce Strength x GI Intensity (the GI as it is shown), without the share the relit pickup already
// passes on. The returned light is held below 0.9 x the GI it came from IN EVERY CHANNEL (one common scale, so the hue is kept), so
// the loop between frames always converges, also for a saturated surface under differently coloured light.
float3 BounceGain(float3 reflected, float3 g)
{
    float3 c = reflected * (MultibounceAmount * Intensity * (1.0 - RelitBias));
    float3 lim = 0.9 * max(g, 0.0);
    float3 r = (c > 1.0e-6) ? lim / max(c, 1.0e-6) : float3(1.0e6, 1.0e6, 1.0e6);
    return c * saturate(min(r.r, min(r.g, r.b)));
}

// One tap of the multibounce patch (written out five times instead of a loop, so no loop has to be unrolled inside the ray march).
void MultibounceTap(float2 dir, float2 r, float2 uvH, float2 uvS, float zS, float3 nS, float lodT, inout float3 sum, inout float ws)
{
    float2 o = dir * r;
    float2 uvT = uvH + o;
    if (all(uvT > 0.0) && all(uvT < 1.0))
    {
        float zH = tex2Dlod(sDenIn, float4(uvT, 0, 0)).a;
        float wD = saturate(1.0 - RelDepth(zH, zS) / 0.1);
        float3 nT = OctDecode(tex2Dlod(sGeo, float4(uvS + o, 0, 0)).xy);
        float wN = PowN(saturate(dot(nT, nS)), ProbeNormalPower);
        float w = wD * lerp(1.0, wN, MultibounceSurfaceMatch);
        if (w > 1.0e-4)
        {
            sum += Clean(tex2Dlod(sDenLin, float4(uvT, 0, lodT)).rgb) * w;
            ws += w;
        }
    }
}

// Multibounce, read where a ray lands. The surface hands on the GI that last frame's result put on it (reprojected with
// the motion flow) as light of its own color. The patch it is read from is the PENUMBRA of the ray that found it: ray
// spacing is sqrt(2 pi) / tile_c radians in cascade c, so at travel r two neighbouring rays are r sqrt(2 pi) / tile_c
// apart, which equals one probe spacing exactly at the cascade's end distance. A hit is therefore resolved no finer and no
// coarser than the ray can resolve it: near hits tightly, far hits over a wider patch, so the bounce spreads in proportion
// to the distance it travels. The patch is five taps, each weighted by depth and normal agreement with the hit surface,
// so light stays on its plane (a lit floor reaches the wall of a corner by bouncing, not through the blur).
float3 MultibounceLight(float4 radS, float2 uvS, float zS, float3 nS, float travelRef, int level)
{
    float4 fl = tex2Dlod(sFlow0, float4(uvS, 0, 0));
    float2 uvH = uvS - fl.xy * ReShade::PixelSize;
    if (any(uvH <= 0.0) || any(uvH >= 1.0))
        return float3(0, 0, 0);
    float conf = exp(-max(fl.z - 0.01, 0.0) * MotionRejection);

    float wTex = max(travelRef * RES_SCALE * 2.5066283 / (float(CascadeTile(level)) * RC_DIV) * MultibounceSpread, 1.0);
    float lodT = max(clamp(log2(wTex), 0.0, 4.0) - 1.0, 0.0);   // each of the five taps covers about half the patch
    float2 r = 0.5 * wTex / RC_SIZE;

    float3 sum = float3(0, 0, 0);
    float ws = 0.0;
    MultibounceTap(float2(0, 0),   r, uvH, uvS, zS, nS, lodT, sum, ws);
    MultibounceTap(float2(1, 0),   r, uvH, uvS, zS, nS, lodT, sum, ws);
    MultibounceTap(float2(-1, 0),  r, uvH, uvS, zS, nS, lodT, sum, ws);
    MultibounceTap(float2(0, 1),   r, uvH, uvS, zS, nS, lodT, sum, ws);
    MultibounceTap(float2(0, -1),  r, uvH, uvS, zS, nS, lodT, sum, ws);
    if (ws < 1.0e-3)
        return float3(0, 0, 0);
    float3 g = sum / ws;
    g = lerp(g, dot(g, LUMA).xxx, 0.5);                       // returned light is half-desaturated, as the loop could otherwise run away
    g = ClampLuminance(g, MaxSampleLuminance * 2.0);
    return BounceGain(BounceAlbedo(radS, uvS) * g, g) * conf * saturate(ws * 0.5);
}

#if CRCGI_SECONDARY_DIVISOR > 0
// The same bounce read from the multibounce map: one fetch, at the mip that matches the penumbra of the ray that found the hit.
float3 MultibounceLightMap(float4 radS, float2 uvS, float travelRef, int level)
{
    float wTex = max(travelRef * RES_SCALE * 2.5066283 / (float(CascadeTile(level)) * RC_DIV) * MultibounceSpread, 1.0);
    float lod = clamp(log2(wTex / float(CRCGI_SECONDARY_DIVISOR)), 0.0, 4.0);
    float4 m = tex2Dlod(sMB, float4(uvS, 0, lod));
    if (m.a < 1.0e-3)
        return float3(0, 0, 0);
    float3 g = m.rgb / m.a;
    return BounceGain(BounceAlbedo(radS, uvS) * g, g) * saturate(m.a);
}
#endif

// hitW: the share of this ray that the result accounts for (1 = all of it). The rest counts as escaped and
// is answered by the parent cascade: below 1 inside the Cascade Cross-Fade zone, and for canvas / border pick-ups.
int MarchRay(float2 uvP, float dP, float3 nP, float3 D, int level, float jit, out float3 hitRad, out float hitW)
{
    hitRad = float3(0, 0, 0);
    hitW = 1.0;
    float pixA = PixelAngle();

    // Ray origin and screen direction.
    float2 uvO = uvP;
    float zO = dP;
    float a = 1.0;
    float2 dirS;
    if (RayDepthModel == 1)
    {
        float3 O = ViewPos(uvP, dP) + nP * (PerspectiveLift * TexelSize(dP));
        uvO = ViewToUV(O);
        zO = O.z;
        float2 v = O.z * D.xy - D.z * O.xy;   // projected direction of the 3D ray on the view plane
        a = length(v);
        if (a < 1e-6)
            return 0;                         // along the line of sight: never moves on screen
        dirS = v / a;
    }
    else
    {
        float dl = length(D.xy);
        dirS = (dl > 1e-4) ? D.xy / dl : float2(0.70710678, 0.70710678);
    }

    // Interval of this cascade: manual (R0, growth) or the physical penumbra-uniform layout.
    float rStart, rLen, g;
    CascadeInterval(level, rStart, rLen, g);
    if (rLen < 0.5)
        return 0;                           // beyond the reach: nothing left to trace here
    int steps = (level < 2) ? StepsNear : StepsFar;
    float stepLen = rLen / float(steps);

    // Cascade Cross-Fade: keep marching into the parent's interval with the same step, fading this cascade out there.
    int ovSteps = 0;
    if (CascadeOverlap > 0.0 && level < CRCGI_CASCADES - 1 && CascadeDebugLevel == 0)
        ovSteps = int(ceil(float(steps) * CascadeOverlap));
    float ovLen = max(float(ovSteps) * stepLen, 1.0e-3);
    float thickFixed = ThicknessThreshold;
    float stepView = stepLen * RES_SCALE * pixA;    // view-plane size of one step, for the footprint thickness

    // Width of this cascade's cone per unit of travel (2 / directions-per-axis).
    float coneScale = ConeFootprint * 2.0 / float(CascadeTile(level));
    // Falloff relative to the cascade: with the physical layout the divisor follows the cascade's own end distance, so a
    // hit at the same relative position counts the same in every cascade; manual: scale-invariant with the scale slider.
    float reachInv = 1.0 / max(CascadeEnd(CRCGI_CASCADES - 1), 1.0e-3);   // travel -> fraction of the full reach
    float softness = max(DepthSeparationSoftness, 0.02);
    float sepFar = 1.0 + DepthSeparationFarRelax * float(level);   // far hits come from other depths: relax the separation limit

    // Coarse cascades (3 and up) test against the depth range of the step's whole footprint, from the min/max pyramid.
    int lodp = int(clamp(floor(log2(max(stepLen * RES_SCALE / RC_DIV, 1.0))), 1.0, 3.0));

    float2 dr0 = RayDrift(rStart * RES_SCALE, D, zO, a, pixA);
    if (dr0.y <= 0.0)
        return 0;
    float prevDrift = dr0.x;

    [loop]
    for (int s = 0; s < 18; s++)
    {
        if (s >= steps + ovSteps)
            break;

        float travelRef = rStart + (float(s) + jit) * stepLen;   // internal travel units
        float travelPx = travelRef * RES_SCALE;
        float2 uvS = uvO + dirS * travelPx * ReShade::PixelSize;
        // 1 inside this cascade's own interval, fading to 0 across the cross-fade zone.
        float ovW = (s < steps) ? 1.0 : saturate(1.0 - (float(s - steps) + jit) * stepLen / ovLen);
        if (any(uvS < 0.0) || any(uvS >= 1.0))
        {
#if CRCGI_CANVAS_MARGIN > 0
            // Off-Screen Canvas: march on through the remembered border as through the screen (no normals there, so no
            // facing check; thickness from the object thickness and the ray's own depth change only).
            float2 uvC = ScreenToCanvas(uvS);
            if (OffscreenCanvas && all(uvC >= 0.0) && all(uvC < 1.0))
            {
                float2 drC = RayDrift(travelPx, D, zO, a, pixA);
                if (drC.y <= 0.0)
                    break;
                float stepDzC = abs(drC.x - prevDrift);
                prevDrift = drC.x;
                float4 cv = tex2Dlod(sCanvas, float4(uvC, 0, 0));
                if (cv.a <= 0.0 || cv.a >= SkyDepthThreshold)
                    continue;                       // nothing known there, or sky: the ray passes
                float cw = tex2Dlod(sCanvasW, float4(uvC, 0, 0)).r;   // how much the remembered texel is still trusted
                float dC = cv.a;
                float zRayC = zO + drC.x;
                // The canvas is coarse and remembered, so its hit band is generous (half the maximum hit thickness at least).
                float thickC = min(max(ObjectThickness, 0.5 * HitThicknessCap) * dC + stepDzC, HitThicknessCap * dC);
                if (zRayC > dC - HitFrontTolerance * thickC && zRayC < dC + thickC)
                {
                    float relSepC = (dC - dP) / (dP + DepthEps(dP));
                    float sepLimitC = DepthSeparationLimit * sepFar * ((relSepC < 0.0) ? lerp(1.0, 0.4, ForegroundBleedBlock) : 1.0);
                    float sepWC = 1.0 - smoothstep(sepLimitC * (1.0 - softness), sepLimitC, abs(relSepC));
                    if (sepWC > 0.01 && dot(cv.rgb, float3(1, 1, 1)) > 1e-5)
                    {
                        float falloffC = HitFalloff(travelRef * reachInv, dP);
                        hitW = saturate(CanvasStrength * cw) * ovW;
                        hitRad = cv.rgb * falloffC * sepWC * hitW;
                        return 3;
                    }
                }
                else if (zRayC - dC >= max(thickC, thickFixed * DepthTolScale(dC)) && dC < dP - OcclusionTolerance * DepthTolScale(dP) && (dP - dC) < OccluderDepthRange * (dP + DepthEps(dP)))
                {
                    hitW = saturate(CanvasStrength * cw) * ovW;   // remembered geometry blocks only as much as it is trusted
                    return 2;
                }
                continue;
            }
#endif
            // The ray left the screen. Continue the nearest border pixel off screen: its light counts as
            // a hit, under the same depth-separation, facing and falloff rules as an ordinary hit.
            if (ScreenEdgeExtend > 0.0 && level <= 2)   // only the short cascades: a border pixel is a poor stand-in for a far wall
            {
                float2 hp = 0.5 / RC_SIZE;
                float2 uvB = clamp(uvS, hp, 1.0 - hp);
                float4 gB = tex2Dlod(sGeo, float4(uvB, 0, 0));
                if (gB.z < SkyDepthThreshold)
                {
                    float relSep = (gB.z - dP) / (dP + DepthEps(dP));
                    float sepLimit = DepthSeparationLimit * sepFar * ((relSep < 0.0) ? lerp(1.0, 0.4, ForegroundBleedBlock) : 1.0);
                    float sepW = 1.0 - smoothstep(sepLimit * (1.0 - softness), sepLimit, abs(relSep));
                    float facingDot = dot(OctDecode(gB.xy), -D);
                    float facingWeight = lerp(1.0, saturate(facingDot * FacingCheckSlope + FacingCheckBias), FacingCheckIntensity);
                    float lod = clamp(log2(max(travelPx * coneScale / RC_DIV, 1.0)), 0.0, MaxConeMip);
                    float3 c = tex2Dlod(sRad, float4(uvB, 0, lod)).rgb * facingWeight;
                    float falloff = HitFalloff(travelRef * reachInv, dP);
                    hitW = ScreenEdgeExtend * ovW;
                    hitRad = c * falloff * sepW * hitW;
                    if (sepW > 0.01 && facingWeight > 0.01 && dot(hitRad, float3(1, 1, 1)) > 1e-5)
                    {
                        if (GBufferDebugView == 8)
                        {
                            hitRad = float3(0, 0, 0);   // Off-Screen Light Only: the border pixel is on-screen light
                            return 2;
                        }
                        return 3;
                    }
                    hitW = 1.0;
                    hitRad = float3(0, 0, 0);
                }
            }
            break;
        }

        float2 dr = RayDrift(travelPx, D, zO, a, pixA);
        if (dr.y <= 0.0)
            break;
        float drift = dr.x;
        float stepDz = abs(drift - prevDrift);   // how far the ray moved in depth during this step
        prevDrift = drift;

        float4 gS = tex2Dlod(sGeo, float4(uvS, 0, 0));
        float dS = gS.z;
        if (dS >= SkyDepthThreshold)
            continue;

        float lod = clamp(log2(max(travelPx * coneScale / RC_DIV, 1.0)), 0.0, MaxConeMip);
        float zRay = zO + drift;
        float delta = zRay - dS;   // > 0: ray is behind the surface

        // Exact early-out: a ray in front of the surface by more than the widest band that surface could ever have can
        // neither hit it (hit band starts at dS - front tolerance x thickness) nor be blocked by it (needs delta > 0).
        // Skips the normal decode, the thickness maths and the radiance fetch for every step of a ray that is still in open space.
        if (level < 3 && zRay < dS - HitFrontTolerance * HitThicknessCap * dS * (1.0 + 2.0 * DarkSurfacesBlock))
            continue;

        // Thickness, scaled once: the object's own thickness plus what one step can hide, which is the ray's own depth change
        // and the surface's depth change across one stride (dz/du = z (n.xy . s) / |n . (u,1)| for a plane, u = view-plane
        // position). A longer step gets a thicker band in proportion, as a shadow-map bias grows with the cascade's texel
        // size, so no per-cascade multiplier is needed; thickness is in camera space as in McGuire & Mara 2014.
        float3 nS = OctDecode(gS.xy);
        float2 uPlane = (uvS * 2.0 - 1.0) * TanHalf();
        float cosV = max(abs(dot(nS.xy, uPlane) + nS.z), 0.1);
        float slopeStep = dS * abs(dot(nS.xy, dirS)) / cosV * stepView;
        float thick = min(gS.w * ThinObjectScale, ObjectThickness * dS) + stepDz + min(slopeStep, 0.25 * dS);
        thick = min(thick, HitThicknessCap * dS);   // never thicker than a fraction of the surface's own distance

        // A dark surface is a shadow caster: the darker, the thicker its band (Dark Surfaces Block Rays).
        float darkMul = 1.0;
        float4 radS = float4(0, 0, 0, 0);   // this step's hit radiance (rgb) and luminance (a): fetched once, used by both checks
        bool haveRad = false;
        if (DarkSurfacesBlock > 0.0)
        {
            radS = tex2Dlod(sRad, float4(uvS, 0, lod));
            haveRad = true;
            darkMul += 2.0 * DarkSurfacesBlock * (1.0 - smoothstep(0.0, 3.0 * SurfaceBounceThreshold + 0.02, radS.a));
        }
        thick *= darkMul;

        // The band of ray depths that counts as a hit. One texel: the surface +- its thickness. Coarse cascades: the nearest
        // to the farthest surface in the footprint (so the footprint's own slope and relief need no guessed thickness) plus
        // what one step and the object's thickness can hide behind it.
        float hitLo = dS - HitFrontTolerance * thick;
        float hitHi = dS + thick;
        if (level >= 3)
        {
            float2 mm = PyrMinMax(lodp, uvS);
            if (mm.y > 0.0)
            {
                float tb = min(ObjectThickness * dS + stepDz, HitThicknessCap * dS) * darkMul;
                hitLo = mm.x - HitFrontTolerance * (tb + (mm.y - mm.x));
                hitHi = mm.y + tb;
            }
        }

        if (zRay > hitLo && zRay < hitHi)
        {
            // Depth separation between emitter and receiver (relative, signed: < 0 = emitter in front).
            float relSep = (dS - dP) / (dP + DepthEps(dP));
            float sepLimit = DepthSeparationLimit * sepFar * ((relSep < 0.0) ? lerp(1.0, 0.4, ForegroundBleedBlock) : 1.0);
            float sepW = 1.0 - smoothstep(sepLimit * (1.0 - softness), sepLimit, abs(relSep));

            if (sepW > 0.01)
            {
                // A thin object (a grass blade, a strand of hair) has no reliable normal: it is rebuilt from a depth that
                // changes within the object itself, so it flips with the camera. Its facing check fades out with its thickness.
                float reliab = saturate(gS.w / max(ObjectThickness * dS, 1.0e-4));
                float facingWeight = lerp(1.0, saturate(dot(nS, -D) * FacingCheckSlope + FacingCheckBias), FacingCheckIntensity * reliab);

                if (!haveRad)
                    radS = tex2Dlod(sRad, float4(uvS, 0, lod));
                float3 c = radS.rgb;
                if (Multibounce && MultibounceAmount > 0.0 && level >= MultibounceFromCascade - 1)
                {
#if CRCGI_SECONDARY_DIVISOR > 0
                    c += MultibounceLightMap(radS, uvS, travelRef, level);   // second cascade and up: the long-range bounce, from the map
#else
                    c += MultibounceLight(radS, uvS, dS, nS, travelRef, level);   // ... or read exactly, surface by surface
#endif
                }
                c *= facingWeight;

                if (GBufferDebugView == 8 && facingWeight > 0.01)
                {
                    hitW = ovW;                         // Off-Screen Light Only: on-screen surfaces block but give no light
                    return 2;
                }
                if (facingWeight > 0.01 && dot(c, float3(1, 1, 1)) > 1e-5)
                {
                    float falloff = HitFalloff(travelRef * reachInv, dP);
                    hitRad = c * falloff * sepW * ovW;
                    hitW = ovW;
                    return 1;
                }
                if (DarkSurfacesBlock > 0.0 && facingWeight > 0.01)
                {
                    hitW = ovW;
                    return 2;
                }
            }
        }
        else if (delta >= max(thick, thickFixed * DepthTolScale(dS)) && dS < dP - OcclusionTolerance * DepthTolScale(dP) && (dP - dS) < OccluderDepthRange * (dP + DepthEps(dP)))
        {
            // Only a surface near the receiver's own depth can block; something far in front (a
            // character in front of a wall) just overlaps on screen. A ray passing behind a thin
            // object (between its own thickness and the fixed band) is neither hit nor blocked.
            hitW = ovW;
            return 2;
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
// One cascade texel = one (probe, direction) pair.
// ----------------------------------------------------------------------------

struct ProbeInfo
{
    bool ok;
    int2 texel;
    int2 probe;
    int2 cell;
    float3 n;
    float d;
    float3 T;
    float3 B;
    float2 uv;
};

// Finds this texel's probe. ok = false if there is nothing to trace (sky / faded out).
ProbeInfo SetupProbe(int level, float2 uv)
{
    ProbeInfo p;
    int N = CascadeTile(level);
    p.texel = int2(floor(uv * CASC_SIZE));
    p.probe = p.texel / N;
    p.cell = p.texel - p.probe * N;

    float4 g;
    float2 puv;
    ProbeGeometry(level, p.probe, g, puv);
    p.uv = puv;
    p.n = OctDecode(g.xy);
    p.d = g.z;
    BuildFrame(p.n, p.T, p.B);
    p.ok = (p.d < SkyDepthThreshold) && (GetDistanceFade(p.d) > 0.001);
    return p;
}

// How far this probe's history can be trusted: last frame's geometry at the position the picture moved from
// must match the probe's depth and normal, and the motion match must be good. 0 = history lost (new area), 1 = valid.
float ProbeHistoryValid(ProbeInfo p)
{
    float4 fl = tex2Dlod(sFlow0, float4(p.uv, 0, 0));
    float2 uvPrev = p.uv - fl.xy * ReShade::PixelSize;
    if (any(uvPrev < 0.0) || any(uvPrev >= 1.0))
        return 0.0;
    float4 gPrev = tex2Dlod(sGeoPrev, float4(uvPrev, 0, 0));
    float depthRel = RelDepth(gPrev.z, p.d);
    float nd = dot(OctDecode(gPrev.xy), p.n);
    return saturate(1.0 - depthRel / max(TemporalDepthGuard, 1.0e-3)) * saturate((nd - 0.7) / 0.3)
         * exp(-max(fl.z - 0.01, 0.0) * MotionRejection);
}

// Adaptive Ray Count: rays per cell for this probe.
int ProbeRayCount(ProbeInfo p)
{
    int n = clamp(RaysPerCell, 1, 4);
    if (AdaptiveRays == 0 || !TemporalOn)
        return n;
    float v = ProbeHistoryValid(p);
    if (v < 0.5)
        return min(n + AdaptiveRaysExtra, 4);
    if (AdaptiveRays == 2)
        return max(n - 1, 1);
    return n;
}

// One stratified sub-ray of this texel's cell. 0 = escaped, 1 = hit, 2 = blocked, 3 = border / canvas pick-up.
int TraceSub(int level, int sub, int count, ProbeInfo p, out float3 hitRad, out float hitW)
{
    int N = CascadeTile(level);

    float2 subOff = float2(0.5, 0.5);
    if (count > 1)
        subOff = (sub == 0) ? float2(0.25, 0.25) : ((sub == 1) ? float2(0.75, 0.75)
               : ((sub == 2) ? float2(0.75, 0.25) : float2(0.25, 0.75)));

    // Ray start offset: interleaved gradient noise of the texel, static unless Ray Jitter Accumulation moves it each frame.
    float ign = frac(52.9829189 * frac(dot(float2(p.texel) + float2(11.0, 29.0) * float(level)
                                              + float2(37.0, 17.0) * float(sub), float2(0.06711056, 0.00583715))));
    float jitSel = 0.1 + 0.8 * ign;
    float2 dOff = float2(0.0, 0.0);
    float jitAmt = (CascadeAccumulates(level) && level >= 2) ? max(TemporalJitter, FarJitter) : TemporalJitter;
    if (TemporalOn && jitAmt > 0.0)
    {
        // Low-discrepancy (R2) rotation per frame: the temporal history averages the samples. Directions move within their
        // own cell, so the merge between cascades still sees the same cells.
        float2 r2 = frac(float2(ign, frac(ign * 7.31 + 0.37)) + float(framecount % 64) * float2(0.7548776662, 0.5698402910));
        jitSel = lerp(jitSel, 0.1 + 0.8 * r2.x, jitAmt);
        dOff = (r2 - 0.5) * (0.5 / float(N)) * jitAmt;
    }

    float jit = lerp(0.5, jitSel, StartJitterAmount);
    float2 xi = (float2(p.cell) + subOff) / float(N) + dOff;

    float3 D = DirFromXi(xi, p.n, p.T, p.B);
    return MarchRay(p.uv, p.d, p.n, D, level, jit, hitRad, hitW);
}

// All sub-rays of this texel's cell. rgb = mean hit radiance, a = fraction of rays that escaped.
float4 TraceCell(int level, ProbeInfo p)
{
    float3 hitSum = float3(0, 0, 0);
    float misses = 0.0;
    int count = ProbeRayCount(p);
    [loop]
    for (int sub = 0; sub < 4; sub++)
    {
        if (sub >= count)
            break;
        float3 hr;
        float hw;
        int r = TraceSub(level, sub, count, p, hr, hw);
        if (r == 0)
            misses += 1.0;
        else
        {
            // hit (1), blocked (2, hr = 0) or border / canvas pick-up (3): the part of the ray the result does not
            // account for (cross-fade zone, partial pick-up) still counts as escaped and goes to the parent cascade.
            hitSum += hr;
            misses += 1.0 - hw;
        }
    }
    float inv = 1.0 / float(max(count, 1));
    return float4(hitSum * inv, misses * inv);
}


// ----------------------------------------------------------------------------
// Merge: this ray's 4 child directions, looked up in the 4 bilinearly-weighted parent probes
// (re-expressed in each parent's own hemisphere frame). One copy per parent cascade, because a pass
// may not reference the texture it renders to.
// ----------------------------------------------------------------------------

// Merge Visibility: is the straight segment from the child probe (A) to a parent probe (B) clear? Three points
// along it (interpolated in view space, then projected) are compared with the depth buffer; a surface that sticks out in
// front of the segment by more than the tolerance blocks it. Returns 1 = clear, down to 1 - Merge Visibility = hidden.
float SegmentVisibility(float2 uvA, float zA, float2 uvB, float zB)
{
    if (MergeVisibility <= 0.0 || zB >= SkyDepthThreshold)
        return 1.0;
    float2 th = TanHalf();
    float3 A = ViewPosT(uvA, zA, th);
    float3 B = ViewPosT(uvB, zB, th);
    float tol = max(MergeVisibilityTolerance, 1.0e-3);
    float occ = 0.0;
    [unroll]
    for (int i = 1; i <= 3; i++)
    {
        float3 P = lerp(A, B, float(i) * 0.25);
        float2 u = P.xy / (P.z * th) * 0.5 + 0.5;
        float zs = tex2Dlod(sGeo, float4(u, 0, 0)).z;
        occ = max(occ, saturate((P.z - zs) / (tol * P.z) - 1.0));   // starts at 1x the tolerance, full at 2x
    }
    return 1.0 - MergeVisibility * occ;
}


#define CRCGI_DEFINE_MERGE(NAME, PARENT_SAMPLER) \
float4 NAME(int level, int2 probe, int2 cell, float3 nP, float dP, float3 T, float3 B, float2 uvP) \
{ \
    int N = CascadeTile(level); \
    int Np = CascadeTile(level + 1); \
    int2 maxProbe = int2(RC_TEX_W / Np - 1, RC_TEX_H / Np - 1); \
    float2 gp = (float2(probe) - 0.5) * 0.5; \
    int2 i0 = int2(floor(gp)); \
    float2 f = gp - float2(i0); \
    float K = ProbeDepthSharpness / (1.0 + MergeSharpnessFalloff * float(level)); \
    float foot = TexelSize(dP) * float(Np); \
    int2 pIdx[4]; \
    float pW[4]; \
    float3 pT[4]; \
    float3 pB[4]; \
    float3 pN[4]; \
    [unroll] \
    for (int j = 0; j < 4; j++) \
    { \
        int2 o = int2(j % 2, j / 2); \
        int2 idx = clamp(i0 + o, int2(0, 0), maxProbe); \
        float2 bw = lerp(1.0 - f, f, float2(o)); \
        float4 gP; \
        float2 puv; \
        ProbeGeometry(level + 1, idx, gP, puv); \
        float3 nQ = OctDecode(gP.xy); \
        float3 tt, bt; \
        BuildFrame(nQ, tt, bt); \
        pW[j] = bw.x * bw.y * (ProbeWeight(nP, dP, uvP, gP, puv, K, foot) * SegmentVisibility(uvP, dP, puv, gP.z) + 1e-5); \
        pIdx[j] = idx; \
        pN[j] = nQ; \
        pT[j] = tt; \
        pB[j] = bt; \
    } \
    float4 acc = float4(0, 0, 0, 0); \
    [unroll] \
    for (int k = 0; k < 4; k++) \
    { \
        float2 kk = float2(k % 2, k / 2); \
        float3 Dk = DirFromXi((float2(cell) + 0.25 + 0.5 * kk) / float(N), nP, T, B); \
        float4 sumV = float4(0, 0, 0, 0); \
        float sumW = 0.0; \
        [unroll] \
        for (int j2 = 0; j2 < 4; j2++) \
        { \
            float3 lp = float3(dot(Dk, pT[j2]), dot(Dk, pB[j2]), dot(Dk, pN[j2])); \
            if (lp.z > MergeHorizon) \
            { \
                float2 fc = clamp(DiskToSquare(lp.xy) * float(Np), float2(0.5, 0.5), float2(float(Np) - 0.5, float(Np) - 0.5)); \
                float2 cc = BilinearDirectionMerge ? fc : (floor(fc) + 0.5); \
                sumV += tex2Dlod(PARENT_SAMPLER, float4((float2(pIdx[j2] * Np) + cc) / CASC_SIZE, 0, 0)) * pW[j2]; \
                sumW += pW[j2]; \
            } \
        } \
        acc += (sumW > 1e-9) ? (sumV / sumW) : float4(0, 0, 0, 1); \
    } \
    return acc * 0.25; \
}

// Energy Carry: the light a parent cascade hands down is multiplied by Energy Carry raised to the interval ratio
// of the two cascades (log4 of their end distances = 1 per x4 penumbra step), so the weight per merge follows the
// geometry of the cascade layout and not the cascade count. Only the light is scaled; the visibility stays as traced.
float4 CarryParent(int level, float4 parent)
{
    if (level + 2 < EnergyCarryStart)                       // the parent is cascade level + 2 (1 = nearest)
        return parent;
    float ratio = CascadeEnd(level + 1) / max(CascadeEnd(level), 1.0e-3);
    float g = pow(MergeEnergyCarry, 0.5 * log2(max(ratio, 1.0)));
    return float4(parent.rgb * g, parent.a);
}

CRCGI_DEFINE_MERGE(MergeParent_0, sC1)
CRCGI_DEFINE_MERGE(MergeParent_1, sC2)
CRCGI_DEFINE_MERGE(MergeParent_2, sC3)
CRCGI_DEFINE_MERGE(MergeParent_3, sC4)
CRCGI_DEFINE_MERGE(MergeParent_4, sC5)

// ----------------------------------------------------------------------------
// Pass: once-per-frame sky tint
// ----------------------------------------------------------------------------

void SkyStateCalc(out float4 skyOut)
{
    static const float2 GRID[6] = {
        float2(0.2, 0.05), float2(0.5, 0.05), float2(0.8, 0.05),
        float2(0.2, 0.2),  float2(0.5, 0.2),  float2(0.8, 0.2)
    };
    float3 sum = float3(0, 0, 0);
    float count = 0.0;
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        if (GetLinearDepth(GRID[i]) >= SkyDepthThreshold)
        {
            sum += BackBufferWork(GRID[i]);
            count += 1.0;
        }
    }

    float3 custom = max(SkyCustomColor, 0.001);
    float3 customChroma = custom / max(dot(custom, LUMA), 0.01);

    float3 sampledChroma = customChroma;   // no sky on screen: fall back to the custom tint, not grey
    if (count > 0.0)
    {
        float3 avg = sum / count;
        if (ConvertTonemapper && PickupAfterConversion)
            avg = ConvertTone(avg);   // the sky tint of the converted image
        avg = max(avg, 0.001);
        sampledChroma = avg / max(dot(avg, LUMA), 0.01);
    }

    float3 chroma;
    if (SkyColorSource == 0)      chroma = sampledChroma;
    else if (SkyColorSource == 1) chroma = customChroma;
    else                          chroma = lerp(sampledChroma, customChroma, SkyBlend);

    chroma = max(lerp(float3(1, 1, 1), chroma, SkySaturation), 0.0);

    skyOut = float4(chroma, 1.0);
}

// The two 1x1 states, in two passes: the sky tint reads this frame's tonemapper state (through ConvertTone when Convert
// Tonemapper and Pickup After Conversion are on), so it must not be written in the same pass. (One combined pass sampled
// CRCGIV1_TM while rendering to it: error X3020, and on D3D11 the read was unbound.)
// z = exposure that matches the game's HDR scene light to the image (0 = not delivered / no match), w = display / scene ratio of
// mid grey on the Input Tonemapper (normalises the HDR Light Pickup).
float4 PS_TM(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float4 st = TMStateCalc();
    st.w = TonemapSel(GameTonemap, float3(0.18, 0.18, 0.18)).x / 0.18;
    st.z = 0.0;
    if (Integration_HDRScene)
    {
        // Log-average ratio over an 8 x 8 grid of mid-tone, non-sky pixels: mid tones of the scene light land where the image has them.
        float sumLog = 0.0, n = 0.0;
        [loop]
        for (int j = 0; j < 8; j++)
        {
            [loop]
            for (int i = 0; i < 8; i++)
            {
                float2 u = (float2(i, j) + 0.5) / 8.0;
                if (GetLinearDepth(u) >= SkyDepthThreshold)
                    continue;
                float b = dot(SRGBToLinear(max(BackBufferWork(u), 0.0)), LUMA);
                float sl = dot(tex2Dlod(sHDRSceneTex, float4(u, 0, 0)).rgb, LUMA);
                if (b > 0.01 && b < 0.8 && sl > 1.0e-6 && sl < 1.0e6)
                {
                    sumLog += log(b / sl);
                    n += 1.0;
                }
            }
        }
        if (n >= 4.0)
            st.z = exp(sumLog / n);
    }
    return st;
}

float4 PS_Sky(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float4 sky;
    SkyStateCalc(sky);
    return sky;
}

// ----------------------------------------------------------------------------
// Pass: sky map. Each map texel averages the sky pixels in its patch of the screen (3 x 3 taps). Patches with
// no sky keep last frame's colour with fading confidence; the mips (premultiplied by confidence) fill what is left.
// ----------------------------------------------------------------------------

float4 PS_SkyMap(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float2 cell = 1.0 / float2(SKYMAP_W, SKYMAP_H);
    float3 sum = float3(0, 0, 0);
    float cnt = 0.0;
    [unroll]
    for (int j = 0; j < 3; j++)
    {
        [unroll]
        for (int i = 0; i < 3; i++)
        {
            float2 u = uv + (float2(i, j) - 1.0) * cell / 3.0;
            if (GetLinearDepth(u) >= SkyDepthThreshold)
            {
                sum += BackBufferWork(u);
                cnt += 1.0;
            }
        }
    }

    float4 prev = tex2Dlod(sSkyMapPrev, float4(uv, 0, 0));
    float3 prevCol = prev.rgb / max(prev.a, 1.0e-4);
    bool prevOk = prev.a > 0.01 && !IsBad(prevCol);
    if (cnt > 0.0)
    {
        float3 c = sum / cnt;
        if (ConvertTonemapper && PickupAfterConversion)
            c = ConvertTone(c);                   // the sky of the converted image, as the tint
        c = max(c, 0.0);
        if (prevOk)
            c = lerp(prevCol, c, 0.35);           // a little smoothing against flicker
        float cov = cnt / 9.0;
        float conf = saturate(cov + (prevOk ? prev.a * 0.98 : 0.0) * (1.0 - cov));
        return float4(Clean(c) * conf, conf);
    }
    if (!prevOk)
        return float4(0, 0, 0, 0);
    float confOld = prev.a * 0.98;
    return float4(prevCol * confOld, confOld);
}

float4 PS_CopySkyMap(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    return tex2Dlod(sSkyMap, float4(uv, 0, 0));
}

// Sky light colour for a view-space direction D: the sky map where D points (a point infinitely far along D;
// directions that do not point into the view frustum use the screen border they lean towards), shaped by Sky Color
// Source, Sky Saturation and Sky Map Brightness Variation. 'fallback' (the plain tint) is used where no sky is known.
float3 SkyDirectionalTint(float3 D, float3 fallback)
{
    float2 q = D.xy / TanHalf();
    float2 u;
    if (D.z > 0.05)
        u = q / D.z * 0.5 + 0.5;
    else
        u = q / max(max(abs(q.x), abs(q.y)), 1.0e-4) * 0.5 + 0.5;
    u = clamp(u, 0.0, 1.0);

    float4 v = float4(0, 0, 0, 0);
    [loop]
    for (int m = 0; m < 7; m++)
    {
        v = tex2Dlod(sSkyMap, float4(u, 0, m));
        if (v.a > 0.02)
            break;
    }
    if (v.a <= 0.02)
        return fallback;

    float3 c = max(v.rgb / v.a, 0.001);
    float l = dot(c, LUMA);
    float4 avg = tex2Dlod(sSkyMap, float4(0.5, 0.5, 0, 6));
    float avgL = (avg.a > 1.0e-4) ? dot(avg.rgb / avg.a, LUMA) : l;

    float3 chroma = c / max(l, 0.01);
    float3 custom = max(SkyCustomColor, 0.001);
    float3 customChroma = custom / max(dot(custom, LUMA), 0.01);
    if (SkyColorSource == 1)      chroma = customChroma;
    else if (SkyColorSource == 2) chroma = lerp(chroma, customChroma, SkyBlend);
    chroma = max(lerp(float3(1, 1, 1), chroma, SkySaturation), 0.0);

    float b = clamp(pow(max(l, 1.0e-4) / max(avgL, 1.0e-4), SkyMapBrightness), 0.0, 4.0);
    return chroma * b;
}

// ----------------------------------------------------------------------------
// Pass: off-screen canvas. On screen: this frame's hit light and depth. Off screen: last frame's canvas, moved
// with the motion flow at the nearest screen border and faded in trust (not in brightness) by Canvas Memory (faster where the motion match is
// poor). Content that was never seen stays empty (depth 0), and rays pass through it.
// ----------------------------------------------------------------------------

void PS_Canvas(float4 pos : SV_Position, float2 uv : TexCoord, out float4 canvas : SV_Target0, out float4 conf : SV_Target1)
{
    float2 us = CanvasToScreen(uv);
    canvas = float4(0, 0, 0, 0);
    conf = float4(0, 0, 0, 0);
    if (all(us >= 0.0) && all(us < 1.0))
    {
        canvas = float4(tex2Dlod(sRad, float4(us, 0, 0)).rgb, tex2Dlod(sGeo, float4(us, 0, 0)).z);
        conf = float4(1, 0, 0, 0);
        return;
    }
    if (!OffscreenCanvas)
        return;

    float2 hp = 0.5 / RC_SIZE;
    float4 fl = tex2Dlod(sFlow0, float4(clamp(us, hp, 1.0 - hp), 0, 0));
    float2 ucPrev = ScreenToCanvas(us - fl.xy * ReShade::PixelSize);
    if (any(ucPrev < 0.0) || any(ucPrev >= 1.0))
        return;
    float4 h = tex2Dlod(sCanvasPrev, float4(ucPrev, 0, 0));
    float w = tex2Dlod(sCanvasWPrev, float4(ucPrev, 0, 0)).r * CanvasPersistence * exp(-max(fl.z - 0.01, 0.0) * MotionRejection);
    if (h.a <= 0.0 || IsBad(h.rgb) || w < 0.03)
        return;
    canvas = h;                                      // the light stays as it was seen; only the confidence fades
    conf = float4(w, 0, 0, 0);
}

void PS_CopyCanvas(float4 pos : SV_Position, float2 uv : TexCoord, out float4 canvas : SV_Target0, out float4 conf : SV_Target1)
{
    canvas = tex2Dlod(sCanvas, float4(uv, 0, 0));
    conf = tex2Dlod(sCanvasW, float4(uv, 0, 0));
}

// ----------------------------------------------------------------------------
// Motion flow: block matching on a luminance pyramid, coarse to fine. Each level searches a small window around the
// (bilinearly upsampled) answer of the coarser one, comparing a 5-tap cross of the current frame's luminance with the
// previous frame's at the displaced position; a small preference for the predicted flow decides flat regions.
// ----------------------------------------------------------------------------

float4 PS_Luma(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    return float4(dot(BackBufferWork(uv), LUMA), 0, 0, 1);
}

#define CRCGI_DEFINE_FLOW(NAME, LOD, RADIUS, PRED) \
float4 NAME(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target \
{ \
    if (Integration_Motion) return GameMotionFlow(uv, LOD); \
    float texPx = exp2(float(LOD + 1)); \
    float2 texUV = texPx * ReShade::PixelSize; \
    float2 pred = PRED; \
    float best = 1.0e9; \
    float bestPatch = 0.0; \
    float2 bestM = pred; \
    [loop] \
    for (int oy = -RADIUS; oy <= RADIUS; oy++) \
    { \
        [loop] \
        for (int ox = -RADIUS; ox <= RADIUS; ox++) \
        { \
            float2 m = pred + float2(float(ox), float(oy)) * texPx; \
            float2 uvP = uv - m * ReShade::PixelSize; \
            float patch = abs(tex2Dlod(sLuma, float4(uv, 0, LOD)).r - tex2Dlod(sLumaPrev, float4(uvP, 0, LOD)).r) \
                        + abs(tex2Dlod(sLuma, float4(uv + float2(texUV.x, 0), 0, LOD)).r - tex2Dlod(sLumaPrev, float4(uvP + float2(texUV.x, 0), 0, LOD)).r) \
                        + abs(tex2Dlod(sLuma, float4(uv - float2(texUV.x, 0), 0, LOD)).r - tex2Dlod(sLumaPrev, float4(uvP - float2(texUV.x, 0), 0, LOD)).r) \
                        + abs(tex2Dlod(sLuma, float4(uv + float2(0, texUV.y), 0, LOD)).r - tex2Dlod(sLumaPrev, float4(uvP + float2(0, texUV.y), 0, LOD)).r) \
                        + abs(tex2Dlod(sLuma, float4(uv - float2(0, texUV.y), 0, LOD)).r - tex2Dlod(sLumaPrev, float4(uvP - float2(0, texUV.y), 0, LOD)).r); \
            patch *= 0.2; \
            float cost = patch + 0.004 * length(float2(float(ox), float(oy))); \
            if (cost < best) \
            { \
                best = cost; \
                bestPatch = patch; \
                bestM = m; \
            } \
        } \
    } \
    return float4(bestM, bestPatch, 1.0); \
}

CRCGI_DEFINE_FLOW(PS_Flow3, 3, 4, float2(0.0, 0.0))
CRCGI_DEFINE_FLOW(PS_Flow2, 2, 2, tex2Dlod(sFlow3, float4(uv, 0, 0)).xy)
CRCGI_DEFINE_FLOW(PS_Flow1, 1, 2, tex2Dlod(sFlow2, float4(uv, 0, 0)).xy)
CRCGI_DEFINE_FLOW(PS_Flow0, 0, 1, tex2Dlod(sFlow1, float4(uv, 0, 0)).xy)



// Min / max depth pyramid of the geometry (sky ignored), levels of 2, 4 and 8 cascade texels.
float2 PyrLeaf(float z) { return (z < SkyDepthThreshold) ? float2(z, z) : float2(1.0, 0.0); }

float4 PS_Pyr1(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float2 o = 0.5 / RC_SIZE;
    float2 a = PyrLeaf(tex2Dlod(sGeo, float4(uv + float2(-o.x, -o.y), 0, 0)).z);
    float2 b = PyrLeaf(tex2Dlod(sGeo, float4(uv + float2( o.x, -o.y), 0, 0)).z);
    float2 c = PyrLeaf(tex2Dlod(sGeo, float4(uv + float2(-o.x,  o.y), 0, 0)).z);
    float2 d = PyrLeaf(tex2Dlod(sGeo, float4(uv + float2( o.x,  o.y), 0, 0)).z);
    return float4(min(min(a.x, b.x), min(c.x, d.x)), max(max(a.y, b.y), max(c.y, d.y)), 0, 1);
}

float4 PS_Pyr2(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float2 o = 1.0 / RC_SIZE;
    float2 a = tex2Dlod(sPyr1, float4(uv + float2(-o.x, -o.y), 0, 0)).rg;
    float2 b = tex2Dlod(sPyr1, float4(uv + float2( o.x, -o.y), 0, 0)).rg;
    float2 c = tex2Dlod(sPyr1, float4(uv + float2(-o.x,  o.y), 0, 0)).rg;
    float2 d = tex2Dlod(sPyr1, float4(uv + float2( o.x,  o.y), 0, 0)).rg;
    return float4(min(min(a.x, b.x), min(c.x, d.x)), max(max(a.y, b.y), max(c.y, d.y)), 0, 1);
}

float4 PS_Pyr3(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float2 o = 2.0 / RC_SIZE;
    float2 a = tex2Dlod(sPyr2, float4(uv + float2(-o.x, -o.y), 0, 0)).rg;
    float2 b = tex2Dlod(sPyr2, float4(uv + float2( o.x, -o.y), 0, 0)).rg;
    float2 c = tex2Dlod(sPyr2, float4(uv + float2(-o.x,  o.y), 0, 0)).rg;
    float2 d = tex2Dlod(sPyr2, float4(uv + float2( o.x,  o.y), 0, 0)).rg;
    return float4(min(min(a.x, b.x), min(c.x, d.x)), max(max(a.y, b.y), max(c.y, d.y)), 0, 1);
}

// ----------------------------------------------------------------------------
// Pass: geometry and hit light at cascade resolution
// ----------------------------------------------------------------------------

// The game's HDR scene light (Game Integration) is used for the pickup when it is delivered and the exposure match found a value.
bool SceneLightOn() { return Integration_HDRScene && tex2Dlod(sTM, float4(0.5, 0.5, 0, 0)).z > 0.0; }

// Light a ray picks up at u, as a work signal: the game's HDR scene light (exposure-matched, sRGB-encoded, above 1 for highlights)
// or the finished image.
float3 PickupSample(float2 u, bool sceneLight)
{
    if (sceneLight)
    {
        float3 sc = tex2Dlod(sHDRSceneTex, float4(u, 0, 0)).rgb;
        if (!IsBad(sc))
            return LinearToSRGB(max(sc * tex2Dlod(sTM, float4(0.5, 0.5, 0, 0)).z, 0.0));
    }
    return BackBufferWork(u);
}

// HDR Light Pickup: how much brighter than the work signal shows it a picked-up colour really is. 1 at mid grey and below.
// Signals that already hold the range (HDR output, the game's scene light): the part above paper white. SDR: the inverse of the
// game's tonemapper (the Input Tonemapper), normalised so mid grey stays where it is.
float PickupExpansion(float3 work, bool rangeKnown)
{
    if (rangeKnown || SignalIsHDR())
        return clamp(dot(work, LUMA) / max(dot(min(work, 1.0), LUMA), 1.0e-4), 1.0, HDRPickupRange);
    float3 lin = SRGBToLinear(saturate(work));
    float midScale = tex2Dlod(sTM, float4(0.5, 0.5, 0, 0)).w;        // display / scene ratio of mid grey
    float lm = dot(lin, LUMA);
    if (lm <= 0.18 * midScale)
        return 1.0;                       // at or below mid grey: no expansion (a curve's toe would otherwise "expand" the darks)
    float3 sc = InvToneVecSel(GameTonemap, lin);
    return clamp(dot(sc, LUMA) / max(lm, 1.0e-4) * midScale, 1.0, HDRPickupRange);
}

// Takes the nearest of the full-res depths under the texel (thin geometry survives, no blended
// in-between depths), and the normal, thickness and colour of that same pixel.
void PS_Prep(float4 pos : SV_Position, float2 uv : TexCoord, out float4 geo : SV_Target0, out float4 rad : SV_Target1)
{
    float2 px = ReShade::PixelSize;
    float zc = 2.0;
    float2 uvc = uv;
    float zsum = 0.0;
    [unroll]
    for (int j = 0; j < CRCGI_RESOLUTION_DIVISOR; j++)
    {
        [unroll]
        for (int i = 0; i < CRCGI_RESOLUTION_DIVISOR; i++)
        {
            float2 u = uv + (float2(i, j) - 0.5 * (RC_DIV - 1.0)) * px;
            float z = GetLinearDepth(u);
            zsum += z;
            if (z < zc)
            {
                zc = z;
                uvc = u;
            }
        }
    }
    if (DownsampleDepthMode == 1)
    {
        zc = zsum / (RC_DIV * RC_DIV);
        uvc = uv;
    }
    zc = max(zc, 1e-4);   // depth 0 (camera inside geometry) would give 0/0

    float3 n = GeometryNormal(uvc, zc);

    // Thickness from the silhouette: opposing depth edges along x or y provide a screen-space
    // chord estimate (2*sqrt(d1*d2)). It is not physical mesh thickness; a single wall border
    // does not constrain the estimate. Skipped unless this mode or the thickness debug view uses it.
    // Generic thickness skips this. Debug view 9 still computes the measurement so it remains useful in either mode.
    float thick = 1e4;
    if (HitThicknessMode == 1 || DebugView == 9)
    {
        float3 pc = ViewPos(uvc, zc);
        float dEdge[4];
        [unroll]
        for (int k = 0; k < 4; k++)
        {
            float2 dir = float2((k == 0) ? 1.0 : ((k == 1) ? -1.0 : 0.0), (k == 2) ? 1.0 : ((k == 3) ? -1.0 : 0.0));
            dEdge[k] = 1e4;
            [unroll]
            for (int r = 0; r < 6; r++)
            {
                float dist = float(1 << r);
                if (r < SilhouetteSearchSteps && dEdge[k] > 1e3)
                {
                    float2 u = uvc + dir * dist * px;
                    float zp = dot(pc, n) / min(dot(ViewPos(u, 1.0), n), -1e-3);   // depth of the tangent plane there
                    if (GetLinearDepth(u) > max(zp, zc) + SilhouetteDepthGap * zc)
                        dEdge[k] = dist;
                }
            }
        }
        float cx = (dEdge[0] < 1e3 && dEdge[1] < 1e3) ? dEdge[0] * dEdge[1] : 1e8;
        float cy = (dEdge[2] < 1e3 && dEdge[3] < 1e3) ? dEdge[2] * dEdge[3] : 1e8;
        thick = min(2.0 * sqrt(min(cx, cy)) * PixelAngle() * zc, 1e4);
    }

    // Light this surface gives a ray that hits it, pre-filtered around the same pixel.
    float2 texel = px * (PickupBlurTexels * RC_DIV);
    static const float2 OFFS[5] = { float2(0, 0), float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };
    float3 sumD = float3(0, 0, 0);
    bool sceneLight = SceneLightOn();
    [unroll]
    for (int t = 0; t < 5; t++)
        sumD += PickupSample(uvc + texel * OFFS[t], sceneLight);
    float3 avg = sumD / 5.0;
    if (ConvertTonemapper && PickupAfterConversion && !sceneLight && !SignalIsHDR())
        avg = ConvertTone(avg);   // rays pick up the converted image
    float3 display = pow(max(avg, 1e-4), PreFilterGamma);
    float rawLum = dot(display, LUMA);

    // Lit-surface and emitter weighting. The surface weight is continuous in the luminance (a saturating curve, half
    // weight at the threshold), so a small change in shading from wind, the camera or a moving character can never flip
    // a pixel between 'source' and 'not a source' (that cliff made grass tips glow and their bases go dark).
    float surfaceWeight = rawLum / (rawLum + SurfaceBounceThreshold + 1.0e-4);
    float emitterRange = max(EmitterThreshold / EmitterSensitivity, 0.01);
    float emitterFactor = smoothstep(EmitterThreshold, EmitterThreshold + emitterRange * 0.5, rawLum);
    float weight = lerp(surfaceWeight, surfaceWeight * EmitterBoost, emitterFactor);
    // HDR Light Pickup: bright sources keep (part of) the range the SDR image squeezed out of them.
    if (HDRPickupBoost > 0.0)
        weight *= lerp(1.0, PickupExpansion(avg, sceneLight), HDRPickupBoost);
    float3 light = ClampLuminance(display, MaxSampleLuminance) * weight;

    // Relit pickup. Where last frame's GI is known for this surface, the light it hands on is built from what this effect
    // knows instead of from the game's baked lighting: reflectance (the pixel's hue and its detail relative to the plane-local
    // average, so the slowly-changing baked lighting is divided out) times the light that reached the spot last frame (the
    // sources and bounces on screen, and the sky through the cascades' visibility). Emitters and bright, directly lit
    // surfaces keep the image: direct light is in the image but not in the GI. Relit Pickup Bias blends the two.
    if (RelitBias > 0.0 && zc < SkyDepthThreshold)
    {
        float4 flR = tex2Dlod(sFlow0, float4(uv, 0, 0));
        float2 uvR = uvc - flR.xy * px;
        if (all(uvR > 0.0) && all(uvR < 1.0))
        {
            float4 gpR = tex2Dlod(sDenIn, float4(uvR, 0, 0));
            float wR = saturate(1.0 - RelDepth(gpR.a, zc) / 0.1) * exp(-max(flR.z - 0.01, 0.0) * MotionRejection);
            if (wR > 1.0e-3 && !IsBad(gpR.rgb))
            {
                float3 tint = display / max(rawLum, 1.0e-4);            // hue of the surface at unit luminance
                float reflectance;
                float3 alb = Integration_Albedo ? max(tex2Dlod(sAlbedoTex, float4(uvc, 0, 0)).rgb, 0.0) : float3(0, 0, 0);
                float la = dot(alb, LUMA);
                if (la > 1.0e-3)
                {
                    // Game albedo: the hue and reflectance are known, so the plane-local detail estimate below (8 luminance +
                    // 8 depth taps) is skipped. 0.5 = mid reflectance.
                    tint = alb / la;
                    reflectance = clamp(RelitReflectance * la / 0.5, 0.02, 1.0) * MaterialDiffuse(uvc);
                }
                else
                {
                    // Plane-local average of the image luminance (depth-weighted ring), to take the baked lighting out.
                    static const float2 RING[8] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
                                                    float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071) };
                    float lumC = tex2Dlod(sLuma, float4(uvc, 0, 0)).r;
                    float sumL = lumC;
                    float sumW = 1.0;
                    [unroll]
                    for (int q = 0; q < 8; q++)
                    {
                        float2 uq = clamp(uvc + RING[q] * (RelitDetailRadius * RC_DIV) * px, px, 1.0 - px);
                        float wq = exp(-RelDepth(GetLinearDepth(uq), zc) * 12.0);
                        sumL += tex2Dlod(sLuma, float4(uq, 0, 0)).r * wq;
                        sumW += wq;
                    }
                    float detail = lerp(1.0, clamp(lumC / max(sumL / sumW, 1.0e-3), 0.25, 2.5), RelitDetail);
                    reflectance = clamp(RelitReflectance * detail, 0.02, 1.0);
                }

                float3 E = Clean(tex2Dlod(sDenLin, float4(uvR, 0, 0)).rgb);
                E = lerp(E, dot(E, LUMA).xxx, 0.5);                     // half-desaturated: the loop cannot run away
                E = ClampLuminance(E, MaxSampleLuminance * 2.0);
                float3 relit = ClampLuminance(tint * reflectance * E, MaxSampleLuminance);

                float keep = max(emitterFactor, smoothstep(0.25, 0.7, rawLum) * RelitKeepBright);
                light = lerp(light, relit, RelitBias * (1.0 - keep) * wR);
            }
        }
    }

    // (Multibounce is added where a ray of a longer cascade lands: see MultibounceLight.)
    if (zc >= SkyDepthThreshold)
        light = float3(0, 0, 0);   // sky is never a hit; keep it out of the filtered / mipped reads

    geo = float4(OctEncode(n), zc, thick);
    rad = float4(Clean(light), rawLum);
}

#if CRCGI_SECONDARY_DIVISOR > 0
// Pass: multibounce map at the secondary resolution (effect resolution / CRCGI_SECONDARY_DIVISOR).
float4 PS_MBMap(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    if (!Multibounce || MultibounceAmount <= 0.0)
        return float4(0, 0, 0, 0);
    float zc = tex2Dlod(sGeo, float4(uv, 0, 0)).z;
    if (zc >= SkyDepthThreshold)
        return float4(0, 0, 0, 0);
    float4 fl = tex2Dlod(sFlow0, float4(uv, 0, 0));
    float2 uvH = uv - fl.xy * ReShade::PixelSize;
    if (any(uvH <= 0.0) || any(uvH >= 1.0))
        return float4(0, 0, 0, 0);
    float zH = tex2Dlod(sDenIn, float4(uvH, 0, 0)).a;
    float w = saturate(1.0 - RelDepth(zH, zc) / 0.1);
    if (w <= 1.0e-4)
        return float4(0, 0, 0, 0);
    float conf = exp(-max(fl.z - 0.01, 0.0) * MotionRejection);
    float3 g = Clean(tex2Dlod(sDenLin, float4(uvH, 0, 0)).rgb);
    g = lerp(g, dot(g, LUMA).xxx, 0.5);
    g = ClampLuminance(g, MaxSampleLuminance * 2.0);
    return float4(g * (w * conf), w);
}
#endif

// ----------------------------------------------------------------------------
// Per-cascade temporal accumulation of the far cascades. A probe's value is the same quantity in every frame; the cell
// (direction) is kept and the four nearest probes at the position the picture moved from are blended bilinearly. History
// is trusted only where last frame's geometry at that position matches this probe's depth and normal. The merged result
// is what is stored, so the cascades below are merged against the accumulated one.
// ----------------------------------------------------------------------------

float4 FetchHistCell(int level, int2 texel)
{
    float4 uv4 = float4((float2(texel) + 0.5) / CASC_SIZE, 0, 0);
    if (level <= 1) return tex2Dlod(sHC1, uv4);
    if (level == 2) return tex2Dlod(sHC2, uv4);
    if (level == 3) return tex2Dlod(sHC3, uv4);
    if (level == 4) return tex2Dlod(sHC4, uv4);
    return tex2Dlod(sHC5, uv4);
}


// Reprojected history of a cascade texel and how far it can be trusted (0 = not at all).
float CascadeHistory(int level, ProbeInfo p, out float4 h)
{
    h = float4(0, 0, 0, 1);
    float4 fl = tex2Dlod(sFlow0, float4(p.uv, 0, 0));
    float2 uvPrev = p.uv - fl.xy * ReShade::PixelSize;
    if (any(uvPrev < 0.0) || any(uvPrev >= 1.0))
        return 0.0;

    float4 gPrev = tex2Dlod(sGeoPrev, float4(uvPrev, 0, 0));
    float depthRel = RelDepth(gPrev.z, p.d);
    float nd = dot(OctDecode(gPrev.xy), p.n);
    float valid = saturate(1.0 - depthRel / max(TemporalDepthGuard, 1.0e-3)) * saturate((nd - 0.7) / 0.3);
    if (valid <= 0.0)
        return 0.0;

    int N = CascadeTile(level);
    int2 maxProbe = int2(RC_TEX_W / N - 1, RC_TEX_H / N - 1);
    float2 gp = uvPrev * RC_SIZE / float(N) - 0.5;
    int2 i0 = int2(floor(gp));
    float2 f = gp - float2(i0);
    float4 acc = float4(0, 0, 0, 0);
    float ws = 0.0;
    [unroll]
    for (int j = 0; j < 4; j++)
    {
        int2 o = int2(j % 2, j / 2);
        int2 idx = clamp(i0 + o, int2(0, 0), maxProbe);
        float2 bw = lerp(1.0 - f, f, float2(o));
        float wj = bw.x * bw.y;
        acc += FetchHistCell(level, idx * N + p.cell) * wj;
        ws += wj;
    }
    h = acc / max(ws, 1.0e-6);
    if (IsBad(h.rgb))
        return 0.0;
    return valid * exp(-max(fl.z - 0.01, 0.0) * MotionRejection);
}

// Per-cascade temporal accumulation of the accumulated cascades (Far Cascades: Accumulate From). The farther the cascade, the
// more frames: Frames Accumulated x Growth ^ (steps past the start + 1), at most 64. New light (a rise much larger than the noise)
// is let through faster so a lamp switched on does not take a second to reach the far range.
float4 CascadeTemporal(int level, ProbeInfo p, float4 cur)
{
    if (!CascadeAccumulates(level))
        return cur;
    float4 h;
    float trust = CascadeHistory(level, p, h);
    if (trust <= 0.0)
        return cur;
    float frames = min(TemporalFrames * pow(max(FarAccumGrowth, 1.0), float(level - FarStartLevel() + 1)), 64.0);
    float lc = dot(cur.rgb, LUMA), lp = dot(h.rgb, LUMA);
    float rise = max(lc - lp, 0.0) / (abs(lc) + abs(lp) + 0.02);
    float guard = saturate(1.0 - rise / max(TemporalLightGuard * 4.0, 1.0e-3));
    float w = min(1.0 - 1.0 / max(frames, 1.0), 1.0 - 1.0 / 64.0) * trust * guard;
    return lerp(cur, h, w);
}

// Half-Rate Update: true when this probe skips tracing this frame and reuses its history (h). Checkerboard over the probes.
bool CascadeSkip(int level, ProbeInfo p, out float4 h)
{
    h = float4(0, 0, 0, 1);
    if (!FarHalfRate || !CascadeAccumulates(level) || ((p.probe.x + p.probe.y + framecount) % 2) == 0)
        return false;
    return CascadeHistory(level, p, h) > 0.5;
}

// ----------------------------------------------------------------------------
// Passes: cascades, far -> near
// ----------------------------------------------------------------------------

// Cascades 5 and 4 exist only with CRCGI_CASCADES 6 / 5; otherwise these passes return at once on a 1x1 target.
float4 PS_Cascade5(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
#if CRCGI_CASCADES >= 6
    ProbeInfo p = SetupProbe(5, uv);
    if (!p.ok)
        return float4(0, 0, 0, 1);
    float4 hSkip;
    if (CascadeSkip(5, p, hSkip))
        return hSkip;
    return CascadeTemporal(5, p, TraceCell(5, p));   // the top cascade: escaped rays have no parent, they add visibility only
#else
    return float4(0, 0, 0, 1);
#endif
}

float4 PS_Cascade4(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
#if CRCGI_CASCADES >= 5
    ProbeInfo p = SetupProbe(4, uv);
    if (!p.ok)
        return float4(0, 0, 0, 1);
    float4 hSkip;
    if (CascadeSkip(4, p, hSkip))
        return hSkip;
    float4 r = TraceCell(4, p);
  #if CRCGI_CASCADES >= 6
    float4 result = float4(r.rgb, 0.0);
    if (r.a > 0.0)
    {
        float4 parent = float4(0, 0, 0, 1);
        if (CascadeDebugLevel == 0)
            parent = MergeParent_4(4, p.probe, p.cell, p.n, p.d, p.T, p.B, p.uv);
        result += CarryParent(4, parent) * r.a;
    }
    return CascadeTemporal(4, p, result);
  #else
    return CascadeTemporal(4, p, r);         // the top cascade
  #endif
#else
    return float4(0, 0, 0, 1);
#endif
}

float4 PS_Cascade3(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    ProbeInfo p = SetupProbe(3, uv);
    if (!p.ok)
        return float4(0, 0, 0, 1);
    float4 hSkip;
    if (CascadeSkip(3, p, hSkip))
        return hSkip;
    float4 r = TraceCell(3, p);
#if CRCGI_CASCADES >= 5
    float4 result = float4(r.rgb, 0.0);
    if (r.a > 0.0)
    {
        float4 parent = float4(0, 0, 0, 1);
        if (CascadeDebugLevel == 0)
            parent = MergeParent_3(3, p.probe, p.cell, p.n, p.d, p.T, p.B, p.uv);
        result += CarryParent(3, parent) * r.a;
    }
    return CascadeTemporal(3, p, result);
#else
    // Escaped rays have no parent: they add visibility only.
    return CascadeTemporal(3, p, r);
#endif
}

float4 PS_Cascade2(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    ProbeInfo p = SetupProbe(2, uv);
    if (!p.ok)
        return float4(0, 0, 0, 1);
    float4 hSkip;
    if (CascadeSkip(2, p, hSkip))
        return hSkip;
    float4 r = TraceCell(2, p);
    float4 result = float4(r.rgb, 0.0);
    if (r.a > 0.0)
    {
        // Escaped rays take the parent cascade's answer. (Debug: interval only -> visibility only.)
        float4 parent = float4(0, 0, 0, 1);
        if (CascadeDebugLevel == 0)
            parent = MergeParent_2(2, p.probe, p.cell, p.n, p.d, p.T, p.B, p.uv);
        result += CarryParent(2, parent) * r.a;
    }
    return CascadeTemporal(2, p, result);
}

float4 PS_Cascade1(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    ProbeInfo p = SetupProbe(1, uv);
    if (!p.ok)
        return float4(0, 0, 0, 1);
    float4 hSkip;
    if (CascadeSkip(1, p, hSkip))
        return hSkip;
    float4 r = TraceCell(1, p);
    float4 result = float4(r.rgb, 0.0);
    if (r.a > 0.0)
    {
        float4 parent = float4(0, 0, 0, 1);
        if (CascadeDebugLevel == 0)
            parent = MergeParent_1(1, p.probe, p.cell, p.n, p.d, p.T, p.B, p.uv);
        result += CarryParent(1, parent) * r.a;
    }
    return CascadeTemporal(1, p, result);
}

float4 PS_Cascade0(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    ProbeInfo p = SetupProbe(0, uv);
    if (!p.ok)
        return float4(0, 0, 0, 1);
    float4 r = TraceCell(0, p);
    float4 result = float4(r.rgb, 0.0);
    if (r.a > 0.0)
    {
        float4 parent = float4(0, 0, 0, 1);
        if (CascadeDebugLevel == 0)
            parent = MergeParent_0(0, p.probe, p.cell, p.n, p.d, p.T, p.B, p.uv);
        result += CarryParent(0, parent) * r.a;
    }
    return result;
}

// ----------------------------------------------------------------------------
// Gather: integrate cascade 0 for the texel's neighbourhood -> GI
// ----------------------------------------------------------------------------

float4 FetchCAny(int level, int2 texel)
{
    float4 uv4 = float4((float2(texel) + 0.5) / CASC_SIZE, 0, 0);
    if (level <= 0) return tex2Dlod(sC0, uv4);
    if (level == 1) return tex2Dlod(sC1, uv4);
    if (level == 2) return tex2Dlod(sC2, uv4);
    if (level == 3) return tex2Dlod(sC3, uv4);
    if (level == 4) return tex2Dlod(sC4, uv4);
    return tex2Dlod(sC5, uv4);
}

// Debug: what ONE cascade sees on its own interval (the cascades skip merging while this is on).
float4 DebugCascade(int level, float2 tc)
{
    int N = CascadeTile(level);
    float2 gp = (tc - 0.5 * float(N)) / float(N);
    int2 i0 = int2(floor(gp));
    float2 f = gp - float2(i0);
    int2 maxProbe = int2(RC_TEX_W / N - 1, RC_TEX_H / N - 1);
    int stride = max(N / 8, 1);

    float3 rad = float3(0, 0, 0);
    float vis = 0.0;
    float cnt = 0.0;
    [loop]
    for (int j = 0; j < 4; j++)
    {
        int2 o = int2(j % 2, j / 2);
        float2 bw = lerp(1.0 - f, f, float2(o));
        float wb = bw.x * bw.y;
        int2 idx = clamp(i0 + o, int2(0, 0), maxProbe);
        [loop]
        for (int y = 0; y < N; y += stride)
        {
            [loop]
            for (int x = 0; x < N; x += stride)
            {
                float4 v = FetchCAny(level, idx * N + int2(x, y));
                rad += v.rgb * wb;
                vis += v.a * wb;
                cnt += wb;
            }
        }
    }
    cnt = max(cnt, 1e-6);
    rad /= cnt;
    vis /= cnt;
    float cov = 1.0 - vis;
    float3 shown = (CascadeDebugMode == 0) ? rad : float3(cov, cov, cov) / max(DebugGain, 0.001);
    return float4(shown, vis);
}

// Log-average brightness of the surfaces in view, from a 16 x 9 grid of the pick-up buffer (alpha = display luminance).
// It is the reference of the Dark Area Threshold: 'dark' means dark compared to the rest of the screen.
float4 PS_LumStats(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float sum = 0.0;
    float cnt = 0.0;
    [loop]
    for (int j = 0; j < 9; j++)
    {
        [loop]
        for (int i = 0; i < 16; i++)
        {
            float2 su = (float2(float(i), float(j)) + 0.5) / float2(16.0, 9.0);
            if (tex2Dlod(sGeo, float4(su, 0, 0)).z < SkyDepthThreshold)
            {
                sum += log(tex2Dlod(sRad, float4(su, 0, 0)).a + 0.002);
                cnt += 1.0;
            }
        }
    }
    float l = (cnt > 0.5) ? exp(sum / cnt) - 0.002 : 0.1;
    return float4(max(l, 0.002), 0.0, 0.0, 1.0);
}

// Per cascade-0 probe: average of its cells, computed once per probe so the gather does not repeat it for every
// pixel and neighbouring probe.
void PS_ProbeAvg0(float4 pos : SV_Position, float2 uv : TexCoord, out float4 avgA : SV_Target0, out float4 avgB : SV_Target1)
{
    int2 idx = clamp(int2(floor(uv * P0_SIZE)), int2(0, 0), int2(RC_P0_W - 1, RC_P0_H - 1));
    int2 pc = min(idx * CRCGI_BASE_TILE + CRCGI_BASE_TILE / 2, RC_MAX_TEXEL);
    bool skyOn = ProcessSkyColor && DebugView != 2 && GBufferDebugView != 8;

    float3 nQ = float3(0, 0, -1);
    float3 T = float3(1, 0, 0);
    float3 B = float3(0, 1, 0);
    if (skyOn)
    {
        float4 gP = FetchGeoTexel(pc);
        nQ = OctDecode(gP.xy);
        BuildFrame(nQ, T, B);
    }

    float3 pr = float3(0, 0, 0);
    float pv = 0.0;
    float ps = 0.0;
    float3 psc = float3(0, 0, 0);
    bool mapOn = skyOn && SkyMapOn;
    float3 tint = tex2Dlod(sSkyA, float4(0.5, 0.5, 0, 0)).rgb;
    [unroll]
    for (int k = 0; k < CRCGI_BASE_TILE * CRCGI_BASE_TILE; k++)
    {
        int2 cell = int2(k % CRCGI_BASE_TILE, k / CRCGI_BASE_TILE);
        float4 v = FetchC0(idx * CRCGI_BASE_TILE + cell);
        pr += v.rgb;
        pv += v.a;
        if (skyOn)
        {
            float3 Dk = DirFromXi((float2(cell) + 0.5) / float(CRCGI_BASE_TILE), nQ, T, B);
            float sw = v.a * SkyDirWeight(Dk);
            ps += sw;
            if (mapOn && sw > 0.0)
                psc += sw * SkyDirectionalTint(Dk, tint);   // the sky where this direction points
        }
    }
    float inv = 1.0 / float(CRCGI_BASE_TILE * CRCGI_BASE_TILE);

    // Dark Area Boost: scales everything the cascades delivered to this probe (all cascades, all distances) when the
    // surface is dark relative to the screen average.
    float darkF = 1.0;
    if (DarkAreaBoost > 0.0)
    {
        float lumC = tex2Dlod(sRad, float4((float2(pc) + 0.5) / RC_SIZE, 0, 0)).a;
        float rel = lumC / max(tex2Dlod(sLum, float4(0.5, 0.5, 0, 0)).x, 1.0e-3);
        darkF = 1.0 + DarkAreaBoost * (1.0 - smoothstep(DarkAreaThreshold, DarkAreaThreshold * 2.0, rel));
    }
    avgA = float4(pr * inv * darkF, pv * inv);
    avgB = float4(ps * inv, psc * inv);
}

float4 PS_Gather(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float4 g = tex2Dlod(sGeo, float4(uv, 0, 0));
    float3 n = OctDecode(g.xy);
    float depth = g.z;

    float fade = GetDistanceFade(depth);
    if (depth >= SkyDepthThreshold || fade <= 0.001)
        return float4(0, 0, 0, 1);

    float2 tc = uv * RC_SIZE;   // continuous texel coords
    if (CascadeDebugLevel > 0)
        return DebugCascade(CascadeDebugLevel - 1, tc);

    // Cascade-0 probes sit at tile*i + tile/2. Tent weights over a 2x2 (bilinear) or 4x4 probe
    // neighbourhood, always edge-aware.
    float2 gp = (tc - 0.5 * float(CRCGI_BASE_TILE)) / float(CRCGI_BASE_TILE);
    int2 i0 = int2(floor(gp));
    float2 f = gp - float2(i0);
    int2 maxProbe = int2(RC_TEX_W / CRCGI_BASE_TILE - 1, RC_TEX_H / CRCGI_BASE_TILE - 1);
    int span = (GatherSmoothing > 0.001) ? 4 : 2;
    int start = (span == 4) ? -1 : 0;
    float tentR = 1.0 + GatherSmoothing;
    float foot = TexelSize(depth) * float(CRCGI_BASE_TILE);
    bool skyOn = ProcessSkyColor && DebugView != 2 && GBufferDebugView != 8;

    float3 rad = float3(0, 0, 0);
    float visBin = 0.0;
    float visSky = 0.0;
    float3 visSkyCol = float3(0, 0, 0);
    float ws = 0.0;

    [loop]
    for (int jy = 0; jy < 4; jy++)
    {
        if (jy >= span)
            break;
        [loop]
        for (int jx = 0; jx < 4; jx++)
        {
            if (jx >= span)
                break;

            int2 o = int2(start + jx, start + jy);
            float2 tent = saturate(1.0 - abs(float2(o) - f) / tentR);
            float wb = tent.x * tent.y;
            if (wb <= 0.0)
                continue;

            int2 idx = clamp(i0 + o, int2(0, 0), maxProbe);
            int2 pc = min(idx * CRCGI_BASE_TILE + CRCGI_BASE_TILE / 2, RC_MAX_TEXEL);
            float4 gP = FetchGeoTexel(pc);
            float w = wb * (ProbeWeight(n, depth, uv, gP, (float2(pc) + 0.5) / RC_SIZE, ProbeDepthSharpness, foot) + 1e-5);

            float2 puv = (float2(idx) + 0.5) / P0_SIZE;
            float4 pa = tex2Dlod(sP0A, float4(puv, 0, 0));
            rad += pa.rgb * w;
            visBin += pa.a * w;
            if (skyOn)
            {
                float4 pb = tex2Dlod(sP0B, float4(puv, 0, 0));
                visSky += pb.r * w;
                visSkyCol += pb.gba * w;
            }
            ws += w;
        }
    }
    ws = max(ws, 1e-12);
    rad /= ws;
    visBin /= ws;
    visSky /= ws;
    visSkyCol /= ws;

    // ---- bounce shaping ----
    float3 bounce = BoostSaturation(rad, SaturationBoost);
    bounce = ApplyColorFilters(bounce);
    bounce = ApplyBounceCurve(bounce);

    float3 gi = bounce;

    // ---- sky ambient, gated by cascade visibility ----
    if (skyOn && SkyColorInfluence > 0.0)
    {
        float3 skyTint = tex2Dlod(sSkyA, float4(0.5, 0.5, 0, 0)).rgb;
        if (SkyMapOn && visSky > 1.0e-5)
            skyTint = Clean(visSkyCol / visSky);   // average sky colour over this pixel's open directions
        float sv = pow(max(saturate(visSky / max(SkyVisibilityNorm, 1e-4)), 1e-6), SkyOcclusionContrast);
        float edge = saturate(min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)) / max(SkyEdgeFade, 1e-4));
        gi += skyTint * SkyColorInfluence * sv * edge;
    }

    return float4(Finite(gi * fade), visBin);
}

// ----------------------------------------------------------------------------
// Denoise: separable and edge-aware, at the GI's own resolution. It reads the gathered GI directly
// (unless Temporal Accumulation is on), so this is the main smoothing of the raw GI.
// ----------------------------------------------------------------------------

// Weight of a denoise tap at 'su' for the centre texel (P, N, z). 'foot' = max(TexelSize(z) * 1.5, 1e-6)
// and 'th' = TanHalf() are per-pixel constants passed in by the caller.
float DenoiseTapWeight(float3 P, float3 N, float z, float foot, float2 th, float2 su)
{
    float4 gS = tex2Dlod(sGeo, float4(su, 0, 0));
    if (gS.z >= SkyDepthThreshold)   // sky tap: depth term is 0
        return 0.0;

    float depthTerm;
    if (DenoiseDepthMode == 0)
    {
        float d = dot(ViewPosT(su, gS.z, th) - P, N) / foot;
        depthTerm = exp(-d * d * DenoiseDepthSharpness);
    }
    else if (DenoiseDepthMode == 1)
        depthTerm = exp(-RelDepth(gS.z, z) * 24.0 * DenoiseDepthSharpness);
    else
        depthTerm = exp(-abs(gS.z - z) * 64.0 * DenoiseDepthSharpness);

    return depthTerm * PowN(saturate(dot(N, OctDecode(gS.xy)) * 0.75 + 0.25), DenoiseNormalPower);
}

// Luminance edge stop of a tap: steep where the neighbourhood is clean, relaxed where it is noisy (SVGF).
float DenoiseLumWeight(float lTap, float lCenter, float sigma)
{
    float denom = 4.0 * sigma + 0.05 * lCenter + 1.0e-3;
    return lerp(1.0, exp(-abs(lTap - lCenter) / denom), saturate(DenoiseVarianceGuide));
}

float4 PS_DenoiseH(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float3 c = tex2Dlod(sRaw, float4(uv, 0, 0)).rgb;
    float4 g = tex2Dlod(sGeo, float4(uv, 0, 0));
    if (DenoiseMode == 1 || g.z >= SkyDepthThreshold)
        return float4(c, 0.0);

    // Noise of this texel from its 3 x 3 neighbourhood of the raw gather: sigma (std. deviation of luminance), kept
    // relative to the mean in the alpha channel for the vertical pass. A texel far above its neighbourhood is a firefly.
    float lc = dot(c, LUMA);
    float sigma = 0.0;
    float lMean = lc;
    if (DenoiseVarianceGuide > 0.0)
    {
        float2 tpx = 1.0 / RC_SIZE;
        float m1 = lc;
        float m2 = lc * lc;
        [unroll]
        for (int k = 0; k < 8; k++)
        {
            float2 o = float2((k < 3) ? float(k - 1) : ((k < 5) ? ((k == 3) ? -1.0 : 1.0) : float(k - 6)),
                              (k < 3) ? -1.0 : ((k < 5) ? 0.0 : 1.0));
            float l = dot(tex2Dlod(sRaw, float4(uv + o * tpx, 0, 0)).rgb, LUMA);
            m1 += l;
            m2 += l * l;
        }
        lMean = m1 / 9.0;
        sigma = sqrt(max(m2 / 9.0 - lMean * lMean, 0.0));
        float lMax = lMean + 3.0 * sigma + 0.01;
        c *= min(1.0, lMax / max(lc, 1.0e-4));
        lc = dot(c, LUMA);
    }
    float relNoise = sigma / (lMean + 0.01);

    float3 P = ViewPos(uv, g.z);
    float3 N = OctDecode(g.xy);
    float kernelShrink = 1.0 / (1.0 + g.z * DenoiseDepthScale);
    float stepScale = lerp(1.0, lerp(0.6, 1.0, saturate(relNoise * 3.0)), saturate(DenoiseVarianceGuide));
    float stepUV = DenoiseStep * kernelShrink * stepScale / RC_SIZE.x;
    float2 th = TanHalf();
    float tapFoot = max(TexelSize(g.z) * 1.5, 1e-6);
    int R = DenoiseRadius;

    float3 sum = c;
    float weightSum = 1.0;
    [loop]
    for (int x = 1; x <= R; x++)
    {
        [unroll]
        for (int sgn = 0; sgn < 2; sgn++)
        {
            float2 su = uv + float2(((sgn == 0) ? 1.0 : -1.0) * stepUV * float(x), 0.0);
            float3 v = tex2Dlod(sRawLin, float4(su, 0, 0)).rgb;
            float w = DenoiseTapWeight(P, N, g.z, tapFoot, th, su) * DenoiseLumWeight(dot(v, LUMA), lc, sigma);
            sum += v * w;
            weightSum += w;
        }
    }
    return float4(sum / weightSum, relNoise);
}

float4 DenoiseVCore(float4 pos, float2 uv)
{
    float4 c4 = tex2Dlod(sDenTmp, float4(uv, 0, 0));
    float3 c = c4.rgb;
    float4 g = tex2Dlod(sGeo, float4(uv, 0, 0));
    if (DenoiseMode == 1 || g.z >= SkyDepthThreshold)
        return float4(c, 1.0);

    float lc = dot(c, LUMA);
    float sigma = c4.a * (lc + 0.01) * 0.5;   // the horizontal pass already removed part of the noise
    float3 P = ViewPos(uv, g.z);
    float3 N = OctDecode(g.xy);
    float kernelShrink = 1.0 / (1.0 + g.z * DenoiseDepthScale);
    float stepScale = lerp(1.0, lerp(0.6, 1.0, saturate(c4.a * 3.0)), saturate(DenoiseVarianceGuide));
    float stepUV = DenoiseStep * kernelShrink * stepScale / RC_SIZE.y;
    float2 th = TanHalf();
    float tapFoot = max(TexelSize(g.z) * 1.5, 1e-6);
    int R = DenoiseRadius;

    float3 sum = c;
    float weightSum = 1.0;
    [loop]
    for (int y = 1; y <= R; y++)
    {
        [unroll]
        for (int sgn = 0; sgn < 2; sgn++)
        {
            float2 su = uv + float2(0.0, ((sgn == 0) ? 1.0 : -1.0) * stepUV * float(y));
            float3 v = tex2Dlod(sDenTmp, float4(su, 0, 0)).rgb;
            float w = DenoiseTapWeight(P, N, g.z, tapFoot, th, su) * DenoiseLumWeight(dot(v, LUMA), lc, sigma);
            sum += v * w;
            weightSum += w;
        }
    }
    return float4(sum / weightSum, 1.0);
}

// Temporal accumulation of the lighting only: lerp(this frame, history, w), the history reprojected with the motion flow
// (bilinear colour, point depth), w from the frame count and scaled to 0 where the flow found no match, the depth
// disagrees or the light disagrees.
float4 TemporalBlend(float3 cur, float z, float2 uv)
{
    if (!TemporalOn || z >= SkyDepthThreshold)
        return float4(cur, z);
    float4 fl = tex2Dlod(sFlow0, float4(uv, 0, 0));
    float2 uvH = uv - fl.xy * ReShade::PixelSize;
    if (any(uvH < 0.0) || any(uvH >= 1.0))
        return float4(cur, z);
    float4 h4 = tex2Dlod(sHist, float4(uvH, 0, 0));
    float3 h = tex2Dlod(sHistLin, float4(uvH, 0, 0)).rgb;
    if (IsBad(h))
        return float4(cur, z);
    float w = (1.0 - 1.0 / max(TemporalFrames, 1.0)) * exp(-max(fl.z - 0.01, 0.0) * MotionRejection);
    float depthRel = RelDepth(h4.a, z);
    float lc = dot(cur, LUMA);
    float lp = dot(h, LUMA);
    float lsum = abs(lc) + abs(lp) + 0.02;
    float rise = max(lc - lp, 0.0) / lsum;    // new light: the history is stale
    float drop = max(lp - lc, 0.0) / lsum;    // light gone: missing data, or a real occluder?
    // The cascades tell which: when this frame's rays mostly ESCAPED (open sky, or off the screen edge) the light went
    // missing from the screen, it was not blocked, so it stays in the buffer (a drop is tolerated up to 4 x as much).
    // When the rays were blocked or hit something, it is real occlusion and the new frame wins at once. Geometry
    // (depth) decides disocclusion, as in SVGF.
    float escaped = tex2Dlod(sRaw, float4(uv, 0, 0)).a;
    float tol = max(TemporalLightGuard, 1e-3);
    w *= saturate(1.0 - depthRel / max(TemporalDepthGuard, 1e-3)) * saturate(1.0 - rise / tol)
       * saturate(1.0 - drop / (tol * (1.0 + 3.0 * escaped)));
    return float4(lerp(cur, h, w), z);
}

float4 PS_DenoiseV(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float3 gi = DenoiseVCore(pos, uv).rgb;
    float z = tex2Dlod(sGeo, float4(uv, 0, 0)).z;
    return TemporalBlend(gi, z, uv);
}

// Copies the blended GI to the history for the next frame (skipped when off).
// One pass for the three cascade-resolution copies (GI history, luminance, geometry). Always copies: the GI history is simply unused
// while Temporal Accumulation is off.
void PS_CopyPrev(float4 pos : SV_Position, float2 uv : TexCoord, out float4 hist : SV_Target0, out float4 luma : SV_Target1, out float4 geo : SV_Target2)
{
    hist = tex2Dlod(sDenIn, float4(uv, 0, 0));
    luma = tex2Dlod(sLuma, float4(uv, 0, 0));
    geo  = tex2Dlod(sGeo, float4(uv, 0, 0));
}

// History copies of the accumulated cascades, one pass each; a cascade that does not accumulate skips its copy (discard).
float4 CopyCascade(sampler2D src, int level, float2 uv)
{
    if (!CascadeAccumulates(level))
        discard;
    return tex2Dlod(src, float4(uv, 0, 0));
}
float4 PS_CopyHC1(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target { return CopyCascade(sC1, 1, uv); }
float4 PS_CopyHC2(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target { return CopyCascade(sC2, 2, uv); }
float4 PS_CopyHC3(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target { return CopyCascade(sC3, 3, uv); }
#if CRCGI_CASCADES >= 5
float4 PS_CopyHC4(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target { return CopyCascade(sC4, 4, uv); }
#endif
#if CRCGI_CASCADES >= 6
float4 PS_CopyHC5(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target { return CopyCascade(sC5, 5, uv); }
#endif



// ----------------------------------------------------------------------------
// Upsample to full resolution: each pixel takes light from the low-res texels on its own surface.
// The receiver normal comes from the tap closest in depth; a thin feature matching none of the
// taps takes that tap's value instead of a blend of other surfaces.
// ----------------------------------------------------------------------------

float3 UpsampleGI(float2 uv, float z)
{
    if (UpsampleMode == 2)
        return tex2Dlod(sDenLin, float4(uv, 0, 0)).rgb;

    float2 h = uv * RC_SIZE - 0.5;
    int2 i0 = int2(floor(h));
    float2 f = h - float2(i0);

    float4 gt[4];
    float2 tuv[4];
    float bestDz = 1e9;
    int best = 0;
    float2 bestN = float2(0, 0);
    [unroll]
    for (int j = 0; j < 4; j++)
    {
        int2 idx = clamp(i0 + int2(j % 2, j / 2), int2(0, 0), RC_MAX_TEXEL);
        tuv[j] = (float2(idx) + 0.5) / RC_SIZE;
        gt[j] = tex2Dlod(sGeo, float4(tuv[j], 0, 0));
        float dz = abs(gt[j].z - z);
        if (dz < bestDz)
        {
            bestDz = dz;
            best = j;
            bestN = gt[j].xy;
        }
    }

    float3 N = OctDecode(bestN);
    float2 th = TanHalf();
    float3 P = ViewPosT(uv, z, th);
    float foot = max(TexelSize(z), 1e-6);
    float3 acc = float3(0, 0, 0);
    float3 nearest = float3(0, 0, 0);
    float ws = 0.0;
    [unroll]
    for (int k = 0; k < 4; k++)
    {
        float2 bw = lerp(1.0 - f, f, float2(k % 2, k / 2));
        float depthTerm;
        if (UpsampleMode == 0)
        {
            float d = dot(ViewPosT(tuv[k], gt[k].z, th) - P, N) / foot;
            depthTerm = exp(-d * d * UpsampleDepthSharpness);
        }
        else
            depthTerm = exp(-RelDepth(gt[k].z, z) * 24.0 * UpsampleDepthSharpness);
        if (gt[k].z >= SkyDepthThreshold)
            depthTerm = 0.0;
        float w = bw.x * bw.y * depthTerm * PowN(saturate(dot(N, OctDecode(gt[k].xy)) * 0.75 + 0.25), UpsampleNormalPower);
        float3 v = tex2Dlod(sDen, float4(tuv[k], 0, 0)).rgb;
        acc += v * w;
        ws += w;
        if (k == best)
            nearest = v;
    }
    return (ws > 1e-4) ? acc / ws : nearest;
}

// ----------------------------------------------------------------------------
// Contact cascade: the shortest cascade, at full resolution, one probe per pixel, oriented by that pixel's own normal
// (the game's normal texture when bound, so a normal map shapes the hemisphere). Four cosine-distributed directions
// (the cascade-0 layout), rotated a little per pixel in a 2 x 2 pattern that the resolve pass averages out, march a short
// interval against the full-resolution depth. The interval's transmittance T = prod(1 - occupancy) is what every
// cascade carries as visibility; here it is kept per pixel. Occupancy is soft in the ray's depth behind the surface.
// ----------------------------------------------------------------------------

float4 PS_Contact(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    if (CreviceBounceBias == 0.0)
        return float4(1, 0, 0, 1);
    float z = GetLinearDepth(uv);
    if (z >= SkyDepthThreshold)
        return float4(1, 0, 0, 1);
    z = max(z, 1e-4);

    float tanSil = tan(SilhouetteAngle * (RC_PI / 180.0));
    float3 n = ViewNormalAt(uv, z, tanSil);
    float pixA = PixelAngle();
    float pxView = z * pixA;                           // one full-res pixel in view space at this depth
    float3 O = ViewPos(uv, z) + n * (0.75 * pxView);   // lifted off the surface
    float2 uvO = ViewToUV(O);
    float zO = O.z;

    float3 T, B;
    BuildFrame(n, T, B);
    int2 ip = int2(pos.xy);
    int state = (ip.x % 2) + 2 * (ip.y % 2);
    float rot = float(state) * (RC_PI * 0.125);
    float3 T2 = T * cos(rot) + B * sin(rot);
    float3 B2 = B * cos(rot) - T * sin(rot);
    float jit = 0.25 + 0.5 * (float(state) / 3.0);

    float stepPx = ContactRangeTexels * RC_DIV / 4.0;
    float biasV = ContactBiasTexels * RC_DIV * pxView;
    float band = pxView * (stepPx * 2.0 + 1.0) + biasV;

    float Tsum = 0.0;
    [unroll]
    for (int k = 0; k < 4; k++)
    {
        float3 D = DirFromXi((float2(float(k % 2), float(k / 2)) + 0.5) * 0.5, n, T2, B2);
        float2 v = zO * D.xy - D.z * O.xy;
        float a = length(v);
        float Tray = 1.0;
        if (a > 1.0e-6)
        {
            float2 dirS = v / a;
            [loop]
            for (int s = 0; s < 4; s++)
            {
                float travelPx = (float(s) + jit) * stepPx;
                float2 uvS = uvO + dirS * travelPx * ReShade::PixelSize;
                if (any(uvS < 0.0) || any(uvS >= 1.0))
                    break;
                float den = a - travelPx * pixA * zO * D.z;
                if (den <= 1.0e-6)
                    break;
                float zRay = zO * a / den;
                float zS = GetLinearDepth(uvS);
                if (zS < SkyDepthThreshold)
                {
                    float delta = zRay - zS;   // > 0: the ray is behind the surface
                    float occ = smoothstep(biasV, biasV + pxView * 0.75, delta) * (1.0 - smoothstep(band * 0.6, band, delta));
                    Tray *= 1.0 - occ;
                }
            }
        }
        Tsum += Tray;
    }
    return float4(Tsum * 0.25, 0, 0, 1);
}

// Plane-aware weight of a neighbour: 1 on the receiver's own tangent plane, 0 on another surface.
float PlaneWeight(float3 P, float3 n, float foot, float2 th, float2 u)
{
    float zT = GetLinearDepth(u);
    if (zT >= SkyDepthThreshold)
        return 0.0;
    float d = dot(ViewPosT(u, max(zT, 1e-4), th) - P, n) / foot;
    return exp(-0.5 * d * d);
}

// Merge the contact cascade into the GI. T_in (3 x 3 plane-aware average of the contact transmittance, which also
// averages out the per-pixel rotation pattern) is compared with the surroundings T_w (a ring of radius Contact Range,
// same surface). Half the reference is the surroundings, half is open air, so a crevice that is wide compared with the
// range still darkens. The albedo response uses the same neighbourhood for the surface's mean brightness.
float3 ContactResolve(float2 uv, float z, float3 raw, float3 gi, out float contactF)
{
    contactF = 1.0;
    if (CreviceBounceBias == 0.0 && AlbedoResponse <= 0.0)
        return gi;
    // Game albedo (Game Integration): the surface's real albedo replaces the image-based estimate (0.5 = mid reflectance = 1x).
    float3 gameAlb = Integration_Albedo ? saturate(tex2Dlod(sAlbedoTex, float4(uv, 0, 0)).rgb) : float3(0, 0, 0);
    bool haveAlb = dot(gameAlb, LUMA) > 1.0e-3;
    if (haveAlb && CreviceBounceBias == 0.0)
        return gi * lerp(float3(1, 1, 1), clamp(gameAlb * 2.0, 0.1, 2.0), AlbedoResponse);   // no neighbourhood taps needed

    static const float2 INNER[8] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1), float2(1, 1), float2(-1, 1), float2(1, -1), float2(-1, -1) };
    static const float2 RING[8]  = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1), float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071) };

    float2 th = TanHalf();
    float2 px = ReShade::PixelSize;
    float3 n = OctDecode(tex2Dlod(sGeo, float4(uv, 0, 0)).xy);
    float3 P = ViewPosT(uv, z, th);
    float footIn = max(TexelSizeFull(z) * 1.5, 1e-6);
    float rOut = max(ContactRangeTexels * RC_DIV * 0.8, 2.0);
    float footOut = max(TexelSizeFull(z) * rOut * 0.6, 1e-6);

    float Tc = tex2Dlod(sContact, float4(uv, 0, 0)).r;
    float lumC = dot(raw, LUMA);
    float wIn = 1.0, TIn = Tc;
    float wAll = 1.0, TAll = Tc, LAll = lumC;

    [unroll]
    for (int i = 0; i < 8; i++)
    {
        float2 u = uv + INNER[i] * px;
        float w = PlaneWeight(P, n, footIn, th, u);
        float tT = tex2Dlod(sContact, float4(u, 0, 0)).r;
        float lT = dot(BackBufferWork(u), LUMA);
        wIn += w;  TIn += tT * w;
        wAll += w; TAll += tT * w; LAll += lT * w;
    }
    [unroll]
    for (int j = 0; j < 8; j++)
    {
        float2 u = uv + RING[j] * rOut * px;
        float w = PlaneWeight(P, n, footOut, th, u);
        float tT = tex2Dlod(sContact, float4(u, 0, 0)).r;
        float lT = dot(BackBufferWork(u), LUMA);
        wAll += w; TAll += tT * w; LAll += lT * w;
    }
    float Tin = TIn / wIn;
    float Tw = TAll / wAll;
    float Lw = LAll / wAll;

    if (CreviceBounceBias != 0.0)
    {
        float Tref = max(lerp(Tw, 1.0, 0.5), 0.25);
        float ratio = clamp(Tin / Tref, 0.0, 1.15);
        contactF = clamp(pow(max(ratio, 0.05), -CreviceBounceBias * 2.0), 0.0, 3.0);
    }

    float3 albedoF = float3(1, 1, 1);
    if (AlbedoResponse > 0.0)
    {
        float lr = clamp(lumC / max(Lw, 0.02), 0.2, 2.5);
        float3 chroma = clamp(raw / max(lumC, 1.0e-3), 0.0, 3.0);
        float3 resp = haveAlb ? clamp(gameAlb * 2.0, 0.1, 2.0) : lr * lerp(float3(1, 1, 1), chroma, 0.5);
        albedoF = lerp(float3(1, 1, 1), resp, AlbedoResponse);
    }
    return gi * contactF * albedoF;
}

// ----------------------------------------------------------------------------
// Game normal auto-align. Pass 1: each of 32 x 18 tiles sums (depth normal) x (game normal)^T over 4 x 4 samples at
// surfaces where the depth rebuild is reliable (no silhouette). Pass 2: the sum M is turned into the nearest orthogonal
// matrix R (polar decomposition, Newton iteration X = (X + X^-T) / 2), so that depth normal ~ R * game normal. R may
// include a reflection, which absorbs flipped axes. A view with too few surface directions keeps the previous fit.
// ----------------------------------------------------------------------------

void PS_NormalFit(float4 pos : SV_Position, float2 uv : TexCoord, out float4 r0 : SV_Target0, out float4 r1 : SV_Target1, out float4 r2 : SV_Target2)
{
    r0 = float4(0, 0, 0, 0);
    r1 = float4(0, 0, 0, 0);
    r2 = float4(0, 0, 0, 0);
    if (!UseNormalTexture || Integration_NormalSpace != 1)
        return;
    float tanSil = tan(SilhouetteAngle * (RC_PI / 180.0));
    float2 cell = 1.0 / float2(NFIT_W, NFIT_H);
    float2 base = floor(pos.xy) * cell;
    float2 jit = frac(float(framecount % 64) * float2(0.7548777, 0.5698403));   // R2 sequence: new sample spots each frame
    float2 scr = float2(BUFFER_WIDTH, BUFFER_HEIGHT);
    float3 m0 = float3(0, 0, 0), m1 = float3(0, 0, 0), m2 = float3(0, 0, 0);
    float count = 0.0;
    [loop]
    for (int j = 0; j < NFIT_SUB; j++)
    {
        [loop]
        for (int i = 0; i < NFIT_SUB; i++)
        {
            float2 u = base + (float2(i, j) + jit) * cell / float(NFIT_SUB);
            u = (floor(u * scr) + 0.5) / scr;               // pixel centre: no filtering across edges
            float z = GetLinearDepth(u);
            if (z <= 1.0e-4 || z >= SkyDepthThreshold)
                continue;
            float3 g = DecodeGameNormal(tex2Dlod(sNormalTex, float4(u, 0, 0)));
            float gl = dot(g, g);
            if (gl < 0.25 || gl > 2.25 || IsBad(g))
                continue;
            g *= rsqrt(gl);
            bool edge;
            float3 v = ReconNormalAt(u, z, tanSil, edge);
            if (edge)
                continue;
            m0 += v.x * g;
            m1 += v.y * g;
            m2 += v.z * g;
            count += 1.0;
        }
    }
    r0 = float4(m0, count);
    r1 = float4(m1, 0.0);
    r2 = float4(m2, 0.0);
}

// One Newton step of the polar decomposition: X <- (X + X^-T) / 2, with X^-T = cofactor(X) / det(X).
void PolarStep(inout float3 a, inout float3 b, inout float3 c)
{
    float3 ca = cross(b, c), cb = cross(c, a), cc = cross(a, b);
    float det = dot(a, ca);
    if (abs(det) < 1.0e-12)
        return;
    float inv = 1.0 / det;
    a = 0.5 * (a + ca * inv);
    b = 0.5 * (b + cb * inv);
    c = 0.5 * (c + cc * inv);
}

float4 PS_NormalRot(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    int k = int(pos.x);
    float4 p0 = tex2Dlod(sNRotPrev, float4(0.125, 0.5, 0, 0));
    float4 p1 = tex2Dlod(sNRotPrev, float4(0.375, 0.5, 0, 0));
    float4 p2 = tex2Dlod(sNRotPrev, float4(0.625, 0.5, 0, 0));
    float4 p3 = tex2Dlod(sNRotPrev, float4(0.875, 0.5, 0, 0));
    bool prevOk = p3.w > 0.5 && !IsBad(p0.xyz + p1.xyz + p2.xyz);
    if (!UseNormalTexture || Integration_NormalSpace != 1)
        return float4(0, 0, 0, 0);

    float3 a = float3(0, 0, 0), b = float3(0, 0, 0), c = float3(0, 0, 0);
    float n = 0.0;
    [loop]
    for (int y = 0; y < NFIT_H; y++)
    {
        [loop]
        for (int x = 0; x < NFIT_W; x++)
        {
            float2 t = (float2(x, y) + 0.5) / float2(NFIT_W, NFIT_H);
            float4 s0 = tex2Dlod(sNFit0, float4(t, 0, 0));
            a += s0.xyz;
            n += s0.w;
            b += tex2Dlod(sNFit1, float4(t, 0, 0)).xyz;
            c += tex2Dlod(sNFit2, float4(t, 0, 0)).xyz;
        }
    }
    // Confidence: how well-conditioned M is (all three singular values present), 0..1.
    float frob = sqrt(dot(a, a) + dot(b, b) + dot(c, c));
    float conf = 0.0;
    if (n >= 64.0 && frob > 1.0e-6)
    {
        float3 an = a / frob, bn = b / frob, cn = c / frob;
        conf = saturate(abs(dot(an, cross(bn, cn))) * 5.196);   // 1 when M = scaled rotation (det of normalised = 3^-1.5)
    }
    bool fitOk = conf > 0.02;   // tested: 0.02 keeps ~70% of synthetic views with a worst-case error of 0.5 deg
    float3 r0 = p0.xyz, r1 = p1.xyz, r2 = p2.xyz;
    if (fitOk)
    {
        a /= frob; b /= frob; c /= frob;
        [unroll]
        for (int it = 0; it < 12; it++)
            PolarStep(a, b, c);
        if (prevOk)
        {
            float keep = Integration_NormalFitSmoothing;
            a = lerp(a, p0.xyz, keep); b = lerp(b, p1.xyz, keep); c = lerp(c, p2.xyz, keep);
            [unroll]
            for (int it2 = 0; it2 < 3; it2++)
                PolarStep(a, b, c);
        }
        r0 = a; r1 = b; r2 = c;
    }
    bool valid = fitOk || prevOk;
    if (!valid)
        return (k == 0) ? float4(1, 0, 0, 0) : ((k == 1) ? float4(0, 1, 0, 0) : ((k == 2) ? float4(0, 0, 1, 0) : float4(0, n, 0, 0)));
    if (k == 0) return float4(r0, 0);
    if (k == 1) return float4(r1, 0);
    if (k == 2) return float4(r2, 0);
    return float4(conf, n, fitOk ? 1.0 : 0.0, 1.0);
}

float4 PS_CopyNormalRot(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    return tex2Dlod(sNRot, float4(uv, 0, 0));
}

// Debug: G-Buffer & Canvas views (everything except Off-Screen Light Only, which runs the whole pipeline).
float3 DebugNormalColor(float3 n) { return n * 0.5 + 0.5; }

float3 GBufferDebugTile(int view, float2 uv, float depth)
{
    float tanSil = tan(SilhouetteAngle * (RC_PI / 180.0));
    float z = max(depth, 1.0e-4);
    float3 pc = ViewPos(uv, z);
    bool sky = depth >= SkyDepthThreshold;
    if (view == 2)
        return pow(saturate(tex2Dlod(sAlbedoTex, float4(uv, 0, 0)).rgb), 1.0 / 2.2);
    if (view == 3)
    {
        if (sky) return float3(0, 0, 0);
        return DebugNormalColor(ViewNormalAt(uv, z, tanSil));
    }
    if (view == 4)
    {
        if (sky) return float3(0, 0, 0);
        bool edge;
        return DebugNormalColor(ReconNormalAt(uv, z, tanSil, edge));
    }
    if (view == 5 || view == 9)
    {
        if (sky) return float3(0, 0, 0);
        bool ok, edge;
        float3 gn = GameNormalView(uv, pc, ok);
        if (!ok) return float3(0.6, 0, 0);
        float3 rn = ReconNormalAt(uv, z, tanSil, edge);
        float ang = acos(clamp(dot(gn, rn), -1.0, 1.0)) / (RC_PI * 0.5);
        return edge ? float3(0.15, 0.15, 0.3) : saturate(ang).xxx;
    }
    if (view == 6)
    {
        float2 fl = (Integration_Motion ? GameMotionFlow(uv, 0).xy : tex2Dlod(sMotionTex, float4(uv, 0, 0)).xy * float2(BUFFER_WIDTH, BUFFER_HEIGHT));
        return float3(fl / 16.0 * 0.5 + 0.5, 0.5);
    }
    if (view == 10)
        return SceneLightOn() ? saturate(PickupSample(uv, true)) : float3(0, 0, 0);
    if (view == 11)   // game material: reserved, not offered in the UI
        return Integration_Material ? MaterialDiffuse(uv).xxx : float3(0.3, 0, 0.3);
    return depth.xxx;
}

float4 GBufferDebug(float2 uv, float depth)
{
    int view = GBufferDebugView;
    if (view == 1)
    {
        float2 q = floor(uv * 2.0);
        float2 su = frac(uv * 2.0);
        float d = GetLinearDepth(su);
        int tile = int(q.x + 2.0 * q.y);
        int v = (tile == 0) ? 2 : ((tile == 1) ? 3 : ((tile == 2) ? 6 : 0));
        float3 col = GBufferDebugTile(v, su, d);
        float2 fr = abs(su - 0.5);
        return float4((max(fr.x, fr.y) > 0.497) ? float3(1, 1, 1) : col, 1.0);
    }
    if (view == 7)
    {
#if CRCGI_CANVAS_MARGIN > 0
        float4 cv = tex2Dlod(sCanvas, float4(uv, 0, 0));
        float3 col = (cv.a <= 0.0) ? float3(0.0, 0.0, 0.12) : cv.rgb * DebugGain;
        float2 lo = CANVAS_M / (1.0 + 2.0 * CANVAS_M), hi = (1.0 + CANVAS_M) / (1.0 + 2.0 * CANVAS_M);
        float2 px = 2.0 * ReShade::PixelSize;
        bool inside = all(uv >= lo - px) && all(uv <= hi + px);
        bool core = all(uv >= lo + px) && all(uv <= hi - px);
        return float4((inside && !core) ? float3(1, 0.85, 0) : col, 1.0);
#else
        return float4(0.3, 0, 0.3, 1.0);   // CRCGI_CANVAS_MARGIN is 0: there is no canvas
#endif
    }
    if (view == 9 && uv.y < 0.02)
    {
        float4 st = NRotRow(3);
        float3 bar = (st.z > 0.5) ? float3(0, 0.8, 0) : float3(0.8, 0, 0);
        return float4((uv.x < st.x) ? bar : float3(0.1, 0.1, 0.1), 1.0);
    }
    return float4(GBufferDebugTile(view, uv, depth), 1.0);
}

// ----------------------------------------------------------------------------
// Composite
// ----------------------------------------------------------------------------

float4 PS_Composite(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float3 raw = BackBufferWork(uv);
    float3 original = raw;
    if (ConvertTonemapper && !SignalIsHDR())
        original = ConvertTone(raw);
    float centerDepth = GetLinearDepth(uv);

    if (GBufferDebugView > 0 && GBufferDebugView != 8) return GBufferDebug(uv, centerDepth);
    if (DebugView == 4) return float4((1.0 - tex2Dlod(sRaw, float4(uv, 0, 0)).a).xxx, 1.0);
    if (DebugView == 6) return float4(tex2Dlod(sRad, float4(uv, 0, 0)).rgb * DebugGain, 1.0);
    if (DebugView == 7) return float4(tex2Dlod(sC2, float4(uv, 0, 0)).rgb * DebugGain, 1.0);
    if (DebugView == 5) return float4(OctDecode(tex2Dlod(sGeo, float4(uv, 0, 0)).xy) * 0.5 + 0.5, 1.0);
    if (DebugView == 8) return float4(centerDepth.xxx, 1.0);
    if (DebugView == 9)
    {
        float4 g = tex2Dlod(sGeo, float4(uv, 0, 0));
        float cap = ObjectThickness * g.z;
        float th = min(g.w * ThinObjectScale, cap);
        float red = (g.z < SkyDepthThreshold) ? 1.0 - th / max(cap, 1e-6) : 0.0;
        return float4(original * 0.3 + float3(red, 0, 0), 1.0);
    }
    if (DebugView == 10) return float4(tex2Dlod(sRawLin, float4(uv, 0, 0)).rgb * DebugGain, 1.0);
    if (DebugView == 13)
    {
        // Emitters: grey = the light rays actually pick up from each surface (x Debug Gain), orange = the pixels treated as light
        // sources (same smoothstep as the Prep pass, from the surface's pre-filtered brightness), black = nothing sampled (sky, too dark).
        float4 rd = tex2Dlod(sRad, float4(uv, 0, 0));
        float range = max(EmitterThreshold / EmitterSensitivity, 0.01);
        float em = (centerDepth < SkyDepthThreshold) ? smoothstep(EmitterThreshold, EmitterThreshold + range * 0.5, rd.a) : 0.0;
        float3 sampled = dot(rd.rgb, LUMA).xxx * DebugGain;
        float3 emitterCol = float3(1.0, 0.45, 0.0) * max(dot(sampled, float3(0.3333, 0.3333, 0.3333)), 0.35);
        return float4(lerp(sampled, emitterCol, em), 1.0);
    }

    float3 gi = float3(0, 0, 0);
    float contactF = 1.0;
    if (centerDepth < SkyDepthThreshold)
    {
        gi = Finite(UpsampleGI(uv, max(centerDepth, 1e-4)));
        gi = ContactResolve(uv, max(centerDepth, 1e-4), raw, gi, contactF);
    }

    if (DebugView == 11) return float4((contactF * 0.5).xxx, 1.0);
    if (DebugView == 12)
    {
        float4 fl = tex2Dlod(sFlow0, float4(uv, 0, 0));
        return float4(fl.xy / 16.0 * 0.5 + 0.5, saturate(fl.z * 10.0), 1.0);
    }

    if (GBufferDebugView == 8) return float4(gi * DebugGain, 1.0);
    if (CascadeDebugLevel > 0) return float4(gi * DebugGain, 1.0);
    if (DebugView == 1 || DebugView == 2) return float4(gi * DebugGain, 1.0);
    if (DebugView == 3) return float4(tex2Dlod(sRaw, float4(uv, 0, 0)).aaa, 1.0);

    if (centerDepth >= SkyDepthThreshold)   // sky: no GI
        return float4(WorkToOut(TMDitherApply(original, pos.xy)), 1.0);

    float3 added = gi * Intensity * MaterialDiffuse(uv);

    // Added light, soft-limited against the headroom left above the pixel so very bright GI
    // approaches white smoothly instead of clipping. Only the GI term is compressed.
    float3 posPart = max(added, 0.0);
    float3 negPart = min(added, 0.0);
    float3 headroom = max(WorkPeak() - original, HeadroomFloor);
    float3 compressed = headroom * (1.0 - exp(-posPart / headroom));
    added = lerp(posPart, compressed, GIHeadroomRolloff) + negPart;

    return float4(WorkToOut(TMDitherApply(original + added, pos.xy)), 1.0);
}

// ----------------------------------------------------------------------------
// Technique: sky/scene state, geometry + hit light, cascades far -> near (each needs its parent's
// finished result), probe averages, structure, gather, denoise, composite (with the full-resolution
// upsample).
// ----------------------------------------------------------------------------

technique CRCGI_v1 <
    ui_label = "CRCGI v1";
    ui_tooltip = "Compensated Radiance Cascade Global Illumination: radiance cascades with far-cascade accumulation, multibounce,\n"
                 "an off-screen canvas, a directional sky map, HDR support and game G-buffer integration (CRCGI Injector add-on).";
>
{
    pass TM           { VertexShader = PostProcessVS; PixelShader = PS_TM;           RenderTarget = CRCGIV1_TM; }
    pass Sky          { VertexShader = PostProcessVS; PixelShader = PS_Sky;          RenderTarget = CRCGIV1_SkyA; }
    pass SkyMap       { VertexShader = PostProcessVS; PixelShader = PS_SkyMap;       RenderTarget = CRCGIV1_SkyMap; }
    pass Luma         { VertexShader = PostProcessVS; PixelShader = PS_Luma;         RenderTarget = CRCGIV1_Luma; }
    pass Flow3        { VertexShader = PostProcessVS; PixelShader = PS_Flow3;        RenderTarget = CRCGIV1_Flow3; }
    pass Flow2        { VertexShader = PostProcessVS; PixelShader = PS_Flow2;        RenderTarget = CRCGIV1_Flow2; }
    pass Flow1        { VertexShader = PostProcessVS; PixelShader = PS_Flow1;        RenderTarget = CRCGIV1_Flow1; }
    pass Flow0        { VertexShader = PostProcessVS; PixelShader = PS_Flow0;        RenderTarget = CRCGIV1_Flow0; }
    pass NormalFit    { VertexShader = PostProcessVS; PixelShader = PS_NormalFit;    RenderTarget0 = CRCGIV1_NFit0; RenderTarget1 = CRCGIV1_NFit1; RenderTarget2 = CRCGIV1_NFit2; }
    pass NormalRot    { VertexShader = PostProcessVS; PixelShader = PS_NormalRot;    RenderTarget = CRCGIV1_NRot; }
    pass Prep         { VertexShader = PostProcessVS; PixelShader = PS_Prep;         RenderTarget0 = CRCGIV1_Geo; RenderTarget1 = CRCGIV1_Rad; }
#if CRCGI_SECONDARY_DIVISOR > 0
    pass MBMap        { VertexShader = PostProcessVS; PixelShader = PS_MBMap;        RenderTarget = CRCGIV1_MB; }
#endif
    pass Pyr1         { VertexShader = PostProcessVS; PixelShader = PS_Pyr1;         RenderTarget = CRCGIV1_Pyr1; }
    pass Pyr2         { VertexShader = PostProcessVS; PixelShader = PS_Pyr2;         RenderTarget = CRCGIV1_Pyr2; }
    pass Pyr3         { VertexShader = PostProcessVS; PixelShader = PS_Pyr3;         RenderTarget = CRCGIV1_Pyr3; }
#if CRCGI_CANVAS_MARGIN > 0
    pass Canvas       { VertexShader = PostProcessVS; PixelShader = PS_Canvas;       RenderTarget0 = CRCGIV1_Canvas; RenderTarget1 = CRCGIV1_CanvasW; }
#endif
    pass LumStats     { VertexShader = PostProcessVS; PixelShader = PS_LumStats;     RenderTarget = CRCGIV1_Lum; }
#if CRCGI_CASCADES >= 6
    pass Cascade5     { VertexShader = PostProcessVS; PixelShader = PS_Cascade5;     RenderTarget = CRCGIV1_C5; }
#endif
#if CRCGI_CASCADES >= 5
    pass Cascade4     { VertexShader = PostProcessVS; PixelShader = PS_Cascade4;     RenderTarget = CRCGIV1_C4; }
#endif
    pass Cascade3     { VertexShader = PostProcessVS; PixelShader = PS_Cascade3;     RenderTarget = CRCGIV1_C3; }
    pass Cascade2     { VertexShader = PostProcessVS; PixelShader = PS_Cascade2;     RenderTarget = CRCGIV1_C2; }
    pass Cascade1     { VertexShader = PostProcessVS; PixelShader = PS_Cascade1;     RenderTarget = CRCGIV1_C1; }
    pass Cascade0     { VertexShader = PostProcessVS; PixelShader = PS_Cascade0;     RenderTarget = CRCGIV1_C0; }
    pass ProbeAvg0    { VertexShader = PostProcessVS; PixelShader = PS_ProbeAvg0;    RenderTarget0 = CRCGIV1_P0A; RenderTarget1 = CRCGIV1_P0B; }
    pass Gather       { VertexShader = PostProcessVS; PixelShader = PS_Gather;       RenderTarget = CRCGIV1_Raw; }
    pass DenoiseH     { VertexShader = PostProcessVS; PixelShader = PS_DenoiseH;     RenderTarget = CRCGIV1_DenTmp; }
    pass DenoiseV     { VertexShader = PostProcessVS; PixelShader = PS_DenoiseV;     RenderTarget = CRCGIV1_Den; }
    pass CopyPrev     { VertexShader = PostProcessVS; PixelShader = PS_CopyPrev;     RenderTarget0 = CRCGIV1_Hist; RenderTarget1 = CRCGIV1_LumaPrev; RenderTarget2 = CRCGIV1_GeoPrev; }
#if CRCGI_CANVAS_MARGIN > 0
    pass CopyCanvas   { VertexShader = PostProcessVS; PixelShader = PS_CopyCanvas;   RenderTarget0 = CRCGIV1_CanvasPrev; RenderTarget1 = CRCGIV1_CanvasWPrev; }
#endif
    pass CopySkyMap   { VertexShader = PostProcessVS; PixelShader = PS_CopySkyMap;   RenderTarget = CRCGIV1_SkyMapPrev; }
    pass CopyNormalRot { VertexShader = PostProcessVS; PixelShader = PS_CopyNormalRot; RenderTarget = CRCGIV1_NRotPrev; }
    pass CopyHC1      { VertexShader = PostProcessVS; PixelShader = PS_CopyHC1;      RenderTarget = CRCGIV1_HC1; ClearRenderTargets = false; }
    pass CopyHC2      { VertexShader = PostProcessVS; PixelShader = PS_CopyHC2;      RenderTarget = CRCGIV1_HC2; ClearRenderTargets = false; }
    pass CopyHC3      { VertexShader = PostProcessVS; PixelShader = PS_CopyHC3;      RenderTarget = CRCGIV1_HC3; ClearRenderTargets = false; }
#if CRCGI_CASCADES >= 5
    pass CopyHC4      { VertexShader = PostProcessVS; PixelShader = PS_CopyHC4;      RenderTarget = CRCGIV1_HC4; ClearRenderTargets = false; }
#endif
#if CRCGI_CASCADES >= 6
    pass CopyHC5      { VertexShader = PostProcessVS; PixelShader = PS_CopyHC5;      RenderTarget = CRCGIV1_HC5; ClearRenderTargets = false; }
#endif
    pass Contact      { VertexShader = PostProcessVS; PixelShader = PS_Contact;      RenderTarget = CRCGIV1_Contact; }
    pass Composite    { VertexShader = PostProcessVS; PixelShader = PS_Composite; }
}
