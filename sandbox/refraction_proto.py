#!/usr/bin/env python3
"""Refraction sandbox: can raylib do a clear-quartz gem?

    python3 refraction_proto.py          # interactive
    python3 refraction_proto.py --shot   # save refraction_shot.png and exit

Technique (screen-space refraction, the standard rasterizer fake):
  1. Render the background scene into a render texture, without the gem.
  2. Draw the gem mesh with a shader that samples that scene texture, with the
     sample point pushed along the refracted view ray (GLSL refract()) -- so
     each facet bends whatever is behind it. Chromatic dispersion = three
     samples at slightly different IORs; fresnel adds the glassy rim.

The gem is a faceted crystal generated with trimesh (convex hull of a
jittered icosphere, vertices unmerged so every facet is flat).

Controls: wheel zooms, left/right spins the gem, sliders tune the optics.
"""

import math
import sys
from pathlib import Path

import numpy as np
import pyray as rl
import trimesh
from raylib import ffi

SCRATCH = Path(__file__).parent / "export"
SCREEN_W, SCREEN_H = 900, 620

GEM_VS = """
#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPosition;
out vec3 fragNormal;

void main()
{
    fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));
    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 0.0)));
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
"""

# texture0 is the scene render texture (bound through the albedo material map)
GEM_FS = """
#version 330
in vec3 fragPosition;
in vec3 fragNormal;
uniform sampler2D texture0;
uniform vec3 viewPos;
uniform vec3 camRight;
uniform vec3 camUp;
uniform vec2 resolution;
uniform float ior;
uniform float strength;
uniform float chromatic;
uniform float fresnelPow;
uniform vec4 tint;
uniform int showNormals;  // debug: paint the incoming mesh normals
uniform int invertColors; // the inverted gem: transmits the scene as its negative
out vec4 finalColor;

vec3 refrSample(vec2 baseUV, vec3 I, vec3 N, float eta)
{
    vec3 R = refract(I, N, eta);
    // project the bent ray onto the screen axes: how far to shift the lookup
    vec2 off = vec2(dot(R, camRight), dot(R, camUp))*strength;
    return texture(texture0, clamp(baseUV + off, vec2(0.002), vec2(0.998))).rgb;
}

void main()
{
    vec3 N = normalize(fragNormal);
    if (showNormals == 1)
    {
        finalColor = vec4(N*0.5 + 0.5, 1.0); // one flat color per facet = mesh normals arriving
        return;
    }
    vec3 I = normalize(fragPosition - viewPos);
    if (dot(N, I) > 0.0) N = -N; // stay sane on back faces

    // gl_FragCoord and the scene render texture share the GL bottom-left
    // origin, so the un-refracted lookup is just this fragment's position
    vec2 baseUV = gl_FragCoord.xy/resolution;

    float eta = 1.0/ior;
    vec3 col;
    col.r = refrSample(baseUV, I, N, eta*(1.0 - chromatic)).r;
    col.g = refrSample(baseUV, I, N, eta).g;
    col.b = refrSample(baseUV, I, N, eta*(1.0 + chromatic)).b;

    if (invertColors == 1) col = vec3(1.0) - col; // negative-image transmission

    // glassy rim, a cold quartz tint, and a per-facet specular glint
    float fres = pow(1.0 - max(dot(-I, N), 0.0), fresnelPow);
    col = mix(col*1.15, tint.rgb, tint.a);
    col += vec3(0.85, 0.9, 1.0)*fres;
    vec3 L = normalize(vec3(0.5, 1.0, 0.35));
    col += vec3(1.0)*pow(max(dot(reflect(-L, N), -I), 0.0), 48.0)*0.7;

    finalColor = vec4(col, 1.0);
}
"""


