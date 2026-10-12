# 313: the recluse who left in a hurry. A man nobody on the floor really knew: windows boarded from the inside,
# rooms of boxes, medicine in the bathroom. Something made him go tonight - drawers yanked out and dropped, papers
# everywhere, furniture knocked over, the front door left ajar. One kitchen tube still stutters; the bathroom light
# was left on.
from apartment_kit import H, MODES, TUBE, inner
from households import fittings

PAINT = {'Living Room': 'wall_yellowed', 'Hall': 'wall_yellowed', 'Kitchen': 'wall_yellowed',
         'Master Bedroom': 'wall_grey', 'Bedroom 2': 'wall_yellowed', 'Kids Bedroom': 'wall_grey',
         'Ensuite': 'tile_wall', 'Bathroom': 'tile_wall', 'Laundry': 'wall_grey'}
FLOORS = {'Living Room': 'hardwood', 'Hall': 'hardwood', 'Kitchen': 'tile', 'Master Bedroom': 'laminate',
          'Bedroom 2': 'hardwood', 'Kids Bedroom': 'laminate', 'Ensuite': 'tile', 'Bathroom': 'tile', 'Laundry': 'tile'}
DOORS_OPEN = {'Front Door': 28.0, 'Master Door': 85.0, 'Ensuite Door': 70.0, 'Bedroom 2 Door': 70.0,
              'Kids Bedroom Door': 40.0, 'Bathroom Door': 15.0, 'Laundry Door': 30.0}


def scatter(b, room, items):
    """Things dropped or knocked over: (model, x, z, yaw[, pitch, roll[, scale]])."""
    for stem, x, z, yaw, *tip in items:
        kw = {'pitch': tip[0], 'roll': tip[1]} if tip else {}
        if len(tip) > 2:
            kw['scale'] = tip[2]
        b.put(room, stem, x, z, yaw, **kw)


def living(b):
    """The sofa shoved aside, the armchair and the floor lamp knocked over, the coffee table on its side, the TV
    face down; boxes half packed and their papers all over the floor; a painting taken down."""
    r = 'Living Room'
    tvs = b.wall(r, 'W', 7.1, 'Tv_Stand_Assembled', collide=True)
    b.put(r, 'Carpet_D', 2.4, 7.3, 72.0)
    b.put(r, 'Sofa_04', 3.7, 7.25, -72.0, collide=True)
    b.put(r, 'Chair_Arm', 2.1, 8.75, 35.0, pitch=-90.0, collide=True)
    b.put(r, 'Lamp_Floor_A_Assembled', 1.2, 5.95, 20.0, roll=90.0, **b.lamp_off('Lamp_Floor_A_Assembled'))
    b.put(r, 'Sidetable_C', 2.3, 6.35, 30.0, roll=90.0, collide=True)
    b.put(r, 'Tv', 1.2, 6.9, 175.0, pitch=90.0, collide=True)
    scatter(b, r, [('Papers_Folder_A', 2.9, 6.0, 20.0), ('Papers_Folder_C', 3.2, 5.7, -40.0),
                   ('Papers_Pile_C', 2.6, 5.5, 75.0), ('Papers_Folder_B', 1.9, 7.3, 110.0),
                   ('Papers_Pile_A', 4.6, 6.0, -15.0), ('Papers_Folder_F', 4.9, 8.9, 50.0),
                   ('Money_Usa', 4.4, 5.35, 30.0, 0.0, 0.0, 0.5), ('Book_Single_A', 1.6, 6.8, 140.0),
                   ('Cardboard_Box_A_Lid', 5.1, 6.3, 25.0), ('Can', 3.1, 9.2, 0.0, 90.0, 0.0)])
    box = b.put(r, 'Cardboard_Box_D', 5.5, 9.55, 10.0, collide=True)
    b.on(r, box, 'Cardboard_Box_A', yaw=-20.0)
    b.put(r, 'Papers_Box_Open_A', 5.55, 5.75, 40.0)
    b.put(r, 'Papers_Box_Open_B', 4.95, 5.85, -20.0)
    b.wall(r, 'S', 2.0, 'Painting_Large_A', gap=0.13, pitch=-12.0)
    b.wall(r, 'N', 1.3, 'Storage_Box_A', collide=True)
    b.wall(r, 'N', 1.8, 'Storage_Box_B', collide=True)
    b.put(r, 'Garbage_Bag_B', 4.95, 9.62, 0.0)
    b.put(r, 'Chair_Dining', 5.0, 9.0, 0.0, roll=90.0, collide=True)  # dragged out of the kitchen and over
    b.shelf_row(r, tvs, 0, [('Papers_Box_C', 0.0), ('', 0.0), ('Book_Pile_A', 90.0)])


