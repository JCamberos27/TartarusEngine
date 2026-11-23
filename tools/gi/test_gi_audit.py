"""Unit tests for gi_audit.py's pure parts (no scene, no git): python -m unittest discover -s tools/gi -p 'test_*.py'."""
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gi_audit as ga  # noqa: E402


def axis_box(center, size):
    return ga.Box.from_transform(ga.compose(center, (0, 0, 0, 1), (1, 1, 1)), size)


class MathTests(unittest.TestCase):
    def test_quat_yaw_90(self):
        # +90 deg about Y maps +X to -Z (right-handed, GLM).
        s = math.sqrt(0.5)
        m = ga.quat_to_mat3((0, s, 0, s))
        x = [m[0][0], m[1][0], m[2][0]]
        for got, want in zip(x, (0, 0, -1)):
            self.assertAlmostEqual(got, want, places=6)

    def test_compose_parent_child(self):
        parent = ga.compose((10, 0, 0), (0, 0, 0, 1), (2, 2, 2))
        child = ga.compose((1, 0, 0), (0, 0, 0, 1), (1, 1, 1))
        p = ga.mat_point(ga.mat_mul(parent, child), (0, 0, 0))
        self.assertEqual([round(c, 6) for c in p], [12, 0, 0])

    def test_box_from_scaled_transform(self):
        b = ga.Box.from_transform(ga.compose((0, 0, 0), (0, 0, 0, 1), (2, 1, 1)), (1, 4, 6))
        self.assertEqual([round(h, 6) for h in b.half], [1, 2, 3])

    def test_box_fade(self):
        b = axis_box((0, 0, 0), (2, 2, 2))
        self.assertEqual(b.fade((0, 0, 0), 0.05), 1.0)
        self.assertAlmostEqual(b.fade((0.975, 0, 0), 0.05), 0.5, places=6)
        self.assertEqual(b.fade((1.2, 0, 0), 0.05), 0.0)
        self.assertEqual(b.fade((1.0, 0, 0), 0.0), 1.0)  # hard edge: the face itself is inside

    def test_grid_dims_matches_cpp(self):
        # GridDims: ceil(2h/s - 1e-4) + 1, clamped 2..256.
        self.assertEqual(ga.grid_dims((3.74, 1.4075, 2.835), 0.5), (16, 7, 13))
        self.assertEqual(ga.grid_dims((1.0, 1.0, 1.0), 0.5), (5, 5, 5))  # exact multiple: no extra probe
        self.assertEqual(ga.grid_dims((0.01, 0.01, 0.01), 0.5), (2, 2, 2))
        self.assertEqual(ga.grid_dims((500, 1, 1), 0.5)[0], 256)

    def test_split_circuits(self):
        self.assertEqual(ga.split_circuits(" Living room; Lamp_table_A 15 ,,Living room"),
                         ["Living room", "Lamp_table_A 15"])
        self.assertEqual(ga.split_circuits(""), [])


class GiFileTests(unittest.TestCase):
    def make(self):
        dims = (2, 2, 3)
        n = dims[0] * dims[1] * dims[2]
        probes = [tuple(float(i + c) * 0.25 for c in range(12)) for i in range(n)]
        return [{"name": "GI test", "box": axis_box((1, 2, 3), (2, 2, 4)), "dims": dims,
                 "layers": [("", probes), ("Kitchen", probes)]}]

    def test_roundtrip(self):
        vols = ga.parse_gi(ga.serialize_gi(self.make()))
        self.assertEqual(len(vols), 1)
        v = vols[0]
        self.assertEqual(v["name"], "GI test")
        self.assertEqual(v["dims"], (2, 2, 3))
        self.assertEqual([c for c, _ in v["layers"]], ["", "Kitchen"])
        self.assertEqual(v["layers"][1][1][5][3], (5 + 3) * 0.25)
        self.assertTrue(v["box"].matches(axis_box((1, 2, 3), (2, 2, 4))))

    def test_rejects_bad_files(self):
        data = ga.serialize_gi(self.make())
        with self.assertRaises(ValueError):
            ga.parse_gi(b"XXXX" + data[4:])
        with self.assertRaises(ValueError):
            ga.parse_gi(data[:-10])