def make_gem_glb(path, seed=7):
    """A quartz-ish crystal: an icosphere with radially jittered vertices,
    elongated along Y, vertices unmerged so every facet keeps its own flat
    normal (no convex hull -- that would pull in scipy)."""
    rng = np.random.default_rng(seed)
    base = trimesh.creation.icosphere(subdivisions=1, radius=1.0)
    verts = base.vertices.copy()
    verts *= 1.0 + 0.28 * rng.random(len(verts))[:, None]
    verts[:, 1] *= 1.4  # stretch along Y like a quartz point
    gem = trimesh.Trimesh(vertices=verts, faces=base.faces, process=False)
    gem.unmerge_vertices()
    path.parent.mkdir(parents=True, exist_ok=True)
    # include_normals matters: without it trimesh omits the NORMAL accessor
    # and raylib feeds the shader a constant default normal instead
    gem.export(str(path), include_normals=True)
    return path


# Separable gaussian blur for the triangle glow: run once horizontally into a
# scratch buffer, once vertically while compositing additively onto the frame
BLUR_FS = """
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 dir; // one texel step: (s/w, 0) then (0, s/h)
out vec4 finalColor;

void main()
{
    float w[5] = float[](0.227027, 0.194594, 0.121621, 0.054054, 0.016216);
    vec3 c = texture(texture0, fragTexCoord).rgb*w[0];
    for (int i = 1; i < 5; i++)
    {
        c += texture(texture0, fragTexCoord + dir*float(i)).rgb*w[i];
        c += texture(texture0, fragTexCoord - dir*float(i)).rgb*w[i];
    }
    finalColor = vec4(c, 1.0)*fragColor;
}
"""

# Aurora: procedural curtains on a big flat plane in the sky. Three noise-
# driven layers, each a wavy baseline with an upward feather, modulated by
# vertical rays; green at the base shading to purple up high. Drawn additive.
AURORA_FS = """
#version 330
in vec2 fragTexCoord;
uniform float time;
uniform float intensity;
out vec4 finalColor;

float hash(float n) { return fract(sin(n)*43758.5453); }

float noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f*f*(3.0 - 2.0*f);
    float a = hash(i.x + i.y*57.0);
    float b = hash(i.x + 1.0 + i.y*57.0);
    float c = hash(i.x + (i.y + 1.0)*57.0);
    float d = hash(i.x + 1.0 + (i.y + 1.0)*57.0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main()
{
    vec2 uv = fragTexCoord; // x across the sky, y = 0 bottom .. 1 top
    vec3 col = vec3(0.0);

    for (int i = 0; i < 3; i++)
    {
        float fi = float(i);
        // wavy curtain baseline drifting sideways
        float wave = noise(vec2(uv.x*3.0 + fi*7.3 + time*(0.05 + fi*0.03),
                                time*0.1 + fi*3.1));
        float base = 0.20 + 0.16*fi + (wave - 0.5)*0.35;
        float d = uv.y - base;
        // bright at the baseline, feathering upward, cut off below
        float curtain = exp(-max(d, 0.0)*(7.0 - fi*1.5))
                      * exp(-max(-d, 0.0)*30.0);
        // vertical rays shimmering along x
        float rays = 0.55 + 0.45*noise(vec2(uv.x*26.0 + fi*13.0, time*(0.35 + 0.1*fi)));
        vec3 tint = mix(vec3(0.10, 0.95, 0.45), vec3(0.45, 0.25, 0.95),
                        clamp(d*2.2 + fi*0.2, 0.0, 1.0));
        col += tint*curtain*rays;
    }

    col *= intensity;
    finalColor = vec4(col, clamp(max(col.r, max(col.g, col.b)), 0.0, 1.0));
}
"""

RL_TRIANGLES = 0x0004

# ReFantazio-ish palette: royal blues and cyans with white/gold sparks
TRI_COLORS = [(70, 130, 255, 255), (130, 220, 255, 255),
              (255, 255, 255, 255), (255, 215, 130, 255)]


