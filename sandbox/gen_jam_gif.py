#!/usr/bin/env python3
"""Render the jam GIF: the enemy over the card gems seen THROUGH a gem.

    python3 gen_jam_gif.py

Writes ../screenshots/enemy_jam.gif (630x500 itch cover, seamless loop).

Two passes, like the game's own gem material (src/raylib_game.c gemFS
refracts the scene texture): the four card gems render into a texture,
a fullscreen faceted-refraction shader draws that texture as the
background -- looking at the cards through cut crystal -- and the enemy
mesh (enemy_proto.py shaders + enemy_look.json) breathes on top.

Seamless loop: every animated rate (spikes, breath, spin, outline pulse,
facet shimmer, card spin) is snapped to integer cycles over the loop and
driven by a synthetic clock.
"""

import json
import math
from pathlib import Path

import pyray as rl
from raylib import ffi
from PIL import Image

from enemy_proto import ENEMY_VS, ENEMY_FS, OUTLINE_FS, build_enemy_glb, set_f, LOOK_FILE

WIDTH, HEIGHT = 630, 500 # itch cover size (recommended), gif must stay under ~3 MB
LOOP_SECONDS = 6.0       # One full loop; the enemy makes exactly one turn
FPS = 10
FRAMES = int(LOOP_SECONDS * FPS)
OUT = Path(__file__).parent.parent / "screenshots" / "enemy_jam.gif"
SCRATCH = Path(__file__).parent / "jam_frames"
TITLE_FONT = "/usr/share/fonts/opentype/urw-base35/P052-Bold.otf"  # Palatino: radiant historical serif

TWO_PI = math.pi * 2.0

# Stock fullscreen vertex shader for the facet pass (pyray needs explicit
# source; passing None is not supported)
DEFAULT_VS = """
#version 100
attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec4 vertexColor;
uniform mat4 mvp;
varying vec2 fragTexCoord;
varying vec4 fragColor;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
"""

# Looking through the gem: skewed facet cells, each tilting the view ray a
# little (per-facet pseudo-normal offsets the sample), glinting rims where
# facets meet, cool glass tint -- the same trick as the game's gem material,
# with procedural facets instead of mesh normals
FACET_FS = """
#version 100
precision mediump float;
varying vec2 fragTexCoord;
varying vec4 fragColor;
uniform sampler2D texture0;
uniform float time; // pre-snapped: exactly one shimmer cycle per loop

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233)))*43758.5453); }

void main()
{
    vec2 uv = fragTexCoord;
    vec2 g = uv*vec2(7.0, 5.5);
    g.x += floor(g.y)*0.5; // brick-skewed cells read as cut facets
    vec2 id = floor(g);
    vec2 f = fract(g);

    float h1 = hash(id);
    float h2 = hash(id + 17.0);
    // fade the tilt near the borders so no facet samples off the texture
    float window = smoothstep(0.0, 0.10, uv.x)*smoothstep(1.0, 0.90, uv.x)
                 * smoothstep(0.0, 0.10, uv.y)*smoothstep(1.0, 0.90, uv.y);
    vec2 tilt = (vec2(h1, h2) - 0.5)*0.085*window;
    vec2 shimmer = 0.012*window*vec2(sin(time + h1*6.2831), cos(time + h2*6.2831));
    vec3 col = texture2D(texture0, uv + tilt + shimmer).rgb;

    vec2 e = min(f, 1.0 - f);
    float glint = 0.15 + 0.55*hash(id + 31.0);        // only some rims catch light
    float edge = smoothstep(0.018, 0.0, min(e.x, e.y))*glint;
    col = mix(col, col*vec3(0.94, 0.98, 1.08), 0.18); // whisper of crystal tint
    col *= 0.90 + 0.18*h1;                            // per-facet value variance
    col += edge*vec3(0.30, 0.27, 0.33);               // glinting rims
    gl_FragColor = vec4(col, 1.0);
}
"""


def snapped(rate, loop):
    """The closest rate that completes an integer number of sine cycles
    over the loop (at least one), so the seam disappears."""
    cycles = max(1, round(rate * loop / TWO_PI))
    return TWO_PI * cycles / loop


