// StoveIQ PVC Pipe Enclosure — Parametric Split-Tube Design
// Open-source hardware: CERN-OHL-P v2
// Nick DeMarco — 2026-04-10
//
// CONCEPT:
//   1.5" Schedule 40 PVC pipe, cut in half lengthwise with a hacksaw.
//   Lower half (sensor half):  MLX90640 breakout + 14mm camera window
//   Upper half (ESP32 half):   ESP32-S3-DevKitC-1 + USB-C exit notch
//   Snap halves together with zip ties through OD grooves.
//   Whole tube slips into a 3D-printed bracket that mounts under the cabinet.
//   Rotate bracket to aim — tighten one M3 screw to lock angle.
//
// BUILD NOTES:
//   - "Sensor half" and "ESP32 half" are 3D-printed SHELLS, not cut real PVC.
//     OR: Use real PVC pipe + print only the alignment/bracket pieces.
//   - For pure 3D-print approach: print both halves in ASA/PETG at 35% infill.
//   - For hybrid approach: buy 6" of 1.5" PVC, cut in half,
//     glue/press-fit printed standoff inserts into each half.
//
// RENDER MODES (set at bottom of file):
//   SHOW_SENSOR_HALF  = true/false
//   SHOW_ESP32_HALF   = true/false
//   SHOW_END_CAP      = true/false
//   SHOW_BRACKET      = true/false
//   SHOW_ASSEMBLY     = true   (positions all parts together)
//   SHOW_COMPONENTS   = true   (ghost PCB overlay for fit check)
//
// PRINT SETTINGS:
//   Material:   PETG (indoor) or ASA (outdoor)
//   Layer:      0.2mm
//   Infill:     35% gyroid
//   Supports:   NO (all surfaces designed support-free when printed flat-face-down)
//   Perimeters: 3 (for strength around screw bosses)

/* [Pipe Dimensions — 1.5" Sch 40 PVC] */
PIPE_OD = 48.26;      // Outer diameter (mm) — 1.900 inches
PIPE_ID = 40.89;      // Inner diameter (mm) — 1.610 inches
PIPE_L  = 120.0;      // Total tube length (mm)
// Wall thickness = (OD - ID) / 2 = 3.685mm

/* [MLX90640 Breakout (Adafruit 4469)] */
SENSOR_W    = 25.4;   // Board width (mm)
SENSOR_L    = 25.4;   // Board length (mm)
SENSOR_T    = 1.6;    // PCB thickness (mm)
WIN_D       = 14.0;   // Camera window hole diameter (mm) — clears 110° FoV
WIN_Z       = 30.0;   // Window position along pipe from sensor-end (mm)

/* [ESP32-S3-DevKitC-1] */
ESP_W       = 25.5;   // Board width (mm)
ESP_L       = 68.58;  // Board length (mm)
ESP_T       = 1.6;    // PCB thickness (mm)
USBC_W      = 13.0;   // USB-C notch width (mm) — extra for cable clearance
USBC_H      = 8.0;    // USB-C notch height (mm)

/* [Shared Mechanical] */
WALL        = 3.685;  // Shell wall thickness = PVC wall (matches real pipe)
STANDOFF_H  = 3.0;    // PCB standoff height (mm)
STANDOFF_OD = 4.5;    // Standoff boss OD (mm)
TAP_HOLE    = 2.2;    // M2 tap diameter (mm)
TAB_W       = 4.5;    // Alignment tab width (mm)
TAB_H       = 2.0;    // Alignment tab height (protrusion) (mm)
TAB_TOL     = 0.3;    // Tab-to-slot clearance (mm)
TAB_Z1      = 20.0;   // First tab position along pipe (mm)
TAB_Z2      = 100.0;  // Second tab position (mm)
GROOVE_W    = 2.5;    // Zip-tie groove width (mm)
GROOVE_D    = 1.5;    // Zip-tie groove depth (mm)
GROOVE_Z1   = 15.0;   // First groove position (mm)
GROOVE_Z2   = 105.0;  // Second groove position (mm)

/* [End Cap] */
CAP_T       = 4.0;    // Cap flange thickness (mm)
CAP_SPIGOT  = 10.0;   // Spigot length (press-fit into pipe ID) (mm)
CAP_FIT     = 0.4;    // Press-fit tolerance (mm) — print slightly undersize

