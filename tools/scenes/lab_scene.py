# The test labs (AnimTestbed, LocomotionLab, DoorsLab) are each an existing lab scene with more added: this holds
# what their builders share - loading the base scene, ids and orders after its highest, empties, boxes cut from the
# base scene's Ground, and writing the result the way the committed scenes are formatted.
import copy
import json
import os

from scene_kit import first


class LabScene:
    def __init__(self, path):
        with open(path, encoding='utf-8') as f:
            self.data = json.load(f)
        self._next_id = self._next_order = None

    def take_id(self):
        """The next free (id, order): after the highest of the scene as it is at the first call (a builder may
        drop some of the base scene's entities first)."""
        if self._next_id is None:
            ents = [x for k in ('boxes', 'empties', 'models') for x in self.data[k]]
            self._next_id = max(x['id'] for x in ents) + 1
            self._next_order = max(x.get('order', 0) for x in ents) + 1
        i, o = self._next_id, self._next_order
        self._next_id += 1
        self._next_order += 1
        return i, o

    def empty(self, name, parent=-1, pos=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0, 1.0), **components):
        i, o = self.take_id()
        e = {'id': i, 'name': name, 'order': o, 'parentId': parent, 'position': [float(v) for v in pos],
             'rotation': [float(v) for v in rotation], 'scale': [1.0, 1.0, 1.0]}
        e.update(components)
        self.data['empties'].append(e)
        return i

    def ground_box(self, name, pos, size, rotation=(0.0, 0.0, 0.0, 1.0)):
        """A copy of the base scene's 'Ground' box (its material, collider and flags), placed and sized as given."""
        if not hasattr(self, '_ground'):
            self._ground = first((b for b in self.data['boxes'] if b['name'] == 'Ground'), "'Ground' box")
        b = copy.deepcopy(self._ground)
        b['id'], b['order'] = self.take_id()
        b['name'] = name
        b['position'] = [float(v) for v in pos]
        b['scale'] = [float(v) for v in size]
        b['rotation'] = [float(v) for v in rotation]
        self.data['boxes'].append(b)

    def write(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(self.data, f, indent=2)
            f.write('\n')

