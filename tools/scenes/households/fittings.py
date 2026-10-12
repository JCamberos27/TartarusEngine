# The apartment's built-ins, the same in every unit (the building's, not the household's): the kitchen run, the two
# bathrooms' fixtures, the washer and the breaker panel, and the light switches by the doors. Each returns the placed
# pieces a household dresses.
from apartment_kit import WARM, inner


def kitchen(b):
    """Fridge, counter, stove, counter along the north wall and the sink run on the east wall - the fridge -> sink
    -> stove triangle - uppers above, the pantry by the opening."""
    r = 'Kitchen'
    x0, z0, x1, z1 = inner(r)

    def run_n(stem, x, **kw):  # along the north wall, left edge at x
        w = b.idx[stem]['max'][0] - b.idx[stem]['min'][0]
        return b.wall(r, 'N', x + w / 2, stem, **kw)
    f = {'fridge': run_n('Fridge_Assembled', x0 + 0.02, collide=True)}
    f['c1'] = run_n('Kitchen_Counter_A_Assembled', f['fridge']['x1'] + 0.02, collide=True)
    f['stove'] = run_n('Stove', f['c1']['x1'], collide=True)
    f['c2'] = run_n('Kitchen_Counter_A_Assembled', f['stove']['x1'], collide=True)
    st = f['stove']
    f['hood'] = b.wall(r, 'N', (st['x0'] + st['x1']) / 2, 'Stove_Cooker_Hood', y=1.72)
    f['uppers'] = [b.wall(r, 'N', (c['x0'] + c['x1']) / 2, 'Kitchen_Cabinet_B_Assembled', y=1.52, collide=True)
                   for c in (f['c1'], f['c2'])]
    f['sink'] = b.wall(r, 'E', z0 + 0.62 + 0.5, 'Kitchen_Counter_Sink_Assembled', collide=True)
    f['c3'] = b.wall(r, 'E', f['sink']['z1'] + 0.5, 'Kitchen_Counter_A_Assembled', collide=True)
    f['uppers'] += [b.wall(r, 'E', (c['z0'] + c['z1']) / 2, 'Kitchen_Cabinet_B_Assembled', y=1.52, collide=True)
                    for c in (f['sink'], f['c3'])]
    f['pantry'] = b.wall(r, 'S', x0 + 0.85, 'Kitchen_Cabinet_Large_Assembled', collide=True)
    b.switch(r, 'W', 6.05)
    b.outlet(r, 'N', f['c1']['x0'] + 0.2, y=1.12, gfi=True)
    b.outlet(r, 'E', f['c3']['z1'] - 0.2, y=1.12, gfi=True)
    return f


def hood_light(b, f, intensity=1.1):
    """The cooker hood's light: the one people leave on at night."""
    st, hood = f['stove'], f['hood']
    pos = ((st['x0'] + st['x1']) / 2, hood['y0'] - 0.02, (hood['z0'] + hood['z1']) / 2 + 0.1)
    b.light('Kitchen', 'Hood Light', pos, intensity, 2.5, WARM, spot=62.0, fill=0)


def ensuite(b):
    """Shower across the north end, the double vanity on the west wall under the mirror, the toilet on the east wall
    with the paper beside it, a towel rail."""
    r = 'Ensuite'
    x0, z0, x1, z1 = inner(r)
    f = {'shower': b.wall(r, 'N', (x0 + x1) / 2, 'Shower_Stall_190cm_L_Assembled', collide=True)}
    b.wall(r, 'N', (x0 + x1) / 2 + 0.55, 'Shower', y=0.35)
    f['soap'] = b.wall(r, 'N', (x0 + x1) / 2 - 0.4, 'Shower_Soap_Holder', y=1.1)
    f['vanity'] = b.wall(r, 'W', 2.02, 'Bathroom_Sink_Body', collide=True)
    b.wall(r, 'W', 2.02, 'Mirror_Bathroom', y=1.15)
    f['toilet'] = b.wall(r, 'E', 3.4, 'Toilet_Assembled', collide=True)
    b.wall(r, 'E', 2.95, 'Toiletpaper_Single', y=0.7)
    f['rail'] = b.wall(r, 'E', 1.55, 'Towel_Holder', y=1.25)
    b.switch(r, 'W', 2.85)
    return f


