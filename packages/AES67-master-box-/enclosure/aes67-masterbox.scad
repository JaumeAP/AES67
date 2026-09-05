// AES67-MasterBox -- printed enclosure.
//
// Two parts, base and lid, screwed together with M3 into heat-set inserts.
// The layout is the one specified in HARDWARE.md, section "Inside the box:
// how it goes together": USB end of the Teensy to the front, Ethernet end to
// the rear, word clock BNC in a rear corner with the conditioning board
// immediately behind it.
//
//   openscad -o base.stl -D 'part="base"' aes67-masterbox.scad
//   openscad -o lid.stl  -D 'part="lid"'  aes67-masterbox.scad
//
// Everything below is a parameter and the ones marked CONFIRM have NOT been
// measured on real parts. They are the dimensions this model is least sure
// of, and printing before checking them is how a box comes out 2 mm short.

part = "both";          // "base", "lid", "both"
$fn = 64;

// ---------------------------------------------------------------- structure

wall        = 2.4;      // six perimeters at a 0.4 mm nozzle: a wall that is
floor_t     = 2.4;      // stiff rather than merely present, and thick enough
                        // to hold a panel connector's nut
lid_t       = 2.4;
lid_lip     = 1.6;      // the lip that locates the lid inside the walls
fit         = 0.25;     // printing clearance, per side

corner_r    = 3;

// ------------------------------------------------------------------- boards

teensy_len          = 61.0;   // Teensy 4.1 PCB, 2.4 x 0.7 inch
teensy_w            = 17.8;
teensy_hole_inset   = 2.5;    // CONFIRM: hole centres from the board edges
teensy_hole_d       = 2.4;    // pilot for an M2.5 self-tapping screw
teensy_standoff_h   = 6;      // room under the board for the pin tails

pcb_t               = 1.6;
tallest_above_pcb   = 16;     // CONFIRM: the MagJack, mounted offset. This is
                              // what sets the interior height, so measure the
                              // real stack before printing.
headroom            = 3;

cond_len            = 40;     // conditioning board, chosen not measured:
cond_w              = 25;     // make the board fit the box or the box fit the
cond_hole_inset     = 3;      // board, but decide before printing either
cond_hole_d         = 2.4;
cond_standoff_h     = 5;

// ------------------------------------------------------------------ margins

front_gap   = 10;   // between the Teensy's USB end and the front wall: the
                    // internal USB lead has to turn in here
rear_gap    = 12;   // between the Ethernet end and the rear wall: the patch
                    // lead turns in here
side_gap    = 5;
board_gap   = 8;    // between the Teensy and the conditioning board. This is
                    // the separation between the magnetics and the comparator
                    // and it is as much as the box allows: see HARDWARE.md.

// -------------------------------------------------------------- connectors
//
// CONFIRM every one of these against the part actually bought. They are the
// sizes the catalogues give for the usual versions, not measurements.

use_ethercon        = true;

ethercon_hole_d     = 24.0;   // CONFIRM: Neutrik D series panel cutout
ethercon_screw_sp   = 24.0;   // CONFIRM: screw spacing
ethercon_screw_d    = 3.2;

rj45_cut_w          = 16.0;   // plain feedthrough, if use_ethercon = false
rj45_cut_h          = 14.0;

bnc_hole_d          = 9.6;    // CONFIRM: 3/8 inch bulkhead
jack_hole_d         = 8.1;    // CONFIRM: 2.1 x 5.5 mm panel jack

led_hole_d          = 8.1;    // a 5 mm LED in a panel holder. A bare LED
                              // pushed into the plastic wants 5.2 instead.

usb_cut_w           = 16.5;   // CONFIRM: type B panel socket
usb_cut_h           = 14.0;
usb_screw_sp        = 22.5;   // CONFIRM
usb_screw_d         = 3.2;

// ------------------------------------------------------------------- screws

insert_d        = 4.2;    // M3 heat-set insert
insert_depth    = 6.0;
boss_d          = 8.0;
lid_screw_d     = 3.4;
lid_head_d      = 6.2;
lid_head_depth  = 2.2;
boss_merge      = 1.0;    // how far the corner bosses bite into the walls.
                          // Not cosmetic: touching them exactly tangentially
                          // gives a non-manifold edge and a slicer that
                          // argues about it.

// -------------------------------------------------------------- derived box

boards_w    = teensy_w + board_gap + cond_w;
panel_w     = (use_ethercon ? ethercon_hole_d : rj45_cut_w) + bnc_hole_d
              + jack_hole_d + 4 * 8;

inner_w     = max(boards_w + 2 * side_gap, panel_w);
inner_len   = teensy_len + front_gap + rear_gap;
inner_h     = teensy_standoff_h + pcb_t + tallest_above_pcb + headroom;

// x runs left to right, y from the front wall to the rear, z up from the
// inside of the floor.

teensy_x    = side_gap;                          // left edge of the Teensy
teensy_y    = front_gap;                         // its USB end
cond_x      = teensy_x + teensy_w + board_gap;   // left edge of the board
cond_y      = inner_len - rear_gap - cond_len + 4;

// Connector centres on the rear wall, left to right: Ethernet, word clock,
// power. The BNC sits over the conditioning board's corner on purpose.
eth_x       = 8 + (use_ethercon ? ethercon_hole_d : rj45_cut_w) / 2;
jack_x      = inner_w - 8 - jack_hole_d / 2;
bnc_x       = cond_x + cond_w / 2;

