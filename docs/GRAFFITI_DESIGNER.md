# Graffiti Designer

Decals Phase 3. The Designer builds graffiti pieces from layers, composites them on the GPU, and
bakes them into an ordinary decal (`DA_Piece_<name>.decal`) that the clustered decal renderer
draws (see `docs/DECALS.md`). You can also spray straight onto a wall in the Scene view.

Open it from **Window > Graffiti Designer**, by double-clicking a `GP_*.graffiti` in the Asset
Browser, or with **Create Graffiti Piece** in the Asset Browser's context menu.

## Files

| File | What it is |
|---|---|
| `assets/Decals/Graffiti/Pieces/GP_<name>.graffiti` | The recipe (JSON). This is the source of truth and stays editable. |
| `.../Pieces/<name>/T_Decal_Piece_<name>_{Albedo,Normal,Surface,Aux}.png` | The baked maps. |
| `.../Pieces/DA_Piece_<name>.decal` | The decal. The first bake creates it. Later bakes update only the parts the bake owns (maps, tier, paint, glow, channels) and keep your edits to size, fades, category and weathering. |

The recipe, its PNGs and the `.decal` are project assets. The PNGs follow the usual Drive rules:
they are gitignored, and the recipe and `.decal` are what get committed.

## The recipe (`graffitiVersion` 1)

- **Canvas**: 512, 1024 or 2048 texels per side, plus a physical size in metres. The physical size
  becomes the decal's default size.
- **Seed**: drives every random choice (overspray speckle, drip wobble, letter jitter).
- **Paint**: roughness, relief (bump from paint thickness), wall show-through, drips on or off,
  glow colour and strength, and UV paint (glows only under UV light).
- **Weathering** the decal starts with: fade, bleach, peel, buff.
- **Layers**, bottom to top. Every layer has a name, visibility, opacity, a blend mode (normal,
  multiply, screen, add, erase), *clip to the layers below*, a colour or two-colour gradient, an
  outline, a drop shadow, chrome (metal) and a glow mask. Then, by type:
  - **Fill**: the colour or gradient over the whole canvas. With *Clip* on, it fills only the
    paint below it, for example a gradient inside letters.
  - **Stamp**: an image (any of the cooked `T_Decal_Graffiti*` PNGs) with position, size,
    rotation and stretch. Coverage comes from alpha, or from brightness for white-on-black images.
  - **Text**: a string set from one of the four alphabets (Tag, Brush, Bubble, Block), with
    kerning, baseline and spin jitter, arc or wave warp, and per-letter fill and outline colours.
  - **Spray**: the recorded strokes. Each sample stores position, pressure, the can's distance
    from the wall, and time, all as integers. Each stroke also stores its cap, colour, flow,
    nib angle and erase flag.

Colours are saved as `#rrggbb` and samples as integers, so a piece in memory and the same piece
read back from disk bake to the same bytes.

## Spray model (`src/Graffiti/GraffitiPaint.*`)

- **Footprint**: a cone. Its radius is the can's distance times the cap's cone, with a minimum
  dot size, scaled slightly by pressure.
- **Caps**: Fat (wide, soft, lots of overspray), Skinny (tight lines) and Calligraphy (a
  flattened nib at a fixed angle).
- **Paint flow**: paint leaves the can over time, not over distance. A fast pass is light, a slow
  one heavy, and holding the can still makes a pool.
- **Coverage**: falls off with the inverse square of the footprint radius.
- **Drips**: paint pools into an 8-texel grid that dries over about 1.1 s. A cell that passes the
  threshold starts one drip per stroke. The drip is a bead that runs down the canvas, wobbles,
  thins and ends in a bulb, laying thick paint that shows in the relief.
- **Determinism**: every random choice comes from a counter-based hash of the piece seed. A
  stroke's seed comes from its first sample, its cap and its index in its layer, never from the
  layer's position, so reordering layers keeps each stroke's speckle. The seed is known as soon
  as the first sample exists, so the live canvas streams exactly the splats the bake will draw.
  The unit test *Graffiti live stream equals bake* checks this.

## Compositor and bake (`src/Graffiti/GraffitiCompositor.*`, `GraffitiBake.cpp`)

**Compositing**
- Each layer is drawn into its own premultiplied-colour plus paint-thickness target: splats as
  instanced quads, letters and stamps as textured quads.
- Outlines and shadows are blurred from the layer's coverage.
- The layer is then composited onto the stack below through a ping-pong pair that also carries
  height, glow and metal.
- The same program always issues the same draws in the same order, with no atomics, so a re-bake
  is byte-identical on one machine. The editor smoke test checks this.