/* [Mounting Bracket] */
BKT_PLATE_W = 62.0;   // Plate width (mm)
BKT_PLATE_L = 42.0;   // Plate length — cabinet contact (mm)
BKT_PLATE_T = 3.0;    // Plate thickness (mm)
BKT_ARM_T   = 3.5;    // Cradle arm wall thickness (mm)
BKT_HOLE_D  = 4.5;    // Screw hole diameter (#8 or M4)
BKT_HOLE_SP = 40.0;   // Hole spacing (center-to-center) (mm)

/* [Render Mode] */
SHOW_SENSOR_HALF  = true;
SHOW_ESP32_HALF   = false;
SHOW_END_CAP      = false;
SHOW_BRACKET      = false;
SHOW_ASSEMBLY     = false;  // override: positions all parts
SHOW_COMPONENTS   = false;  // ghost PCBs for clearance check

$fn = 64;

// ============================================================
// DERIVED VALUES
// ============================================================
PIPE_WALL   = (PIPE_OD - PIPE_ID) / 2;  // 3.685mm
R_OUT       = PIPE_OD / 2;
R_IN        = PIPE_ID / 2;

// ============================================================
// MODULE: HALF SHELL
// Pipe axis = Z. Split plane = Y=0 (flat faces).
// upper=true  → positive-Y half (ESP32 half, curved face up)
// upper=false → negative-Y half (sensor half, curved face down)
// ============================================================
module half_shell(upper=true) {
    // The D-shaped cross section:
    //  - Outer arc = pipe OD (curved wall)
    //  - Inner arc = pipe ID (curved bore)
    //  - Flat chord = connects the two at Y=0

    difference() {
        union() {
            // Outer arc half
            difference() {
                cylinder(h=PIPE_L, r=R_OUT, center=false);
                cylinder(h=PIPE_L + 2, r=R_IN, center=false, $fn=64);
                if (upper)
                    translate([-R_OUT-1, -(R_OUT+1), -0.5])
                        cube([2*(R_OUT+1), R_OUT+1, PIPE_L+1]);  // remove -Y half
                else
                    translate([-R_OUT-1, 0, -0.5])
                        cube([2*(R_OUT+1), R_OUT+1, PIPE_L+1]);  // remove +Y half
            }

            // Flat closing plate (D-face)
            // Width = pipe ID chord = 2*R_IN
            // Thickness = pipe wall (WALL)
            if (upper)
                translate([-R_IN, 0, 0])
                    cube([2*R_IN, WALL, PIPE_L]);
            else
                translate([-R_IN, -WALL, 0])
                    cube([2*R_IN, WALL, PIPE_L]);
        }

        // Zip-tie grooves on OD (circumferential ring groove)
        for (gz = [GROOVE_Z1, GROOVE_Z2]) {
            translate([0, 0, gz - GROOVE_W/2])
            difference() {
                cylinder(h=GROOVE_W, r=R_OUT + 0.1);
                cylinder(h=GROOVE_W + 0.1, r=R_OUT - GROOVE_D);
            }
        }
    }
}


// ============================================================
// MODULE: SENSOR HALF (lower — faces stove)
// ============================================================
module sensor_half() {
    difference() {
        half_shell(upper=false);

        // Camera window — radial bore through curved wall at WIN_Z
        // Drill direction: from bottom (+Y is into the half from outside)
        translate([0, -R_OUT - 2, WIN_Z])
            rotate([-90, 0, 0])
                cylinder(h=PIPE_OD + 4, r=WIN_D/2);

        // Alignment slots (receive tabs from ESP32 half)
        for (tz = [TAB_Z1, TAB_Z2]) {
            translate([-(TAB_W + TAB_TOL)/2,
                       -(TAB_H + TAB_TOL) - WALL,
                        tz - 1.75])
                cube([TAB_W + TAB_TOL,
                      TAB_H + TAB_TOL,
                      3.5]);
        }
    }

