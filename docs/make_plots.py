# -*- coding: utf-8 -*-
# Convergence plots for the volumetric ReSTIR paper. Exterior default
# camera, static fog Cornell scene. Four estimators:
#   bdpt  : accumulated BDPT (reference method 1)
#   ptacc : accumulated unidirectional path tracing (per frame RIS without
#           reuse; reference method 2, no bidirectional techniques)
#   uni   : ReSTIR PT (unidirectional: light tracing + vertex connections
#           off), per frame estimate (temporal reuse is its accumulation)
#   ours  : ReSTIR BDPT with volumetric shifts, per frame estimate
# Metric: mean FLIP error of the DISPLAYED LUMINANCE (grayscale) against a
# 4096 spp BDPT ground truth. Pipeline: renderer's display transform
# (tonemap.frag: exposure 1, ACES filmic, gamma 1/2.2) -> Rec. 709 luma ->
# NVIDIA flip-evaluator (LDR) on the grayscale images.
# Metric history, kept for the record (each rejected for a measured reason):
#  - linear relMSE (per channel or luma): dominated by imperceptible noise
#    in near-black regions (epsilon-sized denominators) and outlier spikes
#    the tonemapper compresses;
#  - display-referred MSE: cannot distinguish dense high frequency grain
#    (accumulated PT) from smooth correlated error (ReSTIR) - the eye
#    heavily penalizes the former;
#  - color FLIP: both ReSTIR floors share a CHROMA speckle pedestal from
#    winner-only shading of the caustic/airlight reservoir (red/green
#    wall-bounce paths), which compresses every ratio. That pedestal is a
#    genuine defect (candidate fix: extend the marginalized vector-weight
#    shading to the caustic merge), but it masks the luminance story the
#    plots are about.
# Grayscale FLIP matches side-by-side perception of the fog noise. NOTE:
# FLIP is a bounded perceptual scale - ratios are compressed by design;
# the crossing points (frames the references need to look equally close)
# carry the comparison. Raw RGBA32F dumps from ROYALGL_EXPORT_SERIES.
import glob
import os
import re

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

FR = r"C:\Users\Malte\AppData\Local\Temp\claude\C--Users-Malte-Documents-GitHub-RoyalGL\a69c44b3-956f-4345-b2bb-f7e7fdead0dd\scratchpad\frames"
OUT = r"C:\Users\Malte\Documents\GitHub\RoyalGL"
W, H = 1600, 900

MS = {"bdpt": 24.42, "ptacc": 30.03, "uni": 43.85, "ours": 49.97}  # measured


import flip_evaluator as flip

LUMA = np.array([0.2126, 0.7152, 0.0722])


def aces(x):
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0)


def load(path):
    a = np.fromfile(path, dtype=np.float32)
    rgb = a.reshape(H, W, 4)[:, :, :3].astype(np.float64)
    y = (aces(rgb) ** (1.0 / 2.2)) @ LUMA  # displayed luminance
    return np.repeat(y[:, :, None], 3, axis=2).astype(np.float32)


TRUTH = load(os.path.join(FR, "truth_04096.f32"))


def relmse(img):  # grayscale mean FLIP (name kept for the flow below)
    return float(flip.evaluate(TRUTH, img, "LDR")[1])


def series(prefix):
    out = {}
    for p in sorted(glob.glob(os.path.join(FR, prefix + "_*.f32"))):
        n = int(re.search(r"_(\d+)\.f32$", p).group(1))
        out[n] = relmse(load(p))
    ns = sorted(out)
    return np.array(ns), np.array([out[n] for n in ns])


bdpt_n, bdpt_v = series("bdpt")
bceil_n, bceil_v = series("bceil")
ptacc_n, ptacc_v = series("ptacc")
ptceil_n, ptceil_v = series("ptceil")
uni_n, uni_v = series("uni")
uceil_n, uceil_v = series("uceil")
ours_n, ours_v = series("ours")
oceil_n, oceil_v = series("oceil")

C_BDPT, C_PT, C_UNI, C_OURS = "#444444", "#999999", "#e08214", "#1f77b4"
LBL_BDPT = "BDPT, accumulated (reference)"
LBL_PT = "Path tracing, accumulated (reference)"
LBL_UNI = "ReSTIR PT (unidirectional), per frame"
LBL_OURS = "ReSTIR BDPT + volumetric shifts, per frame (ours)"

plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.3,
                     "figure.dpi": 150, "font.family": "serif"})


def joined(dense_n, dense_v, ceil_n, ceil_v):
    m = ceil_n > dense_n.max()
    return np.concatenate([dense_n, ceil_n[m]]), np.concatenate([dense_v, ceil_v[m]])


# ------------------------------------------------ plot 1: per-frame ---------
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10.6, 4.0))
for ax, lim, title in [(ax1, 24, "Frames 1 to 24"),
                       (ax2, 10, "First 10 frames")]:
    for n, v, c, lbl, mk in [(bdpt_n, bdpt_v, C_BDPT, LBL_BDPT, "o"),
                             (ptacc_n, ptacc_v, C_PT, LBL_PT, "d"),
                             (uni_n, uni_v, C_UNI, LBL_UNI, "s"),
                             (ours_n, ours_v, C_OURS, LBL_OURS, "^")]:
        m = n <= lim
        ax.plot(n[m], v[m], mk + "-", color=c, label=lbl, ms=3.5)
    ax.set_yscale("log")
    ax.set_xlabel("frame index")
    ax.set_ylabel("mean FLIP error (luminance) vs 4096 spp reference")
    ax.set_title(title, fontsize=10)
ax1.legend(fontsize=7.5, loc="upper right")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "plot_convergence.png"))
plt.close(fig)

# ------------------------------------------------ plot 2: equal time --------
fig, ax = plt.subplots(figsize=(6.8, 4.1))
t_budget = 20 * MS["ours"]
for key, dense, ceil, c, lbl, mk in [
        ("bdpt", (bdpt_n, bdpt_v), (bceil_n, bceil_v), C_BDPT, LBL_BDPT, "o"),
        ("ptacc", (ptacc_n, ptacc_v), (ptceil_n, ptceil_v), C_PT, LBL_PT, "d"),
        ("uni", (uni_n, uni_v), (uceil_n, uceil_v), C_UNI, LBL_UNI, "s"),
        ("ours", (ours_n, ours_v), (oceil_n, oceil_v), C_OURS, LBL_OURS, "^")]:
    n, v = joined(dense[0], dense[1], ceil[0], ceil[1])
    m = n * MS[key] <= t_budget
    ax.plot(n[m] * MS[key], v[m], mk + "-", color=c, ms=3.5,
            label=lbl + " (%.0f ms)" % MS[key])
ax.set_yscale("log")
ax.set_xlabel("render time [ms]")
ax.set_ylabel("mean FLIP error (luminance) vs 4096 spp reference")
ax.legend(fontsize=7.5)
fig.tight_layout()
fig.savefig(os.path.join(OUT, "plot_equal_time.png"))
plt.close(fig)

# ------------------------------------------------ plot 3: ceiling -----------
fig, ax = plt.subplots(figsize=(6.8, 4.1))
ax.plot(bceil_n, bceil_v, "o-", color=C_BDPT, ms=3, label=LBL_BDPT)
ax.plot(ptceil_n, ptceil_v, "d-", color=C_PT, ms=3, label=LBL_PT)
ax.plot(uceil_n, uceil_v, "s-", color=C_UNI, ms=3, label=LBL_UNI)
ax.plot(oceil_n, oceil_v, "^-", color=C_OURS, ms=3, label=LBL_OURS)
u_floor = float(np.mean(uceil_v[-10:]))
o_floor = float(np.mean(oceil_v[-10:]))
ax.axhline(u_floor, color=C_UNI, ls=":", lw=1)
ax.axhline(o_floor, color=C_OURS, ls=":", lw=1)
ax.annotate("unidirectional floor: %.3g" % u_floor, xy=(400, u_floor),
            xytext=(100, u_floor * 1.4), color=C_UNI, fontsize=8.5)
ax.annotate("bidirectional floor: %.3g  (%.1fx lower)" % (o_floor, u_floor / o_floor),
            xy=(400, o_floor), xytext=(100, o_floor * 1.4), color=C_OURS, fontsize=8.5)
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("frame index (log)")
ax.set_ylabel("mean FLIP error (luminance) vs 4096 spp reference")
ax.legend(fontsize=7.5, loc="lower left")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "plot_ceiling.png"))
plt.close(fig)

