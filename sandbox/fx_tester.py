#!/usr/bin/env python3
"""3D fx composer: place, tilt, and play the NopiA effect animations.

    python3 fx_tester.py          # interactive
    python3 fx_tester.py --shot   # bake impact_C, compose, play, screenshot, exit

Resources come from the NopiA footage folders (frame-sequence PNGs) of every
pack in PACK_DIRS (vol.1 + light pack). Picking an animation in the panel
bakes it into assets/fx/<name>_<N>x192.png on first use (union-cropped so
frames don't jitter).

You are composing an effect: left-click the ground to place an animation as
an editable object (it idles as a ghosted preview frame). Every placement has
three drag boxes -- the orange one moves it along the ground, the green one
beside it lifts it up/down, and the cyan one above it tilts it (drag sideways
= yaw, up/down = pitch). The play button fires the whole composition.

Controls:
    left click       place selected animation / drag a handle box
    right drag       orbit          wheel   zoom
    x                clear all
    s                screenshot to fx_tester_shot.png
"""

import math
import sys
from pathlib import Path

import cv2
import numpy as np
import pyray as rl
from raylib import ffi

PACK_DIRS = [
    Path("/home/jpleona/Documents/itch/NopiA_2dfxPack_vol.1/footage"),
    Path("/home/jpleona/Documents/itch/NopiA_light_2dfxPack/footage"),
]
FOLDERS = {p.name: p for pack in PACK_DIRS if pack.is_dir()
           for p in sorted(pack.iterdir()) if p.is_dir()}
FX_DIR = Path(__file__).parent / "assets" / "fx"
CELL = 192
SCREEN_W, SCREEN_H = 900, 620


def bake_fx(name):
    """Bake one footage folder into a row sheet (cached on disk). Returns
    (path, frame_count) or None if the folder has no frames."""
    frames = sorted(FOLDERS[name].glob("*.png"))
    if not frames:
        return None
    out = FX_DIR / f"{name}_{len(frames)}x{CELL}.png"
    if out.exists():
        return out, len(frames)

    imgs = [cv2.imread(str(f), cv2.IMREAD_UNCHANGED) for f in frames]
    mask = np.zeros(imgs[0].shape[:2], bool)
    for im in imgs:
        mask |= im[..., 3] > 8
    if not mask.any():
        return None
    ys, xs = np.nonzero(mask)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    h, w = y1 - y0, x1 - x0
    side = max(h, w)

    sheet = np.zeros((CELL, CELL * len(imgs), 4), np.uint8)
    for i, im in enumerate(imgs):
        crop = im[y0:y1, x0:x1]
        pad = np.zeros((side, side, 4), np.uint8)
        pad[(side - h) // 2:(side - h) // 2 + h, (side - w) // 2:(side - w) // 2 + w] = crop
        sheet[:, i * CELL:(i + 1) * CELL] = cv2.resize(pad, (CELL, CELL), interpolation=cv2.INTER_AREA)

    FX_DIR.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out), sheet)
    return out, len(imgs)


RL_QUADS = 0x0007


def draw_fx_quad(texture, frame, frames, pos, size, cam, face_camera, rot, alpha):
    """One animation frame on a quad. Base pose faces the camera or lies flat
    on the ground; the instance's tilt (pitch, yaw) stacks on top."""
    u0 = frame / frames
    u1 = (frame + 1) / frames

    rl.rl_push_matrix()
    rl.rl_translatef(pos[0], pos[1], pos[2])
    if face_camera:
        ox = cam.position.x - pos[0]
        oy = cam.position.y - pos[1]
        oz = cam.position.z - pos[2]
        rl.rl_rotatef(math.degrees(math.atan2(ox, oz)), 0.0, 1.0, 0.0)
        rl.rl_rotatef(-math.degrees(math.atan2(oy, math.hypot(ox, oz))), 1.0, 0.0, 0.0)
    else:
        rl.rl_rotatef(-90.0, 1.0, 0.0, 0.0)
    rl.rl_rotatef(rot[1], 0.0, 1.0, 0.0)  # user yaw
    rl.rl_rotatef(rot[0], 1.0, 0.0, 0.0)  # user pitch

    hw = size / 2.0
    rl.rl_set_texture(texture.id)
    rl.rl_begin(RL_QUADS)
    rl.rl_color4ub(255, 255, 255, alpha)
    rl.rl_normal3f(0.0, 0.0, 1.0)
    rl.rl_tex_coord2f(u0, 0.0)
    rl.rl_vertex3f(-hw, hw, 0.0)
    rl.rl_tex_coord2f(u0, 1.0)
    rl.rl_vertex3f(-hw, -hw, 0.0)
    rl.rl_tex_coord2f(u1, 1.0)
    rl.rl_vertex3f(hw, -hw, 0.0)
    rl.rl_tex_coord2f(u1, 0.0)
    rl.rl_vertex3f(hw, hw, 0.0)
    rl.rl_end()
    rl.rl_set_texture(0)
    rl.rl_pop_matrix()


