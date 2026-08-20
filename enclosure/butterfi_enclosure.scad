// =============================================================
//  ButterFi USB Dongle Enclosure
//  Two-part clamshell. The cable is captured at the parting seam,
//  the halves pull together with self-tapping screws, vertical
//  corners are rounded, and a tether lug in one corner takes the
//  locking wire.
//
//  Model units are MILLIMETERS. Enter sizes in inches via `in`.
// =============================================================

/* [Preview] */
part    = "both";   // ["both","bottom","top","assembled","top_print"]
explode = 6;        // for "assembled": lift the lid this many mm (0 = closed)

/* [Core dimensions] */
in          = 25.4;
board_len   = 2.25 * in;   // XIAO far end  ->  just past the cable strain relief
board_width = 0.75 * in;   // measured PCB width
cavity_h    = 0.375 * in;  // 3/8" internal height (slop for taller cables)
wall        = 2.0;         // wall / floor / ceiling thickness (mm)
corner_r    = 3.0;         // rounded vertical-corner radius (mm)

/* [Cable pass-through] */
cable_gap   = 0.125 * in;  // 1/8" opening, centered on the +X end, at the seam

/* [Self-tapping screws] */
screw_pilot = 1.7;   // pilot bore in the BOTTOM boss (M2 self-tapper ~1.6-1.7)
screw_clear = 2.4;   // clearance bore through the TOP boss
screw_head  = 4.0;   // head counterbore diameter
head_depth  = 1.8;   // head counterbore depth
boss_od     = 4.6;   // boss outer diameter
end_inset   = 5.0;   // screw-center distance in from the far end wall (X)
side_inset  = 3.6;   // screw-center distance in from the side walls (Y)

/* [Tether lug] */
tether        = true;
tether_dia    = 4.0;      // locking-wire hole diameter
tether_tab    = 8.0;      // lug outer diameter
tether_corner = [-1, 1];  // which corner: [x sign, y sign]  (-X, +Y here)

/* [Alignment lip] */
lip         = true;   // tongue-and-groove around the seam (anti-shear / anti-pry)
lip_h       = 1.5;    // how far the tongue rises above the seam (mm)
lip_t       = 0.8;    // tongue thickness, centered in the wall (mm)
lip_clear   = 0.2;    // clearance per side between tongue and groove (mm)

/* [Quality] */
$fn = 64;

// ---------------- derived ----------------
outer_len = board_len + 2*wall;
outer_wid = board_width + 2*wall;
outer_h   = cavity_h + 2*wall;
seam_z    = outer_h/2;                    // == wall + cavity_h/2 (cavity mid-height)
inner_r   = max(corner_r - wall, 0.8);

// Two screws at the FAR end only (away from the cable). The cable end stays
// closed by the captured strain relief, so it's left clear for the plug. Two
// screws (rather than one) keep the lid from twisting and require a tool to open.
screw_xy = [
  [ -(board_len/2 - end_inset),   board_width/2 - side_inset ],  // far end, +Y
  [ -(board_len/2 - end_inset), -(board_width/2 - side_inset)],  // far end, -Y
];

// ---------------- primitives ----------------

// centered 2D rounded rectangle
module rr2d(len, wid, r) {
  hull() for (sx = [-1,1], sy = [-1,1])
    translate([sx*(len/2 - r), sy*(wid/2 - r)]) circle(r = r);
}

// centered rounded rectangle, extruded to height h (vertical edges rounded)
module rrect(len, wid, r, h) {
  linear_extrude(height = h) rr2d(len, wid, r);
}

// rounded-rect ring following the main shell outline:
// outer edge inset `o_in` from the shell outer, radial thickness `t`
module lip_ring(o_in, t, z0, h) {
  translate([0, 0, z0])
    linear_extrude(height = h)
      difference() {
        offset(r = -o_in)       rr2d(outer_len, outer_wid, corner_r);
        offset(r = -(o_in + t)) rr2d(outer_len, outer_wid, corner_r);
      }
}

// tongue rises from the bottom-half wall, centered in the wall thickness
module tongue() {
  if (lip) lip_ring(wall/2 - lip_t/2, lip_t, seam_z, lip_h);
}