def kitchen(b):
    """Drawers pulled out and dropped, a cupboard door off its hinges, cans rolled across the tiles, the table
    shoved and a chair over. The tube stutters."""
    r = 'Kitchen'
    f = fittings.kitchen(b)
    c1, c2, c3 = f['c1'], f['c2'], f['c3']
    b.on(r, c1, 'Plate_Pile', dx=-0.2)
    b.on(r, c1, 'Knife', dx=0.2, dz=0.15, yaw=40.0)
    b.on(r, c2, 'Bottle_A', dx=-0.3, dz=-0.15)
    b.on(r, c3, 'Bowl_Pile', dx=0.1)
    scatter(b, r, [('Kitchen_Counter_Drawer', 7.4, 6.7, 15.0), ('Kitchen_Counter_Drawer', 9.2, 7.1, -60.0),
                   ('Kitchen_Cabinet_A_Door_A_L', 6.9, 7.6, 70.0, 0.0, 90.0), ('Can', 8.6, 6.5, 0.0, 90.0, 0.0),
                   ('Can', 7.0, 8.4, 40.0, 90.0, 0.0), ('Can', 9.5, 8.6, 110.0, 90.0, 0.0),
                   ('Macaroni_Package', 7.8, 6.3, 25.0, 0.0, 90.0), ('Cooking_Spoon', 9.0, 6.4, 75.0),
                   ('Papers_Pile_D', 8.2, 9.2, 15.0, 0.0, 90.0)])
    tx, tz = 8.1, 8.25
    tb = b.put(r, 'Table_Dining', tx, tz, 17.0, collide=True)
    b.on(r, tb, 'Papers_Pile_B', 0.2, -0.2, yaw=30.0)
    b.on(r, tb, 'Coffee_Mug', -0.3, 0.1, yaw=200.0)
    b.on(r, tb, 'Vial_Pain_Killer_02', 0.0, 0.35)
    b.put(r, 'Garbage_Bag_A', 8.15, 9.6, 50.0)
    tube = b.wbox('Lamp_Fluorescent_A', 0.0)
    b.put(r, 'Lamp_Fluorescent_A', 8.2, 7.4, 0.0, H - (tube[3] - tube[2]), **b.lamp_glow('Lamp_Fluorescent_A', 1.2))
    b.light(r, 'Tube', (8.2, H - 0.12, 7.4), 1.1, 5.0, TUBE, spot=80.0, fill=0.12,
            flicker={'Mode': MODES['Fluorescent'], 'MinInterval': 0.6, 'MaxInterval': 4.0, 'Depth': 1.0, 'Seed': 31})


