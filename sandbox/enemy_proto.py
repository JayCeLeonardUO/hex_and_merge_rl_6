#!/usr/bin/env python3
"""Enemy look architect: a spiky, breathing icosphere with pastel wires.

    python3 enemy_proto.py          # interactive
    python3 enemy_proto.py --shot   # screenshot after a second, then exit

The enemy is an icosphere displaced entirely in the vertex shader: a stable
per-vertex hash picks which vertices spike, spikes pulse over time, and the
whole body breathes. It renders in two passes sharing that vertex shader:

  1. fill  -- near-black body with the pastel showing as a fresnel rim
  2. wires -- glPolygonMode(GL_LINE) pass; every vertex carries a pastel
              palette color baked into the GLB, so the mesh-defining lines
              interpolate pastel-to-pastel along every edge

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

ENEMY_VS = """
#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform float time;
uniform float yaw;          // spin lives here so world pos stays honest
uniform float spikeLen;
uniform float spikeDensity; // fraction of vertices that grow spikes
uniform float pulseSpeed;
uniform float breatheAmp;
uniform float breatheSpeed;
uniform float inflate;      // wire pass floats just off the fill surface
out vec4 fragColor;
out vec3 fragNormal;
out vec3 fragWorldPos;

float hash(vec3 p) { return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719)))*43758.5453); }

void main()
{
    float h = hash(vertexPosition);
    float mask = smoothstep(1.0 - spikeDensity, 1.0, h);
    float wob = 0.7 + 0.3*sin(time*pulseSpeed + h*6.2831);
    float breath = 1.0 + breatheAmp*sin(time*breatheSpeed);

    vec3 p = (vertexPosition + vertexNormal*(spikeLen*mask*wob + inflate))*breath;
    float c = cos(yaw); float s = sin(yaw);
    mat3 rot = mat3(c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c);
    p = rot*p;

    fragColor = vertexColor;
    fragNormal = rot*vertexNormal;
    fragWorldPos = p;
    gl_Position = mvp*vec4(p, 1.0);
}
"""

FILL_FS = """
#version 330
in vec4 fragColor;
in vec3 fragNormal;
in vec3 fragWorldPos;
uniform vec3 viewPos;
uniform float rim;   // how much pastel bleeds in at grazing angles
out vec4 finalColor;

void main()
{
    vec3 V = normalize(viewPos - fragWorldPos);
    float fres = pow(1.0 - clamp(dot(V, normalize(fragNormal)), 0.0, 1.0), 2.5);
    vec3 body = vec3(0.05, 0.05, 0.09);
    vec3 col = mix(body, fragColor.rgb, fres*rim);
    finalColor = vec4(col, 1.0);
}
"""

WIRE_FS = """
#version 330
in vec4 fragColor;
in vec3 fragNormal;
in vec3 fragWorldPos;
out vec4 finalColor;

void main() { finalColor = vec4(fragColor.rgb, 1.0); }
"""


def build_enemy_glb(subdiv, seed):
    """Icosphere with a pastel palette color per vertex, baked as COLOR_0."""
    rng = np.random.default_rng(seed)
    ico = trimesh.creation.icosphere(subdivisions=subdiv, radius=1.0)
    colors = np.array(PASTELS, np.uint8)[rng.integers(0, len(PASTELS), len(ico.vertices))]
    ico.visual = trimesh.visual.ColorVisuals(
        ico, vertex_colors=np.column_stack([colors, np.full(len(ico.vertices), 255, np.uint8)]))
    ASSETS.mkdir(parents=True, exist_ok=True)
    out = ASSETS / "enemy_ico.glb"
    ico.export(str(out), include_normals=True)
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

    fill_shader = rl.load_shader_from_memory(ENEMY_VS, FILL_FS)
    wire_shader = rl.load_shader_from_memory(ENEMY_VS, WIRE_FS)
    U = {}
    for sh, tag in ((fill_shader, "fill"), (wire_shader, "wire")):
        for name in ("time", "yaw", "spikeLen", "spikeDensity", "pulseSpeed",
                     "breatheAmp", "breatheSpeed", "inflate", "viewPos", "rim"):
            U[(tag, name)] = rl.get_shader_location(sh, name)

    params = {
        "size": ffi.new("float *", 0.9),
        "spike_len": ffi.new("float *", 0.35),
        "spike_density": ffi.new("float *", 0.45),
        "pulse_speed": ffi.new("float *", 2.2),
        "breathe_amp": ffi.new("float *", 0.06),
        "breathe_speed": ffi.new("float *", 1.6),
        "spin": ffi.new("float *", 18.0),
        "line_width": ffi.new("float *", 2.0),
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

    panel = rl.Rectangle(636, 12, 254, 470)
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

        # shared uniforms for both passes
        for tag, sh in (("fill", fill_shader), ("wire", wire_shader)):
            set_f(sh, U[(tag, "time")], t)
            set_f(sh, U[(tag, "yaw")], yaw)
            set_f(sh, U[(tag, "spikeLen")], params["spike_len"][0])
            set_f(sh, U[(tag, "spikeDensity")], params["spike_density"][0])
            set_f(sh, U[(tag, "pulseSpeed")], params["pulse_speed"][0])
            set_f(sh, U[(tag, "breatheAmp")], params["breathe_amp"][0])
            set_f(sh, U[(tag, "breatheSpeed")], params["breathe_speed"][0])
        set_f(fill_shader, U[("fill", "inflate")], 0.0)
        set_f(wire_shader, U[("wire", "inflate")], 0.012)
        set_f(fill_shader, U[("fill", "rim")], params["rim"][0])
        rl.set_shader_value(fill_shader, U[("fill", "viewPos")],
                            ffi.new("float[3]", [cam.position.x, cam.position.y, cam.position.z]),
                            rl.ShaderUniformDataType.SHADER_UNIFORM_VEC3)

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

        model.materials[0].shader = fill_shader
        rl.draw_model(model, pos, size, rl.WHITE)

        model.materials[0].shader = wire_shader
        rl.rl_set_line_width(params["line_width"][0])
        rl.rl_enable_wire_mode()
        rl.draw_model(model, pos, size, rl.WHITE)
        rl.rl_disable_wire_mode()
        rl.rl_set_line_width(1.0)

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
        slider(9, "subdiv", "subdiv", 0.0, 3.0, "{:.0f}")

        by = py + 20 + 10 * 30
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
