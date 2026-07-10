#!/usr/bin/env python3
"""Export the card prototype's current look as a .glb (card + hex form).

Used by card_proto.py's "export .glb" panel button, which passes the live
state (art buffers, threshold cut, relief heights, card color) so the export
matches exactly what is on screen. Can also run standalone for a default
export:

    python3 export_glb.py [out.glb]

Layout in the file: the card stands upright at the origin, its hex form lies
flat on the ground plane at +2.5 X, like the game board. Every layer is its
own named node (card_frame, card_art_raised, hex_ring, ...) so they stay
editable after import.
"""

import math
from pathlib import Path

import numpy as np
import trimesh
from PIL import Image

ASSETS = Path(__file__).parent / "assets"
HEX_OFFSET_X = 2.5  # where the hex form sits beside the card in the combined file


def _material(image=None, rgba=(1.0, 1.0, 1.0, 1.0)):
    return trimesh.visual.material.PBRMaterial(
        baseColorTexture=image, baseColorFactor=list(rgba),
        alphaMode="BLEND", doubleSided=True,
        metallicFactor=0.0, roughnessFactor=1.0)


def quad_mesh(w, h, image=None, rgba=(1.0, 1.0, 1.0, 1.0)):
    """A 2-triangle plane in the XY plane, facing +Z, with 0..1 UVs.
    glTF UV origin is top-left, so v flips against the vertex y."""
    hw, hh = w / 2.0, h / 2.0
    vertices = np.array([[-hw, -hh, 0], [hw, -hh, 0], [hw, hh, 0], [-hw, hh, 0]], float)
    faces = np.array([[0, 1, 2], [0, 2, 3]])
    uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], float)  # trimesh uv origin: bottom-left
    mesh = trimesh.Trimesh(vertices=vertices, faces=faces, process=False)
    mesh.visual = trimesh.visual.TextureVisuals(uv=uv, material=_material(image, rgba))
    return mesh


def _hex_points(radius):
    """Pointy-top hexagon corners in the XY plane, like the game board."""
    return [(radius * math.sin(math.radians(60 * k)),
             radius * math.cos(math.radians(60 * k))) for k in range(6)]


def hex_mesh(radius, image=None, rgba=(1.0, 1.0, 1.0, 1.0)):
    """Pointy-top hexagon fan; UVs map its bounding square onto the texture,
    cropping a square image to the hex like the prototype's SDF shader."""
    points = _hex_points(radius)
    vertices = np.array([(0.0, 0.0, 0.0)] + [(x, y, 0.0) for x, y in points], float)
    faces = np.array([[0, 1 + k, 1 + (k + 1) % 6] for k in range(6)])
    uv = np.array([[(x / (2 * radius)) + 0.5, (y / (2 * radius)) + 0.5]
                   for x, y, _ in vertices], float)
    mesh = trimesh.Trimesh(vertices=vertices, faces=faces, process=False)
    mesh.visual = trimesh.visual.TextureVisuals(uv=uv, material=_material(image, rgba))
    return mesh


def hex_ring_mesh(r_outer, r_inner, rgba):
    """Flat annulus between two hexagons -- the tile's border ring."""
    outer = _hex_points(r_outer)
    inner = _hex_points(r_inner)
    vertices = np.array([(x, y, 0.0) for x, y in outer + inner], float)
    faces = []
    for k in range(6):
        k2 = (k + 1) % 6
        faces.append([k, k2, 6 + k])
        faces.append([k2, 6 + k2, 6 + k])
    mesh = trimesh.Trimesh(vertices=vertices, faces=np.array(faces), process=False)
    uv = np.zeros((12, 2))
    mesh.visual = trimesh.visual.TextureVisuals(uv=uv, material=_material(None, rgba))
    return mesh


def _pil_from_rgba(rgba_array):
    return Image.fromarray(np.ascontiguousarray(rgba_array), "RGBA")