def make_tri_particles(count, rng):
    """Flat triangles drifting in the air; tumble/drift params like the shards."""
    parts = []
    for _ in range(count):
        axis = rng.normal(size=3)
        axis /= np.linalg.norm(axis)
        parts.append({
            "base": (float(rng.uniform(-5.0, 5.0)), float(rng.uniform(0.4, 4.6)),
                     float(rng.uniform(-4.5, 3.0))),
            "phase": float(rng.uniform(0.0, math.tau)),
            "bob": float(rng.uniform(0.15, 0.6)),
            "speed": float(rng.uniform(0.25, 1.0)),
            "axis": tuple(float(a) for a in axis),
            "spin": float(rng.uniform(20.0, 90.0)),
            "size": float(rng.uniform(0.06, 0.16)),
            "color": TRI_COLORS[int(rng.integers(0, len(TRI_COLORS)))],
            "off": [0.0, 0.0, 0.0],
        })
    return parts


RL_QUADS = 0x0007


def draw_aurora_plane(shader, t_loc, i_loc, t, intensity):
    """The aurora shader on a flat plane hung across the sky (z = -10, behind
    the wall). Additive, so it layers over the dark sky like real airglow."""
    set_f(shader, t_loc, t)
    set_f(shader, i_loc, intensity)
    rl.begin_shader_mode(shader)
    rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
    rl.rl_begin(RL_QUADS)
    rl.rl_color4ub(255, 255, 255, 255)
    rl.rl_normal3f(0.0, 0.0, 1.0)
    rl.rl_tex_coord2f(0.0, 1.0)
    rl.rl_vertex3f(-18.0, 14.0, -10.0)
    rl.rl_tex_coord2f(0.0, 0.0)
    rl.rl_vertex3f(-18.0, 1.0, -10.0)
    rl.rl_tex_coord2f(1.0, 0.0)
    rl.rl_vertex3f(18.0, 1.0, -10.0)
    rl.rl_tex_coord2f(1.0, 1.0)
    rl.rl_vertex3f(18.0, 14.0, -10.0)
    rl.rl_end()
    rl.end_blend_mode()
    rl.end_shader_mode()


def update_reactivity(parts, count, mouse_world, dt):
    """Slight mouse reactivity: particles near the cursor's world point get a
    soft push away, eased both ways so they dodge and drift back. Runs once
    per frame per particle set; the draw passes just add the stored offset."""
    radius = 1.8
    for k in range(count):
        p = parts[k]
        off = p["off"]
        dx = p["base"][0] + off[0] - mouse_world[0]
        dy = p["base"][1] + off[1] - mouse_world[1]
        dz = p["base"][2] + off[2] - mouse_world[2]
        d2 = dx * dx + dy * dy + dz * dz

        tx = ty = tz = 0.0
        if 1e-6 < d2 < radius * radius:
            d = math.sqrt(d2)
            push = (radius - d) / radius * 0.6
            tx, ty, tz = dx / d * push, dy / d * push, dz / d * push

        ease = 1.0 - math.exp(-6.0 * dt)
        off[0] += (tx - off[0]) * ease
        off[1] += (ty - off[1]) * ease
        off[2] += (tz - off[2]) * ease


def draw_tri_particles(parts, count, t, size_mult):
    """Immediate-mode flat triangles in the 3D scene (double-sided)."""
    rl.rl_disable_backface_culling()
    for k in range(count):
        p = parts[k]
        x = p["base"][0] + math.sin(t * 0.25 + p["phase"]) * 0.5 + p["off"][0]
        y = p["base"][1] + math.sin(t * p["speed"] + p["phase"]) * p["bob"] + p["off"][1]
        z = p["base"][2] + math.cos(t * 0.2 + p["phase"] * 1.3) * 0.35 + p["off"][2]
        s = p["size"] * size_mult

        rl.rl_push_matrix()
        rl.rl_translatef(x, y, z)
        rl.rl_rotatef(t * p["spin"] + p["phase"] * 57.3,
                      p["axis"][0], p["axis"][1], p["axis"][2])
        rl.rl_begin(RL_TRIANGLES)
        rl.rl_color4ub(*p["color"])
        rl.rl_vertex3f(0.0, s, 0.0)
        rl.rl_vertex3f(-s * 0.87, -s * 0.5, 0.0)
        rl.rl_vertex3f(s * 0.87, -s * 0.5, 0.0)
        rl.rl_end()
        rl.rl_pop_matrix()
    rl.rl_enable_backface_culling()


