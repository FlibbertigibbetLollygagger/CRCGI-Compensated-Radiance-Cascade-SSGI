# Changelog

## v2

Shader (CRCGI_v2.fx, built on v1.6)
- Geometry: every effect texel fits a robust plane to the full-resolution depths of a window around it (8 x 8 pixels at divisor 4, was the
  4 x 4 nearest-depth pick plus a separate plane fit and neighbour smoothing). The plane gives the depth at the texel centre (steady when the
  camera or the game's TAA jitter moves by a pixel), the normal, and how thin the feature is: foliage, grass and wires are stored at their own
  pixel, face the camera and are marked thin. Specks (a single depth that nothing else agrees with) are skipped for the next surface.
- Probe tables: one small pass per cascade computes, once per probe, where the probe sits, how far its history can be trusted and the weights
  of its parents (merge) and of its history (far-cascade accumulation). Every texel of a probe (4 to 1024 of them) used to recompute it.
- Multibounce colour runaway in single-coloured rooms is fixed. The colour of the light the GI explains is divided out of a surface's colour
  before it is used as reflectance (von Kries adaptation), the loop reads the unblurred, unshaped accumulated light (was the denoised, shaped
  result), and the bounce shaping (Bounce Saturation, colour filters, bounce curve, Dark Area Boost) is applied to what is shown only. The
  half-desaturation and the extra luminance clamp of the bounce are gone; the 0.9-per-bounce energy guard stays.
- Shadow (was Contact Shadowing): the escape visibility of the two nearest cascades is a shadow source of its own, filtered by the same
  temporal accumulation and a-trous passes as the GI. New Shadow Contact Bias overlaps it with the contact march (0 = escape only, the
  contact passes are skipped; 1 = contact only).
- The contact march runs at the effect resolution (one probe per effect texel; was every screen pixel): new CRCGI_CONTACT_DIVISOR, 0 (the
  default) follows CRCGI_RESOLUTION_DIVISOR, 1 to 6 sets it on its own (1 = full resolution as before). The resolve upsamples it on depth,
  so a pixel only takes the contact shadow of its own surface; the blur taps are at least one contact texel apart. Crevice Light Bounce Bias is now Shadow Amount with the sign turned round (more =
  darker), presets are converted.
- Contact grain: the march reads depth at pixel centres (no filtered in-between depths at silhouettes), its hemisphere follows the cascades'
  normal instead of the per-pixel one, the occupancy ramp covers the depth a step crosses, and thin features (three scales plus the geometry
  pass) take no contact shadow. The contact history option is removed (the march is deterministic).
- Adaptive Ray Count: decided by the geometry only (depth, normal and age of the history), not by the motion match, which follows the image
  noise; "save on stable ones" saves where the history is settled instead of boosting almost everywhere.
- Off-screen canvas as a radiance cache: it keeps the light with its multibounce, and an uncertain motion match costs at most a tenth of the
  trust per frame (it compounded the whole motion penalty every frame).
- Game material: Use Game Material (reflectivity) If Provided is offered (metals receive and hand on less GI), with a debug view.
- Fewer passes and less data: the history copies are written by the last a-trous pass and by the nearest cascade (2 copy passes removed);
  geometry split into a 4-byte depth and an 8-byte normal/depth/thickness texture; history geometry, probe averages and the depth pyramid in
  half precision; Luma reads a quarter of the taps; the upsample uses a 4-tap normal; the Shadow resolve reads the surface brightness at
  cascade resolution instead of 16 full-resolution taps.
- Settings removed (now automatic): Smoothed Normals, Normal Smoothing Radius, Normal Smoothing Edge Stop, Depth Downsample, Adaptive: Extra
  Rays, Contact Frames Accumulated, Contact Depth Tolerance. The Redundancy section is gone.
- Defaults: every strength, intensity and bias at 1 or at half its range. At 1: GI Intensity, Energy Carry, Emitter Boost, Shadow Amount,
  Shadow Contact Depth Bias, Bounce Saturation, Screen Edge Extension, Dark Surfaces Block Rays and Foreground-to-Background Block. At half:
  Shadow Contact Bias, Blue and Orange Light Filter, Surface Albedo Response, Fade With Receiver Distance and Facing Check Bias 0.5
  (Temperature Strength and Denoise Blur Strength already sit at half their range). Ray Jitter Accumulation, Far Cascades: Jitter and Ray
  Start Jitter 0.4; Convert Tonemapper, Sky Light, HDR Light Pickup, Relit Pickup and Dark Area Boost off. Caps, limits, thresholds and
  tolerances as in v1.6. CRCGI_CASCADES 5, CRCGI_RESOLUTION_DIVISOR 4, CRCGI_CONTACT_DIVISOR 0 (the effect resolution).
- Textures are named CRCGIV2_*, the technique is CRCGI_v2.

Add-on (CRCGI Injector v2)
- The material feed is offered in the overlay (channel and meaning in the G-Buffer tab).
- After a shader update the saved techniques (which name the old file) no longer leave Force render with nothing to render: when none of
  them is loaded, the CRCGI technique is picked again.
- The version is shown in the Build line and written to the log. The notes about asynchronous compute are removed.

Presets
- v2 presets for every game folder ("bright" and "subtle" as before) and DEFAULT.ini. Tuned values are kept; values a preset left at its
  shader's default, and the settings whose meaning changed with the filtering stack (denoise, jitter, adaptive rays, light guard), take the
  v2 defaults, so the presets sit closer to v1.6. tools/port_preset.py --follow-defaults does this.

## v1.6

Shader (CRCGI_v1.6.fx), from the notes in the file
- Temporal accumulation runs on the raw gather (with luminance moments, so every pixel has a stable variance), the history is kept before
  the blur, and the light-change test is variance-aware.
- The 7-tap cross blur is replaced by a variance-guided a-trous filter (5 x 5 taps, spacing doubling per pass); Denoise Iterations and
  Denoise Blur Strength replace Denoise Kernel Radius.
- Ray jitter is scaled by each pixel's history confidence and cycles every 16 frames.
- Cascade geometry in 32-bit float; normals rebuilt by a robust plane fit (5 x 5 taps, two rounds of residual-weighted least squares).
- Chroma-aware motion (YCoCg pyramid, box-filtered over the cascade footprint).
- Adaptive Ray Count decides from the weakest of four signals.
- Contact shadows: one pass (optional history), their own 2-pass blur, and they fade out on thin geometry.
- The per-cascade history copies are one multi-target pass; the a-trous chain is 5 passes.
- Rarely needed sliders moved to a Redundancy section. CRCGI_RESOLUTION_DIVISOR defaults to 4.

## v1.5

Shader (CRCGI_v1.5.fx)
- Temporal accumulation: history age and history reliability are kept apart. The blend still takes every check of the frame (motion match,
  surface agreement, light change), but the age only restarts on a real disocclusion or light change. Steady camera movement with an
  uncertain motion match no longer keeps the GI young and noisy, new areas and switched-on lights still clean up within a few frames.
- Directional sampling: 3 rays per cell (the default) sit on an even lattice centred on the cell instead of three of the four quadrant
  centres (one quadrant was never sampled and every cell's mean direction leaned to one corner). With Ray Jitter Accumulation the pattern
  moves around the whole cell every frame for 1 to 3 rays, so the cell is fully covered within a few frames. 4 rays are unchanged.
- Adaptive Ray Count: a history that is still young counts as partly lost, so newly revealed areas keep their extra rays until the
  accumulation has caught up, and "save on stable ones" only saves where the history is mature.
- Contact Shadowing, Crevice Light Bounce Bias below 0: the cascades' own visibility joins the contact visibility (bilinear, never below
  35%, mixed at 45%), the surroundings count three quarters as open air so medium-range occlusion is kept, and the contact factor darkens
  the game's image at 65% before the GI is added (still through the ui preservation and the output encoding). 0 and above are unchanged.
- No new settings, no changed defaults, ranges or meanings, no new passes or textures. Every setting keeps its name, so presets of every
  version port to CRCGI_v1.5.fx unchanged (tools/port_preset.py or tools/retarget_preset.py).

Add-on (CRCGI Injector v1.5)
- G-buffer data no longer drops out when the camera turns. A layer's identity no longer includes the shader of its first draw (that changed
  with what the camera saw), and feeds follow a layer's layout (target count, formats, screen-relative sizes, depth) instead of one
  binding, so a texture whose address changes and changes back is still found.