def bathroom(b):
    """Shower on the east wall, vanity on the south wall, toilet on the west, a towel bar."""
    r = 'Bathroom'
    x0, z0, x1, z1 = inner(r)
    sw = b.wbox('Shower_Stall_190cm_R_Assembled', -90.0)
    f = {'shower': b.wall(r, 'E', z0 + (sw[5] - sw[4]) / 2 + 0.01, 'Shower_Stall_190cm_R_Assembled', collide=True)}
    b.wall(r, 'E', f['shower']['z0'] + 0.5, 'Shower', y=0.35)
    f['vanity'] = b.wall(r, 'S', x0 + 0.95, 'Bathroom_Sink_Body', collide=True)
    b.wall(r, 'S', x0 + 0.95, 'Mirror_Bathroom', y=1.15)
    f['toilet'] = b.wall(r, 'W', 6.95, 'Toilet_Assembled', collide=True)
    b.wall(r, 'W', 6.45, 'Toiletpaper_Single', y=0.7)
    f['rail'] = b.wall(r, 'W', 7.6, 'Towel_Holder', y=1.3)
    b.switch(r, 'N', 11.85)  # the latch side is jammed in the corner: switch on the hinge side
    return f


def laundry(b):
    """The washer against the south wall and the breaker panel."""
    r = 'Laundry'
    x0, z0, x1, z1 = inner(r)
    # the washer is modelled with its front at local -X (control panel along +X): a quarter turn puts the panel
    # against the wall
    f = {'washer': b.wall(r, 'S', x1 - 1.0, 'Washingmachine_Assembled', collide=True, extra_yaw=90.0)}
    f['panel'] = b.wall(r, 'N', 11.3, 'Electricbox_Main', y=1.35)
    b.latch_side('Laundry Door', r)
    b.outlet(r, 'S', x1 - 0.45, y=0.9, gfi=True)
    return f


def switches(b):
    """The switches by the bedroom doors (both sides of the master's) and at the front door."""
    b.latch_side('Master Door', 'Living Room')
    b.latch_side('Master Door', 'Master Bedroom')
    b.latch_side('Bedroom 2 Door', 'Bedroom 2')
    b.latch_side('Kids Bedroom Door', 'Kids Bedroom')
    b.switch('Hall', 'N', 12.55)


# Each room's ceiling light, where the wiring is: (model, x, z).
CEILING = {'Living Room': ('Lamp_Ceiling_B', 2.4, 7.1), 'Kitchen': ('Lamp_Ceiling_A', 8.2, 6.3),
           'Master Bedroom': ('Lamp_Ceilingfan', 2.4, 2.1), 'Ensuite': ('Lamp_Ceiling_A', 5.5, 2.2),
           'Bedroom 2': ('Lamp_Ceiling_A', 8.3, 2.1), 'Kids Bedroom': ('Lamp_Ceiling_A', 11.5, 2.1),
           'Bathroom': ('Lamp_Ceiling_A', 11.4, 6.85), 'Hall': ('Lamp_Ceiling_B', 11.5, 4.8)}


def ceilings(b, on=None, skip=()):
    """Every room's ceiling light, off unless `on` gives it the ceiling_fixture light settings."""
    on = on or {}
    for room, (stem, x, z) in CEILING.items():
        if room not in skip:
            b.ceiling_fixture(room, stem, x, z, **({'on': True, **on[room]} if room in on else {}))


def kitchen_table(b, chairs=('W', 'S', 'E'), tx=7.9, tz=8.1, yaw=0.0, turn=None):
    """The round table where the kitchen has room for it: clear of the stove's front (it sticks out a metre) and
    the sink run, with chairs tucked in on the given sides - never the stove's, which would close the walk to the
    hob. turn: side -> extra yaw for a chair pulled out at an angle. Returns the table."""
    r = 'Kitchen'
    tb = b.put(r, 'Table_Dining', tx, tz, yaw, collide=True)
    turn = turn or {}
    for side in chairs:
        dx, dz, face = {'W': (-0.8, 0.0, 90.0), 'S': (0.0, 0.8, 180.0), 'E': (0.8, 0.0, -90.0)}[side]
        b.put(r, 'Chair_Dining', tx + dx, tz + dz, face + turn.get(side, 0.0), collide=True)
    return tb
