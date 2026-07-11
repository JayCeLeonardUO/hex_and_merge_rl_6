#!/usr/bin/env python3
"""Card visual prototyping sandbox for hex merge.

    python3 card_proto.py          # interactive
    python3 card_proto.py --shot   # save card_proto_shot.png after ~40 frames and exit

Everything visual lives in draw_card() and the DESIGN constants below --
tweak, save, rerun. The four cards show the game's palette; hover to lift.

Controls:
    up/down     card scale
    left/right  cycle the icon set
    h           toggle shaders
    s           screenshot to card_proto_shot.png
"""

import math
import re
import sys
from pathlib import Path

import cv2
import numpy as np
import pyray as rl
from raylib import ffi

from export_glb import export_card_glb

ASSETS = Path(__file__).parent / "assets"
GANDALF_DIR = Path("/home/jpleona/Documents/itch/GandalfHardcore Icons")

# ----------------------------------------------------------------------------
# SHADERS
# ----------------------------------------------------------------------------
# Foil shine: a diagonal iridescent band sweeping across the card. Drawn as a
# second pass of the template texture, whose alpha masks the shine to the
# card's actual silhouette (ornate corners stay clean).
SHINE_FS = """
#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform float time;
uniform float phase;
out vec4 finalColor;

void main()
{
    vec4 tex = texture(texture0, fragTexCoord);
    float mask = step(0.01, tex.a);

    // the template's arch is transparent, but it is still card face: extend
    // the mask with the open-interior rectangle so the shine crosses the art
    vec2 uv = fragTexCoord;
    float inWindow = step(4.0/39.0, uv.x)*step(uv.x, 35.0/39.0)
                   * step(11.0/66.0, uv.y)*step(uv.y, 62.0/66.0);
    mask = max(mask, inWindow);

    float sweep = fract(time*0.22 + phase);            // one pass every ~4.5s
    float d = fragTexCoord.x*0.6 + fragTexCoord.y*0.4 - (sweep*2.2 - 0.6);
    float band = smoothstep(0.16, 0.0, abs(d));

    // cheap iridescence: hue drifts across the band
    vec3 rainbow = 0.5 + 0.5*cos(6.2831*(d*2.0 + vec3(0.0, 0.33, 0.67)));
    finalColor = vec4(mix(vec3(1.0), rainbow, 0.6), band*0.4*mask);
}
"""

# Background: slow-moving radial gradient + vignette so cards sit in a scene
BG_FS = """
#version 330
in vec2 fragTexCoord;
uniform float time;
out vec4 finalColor;

void main()
{
    vec2 uv = fragTexCoord;
    vec2 c = vec2(0.5 + 0.08*sin(time*0.3), 0.42 + 0.06*cos(time*0.23));
    vec3 col = mix(vec3(0.97, 0.96, 0.94), vec3(0.83, 0.82, 0.87),
                   smoothstep(0.15, 0.85, distance(uv, c)));
    col *= mix(0.90, 1.0, smoothstep(1.05, 0.55, distance(uv, vec2(0.5))));
    finalColor = vec4(col, 1.0);
}
"""


# Rounded hexagon via SDF: fill, magic-card corner rounding, border ring, and
# the same foil shine as the cards, all in one shader. Drawn on a square quad;
# alpha 0 fill/border + shineStrength > 0 gives a shine-only pass.
HEX_FS = """
#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0; // white 1x1 for flat passes; card art for the art pass
uniform vec4 fillColor;
uniform vec4 borderColor;
uniform vec4 srcRect; // texcoord window of the drawn source (u0, v0, u1, v1)
uniform float time;
uniform float phase;
uniform float shineStrength;
out vec4 finalColor;

float sdHexagon(vec2 p, float r)
{
    const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0*min(dot(k.xy, p), 0.0)*k.xy;
    p -= vec2(clamp(p.x, -k.z*r, k.z*r), r);
    return length(p)*sign(p.y);
}

void main()
{
    // Normalize against the source window so the geometry is always 0..1 over
    // the quad, whether the source is the white 1x1 or a sheet cell
    vec2 uv = (fragTexCoord - srcRect.xy) / (srcRect.zw - srcRect.xy);
    vec2 p = (uv - 0.5)*2.0;
    // axis swap makes it pointy-top like the board; the SDF offset rounds the
    // corners like a magic card
    float sd = sdHexagon(p.yx, 0.80) - 0.14;
    float aa = fwidth(sd)*1.4;

    float inside = 1.0 - smoothstep(-aa, aa, sd);
    float inner = 1.0 - smoothstep(-aa, aa, sd + 0.10); // border ring width
    vec4 col = mix(borderColor, fillColor, inner) * texture(texture0, fragTexCoord);

    float sweep = fract(time*0.22 + phase);
    float d = uv.x*0.6 + uv.y*0.4 - (sweep*2.2 - 0.6);
    float band = smoothstep(0.16, 0.0, abs(d));
    vec3 rainbow = 0.5 + 0.5*cos(6.2831*(d*2.0 + vec3(0.0, 0.33, 0.67)));

    finalColor = vec4(col.rgb + mix(vec3(1.0), rainbow, 0.6)*band*shineStrength,
                      max(col.a*inside, band*shineStrength*inside));
}
"""


def set_shader_float(shader, loc, value):
    rl.set_shader_value(shader, loc, ffi.new("float *", value),
                        rl.ShaderUniformDataType.SHADER_UNIFORM_FLOAT)


def set_shader_vec4(shader, loc, rgba):
    rl.set_shader_value(shader, loc, ffi.new("float[4]", list(rgba)),
                        rl.ShaderUniformDataType.SHADER_UNIFORM_VEC4)


