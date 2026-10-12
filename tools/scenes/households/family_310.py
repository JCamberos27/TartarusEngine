# 310: the family. Mum, dad and a young kid; a lived-in, faithful, slightly worn home, everyone asleep but the TV.
# The player starts by their front door and the Combine squad is already inside.
import json
import math

import scene_kit as k
from apartment_kit import BOARDS, H, LAMP, MODES, TUBE, TV, WARM, inner
from households import fittings

PAINT = {'Master Bedroom': 'wall_beige', 'Bedroom 2': 'wall_sage', 'Kids Bedroom': 'wall_blue', 'Ensuite': 'tile_wall',
         'Bathroom': 'tile_wall', 'Laundry': 'wall_white'}
FLOORS = {'Master Bedroom': 'laminate', 'Bedroom 2': 'laminate', 'Kids Bedroom': 'laminate', 'Ensuite': 'tile',
          'Bathroom': 'tile', 'Laundry': 'tile', 'Kitchen': 'strip'}


def living(b):
    """Living room: the TV on the west wall, the sofa facing it with its back to the kitchen opening (the walk
    from the hall to the kitchen and the master door runs behind and beside it), a console behind the sofa, a
    reading lamp at its end, armchairs angled in, the bookshelf on the long south wall."""
    r = 'Living Room'
    x0, z0, x1, z1 = inner(r)
    cz = 7.1
    tvs = b.wall(r, 'W', cz, 'Tv_Stand_Assembled', collide=True)
    tv = b.on(r, tvs, 'Tv', dz=-0.04)
    # the picture: an emissive panel on the CRT's face, lit for the night's late show
    tx, tz = b.frame_of(tv, 0.0, (tv['x1'] - tv['x0']) / 2 + 0.004)
    b.s.box('TV Picture', b.group(r, b.group('Furniture')), b.pos((tx, tv['y0'] + 0.31, tz)),
            (0.004, 0.4, 0.6), b.mats['screen'], collider=False, static=True, castShadows=0)
    b.light(r, 'TV', (tx + 0.35, tv['y0'] + 0.3, tz), 0.9, 5.0, TV, spot=75.0, pitch=0.0, yaw=-90.0,
            flicker={'Mode': MODES['Tv'], 'MinInterval': 0.12, 'MaxInterval': 1.4, 'Depth': 0.55, 'Seed': 7},
            fill=0.12)
    b.shelf_row(r, tvs, 0, [('Books_E', 0.0), ('Books_F', 0.0), ('', 0.0), ('Decorative_Bowl', 0.0)], dz=0.0)
    b.hang(r, 'W', cz, 'Painting_Large_B', 1.75)
    b.put(r, 'Carpet_C', 2.25, cz, 90.0)
    couch = b.put(r, 'Couch', 3.45, cz, -90.0, collide=True)
    # cushions at each end of the sofa, a throw left over its arm
    b.supports.append(dict(couch, y1=couch['y0'] + 0.45))
    for dz, yaw in ((-0.62, -80.0), (0.6, -100.0)):
        b.put(r, 'Pillow', couch['x'] - 0.12, cz + dz, yaw, y=0.45, scale=0.7)
    b.put(r, 'Towel_Pile', couch['x'] + 0.05, cz + 0.1, -70.0, y=0.45)
    coffee = b.put(r, 'Sidetable_C', 2.3, cz, 0.0, collide=True)
    b.on(r, coffee, 'Tv_Remote', dx=0.1, dz=0.12, yaw=35.0)
    b.on(r, coffee, 'Coffee_Mug', dx=-0.14, dz=-0.1, yaw=200.0)
    b.on(r, coffee, 'Magazine_Comic_Kid', dx=0.06, dz=-0.08, yaw=-15.0)
    b.on(r, coffee, 'Bowl', y=BOARDS['Sidetable_C'][0])
    b.supports.append(dict(coffee, y1=coffee['y0'] + BOARDS['Sidetable_C'][0]))
    # a console behind the sofa: lamp (off), flowers, the family Bible, a photo
    console = b.put(r, 'Sidetable_A', couch['x1'] + 0.24, cz, -90.0, collide=True)
    b.table_lamp(r, console, 'Lamp_Table_A_Assembled', False, dx=-0.32)
    b.on(r, console, 'Tulips_A', dx=0.32, dz=-0.04)
    b.on(r, console, 'Holy_Bible', dx=0.02, dz=0.03, yaw=12.0)
    b.on(r, console, 'Painting_Table_A', dx=0.15, dz=-0.08, yaw=180.0)
    # reading lamp at the sofa's south end: one of the few lights on
    fl = b.put(r, 'Lamp_Floor_A_Assembled', couch['x'] - 0.05, couch['z1'] + 0.3, 0.0, castShadows=0,
               **b.lamp_glow('Lamp_Floor_A_Assembled', 0.9))
    b.light(r, 'Floor Lamp', (fl['x'], fl['y1'] - 0.2, fl['z']), 1.1, 4.5, LAMP)
    b.put(r, 'Arm_Chair_01', 1.75, 5.75, 35.0, collide=True)
    b.put(r, 'Chair_Arm', 1.55, 8.95, 150.0, collide=True)
    # the kid's things drift in: a car and blocks on the rug
    b.put(r, 'Toy_Car_C', 2.85, 6.25, 60.0)
    b.put(r, 'Toy_Block_A', 1.75, 7.95, 20.0)
    b.put(r, 'Toy_Block_C', 1.9, 8.05, -35.0)
    bs = b.wall(r, 'S', 3.15, 'Bookshelf', collide=True)
    b.shelf_row(r, bs, 0, [('Cardboard_Box_A', 90.0), ('Book_Pile_A', 90.0), ('Books_A', 0.0)], dz=0.0)
    b.shelf_row(r, bs, 1, [('Books_B', 0.0), ('Books_C', 0.0), ('Painting_Table_B', 0.0),
                           ('Vase_A', 0.0)], dz=0.02)
    b.shelf_row(r, bs, 2, [('Candle_Holder_B', 0.0), ('Books_D', 0.0), ('Books_E', 0.0), ('Books_F', 0.0)],
                dz=0.02)
    b.shelf_row(r, bs, 3, [('Books_A', 0.0), ('Painting_Table_E', 0.0), ('Decorative_Plate', 0.0)],
                dz=0.02)
    # a small shrine on the south wall: a half-moon table, the statue, candles, the crucifix above
    shrine = b.wall(r, 'S', 5.0, 'Sidetable_B', collide=True)
    b.on(r, shrine, 'David', scale=0.16)
    b.on(r, shrine, 'Candle_A', dx=-0.3, dz=0.02)
    b.on(r, shrine, 'Candle_B', dx=0.3, dz=0.02)
    b.hang(r, 'S', 5.0, 'Cross', 1.55)
    b.wall(r, 'S', 4.15, 'Plant_A', gap=0.1, collide=True)
    # the sideboard on the north wall under the big painting: photos, a candle, a plant
    sb = b.wall(r, 'N', 1.2, 'Dresser_B_Assembled', collide=True)
    b.on(r, sb, 'Painting_Table_B', dx=-0.28, dz=-0.06)
    b.on(r, sb, 'Painting_Table_D', dx=-0.05, dz=-0.1, yaw=10.0)
    b.on(r, sb, 'Candle_Holder_A', dx=0.15, dz=-0.05)
    b.on(r, sb, 'Plant_B', dx=0.3)
    b.hang(r, 'N', 1.2, 'Painting_Large_A', 1.65)
    b.put(r, 'Plant_B', 5.6, 9.6)
    b.hang(r, 'N', 5.2, 'Clock_Wall_Analog', 2.0)
    b.outlet(r, 'W', cz - 1.1)
    b.outlet(r, 'S', 1.6)
    b.outlet(r, 'N', 4.1)
    b.ceiling_fixture(r, 'Lamp_Ceiling_B', 2.4, cz)
    b.ceiling_fixture(r, 'Lamp_Ceiling_B', 5.0, 4.85)

