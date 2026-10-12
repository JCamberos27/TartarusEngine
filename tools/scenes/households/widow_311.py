# 311: the widow. She has lived alone since her husband died: everything tidy, dated and dusted, the walls crowded
# with photographs and crosses, his study kept exactly as he left it, the spare room full of what she can't throw
# away. She is still up, in her armchair under the lamp, the TV off; the rest of the flat is dark.
from apartment_kit import BOARDS, H, LAMP, WARM, inner
from households import fittings

PAINT = {'Living Room': 'wall_rose', 'Hall': 'wall_rose', 'Kitchen': 'wall_cream', 'Master Bedroom': 'wall_cream',
         'Bedroom 2': 'wall_olive', 'Kids Bedroom': 'wall_white', 'Ensuite': 'tile_wall', 'Bathroom': 'tile_wall',
         'Laundry': 'wall_white'}
FLOORS = {'Living Room': 'hardwood', 'Hall': 'hardwood', 'Kitchen': 'tile', 'Master Bedroom': 'hardwood',
          'Bedroom 2': 'hardwood', 'Kids Bedroom': 'laminate', 'Ensuite': 'tile', 'Bathroom': 'tile', 'Laundry': 'tile'}
# every door shut but her bedroom's
DOORS_OPEN = {'Master Door': 20.0, 'Ensuite Door': 0.0, 'Bedroom 2 Door': 0.0, 'Kids Bedroom Door': 0.0,
              'Bathroom Door': 0.0, 'Laundry Door': 0.0}


def living(b):
    """The TV on the north wall, her armchair facing it by the window with the lamp, the tea and the Bible; the good
    sofa against the south wall for visitors who don't come; a sideboard between the windows under a wall of family
    photographs around a crucifix; the china cabinet."""
    r = 'Living Room'
    tvs = b.wall(r, 'N', 1.25, 'Tv_Stand_Assembled', collide=True)
    b.on(r, tvs, 'Tv', dz=-0.04)
    b.shelf_row(r, tvs, 0, [('Book_Pile_A', 90.0), ('', 0.0), ('Decorative_Plate', 0.0)])
    b.on(r, tvs, 'Painting_Table_F', dx=0.62, dz=-0.05)
    chair = b.put(r, 'Arm_Chair_01', 1.35, 6.45, 180.0, collide=True)
    side = b.put(r, 'Sidetable_C', 0.55, 6.36, 0.0, collide=True)
    b.table_lamp(r, side, 'Lamp_Table_A_Assembled', True, dx=-0.08, dz=-0.08, intensity=0.85, rng=4.0)
    b.on(r, side, 'Coffee_Mug', dx=0.14, dz=0.13, yaw=200.0)
    b.on(r, side, 'Vial_Pain_Killer_02', dx=0.16, dz=-0.12)
    b.on(r, side, 'Holy_Bible', y=BOARDS['Sidetable_C'][0])
    b.supports.append(dict(side, y1=side['y0'] + BOARDS['Sidetable_C'][0]))
    b.put(r, 'Pillow', chair['x'], chair['z'] + 0.08, 180.0, y=0.42, scale=0.75)
    b.supports.append(dict(chair, y1=chair['y0'] + 0.42))
    b.put(r, 'Carpet_A', 2.6, 7.55, 90.0)
    sofa = b.wall(r, 'S', 2.95, 'Chesterfield_Sofa', collide=True)
    b.put(r, 'Bed_Pillow_A', sofa['x0'] + 0.35, sofa['z0'] + 0.5, 190.0, y=0.45, scale=0.55)
    b.supports.append(dict(sofa, y1=sofa['y0'] + 0.45))
    table = b.put(r, 'Sidetable_C', 2.95, 8.25, 0.0, collide=True)
    b.on(r, table, 'Tulips_B', dx=-0.1, dz=-0.05)
    b.on(r, table, 'Decorative_Bowl', dx=0.1, dz=0.1)
    b.put(r, 'Chair_Arm', 4.6, 7.55, -125.0, collide=True)
    b.put(r, 'Lamp_Floor_A_Assembled', 1.45, 9.55, 0.0, **b.lamp_off('Lamp_Floor_A_Assembled'))
    # the sideboard between the windows, and the wall of photographs over it
    sb = b.wall(r, 'W', 7.1, 'Dresser_B_Assembled', collide=True)
    b.on(r, sb, 'Candle_Holder_A', dx=-0.3, dz=-0.05)
    b.on(r, sb, 'Painting_Table_C', dx=-0.08, dz=-0.08)
    b.on(r, sb, 'Painting_Table_G', dx=0.12, dz=-0.06, yaw=-8.0)
    b.on(r, sb, 'Vase_B', dx=0.3, dz=-0.05)
    b.hang(r, 'W', 7.1, 'Cross', 1.32)
    for t, cy, stem in ((6.62, 1.42, 'Painting_Small_C'), (7.58, 1.42, 'Painting_Small_D'),
                        (6.8, 2.02, 'Painting_Small_B'), (7.45, 2.02, 'Painting_Small_E')):
        b.hang(r, 'W', t, stem, cy)
    # the china cabinet in the south-east corner, and a plant by the door to her room
    b.wall(r, 'S', 5.25, 'Cabinet_A_Assembled', collide=True)
    b.wall(r, 'N', 3.95, 'Plant_A', gap=0.06, collide=True)
    b.hang(r, 'N', 5.05, 'Clock_Wall_Analog', 2.0)
    b.hang(r, 'S', 1.0, 'Painting_Medium_I', 1.55)
    b.hang(r, 'S', 2.95, 'Painting_Large_B', 1.65)
    b.outlet(r, 'N', 2.0)
    b.outlet(r, 'W', 6.3)
    b.outlet(r, 'S', 4.2)


