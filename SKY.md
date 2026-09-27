# Physical Sky — User Guide

The physical sky is the engine's third sky source, next to the colour gradient and HDRI
images. It simulates an atmosphere and draws volumetric clouds. The sun, moon and stars
follow a time-of-day clock. The same simulation lights the scene:

- **The sun**: the scene's directional light is aimed at the sun and filtered through the air.
  It turns warm and dim near the horizon and goes out after sunset. By night the moon takes over.
- **Ambient light and reflections**: they are baked from the live sky, clouds included, and
  re-baked as the sky changes.
- **Distance haze**: distant geometry fades into the sky's own colour (aerial perspective).
- **Cloud shadows**: drifting clouds cast moving shadows on the scene.

---

## 1. Getting started

| To… | Do this |
|---|---|
| Turn it on | **Window → Lighting → Environment**, pick **Physical**. |
| Pick a look | **Preset**: Clear Day, Fair Weather, Partly Cloudy, Overcast, Storm, Golden Hour, Sunset, Twilight, Moonlit Night, Hazy, Alien World. |
| Set the time | **Time of Day → Time**. The directional light follows the sun. |
| Animate the day | Set **Day length** (real minutes per 24 hours). The clock runs in Play; tick **Animate in editor** to run it while editing too. Stopping Play restores the authored time. |
| Aim the sun by hand | Untick **Drive the sun from the clock**. The directional light's rotation is then the sun, as with the other sky sources. |

The scene needs a **directional light** for the sun to light anything. Without one the sky
still shows a sun, but it casts no light. The light's **Intensity** and **Color** set the
sun's brightness above the atmosphere. Its **Angular Size** sets the size of the sun disc.

The editor grid and gizmos aren't part of the sky. The Game view shows exactly what Play does.

---

## 2. Settings

### Time of Day
- **Time, Date, Latitude, North**: place the sun, moon and stars as seen from that spot on Earth.
  The panel shows sunrise and sunset for the date. **North** rotates the compass (0 = north is
  world −Z, east is +X).
- **Day length, Animate in editor**: run the clock.

### Sun, Moon and Stars
- **Atmosphere colours the sun**: filter the directional light through the air (on by default).
- **Sun disc brightness / size**: affect only the visible disc. The disc is drawn at the light's
  **Angular Size**, which also sets how soft the shadows are. For soft shadows with a true-size
  sun (0.53°), raise Angular Size and lower **Sun disc size** to match. Near the horizon the disc
  flattens and turns orange, as the real one does.
- **Moon**: phases follow the date, and **Moon phase** shifts the cycle. The lit side faces the
  sun. By night the moon lights the scene and the clouds. The disc shows the near side's
  familiar pattern of maria, is evenly bright to its rim at full, and fades to a pale ghost by
  day. **Moon size** 1 is its real size, which looks small in a game view, so 1.5-2 reads as
  the moon people remember.
- **Stars, Night glow**: a procedural star field with a Milky Way that turns about the
  celestial pole through the night, plus faint airglow.
- **Night brightness** (stops): after sunset it brightens everything the sky lights, the way
  eyes adapt to the dark. The camera exposure is left alone, so your own lamps and the editor
  overlays keep their look. At 0 nights are physically dark.

### Atmosphere
All values multiply Earth's measured coefficients, so 1.0 everywhere is a clear day on Earth.
- **Air (Rayleigh)** + **Air tint**: the blue sky and red sunsets. Tint it for alien skies.
- **Haze (Mie)**, **Haze direction**, **Haze absorption**: dust and moisture. They add the white
  glow around the sun and the milky horizon.
- **Ozone**: deepens twilight blue.
- **Ground colour**: the planet's surface. It lights the lower sky and shows below the horizon
  beyond your level.
- **Distance haze**: scales distances before haze is applied to the scene. Raise it so a
  small level reads as a large landscape; 0 turns the effect off.
- **Sea level, Altitude**: where the ground is, and how high the whole scene sits.
- **Planet** (advanced): the planet's radius, the atmosphere's height, and how fast air and haze
  thin out with height.

### Volumetric Clouds
- **Coverage**: 0 is clear, 1 is overcast.
- **Density**: thin wisps up to heavy storm cloud.
- **Type**: 0 is flat stratus, 0.5 is puffy cumulus, 1 is towering cumulonimbus.
- **Base altitude, Thickness**: where the cloud layer sits and how deep it is.
- **Size, Detail**: the scale of the formations and how ragged their edges are.
- **Wind speed / direction / shear**: the clouds drift, and they move in the editor too.
  **Wind direction** is the compass bearing the wind blows toward. **Wind shear** is how far
  the tops lean downwind.