def kitchen(b):
    """Kitchen: fridge, counter, stove, counter along the north wall and the sink run on the east wall - the
    fridge -> sink -> stove triangle - uppers above, the pantry by the opening, the table in the middle with
    last night's dishes still on it. Only the cooker hood's light is on, and one dim bulb over the table."""
    r = 'Kitchen'
    x0, z0, x1, z1 = inner(r)

    f = fittings.kitchen(b)
    c1, st, c2, sk, c3 = f['c1'], f['stove'], f['c2'], f['sink'], f['c3']
    fittings.hood_light(b, f)
    # coffee corner on the left counter, against the backsplash
    b.on(r, c1, 'Coffeemaker', dx=-0.28, dz=-0.07)
    b.on(r, c1, 'Coffee_Mug', dx=-0.02, dz=0.02, yaw=160.0)
    b.on(r, c1, 'Coffee_Mug', dx=0.1, dz=-0.12, yaw=40.0)
    b.on(r, c1, 'Bottle_A', dx=0.38, dz=-0.18)
    b.on(r, c1, 'Can', dx=0.27, dz=-0.17)
    # cooking: pot and pan on the hob, utensils by it, a knife left out by a plate
    b.on(r, st, 'Cooking_Pot', dx=-0.22, dz=0.08)
    b.on(r, st, 'Frying_Pan', dx=0.22, dz=0.12, yaw=30.0)
    b.on(r, c2, 'Utensil_Holder_Full', dx=-0.38, dz=-0.18)
    b.on(r, c2, 'Plate_Single', dx=-0.1, dz=0.08)
    b.on(r, c2, 'Knife', dx=-0.1, dz=0.08, yaw=70.0, y=c2['y1'] - c2['y0'] + 0.021)
    b.on(r, c2, 'Cooking_Spatula', dx=0.2, dz=0.1, yaw=95.0)
    b.on(r, c2, 'Jar_A', dx=0.18, dz=-0.18)
    b.on(r, c2, 'Jar_B', dx=0.3, dz=-0.18)
    b.on(r, c2, 'Pepper_Mill', dx=0.4, dz=-0.15)
    # the sink run: soap, and the drying dishes on the counter beside it
    b.on(r, sk, 'Handsoap', dx=-0.38, dz=-0.17)
    b.on(r, c3, 'Plate_Pile', dx=-0.2, dz=-0.05)
    b.on(r, c3, 'Bowl_Pile', dx=0.15, dz=-0.12)
    b.on(r, c3, 'Glass_Drinking_Pile', dx=0.36, dz=0.1)
    b.on(r, c3, 'Teapot', dx=-0.35, dz=0.12, yaw=-60.0)
    b.on(r, c3, 'Macaroni_Package', dx=0.38, dz=-0.2, yaw=-80.0)
    b.put(r, 'Garbage_Bag_A', x0 + 1.95, z1 - 0.3, 25.0)
    # the table: breakfast never cleared - two cereal bowls, a glass, the mail
    tx, tz = 7.9, 8.1
    tb = fittings.kitchen_table(b, ('W', 'S', 'E'), tx, tz, turn={'S': -8.0, 'W': 15.0})
    b.on(r, tb, 'Bowl_Cereal', 0.0, -0.42)
    b.on(r, tb, 'Bowl_Cereal', -0.42, 0.05)
    b.on(r, tb, 'Glass_Drinking', -0.2, -0.38)
    b.on(r, tb, 'Papers_Pile_A', 0.35, 0.25, yaw=25.0)
    b.on(r, tb, 'Decorative_Bowl', 0.05, 0.08)
    chand = b.wbox('Lamp_Chandelier_A_Assembled', 0.0)
    b.put(r, 'Lamp_Chandelier_A_Assembled', tx, tz, 0.0, H - (chand[3] - chand[2]),
          **b.lamp_glow('Lamp_Chandelier_A_Assembled', 0.45))
    b.light(r, 'Table', (tx, H - (chand[3] - chand[2]) + 0.25, tz), 0.55, 3.2, WARM, spot=58.0, fill=0)
    b.ceiling_fixture(r, 'Lamp_Ceiling_A', 8.2, 6.3)
    b.hang(r, 'S', 9.3, 'Painting_Medium_E', 1.5)
    b.hang(r, 'W', 9.35, 'Clock_Wall_Analog', 1.95)