- Normals, albedo, motion and material are captured at the end of a write run (all consecutive intervals drawing into the texture with that
  layout), so a G-buffer drawn in several intervals is captured complete. Later runs only replace the copy if they continue it or draw
  more, so unrelated passes reusing the memory still can't overwrite it. HDR scene light and finished frame stay interval-exact.
- The last interval of a command list is captured too (lists closed right after the G-buffer pass were missed).
- Driven shader switches are set where the textures are bound, never stay on while a texture is unbound, and come on after 3 delivered
  frames in a row: no flicker between game data and the shader's own estimate. Albedo and material bridge short capture gaps (8 frames).
- Shared target listings: feeds assigned from a layer are saved as the layer's layout, so they are found again at other resolutions.
  Feeds assigned from a layer by an older add-on need to be assigned once more (written to the ReShade log).

Presets
- Batman Arkham Knight and Promotional Atmosphere Lighting Mod: the v1.2 presets are regenerated as complete files (every setting, in the
  form ReShade saves them, v1.1 values). New v1.5 presets: "bright" (the v1.1 tuning) for both, "subtle" (the Better Changes Vanilla
  Lighting Mod tuning) for Promotional Atmosphere Lighting Mod and Better Changes Vanilla Lighting Mod.
- tools/check_presets.py checks every preset against its shader, tools/port_preset.py ports a preset to the newest shader.