RL_QUADS = 0x0007  # rlgl primitive id


def draw_rt_quad_3d(rt, wx, wy, wz, world_w, world_h, face_deg, tilt):
    """A render texture as a textured quad in the 3D scene (inside
    begin_mode_3d). face_deg poses the quad around the x axis: -90 lies it
    flat on the board, -CAM_ELEV_DEG makes it face the camera dead on. The
    press tilt is a pair of real rotations in the quad's local frame, so the
    hovered spot rocks away like pressing a physical card."""
    rl.rl_push_matrix()
    rl.rl_translatef(wx, wy, wz)
    rl.rl_rotatef(face_deg, 1.0, 0.0, 0.0)              # rest pose
    rl.rl_rotatef(tilt[1] * TILT_DEG, 1.0, 0.0, 0.0)    # press pitch: bottom edge sinks
    rl.rl_rotatef(tilt[0] * TILT_DEG, 0.0, 1.0, 0.0)    # press yaw: right edge sinks

    hw, hh = world_w / 2.0, world_h / 2.0
    rl.rl_set_texture(rt.texture.id)
    rl.rl_begin(RL_QUADS)
    rl.rl_color4ub(255, 255, 255, 255)
    rl.rl_normal3f(0.0, 0.0, 1.0)
    # Local XY plane; the render texture is stored bottom-up so visual top is v=1
    rl.rl_tex_coord2f(0.0, 1.0)
    rl.rl_vertex3f(-hw, hh, 0.0)
    rl.rl_tex_coord2f(0.0, 0.0)
    rl.rl_vertex3f(-hw, -hh, 0.0)
    rl.rl_tex_coord2f(1.0, 0.0)
    rl.rl_vertex3f(hw, -hh, 0.0)
    rl.rl_tex_coord2f(1.0, 1.0)
    rl.rl_vertex3f(hw, hh, 0.0)
    rl.rl_end()
    rl.rl_set_texture(0)
    rl.rl_pop_matrix()


def draw_art_relief_3d(layers, icon_src, wx, wy, wz, world_w, world_h, face_deg, tilt,
                       lx, ly, z0, z_step, hexfx=None, t=0.0, phase=0.0):
    """The segmented art as an extruded relief: one quad per color layer,
    stacked along the element's local normal (floor first, brightest highest).
    Shares the element's pose + press tilt via the same matrix stack. With
    hexfx the layers draw through the hex SDF shader, cropped to the tile."""
    rl.rl_push_matrix()
    rl.rl_translatef(wx, wy, wz)
    rl.rl_rotatef(face_deg, 1.0, 0.0, 0.0)
    rl.rl_rotatef(tilt[1] * TILT_DEG, 1.0, 0.0, 0.0)
    rl.rl_rotatef(tilt[0] * TILT_DEG, 0.0, 1.0, 0.0)
    rl.rl_translatef(lx, ly, 0.0)

    hw, hh = world_w / 2.0, world_h / 2.0
    for j, texture in enumerate(layers):
        u0, v0 = icon_src.x / texture.width, icon_src.y / texture.height
        u1 = (icon_src.x + icon_src.width) / texture.width
        v1 = (icon_src.y + icon_src.height) / texture.height
        z = z0 + j * z_step

        if hexfx is not None:
            sh = hexfx["shader"]
            rl.begin_shader_mode(sh)
            set_shader_vec4(sh, hexfx["fill"], (1, 1, 1, 1))
            set_shader_vec4(sh, hexfx["border"], (1, 1, 1, 1))
            set_shader_vec4(sh, hexfx["src"], (u0, v0, u1, v1))
            set_shader_float(sh, hexfx["time"], t)
            set_shader_float(sh, hexfx["phase"], phase)
            set_shader_float(sh, hexfx["shine"], 0.0)

        rl.rl_set_texture(texture.id)
        rl.rl_begin(RL_QUADS)
        rl.rl_color4ub(255, 255, 255, 255)
        rl.rl_normal3f(0.0, 0.0, 1.0)
        rl.rl_tex_coord2f(u0, v0)
        rl.rl_vertex3f(-hw, hh, z)
        rl.rl_tex_coord2f(u0, v1)
        rl.rl_vertex3f(-hw, -hh, z)
        rl.rl_tex_coord2f(u1, v1)
        rl.rl_vertex3f(hw, -hh, z)
        rl.rl_tex_coord2f(u1, v0)
        rl.rl_vertex3f(hw, hh, z)
        rl.rl_end()
        rl.rl_set_texture(0)

        if hexfx is not None:
            rl.end_shader_mode()

    rl.rl_pop_matrix()

# ----------------------------------------------------------------------------
# DESIGN -- the numbers to argue with
# ----------------------------------------------------------------------------
# Card outline: the Captainskeleto fire card template, drawn whole (no n-patch,
# the ornaments must not stretch). Its arch window is transparent, so the
# backing panel + icon draw first and the frame goes on top.
CARD_W, CARD_H = 39, 66     # template size in source px; card = this * scale
WINDOW = (4, 11, 31, 51)    # x, y, w, h of the template's open interior
CARD_GAP = 40               # space between cards in the lineup
ICON_SCALE = 0.62           # card art size as a fraction of the window width

# Card art: GandalfHardcore 16x16 icons baked into a 10x10 sheet (100 icons)
ICON_SIZE_PX = 16
ICON_COLS = 10
ICON_COUNT = 100