def kitchen(b):
    """Her kitchen: canisters in a row, the teapot out, one place set at the table with the other chairs tucked in,
    everything washed and put away. The hood light is left on for her."""
    r = 'Kitchen'
    f = fittings.kitchen(b)
    fittings.hood_light(b, f, intensity=0.8)
    c1, st, c2, sk, c3 = f['c1'], f['stove'], f['c2'], f['sink'], f['c3']
    b.on(r, c1, 'Teapot', dx=-0.25, dz=-0.05, yaw=30.0)
    b.on(r, c1, 'Coffee_Mug', dx=0.05, dz=0.05, yaw=120.0)
    b.on(r, c1, 'Jar_A', dx=0.25, dz=-0.18)
    b.on(r, c1, 'Jar_B', dx=0.38, dz=-0.18)
    b.on(r, st, 'Pot_Kitchen', dx=-0.22, dz=0.1)
    b.on(r, c2, 'Utensil_Holder_Full', dx=-0.38, dz=-0.18)
    b.on(r, c2, 'Jar_C', dx=-0.1, dz=-0.2)
    b.on(r, c2, 'Pepper_Mill', dx=0.05, dz=-0.18)
    b.on(r, c2, 'Kitchen_Items_Set_A', dx=0.0, dz=0.12)
    b.on(r, sk, 'Handsoap', dx=-0.38, dz=-0.17)
    b.on(r, c3, 'Plate_Pile', dx=-0.2, dz=-0.05)
    b.on(r, c3, 'Glass_Drinking_Pile', dx=0.3, dz=-0.1)
    tx, tz = 7.9, 8.1
    # her place (west) pulled out a little; the others tucked in
    tb = fittings.kitchen_table(b, ('W', 'S', 'E'), tx, tz, turn={'W': 12.0})
    b.on(r, tb, 'Plate_Single', -0.4, 0.0)
    b.on(r, tb, 'Glass_Drinking', -0.38, -0.25)
    b.on(r, tb, 'Tulips_A', 0.0, 0.05)
    b.on(r, tb, 'Book_Single_A', -0.25, 0.3, yaw=15.0)
    chand = b.wbox('Lamp_Chandelier_B_Assembled', 0.0)
    b.put(r, 'Lamp_Chandelier_B_Assembled', tx, tz, 0.0, H - (chand[3] - chand[2]),
          **b.lamp_off('Lamp_Chandelier_B_Assembled'))
    b.hang(r, 'S', 9.3, 'Painting_Medium_F', 1.55)
    b.hang(r, 'W', 9.35, 'Clock_Wall_Analog', 1.95)
    b.hang(r, 'W', 5.9, 'Cross', 1.75)


def master(b):
    """Her bedroom: the bed's headboard on the east wall, her nightstand on the window side, the dresser of photos
    of him, her chair in the window corner."""
    r = 'Master Bedroom'
    bed = b.wall(r, 'E', 1.75, 'Bed', collide=True)
    ns = b.wall(r, 'E', bed['z0'] - 0.26, 'Nightstand_B_Assembled', collide=True)
    b.table_lamp(r, ns, 'Lamp_Table_B_Assembled', False, dz=-0.06)
    b.on(r, ns, 'Glass_Drinking', dx=0.12, dz=0.12)
    b.hang(r, 'E', 1.75, 'Cross', 1.55)
    b.put(r, 'Carpet_D', 1.45, 1.75, 90.0)
    dr = b.wall(r, 'W', 2.65, 'Dresser_C_Assembled', collide=True)
    b.on(r, dr, 'Painting_Table_A', dx=-0.6, dz=-0.08, yaw=-10.0)
    b.on(r, dr, 'Painting_Table_C', dx=-0.3, dz=-0.1)
    b.on(r, dr, 'Painting_Table_D', dx=-0.02, dz=-0.08, yaw=12.0)
    b.on(r, dr, 'Candle_Holder_B', dx=0.3, dz=-0.05)
    b.on(r, dr, 'Holy_Bible', dx=0.52, dz=0.08, yaw=8.0)
    b.on(r, dr, 'Vase_B_Sticks', dx=0.68, dz=-0.08)
    b.hang(r, 'W', 2.65, 'Painting_Medium_C', 1.75)
    b.put(r, 'Chair_Arm', 0.75, 0.95, 135.0, collide=True)
    b.put(r, 'Towel_Pile', 0.8, 0.95, 135.0, y=0.45)
    b.supports.append({'x0': 0.4, 'x1': 1.2, 'z0': 0.6, 'z1': 1.3, 'y0': 0.0, 'y1': 0.45, 'unit': b.unit})
    b.hang(r, 'S', 1.3, 'Painting_Medium_J', 1.55)
    b.outlet(r, 'N', 3.6)
    b.outlet(r, 'W', 1.2)


