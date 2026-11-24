Update v2: Added "Batman Arkham Knight - CRCGI v2 bright.ini", the v1.5 bright preset on the v2 shader (use it with CRCGI_v2.fx). The values you tuned are kept; values the preset left at the old defaults, and the denoise, jitter and adaptive-ray settings (their meaning changed with the new filtering stack), take the v2 defaults, which are the v1.6 tuning. Crevice Light Bounce Bias is now Shadow Amount with the sign turned round (the value is converted, more = darker), and the new Shadow Contact Bias is 0.5, so the denoised escape shadow of the cascades and the contact march overlap. Move Shadow Contact Bias to 0 for the softest and cheapest shadow (the full-resolution contact passes are skipped). The bounce shaping (saturation, colour filters) no longer feeds back into the multibounce, so single-coloured rooms (the red cabaret, orange palace interiors) no longer run away. The v2 add-on can also hand over the game's material (metalness) texture, and it picks the CRCGI technique again by itself when the saved one (in crcgi_injector.ini, an older file) is not loaded.

Update v1.5: Added "Batman Arkham Knight - CRCGI v1.5 bright.ini", the v1.1 values on the v1.5 shader (use it with CRCGI_v1.5.fx). The v1.2 preset is regenerated as a complete file (same values as v1.1, it was missing a setting and was not in the form ReShade saves). The v1.5 add-on follows the G-buffer by its layout instead of its memory address and captures it once the pass is complete, so albedo and normals should not drop out when the camera turns any more. Feeds assigned from a layer with an older add-on need to be assigned again once. Contact shadowing note: these presets use a negative Crevice Light Bounce Bias, and in v1.5 that also darkens the game's own image in crevices (65% of the contact factor) and takes the cascades' visibility into account, so crevices and corners are darker than on older versions. The values are untouched, move the bias towards 0 if it's too much for you.

Update v1.2: A CRCGI v1.2 copy of this preset is included (same values, only the section and technique names point at CRCGI_v1.2.fx, made with tools/retarget_preset.py). Settings keep their names between versions, so any older CRCGI preset can be retargeted the same way. Also, the injector file is obsolete but you can use it. It still loses albedo and other injection layers tho

I do not know if the preset is playable, it is calibrated over an afternoon as an experiment.
Download v1 zip for the add-on, use shaders v1.1 zip for the shader
Turn on performance mode

First integration with Albedo, HDR and Normals (Do not use any other integration, it is calibrated with these in mind.)
DX11 is reliable to track
Slope model is used instead of perspective correct due to frequent FoV changes during gameplay.
Adaptive ray system is off, because the games lighting is predictable.
Add-on might misbehave, the attached add-on config file is only there for reference. It may work or you might need to manually adjust it. For me, these work flawlessly.
Green is the only color that could look overdone so I muted it a bit.
No graphics mod is used aside from DLAA mod. (Rehsade on dxgi.dll, DLAA mod on d3d11.dll)
The constant cost of a Radiance Cascade system is leveraged in this preset. On a 3060/4060 it should take between 3.5ms to 5ms to render. Achieving a locked 60fps target is easy with performance mode on
I tried to not overdo brightness and color saturation.
It is tested while flying, walking on street level, and on rooftops, interiors are not properly tested but I think very brightly lit streets in this game are a good substitute.
Neons are a bit brightened by me, because I like it when neons look bright.
The games own anti-aliasing sucks so it is recommended to use the DLAA mod I mentioned.
If you think lampposts increase enviroment brightness too much, decrease overall intensity but I think it looks good. Lampposts in the game tend to be bunched up anyways so brightness increase is expected.

I know I said it is not meant to be used/is not recommended to be used in games with good baked lighting but I thought a dynamic system to complement the already really good baked enviroment lighting would look good, and I wanted a testbed for DX11 integration anyways.