    // Standoffs for MLX90640 breakout inside lower half
    // Board positioned: centered on X, near the sensor window end
    sb_z0 = WIN_Z + 5;  // board starts 5mm past camera window
    standoff_positions = [
        [ SENSOR_W/2 - 3, sb_z0 + 3],
        [-SENSOR_W/2 + 3, sb_z0 + 3],
        [ SENSOR_W/2 - 3, sb_z0 + SENSOR_L - 3],
        [-SENSOR_W/2 + 3, sb_z0 + SENSOR_L - 3]
    ];

    for (sp = standoff_positions) {
        translate([sp[0], -WALL, sp[1]])
            rotate([90, 0, 0])
            difference() {
                cylinder(h=STANDOFF_H, r=STANDOFF_OD/2);
                translate([0, 0, -0.5])
                    cylinder(h=STANDOFF_H + 1, r=TAP_HOLE/2);
            }
    }
}


// ============================================================
// MODULE: ESP32 HALF (upper — faces cabinet)
// ============================================================
module esp32_half() {
    difference() {
        half_shell(upper=true);

        // USB-C cable exit notch at Z=0 end (cable exits toward one end)
        // Cut a U-channel through the flat face and pipe end wall
        translate([-USBC_W/2, -1, -0.5])
            cube([USBC_W, WALL + 3, USBC_H + 0.5]);

        // Alignment tab slots for mating (receive tabs from sensor side)
        // These are NOT needed on the ESP half — the ESP half HAS the tabs.
        // (slots are on sensor half; tabs are on ESP half — see union below)
    }

    // Alignment tabs (protrude from flat face at Y=0, into sensor half slot)
    for (tz = [TAB_Z1, TAB_Z2]) {
        translate([-TAB_W/2, 0, tz - 1.5])
            cube([TAB_W, TAB_H, 3.0]);
    }

    // Standoffs for ESP32-S3-DevKitC-1 inside upper half
    // USB-C end is at Z=0; board runs from Z=7 to Z=7+68.58
    eb_z0 = 7.0;
    esp_standoff_positions = [
        [ ESP_W/2 - 3, eb_z0 + 3],
        [-ESP_W/2 + 3, eb_z0 + 3],
        [ ESP_W/2 - 3, eb_z0 + ESP_L - 3],
        [-ESP_W/2 + 3, eb_z0 + ESP_L - 3]
    ];

    for (sp = esp_standoff_positions) {
        translate([sp[0], WALL, sp[1]])
            rotate([-90, 0, 0])
            difference() {
                cylinder(h=STANDOFF_H, r=STANDOFF_OD/2);
                translate([0, 0, -0.5])
                    cylinder(h=STANDOFF_H + 1, r=TAP_HOLE/2);
            }
    }
}


// ============================================================
// MODULE: END CAP (press-fit into pipe ID)
// ============================================================
module end_cap(vented=false) {
    union() {
        // Flange (sits outside pipe end)
        cylinder(h=CAP_T, r=R_OUT + 1.0);

        // Spigot (inserts into pipe ID)
        translate([0, 0, CAP_T])
            cylinder(h=CAP_SPIGOT, r=(R_IN - CAP_FIT/2));
    }

    // Optional: vent hole for cable routing
    if (vented) {
        translate([0, 0, -0.5])
            cylinder(h=CAP_T + CAP_SPIGOT + 1, r=3.5);
    }
}


// ============================================================
// MODULE: MOUNTING BRACKET
// Flat plate + U-shaped pipe cradle
// Pipe rotates freely in cradle until M3 clamp screw tightened
// ============================================================
module mounting_bracket() {
    pipe_clear = 0.6;   // clearance in cradle (allows rotation)
    cradle_r   = R_OUT + pipe_clear;
    cradle_wall = BKT_ARM_T;
    cradle_depth = BKT_PLATE_L * 0.8;
    arm_z_top  = -(BKT_PLATE_T);
    arm_z_bot  = arm_z_top - (cradle_r + cradle_wall);
    wrap_angle = 220;    // degrees of arc cradle wraps pipe