def study(b):
    """His study, kept as he left it: the desk on the east wall with his chair, his books floor to ceiling on the
    west wall, the boxes she started packing and stopped."""
    r = 'Bedroom 2'
    desk = b.wall(r, 'E', 2.0, 'Desk_Office_Assembled', collide=True)
    b.put(r, 'Chair_Office', desk['x0'] - 0.25, 2.05, 95.0, collide=True)
    b.table_lamp(r, desk, 'Lamp_Table_B_Assembled', False, dx=-0.65, dz=-0.2)
    b.on(r, desk, 'Papers_Pile_A', dx=-0.1, dz=0.05, yaw=-6.0)
    b.on(r, desk, 'Pen_Holder_With_Pens', dx=0.25, dz=-0.25)
    b.on(r, desk, 'Folders_Set_B', dx=0.6, dz=-0.2)
    b.on(r, desk, 'Book_Single_B', dx=0.3, dz=0.1, yaw=20.0)
    b.on(r, desk, 'Painting_Table_H', dx=-0.35, dz=-0.22, yaw=-15.0)
    b.on(r, desk, 'Coffee_Mug', dx=0.05, dz=-0.2, yaw=60.0)
    for z in (0.9, 2.15):
        bs = b.wall(r, 'W', z, 'Bookshelf', collide=True)
        b.shelf_row(r, bs, 0, [('Books_A', 0.0), ('Books_C', 0.0), ('Book_Pile_A', 90.0), ('Books_E', 0.0)])
        b.shelf_row(r, bs, 1, [('Books_B', 0.0), ('Books_D', 0.0), ('Books_F', 0.0), ('Books_A', 0.0)], dz=0.02)
        b.shelf_row(r, bs, 2, [('Books_E', 0.0), ('Books_C', 0.0), ('Painting_Table_I', 0.0), ('Books_B', 0.0)],
                    dz=0.02)
        b.shelf_row(r, bs, 3, [('Books_F', 0.0), ('Books_D', 0.0), ('Vase_A', 0.0)], dz=0.02)
    b.put(r, 'Carpet_B', 8.35, 2.0, 90.0)
    box = b.put(r, 'Cardboard_Box_D', 8.6, 3.55, 8.0, collide=True)
    b.on(r, box, 'Cardboard_Box_A', yaw=-12.0)
    b.put(r, 'Papers_Box_Open_A', 9.2, 3.7, 30.0)
    b.put(r, 'Cardboard_Box_A_Lid', 9.55, 3.25, 70.0)
    b.hang(r, 'S', 8.9, 'Painting_Large_C', 1.6)
    b.outlet(r, 'E', 3.2)


def spare(b):
    """The spare room: a made single bed nobody sleeps in, linen folded on it, a shelf and boxes of things she can't
    part with."""
    r = 'Kids Bedroom'
    bed = b.wall(r, 'E', 1.2, 'Bed_Single_Person', collide=True)
    b.put(r, 'Towel_Pile', bed['x'] - 0.3, bed['z'], 90.0, y=0.52)
    b.put(r, 'Towel_Pile', bed['x'] - 0.3, bed['z'] - 0.05, 80.0, y=0.68)
    b.supports.append(dict(bed, y1=bed['y0'] + 0.52))
    sh = b.wall(r, 'W', 1.5, 'Shelf_Standing_A', collide=True)
    b.shelf_row(r, sh, 0, [('Storage_Box_B', 90.0), ('Papers_Box_C', 0.0)])
    b.shelf_row(r, sh, 1, [('Toy_Box_B', 0.0), ('Toy_Dinosaur', 90.0), ('Book_Pile_A', 90.0)])
    b.shelf_row(r, sh, 2, [('Folders_Set_C', 0.0), ('Painting_Table_J', 0.0), ('Vase_A', 0.0)])
    b.shelf_row(r, sh, 3, [('Papers_Box_Open_B', 0.0), ('Candle_A', 0.0), ('Candle_B', 0.0)])
    b.wall(r, 'W', 2.6, 'Dresser_D_Assembled', collide=True)
    b.put(r, 'Storage_Box_A', 10.5, 0.62, 5.0, collide=True)
    b.wall(r, 'S', 12.4, 'Cardboard_Box_Covered', collide=True)
    b.put(r, 'Cardboard_Box_D', 11.65, 3.75, -6.0, collide=True)
    b.put(r, 'Laundrybasket', 12.55, 2.35, 15.0)
    b.hang(r, 'E', 3.1, 'Painting_Small_A', 1.5)


