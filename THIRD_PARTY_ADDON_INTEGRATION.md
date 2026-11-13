# What the add-on talks to (for other shaders)

Everything below is already in `shader/CRCGI_v1_4C.fx`. This page lists the textures and settings the CRCGI Injector binds or writes,
for anyone who wants to read the same data from another effect.

## Textures (bound by the add-on, readable by any effect)

| Semantic | Holds | Bound when |
|---|---|---|
| `NORMAL` | the game's normals (decoded per `Integration_NormalMode`) | level C, normals feed delivered |
| `CRCGI_ALBEDO` | base colour (sRGB view if the game wrote it through one, so samples are linear) | level C, albedo feed delivered |
| `CRCGI_MOTION` | motion vectors (units per `Integration_MotionUnits`) | level C, motion feed delivered |
| `CRCGI_HDRSCENE` | the game's lighting buffer before tonemapping (linear, game units; the shader matches its exposure) | level C, HDR feed delivered |
| `DEPTH` | the depth buffer in use at the injection layer | level B / C with an in-frame injection |

## Settings (category "Game Integration" and the camera)

| Name | Visible | Written by the add-on |
|---|---|---|
| `UseNormalTexture`, `Integration_Albedo`, `Integration_Motion`, `Integration_HDRScene` | yes | only with "Drive the shader's switch" ticked for that feed (off by default): on exactly while the feed is delivered |
| `Integration_NormalSpace` (0 view, 1 world / any, auto-aligned) | yes | with the normals switch |
| `Integration_GeometrySource` (0 rebuild, 1 game where valid, 2 game only) | yes | with the normals switch (add-on G-Buffer tab) |
| `Integration_NormalFitSmoothing` | yes | no |
| `Integration_NormalMode` (0 RGB 0..1, 1 octahedral, 2 RGB signed), `Integration_NormalFlipY` | hidden | always at level C |
| `Integration_MotionUnits` (0 UV, 1 pixels, 2 NDC), `Integration_MotionFlipY`, `Integration_MotionInvert` | hidden | always at level C |
| `FieldOfView`, `Integration_Near`, `Integration_Far`, `Integration_Reversed` | `FieldOfView` yes | level B / C with a detected or picked camera |

`ConvertTonemapper` is never written.

## Passes

`TM` (tonemapper state, and the HDR scene light's exposure match), `Sky`, `SkyMap`, `Luma`, `Flow3..0` (no work with game motion),
`NormalFit` / `NormalRot` (world-space normal alignment), `Prep` (geometry + pickup), `MBMap`, `Pyr1..3`, `Canvas` (with confidence),
`LumStats`, `Cascade5..0`, `ProbeAvg0`, `Gather`, `DenoiseH/V`, `CopyPrev`, `CopyCanvas`, `CopySkyMap`, `CopyNormalRot`, `CopyHC1..5`
(history of the accumulated cascades), `Contact` (full resolution), `Composite`.

## Debug: G-Buffer & Canvas (`GBufferDebugView`)

0 off, 1 overview, 2 albedo, 3 normals as used, 4 normals from depth, 5 game vs depth difference, 6 motion, 7 whole off-screen canvas,
8 off-screen light only, 9 auto-align status, 10 game HDR scene light (exposure-matched).

## Debug View (`DebugView`)

0 off, 1 GI only, 2 bounce only, 3 escape visibility, 4 enclosure, 5 normals, 6 hit radiance buffer, 7 cascade 3 radiance, 8 linear depth,
9 object thickness, 10 GI before denoise, 11 contact occlusion, 12 motion flow, 13 emitters.
