# 312: the roommates. Three guys in their twenties: a sofa each, the TV always on, cans and takeaway everywhere,
# boxes from the move never unpacked, bare walls and bare windows, the third bedroom a junk room. Every door is
# open and the kitchen tube is on; whoever's still up isn't in the living room.
from apartment_kit import H, MODES, TUBE, TV, inner
from households import fittings

PAINT = {'Living Room': 'wall_grey', 'Hall': 'wall_grey', 'Kitchen': 'wall_yellowed', 'Master Bedroom': 'wall_grey',
         'Bedroom 2': 'wall_teal', 'Kids Bedroom': 'wall_grey', 'Ensuite': 'tile_wall', 'Bathroom': 'tile_wall',
         'Laundry': 'wall_yellowed'}
FLOORS = {'Living Room': 'laminate', 'Hall': 'laminate', 'Kitchen': 'tile', 'Master Bedroom': 'laminate',
          'Bedroom 2': 'laminate', 'Kids Bedroom': 'laminate', 'Ensuite': 'tile', 'Bathroom': 'tile', 'Laundry': 'tile'}
DOORS_OPEN = {'Master Door': 60.0, 'Ensuite Door': 50.0, 'Bedroom 2 Door': 35.0, 'Kids Bedroom Door': 10.0,
              'Bathroom Door': 30.0, 'Laundry Door': 80.0}


def litter(b, room, items):
    """Things dropped on the floor: (model, x, z, yaw[, pitch, roll])."""
    for stem, x, z, yaw, *tip in items:
        b.put(room, stem, x, z, yaw, **({'pitch': tip[0], 'roll': tip[1]} if tip else {}))


def living(b):
    """The big sofa square to the TV between the bare windows, a crate-and-sidetable coffee table buried in cans,
    the gaming chair, the couch someone is sleeping on, cords across the floor, a painting never hung."""
    r = 'Living Room'
    cz = 7.1
    tvs = b.wall(r, 'W', cz, 'Tv_Stand_Assembled', collide=True)
    tv = b.on(r, tvs, 'Tv', dz=-0.04)
    tx, tz = b.frame_of(tv, 0.0, (tv['x1'] - tv['x0']) / 2 + 0.004)
    b.s.box('TV Picture', b.group(r, b.group('Furniture')), b.pos((tx, tv['y0'] + 0.31, tz)),
            (0.004, 0.4, 0.6), b.mats['screen'], collider=False, static=True, castShadows=0)
    b.light(r, 'TV', (tx + 0.35, tv['y0'] + 0.3, tz), 1.0, 5.0, TV, spot=75.0, pitch=0.0, yaw=-90.0,
            flicker={'Mode': MODES['Tv'], 'MinInterval': 0.08, 'MaxInterval': 0.9, 'Depth': 0.6, 'Seed': 21},
            fill=0.12)
    b.shelf_row(r, tvs, 0, [('Cardboard_Box_A', 90.0), ('Storage_Box_Cylindrical', 0.0), ('Can', 0.0)])
    sofa = b.put(r, 'Sofa_03', 3.6, cz, -90.0, collide=True)
    b.supports.append(dict(sofa, y1=sofa['y0'] + 0.45))
    b.put(r, 'Bed_Pillow_B', sofa['x'] + 0.1, sofa['z0'] + 0.45, -80.0, y=0.45, scale=0.6)
    table = b.put(r, 'Sidetable_C', 2.3, cz, 15.0, collide=True)
    for stem, dx, dz, yaw in (('Can', -0.15, -0.12, 0.0), ('Can', 0.1, -0.16, 0.0), ('Bottle_A', 0.16, 0.12, 0.0),
                              ('Bowl_Cereal_Pile', -0.1, 0.12, 0.0), ('Tv_Remote', 0.05, 0.0, 70.0)):
        b.on(r, table, stem, dx=dx, dz=dz, yaw=yaw)
    b.put(r, 'Chair_Office', 2.25, 8.45, -105.0, collide=True)
    # the other couch, against the north wall, slept on: pillow and a towel for a blanket
    couch = b.wall(r, 'N', 1.2, 'Sofa_02', collide=True)
    b.supports.append(dict(couch, y1=couch['y0'] + 0.42))
    b.put(r, 'Bed_Pillow_A', couch['x0'] + 0.35, couch['z'] + 0.05, 95.0, y=0.42, scale=0.6)
    b.put(r, 'Towel_Pile', couch['x'] + 0.2, couch['z'] + 0.08, 30.0, y=0.42)
    litter(b, r, [('Cord_On_Floor_A', 0.75, 6.15, 20.0), ('Cord_On_Floor_B', 1.05, 7.95, -30.0),
                  ('Can', 2.9, 6.1, 0.0, 90.0, 0.0), ('Can', 1.6, 8.6, 0.0), ('Bottle_A', 2.75, 8.95, 40.0, 90.0, 0.0),
                  ('Cardboardbox_Squeezed', 1.35, 9.35, 12.0), ('Magazine_Comic_Kid', 2.0, 6.0, -25.0),
                  ('Papers_Pile_C', 4.6, 5.0, 33.0)])
    b.wall(r, 'S', 3.4, 'Painting_Large_C', gap=0.12, pitch=-12.0)
    b.put(r, 'Laundrybasket', 5.45, 9.55, 20.0)
    b.put(r, 'Towel_Pile', 5.45, 9.55, 20.0, y=0.716)
    b.put(r, 'Garbage_Bag_A', 4.9, 9.62, -30.0)
    b.outlet(r, 'W', cz - 1.1)
    b.outlet(r, 'S', 1.6)


