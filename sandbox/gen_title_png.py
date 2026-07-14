#!/usr/bin/env python3
"""Render the LEYLINES title lockup to a transparent PNG for the itch page.

    python3 gen_title_png.py

Writes ../screenshots/leylines_title.png -- the FF fan font with the black
outline and the pastel gradient fill, on transparent, ready to embed in the
itch description (itch HTML cannot load custom fonts, so the title ships as
an image).
"""

import math
from pathlib import Path

import pyray as rl

TEXT = "LEYLINES"
FONT = Path(__file__).parent / "assets" / "finalf.ttf"
OUT = Path(__file__).parent.parent / "screenshots" / "leylines_title.png"
W, H = 1200, 300
BASE = 190  # glyph size

GRAD_VS = """
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
GRAD_FS = """
#version 100
precision mediump float;
varying vec2 fragTexCoord;
varying vec4 fragColor;
uniform sampler2D texture0;
void main()
{
    vec4 texel = texture2D(texture0, fragTexCoord);
    float y = 1.0 - fragTexCoord.y;
    vec3 top = vec3(1.0, 0.988, 0.94);
    vec3 mid = vec3(1.0, 0.75, 0.82);
    vec3 low = vec3(0.62, 0.75, 1.0);
    vec3 grad = mix(top, mid, smoothstep(0.18, 0.58, y));
    grad = mix(grad, low, smoothstep(0.55, 0.95, y));
    gl_FragColor = vec4(grad, 1.0)*texel;
}
"""


def main():
    rl.set_config_flags(rl.ConfigFlags.FLAG_WINDOW_HIDDEN)
    rl.init_window(W, H, "title render")

    font = rl.load_font_ex(str(FONT), BASE, None, 0)
    rl.set_texture_filter(font.texture, rl.TextureFilter.TEXTURE_FILTER_BILINEAR)
    grad = rl.load_shader_from_memory(GRAD_VS, GRAD_FS)

    glyphs = rl.load_render_texture(W, H)  # the raw white text
    final = rl.load_render_texture(W, H)   # composed lockup

    spacing = 4.0
    tw = rl.measure_text_ex(font, TEXT, BASE, spacing).x
    rl.begin_texture_mode(glyphs)
    rl.clear_background(rl.BLANK)
    rl.draw_text_ex(font, TEXT, rl.Vector2((W - tw) / 2.0, (H - BASE) / 2.0), BASE, spacing, rl.WHITE)
    rl.end_texture_mode()

    src = rl.Rectangle(0, 0, W, -H)

    def stamp(dx, dy, tint):
        rl.draw_texture_rec(glyphs.texture, src, rl.Vector2(dx, dy), tint)

    rl.begin_texture_mode(final)
    rl.clear_background(rl.BLANK)
    for radius, alpha in ((10, 60), (6, 110)):  # warm halo
        for k in range(8):
            a = math.tau * k / 8.0
            stamp(math.cos(a) * radius, math.sin(a) * radius, rl.Color(255, 190, 120, alpha))
    for k in range(16):  # black outline ring
        a = math.tau * k / 16.0
        stamp(math.cos(a) * 4.0, math.sin(a) * 4.0, rl.BLACK)
    rl.begin_shader_mode(grad)
    stamp(0.0, 0.0, rl.WHITE)  # pastel gradient fill
    rl.end_shader_mode()
    rl.end_texture_mode()

    shot = rl.load_image_from_texture(final.texture)
    rl.image_flip_vertical(shot)
    rl.export_image(shot, str(OUT))
    rl.close_window()
    print(f"wrote {OUT} ({OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
