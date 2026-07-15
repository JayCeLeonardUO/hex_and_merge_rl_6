#!/usr/bin/env python3
"""Leyline connection prototype: how spell-connection cards read on the board.

    python3 leyline_proto.py          # interactive
    python3 leyline_proto.py --shot   # screenshot after a second, then exit

Rules being prototyped: leyline cards sit on tiles and conduct; a spell card
only activates while a chain of adjacent leyline tiles connects the PLAYER to
the SPELL. Connected network = energized (bright beams, pulses flowing toward
the spell, spell card lit and bobbing). Broken network = inert grey.

Controls:
    left click        toggle a leyline card on a tile
    drag P / S        move the player / spell markers (click their tile, drag)
    right drag        orbit          wheel   zoom
    s                 screenshot     x       clear leylines
"""

import json
import math
import sys
from pathlib import Path

import pyray as rl
from raylib import ffi

LOOK_FILE = Path(__file__).parent / "leyline_look.json"
SCREEN_W, SCREEN_H = 900, 620
GRID_R = 3
HEX = 1.0
TILE_H = 0.30

DIRS = ((1, 0), (1, -1), (0, -1), (-1, 0), (-1, 1), (0, 1))

RL_LINES = 0x0001
RL_QUADS = 0x0007
RL_TRIANGLES = 0x0004


def axial_to_world(q, r):
    x = HEX * math.sqrt(3.0) * (q + r / 2.0)
    z = HEX * 1.5 * r
    return x, z


def on_board(q, r):
    return abs(q) <= GRID_R and abs(r) <= GRID_R and abs(q + r) <= GRID_R


def pick_tile(mouse, cam):
    """Nearest tile under the cursor via the tile-top plane, or None."""
    ray = rl.get_screen_to_world_ray(mouse, cam)
    if abs(ray.direction.y) < 1e-4:
        return None
    t = (TILE_H - ray.position.y) / ray.direction.y
    if t <= 0:
        return None
    px = ray.position.x + ray.direction.x * t
    pz = ray.position.z + ray.direction.z * t
    best, best_d = None, HEX * 0.95
    for q in range(-GRID_R, GRID_R + 1):
        for r in range(-GRID_R, GRID_R + 1):
            if not on_board(q, r):
                continue
            x, z = axial_to_world(q, r)
            d = math.hypot(px - x, pz - z)
            if d < best_d:
                best, best_d = (q, r), d
    return best


