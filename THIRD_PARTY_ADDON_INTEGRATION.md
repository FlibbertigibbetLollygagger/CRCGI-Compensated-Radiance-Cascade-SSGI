# What the add-on talks to (for other shaders)

Everything below is already in `shader/CRCGI_v2.fx`. This page lists the textures and settings the CRCGI Injector binds or writes,
for anyone who wants to read the same data from another effect.

## Textures (bound by the add-on, readable by any effect)

| Semantic | Holds | Bound when |
|---|---|---|
| `NORMAL` | the game's normals (decoded per `Integration_NormalMode`) | level C, normals feed delivered |
| `CRCGI_ALBEDO` | base colour (sRGB view if the game wrote it through one, so samples are linear) | level C, albedo feed delivered |
| `CRCGI_MOTION` | motion vectors (units per `Integration_MotionUnits`) | level C, motion feed delivered |
| `CRCGI_MATERIAL` | the game's material texture (metalness or specular in the channel per `Integration_MaterialChannel`) | level C, material feed delivered |
| `CRCGI_HDRSCENE` | the game's lighting buffer before tonemapping (linear, game units; the shader matches its exposure) | level C, HDR feed delivered |
| `CRCGI_SCENE` | the finished frame: the image where the game's 3D rendering is done (before ui / fog), same size and format family as the back buffer | level C, finished-frame feed delivered (this frame) |
| `DEPTH` | the depth buffer in use at the forced layer | level B / C with Force render (advanced) |

## Settings (category "Game Integration" and the camera)

| Name | Visible | Written by the add-on |
|---|---|---|
| `UseNormalTexture`, `Integration_Albedo`, `Integration_Motion`, `Integration_Material`, `Integration_HDRScene`, `Integration_Scene` | yes | only with "Drive the shader's switch" ticked for that feed (off by default): set where the texture is bound, on after 3 frames in a row with the feed delivered, off in the first frame without it (never on while its texture is unbound) |
| `Integration_NormalSpace` (0 view, 1 world / any, auto-aligned) | yes | with the normals switch |
| `Integration_GeometrySource` (0 rebuild, 1 game where valid, 2 game only) | yes | with the normals switch (add-on G-Buffer tab) |
| `Integration_NormalFitSmoothing` | yes | no |
| `Integration_NormalMode` (0 RGB 0..1, 1 octahedral, 2 RGB signed), `Integration_NormalFlipY` | hidden | always at level C |
| `Integration_MotionUnits` (0 UV, 1 pixels, 2 NDC), `Integration_MotionFlipY`, `Integration_MotionInvert` | hidden | always at level C |
| `Integration_MaterialChannel` (0 R, 1 G, 2 B, 3 A), `Integration_MaterialMeaning` (0 metalness, 1 specular, 2 roughness, 3 smoothness) | hidden | always at level C |
| `FieldOfView`, `Integration_Near`, `Integration_Far`, `Integration_Reversed` | `FieldOfView` yes | level B / C with a detected or picked camera |

`ConvertTonemapper` is never written.

## Passes

`TM` (tonemapper state, and the HDR scene light's exposure match), `Sky`, `SkyMap`, `Luma`, `Flow3..0` (no work with game motion, or when nothing uses the motion),
`NormalFit` / `NormalRot` (world-space normal alignment), `Prep` (geometry, thin features, pickup), `MBMap`, `Pyr1..3`, `Canvas` (with confidence),
`LumStats`, `ProbeTable0..5` (one per cascade), `Cascade5..0` (`Cascade1` also writes the near escape visibility, `Cascade0` the near visibility
and the history of the accumulated cascades), `ProbeAvg0`, `Gather`, `TemporalRaw` (accumulation, moments, history age), `Atrous0..4`
(`Atrous4` also writes the history for the next frame), `CopyCanvas`, `CopySkyMap`, `CopyNormalRot`, `Contact`, `ContactBH`, `ContactBV`
(at `CRCGI_CONTACT_DIVISOR`, by default the effect resolution; skipped while Shadow Contact Bias or Shadow Amount is 0), `Composite`.

## Debug: G-Buffer & Canvas (`GBufferDebugView`)

0 off, 1 overview, 2 albedo, 3 normals as used, 4 normals from depth, 5 game vs depth difference, 6 motion, 7 whole off-screen canvas,
8 off-screen light only, 9 auto-align status, 10 game HDR scene light (exposure-matched), 11 game material (diffuse share).

## Debug View (`DebugView`)

0 off, 1 GI only, 2 bounce only, 3 escape visibility, 4 enclosure, 5 normals, 6 hit radiance buffer, 7 cascade 3 radiance, 8 linear depth,
9 object thickness, 10 GI before denoise, 11 shadow, 12 motion flow, 13 emitters.

## The finished frame

`CRCGI_SCENE` is a copy of a render target the game finished drawing the 3D scene into. With `Integration_Scene` on, the CRCGI shader
reads it instead of the back buffer for everything it analyses, and adds its light to the final back buffer through
`final + keep * (result - scene)`, where `keep` falls to 0 for pixels where the back buffer no longer matches the copy (ui, opaque
overlays) and stays high for soft changes (fog). Another effect can use the same semantic to stay away from the ui in the same way.

## When the textures are captured

Normals, albedo, motion vectors and material are copied at the end of a write run: consecutive binding intervals with the feed's target
layout (target count, formats, sizes relative to the screen, depth bound) that draw into the same texture, so the copy is the complete
G-buffer whatever the camera sees and whichever texture of a swapped pair is bound this frame. The HDR scene light is the interval that
draws most into its texture, the finished frame the first capture of the frame at its layer. A feed is bound at the end of the frame
while its latest capture is recent enough: albedo and material 8 frames, normals and the HDR scene light 3, motion vectors 1, the finished
frame only this frame.
