#!/usr/bin/env python3
"""Enemy look architect: a spiky, breathing icosphere with pastel wires.

    python3 enemy_proto.py          # interactive
    python3 enemy_proto.py --shot   # screenshot after a second, then exit

The enemy is an icosphere displaced entirely in the vertex shader: a stable
per-vertex hash picks which vertices spike, spikes pulse over time, and the
whole body breathes. One shader draws the whole look: near-black body with a
pastel fresnel rim, plus the mesh-defining lines rendered in the fragment
shader from barycentric coords (corner ids ride the vertex color alpha of
the unwelded mesh; the RGB is the pastel palette). fwidth keeps the lines a
constant pixel width -- and it all works on GLES/WebGL, unlike glPolygonMode.

"save look" writes enemy_look.json; "reroll colors" / subdiv rebuild the mesh.

Controls:
    right drag   orbit          wheel   zoom          s   screenshot
"""

import json
import math
import sys
from pathlib import Path

import numpy as np
import trimesh
import pyray as rl
from raylib import ffi

ASSETS = Path(__file__).parent / "assets"
LOOK_FILE = Path(__file__).parent / "enemy_look.json"
SCREEN_W, SCREEN_H = 900, 620
TILE_R = 1.0
TILE_H = 0.35

# soft pastel palette the wires cycle through
PASTELS = [(255, 179, 186), (255, 223, 186), (255, 255, 186),
           (186, 255, 201), (186, 225, 255), (222, 197, 255)]

# All three shaders are written in GLSL 100 -- the WebGL/GLES dialect the
# wasm build compiles -- and desktop GL accepts them through ES2
# compatibility, so what works here works in the browser verbatim.
ENEMY_VS = """
#version 100
attribute vec3 vertexPosition;
attribute vec3 vertexNormal;
attribute vec4 vertexColor;
uniform mat4 mvp;
uniform float time;
uniform float yaw;          // spin lives here so world pos stays honest
uniform float spikeLen;
uniform float spikeDensity; // fraction of vertices that grow spikes
uniform float pulseSpeed;
uniform float breatheAmp;
uniform float breatheSpeed;
uniform float expand;       // inverted-hull outline pass pushes along normals
varying vec4 fragColor;
varying vec3 fragNormal;
varying vec3 fragWorldPos;
varying vec3 fragBary;

float hash(vec3 p) { return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719)))*43758.5453); }

void main()
{
    float h = hash(vertexPosition);
    float mask = smoothstep(1.0 - spikeDensity, 1.0, h);
    float wob = 0.7 + 0.3*sin(time*pulseSpeed + h*6.2831);
    float breath = 1.0 + breatheAmp*sin(time*breatheSpeed);

    vec3 p = (vertexPosition + vertexNormal*(spikeLen*mask*wob + expand))*breath;
    float c = cos(yaw); float s = sin(yaw);
    mat3 rot = mat3(c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c);
    p = rot*p;

    // corner id one-hot from the color alpha (0 / 0.5 / 1): interpolating it
    // across the triangle yields barycentric coords for the edge shader
    float aa = vertexColor.a;
    fragBary = vec3(1.0 - step(0.25, aa), step(0.25, aa)*(1.0 - step(0.75, aa)), step(0.75, aa));

    fragColor = vec4(vertexColor.rgb, 1.0);
    fragNormal = rot*vertexNormal;
    fragWorldPos = p;
    gl_Position = mvp*vec4(p, 1.0);
}
"""

# One pass does it all: near-black body, pastel fresnel rim, and the mesh
# lines drawn where any barycentric coord runs out -- fwidth (via the
# standard_derivatives extension on GLES) keeps the line width constant in
# screen pixels
ENEMY_FS = """
#version 100
#extension GL_OES_standard_derivatives : enable
precision mediump float;
varying vec4 fragColor;
varying vec3 fragNormal;
varying vec3 fragWorldPos;
varying vec3 fragBary;
uniform vec3 viewPos;
uniform float rim;        // how much pastel bleeds in at grazing angles
uniform float lineWidth;  // wire width in screen pixels

void main()
{
    vec3 d = fwidth(fragBary);
    vec3 a3 = smoothstep(vec3(0.0), d*lineWidth, fragBary);
    float edge = 1.0 - min(min(a3.x, a3.y), a3.z);

    vec3 V = normalize(viewPos - fragWorldPos);
    float fres = pow(1.0 - clamp(dot(V, normalize(fragNormal)), 0.0, 1.0), 2.5);
    vec3 body = mix(vec3(0.05, 0.05, 0.09), fragColor.rgb, fres*rim);
    gl_FragColor = vec4(mix(body, fragColor.rgb, edge), 1.0);
}
"""

