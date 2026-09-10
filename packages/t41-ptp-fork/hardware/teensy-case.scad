//
// teensy-case.scad
// A printable case for a Teensy 4.1 with the PJRC Ethernet kit, a BNC
// reference input and the board's own USB reachable from outside.
//
// Three connectors and nothing else, which is the whole design: network
// in, reference in, USB for power and programming.
//
//   openscad -o teensy-case-body.stl -D part=\"body\" teensy-case.scad
//   openscad -o teensy-case-lid.stl  -D part=\"lid\"  teensy-case.scad
//
// EVERY DIMENSION THAT TOUCHES A BOUGHT PART IS A VARIABLE, and the ones
// worth measuring before printing are marked. A case that is 0.4 mm out
// on a panel cut-out is a case that gets printed twice.
//
// The Teensy 4.1 has NO mounting holes. It is held here the way it is
// held on every sled: by its long edges, in a pair of slots, with a lip
// at each end. Nothing screws to the board.
//

part = "body";          // "body", "lid", or "both" for a look at it

// ---------------------------------------------------------------- board

board_length      = 61.0;   // Teensy 4.1, PJRC's figure
board_width       = 17.8;
board_thickness   = 1.6;
// Under the board: the pin headers if they are fitted, and the ribbon
// cable that reaches the Ethernet kit. MEASURE if the board is socketed.
under_board       = 6.0;
// Over the board: components, and the SD card that has to come out.
over_board        = 7.0;

// The slot the board's edge sits in. 0.4 is a print-and-fit number on a
// 0.4 mm nozzle; tighten it if the board rattles, loosen it if it fights.
edge_slot_depth   = 1.6;
edge_slot_clear   = 0.4;

// --------------------------------------------------------- the case box

wall              = 2.4;
floor_thickness   = 2.0;
// Room at each end for the USB connector at one and the ribbon cable's
// bend radius at the other.
end_clearance     = 12.0;
// Across: the board, plus room down one side for the level shifter and
// the wiring from the BNC.
side_clearance    = 26.0;

inner_length      = board_length + 2 * end_clearance;
inner_width       = board_width + side_clearance;
inner_height      = under_board + board_thickness + over_board;

// ------------------------------------------------------- the connectors

// RJ45, as a snap-in keystone. The MagJack stays inside on its own board
// and a short patch lead reaches this: what a yanked cable pulls on is
// then the keystone, not fifteen hand-soldered joints.
// MEASURE the keystone: 14.8 x 20.6 is the common one, not the only one.
keystone_width    = 14.8;
keystone_height   = 20.6;

// BNC bulkhead. 12.7 mm is the usual 1/2 inch thread; some are 12.0.
// MEASURE the one that arrives.
bnc_hole          = 12.7;
// The flat that stops the socket turning when the nut is done up. Set to
// 0 for a plain round hole.
bnc_flat          = 11.6;

// The Teensy's own micro-B socket, reached through the end wall. It sits
// centred on the board's width, hard against the end of the PCB.
usb_width         = 9.0;
usb_height        = 5.5;
// How far the socket's face sits back from the end of the PCB, plus the
// room a plug's moulding needs. Generous on purpose: a tight USB cut-out
// is the one that stops a cable seating.
usb_relief        = 2.0;

// ------------------------------------------------------------ the lid

lid_thickness     = 2.0;
// The lip that locates the lid inside the walls.
lid_lip           = 1.2;
lid_clear         = 0.3;

// Vents. The crystal's drift is what the servo spends its time
// correcting, and a sealed box in a warm rack gives it more to correct.
vent_slot         = 2.4;
vent_gap          = 4.0;

// ------------------------------------------------------------- helpers

module board_slots() {
    // Two slots, one down each long side, holding the PCB by its edges.
    slot_z = floor_thickness + under_board;
    for (side = [0, 1]) {
        translate([end_clearance - 2,
                   side == 0 ? side_wall_offset() - edge_slot_depth
                             : side_wall_offset() + board_width,
                   slot_z])
            cube([board_length + 4, edge_slot_depth, board_thickness + edge_slot_clear]);
    }
}

