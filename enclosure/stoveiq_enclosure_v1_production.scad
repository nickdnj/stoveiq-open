// StoveIQ Enclosure — Parametric Wedge Design v2
// Under-cabinet mount: flat top against cabinet, angled bottom faces stove
// Camera looks DOWN through angled bottom face
// Nick DeMarco — 2026-03-30
//
//  CABINET SURFACE (flat)
//  ┌─────────────────────┐ ← flat top, mounts to cabinet
//  │                     │
//  │   PCB sits here     │  back_h (tall, toward wall)
//  │                     │
//  └─────────\           │  front_h (short, toward room)
//     IR window \────────┘
//        looks    \
//         down      \ ← 35° angle toward stove
//          at         \
//          stove        ▼

/* [Board Dimensions] */
pcb_w = 45;          // PCB width (X) mm
pcb_d = 35;          // PCB depth (Y) mm
pcb_h = 1.6;         // PCB thickness mm

/* [Component Heights] */
buzzer_h = 9.5;      // Tallest component (buzzer)
sensor_h = 4.5;      // MLX90640 TO-39 height
sensor_dia = 9.2;    // TO-39 can diameter
usbc_w = 9;          // USB-C width
usbc_h = 3.5;        // USB-C height

/* [Sensor Position — from PCB origin (bottom-left)] */
sensor_pcb_x = 40;   // MLX90640 X on PCB
sensor_pcb_y = 17;   // MLX90640 Y on PCB

/* [Enclosure] */
wall = 2.0;          // Shell thickness
gap = 0.5;           // PCB clearance all around
standoff_h = 3;      // PCB standoff height above floor
sensor_angle = 35;   // Camera tilt from vertical (degrees)
corner_r = 2;        // Fillet radius
ir_aperture = 10;    // IR window diameter (> sensor for FOV margin)

/* [Derived — don't edit] */
// Internal cavity
cav_w = pcb_w + gap * 2;
cav_d = pcb_d + gap * 2;
comp_h = buzzer_h + pcb_h + standoff_h + 2; // total internal height needed

// Outer box
out_w = cav_w + wall * 2;  // ~50mm
out_d = cav_d + wall * 2;  // ~40mm

// Wedge: back wall is full height, front is shorter
// The bottom face tilts at sensor_angle
back_h = comp_h + wall * 2;         // full height at back (wall side)
front_h = back_h - out_d * tan(sensor_angle);
safe_front_h = max(front_h, wall + 2);

echo(str("=== StoveIQ Enclosure ==="));
echo(str("Outer: ", out_w, " x ", out_d, "mm"));
echo(str("Back height: ", back_h, "mm"));
echo(str("Front height: ", safe_front_h, "mm"));
echo(str("Sensor angle: ", sensor_angle, "°"));

// ============================================================
// HELPERS
// ============================================================
module rcube(w, d, h, r) {
    // Rounded-corner box
    hull() for (x=[r, w-r], y=[r, d-r])
        translate([x, y, 0]) cylinder(h=h, r=r, $fn=24);
}

// ============================================================
// MAIN ENCLOSURE — open bottom, wedge profile
// The TOP is flat (cabinet mount surface)
// The BOTTOM is angled (camera face, faces stove)
// ============================================================
module enclosure() {
    difference() {
        // Outer wedge solid
        hull() {
            // Back edge (y=out_d) — full height
            translate([0, out_d - 0.01, 0])
                rcube(out_w, 0.01, back_h, corner_r);
            // Front edge (y=0) — shorter
            translate([0, 0, 0])
                rcube(out_w, 0.01, safe_front_h, corner_r);
        }

        // Hollow cavity — offset by wall on all sides
        // Same wedge profile but smaller
        hull() {
            translate([wall, out_d - 0.01, wall])
                cube([cav_w, 0.01, back_h - wall * 2]);
            translate([wall, wall, wall])
                cube([cav_w, 0.01, safe_front_h - wall * 2]);
        }

        // --- CUTOUTS ---

        // IR CAMERA WINDOW — hole through the angled bottom face
        // The bottom face slopes from z=0 at back to z=0 at front
        // but the bottom is actually the angled surface
        // Since the top is flat at z=back_h (back) and z=safe_front_h (front),
        // and the bottom is at z=0, the angled face IS the top.
        // Wait — let me reconsider: for under-cabinet mount,
        // the flat surface (TOP at z=back_h) goes against the cabinet.
        // The BOTTOM face at z=0 is what faces the stove.
        // But z=0 is flat... The wedge slopes the TOP face.
        //
        // CORRECTION: The wedge should slope the BOTTOM, not the top.
        // Flat top = cabinet mount. Angled bottom = camera face.
        // I need to flip the geometry.
    }
}

