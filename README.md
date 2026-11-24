# CRCGI v2 (Compensated Radiance Cascade Global Illumination)

CRCGI is a global illumination effect for ReShade (it's built on radiance cascades), and it comes with an add-on, the CRCGI Injector, that hands the effect what the game already knows (normals, albedo, motion vectors, depth, camera fov) and lets the effect ignore the ui and fog (it works on a copy of the game's finished 3D picture and adds its light to the final image). This README explains how both parts work, how to install them, and how to tune the settings, every setting is listed near the bottom with its default, range and what it does (the tooltips in ReShade say the same thing, just shorter).

The shader works on its own like any other ReShade effect. The add-on is optional, it just removes guesswork (without it the shader rebuilds normals from depth, estimates motion by block matching, and estimates reflectance from the image) and lets the effect leave the ui and fog alone instead of processing the whole final image.


**Why this approach matters.** Instead of relying on independent, randomly scattered rays, CRCGI uses structured radiance cascades to reuse directional lighting information across distances. It combines depth-aware hit detection, visibility-preserving cascade merging, temporal stabilization, and a remembered off-screen canvas to make diffuse GI practical in a screen-space ReShade effect. The optional add-on can provide real game buffers and place the effect earlier in the rendering pipeline, avoiding some of the limitations of sampling only the final image. The goal is playable, subtle lighting that complements the game's art direction, not a screenshot-only effect.

## Requirements

- ReShade with add-on support (the add-on build, 6.x) for the exe you're playing. The add-on is built against one pinned ReShade revision (it's written in the add-on's Options tab under Build, and in `.github/workflows/build.yml` in my private repo), so use a ReShade 6.x add-on build that's close to it if something doesn't load.
- Direct3D 10, 11 or 12, Vulkan, or OpenGL. DirectX 9 games work through DXVK (ReShade sees that as Vulkan), native DirectX 9 isn't supported because CRCGI needs Direct3D 10 or newer.
- A 64-bit game uses `crcgi_injector.addon64`, a 32-bit game uses `crcgi_injector.addon32`.

## Install

1. Copy `crcgi_injector.addon64` (64-bit game) or `crcgi_injector.addon32` (32-bit game) next to the game's exe, where ReShade's dll is.
2. Copy `CRCGI_v2.fx` into `reshade-shaders\Shaders`.
3. Start the game, enable **CRCGI v2** in ReShade, then open **Add-ons > CRCGI Injector**.

The `shader` folder also has the earlier versions (`CRCGI_v1.5.fx`, `CRCGI_v1.6.fx` and older) for the presets made with them. Use one version at a time, each has its own technique.

For example in The Witcher 3 the DX12 exe is `bin\x64_dx12\witcher3.exe` and the DX11 one is `bin\x64\witcher3.exe`, the game is 64-bit so you want the `.addon64`, and on DX12 forcing the effect into the frame (an advanced option) is experimental (more on that below).

Out of the box the add-on is on **level A** (the effect runs at the end of the frame like any ReShade effect) and it doesn't touch any of the shader's switches, so a first start can't go wrong because of a wrong detection. You turn the extra stuff on yourself when you want it (first run, below). The shader starts with **GI Intensity at 1** and the optional features off (tonemapper conversion, sky light, HDR light pickup, relit pickup, dark area boost), so the first thing you see is the plain GI on the game's own picture (the tuning guide starts there).

## How the shader works

### The pipeline, in order

It's one technique with a lot of passes. Most of them run at the **effect resolution** (screen size divided by `CRCGI_RESOLUTION_DIVISOR`, default 4), the Shadow's contact march included (it follows the same divisor by default, see `CRCGI_CONTACT_DIVISOR`). Only the composite runs at full resolution.

