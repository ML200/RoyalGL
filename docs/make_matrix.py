# -*- coding: utf-8 -*-
# Builds fig_matrix.png: a scene x method comparison matrix for the paper.
# Rows: three fog scenes; columns: converged BDPT reference and four
# per-frame estimators (PT, ReSTIR PT, BDPT, ReSTIR BDPT+volumetric shifts).
# Under each full frame: mean FLIP of the displayed luminance against the
# 4096 spp reference (identical pipeline to docs/make_plots.py). Below each
# scene: two zoomed crops per method with per-crop mean FLIP.
#
# Inputs are raw RGBA32F dumps (ROYALGL_EXPORT_SERIES) named
# s{1,2,3}_{truth_04096,pt_00001,uni_00008,bdpt_00001,ours_00008}.f32 in FR.
# PT/BDPT are memoryless (any frame is one sample); the ReSTIR estimators
# are per-frame estimates at frame 8 (paper teaser convention, ACCUM=0).
#
# Render recipe (RoyalGL.exe, always ROYALGL_LOCK_CAMERA=1):
#   s1 exterior Cornell: fallback scene, default camera,
#      ROYALGL_FOG=0.15,0.02,0.3
#   s2 interior Cornell: fallback scene,
#      ROYALGL_CAM=0.4,1.9,3.8,0.2,0.95,-0.3, ROYALGL_FOG=0.15,0.02,0.3
#   s3 lens beam: assets/scenes/LensFog.glb (docs/make_lens_scene.py),
#      ROYALGL_CAM=2.2,1.4,3.2,0,1.2,0, ROYALGL_FOG=0.30,0.02,0.5
# Methods (prefix -> env on top of the scene env):
#   truth: RESTIR=0 ACCUM=1 EXPORT_FRAMES=4096 EXPORT_STRIDE=4096
#   pt   : RESTIR=1 ACCUM=1 TEMPORAL=0 SPATIAL=0 LIGHT=0 CONN=0, frame 1
#   uni  : RESTIR=1 ACCUM=0 LIGHT=0 CONN=0, frame 8
#   bdpt : RESTIR=0 ACCUM=1, frame 1
#   ours : RESTIR=1 ACCUM=0, frame 8
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

import flip_evaluator as flip

FR = r"C:\Users\Malte\AppData\Local\Temp\claude\C--Users-Malte-Documents-GitHub-RoyalGL\fca9cd82-8db0-4710-bc7c-ef9c683e52d3\scratchpad\frames_matrix"
OUT = r"C:\Users\Malte\Documents\GitHub\RoyalGL"
W, H = 1600, 900
LUMA = np.array([0.2126, 0.7152, 0.0722])

SCENES = [
    ("s1", "Exterior Cornell\nfog $\\sigma_s$=0.15, g=0.3",
     [(120, 180, 300, 300), (830, 210, 300, 300)]),
    ("s2", "Interior Cornell + glass duck\nfog $\\sigma_s$=0.15, g=0.3",
     [(680, 460, 340, 340), (330, 330, 300, 300)]),
    ("s3", "Lens beam\nfog $\\sigma_s$=0.30, g=0.5",
     [(640, 530, 330, 330), (620, 250, 330, 260)]),
]
METHODS = [("pt_00001", "PT, 1 frame"),
           ("bdpt_00001", "BDPT, 1 frame"),
           ("uni_00008", "ReSTIR PT, frame 8"),
           ("ours_00008", "ReSTIR BDPT (ours), frame 8")]
CROP_COLORS = ["#ff8c1a", "#20b2c8"]


def aces(x):
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0)


def load_display(path):
    a = np.fromfile(path, dtype=np.float32)
    rgb = a.reshape(H, W, 4)[:, :, :3].astype(np.float64)
    return (aces(rgb) ** (1.0 / 2.2)).astype(np.float32)  # displayed color


def gray(img):
    y = img @ LUMA
    return np.repeat(y[:, :, None], 3, axis=2).astype(np.float32)


def flip_map(ref_gray, img_gray):
    r = flip.evaluate(ref_gray, img_gray, "LDR")
    return np.asarray(r[0]), float(r[1])