    difference() {
        union() {
            // Mounting plate (flat against cabinet underside)
            translate([-BKT_PLATE_W/2, -BKT_PLATE_L/2, 0])
                cube([BKT_PLATE_W, BKT_PLATE_L, BKT_PLATE_T]);

            // U-shaped cradle centered on plate
            // Cradle is centered at Z = -(plate_T + cradle_r + cradle_wall)
            // Y axis = along pipe axis
            translate([0, 0, arm_z_top]) {
                difference() {
                    // Outer cradle shell
                    translate([0, -cradle_depth/2, -(cradle_r + cradle_wall)])
                        rotate([90, 0, 0])
                        rotate([0, 0, 180 - wrap_angle/2])
                        linear_extrude(height=cradle_depth)
                            arc_section(cradle_r + cradle_wall,
                                        cradle_r,
                                        wrap_angle);

                    // Pipe clearance bore
                    translate([0, -cradle_depth/2 - 1, -(cradle_r + cradle_wall)])
                        rotate([90, 0, 0])
                        cylinder(h=cradle_depth + 2, r=cradle_r);
                }

                // Side walls connecting plate to cradle
                for (sy = [-cradle_depth/2, cradle_depth/2 - cradle_wall])
                    translate([-cradle_r - cradle_wall,
                                sy,
                               -(cradle_r + cradle_wall)])
                        cube([2*(cradle_r + cradle_wall),
                              cradle_wall,
                              cradle_r + cradle_wall]);
            }
        }

        // Cabinet mounting screw holes (through plate)
        for (sx = [-BKT_HOLE_SP/2, BKT_HOLE_SP/2]) {
            translate([sx, 0, -0.5])
                cylinder(h=BKT_PLATE_T + 1, r=BKT_HOLE_D/2);
            // Countersink for flathead screw
            translate([sx, 0, BKT_PLATE_T - 1.5])
                cylinder(h=2, r1=BKT_HOLE_D/2, r2=BKT_HOLE_D);
        }

        // Clamp screw hole through one cradle arm (M3 clearance = 3.4mm)
        translate([-(cradle_r + cradle_wall + 1),
                    0,
                   -(BKT_PLATE_T + cradle_r/2 + cradle_wall/2)])
            rotate([0, 90, 0])
                cylinder(h=cradle_r*2 + cradle_wall*2 + 2, r=1.75);
    }
}

// 2D arc section for cradle extrusion
module arc_section(r_outer, r_inner, angle) {
    difference() {
        circle(r=r_outer);
        circle(r=r_inner);
        // Remove the open side
        rotate([0, 0, angle/2])
            translate([-r_outer*2, 0, 0])
                square([r_outer*4, r_outer*2]);
    }
}


// ============================================================
// GHOST PCB COMPONENTS (clearance visualization)
// ============================================================
module mlx90640_ghost() {
    color("green", 0.5)
        translate([-SENSOR_W/2, -WALL - STANDOFF_H - SENSOR_T,
                    WIN_Z + 5])
            cube([SENSOR_W, SENSOR_T, SENSOR_L]);

    // TO-39 sensor dome
    color("silver", 0.7)
        translate([0, -WALL - STANDOFF_H - SENSOR_T - 4.5,
                    WIN_Z + 5 + SENSOR_L/2])
            sphere(r=4.5);
}

module esp32_ghost() {
    color("darkgreen", 0.5)
        translate([-ESP_W/2, WALL + STANDOFF_H, 7])
            cube([ESP_W, ESP_T, ESP_L]);

    // USB-C connector
    color("silver", 0.7)
        translate([-USBC_W/2 + 2, WALL + STANDOFF_H, -3])
            cube([USBC_W - 4, 3.3, 8]);
}


// ============================================================
// RENDER
// ============================================================

if (SHOW_ASSEMBLY) {
    // Show all parts in their assembled position
    sensor_half();
    translate([0, 0, 0]) rotate([180, 0, 0])
        translate([0, -PIPE_OD, 0])
            esp32_half();
    translate([0, 0, -CAP_T]) end_cap(vented=false);
    translate([0, 0, PIPE_L]) end_cap(vented=false);
    if (SHOW_COMPONENTS) {
        mlx90640_ghost();
        esp32_ghost();
    }
} else {
    if (SHOW_SENSOR_HALF) sensor_half();
    if (SHOW_ESP32_HALF)  esp32_half();
    if (SHOW_END_CAP)     end_cap(vented=true);
    if (SHOW_BRACKET)     mounting_bracket();
    if (SHOW_COMPONENTS && SHOW_SENSOR_HALF) mlx90640_ghost();
    if (SHOW_COMPONENTS && SHOW_ESP32_HALF)  esp32_ghost();
}
