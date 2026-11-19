# The test labs (AnimTestbed, LocomotionLab, DoorsLab) are each an existing lab scene with more added: this holds
# what their builders shared - loading the base scene, ids and orders after its highest, a Y-axis quaternion and
# writing the result the way the committed scenes are formatted.
import json
import math
import os


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

    def write(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(self.data, f, indent=2)
            f.write('\n')


def yaw_quat(deg):
    """[x, y, z, w] for a turn of `deg` degrees about +Y."""
    h = math.radians(deg) / 2
    return [0.0, round(math.sin(h), 6), 0.0, round(math.cos(h), 6)]