# Parallax: the card is a stack of floating layers. Each value is a layer's
# maximum shift in template px at full tilt -- negative sinks away from the
# cursor, positive floats toward it. Tilt comes from the mouse over the card.
PAR_BACK = -1.1    # backing panel, deepest
PAR_FRAME = 1.2    # the fire outline floats over the art
PAR_TEXT = 2.1     # name + value float over the frame
PAR_BADGE = 3.0    # coin badge, highest

# The art is split live by a brightness threshold (OpenCV): the full print
# stays as the back layer, everything brighter than the threshold raises above
# it as a real 3D quad. Tune with up/down while running.
ART_RAISED_LAYERS = 1    # one raised layer above the full print
ART_Z0 = 0.05            # back layer height above the element face, world units
ART_Z_STEP = 0.14        # how far the raised layer rises off the back
SEG_THRESHOLD = 120.0    # starting brightness threshold, 0..255

# Balatro-style press, done in real 3D: every element is a textured quad in a
# 3D scene; hovering rocks the quad around its local axes with rlgl rotations
TILT_DEG = 17.0  # max press tilt in degrees

# The scene uses the game's board camera: orthographic, tilted down from
# (0, 14, 12). Hex tiles lie flat on the y=0 plane like in-game (civ tiles),
# cards stand upright perpendicular to the camera.
CAM_FOVY = 7.0                                       # ortho view height at 1x (wheel zooms)
CAM_ELEV_DEG = math.degrees(math.atan2(14.0, 12.0))  # camera elevation, ~49.4
CARD_WORLD_H = 2.6          # card quad height in world units
HEX_WORLD_R = 1.05          # hex tile outer radius in world units (game HEX_SIZE ~ 1)
CARD_X, CARD_Z = -2.3, 0.0  # the one card, left of center (panel lives right)
HEX_X, HEX_Z = 0.5, 0.0     # its hex form beside it on the board plane
CARD_Y = 1.15               # card center height above the board plane
HOVER_LIFT = 18             # px a hovered card rises
NAME_SIZE = 4.5             # name text size, template px (scales with the card)
VALUE_SIZE = 9              # merge value text size, template px

# name, value, cost, palette color (the game's card palette)
CARDS = [
    ("strike", 1, 1, (255, 203, 0, 255)),    # GOLD
    ("ward",   2, 1, (255, 109, 194, 255)),  # PINK
    ("grow",   3, 2, (0, 158, 47, 255)),     # LIME
    ("hex",    4, 3, (135, 60, 190, 255)),   # VIOLET
]

SCREEN_W, SCREEN_H = 900, 620


def load(name):
    tex = rl.load_texture(str(ASSETS / name))
    rl.set_texture_filter(tex, rl.TextureFilter.TEXTURE_FILTER_POINT)
    return tex