def main():
    look = json.loads(LOOK_FILE.read_text()) if LOOK_FILE.is_file() else {}
    size = float(look.get("size", 0.9))
    spike_len = float(look.get("spike_len", 0.35))
    spike_density = float(look.get("spike_density", 0.45))
    rim = float(look.get("rim", 0.8))
    line_width = float(look.get("line_width", 2.0))
    outline = float(look.get("red_outline", 0.05))
    subdiv = int(look.get("subdiv", 2))
    seed = int(look.get("seed", 7))

    pulse_speed = snapped(float(look.get("pulse_speed", 2.2)), LOOP_SECONDS)
    breathe_speed = snapped(float(look.get("breathe_speed", 1.6)), LOOP_SECONDS)
    outline_pulse = snapped(2.6, LOOP_SECONDS)
    yaw_rate = TWO_PI / LOOP_SECONDS  # exactly one turn per loop

    rl.init_window(WIDTH, HEIGHT, "jam gif render")
    target = rl.load_render_texture(WIDTH, HEIGHT)  # final frame FBO
    scene = rl.load_render_texture(WIDTH, HEIGHT)   # the cards, pre-refraction

    shader = rl.load_shader_from_memory(ENEMY_VS, ENEMY_FS)
    U = {n: rl.get_shader_location(shader, n)
         for n in ("time", "yaw", "spikeLen", "spikeDensity", "pulseSpeed",
                   "breatheAmp", "breatheSpeed", "viewPos", "rim", "lineWidth", "expand")}
    # The outline pulse rate is baked in the FS source; swap it for the
    # loop-snapped rate before compiling
    outline_fs = OUTLINE_FS.replace("time*2.6", f"time*{outline_pulse:.6f}")
    outline_shader = rl.load_shader_from_memory(ENEMY_VS, outline_fs)
    UO = {n: rl.get_shader_location(outline_shader, n)
          for n in ("time", "yaw", "spikeLen", "spikeDensity", "pulseSpeed",
                    "breatheAmp", "breatheSpeed", "expand", "alpha")}
    facet_shader = rl.load_shader_from_memory(DEFAULT_VS, FACET_FS)
    facet_time_loc = rl.get_shader_location(facet_shader, "time")

    model = rl.load_model(str(build_enemy_glb(subdiv, seed)))

    # the four card gems from the game, laid out for the background pass
    models_dir = Path(__file__).parent.parent / "src" / "resources" / "models"
    gem_names = ["strike_16x16-Icon85_0_hex.glb", "strike_16x16-Icon16_0_hex.glb",
                 "strike_16x16-Icon38_0_hex.glb", "strike_16x16-Icon29_0_hex.glb"]
    gems = [rl.load_model(str(models_dir / n)) for n in gem_names]
    gem_spots = [(-1.05, 0.55), (1.05, 0.55), (-1.05, -0.95), (1.05, -0.95)]
    gem_scale = rl.Vector3(0.58, 0.58, 0.58)

    # title font: an old-book serif, rendered big and filtered
    title_font = rl.load_font_ex(TITLE_FONT, 68, None, 0)
    rl.set_texture_filter(title_font.texture, rl.TextureFilter.TEXTURE_FILTER_BILINEAR)

    cam = rl.Camera3D(rl.Vector3(0.0, 1.42, 2.1), rl.Vector3(0.0, 1.05, 0.0),
                      rl.Vector3(0.0, 1.0, 0.0), 42.0,
                      rl.CameraProjection.CAMERA_PERSPECTIVE)
    # background pass camera: looking down at the cards on the table
    cam_bg = rl.Camera3D(rl.Vector3(0.0, 4.4, 1.6), rl.Vector3(0.0, 0.0, 0.0),
                         rl.Vector3(0.0, 1.0, 0.0), 42.0,
                         rl.CameraProjection.CAMERA_PERSPECTIVE)

    SCRATCH.mkdir(exist_ok=True)

    for frame in range(FRAMES):
        rl.begin_drawing()  # pump events; the scene renders into FBOs below
        t = frame / FPS
        phase = t / LOOP_SECONDS
        yaw = yaw_rate * t

        # PASS A: the cards on tan paper, spinning slowly (one turn per loop)
        rl.begin_texture_mode(scene)
        rl.clear_background(rl.Color(233, 221, 197, 255))
        rl.begin_mode_3d(cam_bg)
        rl.rl_disable_backface_culling()
        for g, (gem, (gx, gz)) in enumerate(zip(gems, gem_spots)):
            spin = math.degrees(yaw) * (1.0 if g % 2 == 0 else -1.0) + g * 45.0
            bob = 0.08 * math.sin(TWO_PI * (2.0 * phase) + g * 1.7)
            rl.draw_model_ex(gem, rl.Vector3(gx, bob, gz),
                             rl.Vector3(0.0, 1.0, 0.0), spin, gem_scale, rl.WHITE)
        rl.rl_enable_backface_culling()
        rl.end_mode_3d()
        rl.end_texture_mode()

        # PASS B: the cards seen through the gem, then the enemy on top
        rl.begin_texture_mode(target)
        rl.clear_background(rl.Color(233, 221, 197, 255))
        set_f(facet_shader, facet_time_loc, TWO_PI * phase)
        rl.begin_shader_mode(facet_shader)
        rl.draw_texture_rec(scene.texture,
                            rl.Rectangle(0, 0, WIDTH, -HEIGHT),  # FBO textures are flipped
                            rl.Vector2(0, 0), rl.WHITE)
        rl.end_shader_mode()

        # enemy uniforms off the synthetic clock
        set_f(shader, U["time"], t)
        set_f(shader, U["yaw"], yaw)
        set_f(shader, U["spikeLen"], spike_len)
        set_f(shader, U["spikeDensity"], spike_density)
        set_f(shader, U["pulseSpeed"], pulse_speed)
        set_f(shader, U["breatheAmp"], float(look.get("breathe_amp", 0.06)))
        set_f(shader, U["breatheSpeed"], breathe_speed)
        set_f(shader, U["rim"], rim)
        set_f(shader, U["lineWidth"], line_width)
        set_f(shader, U["expand"], 0.0)
        rl.set_shader_value(shader, U["viewPos"],
                            ffi.new("float[3]", [cam.position.x, cam.position.y, cam.position.z]),
                            rl.ShaderUniformDataType.SHADER_UNIFORM_VEC3)
        set_f(outline_shader, UO["time"], t)
        set_f(outline_shader, UO["yaw"], yaw)
        set_f(outline_shader, UO["spikeLen"], spike_len)
        set_f(outline_shader, UO["spikeDensity"], spike_density)
        set_f(outline_shader, UO["pulseSpeed"], pulse_speed)
        set_f(outline_shader, UO["breatheAmp"], float(look.get("breathe_amp", 0.06)))
        set_f(outline_shader, UO["breatheSpeed"], breathe_speed)

        rl.begin_mode_3d(cam)
        pos = rl.Vector3(0.0, 1.05, 0.0)

        # L4D outline rings under the body, tracking the spikes
        if outline > 0.002:
            rl.rl_draw_render_batch_active()
            rl.rl_set_cull_face(rl.rl.RL_CULL_FACE_FRONT)
            model.materials[0].shader = outline_shader
            set_f(outline_shader, UO["expand"], outline)
            set_f(outline_shader, UO["alpha"], 1.0)
            rl.draw_model(model, pos, size, rl.WHITE)
            rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
            set_f(outline_shader, UO["expand"], outline * 2.6)
            set_f(outline_shader, UO["alpha"], 0.28)
            rl.draw_model(model, pos, size, rl.WHITE)
            rl.end_blend_mode()
            rl.rl_set_cull_face(rl.rl.RL_CULL_FACE_BACK)

        model.materials[0].shader = shader
        rl.draw_model(model, pos, size, rl.WHITE)
        rl.end_mode_3d()

        # title: radiant old-book serif -- warm halo rings, an ink outline,
        # and a gold fill
        label = "Leylines"
        font_size = 64
        tw = rl.measure_text_ex(title_font, label, font_size, 1.0).x
        tx, ty = (WIDTH - tw) / 2.0, HEIGHT - font_size - 10
        for radius, glow in ((7, 40), (4, 70)):
            for k in range(8):
                a = TWO_PI * k / 8.0
                at = rl.Vector2(tx + math.cos(a) * radius, ty + math.sin(a) * radius)
                rl.draw_text_ex(title_font, label, at, font_size, 1.0,
                                rl.Color(255, 170, 60, glow))
        for oy in (-2, 0, 2):
            for ox in (-2, 0, 2):
                if ox or oy:
                    rl.draw_text_ex(title_font, label, rl.Vector2(tx + ox, ty + oy),
                                    font_size, 1.0, rl.Color(28, 16, 8, 255))
        rl.draw_text_ex(title_font, label, rl.Vector2(tx, ty), font_size, 1.0,
                        rl.Color(255, 214, 70, 255))

        rl.end_texture_mode()
        rl.end_drawing()
        shot = rl.load_image_from_texture(target.texture)
        rl.image_flip_vertical(shot)  # render textures come out upside down
        rl.export_image(shot, str(SCRATCH / f"frame_{frame:03d}.png"))
        rl.unload_image(shot)

    rl.close_window()

    # assemble: one shared adaptive palette keeps the file small and the
    # colors steady across the loop
    frames = [Image.open(SCRATCH / f"frame_{n:03d}.png").convert("RGB") for n in range(FRAMES)]
    palette_src = frames[len(frames) // 2].quantize(colors=128)
    quantized = [f.quantize(palette=palette_src, dither=Image.Dither.NONE) for f in frames]
    OUT.parent.mkdir(exist_ok=True)
    quantized[0].save(OUT, save_all=True, append_images=quantized[1:],
                      duration=int(1000 / FPS), loop=0, optimize=True)
    for f in SCRATCH.glob("frame_*.png"):
        f.unlink()
    SCRATCH.rmdir()
    print(f"wrote {OUT} ({FRAMES} frames, {OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
