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

import json
import math
import sys
from pathlib import Path

import numpy as np
import pyray as rl
import trimesh
from raylib import ffi

from enemy_proto import ENEMY_VS, ENEMY_FS, OUTLINE_FS, build_enemy_glb
from enemy_proto import LOOK_FILE as ENEMY_LOOK_FILE

SCRATCH = Path(__file__).parent / "export"
SCREEN_W, SCREEN_H = 630, 500 # the itch cover gif aspect: what you see is what exports

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

# Water: a plane that mirrors the sky. Wave noise perturbs the normal, the
# eye ray reflects off it, and the reflected ray is intersected with the same
# aurora sky plane the demo hangs at z = -10 -- so the water shows a true
# planar reflection of the exact curtains above it. Fresnel fades the mirror
# into a deep-water teal when looking straight down; glints ride wave crests.
WATER_FS = """
#version 330
in vec3 fragPosition;
in vec3 fragNormal;
uniform vec3 viewPos;
uniform float time;
uniform float intensity;   // aurora brightness, kept in sync with the sky
uniform float waveAmp;
uniform float waveScale;
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

// same curtain math as AURORA_FS, so the reflection matches the sky
vec3 aurora(vec2 uv)
{
    vec3 col = vec3(0.0);
    for (int i = 0; i < 3; i++)
    {
        float fi = float(i);
        float wave = noise(vec2(uv.x*3.0 + fi*7.3 + time*(0.05 + fi*0.03),
                                time*0.1 + fi*3.1));
        float base = 0.20 + 0.16*fi + (wave - 0.5)*0.35;
        float d = uv.y - base;
        float curtain = exp(-max(d, 0.0)*(7.0 - fi*1.5))
                      * exp(-max(-d, 0.0)*30.0);
        float rays = 0.55 + 0.45*noise(vec2(uv.x*26.0 + fi*13.0, time*(0.35 + 0.1*fi)));
        vec3 tint = mix(vec3(0.10, 0.95, 0.45), vec3(0.45, 0.25, 0.95),
                        clamp(d*2.2 + fi*0.2, 0.0, 1.0));
        col += tint*curtain*rays;
    }
    return col*intensity;
}

// what a ray leaving the water sees: night gradient, stars, aurora curtains
vec3 skyColor(vec3 org, vec3 dir)
{
    float up = clamp(dir.y, 0.0, 1.0);
    vec3 col = mix(vec3(0.10, 0.10, 0.16), vec3(0.03, 0.03, 0.07), up);

    // stars: hash the direction on a coarse dome grid
    vec2 sp = dir.xz/(dir.y + 0.25);
    float star = hash(floor(sp.x*60.0) + floor(sp.y*60.0)*91.7);
    col += vec3(smoothstep(0.995, 1.0, star))*up;

    // the aurora hangs on the plane z = -10, x -18..18, y 1..14
    if (dir.z < -0.001)
    {
        float tt = (-10.0 - org.z)/dir.z;
        vec3 hit = org + dir*tt;
        vec2 uv = vec2((hit.x + 18.0)/36.0, (hit.y - 1.0)/13.0);
        if (tt > 0.0 && uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0)
            col += aurora(uv);
    }
    return col;
}

float waveHeight(vec2 xz)
{
    // two drifting octaves; the reflection wobble does the visual work
    return noise(xz*waveScale + vec2(time*0.35, time*0.22))
         + 0.5*noise(xz*waveScale*2.7 - vec2(time*0.28, time*0.4));
}

void main()
{
    vec2 xz = fragPosition.xz;
    float e = 0.18;
    float hC = waveHeight(xz);
    float hX = waveHeight(xz + vec2(e, 0.0));
    float hZ = waveHeight(xz + vec2(0.0, e));
    vec3 N = normalize(vec3(-(hX - hC)/e*waveAmp, 1.0, -(hZ - hC)/e*waveAmp));

    vec3 I = normalize(fragPosition - viewPos);
    vec3 R = reflect(I, N);
    R.y = abs(R.y);  // waves never reflect below the horizon

    vec3 sky = skyColor(fragPosition, R);
    vec3 deep = vec3(0.02, 0.07, 0.09);

    float fres = pow(1.0 - max(dot(-I, N), 0.0), 3.0);
    vec3 col = mix(deep, sky, 0.25 + 0.75*fres);

    // crest glints: bright pinpricks where the wave slope peaks toward the eye
    float glint = pow(max(dot(R, normalize(vec3(0.3, 0.5, -1.0))), 0.0), 60.0);
    col += vec3(0.7, 0.9, 1.0)*glint*0.6;

    finalColor = vec4(col, 1.0);
}
"""