def kitchen(b):
    """Every surface used: the sink full, cans and bottles, the pans left on the hob, mail and takeaway on the table
    with two odd chairs. The tube on the ceiling is on."""
    r = 'Kitchen'
    f = fittings.kitchen(b)
    c1, st, c2, sk, c3 = f['c1'], f['stove'], f['c2'], f['sink'], f['c3']
    for base, items in ((c1, [('Coffeemaker', -0.28, -0.07, 0.0), ('Coffee_Mug', 0.0, 0.05, 140.0),
                              ('Can', 0.2, -0.15, 0.0), ('Can', 0.32, -0.08, 0.0), ('Bottle_A', 0.4, -0.2, 0.0)]),
                        (st, [('Frying_Pan', -0.22, 0.1, 50.0), ('Pot_Kitchen', 0.22, 0.05, 0.0)]),
                        (c2, [('Macaroni_Package', -0.3, -0.15, -70.0), ('Bowl_Pile', 0.0, 0.0, 0.0),
                              ('Coffee_Mug', 0.25, 0.1, 30.0), ('Bottle_A', 0.4, -0.18, 0.0)]),
                        (sk, [('Plate_Pile', -0.1, 0.0, 0.0), ('Handsoap', -0.38, -0.17, 0.0)]),
                        (c3, [('Glass_Drinking_Pile', -0.3, -0.1, 0.0), ('Bowl_Cereal_Pile', 0.0, 0.05, 0.0),
                              ('Can', 0.25, -0.15, 0.0), ('Macaroni_Package', 0.35, 0.1, 20.0)])):
        for stem, dx, dz, yaw in items:
            b.on(r, base, stem, dx=dx, dz=dz, yaw=yaw)
    tx, tz = 7.9, 8.1
    tb = fittings.kitchen_table(b, ('W',), tx, tz, turn={'W': 20.0})
    b.put(r, 'Chair_Workdesk', tx + 0.1, tz + 0.85, 165.0, collide=True)
    for stem, dx, dz, yaw in (('Papers_Pile_A', -0.3, 0.2, 20.0), ('Can', 0.1, -0.3, 0.0), ('Can', 0.3, 0.25, 0.0),
                              ('Bowl_Cereal', -0.05, -0.35, 0.0), ('Glass_Drinking', 0.35, -0.1, 0.0),
                              ('Papers_Folder_B', 0.0, 0.3, -40.0)):
        b.on(r, tb, stem, dx, dz, yaw=yaw)
    b.put(r, 'Garbage_Bag_A', 7.95, 9.6, 10.0)
    b.put(r, 'Garbage_Bag_B', 8.5, 9.62, -40.0)
    tube = b.wbox('Lamp_Fluorescent_B', 0.0)
    b.put(r, 'Lamp_Fluorescent_B', 8.2, 7.6, 0.0, H - (tube[3] - tube[2]), **b.lamp_glow('Lamp_Fluorescent_B', 1.3))
    b.light(r, 'Tube', (8.2, H - 0.1, 7.6), 1.2, 5.0, TUBE, spot=80.0, fill=0.14)


