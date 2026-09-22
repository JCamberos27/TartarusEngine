# Render frames out of a screen recording so they can be inspected as stills.
# Usage: blender.exe -b -P work/extract_frames.py -- <video> <outdir> [maxFrames]
# There is no ffmpeg on this box, but Blender ships an FFmpeg decoder and the
# sequencer renders a movie strip straight to PNG without needing a render engine.
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src = argv[0]
out = argv[1]
max_frames = int(argv[2]) if len(argv) > 2 else 16

sc = bpy.context.scene
if sc.sequence_editor is None:
    sc.sequence_editor_create()
se = sc.sequence_editor
# Blender renamed Sequence -> Strip in 4.4; support either. Test for None, not
# truthiness: an empty bpy_prop_collection is falsy and would fall through.
strips = getattr(se, "strips", None)
if strips is None:
    strips = se.sequences
strip = strips.new_movie(name="clip", filepath=src, channel=1, frame_start=1)

total = max(1, int(strip.frame_duration))
step = max(1, total // max_frames)

sc.frame_start = 1
sc.frame_end = total
sc.frame_step = step
sc.render.image_settings.file_format = "PNG"
sc.render.resolution_x = 1280
sc.render.resolution_y = 720
sc.render.resolution_percentage = 100
sc.render.use_sequencer = True
sc.render.use_compositing = False
os.makedirs(out, exist_ok=True)
sc.render.filepath = os.path.join(out, "f_")

bpy.ops.render.render(animation=True)
print("EXTRACTED total=%d step=%d frames=%d out=%s"
      % (total, step, len(range(1, total + 1, step)), out))