| Stage | Passes | What it does |
|---|---|---|
| State | TM, Sky, SkyMap | tonemapper exposure state, the average sky tint from 6 points near the top of the screen, and a small map of the sky's color across the screen |
| Color and motion | Luma, Flow3 to Flow0 | a color pyramid (YCoCg, the box average of each texel's pixels), then a block-matching motion estimate (coarse to fine, candidates are scored by color and by the depth of the surface they land on). With *Use Game Motion Vectors* on, or when nothing uses the motion (Temporal Accumulation, Multibounce, Relit Pickup and the off-screen canvas all off), these passes do no work |
| World-space normals | NormalFit, NormalRot | only used when the game's normals are in world space, they fit the rotation that lines the game's normals up with normals rebuilt from depth (about 9000 samples every frame) and apply it |
| Prep | Prep | builds the per-texel geometry (depth, normal, thickness, how thin the feature is) and the hit radiance, which is the light a ray gets when it hits that surface (the image, relit pickup, HDR pickup, the game's HDR scene light) |
| Secondary | MBMap, Pyr1 to Pyr3, Canvas, LumStats | the multibounce map (last frame's accumulated GI reprojected, at `CRCGI_SECONDARY_DIVISOR`), the min/max depth pyramid for the far rays, the off-screen canvas, and the screen brightness for Dark Area Boost |
| Probe tables | ProbeTable0 to ProbeTable5 | one small pass per cascade, one pixel per probe: where the probe sits, how far its history can be trusted, and the weights of its parent probes and of its history. Every texel of a probe used to work this out for itself |
| **Cascades** | far cascade down to cascade 1 | trace and merge, farthest first, each one merged into the next nearer one. The last one also writes the history of the accumulated cascades |
| Gather | ProbeAvg0, Gather | average each probe of the nearest cascade, then an edge-aware read of those probes into per-texel GI plus sky ambient and the near escape visibility |
| Filter | TemporalRaw, Atrous0 to Atrous4 | temporal accumulation of the raw GI (with luminance moments, so every pixel knows its own noise), then up to 5 variance-guided a-trous passes (SVGF). The last pass applies the bounce shaping and writes the history for the next frame |
| History | CopyCanvas, CopySkyMap, CopyNormalRot | the three small copies that are left (the canvas, the sky map and the normal rotation) |
| Output | Contact, ContactBH, ContactBV, Composite | the Shadow's contact march at the effect resolution and its blur (all three skipped while Shadow Contact Bias or Shadow Amount is 0), then the edge-aware upsample to full resolution, the Shadow (its contact part upsampled on depth), albedo response, rolloff, add to the image, dither |

Unused cascade passes are removed from the technique, so at the default 5 cascades the passes for cascade 6 aren't even there. The Emitters debug view (Debug View) shows what the Prep pass samples light from and which pixels it treats as light sources.

### Geometry (depth, normals and thin features)

Everything the rays do depends on the geometry of each texel, so this is where most of the noise used to come from. Each texel looks at a window of the full-resolution depth around it (its own block of pixels plus half a texel on every side, so 8 x 8 pixels at divisor 4) and fits a **robust plane** through the nearest surface of its own block (two rounds of reweighted least squares, pixels of another surface get no weight). From that one fit:

- the **depth** is the plane at the texel centre. It doesn't jump between pixels when the camera or the game's TAA jitter moves by a pixel, and it never invents an in-between depth at a silhouette.
- the **normal** is the slope of the same plane, so there's no second fit and no neighbour smoothing needed.
- the share of the window that lies on the plane says **how thin** the feature is. A twig, a blade of grass or a wire covers only a few pixels of the window, so it's stored at its own pixel (no extrapolation off the feature), its normal turns towards the camera (a normal rebuilt from a one-pixel-wide feature is just noise) and it's marked thin, so the Shadow leaves it alone.
- a nearest pixel that almost nothing else in the window agrees with (a speck, one noisy depth) is skipped for the next surface.

With the game's normals (Game Integration) the game's normal is used instead and the rest stays the same.

### Cascades (layout, rays and merging)

Each cascade has probes spaced `tile x divisor` screen pixels apart, and `tile^2` directions per probe. With the defaults (`CRCGI_BASE_TILE 2`, divisor 4, 5 cascades) it looks like this at 2560 x 1440 (the example interval ends are approximate screen-pixel distances and depend on the viewport):

| Cascade | Tile | Directions per probe | Probe spacing (px) | Ray interval ends at (px) | Steps per ray |
|---|---|---|---|---|---|
| 1 (nearest) | 2 | 4 | 8 | about 6.4 | 4 (Ray Steps: near) |
| 2 | 4 | 16 | 16 | about 18 | 4 (near) |
| 3 | 8 | 64 | 32 | about 53 | 12 (Ray Steps: far) |
| 4 | 16 | 256 | 64 | about 150 | 12 (far) |
| 5 | 32 | 1024 | 128 | about 440 (Cascade Reach, 0.15 screen diagonals) | 12 (far) |

The penumbra layout says a cascade's interval should end where the probe spacing equals the ray spacing, which is `tile^2 x divisor / 2.507` screen pixels, so every interval ends up 4 times longer than the last (that's the layout of cascade 1 only in the table above, and it's what you get for the later cascades too if you raise Reach Applies From Cascade). **Reach Applies From Cascade** (default 2) is the first cascade that *Cascade Reach* stretches, the earlier ones keep the penumbra layout, and the cascades from it onwards are spread geometrically from the end of the previous one up to the reach. With `CRCGI_CASCADES` at 5 or 6 the extra cascades just keep stretching (1024 and 4096 directions).

Each ray only traces its own interval, sampling the depth buffer at 4 or 12 points with a jittered start. A ray that **hits** returns the light of that surface, one that is **blocked** by a nearer surface returns nothing, and one that **escapes** its interval gets its answer from the parent cascade.

**Sub-rays inside a direction cell.** *Rays Per Cell* splits every direction cell: 1 ray sits in the middle, 2 and 4 rays on the quadrant centres, and 3 rays (the default) on an even lattice across the cell (1/6, 1/6), (1/2, 5/6), (5/6, 1/2), which is centred on the cell, so no part of the cell is left out and the cell's average direction stays where it should be. With *Ray Jitter Accumulation* the pattern moves around the cell every frame on a low-discrepancy (R2) sequence, the whole pattern together for 1 to 3 rays (wrapping around the cell's edges, so every part of the cell gets sampled within a few frames) and each ray inside its own quadrant for 4. Rays never leave their own cell, so the merge between cascades sees the same cells as before, and each frame already covers the cell evenly instead of relying on the history to fill it.

**Merging.** An escaped ray looks up its 4 child directions in the 4 bilinearly weighted parent probes. Parent probes are weighted by how similar the surface is (relative depth and normal agreement, or distance from the tangent plane) and by a **merge visibility** test (3 taps along the segment to each parent, so light doesn't leak around a pillar). Every cascade carries visibility (the alpha channel) next to the radiance (rgb), so a blocked chain stays blocked. These parent weights depend only on the probe, not on the direction, so they're worked out once per probe in the probe tables (the far cascades have up to 1024 texels per probe that used to repeat the same work).

**Energy Carry** multiplies the light handed down by each merge by `carry ^ (interval ratio)` (1 per 4x step). At the default 1 it does nothing (physically nothing is lost or gained in a merge). Above 1 the farthest cascade reaches the nearest one multiplied several times, which makes up for the energy the far cascades lose (missed thin geometry, probe interpolation, depth rejection), below 1 it favors near light. *Energy Carry From Cascade* limits it to the farther cascades.

**Cross-fade** lets each cascade's rays go on for 25% into the next interval and fade out while the next one fades in, which gets rid of rings at fixed distances.

**Far cascades** (from *Far Cascades: Accumulate From*, default 2) have few, long rays, so their noise is big and static and can't be filtered spatially. They get their own history, they jitter their probes inside the middle half of their tile and their rays inside their direction cell every frame, and accumulates them over `Frames Accumulated x Growth ^ (steps past start + 1)` frames (capped at 64, so 16 frames for every accumulated cascade at the defaults, Growth 1). The jitter fades with each probe's history confidence, like the ray jitter. A brightness rise much bigger than the noise goes through faster, so a lamp you switch on doesn't leave a long trail. *Half-Rate Update* (off by default) traces half of the probes of these cascades per frame in a checkerboard.

### Rays, hits and blockers (what "hit detection" means)

This is the part everything else sits on, so it's worth reading slowly. A depth buffer only knows the **front** of things, it doesn't know how thick an object is, so the shader has to decide for every step of a ray whether the ray is "in" a surface. It does that by comparing two depths (both are linear depth, 0 at the camera and 1 at the far plane):

- the **surface depth**, what the depth buffer says is visible at the spot the ray is above
- the **ray depth**, how deep the ray itself is at that spot (from the **Ray Depth Model**, default *Perspective-correct*, which uses the real 3D ray and the field of view, the other one is a slope guess)

Then one of three things happens:

| Where the ray depth is | Result | What it means for the picture |
|---|---|---|
| inside the surface's **hit band** (from a little in front of the surface to the band's thickness behind it) | **hit** | the ray picks up that surface's light |
| behind a nearer surface by more than the band and by more than *Occlusion Depth Threshold*, and that surface is allowed to block (*Occlusion Tolerance*, *Occluder Depth Range*) | **blocked** | the ray returns nothing (a shadow) |
| anywhere else (in front of the surface, or behind it but not far enough behind) | **passes** | the ray keeps going |

The **hit band** is the important one. Its thickness is the object thickness (a fixed share of the surface's own depth in *Generic* mode, or measured from silhouettes in *Silhouette-derived* mode, see Hit Thickness Mode), plus how much the ray's own depth changes in one step, plus the surface slope, and it's never thicker than *Max Hit Thickness*. Far cascades use the min/max depth pyramid of the whole footprint of the step instead of a guessed thickness. *Hit Front Tolerance* is how far in front of the surface (as a share of the band) the ray may stop and still count.

After a hit, the light is weighted by **depth separation** (a character standing far in front of a wall doesn't tint that wall), the **facing check** (the back of a lit surface bounces very little) and the optional distance falloff. Dark surfaces get a thicker band, so they act as shadow casters.

**What you see when it's wrong.** A band that's too thin and rays pass through objects (light missing from thin things, poles that don't block, bright light leaking from behind objects). A band that's too thick and rays hit surfaces from far behind them (dark halos, darkened surroundings of the player, splotchy far cascades). Blocking too eager and every crevice and every thin thing is a shadow, too lazy and there are no contact shadows at all. Look at *Debug View > Hit Radiance Buffer*, *Escape Visibility* and *Object Thickness* while you tune it.

**Depth scales are not the same in every game.** A depth buffer isn't linear to begin with (standard, reversed or logarithmic), ReShade linearizes it with its own depth settings (the far plane, reversed, logarithmic and upside-down options of its depth setup, `DisplayDepth.fx` is the way to get them right), and "linear depth 1" is whatever far plane that gives. So the same slider value is a different real distance in a game with a 100 m far plane than in one with 10 km, and a game whose geometry sits at depth 0.001 to 0.05 (long draw distance) needs very different absolute values than one that fills 0 to 1. That's why the absolute depth checks (*Sky Rejection Depth*, *Max GI Distance*, *Occlusion Tolerance*, *Occlusion Depth Threshold*, *Ray Depth Drift Limit*) have to be set per game, and why *Relative Depth Tolerances* exists (it scales them by the depth of whatever is being tested). The checks that are written as a share of the surface's own depth (*Object Thickness Cap*, *Max Hit Thickness*, *Depth Separation Limit*) don't care about the game's depth scale.

### What a ray picks up

What a ray gets at a hit is built in the Prep pass, the image over a 5-tap blur, gamma, a smooth surface/emitter weighting (half weight at Surface Bounce Threshold, emitters boosted above Emitter Threshold) and a brightness cap against fireflies. **Debug View > Emitters** shows exactly this (grey is the light rays pick up from ordinary surfaces, orange is what the shader treats as a light source, black is nothing sampled: sky, or surfaces too dark to count). Three things sit on top of that:

- **Relit pickup** (Relit Pickup Bias, 0 = off). The surface gets lit again from what this effect knows (its reflectance times the GI that last frame's accumulation put on that spot, unblurred) instead of the game's baked lighting. Light sources and bright surfaces keep their image color, since direct sunlight isn't part of the GI. With the game's albedo the reflectance is exact and the 16-tap image estimate is skipped.
- **Multibounce.** Rays also pick up last frame's GI on the surface they hit, as light of that surface's color. The bounce is `reflectance x GI received x GI Intensity x Multibounce Strength` and it's held below 0.9 of the light it came from (that's the energy conservation the loop between frames needs: reflectance times gain must stay under 1 for the series of bounces to settle). It's read over the penumbra of the ray that found the hit, weighted by depth and normal agreement.
  - **Why single-colored rooms don't run away any more.** What the image shows of a surface is its reflectance times the light that reached it, so a white wall under a red lamp *looks* red. Taking that color as the wall's reflectance bounces red times red light, and with the bounce shaping inside the loop (the saturation boost was applied to the light that bounced again) every frame came out redder than the last. Now the color of the light the GI explains is divided out of the surface's color first (a von Kries adaptation to a mix of the GI and white direct light, weighted by how much of the surface's brightness the GI explains), the loop reads the plain accumulated light (no blur, no shaping), and the shaping (Bounce Saturation, the color filters, the bounce curve, Dark Area Boost) is applied only to what you see. The bounce then settles at the saturation the surface really has (the property tests run 300 single-colored rooms: at most 0.05 more saturated than seen, where the old loop ran up to 0.39 over and kept going).
- **HDR Light Pickup.** (only when you raise it from 0) Bright sources are expanded by the inverse of the game's tonemapper (or taken from real HDR data), so lamps and fire emit more than a white wall does. Mid gray and everything darker stays the same.

### Temporal, denoise, upsample

The GI is gathered from the probes of the nearest cascade (a 4x4 tent when Gather Smoothing is above 0), then filtered in the order SVGF uses: **temporal first, on the raw light, then spatial**.

- **Temporal accumulation** blends the raw gather with the history reprojected by the motion (game vectors or the estimate), after clamping fireflies against the 3x3 neighbourhood. The history is read from the four texels around the reprojected position, each weighted by how well its depth and normal agree with the pixel, so light doesn't cross over from another surface. It also keeps the **luminance moments** (mean and mean of squares), so every pixel knows its own noise, and the light-change test ignores differences inside that noise (noise can't lower the history weight and feed itself). The history is kept **before** the blur, so the blur never feeds back.
- Every pixel carries its **history age**: a pixel without history takes 1/2, 1/3, 1/4 of each new frame (instead of 1 - 1/Frames at once), so a newly revealed surface settles in a few frames. Age and reliability are kept apart: how much history a pixel takes this frame uses every check (motion match, surface agreement, light change), but the age only follows whether it's still the **same surface under the same light**, so steady camera movement doesn't keep the GI young and noisy.
- **Spatial denoise** is a variance-guided a-trous filter (5 x 5 B-spline taps, the spacing doubling every pass, up to 5 passes, reach about 62 texels each side at the defaults). Its edge stops are the surface plane, the normal and the color weighted by the pixel's own noise, so settled, clean pixels stay sharp and noisy or young ones blur widely. The Shadow's escape visibility goes through the same passes in its own channel.
- The **upsample** to full resolution is edge-aware (every pixel only takes light from low-res texels on its own surface).

**Adaptive Ray Count.** A probe whose surface was hidden last frame (its depth or normal doesn't match, or its history is younger than 4 frames) traces 4 rays per cell so new areas clean up faster, and *save on stable ones* traces one fewer only where the history is settled. Only the geometry decides, not the motion match: its error follows the image noise, and a decision that flips with the noise switched probes between ray counts every frame (that was noise of its own, and it kept the boost on almost everywhere, which is why "save" used to cost more than it saved).

### Canvas, sky, Shadow

- **Off-screen canvas (a radiance cache).** The light (including its multibounce) and depth of what recently left the screen are remembered in a border around the screen (`CRCGI_CANVAS_MARGIN` percent per side, default 24), and moved along with the motion vectors. Rays that leave the screen keep marching through it. Its trust fades by **Canvas Memory** per frame (0.985 means half trust after about 0.8 s at 60 fps), and an uncertain motion match takes at most a tenth of the trust per frame (it used to compound the whole motion penalty every frame, which emptied the canvas within a few frames of ordinary camera movement).
- **Sky.** A tintable ambient added to the GI and gated by the cascades' own visibility, escaped upward directions carry sky light and blocked ones don't. A small directional sky map gives the light the sky's color gradient. Off by default.
- **Shadow.** Places more enclosed than their surroundings (crevices, corners, the foot of an object) get darker. It has two sources, and **Shadow Contact Bias** decides how much each counts:
  - the **escape visibility** of the two nearest cascades (the share of rays that leave a spot without hitting anything within about 18 pixels at the defaults). It's traced anyway, so it costs nothing extra, and it's filtered by the same temporal accumulation and a-trous passes as the GI, so it's smooth. Thin features (from the geometry above) take no escape shadow.
  - the **contact march**, the shortest cascade (8 directions, 4 steps against the full-resolution depth) for the finest contact lines, with its own 2-pass blur. It runs at the effect resolution, one probe per effect texel, so it costs what one effect pass costs and gets cheaper with the divisor like everything else. The resolve brings it to full resolution on depth: each pixel takes the 4 nearest contact texels weighted by how well their depth matches its own, so a shadow never spills onto a surface in front of or behind it. `CRCGI_CONTACT_DIVISOR` 1 puts it back to one probe per pixel.

  At bias 0 only the escape visibility counts and the three contact passes are skipped. At 1 the contact march overrides it. In between they overlap: `T = lerp(T_escape, 1, bias) x lerp(1, T_contact, bias)`. **Shadow Amount** then sets how strongly that darkens: the result is compared with the same thing around the pixel, and the factor is `(T / T_surroundings) ^ (2 x Shadow Amount)`, so open, flat areas stay as they are. Above 0 it also darkens the **game's own image** at 65% strength before the GI is added, and that darker base plus the GI still goes through the finished-frame ui preservation and the output encoding like everything else. Below 0 it does the opposite (crevices get more bounce light) and the game's image is left untouched. At 0 everything is skipped.
  - **Why the contact shadows were grainy, and what changed.** A blur doesn't fix it because the grain came from the march itself: the depth taps were filtered, so at silhouettes they returned an in-between depth that blocked rays where nothing is; the hemisphere followed the per-pixel normal, which flips on thin or noisy geometry and takes every ray direction with it; the occupancy ramp was a fixed 0.75 pixel, while a step can jump much further in depth, so the decision was a coin flip between neighbouring pixels; and grass and foliage were marched like solid surfaces. Now the depth is read at pixel centres, the hemisphere follows the cascades' smooth normal, the ramp covers the depth the ray itself crosses in one step, and thin features (found on three scales, 1, 2 and 4 pixels, and by the geometry pass) take no contact shadow at all.



For SDR output, **Convert Tonemapper** undoes the game's tonemapper, applies the output curve you pick, and adds the GI on top. Nothing is detected, you choose the matching *Input Tonemapper* yourself. For HDR output (scRGB or HDR10 PQ) the image is decoded to light using *HDR Paper White* and *Peak Brightness*, the GI is added, and the result is encoded again (tonemapper conversion and dither are skipped).

### Preprocessor definitions

Changing any of these reloads the effect.

| Name | Default | What it is (and what it costs) |
|---|---|---|
| `CRCGI_RESOLUTION_DIVISOR` | 4 | effect resolution, 1 full, 2 half, 3 third, 4 quarter (5 and 6 also work). This is the main cost lever, 3 is sharper and a lot more expensive |
| `CRCGI_BASE_TILE` | 2 | directions per probe axis in cascade 1, 2 (4 directions) or 4 (16), 4 multiplies the ray count |
| `CRCGI_CASCADES` | 5 | 4, 5 or 6, more cascades means more reach, more rays and more memory |
| `CRCGI_CANVAS_MARGIN` | 24 | off-screen canvas border in percent of the screen per side, 0 removes the canvas passes completely |
| `CRCGI_SECONDARY_DIVISOR` | 1 | the multibounce map, 1 is the effect resolution, 2, 3 and so on are lower, **0 is the exact per-hit multibounce read** (no map, slower) |
| `CRCGI_CONTACT_DIVISOR` | 0 | resolution of the Shadow's contact march. **0 follows `CRCGI_RESOLUTION_DIVISOR`** (the effect resolution), 1 is full resolution (one probe per pixel, sharpest and by far the most expensive), 2 to 6 set it on their own |
| `CRCGI_CONTACT_BLUR` | 1 | the 2-pass blur of the Shadow's contact march, 0 removes it (the resolve then averages 3 x 3 contact texels itself) |
| `CRCGI_CONTACT_BLUR_STEP` | 2 | screen pixels between the 9 taps of each contact blur pass, at least one contact texel (at the default contact resolution the taps are 1 texel = 4 pixels apart) |

`BUFFER_COLOR_SPACE` is set by ReShade itself (1 sRGB, 2 scRGB, 3 HDR10 PQ, 4 HLG) and the shader reads it for *Game Output Signal: Auto*.

## The add-on (CRCGI Injector)

### What it does and the levels

It does three things, and each one can be switched on or off while the game is running:

1. **Feeds.** It copies the game's own render targets (G-buffer normals, albedo, motion vectors, HDR scene light, and the **finished frame**) and hands them to the shader as textures. It also reads the camera's vertical fov from the game's projection matrix and writes it to the shader.
2. **The finished frame (ignoring ui and fog).** You point the add-on at the last layer that holds the finished 3D picture (right before the ui, or before fog, whichever you want left alone). It copies that picture and the shader works on the copy, then adds its light to the final image, so everything the game draws after that point stays exactly as the game drew it. The effect still runs at the end of the frame like any ReShade effect, nothing is injected into the game (that is the point, it's the safe way).
3. **Driving the shader.** It can set the shader's own switches (`Use Game Normals`, `Use Game Albedo` and so on) to match what it's delivering (the checkbox *Drive the shader's switch* in the G-Buffer tab, **off by default**, so the shader's checkboxes are yours), and it handles Geometry Source.

There's also an advanced fourth thing, **Force render** (Options), that renders the effect inside the game's frame right before a layer you pick (more on it below). It's off by default and you only want it when the default can't do what you need.

Levels (Numpad 7 / 9 or the Setup tab, Numpad 0 turns the whole add-on off). The add-on starts on **level A**:

| Level | What changes |
|---|---|
| A | nothing, the effect runs at the end of the frame like any ReShade effect |
| B | A plus the camera's fov (and, if Force render is on, the effect rendered right before the layer you picked, with the depth buffer in use there) |
| C | B plus the feeds (normals, albedo, motion vectors, HDR scene light, finished frame). They're captured while the game draws and bound before ReShade's end-of-frame render |

Assigning a feed (Use as in the Profiler, or a role in the Layers tab) raises the level to C by itself.

### What works on which API

| | D3D10 / 11 | D3D12 | Vulkan (incl. D3D9-11 through DXVK) | OpenGL |
|---|---|---|---|---|
| Feeds handed to the shader (level C) | yes | yes (captured at the end of a binding, or right after a render pass ended) | yes (captured at the game's barriers) | yes |
| Finished frame | yes | yes | yes | yes |
| Profiler, layer list with kinds | yes | yes | yes (no live layer preview) | yes |
| Compute-written targets (UAV) | yes (the bound views are seen) | no (descriptor heaps hide them, the texture is listed but can't be captured safely) | no | no |
| Force render (inside the frame) | yes (immediate context only) | **experimental**, it has its own state tracker and can be switched off | no | no |

Native DirectX 9 isn't supported (CRCGI needs Direct3D 10 or newer), DX9 games run through DXVK, which ReShade sees as Vulkan.

### First run

1. Enable **CRCGI v2** in ReShade, open **Add-ons > CRCGI Injector**. The effect already works at this point (level A, end of the frame, ui included).
2. Optional, to use what the game knows. With the **3D scene on screen** (not a menu), open the **Profiler** and press **Take snapshot**. The add-on copies the game's candidate textures, analyzes them and **suggests** one per feed (normals, albedo, motion vectors, HDR scene light, finished frame) with a score bar, nothing is assigned for you (there's no auto-setup, every choice is yours). Press **Use as** under the texture you want for each feed, and check the thumbnail first (the thumbnail is exactly what the shader will receive).
3. To leave the ui and fog alone: in the **Layers** tab pick the last layer that shows the finished 3D picture without the ui (click a row to preview it, **double-click** it or press its role combo and choose **Finished frame**). Then turn on *Use Game Finished Frame If Provided* in the shader (Game Integration).
4. Check the other feeds in the shader, under *Debug: G-Buffer & Canvas* (the *G-Buffer / Canvas View* combo):
   - **Normal Difference: Game vs Depth**, mostly black is good (the game normals agree with the ones rebuilt from depth), white is 90 degrees apart.
   - **Game Albedo (input)**, **Game Motion Vectors (input)** and **Game HDR Scene Light (input)** should each look like what their name says.
   - **Game Normal Auto-Align Status** if the game's normals are in world space (assigning normals from the Profiler sets *Game Normal Space* to world by itself when the analysis says so).
5. Once a feed looks right, turn its switch on in the shader (*Use Game Normals If Provided* and so on, in Game Integration), or tick **Drive the shader's switch** for that feed in the add-on's G-Buffer tab and let the add-on do it (it then keeps the switch on while the feed is delivered, see *Driven switches don't flicker* below).
6. For games with DLSS, FSR or XeSS leave **Upscaler** on Auto (see below).
7. If a feed looks wrong, go to the **G-Buffer** tab and check the decode options (normal encoding and space, motion units), or pick a different texture in the **Profiler** or **Layers** tab.

After a loading screen the game recreates its textures and the add-on finds them again by their description and creation order (a feed whose texture isn't found yet shows as *not created by the game right now* until it is).

### The tabs

The overlay is laid out vertically (cards and stacked bars instead of wide tables), so it stays readable at about a third of a 1440p screen's width.

| Tab | What it's for |
|---|---|
| **Setup** | quick start, the upscaler, and a checklist (technique, camera, depth, each feed) that tells you the reason when something is missing. Also the camera picker and the shader link |
| **G-Buffer** | one collapsible section per feed (normals, albedo, motion, material, HDR scene light, finished frame) with a status badge, where it comes from, a thumbnail of exactly what the shader receives, why it's not used if it isn't, and its decode options. Also *Geometry Source* and a "shader work saved" list that shows what the shader is skipping right now |
| **Layers** | the recorded layer catalog as a list of cards (id, target, kind, draws, note, role), you can sort, filter, preview and assign roles. By default only layers the game draws **every frame** are listed |
| **Profiler** | the game's render-target textures, found from their creation calls (so it works on D3D12 and Vulkan, where descriptor heaps hide the bindings), with a thumbnail, usage, a score bar for each feed type it could be (the best candidate per feed is marked), and "use as" buttons |
| **Keys** | every action is remappable (with Ctrl, Shift, Alt), hold delay and repeat rate are adjustable, and there's a Reset all |
| **Debug** | what the shader receives, capture counters, the last check of the D3D12 tracker, and a verbose log switch |
| **Options** | sharing the target listing (below), feeds from other command lists, **Force render (advanced)**, a note about performance, and the Build line (add-on version and ReShade revision) |

### Layers, kinds and roles

A layer is a render target that gets written during the frame, listed in the order it's drawn. The catalog is **recorded over about 3 seconds and then kept**, so the list stays stable, and a layer that hasn't been drawn for 10 seconds (and has no role) is dropped. Each row has a **kind**, detected from how it's drawn (blend and depth-write state are recorded per pipeline):

| Kind | Detected when (from the code) |
|---|---|
| G-buffer | 3 or more targets, depth bound, at least 8 draws, mostly not blended |
| UI / overlay | drawn into the back buffer, or late in the frame, mostly blended, no depth, same size and format as the back buffer, at least 3 draws |
| Lighting | no depth, few draws and a floating-point target early in the frame, or depth, floating-point, under 40 draws and no depth writes |
| Post-process | no depth, at most 4 draws, 2 or fewer targets (and not an early floating-point one) |
| Transparent | depth bound and mostly blended draws |
| Opaque (geometry) | depth bound, otherwise |

**Only layers that are always rendered.** The list hides a layer unless it was drawn in at least 95% of the frames since it was first seen (and for at least a second of frames, and it's still drawn now). Menus, cut-scenes, weather and effects the camera only sometimes sees would otherwise fill the list with rows that are grayed out most of the time. Layers with a role are always shown, and the checkbox *Only layers drawn every frame* (Layers tab) turns the filter off. Draws that draw nothing (no vertices or no instances) don't open a layer, and a clear of a bound target ends the layer it was part of.

These are heuristics, not facts, so **check the preview**. Click a row to preview it, **double-click to use it as the finished frame**. The role combo is on every card (*Force render here*, *Depth*, *Normals*, *Albedo*, *Motion vectors*, *HDR scene light*, *Finished frame*). Sorting can be by frame order, resolution then frame order, kind then frame order, most draws, format then frame order, or assigned roles first (ties always keep frame order), and there's a text filter.

**Choosing the finished frame.** It has to be a single-sampled target of the back buffer's size and format family (it's the picture the shader reads instead of the final image, so a different format would be decoded wrongly). Pick the last layer **before** whatever you want left alone: before the ui to leave the hud untouched, before fog or particles if you want those untouched too (the GI then also ignores what they add to the picture). Where the final image still shows the finished frame the effect's light is added to it, where it doesn't (hud pixels, opaque overlays) the pixel stays as the game drew it, and soft changes made after the copy (fog) keep most of the effect. It's a copy plus a few extra texture reads, not an injection.

### The feeds

| Feed | Semantic | What the shader does with it | Work it removes |
|---|---|---|---|
| Normals | `NORMAL` | geometry normals (world space gets aligned to the camera every frame) | the normal from the depth plane fit |
| Albedo | `CRCGI_ALBEDO` | multibounce reflectance, relit pickup, Surface Albedo Response | the 16-tap reflectance estimate per texel and the albedo-response taps |
| Motion vectors | `CRCGI_MOTION` | history reprojection, canvas, multibounce | the 4 block-matching passes |
| Material | `CRCGI_MATERIAL` | the diffuse share of each surface (metalness or specular, from the channel you pick): metals receive and hand on less GI, as in the game's own lighting | none (it makes the GI more accurate on metal) |
| HDR scene light | `CRCGI_HDRSCENE` | light pickup from the lighting buffer (no ui, bloom or grain, full range), exposure matched to the image every frame | none |
| Finished frame | `CRCGI_SCENE` | the picture the effect works on (light pickup, luminance, composite base) while the final image is only used to keep what was drawn later | none |

Other effects can read the same textures by declaring `texture2D t : CRCGI_ALBEDO;` and so on.

**Decode options (G-Buffer tab).** Material has a channel (R, G, B or A) and what it holds (metalness, specular, roughness or smoothness; roughness and smoothness have no effect on diffuse GI). Normals have an encoding (RGB stored 0 to 1 and decoded as *2-1, octahedral 2-channel, or RGB signed as stored) and a space (view, or world / anything else, which gets auto-aligned). Motion has units (UV, pixels, NDC), flip Y and invert direction. A motion target with four channels (velocity in two of them, depth or a mask in the others) is recognized by the profiler too, and the shader reads the first two.

### How the data is captured (and why it doesn't flash)

- **Hooks.** The add-on listens to the same things a shader toggler does: every draw, indexed draw, indirect draw and compute dispatch, pipeline binds (the shader hashes), render target binds, render passes, clears and barriers. GPU-driven games (indirect draws) and compute passes therefore count.
- **Game data is identified by the frame's structure, not by addresses.** A layer's **layout** is how many targets it binds, their formats, their size relative to the screen and whether a depth buffer is bound. It's the same every frame, it doesn't change when the camera turns, when the game swaps or recreates its textures (a texture whose address changes and changes back is still the same target), and it's the same for a player at another resolution. Normals, albedo, motion and material assigned from a layer follow that layout and the target's slot in it, whichever texture is bound there this frame.
- **Write runs, copied once they're complete.** A G-buffer is often drawn in several binding intervals (the game rebinds the same targets between draws, changes state, splits static and dynamic geometry, and the camera decides what gets drawn first). Consecutive intervals with the feed's layout that draw into the same texture are one **run**, and the texture is copied once, when the run ends: the next time it's drawn to in another layout, unbound, cleared, read by a compute pass, moved on by a barrier, or when the command list is closed. The copy therefore holds everything the run drew, whatever the camera sees. A later run in the same frame only replaces it if it continues it (same texture, not cleared in between) or draws more than everything captured so far, so a pass that reuses the same memory later in the frame for something unrelated never overwrites the G-buffer copy (that's what made a feed flash on and off).
- **The HDR scene light and the finished frame are interval-exact.** The finished frame is the first capture of the frame at its layer (the ui is often drawn into the same target right after it), the HDR scene light the interval that draws most into it, so fog or particles drawn afterwards into the same target don't end up in it.
- **Feeds picked as a texture in the profiler** are copied the same way, at runs with the texture's main layout (the layout of the binding that drew most to it). The main layout only changes if the old one is gone for half a second or the new one draws more than twice as much.
- **Copies are only taken where the texture's state is known**, at the end of a binding interval in which it was a render target, or, on D3D12 inside a render pass, right after the pass ended (at the next event of the command list, since nothing may be copied inside one). On Vulkan, where nothing can be copied inside a render pass at all, it's right after the game's own barrier out of that state, once the texture's main binding has ended. **Direct3D 12 never copies at a barrier** (the event can't tell the first half of a split barrier from a full transition, and a copy between the halves is invalid). A barrier is never placed on a guessed state.
- **Compute (D3D10/11).** The unordered access views the game binds before a dispatch are seen, so a target written by compute (motion vectors, lighting, a ui composite) can be a feed too: it's copied right after the first dispatch that writes it in the frame. On D3D12 and Vulkan the views live in descriptor heaps and aren't visible, the texture is still listed in the profiler but can't be captured safely.
- **Layer identity (the list).** A layer's key is its targets (format, size, creation order) plus the occurrence of that signature within the frame's recording of one command list, so it doesn't depend on how command lists are interleaved on other threads. The shaders aren't part of it (the first draw of a G-buffer or lighting pass depends on what the camera sees, so a key with the shader in it changed whenever the view did). The occurrence counter restarts every frame on every API, so a command list that's reused or recorded in a different order doesn't make a layer's identity drift.
- **The end of a command list counts.** When the game closes a command list (D3D12, D3D11 deferred contexts) its last binding interval and any open run end there, so a G-buffer pass that's the last thing on its list is captured too.
- **Threads.** The frame number is atomic, so command lists recorded on worker threads always compare against the current frame.
- At the end of the frame a feed is used if its latest capture is recent enough: albedo and material up to 8 frames old (surface properties, a short gap in the capture is bridged instead of switching the game data off and on), normals and the HDR scene light 3, motion vectors 1, the **finished frame** only if it was captured in this very frame (an old copy would show a stale picture and the effect falls back to the final image). At a forced-render point every feed must be from this frame.
- **Driven switches don't flicker.** With *Drive the shader's switch* the add-on sets the switch right where it binds the texture, so the switch is never on while its texture is unbound. A switch that's off comes on after 3 frames in a row with the feed delivered and goes off in the first frame without it, so a feed that comes and goes can't make the picture jump between the game's data and the shader's own.
- Textures are identified across loads and restarts by their description and creation slot. A recreated texture takes over its assignment.
- Albedo written through an sRGB view is sampled through an sRGB view, normals and motion are always linear.
- Multisampled targets: albedo, HDR scene light and the finished frame are resolved, normals and motion are reported as unsupported.
- *Accept feeds recorded on another command list* (Options, on by default), since D3D12 games record the G-buffer and the lighting on different command lists.

### What the profiler suggests (and how it scores)

The profiler reads 16 small tiles of each snapshot and measures them, then scores every feed type from 0 to 1 (0.6 and above is a suggestion, and nothing is ever assigned for you).

| Feed | What the score looks for |
|---|---|
| Normals | unit-length vectors in most pixels (three channels, with variety), or two channels spread over the square (octahedral) |
| Albedo | three or more channels in 0..1, not unit vectors, not HDR, plausible brightness. sRGB storage helps, but plain 8-bit (4 bytes per pixel) or R11G11B10 colour clears the bar too, a write together with a depth buffer in a many-target pass adds a bit |
| Motion vectors | two channels that are mostly tiny (UV sized), or four channels where the first two are tiny and it's float or signed (velocity next to depth or a mask) |
| HDR scene light | a floating-point colour target. Values above 1 raise the score but aren't required (dark rooms and overcast scenes stay inside 0..1), a target written alone with many draws and at screen size scores higher |
| Finished frame | a screen-sized colour target in the back buffer's format family, no depth, written alone |

### Share the target listing (Options)

What the add-on knows about one game (which texture is the normals, which layer is the finished frame, the upscaler setting, your notes) can be exported and shared next to a preset, if the preset's author wants to.

- **Export the target listing** writes `crcgi_targets_<game>.ini` next to the game's exe. It has the feeds (as textures: format, usage, creation order, and the **size relative to the screen**, so it's found again at another resolution), the level, the geometry source and upscaler settings, the camera choice and the author and notes you typed. It does **not** have your keys, your overlay options, your technique choice or a force-render layer (those are yours).
- **Import** (same place) lists every `crcgi_targets*.ini` next to the exe. Copy a listing there, press Import, and the add-on matches the textures as the game creates them (it also looks again every second or so until the missing ones appear). Normals, albedo, motion and material assigned from a layer are saved as the layer's **layout** (screen-relative sizes), so they're found again at another resolution too. The HDR scene light and the finished frame assigned from a layer are tied to that exact layer, whose key includes its size, so for those assign from the Profiler (Use as) if you want to share the listing. A listing or settings file written by an older add-on keeps its texture feeds, but feeds it assigned from a layer have to be assigned again once (the add-on writes that to the ReShade log).
- A listing is only text, open it, it's short. The same file format is what the add-on saves for itself in `crcgi_injector.ini` (that one also has the keys and the overlay options).

### How Force render works (and why the effect isn't rendered twice)

This is the old way of doing it and it's still there (Options > Force render (advanced), plus the layer role *Force render here*): at the chosen layer the add-on calls ReShade's `render_technique` for each ticked technique that's also enabled in ReShade. During ReShade's own end-of-frame render the managed techniques are switched off and switched back on right after, and only in frames where the forced render actually ran, your switches and your preset are never changed. It changes the game's command stream, so it's the one place where a game can misbehave, which is why it's not the default.

- **D3D10/11.** ReShade restores the game's state itself, and only the immediate context is used.
- **D3D12 (experimental).** The add-on records and restores everything the game had bound (pipeline state, root signatures for graphics and compute, every root parameter, descriptor heaps, topology, blend factor, stencil reference, viewports, scissors and render targets). A layer whose state is incomplete, or that's drawn inside a render pass, is refused and the reason is shown. If the game crashes when a layer is chosen, switch off **Options > Force render** (or **Direct3D 12: allow it**).
- **Vulkan / OpenGL.** End of frame only, since restoring a Vulkan game's state (and DXVK's cached copy of it) after an effect can't be done safely from an add-on.
- **ReShade Effect Shader Toggler.** While it's loaded Force render stands down (feeds and the profiler still work).

### Default keys (numeric keypad, they work with the overlay closed)

| Key | Action |
|---|---|
| Numpad 0 | integration on / off |
| Numpad 7 / 9 | level down / up |
| Numpad 8 / 2 | previous / next layer (hold to repeat, the list follows) |
| Numpad 5 | selected layer = finished frame |
| Numpad 4 / 6 | previous / next role of the selected layer |
| Numpad . | record layers |

Keys pause while you're typing in the layer filter or while the Keys tab is waiting for a key.

### Camera, depth and upscalers

- **Camera.** The add-on scans the game's constant buffer writes for a perspective projection matrix and gets the vertical fov, the near and far plane and the depth direction from it, and writes the fov to the shader's *Vertical Field of View* while it has the camera. It's a heuristic, so if no camera is found set the fov by hand (a wrong fov shows up as speckled or striped normals in *Debug View > Normals*).
- **Depth.** With Force render on, *Hand the depth buffer in use at the layer to the shader* (Options) shows the shader the depth buffer that's bound at that point (all the usual depth formats, including D24S8 and D32S8, are mapped to a readable view). Without it, the shader uses the depth ReShade provides.
- **Upscalers (DLSS / FSR / XeSS).** With the **Upscaler** option on Auto, the add-on finds the render resolution from the most-drawn smaller target. Render-resolution textures then count as screen-sized in the layer list and the profiler, and can be used as feeds. Presets are Quality 66.7%, Balanced 58%, Performance 50%, Ultra Performance 33.3%, or a custom percentage.

### Logging (and what to send when something goes wrong)

Everything goes to `ReShade.log` on lines that start with `[CRCGI Injector]` (the load line with 64/32-bit and ReShade revision, the device API, every refused forced render or feed with its reason). If you report a problem, send `ReShade.log` and `crcgi_injector.ini`, a screenshot of the Setup checklist and the G-Buffer tab, what you did right before, whether it's DX11 or DX12 and which other add-ons are installed, and for a crash the `exception_*.dmp` that ReShade wrote next to the exe.

### Limits

- The finished frame (and a forced-render layer) has to match the back buffer's size and format family, HDR scene targets on an SDR back buffer can't be used for it.
- Force render is D3D10/11 only (D3D12 is experimental), Vulkan and OpenGL run at the end of the frame.
- A per-pixel transparency mask isn't something a game keeps in a texture, so there's no transparency input (the finished frame is the way to leave fog and glass alone).
- Normals and motion feeds on multisampled targets are unsupported.
- Targets written by compute are only visible on D3D10/11.
- Layer identity is a hash of the target description and its position, so it can change after a game update (layers assigned by role then need picking again, textures are matched by description and survive more).

## Tuning guide

### Ground rules

- Change one thing at a time, and look at it in a debug view before you judge it in the final image (*Debug View* and *G-Buffer / Canvas View* are two separate combos, and both live in the Debug section, which is open by default together with Setup).
- Everything is resolution independent, sizes are in effect texels (1 texel is `CRCGI_RESOLUTION_DIVISOR` screen pixels) or screen fractions, so a preset looks the same at every screen resolution.
- Stay out of the **Expert** sections until a symptom sends you there, Setup, Brightness, Game Integration, HDR, Multibounce, Sky, Canvas and Temporal cover almost everything.
- **GI Intensity is the master level**, everything else shapes the light and it scales it, so use a temporary low level for hit detection and set the final level after the geometry and light pickup are reliable.
- Save a ReShade preset before each stage (ReShade stores settings per file name).
- The values the shader loads with are a starting point, not a recommendation, move them to taste.

### Where the guide starts (the defaults, and the zero point)

The values baked into the shader are the ones in `presets/DEFAULT.ini` (so loading that preset and resetting the shader do the same thing). They're a working look, not a blank slate: every strength, intensity and bias sits at **1** where 1 is its natural "full" value (GI Intensity, Energy Carry, Multibounce Strength, Canvas Light Strength, Facing Check Intensity, Merge Visibility Check, Emitter Boost, Shadow Amount, Shadow Contact Depth Bias, Bounce Saturation, Screen Edge Extension, Dark Surfaces Block Rays, Foreground-to-Background Block) and at **half its range** where 1 would be too much (Shadow Contact Bias, Blue and Orange Light Filter, Surface Albedo Response, Fade With Receiver Distance and Facing Check Bias at 0.5, Temperature Strength 0.15, Denoise Blur Strength 4). Every jitter sits at **0.4** (40% of its range), the optional features are **off** (Convert Tonemapper, Sky Light, HDR Light Pickup, Relit Pickup, Dark Area Boost, the game-data switches), the light pickup is plain SDR, and the caps, limits, thresholds and tolerances are tuned values that work in most games. Shadow Contact Bias at 0.5 overlaps the escape and contact shadows (1 would drop the denoised escape shadows completely, 0 skips the contact passes).

The tables still read upward from each slider's **zero point**, because that's the clearest way to see what a slider does. Every row below says what you get when you **raise a slider from 0 (or from its lowest value, or from off)**, and what it looks like when that's too much. Nothing in the shader is hidden behind something else at the start, and when two sliders do overlap each other the guide says so (the section *Which sliders overlap* near the end of the guide collects all of them, with the reason each overlap is useful).

**How to read the tables.** Every slider in the shader is in one of the tables below, in the order you should reach it. The columns are the slider, where it **starts** (its zero point), its **default** (what the shader loads with, the same as `presets/DEFAULT.ini`), what you get when you **raise it** from the zero point, and what it looks like when it's **too much** (or too little). The Start column uses a few short words:

- a number (like 0) is the value to set in the blank state, and `off` is the same for a checkbox
- **min** or a number above 0 means the slider can't go to 0 (its lowest value is not 0), so it starts at its lowest value (for example Frames Accumulated starts at 1, which means no accumulation)
- **1 (neutral)** is for sliders where 1 is the "does nothing" value (Output Contrast, Output Saturation, Bounce Contrast Curve)
- **match the game** is for sliders that can't be zero and have to describe the game itself (field of view, sky depth, the game's tonemapper, HDR paper white and peak)
- **keep** means leave it at the default until a symptom sends you there, these are mostly the Expert sliders and the sliders that only shape something another slider switched on

Where Start and Default are the same you're already at the zero point. Where they differ (Temporal Accumulation, Multibounce, a few others), either set the slider to its Start value to watch it come in, or leave it where it is and read the Raise column as "what the slider does when you go up from its zero".

**The blank state.** With **GI Intensity** at 0, **Shadow Amount** at 0 and **Convert Tonemapper** off, the picture has to be exactly the game's picture, if it isn't, something in the shader is still touching the image and you should find that before going on (the usual cause is Convert Tonemapper being on). Leave the preprocessor definitions (`CRCGI_RESOLUTION_DIVISOR` and the others) at their defaults for the whole guide, change them last.

Checks you can use through the whole guide (they're in the *Debug View* combo and the *G-Buffer / Canvas View* combo): **GI Only** (with Debug Gain) shows the light by itself even while GI Intensity is 0, **Normals** shows whether the geometry is right, and **Emitters** shows what the shader samples light from.

#### Stage 1, inputs (field of view, sky depth, the game's own data)

Everything after this depends on the shader seeing the scene correctly, so do this first. The first two sliders can't be 0, they describe the game. The rest of the table is the game integration switches, those all start off and you turn them on one at a time **only when the add-on delivers that input and its debug view looks right** (without the add-on leave all of them off, the shader estimates everything itself).

What to look at: *Debug View > Normals* (a flat wall must be one even color), *Debug View > Linear Depth* (the sky must be separate from the terrain), and in *G-Buffer / Canvas View* the Normal Difference, Game Albedo, Game Motion Vectors and Game HDR Scene Light views.

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Vertical Field of View | match the game | 60 | Set the game's vertical FOV (convert it if you only know the horizontal one). Check *Debug View > Normals*, a flat wall must be one even color. The add-on writes it for you while it has the camera. | A wrong value shows up as speckled or striped normals and light that slides over surfaces. |
| Sky Rejection Depth | match the game | 0.97 | The linear depth where the sky starts (look at *Debug View > Linear Depth*). Pixels at or beyond it get no GI, cast no bounce, and rays pass through them. Depth scales differ a lot between games (see *Depth scales are not the same in every game*), so find the value in your game instead of copying one. | Too low and distant terrain is treated as sky (no GI), too high and the sky edge gets lit. Long draw distances need it just above the horizon (Ctrl+click to type 0.9995). |
| Use Game Normals If Provided | off | off | Turn it on once the add-on delivers normals and *Normal Difference* (Debug: G-Buffer & Canvas) is mostly black. The add-on only switches it for you if *Drive the shader's switch* is ticked for normals, otherwise it's your checkbox. | If the difference view is bright the encoding or space is wrong, leave it off until that's fixed. |
| Game Normal Space | View space | World or any other space (auto-aligned to the camera) | Use *World or any other space* if the game's normals aren't in view space, every frame it fits the rotation that maps them onto the depth normals (about 9000 samples). | Needs a view with surfaces facing at least two different ways, otherwise the last good fit is kept. |
| Normal Auto-Align Smoothing | 0 | 0.25 | Raise it if the fitted rotation shimmers (World space only). 0 fits fresh every frame and follows fast turns exactly. | Too high and the fit lags behind fast camera turns. |
| Geometry Source (with game normals) | Rebuild from depth everywhere | Game normals where valid, rebuild elsewhere | Compare first, then switch to *Game normals where valid* and finally *Game normals only* (fastest, no depth rebuild at all) once *Normal Difference* is clean. | With *Game normals only*, pixels without a game normal just face the camera (sky and holes look flat). |
| Use Game Albedo If Provided | off | off | Turn it on once *Game Albedo (input)* looks right. Multibounce, the relit pickup and Surface Albedo Response then use the real albedo and the 16-tap estimate is skipped (Relit Albedo Detail, Relit Detail Radius and Bounce Reflectance (estimate) stop doing anything where the albedo is valid). | If the albedo view looks wrong (dark or wrong colors), leave it off. |
| Use Game Motion Vectors If Provided | off | off | Turn it on once *Game Motion Vectors (input)* looks right. The four block-matching passes stop doing work and the history follows the exact motion (Motion Mismatch Rejection stops doing anything). | Light trails behind moving objects usually mean wrong units, Flip Y or Invert direction (G-Buffer tab of the add-on). |
| Use Game Material (reflectivity) If Provided | off | off | Turn it on when the add-on delivers the game's material texture, metals then receive and hand on less GI. | If *Game Material (input)* is not dark on metal and white elsewhere, the channel or its meaning in the add-on is wrong. |
| Use Game HDR Scene Light If Provided | off | off | Turn it on when the add-on delivers the game's lighting buffer, rays then pick up light without UI, bloom or grain and with the full brightness range. | If *Game HDR Scene Light (input)* is black the match failed, leave it off. |
| Use Game Finished Frame If Provided | off | off | Turn it on once the add-on delivers the finished frame (the last layer before the ui or fog, picked in its Layers tab or Profiler). The effect then works on that picture and adds its light to the final image only where the final image still shows it, so ui and fog stay as the game drew them. | If the add-on's Finished frame thumbnail shows the hud or is black, leave it off and pick another layer. |

Set a temporary, low **GI Intensity** while tuning this stage, and use **GI Only**, **Emitters**, **Linear Depth** and **Object Thickness** to distinguish missed hits from blocked rays. Start with the default hit model, check walls, floors, foliage and thin objects, then change only the controls that address visible misses, halos or light passing through geometry. Return to the final brightness stage after the geometry is reliable.

#### Stage 2, hit detection (establish the lighting baseline)

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Hit Thickness Mode | keep | Generic thickness (original) | How the hit band estimates the object's own thickness. **Generic** gives every surface the same thickness (Object Thickness Cap). **Silhouette-derived** measures each object's thickness from the depth buffer (it looks for the object's two opposite edges), so thin things (poles, railings, foliage) get a thin band and big things a thick one, and it falls back to the generic value when it can't find both edges. *Debug View > Object Thickness* shows the measurement (red = thin), in either mode. | Silhouette-derived adds depth-buffer reads. In Generic mode Thickness Search Range, Thickness Silhouette Gap and Object Thickness Scale do nothing. Only the fine cascades use the measurement, the farthest cascades use the depth pyramid. |
| Thickness Search Range | keep | 3 | Silhouette mode only. 1 looks 1 pixel to each side (in four directions), every step doubles it up to 32 pixels at 6. Raising it finds wider objects (they get thicker bands). | Too short and thick objects aren't measured at all (they get Object Thickness Cap). Longer costs more depth reads. |
| Thickness Silhouette Gap | keep | 0.07 | Silhouette mode only. How far behind the object (relative to its depth and surface plane) a pixel must be to count as the object's edge. At the lowest value subtle edges count, raising it keeps only clear depth jumps. | Too low mistakes steep slopes and depth noise for silhouettes (thickness jumps around), too high misses edges (falls back to Object Thickness Cap). |
| Object Thickness Scale | keep | 0.43 | Silhouette mode only. Multiplies the measured thickness. Below 1 rays pass behind things sooner, above 1 thin objects get a wider band (rays hit them more easily). | Limited by Object Thickness Cap: once the measured thickness times this reaches the cap, raising it changes nothing. |
| Object Thickness Cap | keep | 0.015 | The thickness a surface is given, as a fraction of its own depth. Generic mode: every surface gets exactly this. Silhouette mode: the upper limit of the measured thickness, and the fallback when no edges are found. Raise it from 0.005 and rays stop passing through thin objects. | Too high and rays hit surfaces from far behind them (dark halos). At or above Max Hit Thickness it changes nothing (that one wins). |
| Hit Front Tolerance | keep | 0.14 | How far in front of a surface (as a fraction of its hit band) a ray may stop and still hit it. 0 = the ray must be inside the band. Raise it if rays miss grazing surfaces (floors at a low angle) because they stop just short. | Too high and rays hit surfaces they're well in front of (halos in front of objects). |
| Max Hit Thickness (fraction of depth) | keep | 0.35 | The ceiling for every hit band (object thickness plus the ray's step plus the slope), as a fraction of the surface's own depth. It matters for the long steps of the far cascades and for things close to the camera. Lower = stricter, 1 = off. | Too high makes the far cascades splotchy and darkens everything around the player (rays hit the character or grass beside the camera from far behind). If Object Thickness Cap plus the ray step is already below it, it changes nothing. |
| Occlusion Depth Threshold | keep | 0.015 | Decides whether a ray behind a nearer surface is blocked (a shadow) or just passes behind it. From 0 every ray behind a surface is blocked (strongest contact shadows, thin things block too), raising it lets rays pass behind thin or distant objects (less shadow, light from behind objects arrives). It's linear depth, so it depends on the game's depth scale. | If it's smaller than the hit band the band decides and it changes nothing. Occlusion Tolerance and Occluder Depth Range decide which surfaces may block at all. |
| Facing Check Intensity | keep | 1 | 0 = never dims back-facing hits. Raise it and the back of a lit surface bounces less light (1 = fully respected). Very thin objects fade the check out because their normals are unreliable. | Facing Check Steepness and Facing Check Bias do nothing at 0. |
| Facing Check Steepness | keep | 0.87 | Facing weight = dot(hit normal, -ray) x this + Facing Check Bias, limited to 0-1. Higher = a sharper switch between front-facing and back-facing hits. |  |
| Facing Check Bias | keep | 0.5 | How much light a surface seen exactly edge-on still gives (the Bias term of the facing weight). Raise it so grazing hits still bounce, lower it to cut them. |  |
| Ray Depth Model | keep | Perspective-correct | Perspective-correct uses the real depth of the 3D ray from the field of view, Slope heuristic is a linear guess (set by Ray Depth Slope). Ray Depth Drift Limit caps both. | Perspective needs a correct field of view. Perspective Ray Lift only works with Perspective, Ray Depth Slope only with the Slope heuristic. |
| Perspective Ray Lift (texels) | keep | 1.4 | Perspective model only: how far rays start off the surface along its normal (texels). 0 = from the surface itself, raising it avoids rays hitting their own surface on curved or noisy geometry (self-shadow speckle). | Too high loses contact bounce. |
| Ray Depth Slope (per texel) | keep | 0.003 | Slope model only: how fast a ray's expected depth changes per texel. Higher = rays leave the surface plane sooner. |  |
| Ray Depth Drift Limit | keep | 0.25 | Caps how far a ray's expected depth may drift from the surface it started on, so long rays can't leave the depth range of everything on screen. Both ray depth models use it, and it's an absolute linear depth (see Relative Depth Tolerances). |  |


#### Stage 3, color (HDR and the tonemapper)

Two different paths here, pick the one that matches your output. **SDR** uses the Tonemapper Conversion section (undo the game's tonemapper, apply another one, add the GI). **HDR** (scRGB or HDR10) uses the HDR & Color Range section and skips the tonemapper conversion and the dither completely. In SDR you can also leave Convert Tonemapper off and the GI is just added to the untouched image, if that looks fine to you, you can skip the Tonemapper Conversion rows (the HDR Light Pickup sliders still apply in SDR, they use the Input Tonemapper even when the conversion is off).

The key test for the SDR path: turn Convert Tonemapper on, set **Output Tonemapper** equal to the **Input Tonemapper**, and the picture should come back unchanged (apart from a clipped highlight). If it doesn't, the input curve is wrong. Only after that change the output curve and the taste sliders. The curve tunables at the bottom of the table only matter for the curve you picked as the **output** (the input curve always uses its published settings).

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Game Output Signal | Auto | Auto (from the game's output format) | Auto reads the format from ReShade, set it by hand only if the detection is wrong (SDR games: SDR (sRGB), HDR games: scRGB or HDR10). | A wrong signal decodes the image wrongly (washed out or blown out). HDR skips the tonemapper conversion and the dither. |
| HDR Paper White (nits) | match the game | 80 | Match the game's paper white or UI brightness setting (HDR only). | Everything the shader measures treats this as 1.0, so a wrong value shifts every threshold. |
| HDR Peak Brightness (nits) | match the game | 809 | Match the game's peak brightness setting (HDR only). | It decides how far Highlight Rolloff lets added light go, too low clips the added light early. |
| HDR Light Pickup | 0 | 0 | Lets lamps, fire and sunlit spots emit more than a white wall (an SDR image squeezes them into the top of 0 to 1). Mid gray and darker stay as they are. | Too much gives harsh, very directional light and fireflies from small bright spots. |
| HDR Pickup Range | 1 | 4 | The most a source may be brightened (x this). The default 4 is a good target once HDR Light Pickup is up. | Higher means stronger lamps and more firefly risk. |
| Convert Tonemapper | off | off | Turn it on when you want a different curve (SDR only). Off adds the GI to the untouched image. | With the wrong input curve the whole picture changes color and brightness. |
| Input Tonemapper (the game's) | match the game | AgX (Witcher 3 Remastered) | Pick the curve the game used (ACES filmic or ACES 2.0 for most modern games, AgX for Witcher 3 Remastered, Hable for many last-generation games). Nothing is detected. | If the image changes when Output equals Input, this is the wrong curve. |
| Output Tonemapper | same as Input | AgX (Witcher 3 Remastered) | First set it equal to Input, the image must come back unchanged apart from a clipped highlight. Then pick another curve if you want a different look. | Each curve changes contrast, roll-off and color. Match Mid-Grey keeps the overall brightness. |
| Match Mid-Grey | on | on | On scales the light so mid gray lands where the input curve put it, so only contrast, highlight roll-off and color change. | Off lets the overall brightness change with the curve (mid gray differs a lot between curves). |
| Keep Clipped Whites | on | on | Pixels that are white in the game's image stay at least that white after the conversion. | Off can leave clipped highlights slightly gray on AgX and ACES 2.0. |
| Light Rays Pick Up After Conversion | on | on | On: rays pick up the converted image. Off: the game's original image. Costs one conversion per effect texel. | Ignored with the game's HDR scene light and in HDR. |
| Output Dither (levels) | 0 | 1 | Raise it to 1 when you see banding in dark gradients (undoing and re-applying a curve stretches them), up to 2. | Too high shows as fine noise in dark areas. |
| Input Exposure (EV) | 0 | 0 | Only if mid gray sits wrong on the input curve (stops brighter than standard the game fed its curve). It only matters with Match Mid-Grey on. | It moves where the new curve's highlight roll-off starts relative to mid gray. |
| Output Exposure (EV) | 0 | 0 | Stops added to the scene light before the output curve (taste), on top of Match Mid-Grey. | Too high clips highlights, too low crushes the picture. |
| Output Contrast | 1 | 1 | Above 1: darks get darker and brights brighter around mid gray. Below 1: the opposite. 1 is off. | Large values clip highlights and crush shadows. |
| Output Saturation | 1 | 1 | Color saturation after the output curve, around each pixel's own luminance. 1 is off, 0 is gray. | Above 1.5 gets garish quickly. |
| Reinhard: White Point | keep | 100 | Reinhard output only: the scene light that maps to white. Lower = highlights clip sooner and look punchier, 100 = plain Reinhard. | Too low blows out highlights. |
| Hable: Shoulder Strength | keep | 0.15 | Hable output only: higher = a harder, earlier highlight roll-off. | Too high flattens highlights. |
| Hable: Toe Strength | keep | 0.2 | Hable output only: higher = darker, more crushed shadows. | Too high loses shadow detail. |
| Hable: White Point | keep | 11.2 | Hable output only: the scene light that maps to white. | Lower = brighter overall, higher = darker. |
| Gran Turismo 7: Shoulder (alpha) | keep | 0.25 | Gran Turismo 7 output only: how softly the highlights roll off (higher = a harder shoulder). |  |
| Gran Turismo 7: Linear Section | keep | 0.44 | Gran Turismo 7 output only: where the straight part ends and the shoulder starts. Higher = more of the picture stays linear. |  |
| Gran Turismo 7: Toe Strength | keep | 1.28 | Gran Turismo 7 output only: how much the shadows are pushed down (1 = none). | Too high crushes shadows. |
| Uchimura: Contrast | keep | 1 | Uchimura output only: slope of the linear section. |  |
| Uchimura: Linear Section Start | keep | 0.22 | Uchimura output only: where the toe ends and the straight part begins. |  |
| Uchimura: Linear Section Length | keep | 0.4 | Uchimura output only: how long the straight part is before the shoulder. |  |
| Uchimura: Toe (black tightness) | keep | 1.33 | Uchimura output only: higher = darker, tighter shadows. | Too high crushes blacks. |
| AgX: Look | Base | Base | AgX output only: Base is the plain curve, Punchy adds contrast and saturation, Golden a warm tint. |  |

#### Stage 4, the level (brightness)

Now the GI itself. With the Expert sections still at their defaults (so the basic bounce works), raise **GI Intensity** from 0 and watch *Debug View > GI Only* (use **Debug Gain** to see it clearly, it only affects the debug views). Get the shape right first and finalize the level at the very end, since everything else scales with it.

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| GI Intensity | 0 | 1 | Raise it until the added light is visible but not dirty, look at *Debug View > GI Only* with Debug Gain. Finalize this last. | Too high looks washed out and muddy, shadows start to glow. |
| Dark Area Boost | 0 | 0 | Adds GI to surfaces that are dark compared to the screen's average (shadows, interiors). 1 = twice the GI at full darkness. | Too high lifts shadows into flat gray. |
| Dark Area Threshold | keep | 1.05 | What counts as dark, relative to the screen's own average brightness (measured every frame), so it follows the scene and you rarely retune it. | Lower = only the darkest surfaces get the boost, higher = more surfaces count as dark. |
| Highlight Rolloff (added light only) | 0 | 0.17 | Soft-limits the light the GI adds so it approaches white smoothly instead of clipping. 0 = hard add, 1 = full rolloff. The game's own image is never changed. | Too high flattens bright areas. |
| Rolloff Minimum Headroom | keep | 0.03 | Rolloff assumes at least this much headroom even on nearly white pixels, so already-bright areas still get a little light. Raise it if highlights look flat. | Lower it to protect highlights more. |

#### Stage 5, noise and steadiness (temporal, denoise, upsample)

This is where most of the quality is won or lost, and it's done before the bounce character because noise hides everything else. Start with everything at the blank values (so you see the raw, noisy result), then bring the smoothing in one piece at a time: temporal accumulation first, then the far cascades' own history, then the denoise, then the upsample.

What to look at: *Debug View > GI Before Denoise* to see the raw light, *Motion Flow* to check the motion (it shows the game's vectors while Use Game Motion Vectors is on), and *Show Single Cascade* if one cascade looks noisier than the rest.

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Temporal Accumulation | off | on | Turn it on: the GI (never the game image) is blended with previous frames, moved with the motion and rejected where depth or light disagree. All the temporal sliders (and the far cascades' own history) only work while it's on. | Off means every frame stands alone and it's noisy. |
| Frames Accumulated | 1 | 16 | About how many frames the history represents (previous-frame weight 1 - 1/this). Raise it from 1 (no accumulation) toward 16 or more, the maximum is 64. The far cascades accumulate more than this on top of it (see Far Cascades: Accumulation Growth), and they stop accumulating completely at 1. | Higher = steadier light but longer trails when light changes. |
| Light Change Guard | keep | 0.78 | The history is dropped where the light changed by this much. Lower = stricter (less ghosting, less smoothing). | Near 1 trusts the history almost always, you get ghosting. |
| Depth Change Guard | keep | 0.67 | The history is dropped where the surface moved by this much. Lower = stricter, stops light trailing behind moving objects. | Too high leaves trails and disocclusion smears. |
| Motion Mismatch Rejection | 0 | 64 | Shortens trails where the estimated motion couldn't find a good match (fast motion, moving objects). 0 trusts the motion everywhere. | Too high throws the history away too often (noisier). No effect with game motion vectors. |
| Ray Jitter Accumulation | 0 | 0.4 | Rotates every ray's start and direction a little each frame (a 16-frame cycle) so thin or sparse geometry converges instead of sliding through a fixed pattern. The amount fades with each pixel's history confidence, so new or rejected pixels use nearly fixed rays (no grain). Only works with Temporal on. | Higher is smoother after a moment but a little noisier right after fast motion. |
| Far Cascades: Accumulate From Cascade | keep | 2 | Cascades from this one on keep their own, longer history and are jittered every frame. Lower = steadier near-mid range, slower to react. |  |
| Far Cascades: Accumulation Growth | 1 | 1 | Each cascade from the start cascade accumulates Frames Accumulated x this ^ (steps past the start + 1) frames, capped at 64. 1 = same as the rest (16 frames at the defaults), 2.3 puts the far cascades on about 37, 64 and 64 frames. Needs Temporal Accumulation on and Frames Accumulated above 1. | Higher reacts more slowly to lighting changes. |
| Far Cascades: Jitter | 0 | 0.4 | 1 moves the far cascades' probes and rays inside their cell every frame. This is what averages out their static noise. | 0 leaves a fixed pattern, so the static noise stays. |
| Far Cascades: Half-Rate Update (faster) | off | off | Performance: the accumulated far cascades trace half of their probes each frame (checkerboard) and reuse the history for the rest. Roughly halves their cost. | Slightly slower to react to lighting changes. |
| Denoise Mode | Off | Edge-Aware | Edge-Aware runs the a-trous filter on the accumulated light at the GI's own resolution, before the upsample, never across depth edges or corners. The Shadow's escape visibility is filtered with it. | Off leaves only the temporal accumulation (and the escape shadow unfiltered). |
| Denoise Iterations | 1 | 5 | Number of a-trous passes (5 x 5 taps each, the spacing doubling every pass). 2 reaches about 6 texels, 4 about 30, 5 about 62 at Tap Spacing 1. | More = smoother and a little more cost, fewer = sharper and cheaper. |
| Denoise Blur Strength | keep | 4 | How much brightness difference the filter averages across (the luminance edge stop). Depth and normal edges are never crossed. | Low keeps the noise itself (grainier), high blurs light patterns together. |
| Denoise Tap Spacing (texels) | keep | 0.75 | Scales the tap spacing of every pass (1 = contiguous taps in the first pass). Above 1 the reach grows without extra passes. | High values can leave faint grid patterns. |
| Variance Guided Denoise | 0 | 1 | 1 lets the measured noise of each pixel (from its accumulated moments) steer the filter, as SVGF does, so noisy pixels blur widely and clean ones stay sharp. | 0 is the plain depth and normal guided filter (smoothest, softens light detail). |
| Denoise Distance Shrink | 0 | 0.66 | Divides the tap spacing by 1 + depth x this, so distant surfaces blur less. | Too high under-filters the distance. |
| Denoise Edge Test | keep | Absolute depth | How a tap is judged to be on the same surface: surface plane (grazing floors stay smooth), relative depth (the default), or absolute depth. |  |
| Denoise Edge Sharpness | keep | 0.24 | Strength of that edge test. Higher = stricter, more residual noise near edges. Lower = more blur across edges. |  |
| Denoise Normal Power | keep | 3.24 | Higher = the blur stops at creases and corners, lower = it blurs across them. |  |
| GI Upsample | Bilinear | Edge-aware, surface plane | Edge-aware (surface plane or relative depth): each pixel only takes light from low-res texels on its own surface. Bilinear is plain filtering. | Bilinear gives halos at depth edges (it is the cheapest). |
| Upsample Edge Sharpness | keep | 2 | How strictly the upsample sticks to the pixel's own surface. Higher = crisper edges. | Too high and thin features may pick a single tap, too low means halo risk. |
| Upsample Normal Power | keep | 2.72 | Higher = sharper at creases. |  |

#### Stage 6, the character of the bounce (multibounce, shaping, colors)

Multibounce is what makes rooms, corridors and caves fill with light, and it's the part where "too much" is easy to reach, so raise it slowly. The color sliders only change how the bounce light looks, not how much of it there is. Relit Pickup Bias (Expert: Light Pickup, in stage 9) is also part of this stage, it controls how much a bounced surface's light comes from this effect instead of the game's baked lighting.

What to look at: *Debug View > Bounce Only, no sky* to see the bounce by itself.

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Multibounce | off | on | On: rays also pick up the GI that last frame put on the surface they hit, as light of that surface's color. Fills rooms, corridors and caves, settles within a few frames. | Off is a single bounce. |
| Multibounce Strength | 0 | 1 | Reflectance x GI received x GI Intensity x this. 1 is physical with the GI as you see it, and the loop is held below 0.9 per bounce so it always settles. | Near 2, enclosed rooms glow and fill with light. It's scaled by 1 - Relit Pickup Bias. |
| Multibounce From Cascade | 1 | 2 | Rays of this cascade and all farther ones bounce. 1 (all) is what makes it visible indoors, because most bounce light comes from near surfaces. | Higher values keep multibounce to long-range light only. |
| Bounce Reflectance (estimate) | keep | 0.15 | Reflectance of an average surface (with the surface's own hue), used when the game's albedo isn't available, around 0.5 is typical for game worlds. Raise it from 0.1 and rooms carry more bounce, lower values settle faster. | Too high and enclosed rooms fill with light. Ignored where Use Game Albedo provides the real value. |
| Multibounce Spread | keep | 1 | The bounce is read over the penumbra of the ray that found the hit. Lower = sharper. | Higher = broader and slower to build up. |
| Multibounce Surface Match | keep | 0.7 | Weights the patch by depth and normal agreement so light reaches the wall of a corner by bouncing, not through blur. Only used with `CRCGI_SECONDARY_DIVISOR 0`. | 0 is a plain read. |
| Bounce Contrast Curve | 1 | 0.65 | Power curve around the pivot. Below 1 evens the bounce out (weak lifted, strong reduced), above 1 widens the gap. The overall average stays about the same. | Strong values make weak bounce noisy or the strong parts harsh. |
| Bounce Curve Pivot | keep | 0.14 | The bounce level the curve leaves unchanged. Set it near your typical bounce level (*Debug View > Bounce Only*). |  |
| Bounce Curve Max Gain | 1 | 4 | The most the curve may multiply any bounce by. Caps the lift of very weak bounce. | Higher amplifies noise when Bounce Contrast is below 1. |
| Bounce Saturation | 0 | 1 | Raises the color saturation of the bounce light, nearby colored surfaces tint their surroundings more. 0 is the bounce as picked up. Go in small steps. | The top of the range (2.4) is very strong. |
| Blue Light Filter | 0 | 0.5 | Desaturates bounce whose hue is near Blue Filter Hue (toward gray). Stops sky-blue or cold surfaces from tinting everything blue. | 1 removes the color completely inside the hue band. |
| Blue Filter Hue | keep | 0.56 | The hue the blue filter targets on the 0 to 1 color wheel (0 red, 0.17 yellow, 0.33 green, 0.5 cyan, 0.67 blue, 0.83 magenta). |  |
| Blue Filter Hue Width | keep | 0.33 | How far around its hue the blue filter reaches, fading out smoothly toward the edge. |  |
| Orange Light Filter | 0 | 0.5 | Desaturates bounce whose hue is near Orange Filter Hue. Calms warm tints from lamps, sand or sunsets. | 1 removes the color completely inside the band. |
| Orange Filter Hue | keep | 0.01 | The hue the orange filter targets (0 red, 0.08 orange, 0.17 yellow). |  |
| Orange Filter Hue Width | keep | 0.4 | How far around its hue the orange filter reaches. |  |
| GI Color Temperature | 0 | 0 | Shifts the GI warm (above 0) or cool (below 0). The size of the shift at full value is Temperature Strength. |  |
| Temperature Strength | keep | 0.15 | How big the red/blue shift is at GI Color Temperature +1 or -1. |  |

#### Stage 7, sky

Sky light is small on purpose (it's added on top of the bounce), so most of the tuning is picking the tint and the occlusion. Start with Sky Light off and Sky Light Strength at 0, switch it on, and raise the strength a little at a time.

What to look at: *Debug View > Escape Visibility* and *Enclosure* (how open each area is, which is what gates the sky light).

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Sky Light | off | off | On adds a tintable sky ambient gated by the cascades' own visibility: open areas get sky light, crevices don't, nothing gets darker. Needs Sky Light Strength above 0. |  |
| Sky Color Source | Scene Sky | Scene Sky (sampled) | Scene Sky samples the sky at 6 points near the top of the screen. Custom uses Custom Sky Color, Blend mixes the two. | With no sky on screen the custom color is used. |
| Custom Sky Color | keep | orange-yellow | Only its hue and saturation matter, the brightness comes from Sky Light Strength. |  |
| Blend: Custom Amount | 0 | 0 | Only for Blend: 0 = the sampled sky, 1 = the custom color. |  |
| Sky Tint Saturation | 0 | 0.11 | 0 is neutral white sky light whatever the source, 1 is the sampled or custom color as it is. | Above 1 is exaggerated. |
| Sky Light Strength | 0 | 0.01 | Brightness of the sky ambient (tint x this x visibility). Small values are usually enough, it's added on top of the bounce. | Too high flattens the image with a uniform glow. |
| Sky Occlusion Contrast | 0 | 2.67 | How hard the cascades' sky visibility cuts the sky light. 0 is a flat ambient everywhere, higher = sky light drops faster as an area becomes enclosed. | It only removes sky light, it never darkens the image. |
| Sky Fully-Open Threshold | keep | 0.92 | Sky visibility at or above this counts as fully open. Lower = more areas count as fully open. |  |
| Sky Direction Start | keep | 0.29 | Where a probe direction starts counting as sky-facing, by its upward component (-1 down, 0 horizon, 1 up). |  |
| Sky Direction Full | keep | 0.75 | The upward component at which a direction counts fully as sky-facing. |  |
| Sky Screen-Edge Fade | 0 | 0 | Fades sky light out within this fraction of the screen border, to hide that visibility is unknown past the edge. |  |
| Directional Sky Map | off | on | On: a small map of the sky's color across the screen tints the light by direction (a sunset side, a blue zenith) instead of one average tint. |  |
| Sky Map Brightness Variation | 0 | 0.6 | How much of the sky's brightness gradient (not only its color) reaches the GI. 0 = color only, 1 = brighter sky parts give proportionally more light. |  |

#### Stage 8, the Shadow

At 0 every shadow pass is skipped, so this stage costs nothing until you move Shadow Amount. Shadow Contact Bias decides where the shadow comes from: at 0 only the escape visibility of the two nearest cascades counts (already traced, denoised with the GI, no extra cost, the three contact passes are skipped), at 1 only the contact march (at the effect resolution by default), in between the two overlap.

What to look at: *Debug View > Shadow* (mid grey = no change, darker = shadowed) and *Debug View > Escape Visibility*.

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Shadow Amount | 0 | 1 | Darkens places more enclosed than their surroundings (crevices, corners, the foot of objects), also the game's own image there (65% of the factor). Negative does the opposite (crevices get more bounce light) and leaves the image alone. | Strong values make every seam look dirty. |
| Shadow Contact Bias | 0 | 0.5 | Brings in the contact march (the finest contact lines). At 1 it overrides the escape visibility. | 0 is the softest and cheapest (the contact passes are skipped), 1 the sharpest (and the escape shadow no longer counts). |
| Shadow Contact Range (texels) | keep | 1 | How far the contact march looks and the size of the area it compares with (effect texels). Small = tight joints, larger = shadows at the foot of objects. Cost doesn't depend on it. |  |
| Shadow Contact Depth Bias (texels) | keep | 1 | How far behind depth a contact ray must be to count as blocked. Raise it if flat walls look dirty. | Too low: flat surfaces shadow themselves (speckle, stripes on slopes). Too high: shallow crevices vanish. |
| Surface Albedo Response | 0 | 0.5 | Darker parts of a surface (mortar, dirt, seams) receive less GI and colored surfaces take colored light. 0 adds the GI as plain light. | Too high makes texture show through the lighting. |



Two small topics. The canvas (light that left the screen) and the edge extension decide what happens near the screen border, and the Range & Falloff sliders decide how far from the camera the GI reaches.

What to look at: *G-Buffer / Canvas View > Off-Screen Canvas* (the whole canvas, the yellow frame is the visible screen) and *Off-Screen Light Only* (the GI that comes only from the canvas).

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Screen Edge Extension | 0 | 1 | Rays that leave beyond the canvas continue the nearest border pixel (cascades 1 to 3 only). Keeps light from vanishing at the borders. | It can't invent what is off screen. |
| Off-Screen Canvas | off | on | On: remembers the light and depth of what recently left the screen in a border around it (`CRCGI_CANVAS_MARGIN` percent per side), moved with the motion vectors. | See *Off-Screen Light Only* in the G-Buffer / Canvas View to check what it contributes. |
| Canvas Light Strength | 0 | 1 | How much a ray takes from the canvas compared with an on-screen hit. 1 = like an on-screen hit. |  |
| Canvas Memory (per frame) | keep | 0.998 | Trust in remembered light is multiplied by this each frame (the light keeps its color and brightness). 0.985 = half trust after about 0.8 s at 60 fps, 0.995 about 2.3 s. | Lower is safer when the off-screen scene changes. |
| Max GI Distance (linear depth) | keep | 1.1 | Linear depth where the GI has fully faded out. Lower it to save GI on far scenery. | Above 1 never cuts anything. |
| Distance Fade Width | keep | 0.5 | Width of the fade before the cutoff, as a fraction of the cutoff distance (0 hard cut). |  |
| Fade With Receiver Distance | 0 | 0.5 | The GI is multiplied by 1 - this x linear depth, weakening the GI of far surfaces. | 1 means no GI at the far plane. |
| Hit Distance Falloff | 0 | 0 | Weakens light that a ray picks up far away. 0 is the physical result, 0.3 is gentle, 1 leaves far light at about 1/17. |  |

#### Stage 10, the Expert sections (only when a symptom sends you there)

All of these start at **keep** (the default), and for most games you never need to touch them. Each table below is one Expert section, and the symptom table further down says which one to open for which problem. If you do change one, change one slider at a time and go back to the default if it doesn't help.

##### Light pickup (what a ray gets when it hits something)

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Pre-Filter Gamma | keep | 1.12 | Gamma on the picked-up light. Above 1 favors the brighter parts of the image, below 1 evens lit and dim surfaces out. |  |
| Pickup Blur (texels) | keep | 0.36 | The image the rays pick light from is softened once per frame over this radius (effect texels). Larger = steadier bounce, less sparkle. 0 = off. | Smaller is sharper but noisier. |
| Surface Bounce Threshold | keep | 0.18 | The brightness at which a surface counts for half as a source of bounce light, dimmer counts less, brighter counts more, with no cutoff. Raise it to keep shadowed and dark surfaces from lighting their surroundings. *Debug View > Emitters* shows the result (grey is what's sampled, black is nothing). |  |
| Emitter Threshold | keep | 0.3 | Surfaces brighter than this count as light sources and get Emitter Boost. Open *Debug View > Emitters*: the orange pixels are what the shader treats as emitters. Raise the threshold until only real lamps, fire and sunlit spots are orange, lower it if lamps aren't. | Too low and ordinary lit walls are emitters (everything glows orange in the view, fireflies). Too high and lamps are treated as walls. |
| Emitter Sensitivity (transition sharpness) | keep | 0.6 | How sharp the step from normal surface to emitter is. Higher = narrow, sharp transition, lower = wide and gradual, you can see it as the softness of the orange in *Debug View > Emitters*. With Emitter Boost at 1 it only decides which surfaces keep their image color for the relit pickup. |  |
| Emitter Boost | keep | 1 | Multiplier on the light of emitters. 1 = lamps bounce like any lit wall, higher = lamps and bright spots throw more light. It multiplies on top of Pickup Brightness Cap, so a low cap doesn't limit it. | High values cause fireflies. |
| Pickup Brightness Cap (fireflies) | keep | 0.07 | The most light one surface may hand to a ray, before Emitter Boost and HDR Light Pickup multiply it. Low = every bright surface gives the same light (in *Debug View > Hit Radiance Buffer* they all look equally bright) and there are no fireflies. Higher = brighter surfaces throw proportionally more light. | Too high gives fireflies from small bright lights. |
| Dark Surfaces Block Rays | keep | 1 | Dark surfaces act as shadow casters: the band in which a ray counts as hitting them grows with their darkness (what counts as dark follows Surface Bounce Threshold). 0 = off. The thicker band is still limited by Max Hit Thickness. | High values darken the surroundings of dark objects heavily. |
| Relit Pickup Bias (geometry + lights + bounce) | 0 | 0 | How much of a hit surface's light comes from this effect (reflectance times last frame's GI) instead of the game's baked lighting. Raise it from 0 where the game's baked ambient is wrong (flat-lit interiors). 0 = image only. It also reduces Multibounce Strength by the same share, and the other Relit sliders do nothing at 0. | Light sources and bright, directly lit surfaces keep their image color (Relit Highlight Keep). |
| Relit Reflectance | keep | 0.2 | How much of the light that reaches a relit surface it hands on. Lower settles faster and keeps rooms from filling with light, higher carries bounce farther. |  |
| Relit Albedo Detail | keep | 0.5 | How much of a pixel's reflectance detail is kept (its brightness relative to the plane-local average). 0 = every surface reflects the same, only its hue is kept. Ignored with game albedo. | 1 also reads the edge of a baked shadow as dark texture. |
| Relit Detail Radius (cascade texels) | keep | 9 | How far around a pixel the baked lighting is assumed constant (effect texels). Larger = broader shadows and light pools are removed from the reflectance. | Ignored with game albedo. |
| Relit Highlight Keep | keep | 0.6 | Bright surfaces are mostly directly lit (sun, lamps), and direct light is not in the GI, so they keep their image color. 1 = surfaces above about 0.25 to 0.7 display brightness are never relit. | 0 keeps only the emitters. |

##### Radiance cascades (rays, reach, seams)

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Rays Per Cell (quality) | keep | 3 | Each probe direction traces this many stratified sub-rays and averages them. More = smoother, steadier cascades. Cost is linear. |  |
| Ray Steps: Cascades 1-2 (near) | keep | 4 | Samples per ray in cascades 1 and 2 (the short ones). A few are enough. Raise it if thin geometry close to surfaces is missed. | Cost is linear. |
| Ray Steps: Cascades 3+ (far) | keep | 12 | Samples per ray in cascades 3 and up (the long ones). Long intervals need more steps to not skip thin geometry. | Cost is linear, and about half with Half-Rate Update. |
| Cascade Reach (screen diagonals) | keep | 0.15 | How far the farthest cascade reaches (screen diagonals). Lower = far cascades look at nearer geometry with finer steps (steadier), higher = light from farther across the screen. |  |
| Reach Applies From Cascade | keep | 2 | The first cascade that Cascade Reach stretches. Lower = more cascades share the reach (each shorter and denser), higher = only the last ones are stretched. |  |
| Cascade Cross-Fade | keep | 0.25 | Each cascade's rays continue this fraction into the next interval and fade out while the next fades in. Removes rings at fixed distances. 0 = off. | Costs that fraction of extra steps. |
| Adaptive Ray Count | keep | Boost new areas, save on stable ones | Uses the history to decide how many rays a probe needs: *Boost new areas* traces 4 rays per cell where the surface was hidden last frame, *Boost new areas, save on stable ones* also traces one ray fewer where the history is settled. Only the geometry decides, so image noise never flips it. | Needs Temporal Accumulation on. |
| Ray Start Jitter (draw distance vs noise) | keep | 0.4 | Each ray's first sample sits at a fixed per-ray offset so neighboring rays probe different spots and thin far geometry is found on average. 1 = most coverage. | 0 means no pattern noise but misses more. |
| Radiance Cone Blur | keep | 0 | Rays sample a mip-mapped radiance buffer at a level matching how wide their cone is at the hit. Softens speckle from small bright lights. 0 = off. | Higher = blurrier distant light. |
| Max Cone Blur Mip | keep | 5 | The blurriest radiance mip the cone blur may use. | Lower keeps far light sharper (and noisier). |

##### Merge and gather (probes, leaks, far energy)

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Gather Smoothing (quality) | keep | 1 | 0 reads 4 probes bilinearly. Above 0 reads a wider tent over 16 probes: smoother GI and less blockiness on the probe grid (still edge-aware). | Costs a few more fetches per pixel. |
| Bilinear Direction Merge (quality) | keep | on | When a cascade is merged into the one below, the parent's radiance is read with bilinear filtering between direction cells. Smoother directions at no extra fetches. | Turn it off only to compare. |
| Probe Edge Sharpness | keep | 10 | How strictly probes interpolate only with probes on the same surface (gather and merge). Higher = crisper silhouettes, less leaking across edges. | Lower = smoother but more bleed. |
| Probe Edge Weighting | keep | Relative depth + normal | How a neighboring probe is judged to be on the same surface: relative depth + normal, or distance from the receiver's tangent plane (keeps grazing floors and walls smooth). |  |
| Probe Normal Agreement Power | keep | 5.33 | Relative-depth weighting only: probes count by dot(normals) raised to this. Higher = sharper creases. |  |
| Merge Sharpness Falloff | keep | 0 | Probe Edge Sharpness is divided by 1 + this x cascade when merging, so coarse far cascades blend more freely. 0 = same at every cascade. |  |
| Merge Horizon Cutoff | keep | 0.32 | A parent probe is skipped for a direction closer than this to its horizon, avoiding grazing directions its surface can't see. 0 = off. |  |
| Energy Carry (per cascade step) | keep | 1 | How much of the light a farther cascade hands down survives the merge (compounds toward the near cascades). Above 1 makes up the energy far cascades lose, below 1 favors near light. Only light is scaled, never visibility. | Too high makes far light dominate. |
| Energy Carry From Cascade | keep | 3 | Energy Carry scales the light handed down by this cascade and every farther one. 2 = every merge. Higher = only the far cascades are boosted. |  |
| Merge Visibility Check | keep | 1 | The straight segment from a child probe to each parent probe is checked against the depth buffer (3 taps). A parent hidden behind something is down-weighted by this much, so light doesn't leak around corners. 0 = off. |  |
| Merge Visibility Tolerance | keep | 0.06 | How far a surface must stick out in front of the segment to count as blocking it. Higher = only clear occluders count (fewer false blocks on bumpy surfaces). | Lower = stricter. |

##### Occlusion and depth separation (what blocks and what bleeds)

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Depth Separation Limit | keep | 1.04 | A surface only receives bounce from something close to it in depth (relative to distance). Starting from the lowest value (0.05) only surfaces at nearly the same depth exchange light, raising it lets surfaces farther apart in depth light each other, and at this value the bounce has faded to zero. 20 = off. A character far in front of a wall stops tinting it once the gap is bigger than the limit. | Too low and light between different depths is cut (flat looking, far light weak). Too high and foreground objects paint the background. Long draw distances (a road to the horizon) need a high value. At 20 Softness, Foreground-to-Background Block and Far Cascade Separation Relax have nothing left to shape. |
| Depth Separation Softness | keep | 0.6 | How gradually the bounce fades out before the limit. Small (0.05) = full strength then a sharp drop near the limit, 1 = starts fading at 0. Needs a Depth Separation Limit below 20 to matter. |  |
| Foreground-to-Background Block | keep | 1 | Extra strictness when the light comes from something closer to the camera than the receiving surface. 0 = same limit both ways, 1 shrinks the limit to 40% for foreground emitters. Stops foreground objects painting the background. | Stops foreground objects painting the background. |
| Far Cascade Separation Relax | keep | 0.2 | Far light usually comes from a surface at a different depth, so the limit is multiplied by 1 + this x the cascade number. 0 = same limit in every cascade, raise it from 0 if far light is weak. | Raise it if far light is weak. |
| Occlusion Tolerance | keep | 0.05 | A surface can only block a ray for a receiver if it is at least this much nearer (linear depth). 0 = anything nearer can block (tightest contact shadows, but flat surfaces can shadow themselves), raising it stops self-shadowing speckle. Which surfaces may block is decided here and by Occluder Depth Range, how far behind a surface a ray must be is Occlusion Depth Threshold. |  |
| Occluder Depth Range | keep | 0.2 | How far in front of the receiver a blocker may be, relative to the receiver's depth. From the lowest value (0.1) only very close blockers count, raising it lets blockers farther in front count, above 1 anything in front can block. Something much nearer than the range (a character in front of a wall) only overlaps on screen and blocks nothing. |  |
| Relative Depth Tolerances (long draw distance) | keep | 0 | For very long far planes (racing, flight, open worlds): scales Occlusion Tolerance, Occlusion Depth Threshold, Ray Depth Drift Limit and the offsets in the depth tests by the depth of the surface being tested. 0 = absolute depth units, 1 = relative everywhere. Raise it when nothing blocks near the camera because the whole scene sits at depth 0.001 to 0.05. |  |
| Tolerance Reference Depth | keep | 0.001 | The linear depth at which the tolerances keep exactly their slider values when Relative Depth Tolerances is above 0. Set it to the typical depth around the car or the camera. Does nothing while Relative Depth Tolerances is 0. |  |

##### Geometry reconstruction (normals rebuilt from depth)

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Silhouette Angle (degrees) | keep | 3.2 | A depth step steeper than a surface this close to edge-on is a silhouette, not a slope. | Higher = more steps count as silhouettes. |

#### The debug sliders

These never change the picture you'd keep, they only change what you look at.

| Slider | Start | Default | Raise it and you get | Too much (or too little) |
|---|---|---|---|---|
| Debug View | Off | Off | Replaces the picture with an internal buffer so you can check one stage at a time (GI Only, Bounce Only, Normals, Linear Depth, Motion Flow, Emitters, ...). |  |
| Debug Gain | keep | 6 | Brightness multiplier for the GI-type debug views and the single-cascade view. No effect on the normal image. |  |
| Show Single Cascade | Off | Off | Shows one cascade on its own interval with nothing merged in. Use it to tune Cascade Reach, Ray Steps and the far accumulation. |  |
| Cascade Debug Shows | keep | Radiance (interval only) | What the single-cascade view shows: the light that cascade picked up, or hit coverage (white = everything hit). |  |
| G-Buffer / Canvas View | Off | Off | Shows what the game hands to the shader (albedo, normals, motion, HDR scene light) and the off-screen canvas. |  |

### Which sliders overlap (and when that's useful)

Most sliders in the shader exist so you can fix one specific symptom without touching anything else, which means some of them cover the same ground, and some of them stop doing anything once another one passes a threshold. This section says which slider hides which, and when the "redundant" one is still worth using. Read it once and you'll stop moving sliders that aren't connected to anything.

#### Hit detection (what counts as a hit, what blocks a ray)

| When this is the case | ...this slider does nothing (or very little) | Why, and when each one is useful |
|---|---|---|
| **Hit Thickness Mode** is Generic | Thickness Search Range, Thickness Silhouette Gap, Object Thickness Scale | They only feed the silhouette measurement. Generic is cheaper and predictable (every surface gets the same band), good for scenes of mostly one kind of geometry. Silhouette-derived is for scenes that mix thin things (railings, poles, foliage) and big ones, because one fixed thickness is either too thin for the walls or too thick for the poles. |
| **Object Thickness Cap** is at or above **Max Hit Thickness** | Object Thickness Cap | The whole band can never be thicker than Max Hit Thickness, so the cap wins. It works both ways: keep Max Hit Thickness high (1) and Object Thickness Cap is your only thickness control, or keep Object Thickness Cap small and let Max Hit Thickness rein in what adds on top of it (the ray's step and the slope). |
| **Max Hit Thickness** is high compared to Object Thickness Cap plus the ray's step and slope | Max Hit Thickness | It's a safety ceiling, it only acts where the band would grow big, which is the long steps of the far cascades and surfaces close to the camera. Use it when the far cascades are splotchy or the player's surroundings go dark (rays hitting the character from far behind), lower it until that stops. It won't change the fine cascades much. |
| Silhouette mode, and the measured thickness times **Object Thickness Scale** is already above **Object Thickness Cap** | raising Object Thickness Scale further | The cap limits the result. To make thin objects easier to hit, raise the scale while it's still under the cap, past that raise the cap too. |
| **Occlusion Depth Threshold** is smaller than the hit band | Occlusion Depth Threshold | A ray is blocked only when it's behind the surface by more than the band **and** more than this threshold, the bigger of the two wins. At 0 everything behind a surface blocks (hardest contact shadows). Raising it above the band is how you let rays pass behind thin or distant objects without touching the band (which would also change what hits). |
| **Occlusion Tolerance**, **Occluder Depth Range** and **Occlusion Depth Threshold** | each other, when one is extreme | A blocked ray needs all three: Occlusion Tolerance (the blocker must be nearer than the receiver by this much), Occluder Depth Range (not too far in front) and Occlusion Depth Threshold (the ray far enough behind). Making any one of them very strict switches blocking off whatever the others say, so use the one that matches the false shadow you see: self-shadow speckle on flat surfaces = Occlusion Tolerance, a character in front of a wall shadowing it = Occluder Depth Range, thin objects blocking = Occlusion Depth Threshold. |
| **Dark Surfaces Block Rays** | itself, when Max Hit Thickness is low | The thicker band dark surfaces get is still capped by Max Hit Thickness. |
| **Facing Check Intensity** is 0 | Facing Check Steepness, Facing Check Bias | There's no facing check left to shape. Use Intensity to turn the check on, Steepness for how sharply it switches, Bias for how much grazing light survives. |
| **Ray Depth Model** is Perspective-correct | Ray Depth Slope (per texel) | Slope only exists for the Slope heuristic. The other way round, **Perspective Ray Lift** only works with Perspective-correct. Ray Depth Drift Limit applies to both. |
| **Relative Depth Tolerances** is 0 | Tolerance Reference Depth | The reference only says at which depth the tolerances keep their slider values. Use the pair on games with a very long draw distance, where absolute depth values are far bigger than the geometry. With it on, Occlusion Tolerance, Occlusion Depth Threshold and Ray Depth Drift Limit mean "at the reference depth". |
| **Depth Separation Limit** is 20 | Depth Separation Softness, Foreground-to-Background Block, Far Cascade Separation Relax | At 20 nothing is rejected by depth, so there's nothing left to soften or relax. Depth separation works **after** a hit (it weights the light), hit detection decides whether there was a hit at all, so a too strict separation looks like missing bounce and not like missing hits. |

#### Temporal accumulation and the far cascades

| When this is the case | ...this slider does nothing (or very little) | Why, and when each one is useful |
|---|---|---|
| **Temporal Accumulation** is off | Frames Accumulated, Light Change Guard, Depth Change Guard, Motion Mismatch Rejection, Ray Jitter Accumulation, the four Far Cascades sliders, Adaptive Ray Count | They all work on the history. Off is for judging the raw, noisy light. |
| **Frames Accumulated** is 1 | the Far Cascades sliders (Accumulate From, Growth, Jitter) | The far cascades only accumulate while there's an accumulation to build on. |
| **Use Game Motion Vectors** is on | Motion Mismatch Rejection | The motion is exact, there's no mismatch to reject. Keep Motion Mismatch Rejection for the shader's own motion estimate (no add-on). |
| **Ray Jitter Accumulation** vs **Far Cascades: Jitter** | the smaller of the two, on the far cascades | The accumulated far cascades use the larger of the two. So Ray Jitter Accumulation above Far Cascades: Jitter makes the far one redundant, and the other way round. Use Far Cascades: Jitter alone when you want the far cascades jittered more than the near ones. |

#### Denoise, upsample and geometry

| When this is the case | ...this slider does nothing (or very little) | Why, and when each one is useful |
|---|---|---|
| **Denoise Mode** is Off | every other Denoise slider | There's no filter to tune. Off is for judging the accumulated light or saving the filter passes. |
| **GI Upsample** is Bilinear | Upsample Edge Sharpness, Upsample Normal Power | Plain filtering has no edge test. The Edge-aware modes differ in how they judge a surface, the two sliders are shared. |
| **Denoise Edge Test** | the choice only changes how Denoise Edge Sharpness is read | One slider serves all three tests, which is why 1 is not the same strictness in every mode. |
| **Geometry Source** is not "Rebuild from depth everywhere" | Silhouette Angle (mostly) | Pixels that have a game normal skip the normal rebuilt from depth. It still matters for sky, holes and everything without a game normal. |

#### Light pickup, bounce and color

| When this is the case | ...this slider does nothing (or very little) | Why, and when each one is useful |
|---|---|---|
| **Pickup Brightness Cap** is low | the difference between bright surfaces | The cap is applied **before** Emitter Boost and HDR Light Pickup multiply the light, so a low cap makes every bright surface give the same light and the boosts are what separate emitters from walls. Raise the cap when you want brighter surfaces to throw more light without calling them emitters. |
| **Emitter Boost** is 1 and **HDR Light Pickup** is 0 | Emitter Threshold and Emitter Sensitivity (mostly) | Nothing is multiplied for emitters any more, so the classification only decides which surfaces keep their image color for the relit pickup. Debug View > Emitters shows the classification either way. |
| **HDR Light Pickup** is 0 | HDR Pickup Range | Range is the ceiling of the pickup. |
| **Relit Pickup Bias** is 0 | Relit Reflectance, Relit Albedo Detail, Relit Detail Radius, Relit Highlight Keep | The relit pickup is off. And the other way round, at 1 it passes all the bounce itself and **Multibounce Strength** is scaled down to nothing (both are the same bounce counted two ways). Relit Pickup Bias is for fixing flat baked lighting, Multibounce is for filling rooms. |
| **Use Game Albedo** is on (and the albedo is valid) | Relit Albedo Detail, Relit Detail Radius, Bounce Reflectance (estimate) | The real albedo replaces the estimates. Keep them for games where the add-on can't deliver albedo. |
| **Multibounce** is off | Multibounce Strength, Multibounce From Cascade, Bounce Reflectance (estimate), Multibounce Spread, Multibounce Surface Match | Single bounce only. Multibounce Surface Match also needs `CRCGI_SECONDARY_DIVISOR 0`. |
| **Bounce Contrast Curve** is 1 | Bounce Curve Pivot, Bounce Curve Max Gain | No curve to pivot. Max Gain only matters below 1 (it caps the lift of weak bounce). |
| **Blue Light Filter** / **Orange Light Filter** is 0 | their Hue and Hue Width sliders | Hue and width only say where the filter acts. **GI Color Temperature** at 0 does the same to Temperature Strength. |
| **Highlight Rolloff** is 0 | Rolloff Minimum Headroom | The headroom floor belongs to the rolloff. |
| **Dark Area Boost** is 0 | Dark Area Threshold | The threshold only says what counts as dark. |
| **Convert Tonemapper** is off (or the game is HDR) | the Tonemapper Conversion sliders and Output Dither | Nothing is converted. The Input Tonemapper still shapes HDR Light Pickup in SDR. The tunables (White Point, Shoulder, Toe, Look and so on) only act on the Output Tonemapper that matches them. |
| **Sky Light** is off, or **Sky Light Strength** is 0 | every other Sky slider | There's no sky light to tint. Directional Sky Map off makes Sky Map Brightness Variation do nothing, Sky Color Source set to Scene Sky makes Custom Sky Color and Blend: Custom Amount do nothing. |

#### Range, edges and cascades

| When this is the case | ...this slider does nothing (or very little) | Why, and when each one is useful |
|---|---|---|
| **Max GI Distance**, **Fade With Receiver Distance** and **Hit Distance Falloff** | each other, partly | All three weaken the GI with distance, in different ways: Max GI Distance is a hard limit on the receiver's depth (with Distance Fade Width as its soft edge), Fade With Receiver Distance is a smooth falloff on the receiver's depth, Hit Distance Falloff weakens light by how far the **light** travelled. Use the first for performance and far scenery, the second for taste, the third only if far light is too strong (0 is the physical result). |
| **Off-Screen Canvas** is off, or `CRCGI_CANVAS_MARGIN` is 0 | Canvas Light Strength, Canvas Memory | With margin 0 the canvas isn't even built. **Screen Edge Extension** is what's left for rays that leave the screen, and it only acts outside the canvas, so a big canvas margin makes it matter less. |
| **Shadow Amount** is 0 | Shadow Contact Bias, Shadow Contact Range, Shadow Contact Depth Bias | Every shadow pass is skipped. Surface Albedo Response doesn't depend on it and works on its own. |
| **Shadow Contact Bias** is 0 | Shadow Contact Range, Shadow Contact Depth Bias | Only the escape visibility counts, the contact passes are skipped. |
| **Energy Carry** is 1 | Energy Carry From Cascade | There's nothing to carry. |
| **Merge Visibility Check** is 0 | Merge Visibility Tolerance | No check to tolerate. |
| **Probe Edge Weighting** is Surface plane distance | Probe Normal Agreement Power | The power is part of the relative depth + normal weighting only. |
| **Radiance Cone Blur** is 0 | Max Cone Blur Mip | Nothing to limit. |
| **Cascade Reach** and **Reach Applies From Cascade** | each other | Reach says how far the last cascade goes, Reach Applies From Cascade says how many cascades share that stretch. Change them together, one at a time (*Debug View > Show Single Cascade* shows each interval). |

### Symptom to fix

| Symptom | Likely cause | What to change |
|---|---|---|
| Normals speckled or striped | wrong fov or depth setup | Vertical Field of View, ReShade depth, Geometry Source |
| Whole image changes color or brightness | tonemapper doesn't match the game | Input Tonemapper (Output = Input first), or Convert Tonemapper off |
| Banding in dark gradients | curve undone and re-applied | Output Dither up |
| Flat surfaces dirty or self-shadowed | contact or hit band too small | Shadow Contact Depth Bias up, Occlusion Tolerance up, Perspective Ray Lift up |
| Light leaks around edges and corners | probes mixing across surfaces | Probe Edge Sharpness up, keep Merge Visibility at 1, lower Depth Separation Limit |
| Rings or bands at fixed distances | cascade seams | Cascade Cross-Fade up (default 0.25) |
| Ghosting, light trails after movement | history too long or trusted too much | Frames Accumulated down, Light and Depth Change Guard down, Motion Mismatch Rejection up |
| Far light flickers | far cascades under-accumulated | Far Accumulation Growth up or Start cascade down, keep Far Jitter at 1 |
| A lamp you switch on takes long to show up | long far history | Far Growth down (the brightness-rise pass-through helps on its own) |
| Noise after fast movement | history lost | Adaptive Ray Count, Rays Per Cell up, Temporal Jitter |
| Fireflies from small bright lights | single bright hits | Pickup Brightness Cap down, Emitter Boost down, HDR Pickup Range down, Pickup Blur up, Radiance Cone Blur up |
| Indoor rooms look flat or dark | too little multibounce | Multibounce Strength up, From Cascade 1, Bounce Reflectance up, Dark Area Boost up |
| Rooms glow or blow out | bounce too strong | Multibounce Strength down, Relit Reflectance down, Bounce Reflectance down |
| Bounce too saturated or tints everything | saturation boost | Bounce Saturation down, Blue / Orange Light Filter up, Sky Tint Saturation down |
| Distant terrain lit like scenery, sky edge lit | sky threshold | Sky Rejection Depth |
| Blocky or grid-patterned GI | probe grid showing | Gather Smoothing up (default 1), Bilinear Direction Merge on, Denoise Mode on with a bigger radius |
| Halos at depth edges | upsample or gather mixing surfaces | Upsample Edge Sharpness up, Probe Edge Sharpness up, don't use bilinear upsample |
| A foreground object paints the wall behind it | depth separation too loose | Depth Separation Limit down, Foreground-to-Background Block up |
| Far light is weak (far cascades lose energy) | rejection and missed geometry | Energy Carry up, Far Cascade Separation Relax up, Cascade Reach |
| Dark or tinted screen rim | off-screen memory is wrong | Canvas Light Strength down, Canvas Memory down, Screen Edge Extension |
| Off-screen light is wrong or stale | canvas trust lasts too long | Canvas Memory down (0.8 at the lowest) |
| World-space normals look mirrored or lit from the wrong side | space or axes | Game Normal Space = World, Normal Auto-Align Smoothing, compare in *Normal Difference* and *Game Normal Auto-Align Status* |
| Light trails follow moving objects (game motion vectors) | wrong units or direction | G-Buffer tab Units, Flip Y, Invert direction, check *Game Motion Vectors* |
| Lamps don't light their surroundings, or everything glows | emitter detection is off | *Debug View > Emitters* (orange = emitter), Emitter Threshold, Emitter Boost, Pickup Brightness Cap |
| Light leaks through thin things, poles don't cast shadows | hit band too thin or blocking too lazy | Hit Thickness Mode (Silhouette-derived), Object Thickness Cap up, Thickness Search Range up, Occlusion Depth Threshold down |
| Dark halos, the player's surroundings go dark, splotchy far cascades | hit band too thick | Max Hit Thickness down, Object Thickness Cap down, Hit Front Tolerance down |
| Every crevice and thin object is a shadow | blocking too eager | Occlusion Depth Threshold up, Occlusion Tolerance up, Occluder Depth Range down |
| Racing / flight, nothing blocks near the camera | absolute depth tolerances too big | Relative Depth Tolerances 1, Tolerance Reference Depth, higher Depth Separation Limit, Sky Rejection Depth near 1 |

### Recipes

These combine the settings above for common situations, use them as starting points.

- **Indoor, caves, corridors.** Multibounce on with strength 1.0 to 1.5 and From Cascade 1, Bounce Reflectance 0.5 to 0.6, Dark Area Boost 0.3 to 0.6, Sky Light Strength 0 (no sky), Relit Pickup Bias 0.35 to 0.5 if the game's baked ambient is flat, and Shadow Amount around 0.3 to 0.6 for contact depth.
- **Open outdoor.** Sky Light on with a small strength (0.01 to 0.05), Directional Sky Map on, keep Max GI Distance and Fade With Receiver Distance at the defaults, the far cascades matter here so keep Far Jitter at 1 and the far accumulation as it is, and keep the canvas on.
- **HDR output.** Game Output Signal on Auto, set Paper White and Peak to the game's values, keep HDR Light Pickup at 0.6 and watch for fireflies on small bright spots (lower HDR Pickup Range if they show up). With the add-on's *Game HDR Scene Light* the pickup comes from the lighting buffer, with the full range.
- **Racing and flight (long draw distance).** Relative Depth Tolerances 1, Tolerance Reference Depth at the depth where your slider values look right (the typical depth around the car), Sky Rejection Depth just above the horizon's depth, Depth Separation Limit raised (the road runs to the horizon), Far Cascade Separation Relax up, Max GI Distance high.
- **With the full add-on (deferred game).** Geometry Source on *Game normals only* once the Normal Difference view is mostly black (fastest, no depth rebuild), Use Game Albedo on (skips the reflectance estimate), Use Game Motion Vectors on (skips four passes), Use Game HDR Scene Light on, and Use Game Finished Frame on when you want the ui and fog left alone. Each of these is a shader switch (the add-on turns them for you if you tick *Drive the shader's switch* for that feed).
- **Low-end hardware.** `CRCGI_RESOLUTION_DIVISOR` 4 (the default), `CRCGI_CASCADES` 4, Rays Per Cell 1, Ray Steps: Far 8, Far Cascades: Half-Rate Update on, Gather Smoothing 0, Denoise Iterations 3, `CRCGI_CANVAS_MARGIN` 0 if you don't need off-screen light, Shadow Contact Bias 0 (the contact passes are skipped, the escape shadow stays). Expect softer and noisier light.
- **High quality.** `CRCGI_RESOLUTION_DIVISOR` 2, `CRCGI_CASCADES` 5, Rays Per Cell 3, Ray Steps: Far 12, Cross-Fade 0.25, Gather Smoothing 1, `CRCGI_SECONDARY_DIVISOR` 0 for the exact multibounce read, `CRCGI_CONTACT_DIVISOR` 1 or 2 for sharper contact lines. This is a lot more expensive.

## Performance

### Where the cost is

At the defaults a texel traces 40 ray steps per sub-ray set (4 + 4 + 12 + 12 + 12 across the five cascades), most of them in cascades 3 to 5. That gets multiplied by Rays Per Cell (3), plus 25% for the cross-fade overlap and the extra rays on new areas. Outside the rays, the biggest costs are the Shadow's resolve (it runs per screen pixel), the a-trous passes, the gather, the motion-estimation passes and the full-resolution composite.

These are counted from the code (texture reads per frame at 2560 x 1440, divisor 4, 5 cascades, for the parts that changed; the rays, gather, temporal accumulation, motion search and composite read about the same in both). They are not measured frame times: profile your own frame with ReShade's statistics view and toggle one thing at a time.

| Part | v1.6 | v2 | Why |
|---|---|---|---|
| Luma | 3.7 M | 0.9 M | bilinear taps between pixels, a quarter of the reads |
| Prep (depth, normal, pickup) | 18.7 M | 15.9 M | one 8 x 8 plane fit gives depth and normal, no separate smoothing |
| Cascades outside the rays | 61.9 M | 40.8 M | the probe tables (2.7 M) compute probe position, history validity and parent and history weights once per probe; depth-only steps read 4 bytes instead of 16 |
| Shadow, Shadow Contact Bias above 0 | 567.7 M | 100.5 M | the contact march and its blur run at the effect resolution (14.3 M + 5.1 M, one probe per effect texel instead of per pixel) and read the cascades' normal instead of 5 per-pixel plane fits; the resolve (81.1 M) upsamples the march on depth, drops its 3 x 3 average (the blur already does it) and reads the surface brightness at cascade resolution. With `CRCGI_CONTACT_DIVISOR` 1 (full resolution) it's about 380 M |
| Shadow, Shadow Contact Bias 0 | (no such mode) | 3.7 M | only the escape visibility, which the cascades and the denoise already carry |
| Escape visibility in the a-trous passes | none | 27.6 M | one 2-byte read per tap, the price of a denoised shadow |
| **Total of these parts** | **652 M** | **186 M (bias 0.5), 89 M (bias 0)** | |

The copy passes went from 5 to 3 (the history copies are written by the last a-trous pass and by the nearest cascade), and the geometry, probe averages, history geometry and depth pyramid moved to half precision where they're only compared with tolerances. 5 cascades (the new default) cost about one more cascade pass than the 4 that v1.6 used.

### Levers (roughly largest to smallest)

| Lever | Effect |
|---|---|
| `CRCGI_RESOLUTION_DIVISOR` 4 | already the default, 3 is sharper and much more expensive |
| **Shadow Contact Bias 0** | skips the three contact passes and the contact part of the resolve, the escape shadow (denoised, free) stays |
| `CRCGI_CASCADES` 4 | one cascade less (5 is the default), less far reach |
| **Far Cascades: Half-Rate Update** | traces half of the far cascades' probes per frame (checkerboard), roughly halves the cost of the most expensive cascades, with a slightly slower reaction to lighting changes |
| Ray Steps: Far 12 to 8 | linear saving, thin far geometry gets missed more |
| Rays Per Cell 2 to 1 | halves the ray cost, relies on temporal accumulation |
| Adaptive Ray Count: save on stable ones | one ray fewer per cell where the history is settled (the default) |
| Denoise Iterations lower (or Denoise Mode off) | fewer a-trous passes |
| Gather Smoothing 1 to 0 | 2x2 instead of the 4x4 probe filter, visibly softer at probe edges |
| `CRCGI_CANVAS_MARGIN` 0 | removes the canvas and its copy pass |
| Shadow Amount 0 | every shadow pass skipped |
| `CRCGI_SECONDARY_DIVISOR` higher | cheaper multibounce map |
| Light Rays Pick Up After Conversion off | saves one tonemapper conversion per effect texel (only with Convert Tonemapper on) |



- **Game normals** (*Geometry Source: Game normals where valid / only*) replace the normal of the depth plane fit (exact normals, normal maps included).
- **Game albedo** skips the 16-tap per-texel reflectance estimate and the albedo-response taps.
- **Game motion vectors** make the four block-matching passes do no work.

The G-Buffer tab's "shader work saved" list shows what's being skipped right now.

### ReShade's Performance Mode

Once a preset is final, ReShade's own **Performance Mode** (in ReShade's settings) bakes the uniform values into the shader as constants. Use it for the last few percent, and turn it off again when you want to tune.



There are two combos for this and they're separate from each other.

### Debug View (the shader's own stages)

Replaces the picture with an internal buffer so you can check one stage at a time, the GI-type views use **Debug Gain**.

- Off
- GI Only
- Bounce Only, no sky
- Escape Visibility
- Enclosure (1 - escape visibility)
- Normals
- Hit Radiance Buffer
- Cascade 3 Radiance
- Linear Depth
- Object Thickness (red = thin)
- GI Before Denoise
- Shadow
- Motion Flow
- Emitters (sampled light and light sources)

**Emitters** shows what the shader samples light from. Grey is the light rays pick up from ordinary surfaces (times Debug Gain, brighter = more light), orange is every pixel the shader treats as a light source (it follows Emitter Threshold and Emitter Sensitivity, the orange brightness is the sampled light), black is nothing sampled (sky, or surfaces too dark to count, see Surface Bounce Threshold). It's the quickest way to tune the Light Pickup sliders: lamps, fire and sunlit spots should be orange, ordinary walls should not.

Normals is the quickest sanity check, a flat wall must be one even color and speckle means the fov or the depth setup is off. Motion Flow shows the game's vectors while Use Game Motion Vectors is on.

### G-Buffer / Canvas View (what the game hands to the shader, and the canvas)

- Off
- G-Buffer Overview (albedo / normals / motion / depth)
- Game Albedo (input)
- Normals As Used (game, or depth where missing)
- Normals Rebuilt From Depth
- Normal Difference: Game vs Depth
- Game Motion Vectors (input)
- Off-Screen Canvas (whole canvas, screen outlined)
- Off-Screen Light Only (GI from the canvas)
- Game Normal Auto-Align Status
- Game HDR Scene Light (input, exposure-matched)
- Game Material (input, diffuse share)

How to read them:

- Overview, top left is albedo, top right is the normals as used, bottom left is motion, bottom right is depth.
- Normals are shown in this shader's view space (x right, y down, z away, times 0.5 plus 0.5).
- Normal Difference, black means the game and depth normals agree, white means 90 degrees apart, red means there's no game normal.
- Off-Screen Canvas shows the whole canvas scaled to the screen, the yellow frame is the visible screen and outside of it is the remembered border (blue means nothing is remembered there yet). The canvas is at the cascade resolution (screen divided by `CRCGI_RESOLUTION_DIVISOR`) plus `CRCGI_CANVAS_MARGIN` percent on each side.
- Off-Screen Light Only is the GI with every on-screen hit treated as a blocker without light and no sky, so only the light found in the canvas border is left (times Debug Gain).
- Auto-Align Status shows the aligned game normals against the depth normals, with the fit confidence as a bar on top (green is good, red means there aren't enough surface directions in view to align).
- HDR Scene Light is the game's lighting buffer after the automatic exposure match, it should look like the image without ui, bloom and post-processing (black means it's not delivered or there's no match).
- Game Material is the diffuse share the shader takes from the game's material texture, white is fully diffuse and dark is metal (purple means it's not delivered or *Use Game Material* is off).


## Individual slider reference

This is a functional reference, not a preset. Each entry explains the control's purpose and, where relevant, which other controls enable, limit, replace or overlap with it. A slider can appear to do nothing because an upstream switch is off, a different mode is selected, a valid game buffer bypasses an estimate, or a stricter safety limit already dominates. The tuning stages above explain the practical order; these tables explain the individual controls.

### Setup

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Vertical Field of View | The game's VERTICAL field of view in degrees (convert it if the game only shows a horizontal one). |
| Sky Rejection Depth | Linear depth (0 = camera, 1 = far plane) where the sky starts. **Relationship:** Depends on Sky Light and Sky Light Strength. |

### Brightness

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| GI Intensity | Master level of the GI that is added to the image. |
| Dark Area Boost | Extra GI on surfaces that are dark compared to the rest of the screen (shadows, interiors). **Relationship:** Master dark-region lift; Dark Area Threshold is inactive without it. |
| Dark Area Threshold | What counts as dark for Dark Area Boost, relative to the average brightness of the screen (measured every frame, so it follows day and night without retuning). **Relationship:** Only classifies dark regions for Dark Area Boost. |
| Highlight Rolloff (added light only) | Eases the added GI toward white instead of letting it clip in bright areas (the game's own image is never changed). **Relationship:** Controls highlight protection on the added GI; Rolloff Minimum Headroom is inactive when rolloff is disabled. |
| Rolloff Minimum Headroom | The least room Highlight Rolloff assumes above a pixel, so pixels that are already bright still receive a little GI. Raise it if highlights look flat, lower it to protect them more. **Relationship:** Only matters when Highlight Rolloff is active. |

### Game Integration

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Use Game Normals If Provided | Use the game's normals (from the add-on) instead of rebuilding them from depth. **Relationship:** Switches to add-on normals where available; normal-space alignment and geometry-source settings depend on this feed. |
| Game Normal Space | How the game stores its normals. |
| Normal Auto-Align Smoothing | World space only: how much of last frame's fit is kept. |
| Geometry Source (with game normals) | Where the normals come from while game normals are on. **Relationship:** Determines which pixels use depth-rebuilt normals; depth smoothing is bypassed wherever valid game normals are used. |
| Use Game Albedo If Provided | Use the game's albedo (base colour) from the add-on instead of estimating it from the image. **Relationship:** Real albedo supersedes estimated albedo/detail and Bounce Reflectance (estimate) where valid. |
| Use Game Motion Vectors If Provided | Use the game's motion vectors from the add-on instead of the shader's own block-matching estimate (those passes then do no work). **Relationship:** Bypasses the shader's motion estimation and its mismatch rejection where valid. |
| Use Game Material (reflectivity) If Provided | Use the game's material texture (from the add-on) for how much of a surface's light is diffuse. **Relationship:** metals and very reflective surfaces receive and hand on less GI (receiver and multibounce); the add-on sets the channel and whether it holds metalness or specular. |
| Use Game HDR Scene Light If Provided | Use the game's lighting buffer (before tonemapping, bloom, fog and UI) from the add-on as the light rays pick up, instead of the finished image: no UI, no bloom, no film grain, and the full brightness range. **Relationship:** Changes the source sampled for light pickup; reduces dependence on tonemapped final-frame colour and emitter reconstruction. |
| Use Game Finished Frame If Provided | Work on the game's finished 3D image from the add-on (a copy taken before the UI, fog or whatever you chose to leave alone) instead of the final image, and add the light to the final image only where it still shows that frame. **Relationship:** Changes the picture every pass reads (light pickup, luminance, composite base); needs the copy to be the screen's size and format.

### HDR & Color Range

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Game Output Signal | How the game's image is encoded. |
| HDR Paper White (nits) | HDR only. |
| HDR Peak Brightness (nits) | HDR only. |
| HDR Light Pickup | Lets bright sources (lamps, fire, sunlit spots) bounce more than a white wall. **Relationship:** Boosts high-intensity sources; HDR Pickup Range is its ceiling and is inactive without the boost. |
| HDR Pickup Range | The ceiling for HDR Light Pickup: the most a source may be brightened (times this). **Relationship:** Only caps active HDR Light Pickup. |

### Tonemapper Conversion

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Convert Tonemapper | Undoes the Input Tonemapper on the game's image, applies the Output Tonemapper instead, then adds the GI on top. **Relationship:** Master conversion; output curve, exposure, matching, dither and curve-specific controls are inactive when conversion is off. |
| Input Tonemapper (the game's) | The curve the game used to turn its light into the image, undone first. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Output Tonemapper | The curve applied once the Input Tonemapper is undone. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Match Mid-Grey | Tonemappers put mid grey at very different brightness. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Keep Clipped Whites | Pixels that are white in the game's image stay at least that white after the conversion. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Light Rays Pick Up After Conversion | Rays pick up the image after the tonemapper conversion (on) or the game's original image (off). **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Output Dither (levels) | Noise of this many 8-bit levels added to the final colour while Convert Tonemapper is on. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Input Exposure (EV) | How many stops brighter than standard the game fed light into its curve. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Output Exposure (EV) | Stops added to the scene light before the Output Tonemapper, on top of Match Mid-Grey. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Output Contrast | Contrast around mid grey before the Output Tonemapper: above 1 darks get darker and brights brighter, below 1 the opposite. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Output Saturation | Colour saturation after the Output Tonemapper, around each pixel's own luminance. **Relationship:** Only applies to the selected output tonemapper when conversion is enabled; it does not modify the hit geometry. |
| Reinhard: White Point | Only used while the Output Tonemapper is Reinhard. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Hable: Shoulder Strength | Only used while the Output Tonemapper is Uncharted 2 / Hable. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Hable: Toe Strength | Only used while the Output Tonemapper is Uncharted 2 / Hable. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Hable: White Point | Only used while the Output Tonemapper is Uncharted 2 / Hable. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Gran Turismo 7: Shoulder (alpha) | Only used while the Output Tonemapper is Gran Turismo 7. How softly the highlights roll off (higher = a harder shoulder). **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Gran Turismo 7: Linear Section | Only used while the Output Tonemapper is Gran Turismo 7. Where the straight part ends and the shoulder starts, as a fraction of the peak. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Gran Turismo 7: Toe Strength | Only used while the Output Tonemapper is Gran Turismo 7. How much the shadows are pushed down (1 = none). **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Uchimura: Contrast | Only used while the Output Tonemapper is Uchimura. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Uchimura: Linear Section Start | Only used while the Output Tonemapper is Uchimura. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Uchimura: Linear Section Length | Only used while the Output Tonemapper is Uchimura. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| Uchimura: Toe (black tightness) | Only used while the Output Tonemapper is Uchimura. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |
| AgX: Look | Only used while the Output Tonemapper is AgX. Base is the plain curve, Punchy adds contrast and saturation, Golden adds a warm tint. **Relationship:** Only affects its named Output Tonemapper while Convert Tonemapper is enabled. |

### Multibounce

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Multibounce | Rays also pick up the GI that last frame put on the surface they hit, in that surface's colour, so light bounces more than once and fills rooms, corridors and caves. **Relationship:** Master switch for secondary bounce; disabling it makes its strength, cascade start, spread, surface match and estimated reflectance controls inactive. |
| Multibounce Strength | How much of last frame's GI a hit surface passes on (times its reflectance). **Relationship:** Controls history-based secondary bounce; Relit Pickup Bias can reduce its contribution to avoid counting the same light twice. |
| Multibounce From Cascade | The first cascade whose rays pick up multibounce (1 = all of them). |
| Bounce Reflectance (estimate) | How much light an average surface reflects, combined with each surface's own hue, for surfaces where the game's albedo is not available. **Relationship:** Fallback reflectance when real game albedo is unavailable; also used by multibounce. |
| Multibounce Spread | How wide the bounce is read around a hit. |
| Multibounce Surface Match | Weights the bounce read by depth and normal agreement with the hit surface, so the wall of a corner is lit by bounce and not by blur. **Relationship:** Depends on Multibounce and the secondary sampling path (including its compile-time divisor setting). |

### Bounce Shaping

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Bounce Contrast Curve | Evens out or widens the gap between weak and strong bounce, around Bounce Curve Pivot. **Relationship:** At a neutral curve, Bounce Curve Pivot and Bounce Curve Max Gain have no visible role. |
| Bounce Curve Pivot | The bounce level the contrast curve leaves unchanged. **Relationship:** Only relevant when Bounce Contrast Curve changes the bounce distribution. |
| Bounce Curve Max Gain | The most the contrast curve may multiply any bounce by. **Relationship:** Caps amplification from the contrast curve, particularly when lifting weak bounce. |

### Colors & Temperature

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Bounce Saturation | Colour saturation of the bounce light before it is added. |
| Blue Light Filter | Desaturates bounce light whose hue is near Blue Filter Hue (toward grey). **Relationship:** Master targeted colour suppression; its Hue and Hue Width controls do nothing without the filter. |
| Blue Filter Hue | The hue the blue filter targets, on the 0-1 colour wheel (0 red, 0.17 yellow, 0.33 green, 0.5 cyan, 0.67 blue, 0.83 magenta). **Relationship:** Only affects Blue Light Filter when that filter is active. |
| Blue Filter Hue Width | How far around its hue the blue filter reaches (0-0.5). **Relationship:** Only affects Blue Light Filter when that filter is active. |
| Orange Light Filter | Desaturates bounce light whose hue is near Orange Filter Hue. **Relationship:** Master targeted colour suppression; its Hue and Hue Width controls do nothing without the filter. |
| Orange Filter Hue | The hue the orange filter targets (0 red, 0.08 orange, 0.17 yellow). **Relationship:** Only affects Orange Light Filter when that filter is active. |
| Orange Filter Hue Width | How far around its hue the orange filter reaches (0-0.5). **Relationship:** Only affects Orange Light Filter when that filter is active. |
| GI Color Temperature | Shifts the GI warm (above 0) or cool (below 0). **Relationship:** Master warm/cool adjustment; Temperature Strength is inactive at neutral temperature. |
| Temperature Strength | How big the red / blue shift is at GI Color Temperature +1 or -1. Does nothing while GI Color Temperature is 0. **Relationship:** Only matters when GI Color Temperature is non-neutral. |

### Sky

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Sky Light | Adds a tintable sky ambient to the GI, gated by the cascades' own visibility: probe directions that escape upward carry sky light, blocked ones do not. **Relationship:** Master sky GI switch; most sky controls require it and nonzero Sky Light Strength. |
| Sky Color Source | Where the sky tint comes from. **Relationship:** Selects sampled, custom or blended sky colour; Blend: Custom Amount only affects blend mode. |
| Custom Sky Color | Custom sky colour. |
| Blend: Custom Amount | Only used by 'Blend Scene Sky + Custom'. **Relationship:** Only applies when Sky Color Source selects blending. |
| Sky Tint Saturation | Saturation of the sky tint. **Relationship:** Depends on Sky Light and Sky Light Strength. |
| Sky Light Strength | Brightness of the sky ambient added to the GI. 0 = off. **Relationship:** Sky contribution amount; zero disables the effect of the remaining sky-shaping controls. |
| Sky Occlusion Contrast | How hard the cascades' sky visibility cuts the sky light. **Relationship:** Depends on Sky Light and Sky Light Strength. |
| Sky Fully-Open Threshold | Sky visibility at or above this counts as fully open. **Relationship:** Depends on Sky Light and Sky Light Strength. |
| Sky Direction Start | Where a probe direction starts counting as sky-facing, by its upward component (-1 straight down, 0 horizon, 1 straight up). **Relationship:** Depends on Sky Light and Sky Light Strength. |
| Sky Direction Full | The upward component at which a direction counts fully as sky-facing. **Relationship:** Depends on Sky Light and Sky Light Strength. |
| Sky Screen-Edge Fade | Fades the sky light out within this fraction of the screen border, to hide that visibility is unknown past the edge. **Relationship:** Depends on Sky Light and Sky Light Strength. |
| Directional Sky Map | Builds a small map of the sky's colour across the screen, so the sky light carries the sky's gradient (a sunset side, a blue zenith) instead of one average tint. **Relationship:** Uses a spatial sky gradient; Sky Map Brightness Variation only matters when enabled. |
| Sky Map Brightness Variation | How much of the sky's brightness gradient (not only its colour) reaches the GI. 0 = colour only, 1 = brighter parts of the sky give proportionally more light. **Relationship:** Only applies to Directional Sky Map. |

### Off-Screen Canvas & Edges

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Screen Edge Extension | Rays that leave the screen beyond the canvas continue the nearest border pixel and pick up its light times this (cascades 1-3 only). **Relationship:** Fallback continuation at the edge beyond the canvas; not the same as storing previously visible light. |
| Off-Screen Canvas | Remembers the light and depth of what recently left the screen in a border around it (CRCGI_CANVAS_MARGIN percent per side), moved with the motion vectors. **Relationship:** Master remembered border data; Canvas Light Strength and Canvas Memory require it. |
| Canvas Light Strength | How much a ray takes from the off-screen canvas compared with an on-screen hit (also scaled by how much that canvas texel is still trusted, see Canvas Memory). **Relationship:** Scales remembered off-screen light; Canvas Memory only matters when the canvas is enabled and contributes. |
| Canvas Memory (per frame) | How long light that left the screen is remembered. **Relationship:** Controls trust/decay of stored border data, not the fallback Screen Edge Extension. |

### Range & Falloff

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Max GI Distance (linear depth) | Linear depth where the GI has fully faded out, pixels farther than this get none (look at Debug View > Linear Depth to see where your scene falls). **Relationship:** Receiver-distance cutoff; Distance Fade Width shapes the approach to it, while Hit Distance Falloff acts on source-to-hit travel. |
| Distance Fade Width | Width of the fade before Max GI Distance, as a fraction of it. **Relationship:** Shapes the receiver cutoff defined by Max GI Distance. |
| Fade With Receiver Distance | Weakens the GI of surfaces far from the camera, smoothly with depth. **Relationship:** Attenuates distant receiving surfaces; separate from Hit Distance Falloff, which attenuates long ray travel. |
| Hit Distance Falloff | Weakens light that a ray picks up far away, measured against the farthest cascade's reach, so it looks the same at every resolution. **Relationship:** Attenuates distant light pickup; separate from receiver depth fading and cascade reach. |

### Shadow

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Shadow Amount | How much darker places that are more enclosed than their surroundings get (crevices, joints, corners, the foot of an object); below 0 they get more bounce light instead. **Relationship:** 0 skips every shadow pass; above 0 the game's image is darkened at 65% of the factor before the GI is added, at 0 and below it is untouched. It is the old Crevice Light Bounce Bias with the sign turned round (presets are converted). |
| Shadow Contact Bias | Where the shadow comes from: 0 = the escape visibility of the two nearest cascades only (denoised with the GI, the contact passes are skipped), 1 = the contact march only (at the effect resolution by default, `CRCGI_CONTACT_DIVISOR`), in between they overlap. **Relationship:** does nothing while Shadow Amount is 0. |
| Shadow Contact Range (texels) | How far the contact march looks from every probe, and the size of the area it compares with, in effect texels (so a preset looks the same at every screen resolution). **Relationship:** does nothing while Shadow Contact Bias or Shadow Amount is 0. |
| Shadow Contact Depth Bias (texels) | How far behind the depth buffer a contact ray must be to count as blocked. **Relationship:** does nothing while Shadow Contact Bias or Shadow Amount is 0. |
| Surface Albedo Response | Makes the GI follow the surface's own colour. |

### Temporal Accumulation

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Temporal Accumulation | Blends the GI (lighting only, never the game image) with previous frames, moved with the motion and rejected where depth or light disagree. **Relationship:** Master history switch; disabling it makes the history guards, accumulation counts, temporal jitter, and far-cascade accumulation controls inactive. |
| Frames Accumulated | About how many frames the GI history represents. **Relationship:** Sets the history window; with essentially no history, far-cascade history controls offer little benefit. |
| Light Change Guard | Drops the history where the light changed. **Relationship:** Rejects history on lighting changes; separate from Depth Change Guard (geometry) and Motion Mismatch Rejection (estimated motion). |
| Depth Change Guard | Drops the history where the surface moved: a relative depth difference of this value gives no blend. **Relationship:** Rejects history on surface changes; separate from Light Change Guard and Motion Mismatch Rejection. |
| Motion Mismatch Rejection | How much the history is distrusted where the estimated motion could not find a good match (fast motion, moving objects). **Relationship:** Primarily checks the shader's estimated motion; not needed when valid game motion vectors replace that estimate. |
| Ray Jitter Accumulation | Rotates every ray's start position and direction a little each frame and lets the history average them, so thin or sparse geometry converges instead of sliding through a fixed pattern when the camera moves. **Relationship:** Near/global temporal jitter; on far cascades, the larger of this and Far Cascades: Jitter is used. |
| Far Cascades: Accumulate From Cascade | Cascades from this one onwards (1 = nearest) keep their own history, are jittered every frame, and accumulate more the farther they are. **Relationship:** Depends on Temporal Accumulation and meaningful history; affects far cascades rather than the nearest cascade. |
| Far Cascades: Accumulation Growth | Each cascade from Far Cascades: Accumulate From Cascade onwards accumulates Frames Accumulated times this, once more for every step past the start cascade, capped at 64 frames. **Relationship:** Depends on Temporal Accumulation and meaningful history; affects far cascades rather than the nearest cascade. |
| Far Cascades: Jitter | Moves the far cascades' probes within their tile, and their rays within their direction cell, a little every frame, so the accumulated history covers the whole tile instead of one fixed point. **Relationship:** Far-only jitter; redundant where Ray Jitter Accumulation is already stronger. |
| Far Cascades: Half-Rate Update (faster) | Performance. **Relationship:** Depends on Temporal Accumulation and meaningful history; affects far cascades rather than the nearest cascade. |

### Denoise & Upsample

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Denoise Mode | Runs at the effect's own resolution on the temporally accumulated light, before the upsample to full resolution. **Relationship:** Master filter selection; when Off, the other denoise controls are inactive and the escape shadow is not filtered either. |
| Denoise Iterations | Number of edge-aware a-trous passes (5 x 5 taps each, the spacing doubling every pass). **Relationship:** Only applies while Denoise Mode is Edge-Aware; reach about 6 / 30 / 62 texels at 2 / 4 / 5 passes and Tap Spacing 1. |
| Denoise Blur Strength | How much brightness difference the filter averages across (the luminance edge stop). **Relationship:** Depth and normal edges are never crossed at any value; scales the noise-based edge stop of Variance Guided Denoise. |
| Denoise Tap Spacing (texels) | Scales the tap spacing of every denoise pass (1 = contiguous taps in the first pass, doubling per pass). **Relationship:** Only applies when an active Denoise Mode uses this control. |
| Variance Guided Denoise | Lets the measured noise of each pixel (from its accumulated history, or its neighbourhood while it is new) steer the filter: noisy pixels blur widely, clean ones stay sharp, so real lighting edges (a shadow boundary, a bright patch) survive. |
| Denoise Distance Shrink | Makes the blur smaller on distant surfaces (fewer pixels cover them, so a smaller kernel is enough). **Relationship:** Only applies when an active Denoise Mode uses this control. |
| Denoise Edge Test | How a tap is judged to be on the same surface as the pixel. **Relationship:** Only applies when an active Denoise Mode uses this control. |
| Denoise Edge Sharpness | Strength of the edge test chosen in Denoise Edge Test. **Relationship:** Only applies when an active Denoise Mode uses this control. |
| Denoise Normal Power | Taps count by their normal agreement raised to this. **Relationship:** Only applies when an active Denoise Mode uses this control. |
| GI Upsample | How the denoised GI reaches full resolution. **Relationship:** Bilinear bypasses edge-aware controls; edge-aware modes use Upsample Edge Sharpness and Upsample Normal Power. |
| Upsample Edge Sharpness | How strictly the edge-aware upsample sticks to the pixel's own surface. **Relationship:** Only applies to an edge-aware GI Upsample mode. |
| Upsample Normal Power | Upsample taps count by their normal agreement raised to this. **Relationship:** Only applies to an edge-aware GI Upsample mode. |

### Expert: Light Pickup

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Pre-Filter Gamma | Gamma on the picked-up light. |
| Pickup Blur (texels) | The image rays pick light from is softened once per frame over this radius, in effect texels. |
| Surface Bounce Threshold | The surface brightness at which a surface counts for half as a source of bounce light. |
| Emitter Threshold | Brightness above which a surface counts as a light source (an emitter) and gets Emitter Boost. |
| Emitter Sensitivity (transition sharpness) | How abrupt the step from ordinary surface to emitter is around Emitter Threshold. |
| Emitter Boost | Multiplier on the light of emitters. |
| Pickup Brightness Cap (fireflies) | The most light one surface may hand to a ray, before Emitter Boost and HDR Light Pickup multiply it. |
| Dark Surfaces Block Rays | Makes dark surfaces shadow casters: the darker a surface, the thicker its hit band, so rays that pass behind a dark object (armour, rock faces, shadowed bark) are blocked by it instead of picking up the bright light behind it. |
| Relit Pickup Bias (geometry + lights + bounce) | The game's image is lit by the game (its baked ambient, shadow maps and light probes). **Relationship:** Depends on Relit Pickup Bias; real game albedo can bypass estimated-albedo detail. |
| Relit Reflectance | How much of the light that reaches a relit surface it hands on. **Relationship:** Depends on Relit Pickup Bias; real game albedo can bypass estimated-albedo detail. |
| Relit Albedo Detail | How much of a pixel's reflectance detail is kept (its brightness relative to the average around it, which divides out slow baked lighting). **Relationship:** Depends on Relit Pickup Bias; real game albedo can bypass estimated-albedo detail. |
| Relit Detail Radius (cascade texels) | How far around a pixel the baked lighting is assumed to be constant, in effect texels. **Relationship:** Depends on Relit Pickup Bias; real game albedo can bypass estimated-albedo detail. |
| Relit Highlight Keep | Bright surfaces are mostly directly lit (sun, lamps), and direct light is in the image but not in the GI, so they keep their image colour instead of being relit. **Relationship:** Depends on Relit Pickup Bias; real game albedo can bypass estimated-albedo detail. |

### Expert: Radiance Cascades

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Rays Per Cell (quality) | Each probe direction traces this many stratified sub-rays and averages them. **Relationship:** 3 rays sit on an even lattice centred on the cell; with Ray Jitter Accumulation the pattern moves around the cell every frame so the whole cell is covered. |
| Ray Steps: Cascades 1-2 (near) | Samples per ray in cascades 1 and 2 (the short ones). **Relationship:** Near-cascade traversal budget; distinct from ray count per cell and far-cascade steps. |
| Ray Steps: Cascades 3+ (far) | Samples per ray in cascades 3 and up (the long ones). **Relationship:** Far-cascade traversal budget; distinct from ray count per cell and near-cascade steps. |
| Cascade Reach (screen diagonals) | How far the farthest cascade reaches, as a fraction of the screen diagonal (so it is resolution independent). **Relationship:** Sets far reach; Reach Applies From Cascade determines which cascade intervals are stretched. |
| Reach Applies From Cascade | The first cascade (1 = nearest) that Cascade Reach stretches. **Relationship:** Only shapes how Cascade Reach is distributed across cascade intervals. |
| Cascade Cross-Fade | Each cascade's rays continue this fraction into the next cascade's interval and fade out there while the next one fades in. |
| Adaptive Ray Count | Uses the temporal history to decide how many rays a probe needs. **Relationship:** Requires temporal history; the decision follows the geometry only (depth, normal and age of the history), not the motion match. |
| Ray Start Jitter (draw distance vs noise) | Each ray's first sample sits at a fixed per-ray offset, so neighbouring rays probe different spots and thin or sparse far geometry is found on average. |
| Radiance Cone Blur | Rays sample a blurred copy of the light at a level that matches how wide their cone is at the hit. **Relationship:** Controls cone-dependent light filtering; Max Cone Blur Mip limits its blur level. |
| Max Cone Blur Mip | The blurriest level Radiance Cone Blur may use. **Relationship:** Only constrains active Radiance Cone Blur. |

### Expert: Merge & Gather

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Gather Smoothing (quality) | How the final per-pixel GI reads the cascade probes. **Relationship:** Adjust interpolation and visibility after hits; do not use it as a substitute for correct hit detection. |
| Bilinear Direction Merge (quality) | When a cascade is merged into the one below, the parent's light for a direction is read with bilinear filtering between neighbouring direction cells instead of snapping to one cell. **Relationship:** Adjust interpolation and visibility after hits; do not use it as a substitute for correct hit detection. |
| Probe Edge Sharpness | How strictly probes interpolate only with probes on the same surface (used by the gather and by cascade merging). **Relationship:** Controls cross-surface interpolation for gather and merge; Probe Edge Weighting selects the metric. |
| Probe Edge Weighting | How the merge, gather and structure lookup decide that a neighbouring probe lies on the same surface. **Relationship:** Selects relative-depth/normal or plane-distance matching; Probe Normal Agreement Power only applies to the former. |
| Probe Normal Agreement Power | Only used with Relative depth + normal weighting. **Relationship:** Only used with relative-depth/normal Probe Edge Weighting. |
| Merge Sharpness Falloff | Makes the merge blend more freely in the coarse far cascades than in the fine near ones: Probe Edge Sharpness is divided by 1 plus this times the cascade number. **Relationship:** Relaxes Probe Edge Sharpness in coarser cascades; does not replace the base sharpness control. |
| Merge Horizon Cutoff | A parent probe is skipped for a direction that lies closer than this to its horizon. **Relationship:** Adjust interpolation and visibility after hits; do not use it as a substitute for correct hit detection. |
| Energy Carry (per cascade step) | How much of the light a farther cascade hands down survives the merge. **Relationship:** Scales far-cascade energy passed down during merge; Energy Carry From Cascade chooses where it begins. |
| Energy Carry From Cascade | Energy Carry applies to the light handed down by this cascade and every farther one (1 = nearest). **Relationship:** Only determines which merges receive Energy Carry. |
| Merge Visibility Check | When a cascade is merged into the one below, the straight line from the child probe to each parent probe is checked against the depth buffer (3 taps). **Relationship:** Rejects occluded parent-probe transfers; Merge Visibility Tolerance adjusts its blocker sensitivity. |
| Merge Visibility Tolerance | How far, relative to its depth, a surface must stick out in front of the line between two probes to count as blocking it. **Relationship:** Only relevant while Merge Visibility Check is active. |

### Expert: Occlusion & Depth Separation

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Depth Separation Limit | A surface only receives bounce from something close to it in depth, measured relative to distance. **Relationship:** Post-hit rejection of light transferred across unrelated depth layers, not a hit test; at its unrestricted limit, Softness and relaxation controls have little to do. |
| Depth Separation Softness | How gradually the bounce fades out before the Depth Separation Limit. **Relationship:** Shapes the cutoff set by Depth Separation Limit; cannot restore light already rejected by hit detection. |
| Foreground-to-Background Block | Extra strictness when the light comes from something closer to the camera than the surface receiving it. **Relationship:** Additional depth-separation restriction for foreground sources lighting backgrounds; depends on the depth-separation stage. |
| Far Cascade Separation Relax | Light that travels far across the screen usually comes from a surface at a different depth (the far end of a street or track), so the Depth Separation Limit rejects far hits much more often than near ones and the far cascades lose energy. **Relationship:** Relaxes depth-separation filtering for distant cascade light; does not extend Cascade Reach or repair missed hits. |
| Occlusion Tolerance | A surface can only block a ray for a receiver if it is nearer than the receiver by at least this much (linear depth), so the receiver's own plane and small bumps never shadow it. **Relationship:** Rejects self-shadowing by requiring separation between blocker and receiver; works jointly with Occluder Depth Range and Occlusion Depth Threshold. |
| Occluder Depth Range | How far in front of the receiver a blocker may be, relative to the receiver's depth. **Relationship:** Rejects blockers too far in front of the receiver, such as a foreground character shadowing a distant wall; works jointly with Occlusion Tolerance. |
| Relative Depth Tolerances (long draw distance) | Fixes depth checks on games with a very long far plane (racing, flight, open worlds). **Relationship:** Scales depth tolerances by distance; Tolerance Reference Depth has no effect when this is disabled. |
| Tolerance Reference Depth | The linear depth at which the depth checks keep exactly the value on their own sliders while Relative Depth Tolerances is above 0. Nearer surfaces get proportionally smaller tolerances, farther ones larger. **Relationship:** Only used when Relative Depth Tolerances is active. |

### Expert: Hit Detection

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Hit Thickness Mode | How thick the shader assumes each visible surface is. **Relationship:** Generic bypasses the silhouette search, scale and gap controls; silhouette-derived adapts the hit band to different object sizes. |
| Thickness Search Range | Only used with Silhouette-derived thickness. **Relationship:** Only works in silhouette-derived mode; changes the neighbourhood used to estimate object thickness. |
| Thickness Silhouette Gap | Only used with Silhouette-derived thickness. **Relationship:** Only works in silhouette-derived mode; sets what depth discontinuity qualifies as a silhouette. |
| Object Thickness Scale | Only used with Silhouette-derived thickness. **Relationship:** Only applies in silhouette-derived mode; increasing it stops helping once Object Thickness Cap is reached. |
| Object Thickness Cap | The thickness a surface is given, as a fraction of its own depth. **Relationship:** Limits silhouette-derived thickness after Object Thickness Scale; Max Hit Thickness can still impose a stricter overall ceiling. |
| Hit Front Tolerance | How far IN FRONT of a surface, as a fraction of its hit band, a ray may stop and still count as a hit. **Relationship:** Use with the hit-band and depth-model controls; stronger rejection can hide valid bounce as well as leaks. |
| Max Hit Thickness (fraction of depth) | The most any hit band may be thick, as a fraction of the surface's own depth, whatever it is made of (object thickness, the ray's step, the surface slope). **Relationship:** Final safety ceiling for the entire hit band, including step and slope contributions; can override Object Thickness Cap. |
| Occlusion Depth Threshold | Decides whether a ray that has gone behind a nearer surface is blocked by it (a shadow) or just passes behind it (a thin object). **Relationship:** A ray must pass behind a surface by both the hit band and this threshold; the stricter condition wins. Distinct from Occlusion Tolerance, which compares blocker and receiver. |
| Facing Check Intensity | How much a surface that faces away from the ray is dimmed. **Relationship:** Master control for facing rejection; when disabled, Facing Check Steepness and Facing Check Bias have no effect. |
| Facing Check Steepness | How sharply a hit switches between front-facing and back-facing. **Relationship:** Only shapes the facing check while Facing Check Intensity is active. |
| Facing Check Bias | How much light a surface seen exactly edge-on still gives (the added term of the facing weight, see Facing Check Steepness). **Relationship:** Only shapes the facing check while Facing Check Intensity is active; preserves grazing-angle contributions. |
| Ray Depth Model | Where in depth a ray is when it reaches a point on screen. **Relationship:** Selects either Perspective Ray Lift or Ray Depth Slope; Ray Depth Drift Limit applies to both. |
| Perspective Ray Lift (texels) | Only used with Perspective-correct. **Relationship:** Only applies to the perspective-correct depth model; Ray Depth Slope is inactive in that mode. |
| Ray Depth Slope (per texel) | Only used with the Slope heuristic. **Relationship:** Only applies to the slope heuristic; Perspective Ray Lift is inactive in that mode. |
| Ray Depth Drift Limit | Caps how far a ray's expected depth may drift from the surface it started on, however far it travels, so long rays cannot leave the depth range of everything on screen. **Relationship:** Safety limit shared by both ray-depth models, not a substitute for hit thickness. |

### Expert: Geometry Reconstruction

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Silhouette Angle (degrees) | A depth step between neighbouring pixels that is steeper than a surface this close to edge-on counts as a silhouette, not a slope. **Relationship:** Affects reconstructed geometry; valid game normals can bypass much of this path. |

### Debug

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| Debug View | Replaces the picture with an internal buffer, to check one stage at a time. **Relationship:** Diagnostic only; does not correct the underlying lighting. |
| Debug Gain | Brightness multiplier for the GI-type debug views and the single-cascade view. **Relationship:** Diagnostic only; does not correct the underlying lighting. |
| Show Single Cascade | Shows ONE cascade on its own interval, with nothing merged in from the farther cascades (1 = nearest). **Relationship:** Diagnostic only; does not correct the underlying lighting. |
| Cascade Debug Shows | What the single-cascade view shows. **Relationship:** Diagnostic only; does not correct the underlying lighting. |

### Debug: G-Buffer & Canvas

| Slider / control | Meaning, intended fix and relationships |
|---|---|
| G-Buffer / Canvas View | Shows what the game hands to the shader, and the off-screen canvas. **Relationship:** Diagnostic only; use it to identify missing/incorrect feeds and canvas data. |


## Informal Note

This is intended as an efficient way of using Radiance Cascade to trace rays, and was built out of passion and a need for such techniques to exist. I didn't know HLSL, but I could design a theoretical pipeline so Claude was used as a replacement. Reshade is s great platform to build these shaders on and being able to iterate, use trial and error to find out problems, and many other time effective ways of engineering. I liked modding games, and this specific project was intended, from the start, as a replacement for The Witcher 3 Remastered's lackluster overall GI system which can only be improved via Ray Tracing, which for a lot of us specifically in that game, leads to aggressive use of upscaling techniques which can diminish overall image quality. I would not use this shader in games such as Red Dead Redemption 2, Assassin's Creed Unity, or many other games with impressive baked or dynamic lighting systems because it is not needed in those games to make them look and perform really good on today's systems. V-Ram in this technique is basically irrelevant and default rendering resolution doesn't need to be much high because diffuse GI is very soft and subtle anyways, and Radiance Cascades are "noiseless" by design (they still can inhibit noise, but not the usual "grain" associated with noise). It is also more friendly to a screen space implementation compared to randomly traced rays, and many compensation techniques are used to make it actually usable and playable. This is not a screenshot tool. It has many options to be customized, and every parameter can be customized. Presets for this shader must be made to look playable, pleasant and overall, improve the atmosphere and augment the art style, not alter it. The main test bed of this project was The Witcher 3 Remastered, to improve lighting and mood without using Ray Tracing. My personal preset file is included for each release, and it is using mods (Promotional Atmosphere Lighting Mod specifically, alongside some other small stuff) as a base. That mod already improves the borked lighting in some areas of the game, and alters how the game looks to be more similar to the coveted pre release material. I think it's an immaculate mod, and it deserves a shout-out. I sincerely hope you enjoy using it as an augmentation to face-lift some of your favorite games. This project is born out of the love of the game (literally), the modding scene, and is inspired by all the greatly advanced effects that other, legendary developers have made, with their bare hands, wits and skill.

## Licence (author’s terms)

You can use any part of this code for your own effects. For example, the geometry reconstruction code, temporal accumulation code, or any other part of this pipeline in your own shaders, effects, or even adapt it to your own game engine/project if it's feasible. For example, if you feel like a part of this pipeline can be used to advance a project, you got it, and you can iterate on it. I could design a pipeline around screen space limitations, but I am not a shader developer. I can use tools, and I use them well. I can research, engineer and hypothesize, do trial and error, and have some limited experience in coding. I can find solution, but your iterations on individual systems will be welcome as long as they do not directly copy the pipeline, modify it a bit, and re-released as your own. The shader and add-on are also written using free tier, Sonnet 5.5 model, with some bigger iterations coming from friends who had premium accounts and had access to Opus 5.5. I would appreciate if you use this open source project, as an actual open source project. Full implementations are permitted as long as it's as an inspiration for an in-engine implementation, but using the whole pipeline as a basis for a Diffuse GI Reshade effect, is not "cool" as one would put it. You can also code your shader to use normals, albedo, motion vectors, etc via the add-on. Full implementation of this add-on as a complementary inclusion to your own release as a compiled file is permitted, strictly as a bundle, but the add-on source files should not be included in your GitHub repository directly, only the compiled add-on itself. Shaders that want to use the add-on can code their shaders to import data from add-on hooks, so this part of the project also should be used to aid other projects. Usage of add-on code itself is also conditional on your usage, it's the same as the shader. For example, you are permitted to use the GPU Profiler code itself to develop an independent tool that is not directly a shader data provider for game normals, albedo, and other features of this add-on directly. Non of these words mean I will pursue, claim copyright or anything of this sort to keep the entire thing exclusively mine, but this licence is based on honor, not exclusivity. I did not use any licence template, as you can obviously see. This is my license, and the few limitations written above will expire the day I decide I will not develop this project any further.