def make_shard_glb(path, seed):
    """A small crystal shard: jittered icosahedron (20 facets), elongated."""
    rng = np.random.default_rng(seed)
    base = trimesh.creation.icosphere(subdivisions=0, radius=1.0)
    verts = base.vertices.copy()
    verts *= 1.0 + 0.35 * rng.random(len(verts))[:, None]
    verts[:, 1] *= 1.0 + rng.uniform(0.3, 1.1)
    shard = trimesh.Trimesh(vertices=verts, faces=base.faces, process=False)
    shard.unmerge_vertices()
    path.parent.mkdir(parents=True, exist_ok=True)
    shard.export(str(path), include_normals=True)
    return path


def make_particles(count, rng):
    """Floating shard particles: a base position in the air plus drift, bob,
    and tumble parameters, all integrated from time in the draw loop."""
    parts = []
    for _ in range(count):
        axis = rng.normal(size=3)
        axis /= np.linalg.norm(axis)
        parts.append({
            "base": (float(rng.uniform(-4.5, 4.5)), float(rng.uniform(0.5, 4.4)),
                     float(rng.uniform(-4.0, 2.8))),
            "phase": float(rng.uniform(0.0, math.tau)),
            "bob": float(rng.uniform(0.1, 0.5)),
            "speed": float(rng.uniform(0.3, 1.2)),
            "axis": tuple(float(a) for a in axis),
            "spin": float(rng.uniform(15.0, 80.0)),
            "scale": float(rng.uniform(0.05, 0.17)),
            "variant": int(rng.integers(0, 3)),
            "off": [0.0, 0.0, 0.0],
        })
    return parts


def set_f(shader, loc, v):
    rl.set_shader_value(shader, loc, ffi.new("float *", v),
                        rl.ShaderUniformDataType.SHADER_UNIFORM_FLOAT)


def set_v2(shader, loc, x, y):
    rl.set_shader_value(shader, loc, ffi.new("float[2]", [x, y]),
                        rl.ShaderUniformDataType.SHADER_UNIFORM_VEC2)


def set_v3(shader, loc, x, y, z):
    rl.set_shader_value(shader, loc, ffi.new("float[3]", [x, y, z]),
                        rl.ShaderUniformDataType.SHADER_UNIFORM_VEC3)


def set_v4(shader, loc, rgba):
    rl.set_shader_value(shader, loc, ffi.new("float[4]", list(rgba)),
                        rl.ShaderUniformDataType.SHADER_UNIFORM_VEC4)


def draw_backdrop(t):
    """Colorful things for the gem to bend: checker floor, pillars, a wanderer."""
    for x in range(-6, 7):
        for z in range(-6, 7):
            c = rl.Color(70, 70, 78, 255) if (x + z) % 2 == 0 else rl.Color(40, 40, 46, 255)
            rl.draw_plane(rl.Vector3(x, 0.0, z), rl.Vector2(1, 1), c)

    # striped back wall so the gem always has color behind it to bend; short
    # enough that the aurora sky shows above it
    stripe_colors = [rl.RED, rl.GOLD, rl.LIME, rl.SKYBLUE, rl.PINK, rl.VIOLET, rl.ORANGE]
    for i in range(-6, 7):
        c = stripe_colors[(i + 6) % len(stripe_colors)]
        rl.draw_cube(rl.Vector3(i * 1.0, 1.5, -5.5), 1.0, 3.0, 0.4, c)

    pillars = [(-2.5, -2.0, rl.RED), (-0.8, -2.6, rl.GOLD), (0.9, -2.4, rl.LIME),
               (2.4, -1.8, rl.SKYBLUE), (3.2, 0.2, rl.PINK), (-3.3, 0.4, rl.VIOLET)]
    for i, (x, z, color) in enumerate(pillars):
        h = 2.2 + (i % 3) * 0.8
        rl.draw_cube(rl.Vector3(x, h / 2, z), 0.6, h, 0.6, color)
        rl.draw_cube_wires(rl.Vector3(x, h / 2, z), 0.6, h, 0.6, rl.Color(20, 20, 20, 255))

    # something moving, so the refraction visibly swims
    wx = 2.6 * math.sin(t * 0.7)
    rl.draw_sphere(rl.Vector3(wx, 0.5, -1.2), 0.4, rl.ORANGE)