def master(b):
    """Master bedroom: the bed's headboard on the west wall facing the door, a nightstand each side (his lamp on,
    hers off with the Bible), the dresser opposite on the north wall, the long mirror by the ensuite."""
    r = 'Master Bedroom'
    x0, z0, x1, z1 = inner(r)
    bed = b.wall(r, 'W', 2.0, 'Bed', collide=True)
    b.put(r, 'Carpet_A', bed['x1'] - 0.35, 2.0, 90.0)
    ns_n = b.wall(r, 'W', bed['z0'] - 0.34, 'Nightstand_B_Assembled', collide=True)
    ns_s = b.wall(r, 'W', bed['z1'] + 0.34, 'Nightstand_B_Assembled', collide=True)
    b.table_lamp(r, ns_n, 'Lamp_Table_B_Assembled', False, dz=-0.06)
    b.on(r, ns_n, 'Vial_Pain_Killer_02', dx=0.15, dz=0.14)
    b.table_lamp(r, ns_s, 'Lamp_Table_B_Assembled', True, dz=-0.06, intensity=0.8, rng=3.8)
    b.on(r, ns_s, 'Clock_Table_Digital', dx=-0.13, dz=0.14, yaw=-20.0)
    b.on(r, ns_s, 'Glass_Drinking', dx=0.16, dz=0.14)
    b.hang(r, 'W', 2.0, 'Cross', 1.62)
    dr = b.wall(r, 'E', 1.15, 'Dresser_C_Assembled', collide=True)
    b.on(r, dr, 'Vase_B', dx=-0.66, dz=-0.04)
    b.on(r, dr, 'Vase_B_Sticks', dx=-0.5, dz=-0.08)
    b.on(r, dr, 'Money_Usa', dx=-0.22, dz=-0.1, yaw=84.0, scale=0.5)  # half behind the photo
    b.on(r, dr, 'Painting_Table_C', dx=0.1, dz=-0.09)
    b.on(r, dr, 'Holy_Bible', dx=0.35, dz=0.08, yaw=-6.0)
    b.on(r, dr, 'Candle_Holder_B', dx=0.65, dz=-0.05)
    b.hang(r, 'E', 1.15, 'Painting_Large_C', 1.6)
    b.wall(r, 'N', 3.4, 'Mirror_Body', gap=0.06, collide=True)
    b.put(r, 'Chair_Arm', 3.68, 2.45, -100.0, collide=True)
    b.supports.append({'x0': 3.2, 'x1': 4.2, 'z0': 2.0, 'z1': 2.9, 'y0': 0.0, 'y1': 0.45, 'unit': b.unit})
    b.put(r, 'Pillow', 3.75, 2.45, -100.0, y=0.45, scale=0.7)
    b.put(r, 'Laundrybasket', 3.9, 3.7, 12.0)
    b.put(r, 'Towel_Pile', 3.9, 3.7, 12.0, y=0.716)
    b.hang(r, 'S', 1.3, 'Painting_Medium_B', 1.5)
    b.outlet(r, 'W', bed['z0'] - 0.7)
    b.outlet(r, 'N', 1.4)
    b.ceiling_fixture(r, 'Lamp_Ceilingfan', 2.4, 2.1)

