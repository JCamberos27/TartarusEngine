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
        # a fade per face (GI::Box::FadeFaces): wide on +x only, hard on -x
        lo, hi = (0.0, 0.05, 0.05), (0.5, 0.05, 0.05)
        self.assertAlmostEqual(b.fade_faces((0.75, 0, 0), lo, hi), 0.5, places=6)   # 0.25 m inside the +x face: 0.25 / 0.5
        self.assertEqual(b.fade_faces((-0.99, 0, 0), lo, hi), 1.0)                  # the -x face is a hard edge
        self.assertEqual(b.fade_faces((-1.01, 0, 0), lo, hi), 0.0)
        self.assertEqual(b.fade_faces((0.975, 0, 0), (0.05,) * 3, (0.05,) * 3), b.fade((0.975, 0, 0), 0.05))

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

    def test_black_room_is_not_filled(self):
        # An unlit room (stair storeroom / box room on final-1): SH ~1e-5 with a little noise. Any neighbour average
        # lands within _close's absolute term, so before the black floor nearly every probe read 'filled'.
        dims = (6, 5, 5)
        probes = self.field(dims, lambda i, j, k, c: 1e-5 * (1.0 + 0.3 * math.sin(1.7 * i + 2.3 * j + 0.9 * k + 0.4 * c)))
        vol = {"name": "v", "box": axis_box((0, 0, 0), (1, 1, 1)), "dims": dims, "layers": [("", probes), ("Lamp", probes)]}
        layers = ga.parse_gi(ga.serialize_gi([vol]))[0]["layers"]  # through half floats, as on disk
        cls = ga.classify_probes(layers, dims)
        self.assertEqual(cls.count("filled"), 0)
        self.assertEqual(cls.count("uniform"), len(cls))
        # A lit probe next to the black ones is still judged on its own (the floor needs ALL neighbours black).
        lit = list(probes)
        lit[ga.probe_index(2, 2, 2, dims)] = tuple(0.05 + 0.01 * c for c in range(12))
        cls = ga.classify_probes([("", lit)], dims)
        self.assertEqual(cls[ga.probe_index(2, 2, 2, dims)], "valid")

    def test_bake_log_inner_invalid(self):
        log = ("[info]  [GI bake] GI box room: 728 probes, 398 inside geometry (filled); mean irradiance (all layers) "
               "0.0000 0.0000 0.0000\n"
               "[info]  [GI bake] GI stair storeroom: 539 probes, 319 inside geometry (filled); mean irradiance\n"
               "[info]  [GI bake] some other line\n")
        rows = ga.parse_bake_log(log)
        self.assertEqual(rows, {"GI box room": (728, 398), "GI stair storeroom": (539, 319)})
        self.assertEqual(ga.bake_inner_invalid(728, 398, (13, 7, 8)), (398, 330, 0))  # every invalid probe is shell
        self.assertEqual(ga.bake_inner_invalid(539, 319, (7, 7, 11)), (314, 225, 5))  # 5 inner = 2%

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

    # Two rooms side by side on x, floor y 0..3, z -2..2, Fade 0.1; their meeting plane is x = 0.
    @staticmethod
    def _pair(a_hi, b_lo, fade=0.1):
        def vol(name, lo, hi):
            return {"name": name, "box": axis_box(((lo + hi) / 2, 1.5, 0), (hi - lo, 3, 4)), "fade": fade,
                    "spacing": 0.5}
        return [vol("GI a", -3.0, a_hi), vol("GI b", b_lo, 3.0)]

    def test_margins_doorway(self):
        # No wall: a doorway. Overlapping by Fade + 0.01 crossfades with wSum >= 1; touching or a gap dips.
        ok = ga.check_margins(self._pair(0.055, -0.055), [])
        self.assertEqual([r["verdict"] for r in ok], ["ok"])
        self.assertEqual(ok[0]["walled_samples"], 0)
        self.assertGreater(ok[0]["open_samples"], 0)
        for a_hi, b_lo in ((0.0, 0.0), (-0.05, 0.05)):
            bad = ga.check_margins(self._pair(a_hi, b_lo), [])
            self.assertEqual(bad[0]["verdict"], "seam")
            self.assertLess(bad[0]["open_min_wsum"], 0)

    def test_margins_wall(self):
        # A 0.2 m wall on the meeting plane. Faces 0.055 m past its mid-plane stay inside it: no leak, both surfaces
        # covered. Faces 0.245 m past it reach past the far surface's sample point (0.1 + the 0.125 m normal bias): the
        # far room's weight shows on our side of the wall (a leak).
        wall = [axis_box((0, 1.5, 0), (0.2, 3, 4))]
        ok = ga.check_margins(self._pair(0.055, -0.055), wall)
        self.assertEqual(ok[0]["verdict"], "ok")
        self.assertEqual(ok[0]["open_samples"], 0)
        self.assertGreater(ok[0]["walled_samples"], 0)
        self.assertEqual(ok[0]["wall_leak_max"], 0.0)
        leak = ga.check_margins(self._pair(0.245, -0.245), wall)
        self.assertIn("leak", leak[0]["verdict"])
        # Faces 0.14 m short of the mid-plane: the wall surfaces' samples (pushed 0.125 m into each room) sit in the fade.
        short = ga.check_margins(self._pair(-0.14, 0.14), wall)
        self.assertIn("wall-gap", short[0]["verdict"])

    def test_margins_uneven_faces_through_wall(self):
        # #1205 review: one face inside the wall, the other past its far surface. The midpoint between the faces lands
        # in room air, so the wall must be found anywhere between them; and a face 0.07 m past the far surface (hidden
        # from the leak test by the 0.125 m normal bias) still puts its probes in the next room's air.
        wall = [axis_box((0, 1.5, 0), (0.2, 3, 4))]
        for a_hi, b_lo in ((0.30, -0.055), (0.17, -0.055), (0.055, -0.17)):
            r = ga.check_margins(self._pair(a_hi, b_lo), wall)[0]
            self.assertGreater(r["walled_samples"], 0, (a_hi, b_lo))
            self.assertEqual(r["open_samples"], 0, (a_hi, b_lo))
            self.assertIn("through-wall", r["verdict"], (a_hi, b_lo))
            self.assertAlmostEqual(r["through_wall_max_m"], max(a_hi, -b_lo) - 0.1, places=3)
        ok = ga.check_margins(self._pair(0.095, -0.055), wall)[0]   # still inside the wall: fine
        self.assertEqual(ok["verdict"], "ok")
        # a corner: more wall right past the far surface, so the face sits in wall, not air
        corner = wall + [axis_box((0.5, 1.5, 0), (0.8, 3, 4))]
        self.assertNotIn("through-wall", ga.check_margins(self._pair(0.17, -0.055), corner)[0]["verdict"])
        # one room's two volumes around a wall stub: the air past it is the same room's, no leak
        same = self._pair(0.17, -0.055)
        same[0]["name"], same[1]["name"] = "GI hall 1", "GI hall 2"
        self.assertEqual(ga.check_margins(same, wall)[0]["verdict"], "ok")
        self.assertEqual(sum(r["through_wall"] for r in ga.face_probe_exposure(same, wall)), 0)

    def test_margins_outer_face_ignored(self):
        # A small box poking out of a room through an outside opening: its outer face is not part of the meeting.
        room = {"name": "GI room", "box": axis_box((0, 1.5, 2), (4, 3, 4)), "fade": 0.1, "spacing": 0.5}
        nook = {"name": "GI nook", "box": axis_box((0, 1.5, -0.1), (1, 3, 0.4)), "fade": 0.1, "spacing": 0.5}
        rows = ga.check_margins([room, nook], [])
        self.assertEqual([r["verdict"] for r in rows], ["ok"])

    def test_face_probe_exposure(self):
        vols = self._pair(0.055, -0.055)
        wall = [axis_box((0, 1.5, 0), (0.2, 3, 4))]
        rows = {r["volume"]: r for r in ga.face_probe_exposure(vols, wall)}
        self.assertGreater(rows["GI a"]["in_wall"], 0)       # its +x face, inside the wall
        self.assertGreater(rows["GI a"]["exposed"], 0)       # its other faces: open air nothing else covers
        self.assertEqual(rows["GI a"]["through_wall"], 0)
        rows = {r["volume"]: r for r in ga.face_probe_exposure(vols, [])}
        self.assertEqual(rows["GI a"]["in_wall"], 0)
        self.assertGreater(rows["GI a"]["shared"], 0)        # with no wall, the +x face sits inside GI b
        # GI a's +x face 0.07 m past the wall's far surface, inside GI b's air: through-wall, not shared
        rows = {r["volume"]: r for r in ga.face_probe_exposure(self._pair(0.17, -0.055), wall)}
        self.assertGreater(rows["GI a"]["through_wall"], 0)
        self.assertEqual(rows["GI b"]["through_wall"], 0)

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