def main():
    shot_mode = "--shot" in sys.argv

    rl.init_window(SCREEN_W, SCREEN_H, "fx composer")
    rl.set_target_fps(60)
    rl.rl_disable_backface_culling()

    cam = rl.Camera3D(rl.Vector3(4.5, 4.0, 7.0), rl.Vector3(0.0, 0.8, 0.0),
                      rl.Vector3(0.0, 1.0, 0.0), 45.0,
                      rl.CameraProjection.CAMERA_PERSPECTIVE)

    names = sorted(FOLDERS)
    loaded = {}  # name -> (texture, frame_count)

    def ensure_loaded(name):
        if name in loaded:
            return True
        baked = bake_fx(name)
        if baked is None:
            return False
        tex = rl.load_texture(str(baked[0]))
        rl.set_texture_filter(tex, rl.TextureFilter.TEXTURE_FILTER_BILINEAR)
        loaded[name] = (tex, baked[1])
        return True

    instances = []  # {name, pos [x,y,z], rot [pitch,yaw]}
    drag = None     # (instance index, "move" | "tilt")
    play_start = -1e9

    panel = rl.Rectangle(636, 12, 254, 596)
    file_ptr = ffi.new("int *", names.index("impact_C") if "impact_C" in names else 0)
    scroll_ptr = ffi.new("int *", max(0, file_ptr[0] - 4))
    scale_ptr = ffi.new("float *", 2.0)
    dur_ptr = ffi.new("float *", 0.6)
    height_ptr = ffi.new("float *", 1.0)
    face_ptr = ffi.new("bool *", True)
    additive_ptr = ffi.new("bool *", False)
    loop_ptr = ffi.new("bool *", False)
    lightbg_ptr = ffi.new("bool *", False)
    # gui_list_view truncates at 1024 chars / 128 items; _ex takes an array
    name_bufs = [ffi.new("char[]", n.encode()) for n in names]
    name_arr = ffi.new("char *[]", name_bufs)
    focus_ptr = ffi.new("int *", -1)
    status = f"{len(names)} animations, {len(PACK_DIRS)} packs"
    frame = 0

    def tilt_handle_pos(inst):
        return (inst["pos"][0], inst["pos"][1] + scale_ptr[0] * 0.55 + 0.2, inst["pos"][2])

    def lift_handle_pos(inst, cam):
        # a green box off to the camera-right of the instance: drags up/down
        fx = cam.target.x - cam.position.x
        fz = cam.target.z - cam.position.z
        flen = math.hypot(fx, fz) or 1.0
        rx, rz = fz / flen, -fx / flen
        return (inst["pos"][0] + rx * 0.45, inst["pos"][1], inst["pos"][2] + rz * 0.45)

    while not rl.window_should_close():
        frame += 1
        t = rl.get_time()
        mouse = rl.get_mouse_position()
        ui_mouse = rl.check_collision_point_rec(mouse, panel)
        selected = names[file_ptr[0]] if 0 <= file_ptr[0] < len(names) else None

        # orbit + zoom (right button / wheel, so they never fight the handles)
        if rl.is_mouse_button_down(rl.MouseButton.MOUSE_BUTTON_RIGHT) and not ui_mouse:
            d = rl.get_mouse_delta()
            off = rl.vector3_subtract(cam.position, cam.target)
            radius = rl.vector3_length(off)
            yaw = math.atan2(off.x, off.z) - d.x * 0.005
            pitch = math.asin(off.y / radius) - d.y * 0.005
            pitch = min(max(pitch, 0.1), 1.45)
            cam.position = rl.Vector3(cam.target.x + radius * math.cos(pitch) * math.sin(yaw),
                                      cam.target.y + radius * math.sin(pitch),
                                      cam.target.z + radius * math.cos(pitch) * math.cos(yaw))
        wheel = rl.get_mouse_wheel_move()
        if wheel != 0.0 and not ui_mouse:
            off = rl.vector3_subtract(cam.position, cam.target)
            cam.position = rl.vector3_add(cam.target, rl.vector3_scale(off, 1.0 - wheel * 0.08))

        # handle picking: press grabs the nearest handle box under the cursor
        hover_handle = None
        for i, inst in enumerate(instances):
            for kind, hp in (("move", inst["pos"]), ("tilt", tilt_handle_pos(inst)),
                             ("lift", lift_handle_pos(inst, cam))):
                sp = rl.get_world_to_screen(rl.Vector3(hp[0], hp[1], hp[2]), cam)
                if abs(mouse.x - sp.x) < 14 and abs(mouse.y - sp.y) < 14:
                    hover_handle = (i, kind)

        if rl.is_mouse_button_pressed(rl.MouseButton.MOUSE_BUTTON_LEFT) and not ui_mouse:
            if hover_handle is not None:
                drag = hover_handle
            elif selected is not None:
                # place a new instance on the ground under the cursor
                ray = rl.get_screen_to_world_ray(mouse, cam)
                if abs(ray.direction.y) > 0.0001:
                    hit_t = -ray.position.y / ray.direction.y
                    if hit_t > 0 and ensure_loaded(selected):
                        instances.append({
                            "name": selected,
                            "pos": [ray.position.x + ray.direction.x * hit_t,
                                    height_ptr[0],
                                    ray.position.z + ray.direction.z * hit_t],
                            "rot": [0.0, 0.0],
                        })
                        status = f"{selected}: {loaded[selected][1]} frames"
        if not rl.is_mouse_button_down(rl.MouseButton.MOUSE_BUTTON_LEFT):
            drag = None

        # dragging: orange box slides on the ground, green box lifts, cyan tilts
        if drag is not None and drag[0] < len(instances):
            inst = instances[drag[0]]
            if drag[1] == "move":
                ray = rl.get_screen_to_world_ray(mouse, cam)
                if abs(ray.direction.y) > 0.0001:
                    hit_t = (inst["pos"][1] - ray.position.y) / ray.direction.y
                    if hit_t > 0:
                        inst["pos"][0] = ray.position.x + ray.direction.x * hit_t
                        inst["pos"][2] = ray.position.z + ray.direction.z * hit_t
            elif drag[1] == "lift":
                # intersect the mouse ray with the vertical plane through the
                # instance that faces the camera, then track its height
                ray = rl.get_screen_to_world_ray(mouse, cam)
                nx = inst["pos"][0] - cam.position.x
                nz = inst["pos"][2] - cam.position.z
                denom = ray.direction.x * nx + ray.direction.z * nz
                if abs(denom) > 0.0001:
                    hit_t = ((inst["pos"][0] - ray.position.x) * nx +
                             (inst["pos"][2] - ray.position.z) * nz) / denom
                    if hit_t > 0:
                        inst["pos"][1] = max(0.0, ray.position.y + ray.direction.y * hit_t)
            else:
                d = rl.get_mouse_delta()
                inst["rot"][1] += d.x * 0.5  # yaw
                inst["rot"][0] += d.y * 0.5  # pitch

        if rl.is_key_pressed(rl.KeyboardKey.KEY_X):
            instances.clear()

        if shot_mode and frame == 5 and ensure_loaded("impact_C"):
            instances.append({"name": "impact_C", "pos": [0.0, 1.0, 0.0], "rot": [0.0, 0.0]})
            instances.append({"name": "impact_C", "pos": [2.2, 1.0, -1.2], "rot": [25.0, 40.0]})
        if shot_mode and frame == 30:
            play_start = t

        rl.begin_drawing()
        rl.clear_background(rl.Color(225, 222, 215, 255) if lightbg_ptr[0] else rl.Color(24, 24, 32, 255))

        rl.begin_mode_3d(cam)
        rl.draw_grid(16, 1.0)
        rl.draw_cube(rl.Vector3(-3.0, 0.5, -3.0), 1.0, 1.0, 1.0, rl.Color(90, 90, 110, 255))
        rl.draw_cube(rl.Vector3(3.5, 0.35, -2.0), 0.7, 0.7, 0.7, rl.Color(110, 90, 90, 255))

        # instances: ghosted preview frame while idle, full playback when the
        # composition is playing
        play_t = (t - play_start) / max(dur_ptr[0], 0.05)
        playing = (0.0 <= play_t < 1.0) or (loop_ptr[0] and play_t >= 0.0)
        if additive_ptr[0]:
            rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
        for inst in instances:
            if not ensure_loaded(inst["name"]):
                continue
            tex, count = loaded[inst["name"]]
            if playing:
                p = play_t % 1.0 if loop_ptr[0] else play_t
                fx_frame = min(int(p * count), count - 1)
                alpha = 255
            else:
                fx_frame = count // 3  # a representative frame as placement ghost
                alpha = 90
            draw_fx_quad(tex, fx_frame, count, inst["pos"], scale_ptr[0], cam,
                         face_ptr[0], inst["rot"], alpha)
        if additive_ptr[0]:
            rl.end_blend_mode()

        # drag handles: orange moves, green lifts, cyan tilts
        for i, inst in enumerate(instances):
            hot = hover_handle is not None and hover_handle[0] == i
            mc = rl.ORANGE if (hot and hover_handle[1] == "move") else rl.Color(200, 120, 30, 255)
            lc = rl.GREEN if (hot and hover_handle[1] == "lift") else rl.Color(60, 160, 70, 255)
            tc = rl.SKYBLUE if (hot and hover_handle[1] == "tilt") else rl.Color(40, 140, 180, 255)
            rl.draw_cube(rl.Vector3(*inst["pos"]), 0.14, 0.14, 0.14, mc)
            th = tilt_handle_pos(inst)
            lh = lift_handle_pos(inst, cam)
            rl.draw_cube(rl.Vector3(*th), 0.11, 0.11, 0.11, tc)
            rl.draw_cube(rl.Vector3(*lh), 0.11, 0.11, 0.11, lc)
            rl.draw_line_3d(rl.Vector3(*inst["pos"]), rl.Vector3(*th), rl.Color(120, 120, 130, 160))
            rl.draw_line_3d(rl.Vector3(*inst["pos"]), rl.Vector3(*lh), rl.Color(120, 120, 130, 160))
        rl.end_mode_3d()

        # panel
        rl.gui_window_box(panel, "fx composer")
        px, py = int(panel.x + 10), int(panel.y + 30)
        rl.gui_label(rl.Rectangle(px, py, 234, 14), status)
        rl.gui_list_view_ex(rl.Rectangle(px, py + 18, 234, 280), name_arr, len(name_bufs), scroll_ptr, file_ptr, focus_ptr)

        if rl.gui_button(rl.Rectangle(px, py + 306, 234, 26), "play animation"):
            play_start = t

        rl.gui_label(rl.Rectangle(px, py + 340, 234, 14), f"scale  {scale_ptr[0]:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 354, 234, 14), "", "", scale_ptr, 0.3, 8.0)
        rl.gui_label(rl.Rectangle(px, py + 374, 234, 14), f"duration  {dur_ptr[0]:.2f}s")
        rl.gui_slider_bar(rl.Rectangle(px, py + 388, 234, 14), "", "", dur_ptr, 0.1, 2.5)
        rl.gui_label(rl.Rectangle(px, py + 408, 234, 14), f"spawn height  {height_ptr[0]:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 422, 234, 14), "", "", height_ptr, 0.0, 4.0)
        rl.gui_check_box(rl.Rectangle(px, py + 446, 14, 14), "face camera", face_ptr)
        rl.gui_check_box(rl.Rectangle(px, py + 466, 14, 14), "additive (light fx)", additive_ptr)
        rl.gui_check_box(rl.Rectangle(px, py + 486, 14, 14), "loop playback", loop_ptr)
        rl.gui_check_box(rl.Rectangle(px + 120, py + 486, 14, 14), "light bg", lightbg_ptr)
        rl.gui_label(rl.Rectangle(px, py + 510, 110, 14), f"placed: {len(instances)}")
        if rl.gui_button(rl.Rectangle(px + 118, py + 506, 116, 22), "clear all"):
            instances.clear()

        rl.draw_text("click: place   orange: move   green: up/down   cyan: tilt", 12, SCREEN_H - 24, 18, rl.GRAY)
        rl.end_drawing()

        if rl.is_key_pressed(rl.KeyboardKey.KEY_S) or (shot_mode and frame == 42):
            rl.take_screenshot("fx_tester_shot.png")
            if shot_mode:
                break

    rl.close_window()


if __name__ == "__main__":
    main()
