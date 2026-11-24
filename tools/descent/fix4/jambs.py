"""fix4 re-apply: dev/fix3-jambs 7768ec95 onto main (db92576f, ids renumbered by #1216).

Owner: W1 Atlas. Source of truth: git diff af9eb895..7768ec95 -- project/scenes/Forest.json
(af9eb895 = the branch base, already on main via #1213; the 60ce4237 merge brought no Forest.json change).
Content: jambs families A-D (door-wall / window-wall Bounds snaps + inward fades), the 35602/35601/36406 snaps,
36393/36371 faint fades, family E (1b) living-room strip copies x3, and 36370 (B) + opening/strip N/strip S.

Matching is by entity NAME + parent name + local position (old ids are void). Each edit is per vector component:
current == new -> already applied; current == old -> set; anything else -> FAIL (someone else changed it).
New entities are copies of their (current, matched) source light with the Bounds fields overridden;
ids 36175-36180, orders 268769-268774 (Atlas's SM block). Idempotent: a second run changes nothing.
Untouched numbers are written back byte-for-byte; the file keeps the editor's JSON layout.

Usage:  python -I jambs.py <repo dir | path/to/Forest.json> [--write]     (dry run without --write)
"""
# ---------------------------------------------------------------- shared helpers (kept inline: scripts run with python -I)
import sys, json, copy, json.encoder as _je
CATS = ("empties", "models", "boxes")
NL = "\n"
TOL = 2e-5                     # editor saves float32: 7.717 comes back as 7.716999816894531
DEF_FADE = [-1, -1, -1]        # a missing 'Bounds Fade +/-' == [-1,-1,-1] (the editor writes the default on save)
ID_BLOCK, ORD_BLOCK = range(36175, 36190), range(268769, 268784)   # Atlas's SM block (TODAY_6PM_PLAN.md)

class _RawFloat(float):
    """float that remembers its source text, so untouched numbers are written back byte-for-byte"""
    __slots__ = ("raw",)
    def __new__(cls, s):
        o = float.__new__(cls, s); o.raw = s; return o

def _floatstr(o, _repr=float.__repr__):
    if isinstance(o, _RawFloat): return o.raw
    if o != o or o in (float("inf"), float("-inf")): raise ValueError("non-finite float")
    return _repr(o)

def load(path):
    with open(path, "rb") as f: t = f.read().decode("utf-8")
    global NL
    NL = "\r\n" if "\r\n" in t else "\n"   # autocrlf checkouts are CRLF: keep whatever the file has
    return json.loads(t, parse_float=_RawFloat)

def dumps(d):
    enc = json.JSONEncoder(indent=2, sort_keys=True, ensure_ascii=False)
    it = _je._make_iterencode({}, enc.default, _je.py_encode_basestring, "  ", _floatstr,
                              enc.key_separator, enc.item_separator, True, False, True)
    return "".join(it(d, 0)).replace("\n", NL)

def die(msg):
    print("FAIL: " + msg, file=sys.stderr); sys.exit(1)

def close(a, b):
    return a is not None and b is not None and abs(float(a) - float(b)) <= TOL

def same_pos(p, q):
    return all(close(x, y) for x, y in zip(p, q))

def index(scene):
    allent = [e for k in CATS for e in scene[k]]
    byid = {e["id"]: e for e in allent}
    return allent, byid

def find_one(allent, byid, name, parent, position, what):
    """exactly one entity with this name, parent name and local position (ids were renumbered by #1216)"""
    hits = [e for e in allent if e.get("name") == name and same_pos(e.get("position", []), position)
            and byid.get(e.get("parentId"), {}).get("name") == parent]
    if len(hits) != 1:
        byname = [e["id"] for e in allent if e.get("name") == name]
        die(f"{what}: {len(hits)} matches for name={name!r} parent={parent!r} pos={position} "
            f"(entities with that name: {byname[:10]})")
    return hits[0]

def fade(L, k):
    v = L.get(k)
    return list(v) if v is not None else list(DEF_FADE) if k in ("Bounds Fade +", "Bounds Fade -") else None