// ============================================================
// BETTER APPROACH: flat top, angled bottom
// ============================================================
module enclosure_v2() {
    // The top face is flat at z = back_h (constant)
    // The bottom face angles from z=0 at back to z=(back_h - safe_front_h) at front
    // This means the front is THINNER and the bottom slopes up toward the front

    total_h = back_h; // constant top height

    difference() {
        // Outer solid — simple box, then cut the angled bottom
        rcube(out_w, out_d, total_h, corner_r);

        // Cut angled bottom face — a wedge-shaped removal from below
        // Remove everything below the angled plane
        // The plane goes from z=0 at y=out_d (back) to z=(total_h - safe_front_h) at y=0 (front)
        bottom_rise = total_h - safe_front_h; // how much the bottom rises at front

        if (bottom_rise > 0) {
            hull() {
                // Front: cut up to bottom_rise
                translate([-1, -1, -1])
                    cube([out_w + 2, 0.01, bottom_rise + 1]);
                // Back: cut nothing (z=0, zero-height wedge)
                translate([-1, out_d + 0.99, -1])
                    cube([out_w + 2, 0.01, 0.01]);
            }
        }

        // Hollow out cavity — same angled floor, inset by wall
        inner_bottom_rise = bottom_rise + wall; // inner floor is wall above outer
        translate([wall, wall, 0])
            hull() {
                // Front inner floor
                translate([0, -0.01, inner_bottom_rise])
                    cube([cav_w, 0.01, total_h - wall - inner_bottom_rise]);
                // Back inner floor
                translate([0, cav_d + 0.01, wall])
                    cube([cav_w, 0.01, total_h - wall * 2]);
            }

        // --- IR CAMERA WINDOW ---
        // Hole through the angled bottom face
        // The sensor sits at (sensor_pcb_x, sensor_pcb_y) on the PCB
        // PCB origin in enclosure coords: (wall+gap, wall+gap, floor + standoff_h)
        sx = wall + gap + sensor_pcb_x;
        sy = wall + gap + sensor_pcb_y;

        // The angled bottom surface height at this Y position:
        frac = 1 - (sy / out_d); // 0 at back, 1 at front
        z_floor = frac * bottom_rise;

        // Cut perpendicular to the angled surface
        // Surface normal angle from vertical:
        floor_angle = atan2(bottom_rise, out_d);

        translate([sx, sy, z_floor - 1])
            rotate([floor_angle, 0, 0])
                cylinder(h=wall + 3, d=ir_aperture, $fn=48);

        // --- USB-C PORT ---
        // Left side (x=0), centered on PCB depth
        usb_y = wall + gap + pcb_d/2;
        frac_usb = 1 - (usb_y / out_d);
        z_floor_usb = frac_usb * bottom_rise;
        usb_z = z_floor_usb + wall + standoff_h + pcb_h;

        translate([-1, usb_y - usbc_w/2, usb_z])
            cube([wall + 2, usbc_w, usbc_h]);

        // --- LED WINDOW ---
        // WS2812B at PCB (20, 31) — front face
        led_x = wall + gap + 20;
        translate([led_x - 2.5, -1, total_h - safe_front_h + wall + standoff_h + pcb_h])
            cube([5, wall + 2, 4]);

        // --- BUTTON ACCESS ---
        // SW1 at PCB (5, 31), SW2 at PCB (12, 31) — front face
        for (bx = [5, 12]) {
            translate([wall + gap + bx - 3, -1,
                       total_h - safe_front_h + wall + standoff_h + pcb_h])
                cube([6, wall + 2, 4]);
        }

        // --- VENT SLOTS (sides) ---
        for (i = [0:4]) {
            vy = 8 + i * 6;
            // Right side
            translate([out_w - wall - 1, vy, total_h - 10])
                cube([wall + 2, 1.5, 7]);
            // Left side
            translate([-1, vy, total_h - 10])
                cube([wall + 2, 1.5, 7]);
        }

        // --- MOUNTING HOLES (top face, countersunk for cabinet screws) ---
        for (mx = [out_w * 0.25, out_w * 0.75]) {
            translate([mx, out_d/2, total_h - wall - 1]) {
                cylinder(h=wall + 2, d=4, $fn=24);   // Through hole
                translate([0, 0, 1])
                    cylinder(h=wall + 1, d1=4, d2=8, $fn=24); // Countersink
            }
        }
    }