# Left 4 Dead style outline: the same displaced mesh expanded along its
# normals with front faces culled (inverted hull), flat danger-red with a
# slow pulse -- so the silhouette ring tracks the spikes exactly
OUTLINE_FS = """
#version 100
precision mediump float;
varying vec4 fragColor;
varying vec3 fragNormal;
varying vec3 fragWorldPos;
varying vec3 fragBary;
uniform float time;
uniform float alpha;

void main()
{
    float pulse = 0.85 + 0.15*sin(time*2.6);
    gl_FragColor = vec4(vec3(1.0, 0.08, 0.06)*pulse, alpha);
}
"""


def build_enemy_glb(subdiv, seed):
    """Icosphere with pastel palette vertex colors. Triangles are unwelded so
    every corner can carry its barycentric id in the color alpha (0/128/255);
    smooth normals from the welded sphere are kept so shared corners displace
    identically and the spiked mesh stays watertight."""
    rng = np.random.default_rng(seed)
    ico = trimesh.creation.icosphere(subdivisions=subdiv, radius=1.0)
    smooth_normals = np.asarray(ico.vertex_normals, np.float64).copy()
    palette = np.array(PASTELS, np.uint8)[rng.integers(0, len(PASTELS), len(ico.vertices))]

    idx = ico.faces.ravel()
    verts = ico.vertices[idx]
    norms = smooth_normals[idx]
    rgb = palette[idx]
    alpha = np.tile(np.array([0, 128, 255], np.uint8), len(ico.faces))
    colors = np.column_stack([rgb, alpha])

    faces = np.arange(len(verts)).reshape(-1, 3)
    mesh = trimesh.Trimesh(vertices=verts, faces=faces,
                           vertex_normals=norms, process=False)
    mesh.visual = trimesh.visual.ColorVisuals(mesh, vertex_colors=colors)
    ASSETS.mkdir(parents=True, exist_ok=True)
    out = ASSETS / "enemy_ico.glb"
    mesh.export(str(out), include_normals=True)
    return out


def set_f(shader, loc, v):
    rl.set_shader_value(shader, loc, ffi.new("float *", v),
                        rl.ShaderUniformDataType.SHADER_UNIFORM_FLOAT)