def ensuite(b):
    """Ensuite: shower across the north end, the double vanity on the west wall under the mirror, the toilet on
    the east wall with the paper beside it, a towel rail, a shelf of towels. Its light is on and the door
    ajar, so it spills into the dark bedroom."""
    r = 'Ensuite'
    x0, z0, x1, z1 = inner(r)
    f = fittings.ensuite(b)
    soap, vn = f['soap'], f['vanity']
    b.on(r, soap, 'Shampoo_Bottle_A', dx=-0.06)
    b.on(r, soap, 'Shampoo_Bottle_B', dx=0.06, yaw=20.0)
    b.on(r, vn, 'Toothbrush_Mug', dx=-0.05, dz=-0.17)
    b.on(r, vn, 'Toothpaste', dx=0.1, dz=-0.2, yaw=0.0)
    b.on(r, vn, 'Handsoap', dx=0.32, dz=-0.18)
    b.on(r, vn, 'Lotiontube', dx=0.46, dz=-0.17, yaw=30.0)
    b.on(r, vn, 'Syringe_Variation_01', dx=-0.55, dz=0.12, yaw=60.0)
    b.on(r, vn, 'Vial_Insulin_01', dx=-0.48, dz=0.0)
    # the room's own ceiling light is the one on: a downlight from the middle of the ceiling, kept off the walls
    b.ceiling_fixture(r, 'Lamp_Ceiling_A', 5.5, 2.2, on=True, glow=0.45, name='Ceiling', intensity=1.0, rng=4.0,
                      color=(1.0, 0.86, 0.68), spot=55.0, fill=0)
    unit = b.wall(r, 'E', 2.45, 'Bathroom_Shelf_Unit_B', collide=True)
    b.shelf_row(r, unit, 1, [('Bathroom_Box_A', 90.0)])
    b.shelf_row(r, unit, 2, [('Towel_Pile', 90.0)])
    b.shelf_row(r, unit, 0, [('Toiletpaper_Set', 90.0)])
    b.shelf_row(r, unit, 3, [('Towel_Roll_Set', 90.0)])
    b.wall(r, 'E', 1.55, 'Towel_Hanging_Large', y=0.75, gap=0.04)
    b.put(r, 'Carpet_D', 5.3, 1.5, 90.0, scale=0.55)