// groove: matching recess in the lid underside, with clearance all round
module groove() {
  if (lip) lip_ring(wall/2 - lip_t/2 - lip_clear, lip_t + 2*lip_clear,
                    seam_z - 0.01, lip_h + 0.31);
}

// tether lug solid: a disc hung off the corner, blended into it
module lug_solid(h) {
  cx = tether_corner[0]*(outer_len/2 - corner_r);
  cy = tether_corner[1]*(outer_wid/2 - corner_r);
  ox = tether_corner[0]*(outer_len/2 + tether_tab/2 - corner_r);
  oy = tether_corner[1]*(outer_wid/2 + tether_tab/2 - corner_r);
  linear_extrude(height = h)
    hull() {
      translate([cx, cy]) circle(r = corner_r);
      translate([ox, oy]) circle(r = tether_tab/2);
    }
}

module lug_hole() {
  ox = tether_corner[0]*(outer_len/2 + tether_tab/2 - corner_r);
  oy = tether_corner[1]*(outer_wid/2 + tether_tab/2 - corner_r);
  translate([ox, oy, -1]) cylinder(d = tether_dia, h = outer_h + 2);
}

// solid outer body (shell + lug) before hollowing
module outer_solid() {
  rrect(outer_len, outer_wid, corner_r, outer_h);
  if (tether) lug_solid(outer_h);
}

// interior cavity
module cavity() {
  translate([0, 0, wall])
    rrect(board_len, board_width, inner_r, cavity_h);
}

// cable pass-through: horizontal bore through the +X wall, centered on the seam
module cable_hole() {
  translate([board_len/2 - 3, 0, seam_z])
    rotate([0, 90, 0])
      cylinder(d = cable_gap, h = wall + 4);
}

// hollow shell, cable slot and tether hole cut, no bosses yet
module shell_hollow() {
  difference() {
    outer_solid();
    cavity();
    cable_hole();
    if (tether) lug_hole();
  }
}

// bottom bosses: floor -> seam, pre-drilled with a self-tap pilot
module bottom_bosses() {
  for (p = screw_xy)
    translate([p[0], p[1], wall])
      difference() {
        cylinder(d = boss_od, h = seam_z - wall);
        translate([0,0,-1]) cylinder(d = screw_pilot, h = seam_z - wall + 2);
      }
}

// top bosses: seam -> ceiling underside (screw cuts drilled separately)
module top_bosses() {
  for (p = screw_xy)
    translate([p[0], p[1], seam_z])
      cylinder(d = boss_od, h = (outer_h - wall) - seam_z);
}

// clearance bore + head counterbore through the top half
module top_screw_cuts() {
  for (p = screw_xy) {
    translate([p[0], p[1], seam_z - 1])
      cylinder(d = screw_clear, h = outer_h);
    translate([p[0], p[1], outer_h - head_depth])
      cylinder(d = screw_head, h = head_depth + 1);
  }
}

// ---------------- halves ----------------

module bottom_half() {
  difference() {
    union() {
      intersection() {
        shell_hollow();
        translate([-outer_len, -outer_wid, 0])
          cube([2*outer_len, 2*outer_wid, seam_z]);
      }
      bottom_bosses();
      tongue();
    }
    cable_hole();   // notch the tongue where the cable passes out
  }
}

module top_half() {
  difference() {
    union() {
      intersection() {
        shell_hollow();
        translate([-outer_len, -outer_wid, seam_z])
          cube([2*outer_len, 2*outer_wid, outer_h - seam_z + 1]);
      }
      top_bosses();
    }
    groove();
    top_screw_cuts();
    cable_hole();   // keep the cable opening clear of the groove ring
  }
}

// ---------------- layout ----------------

if (part == "bottom") {
  bottom_half();
} else if (part == "top") {
  top_half();
} else if (part == "top_print") {          // lid flipped flat on the bed, open side up
  translate([0, 0, outer_h]) rotate([180, 0, 0]) top_half();
} else if (part == "assembled") {
  bottom_half();
  translate([0, 0, explode]) top_half();
} else {                 // "both" - laid out flat on the bed for printing
  bottom_half();
  translate([0, outer_wid + 12, outer_h])
    rotate([180, 0, 0]) top_half();   // lid flipped, open side up, no supports
}
