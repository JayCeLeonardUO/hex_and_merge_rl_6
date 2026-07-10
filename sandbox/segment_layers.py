#!/usr/bin/env python3
"""Break a card/sprite image into relief layers, with OpenCV.

    python3 segment_layers.py IMAGE [-o OUTDIR] [--hue-lo 70] [--hue-hi 170]
                              [--min-sat 0.2] [--min-val 0.15] [--clean N]

Writes two PNGs next to the input (or into -o):
    <name>_back.png   the full print, untouched -- the base layer
    <name>_green.png  only the green pixels -- meant to be raised above the back

Green = HSV hue inside [hue-lo, hue-hi] degrees (cv2.inRange) with saturation
and brightness floors, so dark outlines and neutral parchment stay on the
back layer. --clean N runs an NxN morphological open+close to despeckle the
mask (leave at 0 for pixel art; single-pixel details survive).
"""

import argparse
from pathlib import Path

import cv2
import numpy as np


def segment_green(img_bgra, hue_lo=70.0, hue_hi=170.0, min_sat=0.2, min_val=0.15, clean=0):
    """uint8 mask (0/255) of pixels whose HSV hue falls in the green band."""
    hsv = cv2.cvtColor(img_bgra[..., :3], cv2.COLOR_BGR2HSV)
    lo = np.array([hue_lo / 2.0, min_sat * 255.0, min_val * 255.0], np.uint8)  # cv2 hue is 0..179
    hi = np.array([hue_hi / 2.0, 255, 255], np.uint8)
    mask = cv2.inRange(hsv, lo, hi)

    if img_bgra.shape[-1] == 4:
        mask = cv2.bitwise_and(mask, cv2.inRange(img_bgra[..., 3], 1, 255))

    if clean > 0:
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (clean, clean))
        mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)

    return mask


def segment_bright(img_bgra, thresh=120, clean=0):
    """uint8 mask (0/255) of pixels at or above the brightness threshold."""
    gray = cv2.cvtColor(img_bgra[..., :3], cv2.COLOR_BGR2GRAY)
    _, mask = cv2.threshold(gray, int(thresh) - 1, 255, cv2.THRESH_BINARY)

    if img_bgra.shape[-1] == 4:
        mask = cv2.bitwise_and(mask, cv2.inRange(img_bgra[..., 3], 1, 255))

    if clean > 0:
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (clean, clean))
        mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)

    return mask


def split(path, outdir=None, mode="green", **kw):
    """Write <name>_back.png (full print) and <name>_<mode-layer>.png (raised)."""
    path = Path(path)
    img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise SystemExit(f"could not read {path}")
    if img.shape[-1] == 3:  # promote to BGRA so the layer mask has an alpha to cut
        img = cv2.cvtColor(img, cv2.COLOR_BGR2BGRA)
    outdir = Path(outdir) if outdir else path.parent

    if mode == "bright":
        mask = segment_bright(img, **kw)
        layer_name = "bright"
    else:
        mask = segment_green(img, **kw)
        layer_name = "green"

    raised = img.copy()
    raised[..., 3] = cv2.bitwise_and(img[..., 3], mask)

    back_path = outdir / f"{path.stem}_back.png"
    raised_path = outdir / f"{path.stem}_{layer_name}.png"
    cv2.imwrite(str(back_path), img)
    cv2.imwrite(str(raised_path), raised)
    print(f"{path.name}: {int(np.count_nonzero(mask))} {layer_name} px -> {raised_path.name}, "
          f"full print -> {back_path.name}")
    return back_path, raised_path


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="input PNG (sprite or sheet)")
    ap.add_argument("-o", "--outdir", help="output directory (default: next to input)")
    ap.add_argument("--mode", choices=("green", "bright"), default="green",
                    help="segment by green hue band or by brightness threshold")
    ap.add_argument("--thresh", type=float, default=120, help="brightness threshold (bright mode)")
    ap.add_argument("--hue-lo", type=float, default=70.0, help="green band lower hue, degrees")
    ap.add_argument("--hue-hi", type=float, default=170.0, help="green band upper hue, degrees")
    ap.add_argument("--min-sat", type=float, default=0.2, help="minimum saturation to count as green")
    ap.add_argument("--min-val", type=float, default=0.15, help="minimum brightness to count as green")
    ap.add_argument("--clean", type=int, default=0, help="morphological open+close kernel size")
    args = ap.parse_args()
    if args.mode == "bright":
        split(args.image, args.outdir, mode="bright", thresh=args.thresh, clean=args.clean)
    else:
        split(args.image, args.outdir, mode="green", hue_lo=args.hue_lo, hue_hi=args.hue_hi,
              min_sat=args.min_sat, min_val=args.min_val, clean=args.clean)