Build
- Release and build workflows run on windows-2022 and check every preset.

## v1.2

Shader (CRCGI_v1.2.fx)
- Temporal stability: every pixel of the blended GI now carries a history age. A newly revealed surface takes 1/2, 1/3, ... of each new frame
  and settles in a few frames (it used to keep its first noisy frame at full weight), a clearly rejected history starts again, and once the age
  is full the blend is exactly what it was, so a steady picture looks the same.
- Temporal reprojection: the history is read from the four texels around the reprojected position, each weighted by depth and normal
  agreement (no light crossing from another surface), and the far-cascade history weights its four probes the same way.
- Motion estimation: candidates are also scored by the depth of the surface they land on, near-ties between far apart candidates lower the
  match confidence a little, and the previous frame is read once per search window instead of five times per candidate (cheaper, same result).
- The motion passes (and the luminance they read) are skipped when nothing uses the motion estimate.
- New: Use Game Finished Frame If Provided (Game Integration). The effect works on the game's finished 3D image from the add-on and adds its
  light to the final image only where the final image still shows it, so ui and fog stay as the game drew them.

Add-on (CRCGI Injector v1.2)
- The effect is no longer rendered at a layer to keep the ui out. It runs at the end of the frame and the new Finished frame feed leaves the ui
  and fog alone. Forcing the effect into the frame is still there, as a separate advanced option (Options > Force render, role "Force render here").
- Auto-setup and auto-detect are removed. The profiler suggests (score bars per feed, best candidate marked) and you assign.
- The layer list only shows layers drawn every frame (switchable), empty draws and clear-only bindings no longer create layers, and layers not
  drawn for 10 seconds are dropped.
- Fixed: feeds flashing when the game reuses texture memory later in the frame (a texture is captured only at its main binding, one binding per frame).
- Fixed: profiler snapshot frame check (off by one), layer key drift when a command list is reused or recorded in another order, and the frame
  counter read across threads (now atomic).
- Fixed: Direct3D 12 copies are never made at barriers (split barrier hazard) and, inside a render pass, are made right after it ends.
- Hooks: indirect draws, compute dispatches (with the bound UAVs on D3D10/11) and clears are watched; draws that draw nothing are ignored.
- Profiler scoring: plain RGBA8 and R11G11B10 albedo, HDR in dark scenes, four-channel motion vectors, and a finished-frame score.
- Depth views for D24S8 and D32S8 depth buffers.
- Layout: lists are cards and score bars stack vertically, everything scales with the window width.
- Sharing: Options > Share exports and imports a target listing (crcgi_targets_<game>.ini) that is found again at other screen resolutions.
- Honest note: It is still very very buggy and idk how to fix it tbh

## v1.1

Improved temporal stability. It is now geometry aware and normal aware, should look considerably more coherent during movement.
Removed 4C from file name (stood for 4 cascades, I used different files for different cascade amounts during development)
Removed pre-release screenshots
Changes will not be reflected in the documentation because they are minor, really

## v1.0.0

Initial release.