def master(b):
    """The dresser emptied - both drawers out and on the floor - clothes over the bed and the floor, the
    nightstand's drawer upturned, the long mirror face down. The window is boarded."""
    r = 'Master Bedroom'
    bed = b.wall(r, 'W', 2.0, 'Bed', collide=True)
    b.put(r, 'Towel_Pile', bed['x'] + 0.2, bed['z'] - 0.3, 40.0, y=0.62)
    b.put(r, 'Money_Usa', bed['x'] - 0.3, bed['z'] + 0.4, 100.0, y=0.62, scale=0.5)
    b.supports.append(dict(bed, y1=bed['y0'] + 0.62))
    ns = b.wall(r, 'W', bed['z1'] + 0.36, 'Nightstand_A', collide=True)
    b.on(r, ns, 'Lamp_Table_B_Assembled', dz=-0.05, **b.lamp_off('Lamp_Table_B_Assembled'))
    b.wall(r, 'E', 1.4, 'Dresser_Large_A', collide=True)
    scatter(b, r, [('Dresser_Large_A_Drawer_A', 3.25, 0.95, 70.0),
                   ('Dresser_Large_A_Drawer_B', 3.1, 1.95, 110.0, 0.0, 180.0),
                   ('Nightstand_A_Drawer_A', 1.2, 3.75, 20.0, 0.0, 180.0), ('Towel_Pile', 2.9, 1.4, 15.0),
                   ('Towel_Pile', 2.65, 3.0, -40.0), ('Papers_Folder_D', 3.6, 2.6, 30.0),
                   ('Mirror_Body', 1.75, 3.6, 90.0, 90.0, 0.0)])
    b.put(r, 'Storage_Box_C', 3.9, 0.35, 0.0, collide=True)
    b.put(r, 'Cardboard_Box_C', 0.45, 0.4, 20.0, collide=True)


def office(b):
    """His desk stripped - the drawers on the floor, the chair over - and paper everywhere: files, piles, folders,
    the boxes he was filling. Half the books pulled off the shelf."""
    r = 'Bedroom 2'
    b.wall(r, 'E', 2.0, 'Desk_Office', collide=True)
    bs = b.wall(r, 'W', 1.5, 'Bookshelf', collide=True)
    b.shelf_row(r, bs, 0, [('Books_A', 0.0), ('', 0.0), ('Books_C', 0.0)])
    b.shelf_row(r, bs, 1, [('', 0.0), ('Books_B', 0.0), ('', 0.0), ('Books_E', 0.0)], dz=0.02)
    b.shelf_row(r, bs, 2, [('Books_F', 0.0)], dz=0.02)
    b.shelf_row(r, bs, 3, [('', 0.0), ('', 0.0), ('Books_D', 0.0)], dz=0.02)
    scatter(b, r, [('Desk_Office_Drawer', 8.8, 1.3, 75.0), ('Desk_Office_Drawer', 8.95, 2.75, 120.0, 0.0, 180.0),
                   ('Chair_Office', 8.4, 2.1, -30.0, 0.0, 90.0),
                   ('Papers_Folder_A', 7.8, 1.0, 30.0), ('Papers_Folder_B', 8.2, 1.6, -60.0),
                   ('Papers_Folder_C', 7.6, 2.3, 100.0), ('Papers_Folder_E', 8.3, 3.1, 20.0, 0.0, 90.0),
                   ('Papers_Pile_A', 7.9, 2.9, 45.0), ('Papers_Pile_C', 9.3, 3.5, -20.0),
                   ('Papers_Pile_D', 7.4, 1.7, 80.0, 0.0, 90.0), ('Folders_Pile_A', 9.4, 0.5, 10.0),
                   ('Book_Single_A', 7.45, 0.75, 60.0), ('Book_Single_B', 7.6, 1.25, -20.0),
                   ('Books_A', 7.5, 2.75, 30.0, 0.0, 90.0)])
    b.put(r, 'Papers_Box_Open_A', 9.5, 3.7, 15.0)
    b.put(r, 'Papers_Box_Lid_A', 9.05, 3.85, 60.0)
    b.put(r, 'Storage_Box_B', 7.25, 3.0, 5.0, collide=True)