def export_card_glb(out_path, state):
    """Build the scene from the prototype's live state and write a .glb.

    state keys:
      template_path        fire card outline PNG
      coin_path            coin badge PNG
      art_back, art_raised RGBA numpy arrays of the current icon cell
      color                card color (r, g, b) 0..255
      card_h               card world height; template aspect gives the width
      window               (x, y, w, h) open interior in template px
      template_size        (w, h) template px
      art_z0, art_z_step   relief heights
      hex_radius           hex tile outer radius
    """
    color = tuple(c / 255.0 for c in state["color"])
    # Same wash as the prototype's tile base: dark parchment tinted by the card
    a = 55.0 / 255.0
    wash = ((52 * (1 - a) + state["color"][0] * a) / 255.0,
            (44 * (1 - a) + state["color"][1] * a) / 255.0,
            (52 * (1 - a) + state["color"][2] * a) / 255.0, 1.0)

    template = Image.open(state["template_path"]).convert("RGBA")
    coin = Image.open(state["coin_path"]).convert("RGBA")
    back_img = _pil_from_rgba(state["art_back"])
    raised_img = _pil_from_rgba(state["art_raised"])

    tw, th = state["template_size"]
    card_h = state["card_h"]
    wpt = card_h / th                     # world units per template pixel
    card_w = tw * wpt
    wx, wy, ww, wh = state["window"]
    win_w, win_h = ww * wpt * 0.94, wh * wpt * 0.94
    win_dy = -(wy + wh / 2.0 - th / 2.0) * wpt  # window center offset, y up
    z0, zs = state["art_z0"], state["art_z_step"]

    # Three scenes: the combined file for DCC viewing, plus split card/hex
    # files (each centered at the origin) that the game loads -- raylib bakes
    # node transforms into vertices and drops names, so it cannot split the
    # combined file itself
    combined = trimesh.Scene()
    card_scene = trimesh.Scene()
    hex_scene = trimesh.Scene()

    def add_to(scene, mesh, name, translate, rotate_x_deg=0.0):
        transform = trimesh.transformations.translation_matrix(translate)
        if rotate_x_deg:
            transform = transform @ trimesh.transformations.rotation_matrix(
                math.radians(rotate_x_deg), (1, 0, 0))
        scene.add_geometry(mesh, node_name=name, geom_name=name, transform=transform)

    def add(mesh, name, translate, rotate_x_deg=0.0):
        """Card-group geometry: combined + card scene, same placement."""
        add_to(combined, mesh, name, translate, rotate_x_deg)
        add_to(card_scene, mesh, name, translate, rotate_x_deg)

    def add_hex(mesh, name, translate, rotate_x_deg=0.0):
        """Hex-group geometry: offset beside the card in the combined scene,
        centered at the origin in the hex scene."""
        add_to(combined, mesh, name,
               (translate[0] + HEX_OFFSET_X, translate[1], translate[2]), rotate_x_deg)
        add_to(hex_scene, mesh, name, translate, rotate_x_deg)

    # --- card, upright at the origin ---
    add(quad_mesh(win_w + 0.1, win_h + 0.1, rgba=(0.20, 0.17, 0.20, 1.0)),
        "card_backing", (0, win_dy, -0.02))
    add(quad_mesh(card_w, card_h, image=template), "card_frame", (0, 0, 0))
    add(quad_mesh(win_w, win_h, image=back_img), "card_art_back", (0, win_dy, z0))
    add(quad_mesh(win_w, win_h, image=raised_img), "card_art_raised", (0, win_dy, z0 + zs))
    if state.get("frame_raised") is not None:
        add(quad_mesh(card_w, card_h, image=_pil_from_rgba(state["frame_raised"])),
            "card_frame_raised", (0, 0, state.get("frame_z", z0 + zs + 0.03)))
    coin_size = 10.0 * wpt
    add(quad_mesh(coin_size, coin_size * coin.height / coin.width, image=coin),
        "card_coin", (-card_w / 2 + coin_size * 0.2, card_h / 2 - coin_size * 0.2, 0.02))

    # --- hex form, lying flat on the ground (beside the card when combined) ---
    hr = state["hex_radius"]
    ring_inner = hr * 0.79
    add_hex(hex_mesh(hr, rgba=wash), "hex_fill", (0.0, 0.0, 0.0), rotate_x_deg=-90)
    add_hex(hex_ring_mesh(hr, ring_inner, rgba=color + (1.0,)),
            "hex_ring", (0.0, 0.005, 0.0), rotate_x_deg=-90)
    art_r = ring_inner * 1.02
    add_hex(hex_mesh(art_r, image=back_img), "hex_art_back", (0.0, z0, 0.0), rotate_x_deg=-90)
    add_hex(hex_mesh(art_r, image=raised_img), "hex_art_raised",
            (0.0, z0 + zs, 0.0), rotate_x_deg=-90)

    # Write combined + the split files the game reads
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    combined.export(str(out_path))
    card_scene.export(str(out_path.with_name(out_path.stem + "_card.glb")))
    hex_scene.export(str(out_path.with_name(out_path.stem + "_hex.glb")))
    return out_path


def default_state():
    """Standalone-run state: gandalf sheet icon 0, gold card, threshold 120."""
    import cv2
    img = cv2.imread(str(ASSETS / "gandalf_icons_16x16.png"), cv2.IMREAD_UNCHANGED)
    rgba = np.ascontiguousarray(cv2.cvtColor(img, cv2.COLOR_BGRA2RGBA))[:16, :16]
    gray = cv2.cvtColor(img[:16, :16, :3], cv2.COLOR_BGR2GRAY)
    raised = rgba.copy()
    raised[..., 3] = np.where((gray >= 120) & (rgba[..., 3] > 0), raised[..., 3], 0)
    frame_bgra = cv2.imread(str(ASSETS / "fire_card_39x66.png"), cv2.IMREAD_UNCHANGED)
    frame_rgba = np.ascontiguousarray(cv2.cvtColor(frame_bgra, cv2.COLOR_BGRA2RGBA))
    frame_gray = cv2.cvtColor(frame_bgra[..., :3], cv2.COLOR_BGR2GRAY)
    frame_raised = frame_rgba.copy()
    frame_raised[..., 3] = np.where((frame_gray >= 150) & (frame_rgba[..., 3] > 0),
                                    frame_raised[..., 3], 0)
    return {
        "template_path": ASSETS / "fire_card_39x66.png",
        "coin_path": ASSETS / "CoinIcon_16x18.png",
        "art_back": rgba, "art_raised": raised,
        "frame_raised": frame_raised, "frame_z": 0.05 + 0.14 + 0.03,
        "color": (255, 203, 0),
        "card_h": 2.6, "window": (4, 11, 31, 51), "template_size": (39, 66),
        "art_z0": 0.05, "art_z_step": 0.14, "hex_radius": 1.05,
    }


if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "export/card_default.glb"
    path = export_card_glb(Path(__file__).parent / out, default_state())
    print(f"wrote {path}")