def main():
    shot_mode = "--shot" in sys.argv

    rl.init_window(SCREEN_W, SCREEN_H, "refraction proto")
    rl.set_target_fps(60)

    cam = rl.Camera3D(rl.Vector3(0.0, 2.6, 6.5), rl.Vector3(0.0, 1.3, 0.0),
                      rl.Vector3(0.0, 1.0, 0.0), 45.0,
                      rl.CameraProjection.CAMERA_PERSPECTIVE)

    gem_path = SCRATCH / "gem.glb"
    make_gem_glb(gem_path)
    gem = rl.load_model(str(gem_path))

    gem2_path = SCRATCH / "gem_inverted.glb"
    make_gem_glb(gem2_path, seed=23)
    gem2 = rl.load_model(str(gem2_path))

    shader = rl.load_shader_from_memory(GEM_VS, GEM_FS)
    locs = {name: rl.get_shader_location(shader, name)
            for name in ("viewPos", "camRight", "camUp", "resolution",
                         "ior", "strength", "chromatic", "fresnelPow", "tint",
                         "showNormals", "invertColors")}
    invert_ptr = ffi.new("int *", 0)

    def set_invert(on):
        invert_ptr[0] = 1 if on else 0
        rl.set_shader_value(shader, locs["invertColors"], invert_ptr,
                            rl.ShaderUniformDataType.SHADER_UNIFORM_INT)
    gem.materials[0].shader = shader

    scene_rt = rl.load_render_texture(SCREEN_W, SCREEN_H)

    # the shader reads the scene through texture0 = the albedo material map
    gem.materials[0].maps[rl.MaterialMapIndex.MATERIAL_MAP_ALBEDO].texture = scene_rt.texture
    gem2.materials[0].shader = shader
    gem2.materials[0].maps[rl.MaterialMapIndex.MATERIAL_MAP_ALBEDO].texture = scene_rt.texture

    # shard particle models: three variants sharing the same refraction setup
    shards = []
    for k in range(3):
        shard_path = SCRATCH / f"shard{k}.glb"
        make_shard_glb(shard_path, seed=11 + k)
        shard = rl.load_model(str(shard_path))
        shard.materials[0].shader = shader
        shard.materials[0].maps[rl.MaterialMapIndex.MATERIAL_MAP_ALBEDO].texture = scene_rt.texture
        shards.append(shard)

    MAX_PARTICLES = 120
    particles = make_particles(MAX_PARTICLES, np.random.default_rng(3))
    count_ptr = ffi.new("float *", 60.0)
    psize_ptr = ffi.new("float *", 1.0)

    # Triangle glow: half-res mask + blur scratch buffer, blur shader
    MAX_TRIS = 80
    tris = make_tri_particles(MAX_TRIS, np.random.default_rng(9))
    tri_ptr = ffi.new("float *", 40.0)
    glow_ptr = ffi.new("float *", 0.9)
    gw, gh = SCREEN_W // 2, SCREEN_H // 2
    glow_rt = rl.load_render_texture(gw, gh)
    blur_rt = rl.load_render_texture(gw, gh)
    rl.set_texture_filter(glow_rt.texture, rl.TextureFilter.TEXTURE_FILTER_BILINEAR)
    rl.set_texture_filter(blur_rt.texture, rl.TextureFilter.TEXTURE_FILTER_BILINEAR)
    blur_shader = rl.load_shader_from_memory(ffi.NULL, BLUR_FS)
    blur_dir_loc = rl.get_shader_location(blur_shader, "dir")

    aurora_shader = rl.load_shader_from_memory(ffi.NULL, AURORA_FS)
    aurora_time_loc = rl.get_shader_location(aurora_shader, "time")
    aurora_intensity_loc = rl.get_shader_location(aurora_shader, "intensity")
    aurora_ptr = ffi.new("float *", 0.9)

    # live optics (raygui sliders write these pointers)
    ior_ptr = ffi.new("float *", 1.55)       # quartz
    strength_ptr = ffi.new("float *", 0.5)   # screen-space shift scale
    chroma_ptr = ffi.new("float *", 0.04)    # per-channel IOR spread
    fresnel_ptr = ffi.new("float *", 4.0)
    tint_ptr = ffi.new("float *", 0.10)      # how milky the quartz is

    panel = rl.Rectangle(652, 36, 236, 400)
    spin = 0.0
    frame = 0
    show_normals = False
    normals_ptr = ffi.new("int *", 0)
    cam_base = [0.0, 2.6, 6.5]  # wheel zoom scales this; mouse sway offsets it
    sway = [0.0, 0.0]

    while not rl.window_should_close():
        frame += 1
        t = rl.get_time()
        dt = rl.get_frame_time()

        if rl.is_key_down(rl.KeyboardKey.KEY_RIGHT):
            spin += 60.0 * dt
        if rl.is_key_down(rl.KeyboardKey.KEY_LEFT):
            spin -= 60.0 * dt
        spin += 8.0 * dt  # lazy idle turn

        zoom = rl.get_mouse_wheel_move()
        if zoom != 0.0:
            d = 1.0 - zoom * 0.08
            cam_base[0] *= d
            cam_base[1] = 1.3 + (cam_base[1] - 1.3) * d
            cam_base[2] *= d

        # Slight mouse reactivity: the camera sways a touch with the cursor,
        # and particles near the cursor's world point dodge out of the way
        m = rl.get_mouse_position()
        nx, ny = (m.x / SCREEN_W) * 2.0 - 1.0, (m.y / SCREEN_H) * 2.0 - 1.0
        sway[0] += (nx - sway[0]) * 0.05
        sway[1] += (ny - sway[1]) * 0.05
        cam.position = rl.Vector3(cam_base[0] + sway[0] * 0.35,
                                  cam_base[1] - sway[1] * 0.25, cam_base[2])

        mray = rl.get_screen_to_world_ray(m, cam)
        mouse_world = (mray.position.x + mray.direction.x * 7.5,
                       mray.position.y + mray.direction.y * 7.5,
                       mray.position.z + mray.direction.z * 7.5)
        update_reactivity(tris, int(tri_ptr[0]), mouse_world, dt)
        update_reactivity(particles, int(count_ptr[0]), mouse_world, dt)

        # Pass 1: the world without the gem -- this is what the gem will bend.
        # The flat triangles live here too, so the crystals refract them.
        rl.begin_texture_mode(scene_rt)
        rl.clear_background(rl.Color(12, 12, 18, 255))
        rl.begin_mode_3d(cam)
        draw_aurora_plane(aurora_shader, aurora_time_loc, aurora_intensity_loc, t, aurora_ptr[0])
        draw_backdrop(t)
        draw_tri_particles(tris, int(tri_ptr[0]), t, 1.0)
        rl.end_mode_3d()
        rl.end_texture_mode()

        # Glow mask: only the triangles, on black -- whatever lands in this
        # buffer is what blooms in the post step (no lights involved)
        rl.begin_texture_mode(glow_rt)
        rl.clear_background(rl.BLACK)
        rl.begin_mode_3d(cam)
        draw_tri_particles(tris, int(tri_ptr[0]), t, 1.0)
        rl.end_mode_3d()
        rl.end_texture_mode()

        # Blur, horizontal leg into the scratch buffer
        rl.begin_texture_mode(blur_rt)
        rl.clear_background(rl.BLACK)
        rl.begin_shader_mode(blur_shader)
        rl.set_shader_value(blur_shader, blur_dir_loc, ffi.new("float[2]", [1.6 / gw, 0.0]),
                            rl.ShaderUniformDataType.SHADER_UNIFORM_VEC2)
        rl.draw_texture_pro(glow_rt.texture, rl.Rectangle(0, 0, gw, -gh),
                            rl.Rectangle(0, 0, gw, gh), rl.Vector2(0, 0), 0.0, rl.WHITE)
        rl.end_shader_mode()
        rl.end_texture_mode()

        # camera basis for projecting refracted rays into screen offsets
        forward = rl.vector3_normalize(rl.vector3_subtract(cam.target, cam.position))
        right = rl.vector3_normalize(rl.vector3_cross_product(forward, cam.up))
        up = rl.vector3_cross_product(right, forward)

        set_v3(shader, locs["viewPos"], cam.position.x, cam.position.y, cam.position.z)
        set_v3(shader, locs["camRight"], right.x, right.y, right.z)
        set_v3(shader, locs["camUp"], up.x, up.y, up.z)
        set_v2(shader, locs["resolution"], SCREEN_W, SCREEN_H)
        set_f(shader, locs["ior"], ior_ptr[0])
        set_f(shader, locs["strength"], strength_ptr[0])
        set_f(shader, locs["chromatic"], chroma_ptr[0])
        set_f(shader, locs["fresnelPow"], fresnel_ptr[0])
        set_v4(shader, locs["tint"], (0.85, 0.92, 1.0, tint_ptr[0]))
        if rl.is_key_pressed(rl.KeyboardKey.KEY_N):
            show_normals = not show_normals
        normals_ptr[0] = 1 if show_normals else 0
        rl.set_shader_value(shader, locs["showNormals"], normals_ptr,
                            rl.ShaderUniformDataType.SHADER_UNIFORM_INT)

        # Pass 2: the same scene on screen, gem on top refracting pass 1
        rl.begin_drawing()
        rl.clear_background(rl.Color(12, 12, 18, 255))
        rl.draw_texture_pro(scene_rt.texture,
                            rl.Rectangle(0, 0, SCREEN_W, -SCREEN_H),
                            rl.Rectangle(0, 0, SCREEN_W, SCREEN_H),
                            rl.Vector2(0, 0), 0.0, rl.WHITE)

        rl.begin_mode_3d(cam)
        set_invert(False)
        rl.draw_model_ex(gem, rl.Vector3(0.0, 1.5, 0.0), rl.Vector3(0, 1, 0),
                         spin, rl.Vector3(0.9, 0.9, 0.9), rl.WHITE)

        # the inverted gem: same refraction, but it transmits the negative --
        # floated up into the sky so it works against the aurora curtains
        set_invert(True)
        rl.draw_model_ex(gem2, rl.Vector3(-2.6, 3.8, -2.0), rl.Vector3(0, 1, 0),
                         -spin * 1.3, rl.Vector3(0.7, 0.7, 0.7), rl.WHITE)
        set_invert(False)

        # shard particles drifting in the air, all refracting the scene
        for k in range(int(count_ptr[0])):
            p = particles[k]
            x = p["base"][0] + math.sin(t * 0.2 + p["phase"]) * 0.4 + p["off"][0]
            y = p["base"][1] + math.sin(t * p["speed"] + p["phase"]) * p["bob"] + p["off"][1]
            z = p["base"][2] + math.cos(t * 0.17 + p["phase"] * 1.7) * 0.3 + p["off"][2]
            angle = t * p["spin"] + p["phase"] * 57.3
            s = p["scale"] * psize_ptr[0]
            rl.draw_model_ex(shards[p["variant"]], rl.Vector3(x, y, z),
                             rl.Vector3(p["axis"][0], p["axis"][1], p["axis"][2]),
                             angle, rl.Vector3(s, s, s), rl.WHITE)
        rl.end_mode_3d()

        # Post: vertical blur leg composited additively -- the glow blooms
        # wherever the triangles landed on screen
        rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
        rl.begin_shader_mode(blur_shader)
        rl.set_shader_value(blur_shader, blur_dir_loc, ffi.new("float[2]", [0.0, 1.6 / gh]),
                            rl.ShaderUniformDataType.SHADER_UNIFORM_VEC2)
        rl.draw_texture_pro(blur_rt.texture, rl.Rectangle(0, 0, gw, -gh),
                            rl.Rectangle(0, 0, SCREEN_W, SCREEN_H), rl.Vector2(0, 0), 0.0,
                            rl.fade(rl.WHITE, min(glow_ptr[0], 1.0)))
        rl.end_shader_mode()
        rl.end_blend_mode()

        # optics panel
        rl.gui_window_box(panel, "quartz optics")
        px, py = int(panel.x + 10), int(panel.y + 30)
        rl.gui_label(rl.Rectangle(px, py, 216, 14), f"ior  {ior_ptr[0]:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 14, 216, 14), "", "", ior_ptr, 1.0, 2.4)
        rl.gui_label(rl.Rectangle(px, py + 36, 216, 14), f"strength  {strength_ptr[0]:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 50, 216, 14), "", "", strength_ptr, 0.0, 1.0)
        rl.gui_label(rl.Rectangle(px, py + 72, 216, 14), f"dispersion  {chroma_ptr[0]:.3f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 86, 216, 14), "", "", chroma_ptr, 0.0, 0.15)
        rl.gui_label(rl.Rectangle(px, py + 108, 216, 14), f"fresnel  {fresnel_ptr[0]:.1f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 122, 216, 14), "", "", fresnel_ptr, 1.0, 8.0)
        rl.gui_label(rl.Rectangle(px, py + 144, 216, 14), f"milkiness  {tint_ptr[0]:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 158, 216, 14), "", "", tint_ptr, 0.0, 0.8)
        rl.gui_label(rl.Rectangle(px, py + 180, 216, 14), f"particles  {int(count_ptr[0])}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 194, 216, 14), "", "", count_ptr, 0.0, float(MAX_PARTICLES))
        rl.gui_label(rl.Rectangle(px, py + 216, 216, 14), f"particle size  {psize_ptr[0]:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 230, 216, 14), "", "", psize_ptr, 0.3, 2.5)
        rl.gui_label(rl.Rectangle(px, py + 252, 216, 14), f"triangles  {int(tri_ptr[0])}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 266, 216, 14), "", "", tri_ptr, 0.0, float(MAX_TRIS))
        rl.gui_label(rl.Rectangle(px, py + 288, 216, 14), f"glow  {glow_ptr[0]:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 302, 216, 14), "", "", glow_ptr, 0.0, 1.0)
        rl.gui_label(rl.Rectangle(px, py + 324, 216, 14), f"aurora  {aurora_ptr[0]:.2f}")
        rl.gui_slider_bar(rl.Rectangle(px, py + 338, 216, 14), "", "", aurora_ptr, 0.0, 2.0)

        rl.draw_text("wheel: zoom   left/right: spin gem   n: show normals", 16, SCREEN_H - 28, 20, rl.GRAY)
        rl.end_drawing()

        if rl.is_key_pressed(rl.KeyboardKey.KEY_S) or (shot_mode and frame == 50):
            rl.take_screenshot("refraction_shot.png")
            if shot_mode:
                show_normals = True  # second shot proves the mesh normals arrive
        if shot_mode and frame == 55:
            rl.take_screenshot("refraction_normals_shot.png")
            break

    rl.close_window()


if __name__ == "__main__":
    main()