def bedroom2(b):
    """Guest room / home office: the single bed along the north wall, the desk against the west wall with the
    chair tucked in and the desk lamp on, a dresser, and the boxes nobody unpacked along the south wall."""
    r = 'Bedroom 2'
    x0, z0, x1, z1 = inner(r)
    bed = b.wall(r, 'N', x1 - 0.65, 'Bed_Single_Person', collide=True)
    ns = b.wall(r, 'N', bed['x0'] - 0.26, 'Nightstand_B_Assembled', collide=True)
    b.table_lamp(r, ns, 'Lamp_Table_A_Assembled', False, dz=-0.04)
    desk = b.wall(r, 'W', 1.55, 'Workdesk_Assembled', collide=True)
    b.put(r, 'Chair_Office', desk['x1'] + 0.12, 1.55, -95.0, collide=True)
    b.table_lamp(r, desk, 'Lamp_Table_B_Assembled', True, dx=-0.65, dz=-0.15, intensity=0.75, rng=3.5)
    b.on(r, desk, 'Papers_Pile_B', dx=-0.15, dz=0.05, yaw=8.0)
    b.on(r, desk, 'Papers_Holder_A', dx=0.3, dz=-0.2)
    b.on(r, desk, 'Folders_Set_A', dx=0.6, dz=-0.2)
    b.on(r, desk, 'Pen_Holder_With_Pens', dx=0.12, dz=-0.22)
    b.on(r, desk, 'Scissors', dx=0.2, dz=0.15, yaw=40.0)
    b.on(r, desk, 'Pencil_A', dx=0.4, dz=0.2, yaw=100.0)
    b.on(r, desk, 'Coffee_Mug', dx=-0.42, dz=0.1)
    dr = b.wall(r, 'E', 2.95, 'Dresser_B_Assembled', collide=True)
    b.on(r, dr, 'Folders_Pile_A', dx=-0.22, yaw=8.0)
    b.on(r, dr, 'Books_D', dx=0.15, dz=-0.05)
    b.on(r, dr, 'Plant_B', dx=0.35)
    # the boxes: stacked two high along the south wall, past the door's swing
    b1 = b.wall(r, 'S', 9.55, 'Cardboard_Box_D', collide=True)
    b.on(r, b1, 'Cardboard_Box_A', yaw=10.0)
    b2 = b.wall(r, 'S', 9.0, 'Storage_Box_A', collide=True, extra_yaw=90.0)
    b.on(r, b2, 'Papers_Box_C', yaw=-8.0)
    b.put(r, 'Cardboard_Box_B', 8.55, z1 - 0.3, 75.0)
    b.put(r, 'Carpet_B', 8.05, 2.55, 0.0)
    b.hang(r, 'W', 1.55, 'Painting_Medium_D', 1.55)
    b.hang(r, 'N', bed['x0'] - 0.26, 'Painting_Small_C', 1.45)
    b.outlet(r, 'W', 2.65)
    b.outlet(r, 'N', 7.4)
    b.ceiling_fixture(r, 'Lamp_Ceiling_A', 8.3, 2.1)