class InvalidProbeTests(unittest.TestCase):
    @staticmethod
    def field(dims, f):
        out = []
        for k in range(dims[2]):
            for j in range(dims[1]):
                for i in range(dims[0]):
                    out.append(tuple(f(i, j, k, c) for c in range(12)))
        return out

    def test_filled_probe_detected(self):
        dims = (4, 3, 3)
        # A curved field (no probe is an exact neighbour average), then two probes filled like GI::FillInvalid.
        # Each coefficient varies on its own (real probes have several degrees of freedom; a field where all 12 are
        # scalings of one number lets random neighbour subsets match by chance).
        probes = self.field(dims, lambda i, j, k, c: 0.1 + i * i * (0.37 + 0.05 * c) + j ** 3 * (0.11 + 0.07 * ((c * 5) % 3))
                            + k * k * (0.53 - 0.03 * c) + 0.013 * c * i * j * (k + 1))
        # Ring 1: a probe filled from a subset of its neighbours (only 3 "done").
        idx2 = ga.probe_index(2, 1, 1, dims)
        sub = [ga.probe_index(*p, dims) for p in ((3, 1, 1), (2, 0, 1), (2, 2, 1))]
        probes[idx2] = tuple(sum(probes[n][c] for n in sub) / 3 for c in range(12))
        # Ring 2: its neighbour, filled from all 6 (one of them the ring-1 fill).
        idx = ga.probe_index(1, 1, 1, dims)
        nbs = [ga.probe_index(*p, dims) for p in ((0, 1, 1), (2, 1, 1), (1, 0, 1), (1, 2, 1), (1, 1, 0), (1, 1, 2))]
        probes[idx] = tuple(sum(probes[n][c] for n in nbs) / 6 for c in range(12))
        # Through half floats, as on disk.
        vol = {"name": "v", "box": axis_box((0, 0, 0), (1, 1, 1)), "dims": dims, "layers": [("", probes)]}
        layers = ga.parse_gi(ga.serialize_gi([vol]))[0]["layers"]
        cls = ga.classify_probes(layers, dims)
        self.assertEqual(cls[idx], "filled")
        self.assertEqual(cls[idx2], "filled")
        self.assertEqual(cls.count("filled"), 2)

    def test_uniform_is_not_filled(self):
        dims = (3, 3, 3)
        probes = self.field(dims, lambda i, j, k, c: 0.0)
        cls = ga.classify_probes([("", probes)], dims)
        self.assertEqual(cls.count("filled"), 0)
        self.assertEqual(cls.count("uniform"), 27)

    def test_lit_exact_copy_is_filled(self):
        # A face probe in a wall filled from its one done neighbour is a bit-exact copy of it.
        dims = (3, 2, 2)
        probes = self.field(dims, lambda i, j, k, c: 0.2 + i * i * 0.31 + j * 0.7 + k * (1.3 + c * 0.05) + c * 0.02)
        probes[ga.probe_index(0, 0, 0, dims)] = probes[ga.probe_index(1, 0, 0, dims)]
        cls = ga.classify_probes([("", probes)], dims)
        self.assertEqual(cls[ga.probe_index(0, 0, 0, dims)], "filled")

    def test_must_match_in_every_layer(self):
        dims = (3, 2, 2)
        a = self.field(dims, lambda i, j, k, c: 0.2 + i * 0.5 + c * 0.01)  # linear in i: middle = avg of its x pair
        b = self.field(dims, lambda i, j, k, c: 0.3 + i * i * 0.4 + j * 0.7 + k * 1.3 + c * 0.02)
        cls = ga.classify_probes([("", a), ("K", b)], dims)
        self.assertEqual(cls.count("filled"), 0)


class ShadowTests(unittest.TestCase):
    @staticmethod
    def lamp(i, pos, intensity=1.0, rng=5.0):
        return {"id": i, "name": "L%d" % i, "pos": list(pos), "intensity": intensity, "range": rng,
                "type": "Point", "shadow": True, "lit": True, "active": True}

    def test_budget_picks_nearest(self):
        lamps = [self.lamp(i, (i * 10.0, 0, 0)) for i in range(6)]
        self.assertEqual(ga.pick_shadowed(lamps, (0, 0, 0), 4), [0, 1, 2, 3])
        self.assertEqual(sorted(ga.pick_shadowed(lamps, (50, 0, 0), 4)), [2, 3, 4, 5])

    def test_holder_bonus(self):
        lamps = [self.lamp(0, (10, 0, 0)), self.lamp(1, (9.5, 0, 0))]
        self.assertEqual(ga.pick_shadowed(lamps, (0, 0, 0), 1), [1])
        self.assertEqual(ga.pick_shadowed(lamps, (0, 0, 0), 1, prev=[0]), [0])

    def test_score_inside_range_is_full(self):
        l = self.lamp(0, (1, 0, 0), intensity=2.0, rng=3.0)
        self.assertAlmostEqual(ga.shadow_score(l, (0, 0, 0)), 18.0)

    def test_distance_fade(self):
        self.assertEqual(ga.distance_fade(5, 3, 1.0), 1.0)
        self.assertEqual(ga.distance_fade(1, 1000, 1.0), 0.0)

    def test_candidates_filter(self):
        ls = [self.lamp(0, (0, 0, 0)), dict(self.lamp(1, (0, 0, 0)), shadow=False),
              dict(self.lamp(2, (0, 0, 0)), type="Directional"), dict(self.lamp(3, (0, 0, 0)), active=False)]
        self.assertEqual([l["id"] for l in ga.shadow_candidates(ls)], [0])


