#!/usr/bin/env python3
"""Fireball charge mechanic prototype: place, connect, charge, detonate.

    python3 fireball_proto.py          # interactive
    python3 fireball_proto.py --shot   # scripted charge + boom screenshots

The mechanic being prototyped: a fireball card is PLACED on a tile as an
emplacement. It only charges while a leyline chain connects its tile to the
player -- one charge per lever pull. At full charge (default 2) it detonates,
hitting everything within one tile of it.

Reads: the emplacement's flame grows with stored charge and embers orbit it
(one per charge), the leyline chain shows whether it can charge at all, the
detonation plays an explosion sheet + shockwave ring + spark burst, and AOE
tiles flash. Enemy dummies caught in the ring are destroyed.

Controls:
    left click     cycle tile: empty > leyline > fireball > enemy > empty
    drag player    move the source        space / button   pull the lever
    right drag     orbit                  wheel            zoom
    x reset        s screenshot
"""

import json
import math
import sys
from pathlib import Path

import pyray as rl
from raylib import ffi

from fx_tester import bake_fx, draw_fx_quad, FOLDERS

LOOK_FILE = Path(__file__).parent / "fireball_look.json"
SCREEN_W, SCREEN_H = 900, 620
GRID_R = 3
HEX = 1.0
TILE_H = 0.30
DIRS = ((1, 0), (1, -1), (0, -1), (-1, 0), (-1, 1), (0, 1))

RL_LINES = 0x0001


def axial_to_world(q, r):
    return HEX * math.sqrt(3.0) * (q + r / 2.0), HEX * 1.5 * r


def on_board(q, r):
    return abs(q) <= GRID_R and abs(r) <= GRID_R and abs(q + r) <= GRID_R


def hex_dist(a, b):
    dq = a[0] - b[0]
    dr = a[1] - b[1]
    return (abs(dq) + abs(dr) + abs(dq + dr)) // 2


def pick_tile(mouse, cam):
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


