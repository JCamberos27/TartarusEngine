# Quantum character import (from the "Quantum Characters" pack)

Modular parts only - the 60 preset characters are deliberately not imported. Logs in this folder:
`IMPORT_LOG*.csv` (source -> engine path), `RENAME_LOG*.csv` (old -> new name after cleanup).

## Layout (`project/assets/quantum/`)
- `Models/<European|Afro|Asian|Old|Young>/` male bodies and heads; `Models/Female/{Body,Hair}`; `Models/Hair`; `Models/Beard`
- `Models/Clothing/<Male|Female>/<Tops|Outerwear|Pants|Shoes|Hats|Bags|Glasses|Accessories>/`
- `Textures/<European|Afro|Old|Young|Asian>/<Arms|Body|Head>`, `Textures/Female/<European|Afro|Old>/...`, `Textures/Clothing/<Category>/<Item>/`
- `Materials/` (body/head/arms), `Materials/Clothing/<Category>/<Item>/` (one .mat per colourway)
- Each texture set: BaseColor, Unity_Normal, Roughness, Occlusion (grey 2048 AO). `.meta` files carry the right import
  settings (BaseColor sRGB; Normal as normal map; Roughness/Occlusion linear).

## Materials are wired to meshes
Every model with a recognised material has `materialRemap` in its `.meta` (FBX material name -> `.mat`), so placed
instances start textured. Clothing defaults to a plain (unprinted) Black/Gray colourway - swap colourways per instance.
The match is by name (heuristic); check by eye: Jacket_M65, Hat_Cowboy, Bandana, Headband, French_Pith, Goggles,
male Bracelet/Watch, Shirt_Adventure (no matching textures in the pack, left unassigned).
Do NOT run Extract Materials on these models - it rewrites `materialRemap`.
Skin/eye/teeth materials with no texture in the pack (M_EyeLeft, M_Teeth, Face_Detals...) are left to engine defaults.
Hair and beard have no textures in the pack.

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