def main():
    ncols = 1 + len(METHODS)
    fig_w = 7.0
    cell_w = fig_w / ncols
    full_h = cell_w * 9 / 16
    crop_h = cell_w * 0.52
    label_h = 0.15
    header_h = 0.13
    scene_h = header_h + full_h + label_h + crop_h + label_h + 0.05
    fig_h = scene_h * len(SCENES) + 0.02
    fig = plt.figure(figsize=(fig_w, fig_h), dpi=300)

    def add_ax(x, y, w, h):
        return fig.add_axes([x / fig_w, 1.0 - (y + h) / fig_h, w / fig_w, h / fig_h])

    stats = {}
    for si, (skey, stitle, crops) in enumerate(SCENES):
        ref = load_display(os.path.join(FR, f"{skey}_truth_04096.f32"))
        ref_gray = gray(ref)
        y0 = si * scene_h + header_h

        for ci in range(ncols):
            x0 = ci * cell_w
            axh = add_ax(x0, y0 - header_h, cell_w, header_h)
            axh.axis("off")
            title = "Reference (BDPT, 4096 spp)" if ci == 0 else METHODS[ci - 1][1]
            weight = "bold" if ci in (0, ncols - 1) else "normal"
            axh.text(0.5, 0.25, title, ha="center", va="center", fontsize=5.6,
                     weight=weight, family="serif")

        for ci in range(ncols):
            x0 = ci * cell_w
            if ci == 0:
                img, fmean, fmap = ref, None, None
            else:
                img = load_display(os.path.join(FR, f"{skey}_{METHODS[ci - 1][0]}.f32"))
                fmap, fmean = flip_map(ref_gray, gray(img))
                stats[(skey, METHODS[ci - 1][0])] = fmean

            ax = add_ax(x0 + 0.012, y0, cell_w - 0.024, full_h)
            ax.imshow(img, interpolation="lanczos")
            ax.set_xticks([])
            ax.set_yticks([])
            for spine in ax.spines.values():
                spine.set_linewidth(0.3)
            if ci == 0:  # crop rectangles on the reference
                for k, (cx, cy, cw, ch) in enumerate(crops):
                    ax.add_patch(mpatches.Rectangle((cx, cy), cw, ch, fill=False,
                                                     edgecolor=CROP_COLORS[k], lw=0.7))

            axl = add_ax(x0, y0 + full_h, cell_w, label_h)
            axl.axis("off")
            if ci == 0:
                axl.text(0.5, 0.5, stitle, ha="center", va="center", fontsize=5.0,
                         family="serif", style="italic", linespacing=1.15)
            else:
                axl.text(0.5, 0.6, "mean FLIP %.3f" % fmean, ha="center", va="center",
                         fontsize=5.2, family="serif")

            # crops row: two zooms per method, per-crop mean FLIP beneath
            cw2 = (cell_w - 0.024 - 0.012) / 2.0
            for k, (cx, cy, cwid, chei) in enumerate(crops):
                axc = add_ax(x0 + 0.012 + k * (cw2 + 0.012), y0 + full_h + label_h, cw2, crop_h)
                axc.imshow(img[cy:cy + chei, cx:cx + cwid], interpolation="lanczos")
                axc.set_xticks([])
                axc.set_yticks([])
                for spine in axc.spines.values():
                    spine.set_edgecolor(CROP_COLORS[k])
                    spine.set_linewidth(0.9)
                axcl = add_ax(x0 + 0.012 + k * (cw2 + 0.012), y0 + full_h + label_h + crop_h,
                              cw2, label_h)
                axcl.axis("off")
                if ci > 0:
                    cmean = float(np.mean(fmap[cy:cy + chei, cx:cx + cwid]))
                    axcl.text(0.5, 0.6, "FLIP %.3f" % cmean, ha="center", va="center",
                              fontsize=4.8, family="serif")
                    stats[(skey, METHODS[ci - 1][0], "crop%d" % k)] = cmean

    fig.savefig(os.path.join(OUT, "fig_matrix.png"), facecolor="white")
    plt.close(fig)
    print("wrote fig_matrix.png")
    for k, v in stats.items():
        print(k, "%.4f" % v)

    # Teaser (Fig. 1) images: same dumps, same display transform. Also print
    # the caption numbers (mean FLIP + linear-luminance relMSE, eps 1e-4).
    t_lin = np.fromfile(os.path.join(FR, "s1_truth_04096.f32"),
                        dtype=np.float32).reshape(H, W, 4)[:, :, :3].astype(np.float64) @ LUMA
    for src, dst in [("s1_truth_04096", "fig_exterior_ref"),
                     ("s1_uni_00008", "fig_uni_f8"),
                     ("s1_ours_00008", "fig_ours_f8")]:
        img = load_display(os.path.join(FR, src + ".f32"))
        plt.imsave(os.path.join(OUT, dst + ".png"), np.clip(img, 0, 1))
        if src != "s1_truth_04096":
            y = np.fromfile(os.path.join(FR, src + ".f32"),
                            dtype=np.float32).reshape(H, W, 4)[:, :, :3].astype(np.float64) @ LUMA
            rel = float(np.mean((y - t_lin) ** 2 / (t_lin ** 2 + 1e-4)))
            print("teaser %s: relMSE %.3f" % (src, rel))
    print("wrote teaser figures")


if __name__ == "__main__":
    main()