class CoverageTests(unittest.TestCase):
    def test_top_face_samples(self):
        slab = axis_box((0, -0.1, 0), (2, 0.2, 1))
        pts = ga.sample_top_face(slab, 0.25)
        self.assertEqual(len(pts), 8 * 4)
        self.assertTrue(all(abs(p[1]) < 1e-9 for p in pts))

    def test_coverage_and_overlap(self):
        vols = [{"name": "A", "box": axis_box((0, 0, 0), (2, 2, 2)), "fade": 0.05},
                {"name": "B", "box": axis_box((1.9, 0, 0), (2, 2, 2)), "fade": 0.05}]
        self.assertEqual(ga.coverage_at((-0.5, 0, 0), vols)[0], 1.0)
        self.assertEqual(ga.coverage_at((0.95, 0, 0), vols)[0], 2.0)
        self.assertEqual(ga.coverage_at((5, 0, 0), vols)[0], 0.0)

    def test_clusters(self):
        keys = [(0, 0, 0), (1, 0, 1), (5, 0, 5), (0, 1, 0)]
        comps = ga.cluster_points(keys, 0.25)
        self.assertEqual(sorted(len(c) for c in comps), [1, 1, 2])

    def test_adjacent_pairs(self):
        vols = [{"name": "GI a 1", "box": axis_box((0, 1, 0), (2, 2, 2)), "fade": 0.05},
                {"name": "GI a 2", "box": axis_box((2.1, 1, 0), (2, 2, 2)), "fade": 0.05},   # 0.1 m gap on x
                {"name": "GI b", "box": axis_box((0, 1, 1.8), (2, 2, 2)), "fade": 0.05},     # 0.2 m overlap on z
                {"name": "GI far", "box": axis_box((20, 1, 0), (2, 2, 2)), "fade": 0.05}]
        pairs = {(a["name"], b["name"]): (axis, round(gap, 3)) for a, b, axis, gap in ga.adjacent_pairs(vols)}
        self.assertEqual(pairs[("GI a 1", "GI a 2")], (0, 0.1))
        self.assertEqual(pairs[("GI a 1", "GI b")], (2, -0.2))
        self.assertNotIn(("GI a 1", "GI far"), pairs)
        self.assertIn("gap", ga.blend_note(0.1, 0.05, 0.05))
        self.assertIn("plateau", ga.blend_note(-0.3, 0.05, 0.05))

    def test_room_of(self):
        self.assertEqual(ga.room_of("GI living room 10"), "living room")
        self.assertEqual(ga.room_of("GI landing"), "landing")

    def test_tour_points(self):
        doc = {"waypoints": [{"name": "w1", "position": [1, 2, 3]}, {"pos": [4, 5, 6]}],
               "routes": [{"name": "r", "points": [{"p": [7, 8, 9]}]}]}
        pts = ga.tour_points(doc)
        self.assertEqual([p for _, p in pts], [[1, 2, 3], [4, 5, 6], [7, 8, 9]])
        self.assertEqual(pts[0][0], "w1")


class TourTests(unittest.TestCase):
    def test_tour_waypoints_shape(self):
        doc = {"player": {"eyeHeight": 1.6},
               "waypoints": [{"id": "a", "room": "Hall", "feet": [0, 0, 0]}, {"id": "b", "feet": [2, 0, 0]}],
               "routes": [{"id": "r", "points": ["a", "b", "missing"]}]}
        eye, wps, routes = ga.tour_waypoints(doc)
        self.assertEqual(eye, 1.6)
        self.assertEqual([w[0] for w in wps], ["a", "b"])
        self.assertEqual(routes, [("r", [[0.0, 0.0, 0.0], [2.0, 0.0, 0.0]])])
        self.assertIsNone(ga.tour_waypoints({"waypoints": [{"pos": [1, 2, 3]}]}))

    def test_route_samples(self):
        pts = ga.route_samples([[0, 0, 0], [1, 0, 0], [1, 0, 0.5]], 0.25)
        self.assertEqual(len(pts), 4 + 2 + 1)
        self.assertEqual(pts[-1], [1, 0, 0.5])


class DiffTests(unittest.TestCase):
    def test_diff_lights(self):
        base = {"id": 1, "name": "Lamp", "type": "Point", "pos": [0, 0, 0], "circuit": "Kitchen", "shadow": True,
                "intensity": 1.0, "range": 5.0, "active": True, "color": [1, 1, 1]}
        moved = dict(base, pos=[0.5, 0, 0], circuit="Dining")
        rows = ga.diff_lights([base, dict(base, id=2, name="Gone")], [moved, dict(base, id=3, name="New")])
        kinds = sorted(r[0] for r in rows)
        self.assertEqual(kinds, ["added", "changed", "removed"])
        # An editor save that renumbers every id is not a change.
        self.assertEqual(ga.diff_lights([base], [dict(base, id=99)]), [])
        ch = [r for r in rows if r[0] == "changed"][0][3]
        self.assertIn("moved 0.50 m", ch)
        self.assertIn("circuit", ch)


if __name__ == "__main__":
    unittest.main()