RL_TRIANGLES = 0x0004

# ReFantazio-ish palette: royal blues and cyans with white/gold sparks
TRI_COLORS = [(70, 130, 255, 255), (130, 220, 255, 255),
              (255, 255, 255, 255), (255, 215, 130, 255)]


def frustum_base(rng, cam):
    """A spawn point inside the camera frustum: pick a screen position and a
    depth along the view ray. A fifth of them hug the lens -- big foreground
    shards drifting right past the camera."""
    pos = np.array([cam.position.x, cam.position.y, cam.position.z])
    tgt = np.array([cam.target.x, cam.target.y, cam.target.z])
    fwd = tgt - pos
    fwd /= np.linalg.norm(fwd)
    right = np.cross(fwd, [0.0, 1.0, 0.0])
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)

    if rng.random() < 0.22:
        depth = rng.uniform(0.9, 2.2)   # really close to the camera
    else:
        depth = rng.uniform(2.5, 13.0)  # spread through the scene

    half_h = math.tan(math.radians(cam.fovy / 2.0)) * depth
    half_w = half_h * (SCREEN_W / SCREEN_H)
    p = (pos + fwd * depth
         + right * (rng.uniform(-0.92, 0.92) * half_w)
         + up * (rng.uniform(-0.85, 0.9) * half_h))
    p[1] = max(p[1], 0.2)  # stay off the floor
    return (float(p[0]), float(p[1]), float(p[2]))