def check_new_id(byid, allent, nid, nord, what):
    if nid not in ID_BLOCK or nord not in ORD_BLOCK: die(f"{what}: id {nid}/order {nord} outside Atlas's block")
    if nid in byid: die(f"{what}: id {nid} already used by {byid[nid].get('name')!r}")
    if any(e.get("order") == nord for e in allent): die(f"{what}: order {nord} already used")

def scene_path(arg):
    return arg if arg.lower().endswith(".json") else arg.rstrip("/\\") + "/project/scenes/Forest.json"

# ---------------------------------------------------------------- data (generated from the old diff by Atlas)
SCRIPT = "jambs.py"
REMOVED_ON_MAIN = {"Lamp_stand Lamp_stand 14": "deleted on main by #1216 (furniture cleanup); its only change was a -z inward fade 0.4"}
DATA = json.loads(r"""
{
 "changes": [
  {
   "old_id": 35586,
   "name": "Lamp_chandelier Kitchen",
   "parent": "House Lights",
   "position": [
    -28.876089096069336,
    2.5450000762939453,
    -10.249019622802734
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     -11.579999923706055,
     -11.5215
    ],
    [
     "Bounds Fade +",
     2,
     -1,
     0.4
    ],
    [
     "Bounds Size",
     2,
     7.599999904632568,
     7.717
    ]
   ]
  },
  {
   "old_id": 35587,
   "name": "Pendants Kitchen",
   "parent": "House Lights",
   "position": [
    -35.70861053466797,
    2.6449999809265137,
    -12.094929695129395
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     -11.579999923706055,
     -11.5215
    ],
    [
     "Bounds Fade +",
     2,
     -1,
     0.4
    ],
    [
     "Bounds Size",
     2,
     7.599999904632568,
     7.717
    ]
   ]
  },
  {
   "old_id": 35592,
   "name": "Lamp_chandelier Living room",
   "parent": "House Lights",
   "position": [
    -29.759279251098633,
    2.544999122619629,
    9.946379661560059
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.464500427246094,
     -33.395
    ],
    [
     "Bounds Size",
     0,
     10.390999794006348,
     10.53
    ]
   ]
  },
  {
   "old_id": 35601,
   "name": "Lamp_wall_toilet Upstairs bathroom",
   "parent": "House Lights",
   "position": [
    -31.452800750732422,
    5.598050117492676,
    -6.487249851226807
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.04999923706055,
     -33.0285
    ],
    [
     "Bounds Center",
     2,
     -4.95,
     -5.0185
    ],
    [
     "Bounds Size",
     0,
     3.740000009536743,
     3.977
    ],
    [
     "Bounds Size",
     2,
     5.54,
     5.677
    ]
   ]
  },
  {
   "old_id": 35602,
   "name": "Lamp_bedroom Child's bedroom",
   "parent": "House Lights",
   "position": [
    -34.05670166015625,
    6.10099983215332,
    11.17924976348877
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.75,
     -33.826
    ],
    [
     "Bounds Size",
     0,
     5.139999866485596,
     5.292
    ]
   ]
  },
  {
   "old_id": 35603,
   "name": "Ceiling Stairs",
   "parent": "House Lights",
   "position": [
    -33.65705871582031,
    6.25499963760376,
    0.990369975566864
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.028499603271484,
     -33.0495
    ],
    [
     "Bounds Size",
     0,
     3.892998218536377,
     3.935
    ]
   ]
  },
  {
   "old_id": 35606,
   "name": "Lamp_table_B Lamp_table_B 11",
   "parent": "House Lights",
   "position": [
    -31.081199645996094,
    4.452099800109863,
    -13.637299537658691
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -30.185,
     -30.115
    ],
    [
     "Bounds Size",
     0,
     5.77,
     5.91
    ]
   ]
  },
  {
   "old_id": 35607,
   "name": "Lamp_wall_desk Lamp_wall_desk 12",
   "parent": "House Lights",
   "position": [
    -35.537471771240234,
    5.0964179039001465,
    13.169450759887695
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     10.895000457763672,
     10.829
    ],
    [
     "Bounds Size",
     2,
     5.230000019073486,
     5.362
    ]
   ]
  },
  {
   "old_id": 35608,
   "name": "Lamp_table_B Lamp_table_B 13",
   "parent": "House Lights",
   "position": [
    -28.58530044555664,
    4.452099800109863,
    -13.677200317382812
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -30.185,
     -30.115
    ],
    [
     "Bounds Size",
     0,
     5.77,
     5.91
    ]
   ]
  },
  {
   "old_id": 35609,
   "name": "Lamp_table_A Lamp_table_A 15",
   "parent": "House Lights",
   "position": [
    -31.175100326538086,
    1.3937000036239624,
    12.06089973449707
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.464500427246094,
     -33.395
    ],
    [
     "Bounds Center",
     2,
     10.680000305175781,
     10.195
    ],
    [
     "Bounds Fade +",
     2,
     -1,
     0.19
    ],
    [
     "Bounds Size",
     0,
     10.390999794006348,
     10.53
    ],
    [
     "Bounds Size",
     2,
     5.800000190734863,
     4.83
    ]
   ]
  },
  {
   "old_id": 35611,
   "name": "Lamp_stand Lamp_stand 17",
   "parent": "House Lights",
   "position": [
    -32.61000061035156,
    4.763999938964844,
    -12.754400253295898
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -32.98,
     -32.91
    ],
    [
     "Bounds Center",
     2,
     -10.911,
     -10.8415
    ],
    [
     "Bounds Size",
     0,
     11.36,
     11.5
    ],
    [
     "Bounds Size",
     2,
     6.218,
     6.357
    ]
   ]
  },
  {
   "old_id": 35614,
   "name": "Lamp_floor Lamp_floor 20",
   "parent": "House Lights",
   "position": [
    -37.38959884643555,
    4.466899871826172,
    7.874499797821045
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -36.32600021362305,
     -36.2565
    ],
    [
     "Bounds Center",
     2,
     6.416500091552734,
     6.486
    ],
    [
     "Bounds Size",
     0,
     4.667998790740967,
     4.807
    ],
    [
     "Bounds Size",
     2,
     3.572999954223633,
     3.712
    ]
   ]
  },
  {
   "old_id": 36367,
   "name": "Ceiling Back hall",
   "parent": "House Lights",
   "position": [
    -34.900047302246094,
    3.3450000286102295,
    -4.300005912780762
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -34.91999816894531,
     -34.899
    ],
    [
     "Bounds Size",
     0,
     2.0500025749206543,
     2.092
    ]
   ]
  },
  {
   "old_id": 36368,
   "name": "Ceiling Back hall",
   "parent": "House Lights",
   "position": [
    -36.60004425048828,
    3.3450000286102295,
    -1.4000017642974854
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     0.19499991834163666,
     0.174
    ],
    [
     "Bounds Size",
     2,
     6.320000171661377,
     6.362
    ]
   ]
  },
  {
   "old_id": 36370,
   "name": "Ceiling Living room",
   "parent": "House Lights",
   "position": [
    -36.300045013427734,
    3.3450000286102295,
    5.8000078201293945
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -36.32400131225586,
     -36.3055
    ],
    [
     "Bounds Center",
     2,
     8.791500091552734,
     5.466
    ],
    [
     "Bounds Fade +",
     2,
     -1,
     0.19
    ],
    [
     "Bounds Size",
     0,
     4.868000030517578,
     4.905
    ],
    [
     "Bounds Size",
     2,
     9.493000030517578,
     2.842
    ]
   ]
  },
  {
   "old_id": 36371,
   "name": "Lamp_stand Lamp_stand 14",
   "parent": "House Lights",
   "position": [
    -33.567020416259766,
    1.8931350708007812,
    10.878270149230957
   ],
   "fields": [
    [
     "Bounds Fade +",
     0,
     -1,
     0.4
    ]
   ]
  },
  {
   "old_id": 36372,
   "name": "Ceiling Downstairs bathroom",
   "parent": "House Lights",
   "position": [
    -32.50004196166992,
    3.3450000286102295,
    5.100006580352783
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -32.54349899291992,
     -32.5645
    ],
    [
     "Bounds Size",
     0,
     2.9230003356933594,
     2.965
    ]
   ]
  },
  {
   "old_id": 36373,
   "name": "Ceiling Master bedroom",
   "parent": "House Lights",
   "position": [
    -35.8861198425293,
    6.25499963760376,
    -9.783419609069824
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     -9.942500114440918,
     -9.9425
    ],
    [
     "Bounds Size",
     2,
     4.474999904632568,
     4.559
    ]
   ]
  },
  {
   "old_id": 36384,
   "name": "Ceiling Kitchen",
   "parent": "House Lights",
   "position": [
    -31.052650451660156,
    3.3450000286102295,
    -13.807968139648438
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -32.97999954223633,
     -32.91
    ],
    [
     "Bounds Size",
     0,
     11.359999656677246,
     11.5
    ]
   ]
  },
  {
   "old_id": 36385,
   "name": "Ceiling Kitchen",
   "parent": "House Lights",
   "position": [
    -28.05243682861328,
    3.3450000286102295,
    -13.807968139648438
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -32.97999954223633,
     -32.91
    ],
    [
     "Bounds Size",
     0,
     11.359999656677246,
     11.5
    ]
   ]
  },
  {
   "old_id": 36388,
   "name": "Pendants Kitchen",
   "parent": "House Lights",
   "position": [
    -35.708656311035156,
    2.9749999046325684,
    -12.731266021728516
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     -11.579999923706055,
     -11.5215
    ],
    [
     "Bounds Fade +",
     2,
     -1,
     0.4
    ],
    [
     "Bounds Size",
     2,
     7.599999904632568,
     7.717
    ]
   ]
  },
  {
   "old_id": 36389,
   "name": "Pendants Kitchen",
   "parent": "House Lights",
   "position": [
    -35.708656311035156,
    2.9749999046325684,
    -11.56435489654541
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     -11.579999923706055,
     -11.5215
    ],
    [
     "Bounds Fade +",
     2,
     -1,
     0.4
    ],
    [
     "Bounds Size",
     2,
     7.599999904632568,
     7.717
    ]
   ]
  },
  {
   "old_id": 36392,
   "name": "Ceiling Living room",
   "parent": "House Lights",
   "position": [
    -29.27463722229004,
    3.3450000286102295,
    11.791335105895996
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.464500427246094,
     -33.395
    ],
    [
     "Bounds Center",
     2,
     10.680000305175781,
     10.195
    ],
    [
     "Bounds Fade +",
     2,
     -1,
     0.19
    ],
    [
     "Bounds Size",
     0,
     10.390999794006348,
     10.53
    ],
    [
     "Bounds Size",
     2,
     5.800000190734863,
     4.83
    ]
   ]
  },
  {
   "old_id": 36393,
   "name": "Ceiling Living room",
   "parent": "House Lights",
   "position": [
    -34.658145904541016,
    3.3450000286102295,
    11.791335105895996
   ],
   "fields": [
    [
     "Bounds Fade -",
     2,
     -1,
     0.4
    ]
   ]
  },
  {
   "old_id": 36394,
   "name": "Ceiling Living room",
   "parent": "House Lights",
   "position": [
    -30.681140899658203,
    3.3450000286102295,
    11.791335105895996
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.464500427246094,
     -33.395
    ],
    [
     "Bounds Center",
     2,
     10.680000305175781,
     10.195
    ],
    [
     "Bounds Fade +",
     2,
     -1,
     0.19
    ],
    [
     "Bounds Size",
     0,
     10.390999794006348,
     10.53
    ],
    [
     "Bounds Size",
     2,
     5.800000190734863,
     4.83
    ]
   ]
  },
  {
   "old_id": 36400,
   "name": "Ceiling Master bedroom",
   "parent": "House Lights",
   "position": [
    -34.80495071411133,
    6.25499963760376,
    -9.783419609069824
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     -9.942500114440918,
     -9.9425
    ],
    [
     "Bounds Size",
     2,
     4.474999904632568,
     4.559
    ]
   ]
  },
  {
   "old_id": 36401,
   "name": "Ceiling Master bedroom",
   "parent": "House Lights",
   "position": [
    -33.596920013427734,
    6.25499963760376,
    -9.783419609069824
   ],
   "fields": [
    [
     "Bounds Center",
     2,
     -9.942500114440918,
     -9.9425
    ],
    [
     "Bounds Size",
     2,
     4.474999904632568,
     4.559
    ]
   ]
  },
  {
   "old_id": 36402,
   "name": "Ceiling Stairs",
   "parent": "House Lights",
   "position": [
    -32.2592887878418,
    6.25499963760376,
    0.990369975566864
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.028499603271484,
     -33.0495
    ],
    [
     "Bounds Size",
     0,
     3.892998218536377,
     3.935
    ]
   ]
  },
  {
   "old_id": 36403,
   "name": "Ceiling Stairs",
   "parent": "House Lights",
   "position": [
    -33.65705871582031,
    6.25499963760376,
    2.6645898818969727
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.028499603271484,
     -33.0495
    ],
    [
     "Bounds Size",
     0,
     3.892998218536377,
     3.935
    ]
   ]
  },
  {
   "old_id": 36404,
   "name": "Ceiling Stairs",
   "parent": "House Lights",
   "position": [
    -32.2592887878418,
    6.25499963760376,
    2.6645898818969727
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.028499603271484,
     -33.0495
    ],
    [
     "Bounds Size",
     0,
     3.892998218536377,
     3.935
    ]
   ]
  },
  {
   "old_id": 36406,
   "name": "Lamp_wall_toilet Upstairs bathroom",
   "parent": "House Lights",
   "position": [
    -31.452800750732422,
    5.598050117492676,
    -5.554109573364258
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -33.04999923706055,
     -33.0285
    ],
    [
     "Bounds Center",
     2,
     -4.95,
     -5.0185
    ],
    [
     "Bounds Size",
     0,
     3.740000009536743,
     3.977
    ],
    [
     "Bounds Size",
     2,
     5.54,
     5.677
    ]
   ]
  },
  {
   "old_id": 36407,
   "name": "Ceiling Hall fill",
   "parent": "House Lights",
   "position": [
    -33.0,
    3.3450000286102295,
    -4.0
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -32.529998779296875,
     -32.5435
    ],
    [
     "Bounds Size",
     0,
     2.9799983501434326,
     3.007
    ]
   ]
  },
  {
   "old_id": 36408,
   "name": "Ceiling Hall fill",
   "parent": "House Lights",
   "position": [
    -33.0,
    3.3450000286102295,
    1.2000000476837158
   ],
   "fields": [
    [
     "Bounds Center",
     0,
     -32.529998779296875,
     -32.5435
    ],
    [
     "Bounds Size",
     0,
     2.9799983501434326,
     3.007
    ]
   ]
  }
 ],
 "adds": [
  {
   "old_id": 38103,
   "name": "Ceiling Living room (strip)",
   "parent": "House Lights",
   "position": [
    -29.27463722229004,
    3.3450000286102295,
    11.791335105895996
   ],
   "source_name": "Ceiling Living room",
   "source_old_id": 36392,
   "light_overrides": {
    "Bounds Center": [
     -34.85,
     2.01275,
     13.0
    ],
    "Bounds Size": [
     7.62,
     2.7645,
     1.16
    ],
    "Bounds Fade +": [
     -1,
     0.03,
     -1
    ],
    "Bounds Fade -": [
     -1,
     -1,
     0.19
    ]
   },
   "new_id": 36175,
   "new_order": 268769
  },
  {
   "old_id": 38104,
   "name": "Ceiling Living room (strip)",
   "parent": "House Lights",
   "position": [
    -30.681140899658203,
    3.3450000286102295,
    11.791335105895996
   ],
   "source_name": "Ceiling Living room",
   "source_old_id": 36394,
   "light_overrides": {
    "Bounds Center": [
     -34.85,
     2.01275,
     13.0
    ],
    "Bounds Size": [
     7.62,
     2.7645,
     1.16
    ],
    "Bounds Fade +": [
     -1,
     0.03,
     -1
    ],
    "Bounds Fade -": [
     -1,
     -1,
     0.19
    ]
   },
   "new_id": 36176,
   "new_order": 268770
  },
  {
   "old_id": 38105,
   "name": "Lamp_table_A Lamp_table_A 15 (strip)",
   "parent": "House Lights",
   "position": [
    -31.175100326538086,
    1.3937000036239624,
    12.06089973449707
   ],
   "source_name": "Lamp_table_A Lamp_table_A 15",
   "source_old_id": 35609,
   "light_overrides": {
    "Bounds Center": [
     -34.85,
     2.0380001068115234,
     13.0
    ],
    "Bounds Size": [
     7.62,
     2.815000057220459,
     1.16
    ],
    "Bounds Fade +": [
     -1,
     -1,
     -1
    ],
    "Bounds Fade -": [
     -1,
     -1,
     0.19
    ]
   },
   "new_id": 36177,
   "new_order": 268771
  },
  {
   "old_id": 38109,
   "name": "Ceiling Living room (opening)",
   "parent": "House Lights",
   "position": [
    -36.300045013427734,
    3.3450000286102295,
    5.8000078201293945
   ],
   "source_name": "Ceiling Living room",
   "source_old_id": 36370,
   "light_overrides": {
    "Bounds Center": [
     -36.38,
     2.0380001068115234,
     8.2485
    ],
    "Bounds Size": [
     3.434,
     2.815000057220459,
     3.103
    ],
    "Bounds Fade +": [
     0.05,
     -1,
     -1
    ],
    "Bounds Fade -": [
     0.05,
     -1,
     0.19
    ]
   },
   "new_id": 36178,
   "new_order": 268772
  },
  {
   "old_id": 38110,
   "name": "Ceiling Living room (strip N)",
   "parent": "House Lights",
   "position": [
    -36.300045013427734,
    3.3450000286102295,
    5.8000078201293945
   ],
   "source_name": "Ceiling Living room",
   "source_old_id": 36370,
   "light_overrides": {
    "Bounds Center": [
     -34.283,
     2.0380001068115234,
     8.55
    ],
    "Bounds Size": [
     0.86,
     2.815000057220459,
     2.5
    ],
    "Bounds Fade +": [
     -1,
     -1,
     -1
    ],
    "Bounds Fade -": [
     0.05,
     -1,
     0.2
    ]
   },
   "new_id": 36179,
   "new_order": 268773
  },
  {
   "old_id": 38111,
   "name": "Ceiling Living room (strip S)",
   "parent": "House Lights",
   "position": [
    -36.300045013427734,
    3.3450000286102295,
    5.8000078201293945
   ],
   "source_name": "Ceiling Living room",
   "source_old_id": 36370,
   "light_overrides": {
    "Bounds Center": [
     -38.4025,
     2.0380001068115234,
     8.55
    ],
    "Bounds Size": [
     0.711,
     2.815000057220459,
     2.5
    ],
    "Bounds Fade +": [
     0.05,
     -1,
     -1
    ],
    "Bounds Fade -": [
     -1,
     -1,
     0.2
    ]
   },
   "new_id": 36180,
   "new_order": 268774
  }
 ]
}
""")