def kids(b):
    """Kid's room: bunk bed in the corner, play space on the rug in the middle (toys out), dresser with the
    rocket nightlight - the only light, very low - posters, a little table with drawings."""
    r = 'Kids Bedroom'
    x0, z0, x1, z1 = inner(r)
    bunk = b.wall(r, 'N', x1 - 0.76, 'Bed_Bunk_Assembled', collide=True)
    mattress = dict(bunk, y1=0.438)  # the lower bunk's mattress top (measured on the model)
    b.supports.append(mattress)
    b.put(r, 'Bed_Pillow_A', (bunk['x0'] + bunk['x1']) / 2 + 0.05, bunk['z0'] + 0.3, 0.0, y=0.438, scale=0.7)
    b.put(r, 'Toy_Teddy', (bunk['x0'] + bunk['x1']) / 2 + 0.05, bunk['z0'] + 0.75, 160.0, y=0.438, scale=0.8)
    dr = b.wall(r, 'W', 1.05, 'Dresser_D_Assembled', collide=True)
    b.table_lamp(r, dr, 'Lamp_Table_Rocket_Assembled', True, dx=0.15, intensity=0.28, rng=3.0, glow=0.35)
    b.on(r, dr, 'Toy_Dinosaur', dx=-0.18, yaw=60.0)
    b.put(r, 'Carpet_Kid', 11.45, 2.35, 0.0)
    tb = b.wall(r, 'S', 12.35, 'Toy_Box_A', collide=True)
    b.put(r, 'Toy_Box_A_Lid', tb['x0'] - 0.3, tb['z0'] + 0.12, 70.0)
    b.put(r, 'Toy_Racetrack', 11.2, 2.55, 20.0)
    b.put(r, 'Toy_Car_A', 11.0, 2.25, 75.0)
    b.put(r, 'Toy_Car_B', 11.6, 3.0, -30.0)
    b.put(r, 'Toy_Truck', 10.7, 3.35, 140.0)
    b.put(r, 'Toy_Block_Set_A', 12.0, 3.05, 10.0)
    b.put(r, 'Toy_Block_B', 12.25, 2.75, 40.0)
    b.put(r, 'Toy_Muscleman', 10.9, 1.75, -60.0)
    b.put(r, 'Paper_Kid', 11.85, 1.95, 25.0)
    b.put(r, 'Book_Drawing_Kid', 10.95, 3.25, -40.0)
    b.put(r, 'Notebook_Kid', 11.3, 1.55, 22.0)
    st = b.put(r, 'Sidetable_C', 10.55, 2.75, 90.0, collide=True)
    b.on(r, st, 'Paper_Kid', dx=-0.08, dz=0.05, yaw=8.0)
    b.on(r, st, 'Pen_Set_A_Kid', dx=0.14, dz=-0.12)
    b.on(r, st, 'Jar_Kid', dx=0.15, dz=0.14)
    b.hang(r, 'W', 2.65, 'Poster_Kid_A', 1.5)
    b.hang(r, 'E', 3.2, 'Poster_Kid_C', 1.45)
    b.hang(r, 'S', 11.85, 'Poster_Kid_E', 1.55)
    b.hang(r, 'W', 1.05, 'Poster_Kid_F', 1.65)
    b.outlet(r, 'W', 3.4)
    b.ceiling_fixture(r, 'Lamp_Ceiling_A', 11.5, 2.1)