    // --- PCB STANDOFFS ---
    for (x = [wall + gap + 3, wall + gap + pcb_w - 3],
         y = [wall + gap + 3, wall + gap + pcb_d - 3]) {
        frac_s = 1 - (y / out_d);
        z_floor_s = frac_s * (total_h - safe_front_h);
        translate([x, y, z_floor_s + wall]) {
            difference() {
                cylinder(h=standoff_h, d=5, $fn=24);
                cylinder(h=standoff_h + 1, d=2.2, $fn=24);
            }
        }
    }
}

// ============================================================
// PCB + COMPONENTS MOCKUP (transparent overlay)
// ============================================================
module pcb_ghost() {
    bottom_rise = back_h - safe_front_h;

    // PCB position
    pcb_x = wall + gap;
    pcb_y = wall + gap;
    frac_pcb_back = 1 - ((pcb_y + pcb_d) / out_d);
    frac_pcb_front = 1 - (pcb_y / out_d);
    // PCB sits on standoffs, back edge:
    z_back = frac_pcb_back * bottom_rise + wall + standoff_h;
    z_front = frac_pcb_front * bottom_rise + wall + standoff_h;
    z_pcb = (z_back + z_front) / 2; // approximate average

    // Green PCB
    color("green", 0.6)
        translate([pcb_x, pcb_y, z_pcb])
            cube([pcb_w, pcb_d, pcb_h]);

    // MLX90640 (silver TO-39 can)
    color("silver", 0.8)
        translate([pcb_x + sensor_pcb_x, pcb_y + sensor_pcb_y, z_pcb + pcb_h])
            cylinder(h=sensor_h, d=sensor_dia, $fn=32);

    // Buzzer (black cylinder)
    color("DarkSlateGray", 0.8)
        translate([pcb_x + 35, pcb_y + 29, z_pcb + pcb_h])
            cylinder(h=buzzer_h, d=12, $fn=32);

    // USB-C (silver rectangle)
    color("silver", 0.8)
        translate([pcb_x - 1, pcb_y + pcb_d/2 - usbc_w/2, z_pcb + pcb_h])
            cube([3, usbc_w, usbc_h]);

    // AMS1117 LDO
    color("DimGray", 0.8)
        translate([pcb_x + 16, pcb_y + 14, z_pcb + pcb_h])
            cube([6.5, 3.5, 1.8]);

    // Buttons
    for (bx = [5, 12])
        color("yellow", 0.8)
            translate([pcb_x + bx - 3, pcb_y + 28, z_pcb + pcb_h])
                cube([6, 6, 3.5]);
}

// ============================================================
// RENDER
// ============================================================

// Main enclosure
enclosure_v2();

// Ghost PCB overlay (transparent)
%pcb_ghost();
