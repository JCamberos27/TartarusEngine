import bpy
import json
import sys

paths = sys.argv[sys.argv.index("--") + 1:]
report = {}
for path in paths:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=path, use_anim=True)
    armatures = [o for o in bpy.context.scene.objects if o.type == 'ARMATURE']
    record = []
    for armature in armatures:
        actions = []
        if armature.animation_data and armature.animation_data.action:
            actions.append(armature.animation_data.action)
        for action in bpy.data.actions:
            if action not in actions:
                actions.append(action)
        record.append({
            'object': armature.name,
            'bones': sorted(b.name for b in armature.data.bones),
            'actionNames': sorted(a.name for a in actions),
        })
    report[path] = record
print(json.dumps(report, indent=2))