def bathroom(b):
    """Main bathroom: shower on the east wall, vanity on the south wall, toilet on the west. The light is off -
    a black room behind a closed door."""
    r = 'Bathroom'
    x0, z0, x1, z1 = inner(r)
    vn = fittings.bathroom(b)['vanity']
    b.on(r, vn, 'Toothbrush_A', dx=-0.3, dz=-0.15, yaw=80.0)
    b.on(r, vn, 'Handsoap', dx=0.1, dz=-0.18)
    b.on(r, vn, 'Shampoo_Bottle_B', dx=0.55, dz=-0.15)
    b.put(r, 'Toiletpaper_Set', 10.62, 7.42, 90.0)
    b.wall(r, 'W', 7.6, 'Towel_Hanging_Small', y=0.85, gap=0.04)
    b.put(r, 'Carpet_D', 11.35, 7.15, 0.0, scale=0.55)
    b.put(r, 'Laundrybasket', 11.8, 7.95, -10.0)
    b.ceiling_fixture(r, 'Lamp_Ceiling_A', 11.4, 6.85)

def laundry(b):
    """Laundry / utility: washer against the south wall, the steel shelf of paint, cans and tools, the breaker
    panel. The tube light is on and stutters."""
    r = 'Laundry'
    x0, z0, x1, z1 = inner(r)
    wm = fittings.laundry(b)['washer']
    b.on(r, wm, 'Towel_Roll_Set', dx=-0.1)
    shelf = b.wall(r, 'E', z0 + 0.62, 'Garage_Shelf_A', collide=True)
    b.shelf_row(r, shelf, 0, [('Paintbucket_A', 0.0), ('Paintbucket_B', 0.0), ('Jerrycan', 90.0)])
    b.shelf_row(r, shelf, 1, [('Storage_Box_B', 90.0), ('Spray_Can_Set_A', -90.0), ('Paint_Thinner', -90.0)])
    b.shelf_row(r, shelf, 2, [('Cardboard_Box_A', 90.0), ('Spray_Can_A', 0.0), ('Spray_Can_B', 0.0),
                              ('Wrench', 90.0), ('Screwdriver', 90.0)])
    b.shelf_row(r, shelf, 3, [('Storage_Box_C', 90.0), ('Paintbucket_C', 0.0)])
    b.put(r, 'Laundrybasket', x0 + 0.9, z1 - 0.3, 10.0)
    b.put(r, 'Cooler_Box', x0 + 1.3, z0 + 0.3, 5.0)
    b.put(r, 'Garbage_Bag_B', x0 + 0.75, z0 + 0.32, -20.0)
    tube = b.wbox('Lamp_Fluorescent_A', 0.0)
    b.put(r, 'Lamp_Fluorescent_A', 11.7, 9.15, 0.0, H - (tube[3] - tube[2]),
          **b.lamp_glow('Lamp_Fluorescent_A', 1.2))
    b.light(r, 'Tube', (11.7, H - 0.12, 9.15), 1.1, 4.5, TUBE, spot=80.0, fill=0.12,
            flicker={'Mode': MODES['Fluorescent'], 'MinInterval': 3.0, 'MaxInterval': 11.0, 'Depth': 0.95,
                        'Seed': 3})