function side_wall_offset() = (inner_width - board_width) / 2;

module shell() {
    difference() {
        cube([inner_length + 2 * wall, inner_width + 2 * wall,
              inner_height + floor_thickness]);
        translate([wall, wall, floor_thickness])
            cube([inner_length, inner_width, inner_height + 1]);
    }
}

module board_rails() {
    // The rails the slots are cut into: two ribs standing off the floor.
    rail_height = floor_thickness + under_board + board_thickness + edge_slot_clear + 1.5;
    for (side = [0, 1]) {
        translate([wall + end_clearance - 2,
                   wall + (side == 0 ? side_wall_offset() - edge_slot_depth - 1.5
                                     : side_wall_offset() + board_width + edge_slot_depth),
                   0])
            cube([board_length + 4, 1.5 + edge_slot_depth, rail_height]);
    }
}

module board_slot_cuts() {
    slot_z = floor_thickness + under_board;
    for (side = [0, 1]) {
        translate([wall + end_clearance - 3,
                   wall + (side == 0 ? side_wall_offset() - edge_slot_depth
                                     : side_wall_offset() + board_width),
                   slot_z])
            cube([board_length + 6, edge_slot_depth, board_thickness + edge_slot_clear]);
    }
}

module usb_cutout() {
    // Through the end wall, centred on the board.
    z = floor_thickness + under_board + board_thickness / 2 - usb_height / 2;
    translate([-1, wall + side_wall_offset() + board_width / 2 - usb_width / 2, z])
        cube([wall + usb_relief + 2, usb_width, usb_height]);
}

module keystone_cutout() {
    // Through the far end wall.
    z = floor_thickness + (inner_height - keystone_height) / 2;
    translate([inner_length + 2 * wall - wall - 1,
               wall + inner_width / 2 - keystone_width / 2, z])
        cube([wall + 2, keystone_width, keystone_height]);
}

module bnc_cutout() {
    // Through the same end as the network, so every cable leaves one
    // face and the front of the rack stays clean.
    y = wall + inner_width / 2 - keystone_width / 2 - 14;
    z = floor_thickness + inner_height / 2;
    translate([inner_length + 2 * wall - wall - 1, y, z])
        rotate([0, 90, 0])
            difference() {
                cylinder(h = wall + 2, d = bnc_hole, $fn = 64);
                if (bnc_flat > 0) {
                    translate([bnc_flat / 2, -bnc_hole, -1])
                        cube([bnc_hole, 2 * bnc_hole, wall + 4]);
                }
            }
}

module vents() {
    count = floor((inner_length - 20) / vent_gap);
    for (i = [0 : count - 1]) {
        translate([wall + 10 + i * vent_gap, wall + 6,
                   inner_height + floor_thickness - 1])
            cube([vent_slot, inner_width - 12, lid_thickness + 4]);
    }
}

// --------------------------------------------------------------- parts

module body() {
    difference() {
        union() {
            shell();
            board_rails();
        }
        board_slot_cuts();
        usb_cutout();
        keystone_cutout();
        bnc_cutout();
    }
}

module lid() {
    union() {
        difference() {
            cube([inner_length + 2 * wall, inner_width + 2 * wall, lid_thickness]);
            vents();
        }
        // The lip that drops inside the walls and locates it.
        translate([wall + lid_clear, wall + lid_clear, -lid_lip])
            cube([inner_length - 2 * lid_clear, inner_width - 2 * lid_clear, lid_lip]);
    }
}

if (part == "body") body();
else if (part == "lid") translate([0, 0, inner_height + floor_thickness + 5]) lid();
else {
    body();
    translate([0, 0, inner_height + floor_thickness + 5]) lid();
}
