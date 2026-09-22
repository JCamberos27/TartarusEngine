import bpy
import json

print(json.dumps([
    {
        'name': image.name,
        'path': bpy.path.abspath(image.filepath),
        'packed': image.packed_file is not None,
        'size': list(image.size),
    }
    for image in bpy.data.images
], indent=2))