def master(b):
    """The one who got the big room: a double bed against the west wall, a box for a nightstand, the desk on the
    south wall with its lamp left on, clothes on the floor."""
    r = 'Master Bedroom'
    bed = b.wall(r, 'W', 2.0, 'Bed', collide=True)
    ns = b.wall(r, 'W', bed['z0'] - 0.3, 'Cardboard_Box_B', collide=True)
    b.on(r, ns, 'Clock_Table_Digital', dx=-0.05, yaw=-20.0)
    b.on(r, ns, 'Can', dx=0.1, dz=0.12)
    desk = b.wall(r, 'E', 1.4, 'Workdesk_Assembled', collide=True)
    b.put(r, 'Chair_Workdesk', desk['x0'] - 0.3, 1.45, 90.0, collide=True)
    b.table_lamp(r, desk, 'Lamp_Table_A_Assembled', True, dx=0.6, dz=-0.12, intensity=0.7, rng=3.6)
    for stem, dx, dz, yaw in (('Papers_Pile_B', -0.3, 0.05, 10.0), ('Coffee_Mug', 0.1, 0.12, 0.0),
                              ('Can', 0.3, -0.1, 0.0), ('Pen_A', -0.05, 0.15, 60.0),
                              ('Folders_Pile_A', -0.6, -0.1, 0.0)):
        b.on(r, desk, stem, dx=dx, dz=dz, yaw=yaw)
    litter(b, r, [('Towel_Pile', 2.75, 1.2, 35.0), ('Towel_Pile', 3.0, 2.6, -20.0), ('Can', 2.6, 3.2, 0.0),
                  ('Cardboardbox_Squeezed', 3.0, 2.85, 15.0)])
    b.put(r, 'Laundrybasket', 3.6, 0.35, -15.0)
    stack = b.put(r, 'Cardboard_Box_D', 2.9, 0.45, 5.0, collide=True)
    b.on(r, stack, 'Cardboard_Box_C', yaw=-10.0)


def bedroom2(b):
    """The second roommate: a single bed, a dresser, and the boxes he moved in with still stacked by the wall."""
    r = 'Bedroom 2'
    bed = b.wall(r, 'E', 1.5, 'Bed_Single_Person', collide=True)
    b.put(r, 'Towel_Pile', bed['x'] - 0.2, bed['z'] + 0.1, 60.0, y=0.52)
    b.supports.append(dict(bed, y1=bed['y0'] + 0.52))
    dr = b.wall(r, 'S', 9.3, 'Dresser_B_Assembled', collide=True)
    b.on(r, dr, 'Bottle_A', dx=-0.25)
    b.on(r, dr, 'Clock_Table_Digital', dx=0.05, dz=0.05, yaw=10.0)
    b.on(r, dr, 'Can', dx=0.25, dz=0.0)
    b1 = b.wall(r, 'W', 1.0, 'Cardboard_Box_D', collide=True)
    b2 = b.on(r, b1, 'Cardboard_Box_E', yaw=6.0)
    b.on(r, b2, 'Cardboard_Box_A', yaw=-10.0)
    b.wall(r, 'W', 1.6, 'Cardboard_Box_C', collide=True, extra_yaw=90.0)
    b.put(r, 'Storage_Box_A', 7.4, 2.4, 15.0, collide=True)
    litter(b, r, [('Towel_Pile', 8.1, 2.8, 10.0), ('Book_Pile_A', 7.3, 1.75, 30.0)])