- **Lighting**:
  - **Silver lining**: forward scattering toward the sun.
  - **Sky light**: sky ambient on the clouds.
  - **Dark edges**: the powder effect.
  - **Inner glow**: multiple scattering inside the cloud.
  - **Horizon fade**: how fast distant clouds fade into the sky.
- **Cirrus**: a separate thin, high sheet of ice cloud, streaked along the wind.
- **Cloud shadows / strength**: shadows cast by the clouds on the scene.
- **Quality**:

  | Level | Resolution | Steps |
  |---|---|---|
  | Low | ¼ | 40 |
  | Medium (default) | ½ | 56 |
  | High | ½ | 96 |
  | Ultra | full | 128 |

- **Weather seed**: shifts where clouds form.

The **Ambient intensity** slider under Environment still scales the ambient light and
reflections.

---

## 3. Cost

Measured on an RTX 4060 at 1467×741, Medium quality, averaged per frame:

| Work | GPU time |
|---|---|
| Sky + clouds + haze, per view | ~0.6 ms (clouds ~0.4 ms) |
| Per frame (LUTs, cloud shadows) | ~0.1 ms |
| Environment re-capture + IBL bake | spikes of ~1 ms, only when the sky changes: sun moves > 0.35°, settings edited, or every 4 s while clouds drift |

The Scene view and the Game view each render their own sky while both are visible.

---

## 4. How it works

- **Atmosphere**: the model follows Hillaire 2020, *A Scalable and Production Ready Sky and
  Atmosphere Rendering Technique*, which Unreal's SkyAtmosphere also uses. Compute shaders
  build four lookup tables:
  - transmittance (256×64) and multiple scattering (32×32), rebuilt only when the atmosphere
    settings change;
  - a sky-view table (192×108) for each of the sun and moon, per view, per frame;
  - a 32³ aerial-perspective volume, per view.

  Shaders: `Atmosphere*.comp.glsl`, `SkyAtmosphere.frag.glsl`.
- **Clouds**: the density model follows Schneider's Horizon: Zero Dawn work:
  - a weather map, per-type height profiles, Perlin-Worley base noise and Worley erosion
    noise, all generated once at startup (`CloudNoise.comp.glsl`);
  - lighting from a cone march toward the light, dual-lobe phase, Wrenninge's
    multiple-scattering octaves, a two-stream diffuse term (stronger for closed decks than for
    lone cumulus, which lose light out of their sides), a powder term, and sky ambient;
  - sky light occluded by the cloud above each point, which darkens bases and interiors.

  Noise and weather-map mips follow each sample's pixel footprint, so distant clouds don't
  sparkle. The clouds are raymarched at reduced resolution with a per-frame jitter along the
  ray. Each frame is reprojected and accumulated with a variance-bounded history
  (`CloudTemporal.comp.glsl`), then upsampled to the screen with a cubic B-spline.
  - **Wind:** the texture offsets wrap at whole tiles. Wrapping anywhere else once made the cloud
    field jump between two layouts.
- **Scene lighting**:
  - `SkyAtmosphere::ResolveLighting` (CPU) picks the sun or the moon and filters it through
    the same transmittance the shaders use.
  - `SkyEnvCapture.comp.glsl` renders the sky and clouds into the cube `IblProbe` convolves.
  - The model shader applies the aerial-perspective volume (unit 30) and the cloud shadow
    map (unit 31).
- **Code**:
  - `src/Game/SkySettings.*`: settings and presets.
  - `src/Game/TimeOfDay.*`: sun, moon and star placement.
  - `src/Renderer/SkyAtmosphere.*`, `src/Renderer/VolumetricClouds.*`: the renderer.
  - `src/Editor/EditorLayer_Sky.cpp`: the panel.

  Tests: the `PhysicalSky` unit test, and the `smoke_sky_atmosphere` and `smoke_sky_night`
  smoke scenes.
- **Reviewing changes**: `TartarusEngine.exe --smoke-test <dir> --smoke-shots <out>` saves three
  Scene-view screenshots per scene (toward the west horizon, the east, and up), so rendering
  changes can be compared without opening the editor. `tools/sky-review/` wraps this with seven
  time-of-day scenes and a side-by-side compare script.