def main():
    shot_mode = "--shot" in sys.argv

    rl.init_window(SCREEN_W, SCREEN_H, "enemy architect")
    rl.set_target_fps(60)

    cam = rl.Camera3D(rl.Vector3(0.0, 2.6, 4.6), rl.Vector3(0.0, 1.1, 0.0),
                      rl.Vector3(0.0, 1.0, 0.0), 42.0,
                      rl.CameraProjection.CAMERA_PERSPECTIVE)

    shader = rl.load_shader_from_memory(ENEMY_VS, ENEMY_FS)
    U = {name: rl.get_shader_location(shader, name)
         for name in ("time", "yaw", "spikeLen", "spikeDensity", "pulseSpeed",
                      "breatheAmp", "breatheSpeed", "viewPos", "rim", "lineWidth", "expand")}
    outline_shader = rl.load_shader_from_memory(ENEMY_VS, OUTLINE_FS)
    UO = {name: rl.get_shader_location(outline_shader, name)
          for name in ("time", "yaw", "spikeLen", "spikeDensity", "pulseSpeed",
                       "breatheAmp", "breatheSpeed", "expand", "alpha")}

    params = {
        "size": ffi.new("float *", 0.9),
        "spike_len": ffi.new("float *", 0.35),
        "spike_density": ffi.new("float *", 0.45),
        "pulse_speed": ffi.new("float *", 2.2),
        "breathe_amp": ffi.new("float *", 0.06),
        "breathe_speed": ffi.new("float *", 1.6),
        "spin": ffi.new("float *", 18.0),
        "line_width": ffi.new("float *", 2.0),
        "red_outline": ffi.new("float *", 0.05),  # hull expansion, 0 = off
        "rim": ffi.new("float *", 0.8),
        "subdiv": ffi.new("float *", 2.0),
        "seed": ffi.new("float *", 7.0),
    }

    model = None
    built = (None, None)

    def rebuild():
        nonlocal model, built
        want = (int(params["subdiv"][0]), int(params["seed"][0]))
        if want == built and model is not None:
            return
        if model is not None:
            rl.unload_model(model)
        model = rl.load_model(str(build_enemy_glb(*want)))
        built = want

    def save_look():
        data = {k: round(v[0], 4) for k, v in params.items()}
        LOOK_FILE.write_text(json.dumps(data, indent=4) + "\n")

    def load_look():
        if LOOK_FILE.is_file():
            data = json.loads(LOOK_FILE.read_text())
            for k, v in params.items():
                if k in data:
                    v[0] = float(data[k])

    load_look()
    rebuild()

    panel = rl.Rectangle(636, 12, 254, 500)
    status = "icosphere enemy"
    yaw = 0.0
    frame_counter = 0

    while not rl.window_should_close():
        frame_counter += 1
        t = rl.get_time()
        dt = rl.get_frame_time()
        mouse = rl.get_mouse_position()
        ui_mouse = rl.check_collision_point_rec(mouse, panel)
        yaw += math.radians(params["spin"][0]) * dt

        if rl.is_mouse_button_down(rl.MouseButton.MOUSE_BUTTON_RIGHT) and not ui_mouse:
            d = rl.get_mouse_delta()
            off = rl.vector3_subtract(cam.position, cam.target)
            radius = rl.vector3_length(off)
            cyaw = math.atan2(off.x, off.z) - d.x * 0.005
            pitch = math.asin(off.y / radius) - d.y * 0.005
            pitch = min(max(pitch, 0.08), 1.45)
            cam.position = rl.Vector3(cam.target.x + radius * math.cos(pitch) * math.sin(cyaw),
                                      cam.target.y + radius * math.sin(pitch),
                                      cam.target.z + radius * math.cos(pitch) * math.cos(cyaw))
        wheel = rl.get_mouse_wheel_move()
        if wheel != 0.0 and not ui_mouse:
            off = rl.vector3_subtract(cam.position, cam.target)
            cam.position = rl.vector3_add(cam.target, rl.vector3_scale(off, 1.0 - wheel * 0.08))

        rebuild()

        set_f(shader, U["time"], t)
        set_f(shader, U["yaw"], yaw)
        set_f(shader, U["spikeLen"], params["spike_len"][0])
        set_f(shader, U["spikeDensity"], params["spike_density"][0])
        set_f(shader, U["pulseSpeed"], params["pulse_speed"][0])
        set_f(shader, U["breatheAmp"], params["breathe_amp"][0])
        set_f(shader, U["breatheSpeed"], params["breathe_speed"][0])
        set_f(shader, U["rim"], params["rim"][0])
        set_f(shader, U["lineWidth"], params["line_width"][0])
        set_f(shader, U["expand"], 0.0)
        rl.set_shader_value(shader, U["viewPos"],
                            ffi.new("float[3]", [cam.position.x, cam.position.y, cam.position.z]),
                            rl.ShaderUniformDataType.SHADER_UNIFORM_VEC3)
        for name in ("time", "yaw"):
            set_f(outline_shader, UO[name], t if name == "time" else yaw)
        set_f(outline_shader, UO["spikeLen"], params["spike_len"][0])
        set_f(outline_shader, UO["spikeDensity"], params["spike_density"][0])
        set_f(outline_shader, UO["pulseSpeed"], params["pulse_speed"][0])
        set_f(outline_shader, UO["breatheAmp"], params["breathe_amp"][0])
        set_f(outline_shader, UO["breatheSpeed"], params["breathe_speed"][0])

        rl.begin_drawing()
        rl.clear_background(rl.Color(18, 18, 26, 255))

        rl.begin_mode_3d(cam)
        rl.draw_grid(10, 1.0)

        # game-style tile context: hex prism + dim neighbour stubs
        rl.draw_cylinder(rl.Vector3(0, 0, 0), TILE_R, TILE_R, TILE_H, 6, rl.Color(96, 96, 104, 255))
        rl.draw_cylinder_wires(rl.Vector3(0, 0, 0), TILE_R, TILE_R, TILE_H, 6, rl.Color(232, 190, 170, 255))
        for k in range(6):
            a = math.radians(60.0 * k + 30.0)
            nx, nz = math.cos(a) * TILE_R * 1.85, math.sin(a) * TILE_R * 1.85
            rl.draw_cylinder(rl.Vector3(nx, 0, nz), TILE_R, TILE_R, TILE_H * 0.6, 6, rl.Color(52, 52, 60, 255))

        # the enemy floats over the tile; spin/spikes/breath all in the VS
        size = params["size"][0]
        hover_y = TILE_H + size + 0.35
        pos = rl.Vector3(0.0, hover_y, 0.0)

        # L4D red outline: inverted hulls drawn before the body -- a solid
        # ring plus a fainter, fatter glow ring, both tracking the spikes
        o = params["red_outline"][0]
        if o > 0.002:
            # flush queued batch quads before flipping the cull face
            rl.rl_draw_render_batch_active()
            rl.rl_set_cull_face(rl.rl.RL_CULL_FACE_FRONT)
            model.materials[0].shader = outline_shader
            set_f(outline_shader, UO["expand"], o)
            set_f(outline_shader, UO["alpha"], 1.0)
            rl.draw_model(model, pos, size, rl.WHITE)
            rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
            set_f(outline_shader, UO["expand"], o * 2.6)
            set_f(outline_shader, UO["alpha"], 0.28)
            rl.draw_model(model, pos, size, rl.WHITE)
            rl.end_blend_mode()
            rl.rl_set_cull_face(rl.rl.RL_CULL_FACE_BACK)

        model.materials[0].shader = shader
        rl.draw_model(model, pos, size, rl.WHITE)

        # blob shadow, breathing with the body
        breath = 1.0 + params["breathe_amp"][0] * math.sin(t * params["breathe_speed"][0])
        srad = size * 0.7 * breath
        rl.draw_cylinder(rl.Vector3(0.0, TILE_H + 0.005, 0.0), srad, srad, 0.01, 24,
                         rl.fade(rl.BLACK, 0.4))

        rl.end_mode_3d()

        # panel
        rl.gui_window_box(panel, "enemy architect")
        px, py = int(panel.x + 10), int(panel.y + 30)
        rl.gui_label(rl.Rectangle(px, py, 234, 14), status)

        def slider(i, label, key, lo, hi, fmt="{:.2f}"):
            y = py + 20 + i * 30
            rl.gui_label(rl.Rectangle(px, y, 234, 12), f"{label}  {fmt.format(params[key][0])}")
            rl.gui_slider_bar(rl.Rectangle(px, y + 13, 234, 13), "", "", params[key], lo, hi)

        slider(0, "size", "size", 0.3, 2.0)
        slider(1, "spike length", "spike_len", 0.0, 1.2)
        slider(2, "spike density", "spike_density", 0.05, 1.0)
        slider(3, "pulse speed", "pulse_speed", 0.0, 8.0)
        slider(4, "breathe amp", "breathe_amp", 0.0, 0.25)
        slider(5, "breathe speed", "breathe_speed", 0.0, 6.0)
        slider(6, "spin deg/s", "spin", 0.0, 120.0, "{:.0f}")
        slider(7, "line width", "line_width", 1.0, 6.0, "{:.1f}")
        slider(8, "pastel rim", "rim", 0.0, 1.5)
        slider(9, "red outline", "red_outline", 0.0, 0.15)
        slider(10, "subdiv", "subdiv", 0.0, 3.0, "{:.0f}")

        by = py + 20 + 11 * 30
        if rl.gui_button(rl.Rectangle(px, by, 113, 22), "reroll colors"):
            params["seed"][0] += 1.0
        if rl.gui_button(rl.Rectangle(px + 121, by, 113, 22), "save look"):
            save_look()
            status = f"saved {LOOK_FILE.name}"

        rl.draw_text("right drag: orbit   wheel: zoom", 12, SCREEN_H - 24, 18, rl.GRAY)
        rl.end_drawing()

        if rl.is_key_pressed(rl.KeyboardKey.KEY_S) or (shot_mode and frame_counter == 60):
            rl.take_screenshot("enemy_proto_shot.png")
            if shot_mode:
                break

    rl.close_window()


if __name__ == "__main__":
    main()