def beam_point(a, b, arc, tt):
    mx, mz = (a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0
    x = (1 - tt) ** 2 * a[0] + 2 * (1 - tt) * tt * mx + tt * tt * b[0]
    z = (1 - tt) ** 2 * a[1] + 2 * (1 - tt) * tt * mz + tt * tt * b[1]
    y = TILE_H + 0.08 + 4.0 * arc * tt * (1 - tt)
    return x, y, z


def tapped(tile, live):
    """A spell tile activates when ANY neighbour is a live leyline tile --
    spells tap the network by adjacency, they never extend it."""
    return any((tile[0] + dq, tile[1] + dr) in live for dq, dr in DIRS)


def flood(conducting, start):
    seen = {start}
    frontier = [start]
    while frontier:
        q, r = frontier.pop()
        for dq, dr in DIRS:
            n = (q + dq, r + dr)
            if n in conducting and n not in seen and on_board(*n):
                seen.add(n)
                frontier.append(n)
    return seen


def main():
    shot_mode = "--shot" in sys.argv

    rl.init_window(SCREEN_W, SCREEN_H, "fireball charge proto")
    rl.set_target_fps(60)
    rl.rl_disable_backface_culling()

    cam = rl.Camera3D(rl.Vector3(0.0, 6.5, 8.5), rl.Vector3(0.0, 0.4, 0.3),
                      rl.Vector3(0.0, 1.0, 0.0), 40.0,
                      rl.CameraProjection.CAMERA_PERSPECTIVE)

    flames = sorted(n for n in FOLDERS if n.startswith("fire_"))
    booms = sorted(n for n in FOLDERS if n.startswith(("explosion_", "impact_", "dynamic_impact")))
    loaded = {}

    def sheet(name):
        if name not in loaded:
            baked = bake_fx(name)
            tex = rl.load_texture(str(baked[0]))
            rl.set_texture_filter(tex, rl.TextureFilter.TEXTURE_FILTER_BILINEAR)
            loaded[name] = (tex, baked[1])
        return loaded[name]

    # board state: the mechanic
    player = (-2, 1)
    leylines = {(-1, 1), (0, 0)}
    fireballs = {(1, 0): {"charge": 0, "level": 1}}  # alive emplacements
    dead_husks = set()        # spent fireballs cooling on the field for a turn
    enemies = {(2, 0), (1, -1)}
    hand_count = 0            # cards in hand (the starter fireball is placed)
    turn = 0
    dragging = False

    # the hand: one fireball card, grab it and drop it on a tile to place an
    # emplacement (it deals itself back afterwards). Screen-space rectangle
    CARD_W, CARD_H = 96, 136
    card_home = (SCREEN_W / 2 - 240, SCREEN_H - CARD_H - 14)
    card_pos = list(card_home)
    card_grabbed = False

    # fx state
    explosions = []  # {t0, pos(x,z)}
    sparks = []
    aoe_flash = []   # {t0, tiles}

    params = {
        "flame_size": ffi.new("float *", 0.9),
        "impact_size": ffi.new("float *", 2.6),
        "impact_dur": ffi.new("float *", 0.55),
        "charge_needed": ffi.new("float *", 2.0),
        "fps": ffi.new("float *", 14.0),
        "level": ffi.new("float *", 1.0),  # 1 base / 2 no husk / 3 no cast time
    }
    flame_ptr = ffi.new("int *", flames.index("fire_J") if "fire_J" in flames else 0)
    boom_ptr = ffi.new("int *", booms.index("explosion_B_R") if "explosion_B_R" in booms else 0)
    fscroll = ffi.new("int *", max(0, flame_ptr[0] - 2))
    bscroll = ffi.new("int *", max(0, boom_ptr[0] - 2))
    ffocus = ffi.new("int *", -1)
    bfocus = ffi.new("int *", -1)
    flame_bufs = [ffi.new("char[]", n.encode()) for n in flames]
    flame_arr = ffi.new("char *[]", flame_bufs)
    boom_bufs = [ffi.new("char[]", n.encode()) for n in booms]
    boom_arr = ffi.new("char *[]", boom_bufs)

    def load_look():
        if LOOK_FILE.is_file():
            data = json.loads(LOOK_FILE.read_text())
            for k, v in params.items():
                if k in data:
                    v[0] = float(data[k])
            if data.get("flame") in flames:
                flame_ptr[0] = flames.index(data["flame"])
            if data.get("boom") in booms:
                boom_ptr[0] = booms.index(data["boom"])

    def save_look():
        data = {k: round(v[0], 4) for k, v in params.items()}
        data["flame"] = flames[flame_ptr[0]]
        data["boom"] = booms[boom_ptr[0]]
        LOOK_FILE.write_text(json.dumps(data, indent=4) + "\n")

    load_look()

    def irid(along, time, alpha=255):
        h = (0.52 + 0.35 * (0.55 * along + 0.18 * math.sin(time * 1.4 + along * 6.2831) + 0.08 * time)) % 1.0
        c = rl.color_from_hsv(h * 360.0, 0.62, 1.0)
        return (c.r, c.g, c.b, alpha)

    def detonate(tile, now):
        nonlocal enemies
        x, z = axial_to_world(*tile)
        explosions.append({"t0": now, "pos": (x, z)})
        aoe_flash.append({"t0": now, "tiles": [(tile[0] + dq, tile[1] + dr) for dq, dr in ((0, 0),) + DIRS
                                               if on_board(tile[0] + dq, tile[1] + dr)]})
        for _ in range(26):
            ang = rl.get_random_value(0, 628) / 100.0
            up = rl.get_random_value(15, 100) / 100.0
            spd = rl.get_random_value(15, 55) / 10.0
            sparks.append({"pos": [x, TILE_H + 0.35, z],
                           "vel": [math.cos(ang) * spd * 0.6, up * spd, math.sin(ang) * spd * 0.6],
                           "life": 0.0, "max": rl.get_random_value(25, 60) / 100.0})
        killed = {e for e in enemies if hex_dist(e, tile) <= 1}
        for e in killed:
            ex, ez = axial_to_world(*e)
            for _ in range(10):
                ang = rl.get_random_value(0, 628) / 100.0
                sparks.append({"pos": [ex, TILE_H + 0.3, ez],
                               "vel": [math.cos(ang) * 1.6, rl.get_random_value(15, 45) / 10.0, math.sin(ang) * 1.6],
                               "life": 0.0, "max": 0.5})
        enemies -= killed

    def pull_lever(now):
        nonlocal turn
        turn += 1
        live = flood(set(leylines) | {player}, player)  # only leylines conduct
        need = max(1, int(params["charge_needed"][0]))
        # husks that spent last turn dead on the field come back to the hand
        for tile in list(dead_husks):
            dead_husks.discard(tile)
            hand_count += 1
        for tile in list(fireballs):
            if not tapped(tile, live):
                continue  # no adjacent live leyline: no charge this turn
            fb = fireballs[tile]
            fb["charge"] += 1
            if fb["charge"] >= need:
                del fireballs[tile]
                if fb["level"] >= 2:
                    hand_count += 1        # lvl 2+: straight back to the hand
                else:
                    dead_husks.add(tile)   # lvl 1: dead on the field for a turn
                detonate(tile, now)

    panel = rl.Rectangle(636, 12, 254, 596)
    frame = 0

    while not rl.window_should_close():
        frame += 1
        t = rl.get_time()
        dt = rl.get_frame_time()
        mouse = rl.get_mouse_position()
        ui_mouse = rl.check_collision_point_rec(mouse, panel)

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

        hover = pick_tile(mouse, cam) if not ui_mouse else None
        card_rect = rl.Rectangle(card_pos[0], card_pos[1], CARD_W, CARD_H)
        hand_level = int(params["level"][0])
        # 2048 rule: dropping onto an EQUAL-level fireball merges them into
        # one of the next level (two cards become one, capped at 3)
        can_merge = (hover is not None and hover in fireballs and hand_count > 0
                     and fireballs[hover]["level"] == hand_level and hand_level < 3)
        can_drop = (hover is not None and hover != player and hand_count > 0
                    and (can_merge or (hover not in fireballs and hover not in enemies
                                       and hover not in dead_husks)))

        if rl.is_mouse_button_pressed(rl.MouseButton.MOUSE_BUTTON_LEFT):
            if rl.check_collision_point_rec(mouse, card_rect) and hand_count > 0:
                card_grabbed = True
            elif hover is not None and not ui_mouse:
                # click-cycling handles the rest of the board furniture
                if hover == player:
                    dragging = True
                elif hover in fireballs:
                    del fireballs[hover]
                    hand_count += 1
                elif hover in dead_husks:
                    dead_husks.discard(hover)
                    hand_count += 1
                elif hover in leylines:
                    leylines.discard(hover)
                    enemies.add(hover)
                elif hover in enemies:
                    enemies.discard(hover)
                else:
                    leylines.add(hover)
        if card_grabbed:
            card_pos[0] = mouse.x - CARD_W / 2
            card_pos[1] = mouse.y - CARD_H / 2
        if not rl.is_mouse_button_down(rl.MouseButton.MOUSE_BUTTON_LEFT):
            if card_grabbed:
                if can_drop and can_merge:
                    fireballs[hover] = {"charge": 0, "level": hand_level + 1}
                    hand_count -= 1
                elif can_drop:
                    fireballs[hover] = {"charge": 0, "level": hand_level}
                    hand_count -= 1
                card_pos[:] = card_home
                card_grabbed = False
            dragging = False
        if dragging and hover is not None and hover not in leylines and hover not in fireballs and hover not in enemies:
            player = hover
        if rl.is_key_pressed(rl.KeyboardKey.KEY_SPACE):
            pull_lever(t)
        if rl.is_key_pressed(rl.KeyboardKey.KEY_X):
            leylines.clear()
            fireballs.clear()
            enemies.clear()

        for pcl in sparks:
            pcl["life"] += dt
            pcl["vel"][1] -= 6.5 * dt
            pcl["pos"][0] += pcl["vel"][0] * dt
            pcl["pos"][1] += pcl["vel"][1] * dt
            pcl["pos"][2] += pcl["vel"][2] * dt
        sparks[:] = [pcl for pcl in sparks if pcl["life"] < pcl["max"]]
        explosions[:] = [e for e in explosions if (t - e["t0"]) < params["impact_dur"][0]]
        aoe_flash[:] = [f for f in aoe_flash if (t - f["t0"]) < 0.5]

        # connectivity for this frame's visuals
        conducting = set(leylines) | {player}  # only leylines conduct
        live = flood(conducting, player)
        need = max(1, int(params["charge_needed"][0]))

        # level 3 removes the cast time: those fireballs detonate the instant
        # a live leyline touches them (placement next to one included)
        for tile in list(fireballs):
            if fireballs[tile]["level"] >= 3 and tapped(tile, live):
                del fireballs[tile]
                hand_count += 1
                detonate(tile, t)

        # AOE preview: every tile a placed fireball will hit, with a heat
        # level that rises as the charge builds (and pulses gently)
        aoe_preview = {}
        if card_grabbed and can_merge:
            aoe_preview[hover] = 1.0  # merge target burns bright
        elif card_grabbed and can_drop:
            for dq, dr in ((0, 0),) + DIRS:
                c = (hover[0] + dq, hover[1] + dr)
                if on_board(*c):
                    aoe_preview[c] = 0.45 + 0.15 * math.sin(t * 5.0)
        for tile, fb in fireballs.items():
            heat = (0.35 + 0.65 * fb["charge"] / need) * (0.75 + 0.25 * math.sin(t * 3.0 + tile[0]))
            for dq, dr in ((0, 0),) + DIRS:
                c = (tile[0] + dq, tile[1] + dr)
                if on_board(*c):
                    aoe_preview[c] = max(aoe_preview.get(c, 0.0), heat)

        rl.begin_drawing()
        rl.clear_background(rl.Color(14, 13, 20, 255))
        rl.begin_mode_3d(cam)

        # board
        for q in range(-GRID_R, GRID_R + 1):
            for r in range(-GRID_R, GRID_R + 1):
                if not on_board(q, r):
                    continue
                x, z = axial_to_world(q, r)
                cell = (q, r)
                top = rl.Color(58, 56, 66, 255)
                if cell in leylines:
                    top = rl.Color(50, 62, 82, 255)
                if cell in fireballs:
                    top = rl.Color(92, 54, 46, 255)
                if cell in dead_husks:
                    top = rl.Color(38, 34, 36, 255)  # scorched ash
                if cell == hover:
                    top = rl.Color(84, 84, 100, 255)
                prev = aoe_preview.get(cell, 0.0)
                if prev > 0.0:
                    top = rl.Color(min(255, top.r + int(70 * prev)),
                                   min(255, top.g + int(18 * prev)),
                                   min(255, top.b + int(8 * prev)), 255)
                flash = 0.0
                for f in aoe_flash:
                    if cell in f["tiles"]:
                        flash = max(flash, 1.0 - (t - f["t0"]) / 0.5)
                if flash > 0.0:
                    top = rl.Color(min(255, top.r + int(170 * flash)),
                                   min(255, top.g + int(90 * flash)),
                                   min(255, top.b + int(30 * flash)), 255)
                rl.draw_cylinder(rl.Vector3(x, 0, z), HEX * 0.92, HEX * 0.92, TILE_H, 6, top)
                rl.draw_cylinder_wires(rl.Vector3(x, 0, z), HEX * 0.92, HEX * 0.92, TILE_H, 6,
                                       rl.Color(118, 106, 126, 255))

        # dead husks: a charred mound smoldering on the tile until the next
        # turn hands the card back
        for tile in dead_husks:
            hx, hz = axial_to_world(*tile)
            rl.draw_sphere(rl.Vector3(hx, TILE_H + 0.10, hz), 0.24, rl.Color(28, 24, 26, 255))
            rl.draw_sphere(rl.Vector3(hx + 0.14, TILE_H + 0.07, hz - 0.08), 0.13, rl.Color(24, 20, 22, 255))
            for k in range(3):
                wy = (t * 0.35 + k * 0.33) % 1.0
                rl.draw_sphere(rl.Vector3(hx + math.sin(t * 0.8 + k * 2.1) * 0.09,
                                          TILE_H + 0.25 + wy * 0.55, hz),
                               0.06 + wy * 0.05, rl.Color(90, 86, 90, int(110 * (1.0 - wy))))
            ember = 0.5 + 0.5 * math.sin(t * 2.2 + tile[0])
            rl.draw_sphere(rl.Vector3(hx - 0.08, TILE_H + 0.14, hz + 0.1), 0.045,
                           rl.Color(255, int(90 + 60 * ember), 30, int(120 + 100 * ember)))

        # player + enemy dummies (opaque, keep depth writes)
        pxw, pzw = axial_to_world(*player)
        rl.draw_cylinder(rl.Vector3(pxw, TILE_H, pzw), 0.16, 0.22, 0.55, 8, rl.Color(230, 210, 170, 255))
        rl.draw_sphere(rl.Vector3(pxw, TILE_H + 0.72, pzw), 0.14, rl.Color(255, 240, 200, 255))
        for e in enemies:
            ex, ez = axial_to_world(*e)
            rl.draw_sphere(rl.Vector3(ex, TILE_H + 0.34, ez), 0.26, rl.Color(70, 50, 90, 255))
            rl.draw_cylinder_wires(rl.Vector3(ex, TILE_H, ez), 0.4, 0.4, 0.02, 16, rl.Color(210, 70, 60, 255))

        # everything from here is glowy fx: no depth writes, so billboards
        # never occlude rings/beams through their transparent pixels
        rl.rl_draw_render_batch_active()
        rl.rl_disable_depth_mask()

        # leyline beams: chain spans between conducting tiles plus a tap beam
        # from a live leyline to each adjacent fireball (spells never chain)
        beam_pairs = []
        for (q, r) in conducting:
            for dq, dr in DIRS:
                n = (q + dq, r + dr)
                if n in conducting and (q, r) < n:
                    beam_pairs.append(((q, r), n, (q, r) in live and n in live))
                elif n in fireballs:
                    beam_pairs.append(((q, r), n, (q, r) in live))
        for ((q, r), n, lit) in beam_pairs:
            if True:
                a, b = axial_to_world(q, r), axial_to_world(*n)
                seed = (a[0] + b[0]) * 0.35 + (a[1] + b[1]) * 0.21
                rl.rl_set_line_width(3.0)
                rl.rl_begin(RL_LINES)
                for k in range(10):
                    c0 = irid(k / 10 + seed, t) if lit else (90, 86, 110, 255)
                    c1 = irid((k + 1) / 10 + seed, t) if lit else (90, 86, 110, 255)
                    x0, y0, z0 = beam_point(a, b, 0.22, k / 10)
                    x1, y1, z1 = beam_point(a, b, 0.22, (k + 1) / 10)
                    rl.rl_color4ub(*c0)
                    rl.rl_vertex3f(x0, y0, z0)
                    rl.rl_color4ub(*c1)
                    rl.rl_vertex3f(x1, y1, z1)
                rl.rl_end()
        rl.rl_set_line_width(1.0)

        # AOE preview outlines: a pulsing ember hexagon on every tile the
        # blast will cover, hotter the closer the fireball is to detonating
        rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
        rl.rl_set_line_width(2.5)
        for cell, heat in aoe_preview.items():
            x, z = axial_to_world(*cell)
            rl.rl_begin(RL_LINES)
            rl.rl_color4ub(255, 120, 50, int(160 * heat))
            for k in range(6):
                a0 = math.radians(60.0 * k)
                a1 = math.radians(60.0 * (k + 1))
                rl.rl_vertex3f(x + math.cos(a0) * HEX * 0.8, TILE_H + 0.03, z + math.sin(a0) * HEX * 0.8)
                rl.rl_vertex3f(x + math.cos(a1) * HEX * 0.8, TILE_H + 0.03, z + math.sin(a1) * HEX * 0.8)
            rl.rl_end()
        rl.rl_set_line_width(1.0)
        rl.end_blend_mode()

        # fireball emplacements: a looping flame that grows with charge,
        # one orbiting ember per stored charge; dim when disconnected
        tex, count = sheet(flames[flame_ptr[0]])
        rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
        for tile, fb in fireballs.items():
            charge = fb["charge"]
            x, z = axial_to_world(*tile)
            connected = tapped(tile, live)
            size = params["flame_size"][0] * (0.5 + 0.45 * charge / max(need - 1, 1)) \
                 * (1.0 + 0.18 * (fb["level"] - 1))
            fi = int(t * params["fps"][0]) % count
            alpha = 255 if connected else 110
            draw_fx_quad(tex, fi, count, (x, TILE_H + size * 0.42, z), size, cam, True, (0.0, 0.0), alpha)
            for k in range(fb["level"]):  # gold level pips stacked above
                rl.draw_sphere(rl.Vector3(x, TILE_H + size * 0.95 + 0.14 * k, z), 0.045,
                               rl.Color(255, 210, 90, 235))
            for k in range(charge):
                ang = t * 1.8 + k * (6.2831 / max(charge, 1))
                ex = x + math.cos(ang) * 0.55
                ez = z + math.sin(ang) * 0.55
                ey = TILE_H + 0.32 + math.sin(t * 2.6 + k) * 0.06
                rl.draw_sphere(rl.Vector3(ex, ey, ez), 0.07, rl.Color(255, 230, 150, 230))
                rl.draw_sphere(rl.Vector3(ex, ey, ez), 0.13, rl.Color(255, 120, 40, 80))
        rl.end_blend_mode()

        # sparks + detonations
        rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
        for pcl in sparks:
            k = pcl["life"] / pcl["max"]
            rl.draw_sphere(rl.Vector3(*pcl["pos"]), 0.09 * (1.0 - k * 0.7),
                           rl.Color(255, int(210 - 150 * k), int(110 - 90 * k), int(220 * (1.0 - k))))
        btex, bcount = sheet(booms[boom_ptr[0]])
        for e in explosions:
            it = (t - e["t0"]) / max(params["impact_dur"][0], 0.1)
            fi = min(int(it * bcount), bcount - 1)
            draw_fx_quad(btex, fi, bcount, (e["pos"][0], TILE_H + 0.35, e["pos"][1]),
                         params["impact_size"][0], cam, True, (0.0, 0.0), 255)
            ring = params["impact_size"][0] * 0.55 * it
            rl.draw_cylinder(rl.Vector3(e["pos"][0], TILE_H + 0.02, e["pos"][1]), ring, ring, 0.02, 24,
                             rl.Color(255, 160, 70, int(160 * (1.0 - it))))
        rl.end_blend_mode()

        rl.rl_draw_render_batch_active()
        rl.rl_enable_depth_mask()
        rl.end_mode_3d()

        # the hand card: orange frame, animated flame face, "fire" label.
        # While grabbed it rides the cursor and ghosts when it can drop.
        # No card in hand = a dim empty slot until the husk turn passes
        if hand_count <= 0 and not card_grabbed:
            slot = rl.Rectangle(card_home[0], card_home[1], CARD_W, CARD_H)
            rl.draw_rectangle_rounded_lines_ex(slot, 0.14, 6, 2.0, rl.Color(120, 90, 70, 130))
            rl.draw_text("on field", int(card_home[0] + 14), int(card_home[1] + CARD_H / 2 - 10),
                         17, rl.Color(150, 120, 100, 160))
        card_alpha = 160 if (card_grabbed and can_drop) else 255
        if hand_count <= 0 and not card_grabbed:
            card_alpha = 0  # nothing to draw in hand
        cr = rl.Rectangle(card_pos[0], card_pos[1], CARD_W, CARD_H)
        if card_alpha > 0:
            rl.draw_rectangle_rounded(cr, 0.14, 6, rl.Color(34, 24, 22, card_alpha))
        if card_alpha > 0:
            rl.draw_rectangle_rounded_lines_ex(cr, 0.14, 6, 3.0, rl.Color(255, 130, 45, card_alpha))
            ftex, fcount = sheet(flames[flame_ptr[0]])
            ffi_frame = int(t * params["fps"][0]) % fcount
            rl.draw_texture_pro(ftex,
                                rl.Rectangle(ffi_frame * 192, 0, 192, 192),
                                rl.Rectangle(card_pos[0] + 10, card_pos[1] + 12, CARD_W - 20, CARD_W - 20),
                                rl.Vector2(0, 0), 0.0, rl.Color(255, 255, 255, card_alpha))
            label_w = rl.measure_text("fire", 22)
            rl.draw_text("fire", int(card_pos[0] + (CARD_W - label_w) / 2), int(card_pos[1] + CARD_H - 32),
                         22, rl.Color(255, 170, 90, card_alpha))
            for k in range(int(params["level"][0])):  # level pips
                rl.draw_circle(int(card_pos[0] + 16 + k * 14), int(card_pos[1] + 12), 4.0,
                               rl.Color(255, 200, 90, card_alpha))

        # hover tooltip: what the card does, with the live charge number
        if not card_grabbed and card_alpha > 0 and rl.check_collision_point_rec(mouse, cr):
            lvl = hand_level
            if lvl >= 3:
                tip_lines = [
                    "Drag onto a tile to place.",
                    "No cast time: detonates the",
                    "instant a leyline chain from the",
                    "player reaches it, hitting",
                    "everything within 1 tile.",
                    "Returns to hand immediately.",
                ]
            elif lvl == 2:
                tip_lines = [
                    "Drag onto a tile to place.",
                    "Charges +1 per lever pull while",
                    "leyline-connected to the player.",
                    f"At {need} charge{'s' if need != 1 else ''} it detonates, hitting",
                    "everything within 1 tile.",
                    "Returns to hand immediately.",
                ]
            else:
                tip_lines = [
                    "Drag onto a tile to place.",
                    "Charges +1 per lever pull while",
                    "leyline-connected to the player.",
                    f"At {need} charge{'s' if need != 1 else ''} it detonates, hitting",
                    "everything within 1 tile.",
                    "The spent husk blocks its tile for",
                    "one turn before returning to hand.",
                ]
            if lvl < 3:
                tip_lines.append(f"Drop onto a lvl {lvl} fireball to")
                tip_lines.append(f"merge them into one lvl {lvl + 1}.")
            tip_title = f"FIREBALL  lvl {lvl}"
            tw = max(rl.measure_text(line, 17) for line in tip_lines) + 24
            th = 34 + len(tip_lines) * 20 + 10
            tx = int(card_pos[0])
            ty = int(card_pos[1] - th - 10)
            tr = rl.Rectangle(tx, ty, tw, th)
            rl.draw_rectangle_rounded(tr, 0.12, 6, rl.Color(24, 18, 20, 240))
            rl.draw_rectangle_rounded_lines_ex(tr, 0.12, 6, 2.0, rl.Color(255, 130, 45, 255))
            rl.draw_text(tip_title, tx + 12, ty + 9, 19, rl.Color(255, 170, 90, 255))
            for li, line in enumerate(tip_lines):
                rl.draw_text(line, tx + 12, ty + 34 + li * 20, 17, rl.Color(215, 205, 200, 255))

        # panel
        rl.gui_window_box(panel, "fireball charge")
        px, py = int(panel.x + 10), int(panel.y + 30)
        charging = sum(1 for tile in fireballs if tapped(tile, live))
        rl.gui_label(rl.Rectangle(px, py, 234, 14),
                     f"turn {turn}  hand {hand_count}  charging {charging}/{len(fireballs)}"
                     f"  dead {len(dead_husks)}  enemies {len(enemies)}")
        if rl.gui_button(rl.Rectangle(px, py + 18, 234, 30), "PULL LEVER  (space)"):
            pull_lever(t)

        rl.gui_label(rl.Rectangle(px, py + 58, 234, 12), "flame sheet")
        rl.gui_list_view_ex(rl.Rectangle(px, py + 72, 234, 84), flame_arr, len(flame_bufs),
                            fscroll, flame_ptr, ffocus)
        rl.gui_label(rl.Rectangle(px, py + 162, 234, 12), "impact sheet")
        rl.gui_list_view_ex(rl.Rectangle(px, py + 176, 234, 84), boom_arr, len(boom_bufs),
                            bscroll, boom_ptr, bfocus)

        def slider(i, label, key, lo, hi, fmt="{:.2f}"):
            y = py + 272 + i * 28
            rl.gui_label(rl.Rectangle(px, y, 234, 12), f"{label}  {fmt.format(params[key][0])}")
            rl.gui_slider_bar(rl.Rectangle(px, y + 12, 234, 12), "", "", params[key], lo, hi)

        slider(0, "flame size", "flame_size", 0.3, 2.0)
        slider(1, "flame fps", "fps", 4.0, 30.0, "{:.0f}")
        slider(2, "impact size", "impact_size", 0.8, 6.0)
        slider(3, "impact duration", "impact_dur", 0.15, 1.5)
        slider(4, "charge needed", "charge_needed", 1.0, 4.0, "{:.0f}")
        slider(5, "hand card level", "level", 1.0, 3.0, "{:.0f}")

        if rl.gui_button(rl.Rectangle(px, py + 272 + 6 * 28, 234, 24), "save look"):
            save_look()

        rl.draw_text("drag the card to place a fireball   click cycles: leyline > enemy > off",
                     12, SCREEN_H - 24, 18, rl.GRAY)
        rl.end_drawing()

        if shot_mode and frame in (40, 100):
            pull_lever(t)
        if shot_mode and frame == 70:
            rl.take_screenshot("fireball_charging_shot.png")
        if rl.is_key_pressed(rl.KeyboardKey.KEY_S):
            rl.take_screenshot("fireball_charging_shot.png")
        if shot_mode and frame == 112:
            rl.take_screenshot("fireball_boom_shot.png")
            break

    rl.close_window()


if __name__ == "__main__":
    main()