def make_tri_particles(count, rng, cam):
    """Flat triangles drifting in the air; tumble/drift params like the shards."""
    parts = []
    for _ in range(count):
        axis = rng.normal(size=3)
        axis /= np.linalg.norm(axis)
        parts.append({
            "base": frustum_base(rng, cam),
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


def draw_water_plane(shader):
    """The water: one big quad at y = 0 drawn through the water shader. The
    shader does everything -- waves, sky reflection, fresnel, glints."""
    rl.begin_shader_mode(shader)
    rl.rl_begin(RL_QUADS)
    rl.rl_color4ub(255, 255, 255, 255)
    rl.rl_normal3f(0.0, 1.0, 0.0)
    rl.rl_vertex3f(-24.0, 0.0, -10.0)
    rl.rl_vertex3f(-24.0, 0.0, 14.0)
    rl.rl_vertex3f(24.0, 0.0, 14.0)
    rl.rl_vertex3f(24.0, 0.0, -10.0)
    rl.rl_end()
    rl.rl_draw_render_batch_active()
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


def make_particles(count, rng, cam):
    """Floating shard particles: a base position in the camera frustum plus
    drift, bob, and tumble parameters, integrated from time in the draw loop."""
    parts = []
    for _ in range(count):
        axis = rng.normal(size=3)
        axis /= np.linalg.norm(axis)
        parts.append({
            "base": frustum_base(rng, cam),
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
    """Colorful things for the gem to bend: pillars and a wanderer standing
    in the water (the floor is the water plane, drawn separately)."""

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
    bg_mode = "--bg-shot" in sys.argv  # crystals-only background export, then exit

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

    # the enemy demo (enemy_proto shaders + saved look), standing right in
    # front of the big gem
    elook = json.loads(ENEMY_LOOK_FILE.read_text()) if ENEMY_LOOK_FILE.is_file() else {}
    enemy_shader = rl.load_shader_from_memory(ENEMY_VS, ENEMY_FS)
    EU = {n: rl.get_shader_location(enemy_shader, n)
          for n in ("time", "yaw", "spikeLen", "spikeDensity", "pulseSpeed",
                    "breatheAmp", "breatheSpeed", "viewPos", "rim", "lineWidth", "expand")}
    enemy_outline = rl.load_shader_from_memory(ENEMY_VS, OUTLINE_FS)
    EUO = {n: rl.get_shader_location(enemy_outline, n)
           for n in ("time", "yaw", "spikeLen", "spikeDensity", "pulseSpeed",
                     "breatheAmp", "breatheSpeed", "expand", "alpha")}
    enemy_model = rl.load_model(str(build_enemy_glb(int(elook.get("subdiv", 2)),
                                                    int(elook.get("seed", 7)))))

    # live enemy knobs, seeded from the saved look: size, position, and the
    # shader params (spikes / breath / rim / wires / red hull)
    ep = {
        "size": ffi.new("float *", float(elook.get("size", 0.9)) * 0.55),
        "x": ffi.new("float *", 0.0),
        "y": ffi.new("float *", 1.15),
        "z": ffi.new("float *", 2.3),
        "spike_len": ffi.new("float *", float(elook.get("spike_len", 0.35))),
        "spike_density": ffi.new("float *", float(elook.get("spike_density", 0.45))),
        "pulse_speed": ffi.new("float *", float(elook.get("pulse_speed", 2.2))),
        "breathe_amp": ffi.new("float *", float(elook.get("breathe_amp", 0.06))),
        "breathe_speed": ffi.new("float *", float(elook.get("breathe_speed", 1.6))),
        "rim": ffi.new("float *", float(elook.get("rim", 0.8))),
        "line_width": ffi.new("float *", float(elook.get("line_width", 2.0))),
        "red_outline": ffi.new("float *", float(elook.get("red_outline", 0.05))),
        "spin": ffi.new("float *", 26.0),  # deg/s
    }

    # title text: Final-Fantasy-style serif (Cinzel, the Runic lookalike)
    # with live knobs. The text renders white into its own texture so the
    # fill can be anything: flat picked color, or the FF logo treatment --
    # a vertical pastel gradient
    TITLE_TEXT = "LEYLINES" # the FF fan font is capitals-only
    TITLE_BASE_SIZE = 128   # big base render so the size slider can go huge and stay sharp
    TITLE_RT_W, TITLE_RT_H = 960, 220
    title_font = rl.load_font_ex(str(Path(__file__).parent / "assets" / "finalf.ttf"),
                                 TITLE_BASE_SIZE, None, 0)
    rl.set_texture_filter(title_font.texture, rl.TextureFilter.TEXTURE_FILTER_BILINEAR)
    title_rt = rl.load_render_texture(TITLE_RT_W, TITLE_RT_H)
    rl.set_texture_filter(title_rt.texture, rl.TextureFilter.TEXTURE_FILTER_BILINEAR)
    tp = {
        "size": ffi.new("float *", 64.0),
        "x": ffi.new("float *", 0.0),    # offset from centered
        "y": ffi.new("float *", float(SCREEN_H - 60)),
        "glow": ffi.new("float *", 70.0),   # halo alpha, 0 = off
        "rot": ffi.new("float *", 0.0),     # degrees, spins about the text center
        "outline": ffi.new("float *", 2.0), # black ring thickness, px
        "spacing": ffi.new("float *", 1.0), # letter spacing, px at base size
    }
    title_color = ffi.new("struct Color *", [255, 214, 70, 255])
    title_pastel = ffi.new("bool *", True)  # the FF pastel gradient fill
    title_show = ffi.new("bool *", True)    # off = pure scene (background exports)
    # the three gradient stops, each with its own picker
    grad_top = ffi.new("struct Color *", [255, 252, 240, 255])
    grad_mid = ffi.new("struct Color *", [255, 191, 209, 255])
    grad_low = ffi.new("struct Color *", [158, 191, 255, 255])

    # FF logo fill: white-hot top melting through pastels toward the base
    TITLE_GRAD_FS = """
#version 100
precision mediump float;
varying vec2 fragTexCoord;
varying vec4 fragColor;
uniform sampler2D texture0;
uniform vec3 gradTop;
uniform vec3 gradMid;
uniform vec3 gradLow;
void main()
{
    vec4 texel = texture2D(texture0, fragTexCoord);
    float y = 1.0 - fragTexCoord.y; // render textures store bottom-up
    vec3 grad = mix(gradTop, gradMid, smoothstep(0.18, 0.58, y));
    grad = mix(grad, gradLow, smoothstep(0.55, 0.95, y));
    gl_FragColor = vec4(grad, 1.0)*texel;
}
"""
    TITLE_VS = """
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
    title_grad_shader = rl.load_shader_from_memory(TITLE_VS, TITLE_GRAD_FS)
    grad_locs = {n: rl.get_shader_location(title_grad_shader, n)
                 for n in ("gradTop", "gradMid", "gradLow")}

    def set_grad_color(loc, c):
        rl.set_shader_value(title_grad_shader, loc,
                            ffi.new("float[3]", [c.r / 255.0, c.g / 255.0, c.b / 255.0]),
                            rl.ShaderUniformDataType.SHADER_UNIFORM_VEC3)

    def draw_enemy(t):
        """Outline hulls + body off the live knobs. Called in BOTH passes:
        into the scene texture (so the gem refracts him) and on screen."""
        eyaw = t * math.radians(ep["spin"][0])
        epos = rl.Vector3(ep["x"][0], ep["y"][0], ep["z"][0])
        esize = ep["size"][0]
        set_f(enemy_shader, EU["time"], t)
        set_f(enemy_shader, EU["yaw"], eyaw)
        set_f(enemy_shader, EU["spikeLen"], ep["spike_len"][0])
        set_f(enemy_shader, EU["spikeDensity"], ep["spike_density"][0])
        set_f(enemy_shader, EU["pulseSpeed"], ep["pulse_speed"][0])
        set_f(enemy_shader, EU["breatheAmp"], ep["breathe_amp"][0])
        set_f(enemy_shader, EU["breatheSpeed"], ep["breathe_speed"][0])
        set_f(enemy_shader, EU["rim"], ep["rim"][0])
        set_f(enemy_shader, EU["lineWidth"], ep["line_width"][0])
        set_f(enemy_shader, EU["expand"], 0.0)
        set_v3(enemy_shader, EU["viewPos"], cam.position.x, cam.position.y, cam.position.z)
        eo = ep["red_outline"][0]
        if eo > 0.002:
            set_f(enemy_outline, EUO["time"], t)
            set_f(enemy_outline, EUO["yaw"], eyaw)
            set_f(enemy_outline, EUO["spikeLen"], ep["spike_len"][0])
            set_f(enemy_outline, EUO["spikeDensity"], ep["spike_density"][0])
            set_f(enemy_outline, EUO["pulseSpeed"], ep["pulse_speed"][0])
            set_f(enemy_outline, EUO["breatheAmp"], ep["breathe_amp"][0])
            set_f(enemy_outline, EUO["breatheSpeed"], ep["breathe_speed"][0])
            rl.rl_draw_render_batch_active()
            rl.rl_set_cull_face(rl.rl.RL_CULL_FACE_FRONT)
            enemy_model.materials[0].shader = enemy_outline
            set_f(enemy_outline, EUO["expand"], eo)
            set_f(enemy_outline, EUO["alpha"], 1.0)
            rl.draw_model(enemy_model, epos, esize, rl.WHITE)
            rl.begin_blend_mode(rl.BlendMode.BLEND_ADDITIVE)
            set_f(enemy_outline, EUO["expand"], eo * 2.6)
            set_f(enemy_outline, EUO["alpha"], 0.28)
            rl.draw_model(enemy_model, epos, esize, rl.WHITE)
            rl.end_blend_mode()
            rl.rl_set_cull_face(rl.rl.RL_CULL_FACE_BACK)
        enemy_model.materials[0].shader = enemy_shader
        rl.draw_model(enemy_model, epos, esize, rl.WHITE)

    def render_title_rt():
        """The raw glyphs, white, into their own texture (done OUTSIDE any
        other texture mode -- texture modes cannot nest)."""
        t_spacing = tp["spacing"][0]
        t_w = rl.measure_text_ex(title_font, TITLE_TEXT, TITLE_BASE_SIZE, t_spacing).x
        rl.begin_texture_mode(title_rt)
        rl.clear_background(rl.BLANK)
        rl.draw_text_ex(title_font, TITLE_TEXT,
                        rl.Vector2((TITLE_RT_W - t_w) / 2.0, (TITLE_RT_H - TITLE_BASE_SIZE) / 2.0),
                        TITLE_BASE_SIZE, t_spacing, rl.WHITE)
        rl.end_texture_mode()

    def draw_title():
        """Stamps compose the look -- warm halo, BLACK outline, then the FF
        pastel gradient or the flat picked fill. Drawn INTO the refracted
        scene (pass 1), so the gem and every shard bends the letters."""
        t_scale = tp["size"][0] / TITLE_BASE_SIZE
        src = rl.Rectangle(0, 0, TITLE_RT_W, -TITLE_RT_H)
        t_rot = tp["rot"][0]
        t_origin = rl.Vector2(TITLE_RT_W * t_scale / 2.0, TITLE_RT_H * t_scale / 2.0)

        def stamp(dx, dy, tint):
            dest = rl.Rectangle(SCREEN_W / 2.0 + tp["x"][0] + dx, tp["y"][0] + dy,
                                TITLE_RT_W * t_scale, TITLE_RT_H * t_scale)
            rl.draw_texture_pro(title_rt.texture, src, dest, t_origin, t_rot, tint)

        glow_a = tp["glow"][0]
        if glow_a > 1.0:
            for radius, alpha in ((8, int(glow_a * 0.55)), (5, int(glow_a))):
                for k in range(8):
                    a = math.tau * k / 8.0
                    stamp(math.cos(a) * radius, math.sin(a) * radius,
                          rl.Color(255, 190, 120, min(int(alpha), 255)))
        o_px = tp["outline"][0]
        if o_px > 0.1:
            for k in range(8):  # true black ring, thickness from the slider
                a = math.tau * k / 8.0
                stamp(math.cos(a) * o_px, math.sin(a) * o_px, rl.BLACK)
        if title_pastel[0]:
            set_grad_color(grad_locs["gradTop"], grad_top[0])
            set_grad_color(grad_locs["gradMid"], grad_mid[0])
            set_grad_color(grad_locs["gradLow"], grad_low[0])
            rl.begin_shader_mode(title_grad_shader)
            stamp(0.0, 0.0, rl.WHITE)  # gradient shader paints the glyphs
            rl.end_shader_mode()
        else:
            fill = title_color[0]
            stamp(0.0, 0.0, rl.Color(fill.r, fill.g, fill.b, 255))

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
    particles = make_particles(MAX_PARTICLES, np.random.default_rng(3), cam)
    count_ptr = ffi.new("float *", 60.0)
    psize_ptr = ffi.new("float *", 1.0)

    # Triangle glow: half-res mask + blur scratch buffer, blur shader
    MAX_TRIS = 80
    tris = make_tri_particles(MAX_TRIS, np.random.default_rng(9), cam)
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

    water_shader = rl.load_shader_from_memory(GEM_VS, WATER_FS)
    water_locs = {name: rl.get_shader_location(water_shader, name)
                  for name in ("viewPos", "time", "intensity", "waveAmp", "waveScale")}
    wave_amp_ptr = ffi.new("float *", 0.35)
    wave_scale_ptr = ffi.new("float *", 1.4)

    # live optics (raygui sliders write these pointers)
    ior_ptr = ffi.new("float *", 1.55)       # quartz
    strength_ptr = ffi.new("float *", 0.5)   # screen-space shift scale
    chroma_ptr = ffi.new("float *", 0.04)    # per-channel IOR spread
    fresnel_ptr = ffi.new("float *", 4.0)
    tint_ptr = ffi.new("float *", 0.10)      # how milky the quartz is

    panel = rl.Rectangle(422, 8, 200, 470)
    spin = 0.0
    show_ui = True  # P hides every panel for a clean export view

    # G exports a gif: a deterministic clock drives GIF_SECONDS of frames
    # (so llvmpipe's framerate doesn't change the motion), captured off the
    # screen and assembled by PIL into screenshots/leylines_cover.gif
    GIF_FPS = 6          # itch's 3 MB cover cap rules: few frames...
    GIF_SECONDS = 6.0
    GIF_SCALE = 0.8      # ...and a 504x400 downscale (still above itch minimum)
    GIF_CROSS = 1.5      # extra seconds recorded, crossfaded tail-into-head: seamless loop
    GIF_OUT = Path(__file__).parent.parent / "screenshots" / "leylines_bg.gif"
    GIF_SCRATCH = Path(__file__).parent / "gif_frames"
    rec_i = -1          # -1 = not recording
    rec_base_t = 0.0
    rec_status = ""
    frame = 0
    show_normals = False
    normals_ptr = ffi.new("int *", 0)
    cam_base = [0.0, 2.6, 6.5]  # wheel zoom scales this; mouse sway offsets it
    sway = [0.0, 0.0]

    while not rl.window_should_close():
        frame += 1
        t = rl.get_time()
        dt = rl.get_frame_time()
        if rec_i >= 0:  # recording: fixed synthetic clock, real time ignored
            t = rec_base_t + rec_i / GIF_FPS
            dt = 1.0 / GIF_FPS

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

        render_title_rt()  # glyph texture first: texture modes cannot nest

        # Pass 1: the world without the gem -- this is what the gem will bend.
        # The flat triangles live here too, so the crystals refract them.
        rl.begin_texture_mode(scene_rt)
        rl.clear_background(rl.Color(255, 0, 0, 255))  # flat RED backdrop, nothing else
        rl.begin_mode_3d(cam)
        draw_tri_particles(tris, int(tri_ptr[0]), t, 1.0)
        if not bg_mode:
            draw_enemy(t)  # the gem refracts HIM -- he must exist in the scene it bends
        rl.end_mode_3d()
        if title_show[0]:
            draw_title()  # the title lives in the refracted scene too: shards
        rl.end_texture_mode() # and the gem bend the letters drifting past them

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
        if rl.is_key_pressed(rl.KeyboardKey.KEY_P):
            show_ui = not show_ui
        if (rl.is_key_pressed(rl.KeyboardKey.KEY_G) or (bg_mode and frame == 40)) and rec_i < 0:
            if bg_mode:
                title_show[0] = False
            rec_i = 0
            rec_base_t = t
            show_ui = False
            GIF_SCRATCH.mkdir(exist_ok=True)
        if rl.is_key_pressed(rl.KeyboardKey.KEY_N):
            show_normals = not show_normals
        normals_ptr[0] = 1 if show_normals else 0
        rl.set_shader_value(shader, locs["showNormals"], normals_ptr,
                            rl.ShaderUniformDataType.SHADER_UNIFORM_INT)

        # Pass 2: the same scene on screen, gem on top refracting pass 1
        rl.begin_drawing()
        rl.clear_background(rl.Color(255, 0, 0, 255))
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

        # the enemy again, sharp, in front of the gem on screen
        if not bg_mode:
            draw_enemy(t)
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

        if show_ui:
                # optics panel
            rl.gui_window_box(panel, "quartz optics")
            px, py = int(panel.x + 8), int(panel.y + 28)
            rl.gui_label(rl.Rectangle(px, py, 184, 12), f"ior  {ior_ptr[0]:.2f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 14, 184, 12), "", "", ior_ptr, 1.0, 2.4)
            rl.gui_label(rl.Rectangle(px, py + 36, 184, 12), f"strength  {strength_ptr[0]:.2f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 50, 184, 12), "", "", strength_ptr, 0.0, 1.0)
            rl.gui_label(rl.Rectangle(px, py + 72, 184, 12), f"dispersion  {chroma_ptr[0]:.3f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 86, 184, 12), "", "", chroma_ptr, 0.0, 0.15)
            rl.gui_label(rl.Rectangle(px, py + 108, 184, 12), f"fresnel  {fresnel_ptr[0]:.1f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 122, 184, 12), "", "", fresnel_ptr, 1.0, 8.0)
            rl.gui_label(rl.Rectangle(px, py + 144, 184, 12), f"milkiness  {tint_ptr[0]:.2f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 158, 184, 12), "", "", tint_ptr, 0.0, 0.8)
            rl.gui_label(rl.Rectangle(px, py + 180, 184, 12), f"particles  {int(count_ptr[0])}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 194, 184, 12), "", "", count_ptr, 0.0, float(MAX_PARTICLES))
            rl.gui_label(rl.Rectangle(px, py + 216, 184, 12), f"particle size  {psize_ptr[0]:.2f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 230, 184, 12), "", "", psize_ptr, 0.3, 2.5)
            rl.gui_label(rl.Rectangle(px, py + 252, 184, 12), f"triangles  {int(tri_ptr[0])}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 266, 184, 12), "", "", tri_ptr, 0.0, float(MAX_TRIS))
            rl.gui_label(rl.Rectangle(px, py + 288, 184, 12), f"glow  {glow_ptr[0]:.2f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 302, 184, 12), "", "", glow_ptr, 0.0, 1.0)
            rl.gui_label(rl.Rectangle(px, py + 324, 184, 12), f"aurora  {aurora_ptr[0]:.2f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 338, 184, 12), "", "", aurora_ptr, 0.0, 2.0)
            rl.gui_label(rl.Rectangle(px, py + 360, 184, 12), f"wave amp  {wave_amp_ptr[0]:.2f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 374, 184, 12), "", "", wave_amp_ptr, 0.0, 1.2)
            rl.gui_label(rl.Rectangle(px, py + 396, 184, 12), f"wave scale  {wave_scale_ptr[0]:.2f}")
            rl.gui_slider_bar(rl.Rectangle(px, py + 410, 184, 12), "", "", wave_scale_ptr, 0.3, 4.0)

        if show_ui:
            # title panel: sliders, FF-gradient toggle + its three stop
            # pickers, flat fill picker
            title_panel = rl.Rectangle(214, 8, 200, 484)
            rl.gui_window_box(title_panel, "title")
            tpx, tpy = int(title_panel.x + 8), int(title_panel.y + 28)

            def tslider(i, label, key, lo, hi, fmt="{:.0f}"):
                yy = tpy + i * 30
                rl.gui_label(rl.Rectangle(tpx, yy, 184, 12), f"{label}  {fmt.format(tp[key][0])}")
                rl.gui_slider_bar(rl.Rectangle(tpx, yy + 12, 184, 12), "", "", tp[key], lo, hi)

            tslider(0, "size", "size", 20.0, 240.0)
            tslider(1, "x offset", "x", -300.0, 300.0)
            tslider(2, "y", "y", 0.0, float(SCREEN_H - 20))
            tslider(3, "glow", "glow", 0.0, 200.0)
            tslider(4, "rotation", "rot", -180.0, 180.0)
            tslider(5, "outline px", "outline", 0.0, 6.0, "{:.1f}")
            tslider(6, "spacing", "spacing", 0.0, 12.0, "{:.1f}")
            cy0 = tpy + 7 * 30
            rl.gui_check_box(rl.Rectangle(tpx, cy0, 14, 14), "FF pastel gradient", title_pastel)
            rl.gui_check_box(rl.Rectangle(tpx + 110, cy0, 14, 14), "show title", title_show)
            rl.gui_label(rl.Rectangle(tpx, cy0 + 20, 184, 12), "gradient top / mid / low")
            rl.gui_color_picker(rl.Rectangle(tpx, cy0 + 34, 44, 44), "", grad_top)
            rl.gui_color_picker(rl.Rectangle(tpx + 64, cy0 + 34, 44, 44), "", grad_mid)
            rl.gui_color_picker(rl.Rectangle(tpx + 128, cy0 + 34, 44, 44), "", grad_low)
            rl.gui_label(rl.Rectangle(tpx, cy0 + 86, 184, 12), "flat fill")
            rl.gui_color_picker(rl.Rectangle(tpx, cy0 + 100, 160, 88), "", title_color)

        if show_ui:
            # enemy panel: size, position, and every shader knob
            enemy_panel = rl.Rectangle(8, 8, 200, 434)
            rl.gui_window_box(enemy_panel, "enemy")
            epx, epy = int(enemy_panel.x + 8), int(enemy_panel.y + 28)

            def eslider(i, label, key, lo, hi, fmt="{:.2f}"):
                yy = epy + i * 30
                rl.gui_label(rl.Rectangle(epx, yy, 184, 12), f"{label}  {fmt.format(ep[key][0])}")
                rl.gui_slider_bar(rl.Rectangle(epx, yy + 12, 184, 12), "", "", ep[key], lo, hi)

            eslider(0, "size", "size", 0.15, 1.6)
            eslider(1, "pos x", "x", -3.0, 3.0)
            eslider(2, "pos y", "y", 0.0, 3.5)
            eslider(3, "pos z", "z", -1.5, 4.5)
            eslider(4, "spike length", "spike_len", 0.0, 1.2)
            eslider(5, "spike density", "spike_density", 0.05, 1.0)
            eslider(6, "pulse speed", "pulse_speed", 0.0, 8.0)
            eslider(7, "breathe amp", "breathe_amp", 0.0, 0.25)
            eslider(8, "breathe speed", "breathe_speed", 0.0, 6.0)
            eslider(9, "pastel rim", "rim", 0.0, 1.5)
            eslider(10, "line width", "line_width", 1.0, 6.0, "{:.1f}")
            eslider(11, "red outline", "red_outline", 0.0, 0.15, "{:.3f}")
            eslider(12, "spin deg/s", "spin", 0.0, 120.0, "{:.0f}")

        if show_ui:
            rl.draw_text("wheel: zoom  arrows: spin gem  n: normals  p: panels  g: export gif", 8, SCREEN_H - 22, 16, rl.GRAY)
            if rec_status:
                rl.draw_text(rec_status, 8, SCREEN_H - 42, 16, rl.LIME)
        rl.end_drawing()

        if rec_i >= 0:
            shot = rl.load_image_from_screen()
            rl.export_image(shot, str(GIF_SCRATCH / f"frame_{rec_i:03d}.png"))
            rl.unload_image(shot)
            rec_i += 1
            if rec_i >= int(GIF_FPS * (GIF_SECONDS + GIF_CROSS)):
                from PIL import Image
                gif_size = (int(SCREEN_W * GIF_SCALE), int(SCREEN_H * GIF_SCALE))
                raw = [Image.open(GIF_SCRATCH / f"frame_{k:03d}.png").convert("RGB").resize(gif_size, Image.LANCZOS)
                       for k in range(rec_i)]
                # seamless loop: the extra tail frames continue the motion past
                # the loop point; fading them into the head hides the seam
                n_out = int(GIF_FPS * GIF_SECONDS)
                n_cross = rec_i - n_out
                imgs = []
                for k in range(n_out):
                    if k < n_cross:
                        imgs.append(Image.blend(raw[n_out + k], raw[k], k / n_cross))
                    else:
                        imgs.append(raw[k])
                if bg_mode:
                    # edge vignette into flat red: set the itch page background
                    # color to #FF0000 and the gif dissolves into the page
                    gw_, gh_ = gif_size
                    yy, xx = np.mgrid[0:gh_, 0:gw_]
                    margin_x, margin_y = gw_ * 0.16, gh_ * 0.16
                    d = np.minimum(np.minimum(xx, gw_ - 1 - xx) / margin_x,
                                   np.minimum(yy, gh_ - 1 - yy) / margin_y)
                    a = np.clip(d, 0.0, 1.0)
                    a = a * a * (3.0 - 2.0 * a)  # smoothstep
                    vmask = Image.fromarray((a * 255).astype("uint8"), "L")
                    page_red = Image.new("RGB", gif_size, (255, 0, 0))
                    imgs = [Image.composite(im, page_red, vmask) for im in imgs]
                pal = imgs[len(imgs) // 2].quantize(colors=96)
                q = [im.quantize(palette=pal, dither=Image.Dither.NONE) for im in imgs]
                GIF_OUT.parent.mkdir(exist_ok=True)
                q[0].save(GIF_OUT, save_all=True, append_images=q[1:],
                          duration=int(1000 / GIF_FPS), loop=0, optimize=True)
                for f in GIF_SCRATCH.glob("frame_*.png"):
                    f.unlink()
                GIF_SCRATCH.rmdir()
                rec_status = f"wrote {GIF_OUT.name} ({GIF_OUT.stat().st_size // 1024} KB)"
                print(rec_status)
                rec_i = -1
                show_ui = True
                if bg_mode:
                    break

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
