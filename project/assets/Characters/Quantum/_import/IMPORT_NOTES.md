# Quantum character import (from the "Quantum Characters" pack)

Modular parts only - the 60 preset characters are deliberately not imported. Logs in this folder:
`IMPORT_LOG*.csv` (source -> engine path), `RENAME_LOG*.csv` (old -> new name after cleanup).

## Layout (`project/assets/Characters/Quantum/`)
- `Models/<European|Afro|Asian|Old|Young>/` male bodies and heads; `Models/Female/{Body,Hair}`; `Models/Hair`; `Models/Beard`
- `Models/Clothing/<Male|Female>/<Tops|Outerwear|Pants|Shoes|Hats|Bags|Glasses|Accessories>/`
- `Textures/<European|Afro|Old|Young|Asian>/<Arms|Body|Head>`, `Textures/Female/<European|Afro|Old>/...`, `Textures/Clothing/<Category>/<Item>/`
- `Materials/Characters/` (skin: body/head/arms, plus eyes/teeth/brows/hair/beard), `Materials/Clothing/<Category>/<Item>/` (one .mat per colourway)
- Each texture set: BaseColor, Unity_Normal, Roughness, Occlusion (grey 2048 AO). `.meta` files carry the right import
  settings (BaseColor sRGB; Normal as normal map; Roughness/Occlusion linear).

## Materials are wired to meshes
Every model with a recognised material has `materialRemap` in its `.meta` (FBX material name -> `.mat`), so placed
instances start textured. Clothing defaults to a plain (unprinted) Black/Gray colourway - swap colourways per instance.
The match is by name (heuristic); check by eye: Jacket_M65, Hat_Cowboy, Bandana, Headband, French_Pith, Goggles,
male Bracelet/Watch, Shirt_Adventure (no matching textures in the pack: plain materials, see below).
Do NOT run Extract Materials on these models - it rewrites `materialRemap`.
Parts the pack has no textures for - eyes, cornea, eyelid/tear/saliva overlays, teeth, brows/lashes, hair, beards, fur,
and untextured clothing (M65 jacket, cowboy hat, bandana, headbands, pith helmet, goggles, flip-flops, adventure shirt,
inboots pants, tank tops) - use plain PBR materials - body parts in `Materials/Characters/`, clothing in `Materials/Clothing/Generated/` - (colour + roughness; the eye overlays are
transparent, hair cards double-sided). `tools/quantum/fill_quantum_materials.py` wires them, and the slots whose .mat
exists under another name (Vivian Afro skin, caps, hats, glasses, watches); it only fills slots missing from a remap.

## Hair and beards: not used - balaclavas instead
Hair, beards and the hats with built-in hair are out of the wardrobe (the models stay on disk; the pack ships the hair cards
without usable textures). Every character wears a balaclava (`Models/Clothing/<Male|Female>/Balaclava/`, a static mesh
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

## Git: clothing textures are not committed
`Textures/Clothing/**/*.png` (~9 GB) is in `.gitignore`; their `.meta` files, the models and the materials are tracked.
A fresh clone has untextured clothing until the PNGs are restored from the pack: back up "Quantum Characters" (e.g. to
Google Drive) and re-run the import script, or copy the `Textures/Clothing` folder over from a machine that has it.