def storage(b):
    """The third bedroom packed with boxes, floor to shoulder."""
    r = 'Kids Bedroom'
    x0, z0, x1, z1 = inner(r)
    stacks = [('W', 0.9), ('W', 1.5), ('W', 2.1), ('E', 0.75), ('E', 1.35), ('E', 1.95), ('E', 2.55)]
    for i, (side, t) in enumerate(stacks):
        base = b.wall(r, side, t, 'Cardboard_Box_D', collide=True, extra_yaw=(i % 3 - 1) * 4.0)
        mid = b.on(r, base, 'Cardboard_Box_E' if i % 2 else 'Cardboard_Box_D', yaw=(i % 4 - 1.5) * 5.0, collide=True)
        if i % 3 != 2:
            b.on(r, mid, 'Cardboard_Box_A', yaw=-6.0 + i * 3.0, collide=True)
    b.put(r, 'Cardboard_Box_Covered', 11.5, 2.6, 8.0, collide=True)
    b.put(r, 'Milk_Crate', 12.45, 3.6, 15.0, collide=True)
    b.put(r, 'Picking_Bin', 11.9, 3.75, 30.0)


def ensuite(b):
    """Syringes and vials on the vanity and the floor, a towel dropped; the light left on."""
    r = 'Ensuite'
    f = fittings.ensuite(b)
    vn = f['vanity']
    for stem, dx, dz, yaw in (('Syringe_Variation_01', -0.4, 0.1, 50.0), ('Vial_Insulin_01', -0.25, -0.15, 0.0),
                              ('Vial_Anti_Venom_03', -0.15, -0.12, 0.0), ('Vial_Pain_Killer_02', 0.25, -0.15, 0.0),
                              ('Toothbrush_Mug', 0.45, -0.17, 0.0)):
        b.on(r, vn, stem, dx=dx, dz=dz, yaw=yaw)
    scatter(b, r, [('Syringe_Variation_02', 5.25, 2.3, 35.0, 0.0, 90.0), ('Towel_Pile', 5.4, 1.35, -30.0),
                   ('Vial_Pain_Killer_02', 5.05, 2.75, 0.0, 90.0, 0.0)])
    b.ceiling_fixture(r, 'Lamp_Ceiling_A', 5.5, 2.2, on=True, glow=0.4, name='Ceiling', intensity=0.8, rng=3.5,
                      color=(0.95, 0.9, 0.8), spot=60.0, fill=0)


def bathroom(b):
    r = 'Bathroom'
    vn = fittings.bathroom(b)['vanity']
    b.on(r, vn, 'Handsoap', dx=0.1, dz=-0.18)
    b.put(r, 'Towel_Pile', 11.4, 7.4, 60.0)
    b.put(r, 'Toiletpaper_Set', 10.62, 7.42, 90.0)


def laundry(b):
    """The breaker panel's cover off and on the floor, a fuse beside it; a jerrycan and the shovel."""
    r = 'Laundry'
    x0, z0, x1, z1 = inner(r)
    fittings.laundry(b)
    scatter(b, r, [('Electricbox_Door', 11.3, 8.7, 10.0, -90.0, 0.0), ('Electricbox_Fuse', 11.6, 8.9, 40.0),
                   ('Jerrycan', x1 - 0.25, z0 + 0.35, 10.0)])
    b.put(r, 'Shovel', x1 - 0.12, z0 + 0.75, 0.0, pitch=-10.0)
    b.put(r, 'Garbage_Bag_A', x0 + 1.1, z1 - 0.25, 0.0)


def hall(b):
    """By the front door, left ajar: the boxes he did pack, a bag, a plank."""
    r = 'Hall'
    box = b.wall(r, 'S', 9.95, 'Cardboard_Box_D', collide=True, extra_yaw=90.0)
    b.on(r, box, 'Cardboard_Box_D', yaw=-3.0, collide=True)
    b.put(r, 'Garbage_Bag_B', 9.35, 5.14, 0.0)
    b.wall(r, 'N', 8.6, 'Woodplank', gap=0.05, extra_yaw=90.0, pitch=-75.0)
    scatter(b, r, [('Papers_Folder_C', 11.6, 4.9, 30.0), ('Papers_Pile_C', 8.2, 4.7, -20.0)])


def furnish(b):
    for f in (living, kitchen, master, office, storage, ensuite, bathroom, laundry, hall):
        f(b)
    fittings.switches(b)
    fittings.ceilings(b, skip={'Kitchen', 'Ensuite'})
    b.windows(boarded={'Living Room', 'Master Bedroom'}, glow=0.55)
