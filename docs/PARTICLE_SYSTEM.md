# Particle system and weapon muzzle effects

Open a weapon prefab by double-clicking `AKS74U.prefab` or `Remington870.prefab`, or use
**Edit Primary/Secondary Weapon Definition** under the player's First Person Controller.
Select the prefab root and expand its **C# Script / WeaponDefinition**. The project C# Inspector
has **Overview**, **Muzzle Light** and **Smoke** tabs; child Particle Systems author the flash.
Legacy flame/flash values remain serialized but hidden. Save the prefab before leaving Prefab Mode.

Each shot uses that weapon's settings, including NPC shots. Changing a prefab on disk while
the player's presentation runs refreshes muzzle values on its normal quarter-second poll.
Live flames and lights retain their shot's texture, tint, brightness and duration when switching weapons.
Scene **FX & HUD Settings** now exposes laser/HUD controls; old muzzle fields remain serialized
for compatibility with configurations that reference a bare `.fpsanim` instead of a weapon prefab.

## Emitter authoring

Add **Particle System** to an object. The inspector previews it without Play and uses tabs:

| Tab | Controls |
| --- | --- |
| Emission | Continuous rate, particles per metre travelled, maximum count, delay, duration, looping, initial/repeated bursts |
| Shape | Point/cone, cone volume, sphere, box, disc; volume/surface emission; radius, length, dimensions; world/local simulation |
| Lifetime | Lifetime, speed and size variation; size/color/alpha endpoints |
| Motion | Gravity, acceleration, additional velocity, exponential drag, starting rotation and spin ranges |
| Collision | Off, world-space horizontal plane or Play-mode PhysX world; skin, bounce, friction, lifetime loss, collision limit |
| Rendering | Alpha/additive blending, HDR intensity, texture, billboard/velocity/horizontal alignment, stretch and sprite-sheet animation |

**Restart** resets the emitter cycle and clears live particles. **Emit Burst** immediately emits
Burst Count particles (or one when that count is zero), including while continuous emission is off.
**Clear** removes particles without restarting the cycle.

The component header's **... > Apply Preset** offers Fire, Smoke, Impact Sparks, Dust Burst,
Magic Orb and Distance Trail starting points from `project/presets`. Presets replace authored
module values; use global undo to revert. Impact Sparks uses world collision, so its bounces
appear during Play. A distance trail emits only while the object moves.

**Lifetime & Emission Curves** contains size, opacity and speed multipliers over normalized
particle life, and a rate multiplier over normalized emitter cycle. Each **Edit Curve** button
opens the shared curve window with key/tangent editing, interpolation, snapping and clipboard tools.
Curve changes use global undo and save with the scene/prefab. Windows keep a checked emitter
identity and cannot write into a replacement object after a scene change or emitter removal.

Sprite sheets use Columns/Rows and fill left-to-right, then top-to-bottom. FPS zero plays the
sheet once over a particle's lifetime; a positive FPS loops it. Random Start Frame varies the
initial cell; Blend Frames cross-fades neighbouring cells. Alpha particles sort globally by depth,
including across different textures; additive particles batch by texture. Muzzle tongues retain
their packed-mask rendering, separate from ordinary RGBA sprite textures.

Plane collision previews in edit mode. World collision uses a ray along each particle's frame
movement during Play, ignores triggers and applies a skin offset at impact. It is not a swept
sphere or fluid simulation. Keep collision counts modest for dense effects. Particle capacity
is capped at 100,000 per emitter; the simulation remains on the CPU and rendering is instanced.

Original emitters load unchanged: continuous point/cone emission, 15% lifetime variation,
no texture, forces, rotation, extra bursts or collision, with unit curve multipliers.