def make_texture_rgba(rgba):
    """Create a point-filtered raylib texture from an RGBA numpy array."""
    img = rl.Image(ffi.from_buffer(rgba), rgba.shape[1], rgba.shape[0], 1,
                   rl.PixelFormat.PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
    texture = rl.load_texture_from_image(img)
    rl.set_texture_filter(texture, rl.TextureFilter.TEXTURE_FILTER_POINT)
    return texture


def draw_card(tex, x, y, scale, name, value, cost, color, icon_index, tilt=(0.0, 0.0)):
    """One card, centered at (x, y). Layout is in template px * scale.
    tilt is (-1..1, -1..1) from the mouse; each layer shifts by its parallax
    depth so the sections float apart."""
    s = scale
    w, h = CARD_W * s, CARD_H * s
    left, top = x - w / 2, y - h / 2

    def off(depth):
        return tilt[0] * depth * s, tilt[1] * depth * s

    # Drop shadow (fixed: the light doesn't move)
    rl.draw_rectangle(int(left + 2 * s), int(top + 2.5 * s), int(w), int(h), (0, 0, 0, 40))

    # Backing panel behind the template's transparent interior: dark parchment
    # washed with the card's color so the palette still reads. Deepest layer.
    bx, by = off(PAR_BACK)
    win = rl.Rectangle(left + WINDOW[0] * s + bx, top + WINDOW[1] * s + by, WINDOW[2] * s, WINDOW[3] * s)
    rl.draw_rectangle(int(win.x - s), int(win.y - s), int(win.width + 2 * s), int(win.height + 2 * s),
                      rl.Color(52, 44, 52, 255))
    rl.draw_rectangle(int(win.x), int(win.y), int(win.width), int(win.height),
                      rl.Color(color[0], color[1], color[2], 55))

    # (the card art is a 3D relief now, drawn in the scene pass; the name and
    # value draw as projected overlays after the 3D pass, so the relief cannot
    # cover them)

    # The fire card outline floats over the art window; its transparent arch
    # is where the relief shows through
    fx, fy = off(PAR_FRAME)
    rl.draw_texture_pro(tex["outline"], rl.Rectangle(0, 0, CARD_W, CARD_H),
                        rl.Rectangle(left + fx, top + fy, w, h), rl.Vector2(0, 0), 0.0, rl.WHITE)

    # Cost badge: coin + number, top-left corner, highest layer
    gx, gy = off(PAR_BADGE)
    coin = 10 * s
    coin_x, coin_y = left - coin * 0.3 + gx, top - coin * 0.3 + gy
    rl.draw_texture_pro(tex["coin"], rl.Rectangle(0, 0, 16, 18),
                        rl.Rectangle(coin_x, coin_y, coin, coin * 18 / 16),
                        rl.Vector2(0, 0), 0.0, rl.WHITE)
    cost_px = max(int(4.5 * s), 10)
    rl.draw_text(str(cost), int(coin_x + coin / 2 - rl.measure_text(str(cost), cost_px) / 2),
                 int(coin_y + coin * 0.56 - cost_px / 2), cost_px, rl.Color(60, 45, 35, 255))


def draw_hex_form(tex, x, y, scale, value, color, icon_index, tilt, hexfx, t, phase, shaders_on):
    """The card's board form, given the same love as the card: rounded-corner
    hex (SDF shader) with a sunk backing, floating art, floating border ring,
    foil shine, and the value riding on top."""
    radius = CARD_W * scale * 0.52
    side = radius * 2.15  # SDF quad side: hex outer radius ~= 0.94 * side/2

    def off(depth):
        return tilt[0] * depth * scale, tilt[1] * depth * scale

    icon_index %= ICON_COUNT
    col_i, row_i = icon_index % ICON_COLS, icon_index // ICON_COLS
    icon_src = rl.Rectangle(col_i * ICON_SIZE_PX, row_i * ICON_SIZE_PX, ICON_SIZE_PX, ICON_SIZE_PX)
    icon_size = radius * 1.05  # inscribed square, stays inside the hex edges

    if shaders_on:
        def hex_quad(cx, cy, fill, border, shine_strength, texture=None, src=None):
            sh = hexfx["shader"]
            texture = texture if texture else hexfx["white"]
            src = src if src else rl.Rectangle(0, 0, texture.width, texture.height)
            rl.begin_shader_mode(sh)
            set_shader_vec4(sh, hexfx["fill"], fill)
            set_shader_vec4(sh, hexfx["border"], border)
            set_shader_vec4(sh, hexfx["src"], (src.x / texture.width, src.y / texture.height,
                                               (src.x + src.width) / texture.width,
                                               (src.y + src.height) / texture.height))
            set_shader_float(sh, hexfx["time"], t)
            set_shader_float(sh, hexfx["phase"], phase)
            set_shader_float(sh, hexfx["shine"], shine_strength)
            rl.draw_texture_pro(texture, src,
                                rl.Rectangle(cx - side / 2, cy - side / 2, side, side),
                                rl.Vector2(0, 0), 0.0, rl.WHITE)
            rl.end_shader_mode()

        # Drop shadow (fixed: the light doesn't move)
        hex_quad(x + 2 * scale, y + 2.5 * scale, (0, 0, 0, 0.16), (0, 0, 0, 0.16), 0.0)

        # Base: dark parchment washed with the card color, deepest layer
        bx, by = off(PAR_BACK)
        a = 55.0 / 255.0
        wash = ((52 * (1 - a) + color[0] * a) / 255.0,
                (44 * (1 - a) + color[1] * a) / 255.0,
                (52 * (1 - a) + color[2] * a) / 255.0, 1.0)
        hex_quad(x + bx, y + by, wash, wash, 0.0)

        # (the art is a 3D relief now, drawn above this tile in the scene pass)

        # Border ring floats over the art, rounded like a magic card
        fx_, fy_ = off(PAR_FRAME)
        border_col = (color[0] / 255.0, color[1] / 255.0, color[2] / 255.0, 1.0)
        hex_quad(x + fx_, y + fy_, (0, 0, 0, 0), border_col, 0.0)

        # Foil shine over everything, riding the border layer
        rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
        hex_quad(x + fx_, y + fy_, (0, 0, 0, 0), (0, 0, 0, 0), 0.35)
        rl.end_blend_mode()
    else:
        # Plain flat fallback so the h-toggle shows a true before/after
        rl.draw_poly(rl.Vector2(x, y), 6, radius, 90, rl.Color(52, 44, 52, 255))
        rl.draw_texture_pro(tex["icons"], icon_src,
                            rl.Rectangle(x - icon_size / 2, y - icon_size / 2, icon_size, icon_size),
                            rl.Vector2(0, 0), 0.0, rl.WHITE)
        rl.draw_poly_lines_ex(rl.Vector2(x, y), 6, radius, 90, max(2.0, scale), color)
    # (the value draws as a projected overlay after the 3D pass, floating
    # above the art relief)


def main():
    global ART_Z0, ART_Z_STEP, TILT_DEG  # panel sliders tune these live
    shot_mode = "--shot" in sys.argv

    rl.init_window(SCREEN_W, SCREEN_H, "card proto")
    rl.set_target_fps(60)
    rl.rl_disable_backface_culling()  # press tilts can expose a quad's backside

    # The game's board camera: tilted orthographic, like raylib_game.c
    cam = rl.Camera3D(rl.Vector3(0.0, 14.0, 12.0), rl.Vector3(0.0, 0.0, 0.0),
                      rl.Vector3(0.0, 1.0, 0.0), CAM_FOVY,
                      rl.CameraProjection.CAMERA_ORTHOGRAPHIC)

    tex = {
        "outline": load("fire_card_39x66.png"),
        "icons": load("gandalf_icons_16x16.png"),
        "coin": load("CoinIcon_16x18.png"),
        "impact_fx": load("impact_c_8x192.png"),
    }
    rl.set_texture_filter(tex["impact_fx"], rl.TextureFilter.TEXTURE_FILTER_BILINEAR)
    FX_FRAMES = 8
    FX_DUR = 0.5
    fx_start = -10.0  # trigger animation clock; clicking the hex card restarts it

    # Live brightness segmentation (OpenCV): the raised layer is re-cut from
    # the current art source whenever the threshold moves, straight into the
    # GPU texture. Any PNG in assets/ can be the source (picked in the panel):
    # 16x16-grid images act as sheets, anything else as a single frame.
    seg_threshold = SEG_THRESHOLD
    raised_count = 0
    art = {"back": None, "raised": None, "rgba": None, "gray": None,
           "cols": 1, "count": 1, "cw": 16, "ch": 16}

    def cut_raised():
        nonlocal raised_count
        out = art["rgba"].copy()
        mask = (art["gray"] >= seg_threshold) & (out[..., 3] > 0)
        raised_count = int(mask.sum())
        out[..., 3] = np.where(mask, out[..., 3], 0)
        return out

    def load_art_source(path):
        img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        if img is None:
            return
        if img.shape[-1] == 3:
            img = cv2.cvtColor(img, cv2.COLOR_BGR2BGRA)
        rgba = np.ascontiguousarray(cv2.cvtColor(img, cv2.COLOR_BGRA2RGBA))
        ih, iw = rgba.shape[:2]
        grid = (iw % ICON_SIZE_PX == 0) and (ih % ICON_SIZE_PX == 0) and (iw * ih > ICON_SIZE_PX ** 2)
        cw, ch = (ICON_SIZE_PX, ICON_SIZE_PX) if grid else (iw, ih)
        if art["back"] is not None:
            rl.unload_texture(art["back"])
            rl.unload_texture(art["raised"])
        art.update(rgba=rgba, gray=cv2.cvtColor(img[..., :3], cv2.COLOR_BGR2GRAY),
                   cw=cw, ch=ch, cols=iw // cw, count=(iw // cw) * (ih // ch))
        art["back"] = make_texture_rgba(rgba)
        art["raised"] = make_texture_rgba(cut_raised())

    # Same live treatment for the frame: the template's bright ornaments are
    # cut by their own threshold and float above the card face
    frame_bgra = cv2.imread(str(ASSETS / "fire_card_39x66.png"), cv2.IMREAD_UNCHANGED)
    frame_rgba = np.ascontiguousarray(cv2.cvtColor(frame_bgra, cv2.COLOR_BGRA2RGBA))
    frame_gray = cv2.cvtColor(frame_bgra[..., :3], cv2.COLOR_BGR2GRAY)
    frame_threshold = 150.0
    frame_raised_count = 0

    def cut_frame_raised():
        nonlocal frame_raised_count
        out = frame_rgba.copy()
        mask = (frame_gray >= frame_threshold) & (out[..., 3] > 0)
        frame_raised_count = int(mask.sum())
        out[..., 3] = np.where(mask, out[..., 3], 0)
        return out

    frame_raised_buf = cut_frame_raised()
    frame_raised_tex = make_texture_rgba(frame_raised_buf)

    def natural_key(p):  # Icon2 before Icon10
        return [int(s) if s.isdigit() else s.lower() for s in re.split(r"(\d+)", p.name)]

    art_files = sorted(ASSETS.glob("*.png"), key=natural_key)
    art_names = [p.name for p in art_files]
    if GANDALF_DIR.is_dir():
        gandalf = sorted(GANDALF_DIR.glob("*.png"), key=natural_key)
        art_files += gandalf
        art_names += [f"GH: {p.name}" for p in gandalf]
    file_idx = art_files.index(ASSETS / "gandalf_icons_16x16.png")
    load_art_source(art_files[file_idx])

    # Panel state: raygui works on ffi pointers that persist across frames
    panel = rl.Rectangle(652, 36, 236, 560)
    thr_ptr = ffi.new("float *", seg_threshold)
    fthr_ptr = ffi.new("float *", frame_threshold)
    z0_ptr = ffi.new("float *", ART_Z0)
    step_ptr = ffi.new("float *", ART_Z_STEP)
    tilt_ptr = ffi.new("float *", TILT_DEG)
    file_ptr = ffi.new("int *", file_idx)
    scroll_ptr = ffi.new("int *", 0)
    # gui_list_view truncates its joined string at 1024 chars / 128 items;
    # the _ex variant takes a real string array so every file shows up
    name_bufs = [ffi.new("char[]", n.encode()) for n in art_names]
    name_arr = ffi.new("char *[]", name_bufs)
    focus_ptr = ffi.new("int *", -1)
    export_status = ""
    export_status_until = 0.0

    scale = 3.0
    icon_offset = 0
    card_idx = 0
    lifts = [0.0] * len(CARDS)                # eased hover lift per card def
    tilts = [[0.0, 0.0] for _ in CARDS]       # eased press tilt per card def
    hex_tilts = [[0.0, 0.0] for _ in CARDS]   # eased press tilt per hex form
    frame = 0

    card_rt = None  # offscreen textures, rebuilt when the zoom changes
    hex_rt = None
    rt_scale = 0.0

    shaders_on = True
    shine = rl.load_shader_from_memory(ffi.NULL, SHINE_FS)  # NULL = default vertex shader
    shine_time = rl.get_shader_location(shine, "time")
    shine_phase = rl.get_shader_location(shine, "phase")
    bg_shader = rl.load_shader_from_memory(ffi.NULL, BG_FS)
    bg_time = rl.get_shader_location(bg_shader, "time")
    white_img = rl.gen_image_color(1, 1, rl.WHITE)  # 1x1 quad to run fullscreen/SDF shaders on
    white = rl.load_texture_from_image(white_img)
    rl.unload_image(white_img)

    hex_shader = rl.load_shader_from_memory(ffi.NULL, HEX_FS)
    hexfx = {
        "shader": hex_shader,
        "fill": rl.get_shader_location(hex_shader, "fillColor"),
        "border": rl.get_shader_location(hex_shader, "borderColor"),
        "src": rl.get_shader_location(hex_shader, "srcRect"),
        "time": rl.get_shader_location(hex_shader, "time"),
        "phase": rl.get_shader_location(hex_shader, "phase"),
        "shine": rl.get_shader_location(hex_shader, "shineStrength"),
        "white": white,
    }

    while not rl.window_should_close():
        frame += 1
        if rl.is_key_pressed(rl.KeyboardKey.KEY_RIGHT):
            icon_offset += 1
        if rl.is_key_pressed(rl.KeyboardKey.KEY_LEFT):
            icon_offset -= 1
        if rl.is_key_pressed(rl.KeyboardKey.KEY_C):
            card_idx = (card_idx + 1) % len(CARDS)
        if rl.is_key_pressed(rl.KeyboardKey.KEY_H):
            shaders_on = not shaders_on

        # Keys nudge the threshold too; the panel slider owns the same pointer
        if rl.is_key_down(rl.KeyboardKey.KEY_UP):
            thr_ptr[0] = min(255.0, thr_ptr[0] + 120.0 * rl.get_frame_time())
        if rl.is_key_down(rl.KeyboardKey.KEY_DOWN):
            thr_ptr[0] = max(0.0, thr_ptr[0] - 120.0 * rl.get_frame_time())

        # Apply panel tweaks (the sliders wrote these pointers last frame)
        if thr_ptr[0] != seg_threshold:
            seg_threshold = thr_ptr[0]
            raised_buf = cut_raised()
            # pyray wants a typed cdata pointer, not the raw buffer view
            rl.update_texture(art["raised"], ffi.cast("char *", ffi.from_buffer(raised_buf)))
        if fthr_ptr[0] != frame_threshold:
            frame_threshold = fthr_ptr[0]
            frame_raised_buf = cut_frame_raised()
            rl.update_texture(frame_raised_tex, ffi.cast("char *", ffi.from_buffer(frame_raised_buf)))
        ART_Z0, ART_Z_STEP, TILT_DEG = z0_ptr[0], step_ptr[0], tilt_ptr[0]
        if file_ptr[0] != file_idx and 0 <= file_ptr[0] < len(art_files):
            file_idx = file_ptr[0]
            load_art_source(art_files[file_idx])

        # Mouse wheel zooms the ortho camera; the offscreen resolution follows
        # the zoom so the art stays crisp close up
        cam.fovy = max(2.0, min(14.0, cam.fovy * (0.9 ** rl.get_mouse_wheel_move())))
        scale = max(1.0, min(8.0, round(3.0 * CAM_FOVY / cam.fovy * 4.0) / 4.0))

        mouse = rl.get_mouse_position()
        t = rl.get_time()
        w, h = CARD_W * scale, CARD_H * scale
        hex_radius = CARD_W * scale * 0.52  # hex outer radius in RT pixels
        ppw = SCREEN_H / cam.fovy           # pixels per world unit (ortho camera)

        ci = card_idx
        name, value, cost, color = CARDS[ci]
        icon_index = (icon_offset + ci) % art["count"]
        col_i, row_i = icon_index % art["cols"], icon_index // art["cols"]
        icon_src = rl.Rectangle(col_i * art["cw"], row_i * art["ch"], art["cw"], art["ch"])
        phase = ci * 0.19
        ui_mouse = rl.check_collision_point_rec(mouse, panel)  # panel owns the mouse there

        # One mouse ray, intersected with the board plane -- same picking as
        # the game does for its tiles
        ray = rl.get_screen_to_world_ray(mouse, cam)
        board_hit = None
        if abs(ray.direction.y) > 0.0001:
            rt_ = -ray.position.y / ray.direction.y
            if rt_ > 0:
                board_hit = (ray.position.x + ray.direction.x * rt_,
                             ray.position.z + ray.direction.z * rt_)

        # Rebuild the offscreen textures when the zoom changes; padded so the
        # coin badge overhang and parallax shifts stay inside
        if rt_scale != scale:
            if card_rt is not None:
                rl.unload_render_texture(card_rt)
                rl.unload_render_texture(hex_rt)
            pad = int(14 * scale)
            card_rt = rl.load_render_texture(int(w) + pad * 2, int(h) + pad * 2)
            hex_side = int(hex_radius * 2.15 + 14 * scale)  # SDF quad + parallax/shadow margin
            hex_rt = rl.load_render_texture(hex_side, hex_side)
            rt_scale = scale

        # Hover/tilt state. Card hover via its screen projection (the quad
        # faces the camera); hex hover from the board-plane hit
        k_card = CARD_WORLD_H / h  # world units per RT pixel
        qw, qh = card_rt.texture.width * k_card, card_rt.texture.height * k_card
        wy = CARD_Y + lifts[ci]
        sp = rl.get_world_to_screen(rl.Vector3(CARD_X, wy, CARD_Z), cam)
        hovered = (abs(mouse.x - sp.x) < qw * ppw / 2) and (abs(mouse.y - sp.y) < qh * ppw / 2) and not ui_mouse
        lifts[ci] += ((0.4 if hovered else 0.0) - lifts[ci]) * 0.2

        tilt_target = ((mouse.x - sp.x) / (qw * ppw / 2), (mouse.y - sp.y) / (qh * ppw / 2)) if hovered else (0.0, 0.0)
        tilts[ci][0] += (tilt_target[0] - tilts[ci][0]) * 0.12
        tilts[ci][1] += (tilt_target[1] - tilts[ci][1]) * 0.12

        hex_target = (0.0, 0.0)
        hex_hovered = False
        if board_hit is not None and not ui_mouse:
            hdx, hdz = (board_hit[0] - HEX_X) / HEX_WORLD_R, (board_hit[1] - HEX_Z) / HEX_WORLD_R
            if hdx * hdx + hdz * hdz < 1.0:
                hex_target = (hdx, hdz)
                hex_hovered = True

        # clicking the hex card fires its trigger animation
        if hex_hovered and rl.is_mouse_button_pressed(rl.MouseButton.MOUSE_BUTTON_LEFT):
            fx_start = t
        hex_tilts[ci][0] += (hex_target[0] - hex_tilts[ci][0]) * 0.12
        hex_tilts[ci][1] += (hex_target[1] - hex_tilts[ci][1]) * 0.12

        hqs = hex_rt.texture.width * (HEX_WORLD_R / hex_radius)

        # Render the card face and the tile face into their textures
        rl.begin_texture_mode(card_rt)
        rl.clear_background(rl.Color(0, 0, 0, 0))
        ccx, ccy = card_rt.texture.width / 2, card_rt.texture.height / 2
        draw_card(tex, ccx, ccy, scale, name, value, cost, color, icon_index, tilts[ci])
        if shaders_on:
            # Foil shine, baked into the card texture so it tilts along.
            # Rides the frame's parallax layer so the two stay glued.
            rl.begin_shader_mode(shine)
            set_shader_float(shine, shine_time, t)
            set_shader_float(shine, shine_phase, phase)
            rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
            fx, fy = tilts[ci][0] * PAR_FRAME * scale, tilts[ci][1] * PAR_FRAME * scale
            rl.draw_texture_pro(tex["outline"], rl.Rectangle(0, 0, CARD_W, CARD_H),
                                rl.Rectangle(ccx - w / 2 + fx, ccy - h / 2 + fy, w, h),
                                rl.Vector2(0, 0), 0.0, rl.WHITE)
            rl.end_blend_mode()
            rl.end_shader_mode()
        rl.end_texture_mode()

        rl.begin_texture_mode(hex_rt)
        rl.clear_background(rl.Color(0, 0, 0, 0))
        hcx = hex_rt.texture.width / 2
        draw_hex_form(tex, hcx, hcx, scale, value, color, icon_index,
                      hex_tilts[ci], hexfx, t, phase + 0.1, shaders_on)
        rl.end_texture_mode()

        # Compose: background in 2D, then the real 3D scene: the card stands
        # facing the camera, its hex form lies flat on the board plane
        rl.begin_drawing()
        rl.clear_background(rl.RAYWHITE)

        if shaders_on:
            rl.begin_shader_mode(bg_shader)
            set_shader_float(bg_shader, bg_time, t)
            rl.draw_texture_pro(white, rl.Rectangle(0, 0, 1, 1),
                                rl.Rectangle(0, 0, SCREEN_W, SCREEN_H),
                                rl.Vector2(0, 0), 0.0, rl.WHITE)
            rl.end_shader_mode()

        rl.begin_mode_3d(cam)
        wpt = CARD_WORLD_H / CARD_H  # world units per template pixel

        # Card, then its art relief floating in the arch window (slightly
        # inset so the frame's inner edge stays visible around it)
        draw_rt_quad_3d(card_rt, CARD_X, wy, CARD_Z, qw, qh, -CAM_ELEV_DEG, tilts[ci])
        draw_art_relief_3d([art["back"], art["raised"]], icon_src, CARD_X, wy, CARD_Z,
                           WINDOW[2] * wpt * 0.94, WINDOW[3] * wpt * 0.94, -CAM_ELEV_DEG, tilts[ci],
                           0.0, -(WINDOW[1] + WINDOW[3] / 2 - CARD_H / 2) * wpt,
                           ART_Z0, ART_Z_STEP)

        # Raised frame: the template's bright ornaments float highest, cut by
        # their own threshold slider
        draw_art_relief_3d([frame_raised_tex], rl.Rectangle(0, 0, CARD_W, CARD_H),
                           CARD_X, wy, CARD_Z, CARD_W * wpt, CARD_WORLD_H,
                           -CAM_ELEV_DEG, tilts[ci], 0.0, 0.0,
                           ART_Z0 + ART_Z_STEP + 0.03, 0.0)

        # Tile, then its art relief rising off the board -- SDF-cropped and
        # sized from the world radius (the RT quad has padding margins) so
        # it fills the tile up to the border ring, not over it
        hex_art = HEX_WORLD_R * 1.9  # art hex outer radius ~= ring inner edge
        draw_rt_quad_3d(hex_rt, HEX_X, 0.02, HEX_Z, hqs, hqs, -90.0, hex_tilts[ci])
        draw_art_relief_3d([art["back"], art["raised"]], icon_src, HEX_X, 0.02, HEX_Z, hex_art, hex_art,
                           -90.0, hex_tilts[ci], 0.0, 0.0, ART_Z0, ART_Z_STEP,
                           hexfx, t, phase + 0.1)

        # trigger animation: the impact footage on a quad facing the camera,
        # centered over the hex card. The NopiA effect is a dark ink splash,
        # so it draws with plain alpha blending, not additive.
        fx_t = (t - fx_start) / FX_DUR
        if 0.0 <= fx_t < 1.0:
            fx_frame = min(int(fx_t * FX_FRAMES), FX_FRAMES - 1)
            fx_src = rl.Rectangle(fx_frame * 192, 0, 192, 192)
            fx_size = HEX_WORLD_R * 3.2
            draw_art_relief_3d([tex["impact_fx"]], fx_src, HEX_X, 0.9, HEX_Z,
                               fx_size, fx_size, -CAM_ELEV_DEG, (0.0, 0.0),
                               0.0, 0.0, 0.0, 0.0)
        rl.end_mode_3d()

        # Text overlay, projected from world space so it floats above the art
        spt = ppw * wpt  # screen px per template px
        name_px = max(int(NAME_SIZE * spt), 10)
        rl.draw_text(name, int(sp.x - rl.measure_text(name, name_px) / 2),
                     int(sp.y + 6.0 * spt), name_px, rl.Color(222, 210, 190, 255))

        value_px = int(VALUE_SIZE * spt)
        vt = str(value)
        vx = int(sp.x - rl.measure_text(vt, value_px) / 2)
        vy = int(sp.y + 14.0 * spt)
        rl.draw_text(vt, vx + 2, vy + 2, value_px, rl.Color(20, 12, 12, 255))
        rl.draw_text(vt, vx, vy, value_px, rl.Color(color[0], color[1], color[2], 255))

        hsp = rl.get_world_to_screen(
            rl.Vector3(HEX_X, ART_Z0 + ART_RAISED_LAYERS * ART_Z_STEP,
                       HEX_Z + HEX_WORLD_R * 0.45), cam)
        hv_px = int(VALUE_SIZE * spt * 0.9)
        hvx = int(hsp.x - rl.measure_text(vt, hv_px) / 2)
        hvy = int(hsp.y - hv_px / 2)
        rl.draw_text(vt, hvx + 2, hvy + 2, hv_px, rl.Color(20, 12, 12, 255))
        rl.draw_text(vt, hvx, hvy, hv_px, rl.Color(color[0], color[1], color[2], 255))

        rl.draw_text("wheel: zoom   up/down: threshold   left/right: icons   c: card   h: shaders   s: screenshot",
                     16, SCREEN_H - 28, 20, rl.GRAY)
        rl.draw_text(f"{name}  ({ci + 1}/{len(CARDS)})   zoom {CAM_FOVY / cam.fovy:.2f}x",
                     16, 16, 20, rl.DARKGRAY)

        # Tweaker panel (raygui): sliders write their pointers, applied at the
        # top of the next frame
        rl.gui_window_box(panel, "tweakers")
        px, py = int(panel.x + 10), int(panel.y + 32)
        rl.gui_label(rl.Rectangle(px, py, 216, 16), f"art brightness > {seg_threshold:.0f}  ({raised_count} px)")
        rl.gui_slider_bar(rl.Rectangle(px, py + 18, 216, 16), "", "", thr_ptr, 0.0, 255.0)
        rl.gui_label(rl.Rectangle(px, py + 42, 216, 16), f"frame brightness > {frame_threshold:.0f}  ({frame_raised_count} px)")
        rl.gui_slider_bar(rl.Rectangle(px, py + 60, 216, 16), "", "", fthr_ptr, 0.0, 255.0)
        rl.gui_label(rl.Rectangle(px, py + 84, 216, 16), f"relief base  {ART_Z0:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 102, 216, 16), "", "", z0_ptr, 0.0, 0.4)
        rl.gui_label(rl.Rectangle(px, py + 126, 216, 16), f"relief step  {ART_Z_STEP:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 144, 216, 16), "", "", step_ptr, 0.0, 0.6)
        rl.gui_label(rl.Rectangle(px, py + 168, 216, 16), f"press tilt  {TILT_DEG:.0f} deg")
        rl.gui_slider_bar(rl.Rectangle(px, py + 186, 216, 16), "", "", tilt_ptr, 0.0, 45.0)
        rl.gui_label(rl.Rectangle(px, py + 212, 216, 16), "art source:")
        rl.gui_list_view_ex(rl.Rectangle(px, py + 230, 216, 252), name_arr, len(name_bufs), scroll_ptr, file_ptr, focus_ptr)

        # Export the current composition (threshold, heights, art, color) to glb
        if rl.gui_button(rl.Rectangle(px, py + 490, 216, 26), "export .glb"):
            x0, y0 = int(icon_src.x), int(icon_src.y)
            cw_, ch_ = int(icon_src.width), int(icon_src.height)
            raised_full = cut_raised()
            out_name = f"{name}_{art_files[file_idx].stem}_{icon_index}".replace(" ", "-")
            out = export_card_glb(
                Path(__file__).parent / "export" / f"{out_name}.glb",
                {
                    "template_path": ASSETS / "fire_card_39x66.png",
                    "coin_path": ASSETS / "CoinIcon_16x18.png",
                    "art_back": art["rgba"][y0:y0 + ch_, x0:x0 + cw_].copy(),
                    "art_raised": raised_full[y0:y0 + ch_, x0:x0 + cw_].copy(),
                    "frame_raised": cut_frame_raised(),
                    "frame_z": ART_Z0 + ART_Z_STEP + 0.03,
                    "color": (color[0], color[1], color[2]),
                    "card_h": CARD_WORLD_H,
                    "window": WINDOW,
                    "template_size": (CARD_W, CARD_H),
                    "art_z0": ART_Z0,
                    "art_z_step": ART_Z_STEP,
                    "hex_radius": HEX_WORLD_R,
                })
            export_status = f"wrote export/{out.stem}(.glb, _card.glb, _hex.glb)"
            export_status_until = t + 4.0

        if t < export_status_until:
            rl.draw_text(export_status, 16, SCREEN_H - 56, 20, rl.Color(0, 117, 44, 255))
        rl.end_drawing()

        if rl.is_key_pressed(rl.KeyboardKey.KEY_S) or (shot_mode and frame == 40):
            rl.take_screenshot("card_proto_shot.png")
            if shot_mode:
                break

    rl.close_window()


if __name__ == "__main__":
    main()
