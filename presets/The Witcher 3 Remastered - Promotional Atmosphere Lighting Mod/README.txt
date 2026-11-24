Update v2: Two presets for v2, "CRCGI v2 bright" (from v1.5 bright, the v1.1 tuning) and "CRCGI v2 subtle" (from v1.5 subtle, the Better Changes Vanilla Lighting Mod tuning). The values you tuned are kept; values the presets left at the old defaults, and the denoise, jitter and adaptive-ray settings (their meaning changed with the new filtering stack), take the v2 defaults, which are the v1.6 tuning. Crevice Light Bounce Bias is now Shadow Amount with the sign turned round (the value is converted, more = darker), and the new Shadow Contact Bias is 0.5, so the denoised escape shadow of the cascades and the contact march overlap. Move Shadow Contact Bias to 0 for the softest and cheapest shadow (the full-resolution contact passes are skipped). The bounce shaping (saturation, colour filters) no longer feeds back into the multibounce, so single-coloured rooms (the red cabaret, orange palace interiors) no longer run away.

Update v1.5: Two presets for v1.5. "CRCGI v1.5 bright" is the v1.1 tuning of this preset, "CRCGI v1.5 subtle" is the Better Changes Vanilla Lighting Mod tuning (subtler, the newer one). The v1.2 preset is regenerated as a complete file (same values as v1.1). Contact shadowing note: these presets use a negative Crevice Light Bounce Bias, and in v1.5 that also darkens the game's own image in crevices (65% of the contact factor) and takes the cascades' visibility into account, so crevices and corners are darker than on older versions. The values are untouched, move the bias towards 0 if it's too much for you.

Update v1.2: Added a preset that is compatible with v1.2, it is exactly the same as 1.1 preset

Quick Performance note: Enable Reshade's Performance Mode after tuning is done. It helps a lot, especially with these complicated shaders.


Update 1.1: The shaders temporal stability is improved by a noticable amount but it still isn't as coherent as I would like. The extra stability however, allowed me to increase relit bias and multibounce reflectance a bit to improve the overall image and reduce reliance on the game's own lighting. Temporal accumulation values are also retuned to be more aggressive a bit to reduce ghosting. History should now have a tendency to not drop as much as it did on v1, but it still has a long way to go.


This preset is probably not going to look as good as possible in some niche cases, but at the very least it is a good starting point.
Color bleeding, brightness control and overall color control is done in a way the is not intrusive.
Remember, Diffuse GI is inherently very subtle, most ray tracing approaches are a bit overdone.
No integration is used. It is possible to use but memory addresses will get changed so it is not stable.
You can use the standalone shader for this preset.
References in development runs, as mentioned in nexusmods, were usually path traced Witcher 3 images but after really thinking about it and looking at it very closely in real life, I noticed how subtle it is and how often shadows are biased towards grey so saturation and light reach are tuned to stay subtle. color exchange between surfaces is not done unless an object is either very bright or two surfaces are very close.
The basis of this preset is still Promotional Atmosphere Lighting Mod v5 Remastered version. You have to tune brightness and saturation for other lighting mods.
Temporal stability is... arguably good? Depends on lighting but the image does tend to break down in low light areas and very foggy enviroments/weather conditions.
The effect is rendered at 480p if you play at 1440p, and at 360p if you play at 1080p but it does not really matter because radiance cascades and Diffuse GI do not really need that high of a resolution.
The specifics of this preset and some redundant values are arbitrarily tuned for my own setup (Dell Precision 7760 17.3inch IPS) so you might find it lacking in other displays or setups. Performance relies heavily on the adaptive ray count system.
You might encounter weird issues such as archways getting too blue/bright at night but I personally do not find it very intrusive because it happens less often than you would expect. Also, in enviroments that have unified colors (Dandelion's cabaret decor features red lightsources, red carpets, reddish wood and red curtains) the lighting will bias towards that color because it obviously will.
Other than that it generally looks good and performs good. Big cascade shadows are not used because the effect is meant to leverage the game's own shadows and AO (Dark surfaces block rays slider). This is strictly a Diffuse GI shader, nothing more, nothing less. It's meant to enhance a game's mood and lighting and compliment it, not replace it. No shadowing exists in this shader aside from contact shadows to combat the contrast loss creases have after Diffuse GI application and albedo response basically just prevents the shader's added light to look like a wash on a surface (basically marries the added light to the surface)

To sum up the issues:
- Temporal stability (and overall reliance on temporal sampling which I honestly despise)
- Low render resolution (Should not really matter all that much because this kind of GI is inherently soft)
- Too much reliance on the game's own lighting. Relit pickup bias is meant to counteract this but it does not always look good and makes the effect even more reliant on temporal sampling
- Multibounce has a tendency to have wild runaways in mentioned cabaret towards overly red light, orange for Vizima palace, etc so it is tuned to be subtle and mostly affect farther cascades
- Tonemapper converter in this preset only acts as a contrast enhancer and exposure equalizer to counteract the fact that GI can inherently brighten the image on average
- Color tuning, specifically blue and orange filters are tuned to counteract the game's fog and overall bias towards more orange, warm lightsources. This game uses too much fire to light itself so it could mess the shader up
- Offscreen canvas is mostly there to smoothen camera turns, it is not equivelant to a world space cache even though the preset tries to emulate that. Overall stability in terms of distances, colors and brightness change and directionality are very good. Only stability issue is the aformentioned temporal enshittification.

I will try to improve all these mentioned problems. A full integration on a DX11 title could in theory get rid of most of these. DX12 is inherently harder to impregnate via the big shader wiener (god bless my brainrot) because it changes the memory addresses more frequently than Jensen Huang changes partners so it cannot be reliably tracked.

Until then, enjoy the first Radiance Cascade Difffused GI on an open world scale ever. You're free to use this preset as a starting point to tune the shader for other TW3 lighting mods or even other similar games. Using this preset on Cyberpunk should work well with some tuning to suite that game's artstyle. Elden Ring is also a very good candidate.