def ensuite(b):
    r = 'Ensuite'
    f = fittings.ensuite(b)
    vn = f['vanity']
    b.on(r, vn, 'Toothbrush_Mug', dx=-0.05, dz=-0.17)
    b.on(r, vn, 'Handsoap', dx=0.32, dz=-0.18)
    b.on(r, vn, 'Lotiontube', dx=0.46, dz=-0.17, yaw=30.0)
    b.on(r, vn, 'Vial_Pain_Killer_02', dx=-0.45, dz=-0.15)
    b.on(r, f['soap'], 'Shampoo_Bottle_B', dx=0.0)
    b.wall(r, 'E', 1.55, 'Towel_Hanging_Small', y=0.85, gap=0.04)
    b.put(r, 'Carpet_D', 5.3, 1.5, 90.0, scale=0.55)


def bathroom(b):
    r = 'Bathroom'
    vn = fittings.bathroom(b)['vanity']
    b.on(r, vn, 'Handsoap', dx=0.1, dz=-0.18)
    b.on(r, vn, 'Toothbrush_A', dx=-0.3, dz=-0.15, yaw=80.0)
    b.wall(r, 'W', 7.6, 'Towel_Hanging_Large', y=0.75, gap=0.04)
    b.put(r, 'Toiletpaper_Set', 10.62, 7.42, 90.0)
    b.put(r, 'Carpet_D', 11.35, 7.15, 0.0, scale=0.55)


def laundry(b):
    r = 'Laundry'
    x0, z0, x1, z1 = inner(r)
    f = fittings.laundry(b)
    b.on(r, f['washer'], 'Towel_Roll_Set', dx=-0.1)
    shelf = b.wall(r, 'E', z0 + 0.62, 'Garage_Shelf_A', collide=True)
    b.shelf_row(r, shelf, 1, [('Spray_Can_C', 0.0), ('Spray_Can_D', 0.0), ('Spray_Can_E', 0.0), ('Handsoap', 0.0)])
    b.shelf_row(r, shelf, 2, [('Towel_Pile', 90.0), ('Toiletpaper_Set', 90.0)])
    b.shelf_row(r, shelf, 0, [('Storage_Box_B', 90.0), ('Paintbucket_A', 0.0)])
    b.put(r, 'Laundrybasket', x0 + 0.9, z1 - 0.3, 10.0)


def hall(b):
    """A runner, the key shelf by the door with a dish and her late husband's photo, the family along the wall, the
    crucifix over the door. Its light stays on all night."""
    r = 'Hall'
    sh = b.wall(r, 'S', 12.3, 'Shelf', y=1.05, scale=0.8)
    b.on(r, sh, 'Decorative_Bowl', dx=-0.26)
    b.on(r, sh, 'Painting_Table_B', dx=0.22, dz=-0.02)
    b.hang(r, 'E', 4.8, 'Cross', 2.25)
    b.put(r, 'Carpet_B', 9.5, 4.8, 90.0, scale=0.85)
    for t, stem in ((7.4, 'Painting_Small_B'), (8.2, 'Painting_Medium_F'), (9.1, 'Painting_Small_D'),
                    (9.8, 'Painting_Small_A')):
        b.hang(r, 'S', t, stem, 1.55)


def furnish(b):
    living(b)
    kitchen(b)
    master(b)
    study(b)
    spare(b)
    ensuite(b)
    bathroom(b)
    laundry(b)
    hall(b)
    fittings.switches(b)
    fittings.ceilings(b, on={'Hall': {'name': 'Ceiling', 'intensity': 0.45, 'rng': 4.0, 'color': (1.0, 0.78, 0.55),
                                      'spot': 70.0, 'glow': 0.35}}, skip={'Kitchen'})
    b.windows(curtains={'Living Room': 'Curtains_B_Singlewindow', 'Master Bedroom': 'Curtains_B_Singlewindow',
                        'Bedroom 2': 'Curtains_D_Singlewindow', 'Kids Bedroom': 'Curtains_D_Singlewindow'},
              moonlit={'Master Bedroom'}, curtain_tint=(0.62, 0.3, 0.3))
