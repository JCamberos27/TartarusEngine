# Quantum character import (from the "Quantum Characters" pack)

Modular parts only - the 60 preset characters are deliberately not imported. Logs in this folder:
`IMPORT_LOG*.csv` (source -> engine path), `RENAME_LOG*.csv` (old -> new name after cleanup).

## Layout (`project/assets/Characters/Quantum/`)
- `Models/<European|Afro|Asian|Old|Young>/` male bodies and heads; `Models/Female/Body`
- `Models/Clothing/<Male|Female>/<Tops|Outerwear|Pants|Shoes|Bags|Glasses|Balaclava|Accessories>/`
- `Textures/<European|Afro|Old|Young|Asian>/<Arms|Body|Head>`, `Textures/Female/<European|Afro|Old>/...`, `Textures/Clothing/<Category>/<Item>/`
- `Materials/Characters/` (skin: body/head/arms, plus eyes/teeth/brows), `Materials/Clothing/<Category>/<Item>/` (one .mat per colourway)
- Each texture set: BaseColor, Unity_Normal, Roughness, Occlusion (grey 2048 AO). `.meta` files carry the right import
  settings (BaseColor sRGB; Normal as normal map; Roughness/Occlusion linear).

## Materials are wired to meshes
Every model with a recognised material has `materialRemap` in its `.meta` (FBX material name -> `.mat`), so placed
instances start textured. Clothing defaults to a plain (unprinted) Black/Gray colourway - swap colourways per instance.
The match is by name (heuristic); check by eye: Jacket_M65, Bandana, Headband, Goggles,
male Bracelet/Watch, Shirt_Adventure (no matching textures in the pack: plain materials, see below).
Do NOT run Extract Materials on these models - it rewrites `materialRemap`.
Parts the pack has no textures for - eyes, cornea, eyelid/tear/saliva overlays, teeth, brows/lashes, fur,
and untextured clothing (M65 jacket, bandana, headbands, goggles, flip-flops, adventure shirt,
inboots pants, tank tops) - use plain PBR materials - body parts in `Materials/Characters/`, clothing in `Materials/Clothing/Generated/` - (colour + roughness; the eye overlays are
transparent, brow/lash cards double-sided). `tools/quantum/fill_quantum_materials.py` wires them, and the slots whose .mat
exists under another name (Vivian Afro skin, caps, glasses, watches); it only fills slots missing from a remap.

## Hair and beards: not used - balaclavas instead
Hair, beards and hats were imported, then deleted in #501 (the pack ships the hair cards without usable textures). Restore
them from the pack if they are ever wanted. Every character wears a balaclava (`Models/Clothing/<Male|Female>/Balaclava/`, a static mesh
modelled on the Quantum head; `OutfitSystem::UpdateAttachments` rides it on the head bone). Colourways:
`Materials/Clothing/Balaclava/` (Black, Green, Print); textures in `Textures/Clothing/Balaclava/` (git-ignored like
the other clothing PNGs; source: `textures.zip` beside `sm_balaclava_crime.fbx`).

## Eyes, brows and lashes
The pack has no eye, brow or lash textures. `Textures/Eyes/T_Eye_<Colour>.png` (Hazel, Brown, DarkBrown, Blue, Green, Gray) are
built from an iris macro photo by Grégoire Hervé-Bazin on Unsplash (https://unsplash.com/photos/0YMnASP4N0I, Unsplash License:
free for commercial use, no attribution required): the iris rewrapped concentric with a clean pupil, on a generated sclera,
sized to the Quantum eyeball's spherical UV (limbus at UV radius 0.135). Materials `Materials/Characters/Eyes/M_Eye_<Colour>.mat`;
each head picks one (`M_Eye.mat` = Brown). Brows and lashes are hair cards: `T_Brows_Lashes.png` / `T_F_Brows_Lashes.png` are
generated strands fitted to the male / female card layouts (alpha cutout; `M_Brows_Lashes`, `M_F_Brows_Lashes`, grey for Old).

## Naming fixed
Tatto->Tattoo, Qunatum->Quantum, Fase->Face, Glassses->Glasses, Tshist->Tshirt, Vivan->Vivian, Hight->High,
FacerRig->FaceRig, Irokez->Mohawk, Snikers->Sneakers, Classik->Classic, Bordo->Burgundy, Bege->Beige, Grey->Gray, Commo->Camo,
doubled `M_M_` prefixes, lower-case words capitalised, `_old_`->`_Old_`, `Young_Head`->`Head_Young`,
AO maps now `..._Occlusion.<udim>.png`. Existing European assets were renamed/moved into `European/` keeping their GUIDs
(scene and material references were rewritten).

## Not imported
Presets, FaceRig (.mb/.dna), Female Camo/MakeUp/tattoo variants, `.psd`/`.tx` sources, duplicates (`Spare_SKM`, `(1).zip`).

## Pack quirks worth knowing
- The `Flip_Flops` texture folder contains shorts/breeches textures, so flip-flops meshes have no real textures.
- Asian and Young bodies have no textures of their own; they reuse the European body/arms materials.
- Some meshes ship a second skin layer (e.g. tucked T-shirt under a jacket); both layers are wired.

## Git: models and textures are not committed
The pack's licence doesn't allow redistributing it, so every Quantum `.fbx` and `.png` (except the generated
`Textures/Eyes/T_*Brows_Lashes.png`) is in `.gitignore`; their `.meta` files and the materials are tracked. They live on the
team's shared Google Drive under `Tartarus Assets\Used`, at the same paths; `tools\assets\fetch-assets.ps1` copies them in
(docs/ASSETS.md). The parts of the original pack we have are in `Tartarus Assets\Raw\Characters`.