def junk(b):
    """The third bedroom, the junk room: tyres, tools, paint, crates and the boxes nobody claims."""
    r = 'Kids Bedroom'
    sh = b.wall(r, 'W', 1.3, 'Shelf_Standing_A', collide=True)
    b.shelf_row(r, sh, 0, [('Paintbucket_A', 0.0), ('Paintbucket_B', 0.0)])
    b.shelf_row(r, sh, 1, [('Spray_Can_Set_A', 0.0), ('Storage_Box_Cylindrical', 0.0), ('Wrench', 90.0)])
    b.shelf_row(r, sh, 2, [('Storage_Box_B', 90.0), ('Spray_Can_F', 0.0)])
    b.shelf_row(r, sh, 3, [('Cardboard_Box_A', 90.0)])
    b.put(r, 'Car_Tire_Pile', 12.45, 0.6, 0.0, collide=True)
    b.put(r, 'Car_Tire_Solo', 12.42, 1.45, 70.0, pitch=-75.0, collide=True)
    b.put(r, 'Garage_Trolley', 11.6, 0.45, 180.0, collide=True)
    crate = b.put(r, 'Milk_Crate', 12.6, 3.8, 10.0, collide=True)
    b.on(r, crate, 'Milk_Crate', yaw=-15.0, collide=True)
    b.put(r, 'Paintbucket_Set_A', 11.3, 1.9, 25.0, collide=True)
    b.put(r, 'Jerrycan', 11.4, 3.0, 80.0, collide=True)
    b.wall(r, 'E', 2.6, 'Cardboard_Box_Covered', collide=True)
    b.put(r, 'Shovel', 10.35, 2.35, 90.0, pitch=-8.0)


def ensuite(b):
    r = 'Ensuite'
    f = fittings.ensuite(b)
    vn = f['vanity']
    for stem, dx, dz, yaw in (('Toothbrush_A', -0.32, -0.15, 80.0), ('Toothbrush_B', -0.12, -0.17, 70.0),
                              ('Toothpaste', 0.05, -0.12, 30.0), ('Spray_Can_A', 0.35, -0.18, 0.0),
                              ('Handsoap', 0.5, -0.17, 0.0)):
        b.on(r, vn, stem, dx=dx, dz=dz, yaw=yaw)
    b.on(r, f['soap'], 'Shampoo_Bottle_A', dx=0.0)
    litter(b, r, [('Towel_Pile', 5.2, 1.0, 25.0)])


def bathroom(b):
    r = 'Bathroom'
    vn = fittings.bathroom(b)['vanity']
    b.on(r, vn, 'Shampoo_Bottle_B', dx=0.55, dz=-0.15)
    b.on(r, vn, 'Toothbrush_A', dx=-0.3, dz=-0.15, yaw=80.0)
    b.wall(r, 'W', 7.6, 'Towel_Hanging_Small', y=0.85, gap=0.04)
    b.put(r, 'Toiletpaper_Set', 10.62, 7.42, 90.0)
    b.put(r, 'Laundrybasket', 11.8, 7.95, -10.0)


def laundry(b):
    r = 'Laundry'
    x0, z0, x1, z1 = inner(r)
    f = fittings.laundry(b)
    b.on(r, f['washer'], 'Towel_Pile', dx=-0.05, yaw=20.0)
    b.put(r, 'Laundrybasket', x0 + 0.9, z1 - 0.3, 10.0)
    b.put(r, 'Towel_Pile', x0 + 0.9, z1 - 0.3, 10.0, y=0.716)
    b.put(r, 'Garbage_Bag_A', x1 - 0.27, z0 + 0.2, 0.0)
    b.put(r, 'Garbage_Bag_B', x1 - 0.3, z0 + 0.85, -20.0)


def hall(b):
    """Boxes and a bin bag waiting by the front door for someone to take them down."""
    r = 'Hall'
    box = b.wall(r, 'S', 9.95, 'Cardboard_Box_D', collide=True, extra_yaw=90.0)
    b.on(r, box, 'Cardboard_Box_A', yaw=12.0)
    b.put(r, 'Garbage_Bag_B', 9.35, 5.14, 0.0)
    b.put(r, 'Cardboardbox_Squeezed', 8.6, 4.8, 80.0, scale=0.9)


def furnish(b):
    for f in (living, kitchen, master, bedroom2, junk, ensuite, bathroom, laundry, hall):
        f(b)
    fittings.switches(b)
    fittings.ceilings(b, skip={'Kitchen'})
    b.windows(moonlit={'Living Room'})
