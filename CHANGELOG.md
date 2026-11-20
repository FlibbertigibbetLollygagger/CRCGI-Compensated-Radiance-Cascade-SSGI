# Changelog

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