// and on the front wall
led_x       = 10 + led_hole_d / 2;
usb_x       = inner_w - 10 - usb_cut_w / 2;

conn_z      = inner_h / 2;      // every panel part on one centre line

// ------------------------------------------------------------------ modules

module rounded_slab(w, l, h, r) {
    hull() for (x = [r, w - r], y = [r, l - r])
        translate([x, y, 0]) cylinder(h = h, r = r);
}

module shell() {
    difference() {
        rounded_slab(inner_w + 2 * wall, inner_len + 2 * wall,
                     inner_h + floor_t, corner_r);
        translate([wall, wall, floor_t])
            rounded_slab(inner_w, inner_len, inner_h + 1, max(corner_r - wall, 0.1));
        // the rebate the lid's lip drops into
        translate([wall - lid_lip, wall - lid_lip, floor_t + inner_h - lid_t])
            rounded_slab(inner_w + 2 * lid_lip, inner_len + 2 * lid_lip,
                         lid_t + 1, corner_r);
    }
}

module boss_positions() {
    o = wall + boss_d / 2 - boss_merge;
    for (x = [o, inner_w + 2 * wall - o], y = [o, inner_len + 2 * wall - o])
        translate([x, y, 0]) children();
}

module bosses() {
    boss_positions() difference() {
        cylinder(h = floor_t + inner_h - lid_t, d = boss_d);
        translate([0, 0, floor_t + inner_h - lid_t - insert_depth])
            cylinder(h = insert_depth + 1, d = insert_d);
    }
}

module standoff(h, hole_d) {
    difference() {
        cylinder(h = h, d = hole_d + 3.4);
        translate([0, 0, 1]) cylinder(h = h, d = hole_d);
    }
}

module board_standoffs() {
    // Teensy: four corners, inset from the board edges
    for (x = [teensy_x + teensy_hole_inset, teensy_x + teensy_w - teensy_hole_inset],
         y = [teensy_y + teensy_hole_inset, teensy_y + teensy_len - teensy_hole_inset])
        translate([wall + x, wall + y, floor_t])
            standoff(teensy_standoff_h, teensy_hole_d);

    for (x = [cond_x + cond_hole_inset, cond_x + cond_w - cond_hole_inset],
         y = [cond_y + cond_hole_inset, cond_y + cond_len - cond_hole_inset])
        translate([wall + x, wall + y, floor_t])
            standoff(cond_standoff_h, cond_hole_d);
}

// Two slots in the floor to pass a cable tie through: the internal USB lead
// is anchored to the box and never to the Teensy's micro-B socket.
module tie_slots() {
    for (dx = [-6, 6])
        translate([wall + usb_x + dx, wall + 6, -1])
            cube([2.5, 8, floor_t + 2], center = false);
}

module rear_cutouts() {
    y = wall + inner_len + wall;
    z = floor_t + conn_z;

    // Ethernet
    translate([wall + eth_x, y, z]) rotate([90, 0, 0]) {
        if (use_ethercon) {
            cylinder(h = wall * 3, d = ethercon_hole_d, center = true);
            for (dx = [-ethercon_screw_sp / 2, ethercon_screw_sp / 2])
                translate([dx, 0, 0])
                    cylinder(h = wall * 3, d = ethercon_screw_d, center = true);
        } else {
            cube([rj45_cut_w, rj45_cut_h, wall * 3], center = true);
        }
    }

    translate([wall + bnc_x, y, z]) rotate([90, 0, 0])
        cylinder(h = wall * 3, d = bnc_hole_d, center = true);

    translate([wall + jack_x, y, z]) rotate([90, 0, 0])
        cylinder(h = wall * 3, d = jack_hole_d, center = true);
}

module front_cutouts() {
    z = floor_t + conn_z;

    translate([wall + led_x, wall, z]) rotate([90, 0, 0])
        cylinder(h = wall * 3, d = led_hole_d, center = true);

    translate([wall + usb_x, wall, z]) rotate([90, 0, 0]) {
        cube([usb_cut_w, usb_cut_h, wall * 3], center = true);
        for (dx = [-usb_screw_sp / 2, usb_screw_sp / 2])
            translate([dx, 0, 0])
                cylinder(h = wall * 3, d = usb_screw_d, center = true);
    }
}

module base() {
    difference() {
        union() {
            shell();
            bosses();
            board_standoffs();
        }
        rear_cutouts();
        front_cutouts();
        tie_slots();
    }
}

module lid() {
    difference() {
        union() {
            rounded_slab(inner_w + 2 * wall, inner_len + 2 * wall, lid_t, corner_r);
            // the lip, a touch smaller than its rebate so it drops in
            translate([wall - lid_lip + fit, wall - lid_lip + fit, -lid_t])
                rounded_slab(inner_w + 2 * lid_lip - 2 * fit,
                             inner_len + 2 * lid_lip - 2 * fit,
                             lid_t, corner_r);
        }
        boss_positions() {
            translate([0, 0, -lid_t - 1]) cylinder(h = lid_t * 3, d = lid_screw_d);
            translate([0, 0, lid_t - lid_head_depth])
                cylinder(h = lid_head_depth + 1, d = lid_head_d);
        }
    }
}

if (part == "base" || part == "both") base();
if (part == "lid"  || part == "both")
    translate([0, inner_len + 2 * wall + 10, lid_t]) lid();