# ---------------------------------------- column variants for the paper -----
def col_plot(fname, curves, xlabel, xscale=None, legend_loc="upper right",
             annotate=None):
    figc, axc = plt.subplots(figsize=(4.6, 3.2))
    for x, y, c, lbl, mk in curves:
        axc.plot(x, y, mk + "-", color=c, ms=3, label=lbl, lw=1.3)
    axc.set_yscale("log")
    if xscale:
        axc.set_xscale(xscale)
    axc.set_xlabel(xlabel, fontsize=9)
    axc.set_ylabel("mean FLIP error (luminance)", fontsize=9)
    axc.tick_params(labelsize=8)
    axc.legend(fontsize=6.4, loc=legend_loc)
    if annotate:
        annotate(axc)
    figc.tight_layout()
    figc.savefig(os.path.join(OUT, fname))
    plt.close(figc)


m24 = lambda n: n <= 24
col_plot("plot_conv_col.png",
         [(bdpt_n, bdpt_v, C_BDPT, LBL_BDPT, "o"),
          (ptacc_n, ptacc_v, C_PT, LBL_PT, "d"),
          (uni_n, uni_v, C_UNI, LBL_UNI, "s"),
          (ours_n, ours_v, C_OURS, LBL_OURS, "^")],
         "frame index")

t_budget = 20 * MS["ours"]
curves_t = []
for key, dense, ceil, c, lbl, mk in [
        ("bdpt", (bdpt_n, bdpt_v), (bceil_n, bceil_v), C_BDPT, LBL_BDPT, "o"),
        ("ptacc", (ptacc_n, ptacc_v), (ptceil_n, ptceil_v), C_PT, LBL_PT, "d"),
        ("uni", (uni_n, uni_v), (uceil_n, uceil_v), C_UNI, LBL_UNI, "s"),
        ("ours", (ours_n, ours_v), (oceil_n, oceil_v), C_OURS, LBL_OURS, "^")]:
    n, v = joined(dense[0], dense[1], ceil[0], ceil[1])
    m = n * MS[key] <= t_budget
    curves_t.append((n[m] * MS[key], v[m], c, lbl + " (%.0f ms)" % MS[key], mk))
col_plot("plot_time_col.png", curves_t, "render time [ms]")


def ceil_ann(axc):
    uf = float(np.mean(uceil_v[-10:]))
    of = float(np.mean(oceil_v[-10:]))
    axc.axhline(uf, color=C_UNI, ls=":", lw=1)
    axc.axhline(of, color=C_OURS, ls=":", lw=1)
    axc.annotate("floor %.2f" % uf, (12, uf * 1.25), color=C_UNI, fontsize=7.5)
    axc.annotate("floor %.2f (%.1fx lower)" % (of, uf / of), (12, of * 1.25),
                 color=C_OURS, fontsize=7.5)


col_plot("plot_ceil_col.png",
         [(bceil_n, bceil_v, C_BDPT, LBL_BDPT, "o"),
          (ptceil_n, ptceil_v, C_PT, LBL_PT, "d"),
          (uceil_n, uceil_v, C_UNI, LBL_UNI, "s"),
          (oceil_n, oceil_v, C_OURS, LBL_OURS, "^")],
         "frame index (log)", xscale="log", legend_loc="lower left",
         annotate=ceil_ann)

# ------------------------------------------------------------- numbers ------
def at(ns, vs, n):
    i = min(np.searchsorted(ns, n), len(vs) - 1)
    return vs[i]


print("frame1  bdpt %.4g ptacc %.4g uni %.4g ours %.4g" %
      (bdpt_v[0], ptacc_v[0], uni_v[0], ours_v[0]))
for k in [5, 10, 20]:
    print("frame%-3d bdpt %.4g ptacc %.4g uni %.4g ours %.4g" %
          (k, at(bdpt_n, bdpt_v, k), at(ptacc_n, ptacc_v, k),
           at(uni_n, uni_v, k), at(ours_n, ours_v, k)))
print("floors: uni %.4g ours %.4g ratio %.2f" % (u_floor, o_floor, u_floor / o_floor))
for name, cn, cv in [("bdpt", bceil_n, bceil_v), ("ptacc", ptceil_n, ptceil_v)]:
    cross = cn[np.searchsorted(-cv, -o_floor)] if (cv < o_floor).any() else -1
    print("%s crosses ours floor at frame %s" % (name, cross))