# ---------------------------------------------------------------- apply
def by_unique_name(allent, name, what):
    hits = [e for e in allent if e.get("name") == name]
    if len(hits) != 1: die(f"{what}: {len(hits)} entities named {name!r}")
    return hits[0]

def insert_after(scene, src, ent):
    for k in CATS:
        lst = scene[k]
        for i, e in enumerate(lst):
            if e is src:
                j = i + 1
                while j < len(lst) and lst[j].get("id") in ID_BLOCK: j += 1   # keep earlier copies of this source first
                lst.insert(j, ent); return
    die(f"source {src.get('name')!r} not found in any entity list")

def apply_adds(scene, allent, byid, adds, log):
    done = 0
    for a in adds:
        what = f"ADD {a['name']!r} (old id {a['old_id']})"
        ov = {k: [float(x) for x in v] for k, v in a["light_overrides"].items()}
        ex = [e for e in allent if e.get("name") == a["name"] and same_pos(e.get("position", []), a["position"])
              and byid.get(e.get("parentId"), {}).get("name") == a["parent"]]
        if ex:
            if len(ex) > 1 or ex[0]["id"] != a["new_id"]:
                die(f"{what}: already present with id(s) {[e['id'] for e in ex]}, expected exactly one with id {a['new_id']}")
            L = ex[0].get("Light", {})
            for k, v in ov.items():
                cur = fade(L, k) if k.startswith("Bounds Fade ") else L.get(k)
                if cur is None or not all(close(x, y) for x, y in zip(cur, v)):
                    die(f"{what}: present (id {a['new_id']}) but {k} = {cur}, expected {v}")
            log.append(f"  already  {a['new_id']} {a['name']}"); continue
        check_new_id(byid, allent, a["new_id"], a["new_order"], what)
        src = find_one(allent, byid, a["source_name"], a["parent"], a["position"], what + " source")
        if "Light" not in src: die(f"{what}: source has no Light")
        cp = copy.deepcopy(src)
        cp["id"], cp["order"], cp["name"] = a["new_id"], a["new_order"], a["name"]
        cp["Light"].update(ov)
        insert_after(scene, src, cp)
        allent.append(cp); byid[cp["id"]] = cp; done += 1
        log.append(f"  ADDED    {cp['id']} order {cp['order']} {cp['name']!r} (copy of {src['id']} {src['name']!r}) "
                   f"C {ov['Bounds Center']} S {ov['Bounds Size']} F+ {ov['Bounds Fade +']} F- {ov['Bounds Fade -']}")
    return done