**The bake**
- One pass writes:
  - albedo (sRGB, coverage in alpha, wall show-through);
  - a normal from the paint-thickness height;
  - surface (roughness, metal, AO, height);
  - aux (glow and the peel mask).
- The four maps are read back through PBOs a frame later.
- A worker thread then encodes the PNGs (stb at a fixed compression level), writes the `.decal`
  last, and the decal is refreshed in place. No other decal is re-decoded.

**Live strokes**
- The layers below the one being sprayed are composited once.
- After that, only the new splats are drawn into that layer's own target, and the stack above it
  is re-composited.

## The panel (`src/Editor/EditorLayer_GraffitiDesigner.cpp`)

**Top bar**
- New, the piece picker, Save, Bake.
- *Apply to selected decal*: points the selected Decal at this piece and sizes it to the piece.
- *Spray in Scene view* (see below).

**Tools**
- Move: drag the selected stamp or text layer.
- Stamp and Text: click to place a new layer.
- Spray.
- Eyedropper: picks a colour from the canvas.
- Erase: a spray stroke that removes paint from its own layer.

**Brush**
- Cap, pressure, distance (also Ctrl+wheel over the canvas), flow and nib angle.
- *Pressure from pointer speed*: a fast pass sprays lighter. The pointer's speed stands in for
  pressure because no pen pressure is read yet.
- Odometer readouts show pressure (live while spraying) and distance.

**Colour**
- A colour picker, ten swatches and recent colours.

**Layers**
- Add and remove.
- Show or hide each layer.
- Reorder by dragging or with the arrows. Rows glide to their new place.

**Layer and Piece**
- Every setting listed under *The recipe* above.

**Canvas**
- Middle-drag pans, and a fling coasts.
- The wheel zooms around the cursor.
- The brush footprint and overspray ring follow the cursor.
- **Hold Space** for a radial menu of caps and recent colours.
- A sheen sweeps the canvas when a bake lands.
- Newly typed letters cascade in.

All motion runs through `Glass::Motion()`, so it respects Reduce Motion and costs nothing at rest.

**Undo**
- Every recipe write goes through `AtomicFile`, and the editor's file journal makes each edit one
  undo step. An edit is a stroke, a drag, or a typing session; the recipe is saved once no widget
  is held.
- Undo rewrites the file. The Designer notices the new write time, reloads, and re-bakes a piece
  that is on a wall.

## Spraying in the Scene view

1. Open a piece and turn on **Spray in Scene view**.
2. Hold the left button on a wall.
   - If the open piece isn't on a wall in this scene yet, it is baked and placed there as a decal
     facing you. Otherwise, spraying off its decal starts a fresh `GP_Spray_N` piece on that wall.
   - Spraying on the piece's own decal records into it.
3. Strokes are projected into the decal's box: the ray crosses the box's mid-plane, which gives
   canvas UV.
4. While you spray, the live canvas is drawn over the wall. It stays until the re-baked decal is
   resident.
5. Each stroke is one undo step and triggers a bake. Placing a new piece's decal goes into the
   same undo step as its first stroke.

## Sounds

The *Decals & Graffiti* category is in Preferences > Editor Sounds and on the Sound Board:

| Event | When it plays |
|---|---|
| SprayStart | the can's rattle |
| SprayLoop | the hiss, held on a looping editor voice (`AudioEngine::StartEditorLoop`); level and pitch follow pressure |
| SprayEnd | the cap release |
| SprayDrip | a drip starts (rate-capped) |
| GraffitiLayerAdd / Remove / Reorder | layer changes |
| GraffitiBakeStart / BakeDone | a bake starts and finishes |

No spray recordings exist yet. Until CC0 recordings are approved and cut:
- the one-shots borrow other events' files (the Sound Board shows *placeholder: ...*);
- SprayLoop plays a **synthesised** hiss: fixed-seed filtered noise with a seamless loop.

## Tests

- **Unit tests** (`src/Tests/UnitTests_Graffiti.cpp`):
  - defaults, hex colours, round trip and forgiving load;
  - program determinism, including seeds that survive a layer reorder;
  - the live stream equals the bake;
  - the spray model (cone, distance, pressure, drips);
  - text layout and the glyph table.
- **Editor smoke test**: `--editor-tests "graffiti:"` (`src/Tests/EditorSmoke/Graffiti.inl`), in
  the Apartment Hallway.
  1. New piece, then add layers from the Add menu.
  2. A canvas stroke.
  3. Spray in Scene view onto a corridor wall.
  4. Two Bake-button bakes, compared byte for byte.
  5. Check the decal is resident and drawn.
  6. Take clean shots with the gizmos off (`CleanShot`).

  The piece's files are deleted afterwards.