def hall(b):
    """Hall: a drop shelf by the front door (keys, mail, a photo), the boxes and bag waiting by the door, a
    family gallery along the long wall, the crucifix over the door. One ceiling light on at the living-room
    end, failing."""
    r = 'Hall'
    x0, z0, x1, z1 = inner(r)
    sh = b.wall(r, 'S', 12.3, 'Shelf', y=1.05, scale=0.8)
    b.on(r, sh, 'Decorative_Bowl', dx=-0.26)
    b.on(r, sh, 'Papers_Pile_A', dx=0.02, yaw=88.0)
    b.on(r, sh, 'Painting_Table_E', dx=0.28, dz=-0.02)
    b.hang(r, 'E', 4.8, 'Cross', 2.25)
    b.put(r, 'Carpet_B', 8.1, 4.8, 90.0, scale=0.8)
    b.put(r, 'Carpet_B', 10.9, 4.8, 90.0, scale=0.8)
    # gallery on the kitchen wall: one centre line at 1.5 m, mixed sizes
    for t, stem in ((6.55, 'Painting_Small_D'), (7.25, 'Painting_Small_A'), (8.05, 'Painting_Medium_G'),
                    (8.85, 'Painting_Small_B'), (9.55, 'Painting_Small_C'), (10.05, 'Painting_Small_E')):
        if stem == 'Painting_Small_E':
            continue
        b.hang(r, 'S', t, stem, 1.5)
    b.hang(r, 'N', 9.15, 'Clock_Wall_Analog', 1.95)
    b.ceiling_fixture(r, 'Lamp_Ceiling_B', 7.4, 4.8, on=True, name='Ceiling', intensity=0.9, rng=4.5,
                      color=(1.0, 0.8, 0.6), spot=70.0,
                      flicker={'Mode': MODES['Fluorescent'], 'MinInterval': 6.0, 'MaxInterval': 18.0, 'Depth': 0.8,
                                  'Seed': 11})
    b.ceiling_fixture(r, 'Lamp_Ceiling_B', 11.5, 4.8)

def player(b):
    # the spawn and the Player Body under it (outfit + first-person body)
    root = b.from_sandbox('Player Spawn')
    root.update({'position': [12.25, 0.1, 4.8], 'rotation': k.yaw_quat(90.0)})

def npcs(b):
    # The Sandbox's Combine squad, moved in: the director (squad rules) and four soldiers in the rooms off the hall, each on
    # the clearest patch of floor in its room and facing the front door. Respawn stays on, so the flat refills.
    b.from_sandbox('AI Director')['position'] = [6.5, 0.0, 7.0]
    spawn = next(e for e in b._sandbox['empties'] if e['name'] == 'Enemy Spawn 1')
    door = (12.25, 4.8)
    for n, (room, weapon, skill) in enumerate((('Living Room', 0, 0.6), ('Kitchen', 1, 0.55),
                                               ('Master Bedroom', 0, 0.65), ('Bedroom 2', 0, 0.6)), 1):
        (x, z), _ = b.clear_spot(room)
        e = json.loads(json.dumps(spawn))
        i = b.s._next()
        fields = json.loads(e['C# Script']['Fields JSON'])
        fields.update({'Weapon': weapon, 'Skill': skill, 'OutfitSeed': n})
        e['C# Script']['Fields JSON'] = json.dumps(fields, separators=(',', ':'))
        yaw = math.degrees(math.atan2(door[0] - x, door[1] - z))
        e.update({'id': i, 'order': i, 'parentId': b.root, 'name': f'Enemy Spawn {n} ({room})',
                  'position': [round(x, 3), 0.1, round(z, 3)], 'rotation': k.yaw_quat(yaw)})
        b.s.empties.append(e)

def furnish(b):
    for f in (living, kitchen, master, ensuite, bedroom2, kids, bathroom, laundry, hall):
        f(b)
    fittings.switches(b)
    # long drapes in the living room and the master, short ones in the kids' rooms; the moon finds three of them
    b.windows(curtains={'Living Room': 'Curtains_A_Singlewindow', 'Master Bedroom': 'Curtains_B_Singlewindow',
                        'Bedroom 2': 'Curtains_D_Singlewindow', 'Kids Bedroom': 'Curtains_D_Singlewindow'},
              moonlit={'Living Room', 'Kids Bedroom'})
    player(b)
    npcs(b)