def bezier(a, b, arc, t):
    """Point at t along the raised arc between two tile centers."""
    mx, mz = (a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0
    x = (1 - t) ** 2 * a[0] + 2 * (1 - t) * t * mx + t * t * b[0]
    z = (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * mz + t * t * b[1]
    y = TILE_H + 0.08 + 4.0 * arc * t * (1 - t)
    return x, y, z


def connected_component(leylines, start, extra):
    """Tiles reachable from `start` walking leylines (start and the tiles in
    `extra` conduct even without a leyline card on them)."""
    conductive = set(leylines) | {start} | set(extra)
    seen = {start}
    frontier = [start]
    while frontier:
        q, r = frontier.pop()
        for dq, dr in DIRS:
            n = (q + dq, r + dr)
            if n in conductive and n not in seen and on_board(*n):
                seen.add(n)
                frontier.append(n)
    return seen


def main():
    shot_mode = "--shot" in sys.argv

    rl.init_window(SCREEN_W, SCREEN_H, "leyline proto")
    rl.set_target_fps(60)

    cam = rl.Camera3D(rl.Vector3(0.0, 7.5, 8.5), rl.Vector3(0.0, 0.0, 0.3),
                      rl.Vector3(0.0, 1.0, 0.0), 40.0,
                      rl.CameraProjection.CAMERA_PERSPECTIVE)

    # the leyline network renders alone on this layer; the pen outline is
    # stamped from its silhouette in screen space during the composite
    line_rt = rl.load_render_texture(SCREEN_W, SCREEN_H)

    player = (-2, 1)
    spell = (2, -1)
    leylines = {(-1, 1), (0, 0), (1, 0)}  # a starter chain to react to
    dragging = None  # "player" | "spell"

    params = {
        "arc": ffi.new("float *", 0.22),
        "line_width": ffi.new("float *", 3.0),
        "glow": ffi.new("float *", 0.55),
        "pulse_speed": ffi.new("float *", 0.9),
        "pulses": ffi.new("float *", 3.0),
        "node_size": ffi.new("float *", 0.16),
        "hue": ffi.new("float *", 0.52),  # 0..1 around the wheel
        "irid": ffi.new("float *", 0.35),  # hue spread along a beam, 0 = flat
        "ink": ffi.new("float *", 2.0),    # black outline extra px, 0 = off
    }

    def load_look():
        if LOOK_FILE.is_file():
            data = json.loads(LOOK_FILE.read_text())
            for k, v in params.items():
                if k in data:
                    v[0] = float(data[k])

    def save_look():
        LOOK_FILE.write_text(json.dumps({k: round(v[0], 4) for k, v in params.items()},
                                        indent=4) + "\n")

    load_look()

    def line_color(bright, alpha=255):
        h = params["hue"][0] * 360.0
        c = rl.color_from_hsv(h, 0.55 if bright else 0.25, 1.0 if bright else 0.45)
        return (c.r, c.g, c.b, alpha)

    def irid_color(along, time, alpha=255):
        """Iridescent film color: hue slides along the beam and drifts with
        time, with a soft shimmer wave riding on top."""
        spread = params["irid"][0]
        h = (params["hue"][0]
             + spread * (0.55 * along
                         + 0.18 * math.sin(time * 1.4 + along * 6.2831)
                         + 0.08 * time)) % 1.0
        c = rl.color_from_hsv(h * 360.0, 0.62, 1.0)
        return (c.r, c.g, c.b, alpha)

    panel = rl.Rectangle(648, 12, 242, 390)
    frame = 0

    while not rl.window_should_close():
        frame += 1
        t = rl.get_time()
        mouse = rl.get_mouse_position()
        ui_mouse = rl.check_collision_point_rec(mouse, panel)

        # orbit + zoom
        if rl.is_mouse_button_down(rl.MouseButton.MOUSE_BUTTON_RIGHT) and not ui_mouse:
            d = rl.get_mouse_delta()
            off = rl.vector3_subtract(cam.position, cam.target)
            radius = rl.vector3_length(off)
            yaw = math.atan2(off.x, off.z) - d.x * 0.005
            pitch = math.asin(off.y / radius) - d.y * 0.005
            pitch = min(max(pitch, 0.2), 1.45)
            cam.position = rl.Vector3(cam.target.x + radius * math.cos(pitch) * math.sin(yaw),
                                      cam.target.y + radius * math.sin(pitch),
                                      cam.target.z + radius * math.cos(pitch) * math.cos(yaw))
        wheel = rl.get_mouse_wheel_move()
        if wheel != 0.0 and not ui_mouse:
            off = rl.vector3_subtract(cam.position, cam.target)
            cam.position = rl.vector3_add(cam.target, rl.vector3_scale(off, 1.0 - wheel * 0.08))

        # placement / marker dragging
        hover = pick_tile(mouse, cam) if not ui_mouse else None
        if rl.is_mouse_button_pressed(rl.MouseButton.MOUSE_BUTTON_LEFT) and hover is not None:
            if hover == player:
                dragging = "player"
            elif hover == spell:
                dragging = "spell"
            elif hover in leylines:
                leylines.discard(hover)
            else:
                leylines.add(hover)
        if not rl.is_mouse_button_down(rl.MouseButton.MOUSE_BUTTON_LEFT):
            dragging = None
        if dragging is not None and hover is not None and hover not in (player, spell):
            if dragging == "player":
                player = hover
            else:
                spell = hover
        if rl.is_key_pressed(rl.KeyboardKey.KEY_X):
            leylines.clear()

        # connectivity: ONLY leyline tiles conduct (player = source). The
        # spell is an endpoint: it activates when a live leyline touches it,
        # but it never extends the chain itself
        live = connected_component(leylines, player, extra=[])
        active = any((spell[0] + dq, spell[1] + dr) in live for dq, dr in DIRS)

        # segments: chain spans between conducting tiles + taps to the spell
        network = set(leylines) | {player}
        segments = []
        for (q, r) in network:
            for dq, dr in DIRS:
                n = (q + dq, r + dr)
                if n in network and (q, r) < n:
                    lit = (q, r) in live and n in live
                    segments.append(((q, r), n, lit))
                elif n == spell:
                    segments.append(((q, r), n, (q, r) in live))

        # --- layer 1: the leyline network alone, on transparent ---
        rl.begin_texture_mode(line_rt)
        rl.clear_background(rl.BLANK)
        rl.begin_mode_3d(cam)

        # leyline card markers: a flat rune diamond on each leyline tile
        for (q, r) in leylines:
            x, z = axial_to_world(q, r)
            lit = active and (q, r) in live
            c = irid_color((q * 0.31 + r * 0.17), t) if lit else line_color(False)
            s = params["node_size"][0] * (1.6 + (0.25 * math.sin(t * 2.0 + q + r) if lit else 0.0))
            rl.rl_push_matrix()
            rl.rl_translatef(x, TILE_H + 0.02, z)
            rl.rl_rotatef(t * (30.0 if lit else 6.0), 0.0, 1.0, 0.0)
            rl.rl_begin(RL_TRIANGLES)
            rl.rl_color4ub(c[0], c[1], c[2], 255)
            for k in range(4):  # flat diamond fan
                a0 = math.radians(90.0 * k)
                a1 = math.radians(90.0 * (k + 1))
                rl.rl_vertex3f(0.0, 0.0, 0.0)
                rl.rl_vertex3f(math.cos(a0) * s, 0.0, math.sin(a0) * s)
                rl.rl_vertex3f(math.cos(a1) * s, 0.0, math.sin(a1) * s)
            rl.rl_end()
            rl.rl_pop_matrix()

        # beams: color stroke, then an additive glow repeat for lit beams
        for pass_i in range(2):
            if pass_i == 1:
                rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
            for (a, b, lit) in segments:
                if pass_i == 1 and not lit:
                    continue
                aw, bw = axial_to_world(*a), axial_to_world(*b)
                width = params["line_width"][0] * (1.0 if pass_i == 0 else 2.6)
                alpha = 255 if pass_i == 0 else min(int(90 * params["glow"][0] * 2), 255)
                rl.rl_set_line_width(width)
                rl.rl_begin(RL_LINES)
                steps = 12
                # seed the hue walk with the segment midpoint so neighbouring
                # beams shimmer at different film phases
                seed = (aw[0] + bw[0]) * 0.35 + (aw[1] + bw[1]) * 0.21
                for k in range(steps):
                    if lit:
                        c0 = irid_color(k / steps + seed, t, alpha)
                        c1 = irid_color((k + 1) / steps + seed, t, alpha)
                    else:
                        c0 = c1 = line_color(False, alpha)
                    x0, y0, z0 = bezier(aw, bw, params["arc"][0], k / steps)
                    x1, y1, z1 = bezier(aw, bw, params["arc"][0], (k + 1) / steps)
                    rl.rl_color4ub(*c0)
                    rl.rl_vertex3f(x0, y0, z0)
                    rl.rl_color4ub(*c1)
                    rl.rl_vertex3f(x1, y1, z1)
                rl.rl_end()
            if pass_i == 1:
                rl.end_blend_mode()
        rl.rl_set_line_width(1.0)

        # energy pulses flowing along energized beams (direction: toward spell)
        if active:
            rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
            n_pulses = int(params["pulses"][0])
            for (a, b, lit) in segments:
                if not lit:
                    continue
                aw, bw = axial_to_world(*a), axial_to_world(*b)
                seed = (aw[0] + bw[0]) * 0.35 + (aw[1] + bw[1]) * 0.21
                for k in range(n_pulses):
                    pt = (t * params["pulse_speed"][0] + k / max(n_pulses, 1)) % 1.0
                    x, y, z = bezier(aw, bw, params["arc"][0], pt)
                    c = irid_color(pt + seed, t)
                    rl.draw_sphere(rl.Vector3(x, y, z), 0.055, rl.Color(c[0], c[1], c[2], 230))
                    rl.draw_sphere(rl.Vector3(x, y, z), 0.10, rl.Color(c[0], c[1], c[2], 70))
            rl.end_blend_mode()

        rl.end_mode_3d()
        rl.end_texture_mode()

        # --- layer 0: the scene ---
        rl.begin_drawing()
        rl.clear_background(rl.Color(16, 16, 24, 255))
        rl.begin_mode_3d(cam)

        # board
        for q in range(-GRID_R, GRID_R + 1):
            for r in range(-GRID_R, GRID_R + 1):
                if not on_board(q, r):
                    continue
                x, z = axial_to_world(q, r)
                cell = (q, r)
                top = rl.Color(60, 60, 70, 255)
                if cell in leylines:
                    top = rl.Color(52, 64, 84, 255)  # leyline card tint
                if cell == hover:
                    top = rl.Color(84, 84, 100, 255)
                rl.draw_cylinder(rl.Vector3(x, 0, z), HEX * 0.92, HEX * 0.92, TILE_H, 6, top)
                rl.draw_cylinder_wires(rl.Vector3(x, 0, z), HEX * 0.92, HEX * 0.92, TILE_H, 6,
                                       rl.Color(120, 108, 128, 255))

        # player marker: a small pillar with a P-ish crystal top
        px, pz = axial_to_world(*player)
        rl.draw_cylinder(rl.Vector3(px, TILE_H, pz), 0.16, 0.22, 0.55, 8, rl.Color(230, 210, 170, 255))
        rl.draw_sphere(rl.Vector3(px, TILE_H + 0.72, pz), 0.14, rl.Color(255, 240, 200, 255))

        rl.end_mode_3d()

        # --- pen outline composite: the network layer's silhouette stamped
        # in ink around itself (8 screen-space offsets, tinted black so only
        # the alpha shape survives), then the color layer once on top ---
        src = rl.Rectangle(0, 0, SCREEN_W, -SCREEN_H)
        ink = params["ink"][0]
        if ink > 0.1:
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1),
                           (-1, -1), (-1, 1), (1, -1), (1, 1)):
                rl.draw_texture_pro(line_rt.texture, src,
                                    rl.Rectangle(dx * ink, dy * ink, SCREEN_W, SCREEN_H),
                                    rl.Vector2(0, 0), 0.0, rl.Color(12, 10, 18, 255))
        rl.draw_texture_pro(line_rt.texture, src,
                            rl.Rectangle(0, 0, SCREEN_W, SCREEN_H),
                            rl.Vector2(0, 0), 0.0, rl.WHITE)

        # --- layer 2: markers that sit above the network ---
        rl.begin_mode_3d(cam)

        # spell card: a floating card quad over its tile; active = lit + bob
        sx, sz = axial_to_world(*spell)
        bob = math.sin(t * 2.4) * 0.06 if active else 0.0
        base_y = TILE_H + 0.62 + bob
        card_c = line_color(active)
        rl.rl_push_matrix()
        rl.rl_translatef(sx, base_y, sz)
        rl.rl_rotatef(t * (40.0 if active else 8.0), 0.0, 1.0, 0.0)
        rl.rl_begin(RL_QUADS)
        for face in (1.0, -1.0):  # both faces, so the spin never shows a hole
            rl.rl_color4ub(30, 26, 40, 255)
            rl.rl_vertex3f(-0.28 * face, 0.42, 0.0)
            rl.rl_vertex3f(-0.28 * face, -0.42, 0.0)
            rl.rl_vertex3f(0.28 * face, -0.42, 0.0)
            rl.rl_vertex3f(0.28 * face, 0.42, 0.0)
            rl.rl_color4ub(card_c[0], card_c[1], card_c[2], 255)
            rl.rl_vertex3f(-0.22 * face, 0.36, 0.01 * face)
            rl.rl_vertex3f(-0.22 * face, -0.36, 0.01 * face)
            rl.rl_vertex3f(0.22 * face, -0.36, 0.01 * face)
            rl.rl_vertex3f(0.22 * face, 0.36, 0.01 * face)
        rl.rl_end()
        rl.rl_pop_matrix()
        if active:  # activation aura
            rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
            ring = 0.5 + 0.08 * math.sin(t * 3.0)
            rl.draw_cylinder(rl.Vector3(sx, TILE_H + 0.01, sz), ring, ring, 0.02, 24,
                             rl.Color(card_c[0], card_c[1], card_c[2], 60))
            rl.draw_sphere(rl.Vector3(sx, base_y, sz), 0.55, rl.Color(card_c[0], card_c[1], card_c[2], 26))
            rl.end_blend_mode()

        rl.end_mode_3d()

        # panel
        rl.gui_window_box(panel, "leyline look")
        px2, py = int(panel.x + 10), int(panel.y + 30)
        rl.gui_label(rl.Rectangle(px2, py, 222, 14),
                     f"{'SPELL ACTIVE' if active else 'not connected'}   lines: {len(leylines)}")

        def slider(i, label, key, lo, hi, fmt="{:.2f}"):
            y = py + 22 + i * 30
            rl.gui_label(rl.Rectangle(px2, y, 222, 12), f"{label}  {fmt.format(params[key][0])}")
            rl.gui_slider_bar(rl.Rectangle(px2, y + 12, 222, 13), "", "", params[key], lo, hi)

        slider(0, "arc height", "arc", 0.0, 0.8)
        slider(1, "line width", "line_width", 1.0, 8.0, "{:.1f}")
        slider(2, "glow", "glow", 0.0, 1.0)
        slider(3, "pulse speed", "pulse_speed", 0.0, 3.0)
        slider(4, "pulses per beam", "pulses", 0.0, 8.0, "{:.0f}")
        slider(5, "rune size", "node_size", 0.05, 0.5)
        slider(6, "hue", "hue", 0.0, 1.0)
        slider(7, "iridescence", "irid", 0.0, 1.0)
        slider(8, "ink width", "ink", 0.0, 5.0, "{:.1f}")

        if rl.gui_button(rl.Rectangle(px2, py + 22 + 9 * 30, 222, 24), "save look"):
            save_look()

        rl.draw_text("click: toggle leyline   drag P/S markers   x: clear",
                     12, SCREEN_H - 24, 18, rl.GRAY)
        rl.end_drawing()

        if rl.is_key_pressed(rl.KeyboardKey.KEY_S) or (shot_mode and frame == 60):
            rl.take_screenshot("leyline_proto_shot.png")
            if shot_mode:
                break

    rl.close_window()


if __name__ == "__main__":
    main()
