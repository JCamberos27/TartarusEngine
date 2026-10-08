# Weapon muzzle bursts

The AKS-74U and Remington 870 prefabs use a centered camera-plane burst on the
authored `Muzzle Flash` transform, with a smaller `Muzzle Core` particle system
at local zero beneath it. Both are ordinary Particle System components. The
selected muzzle attachment plays both layers for each shot.

`T_MuzzleBurst.png` is the original `Shoot FX/Textures/muzzleflash_1.png` and
`T_MuzzleCore.png` is `Shoot FX/Textures/8.png` from the project's licensed
Knife PRO Effects FPS Muzzle Flashes & Impacts pack. These are full-frame red
channel masks: use Red Mask, a 1 by 1 sheet and a centered pivot. The older
`T_MuzzleFlame.png` is a packed longitudinal flame sheet and needs its original
flame renderer rather than these billboard settings.

`Muzzle Smoke` is a third generic Particle System beneath `Muzzle Flash` in
both weapon prefabs. Its transform sits at local zero and points its emitter
down the bore. Each shot plays a short alpha-blended smoke puff; world-space
simulation lets it drift and fade after the weapon moves. Its Rendering > Shader
is **Procedural Smoke**: the engine generates smooth, animated noise clouds with
a different seed per particle. No smoke texture or flipbook is required.
The shader follows Donion Tech's [procedural smoke tutorial](https://www.youtube.com/watch?v=5FW17vL3P3M):
two simple-noise layers multiplied by gradient noise, with random scrolling
directions and rotation per particle. An asymmetric procedural mask replaces
the tutorial's sampled particle mask and the old spherical envelope, allowing
the noise to form wisps instead of a round outline.
Density, Noise Scale, Turbulence, Softness and Evolution control the shader;
Size, Lifetime, Alpha and Motion remain ordinary Particle System settings.
The weapon root's legacy Smoke and Afterfire Smoke settings are off.

`T_MuzzleSmoke.tga` is the previous, optional flipbook from the licensed pack's
`Particles/Textures/Sheets/Smoke chanel sheet 1.tga`. It is no longer assigned
to the muzzle smoke. It and the licensed flash textures remain excluded from Git.