def trimmed_by_master_ceiling(L):
    """True when master_ceiling.py has trimmed the lamp: local x = world z; jambs' south snap (z 20.16) kept, +z at 26.077"""
    c, s = L["Bounds Center"], L["Bounds Size"]
    zlo, zhi = -(c[0] + s[0] / 2) - 7.0, -(c[0] - s[0] / 2) - 7.0
    return abs(zlo - 20.16) < 1e-3 and abs(zhi - 26.077) < 1e-3 and close(fade(L, "Bounds Fade -")[0], 0.19)

def apply_changes(allent, byid, changes, removed_on_main, log):
    nset = nalready = 0
    for c in changes:
        what = f"CHANGE {c['name']!r} (old id {c['old_id']})"
        if c["name"] in removed_on_main and not any(e.get("name") == c["name"] for e in allent):
            log.append(f"  SKIP     {c['name']!r}: {removed_on_main[c['name']]}"); continue
        e = find_one(allent, byid, c["name"], c["parent"], c["position"], what)
        L = e.get("Light")
        if L is None: die(f"{what}: entity {e['id']} has no Light")
        for k, j, old, new in c["fields"]:
            v = fade(L, k) if k.startswith("Bounds Fade ") else (list(L[k]) if k in L else None)
            if v is None: die(f"{what}: Light has no {k!r}")
            cur = v[j]
            if close(cur, new): nalready += 1; continue
            if c["name"] == "Lamp_stand Lamp_stand 17" and k in ("Bounds Center", "Bounds Size") and j == 0 \
                    and trimmed_by_master_ceiling(L):
                nalready += 1; continue      # master_ceiling.py re-cut this axis after us; its own check holds it
            if not close(cur, old):
                die(f"{what} id {e['id']}: {k}[{j}] is {cur}, expected the old value {old} (or the new {new}): "
                    f"someone else changed it; resolve with the SM")
            v[j] = float(new); L[k] = v; nset += 1
            log.append(f"  set      {e['id']} {e['name']!r} {k}[{j}] {cur} -> {new}")
    return nset, nalready

def main():
    a = sys.argv[1:]
    if not a or a[0].startswith("-"): die(__doc__)
    path = scene_path(a[0]); write = "--write" in a
    scene = load(path); allent, byid = index(scene)
    log = []
    nset, nalready = apply_changes(allent, byid, DATA["changes"], REMOVED_ON_MAIN, log)
    nadd = apply_adds(scene, allent, byid, DATA["adds"], log)
    print("\n".join(log))
    print(f"{SCRIPT}: {nset} values set, {nalready} already applied, {nadd} entities added "
          f"({len(DATA['adds']) - nadd} already present)")
    out = dumps(scene)
    if json.loads(out) != json.loads(dumps(scene)): die("serialisation is not stable")
    if not write:
        print("DRY RUN (nothing written). Add --write to save " + path); return
    if nset or nadd:
        with open(path, "wb") as f: f.write(out.encode("utf-8"))
        print("written " + path)
    else:
        print("nothing to do; file untouched")

if __name__ == "__main__":
    main()
