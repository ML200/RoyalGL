# -*- coding: utf-8 -*-
# Schematic figures for the volumetric ReSTIR paper: the two shift mappings
# and the airlight technique split. Rendered with matplotlib to PNG.
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, Circle, Rectangle

OUT = r"C:\Users\Malte\Documents\GitHub\RoyalGL"

plt.rcParams.update({"font.size": 9, "font.family": "serif", "figure.dpi": 170})

C_BASE = "#1f77b4"    # base path
C_OFF = "#e08214"     # offset path
C_MED = "#dce9f5"     # medium fill
C_SURF = "#555555"    # surfaces
C_LIGHT = "#f2c14e"   # light
C_ANN = "#333333"


def arrow(ax, p0, p1, color, lw=1.6, style="-|>", ls="-", alpha=1.0, ms=11):
    ax.add_patch(FancyArrowPatch(p0, p1, arrowstyle=style, mutation_scale=ms,
                                 color=color, lw=lw, linestyle=ls, alpha=alpha,
                                 shrinkA=0, shrinkB=0))


def cam(ax, p, color, label, dy=-0.10):
    ax.plot(*p, marker="s", ms=7, color=color)
    ax.annotate(label, p, xytext=(p[0] - 0.02, p[1] + dy), fontsize=8, color=color)


def vol_vertex(ax, p, color, filled=True):
    ax.plot(*p, marker="o", ms=8, markerfacecolor=color if filled else "white",
            markeredgecolor=color, markeredgewidth=1.4, zorder=5)


def light(ax, p):
    ax.plot(*p, marker="*", ms=15, color=C_LIGHT, markeredgecolor="#a8842c", zorder=6)
    ax.annotate("light", p, xytext=(p[0] + 0.03, p[1] - 0.01), fontsize=8, color="#a8842c")


# ============================ figures: the two shift mappings ===============
fig1, ax1 = plt.subplots(figsize=(4.9, 3.3))
fig2, ax2 = plt.subplots(figsize=(4.9, 3.3))

for ax in (ax1, ax2):
    ax.add_patch(Rectangle((0.02, 0.05), 0.96, 0.9, facecolor=C_MED, edgecolor="none", zorder=0))
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis("off")

# ------------------------------------------- (a) replay with masks ---------
ax = ax1

camA = (0.07, 0.28)
camB = (0.07, 0.14)
# base path: cam -> v1 -> v2 -> light
v1 = (0.42, 0.47)
v2 = (0.62, 0.72)
lp = (0.87, 0.80)
cam(ax, camA, C_BASE, "pixel i")
cam(ax, camB, C_OFF, "pixel j")
arrow(ax, camA, v1, C_BASE)
arrow(ax, v1, v2, C_BASE)
arrow(ax, v2, lp, C_BASE)
vol_vertex(ax, v1, C_BASE)
vol_vertex(ax, v2, C_BASE)
light(ax, lp)

# offset path: same distances t1, t2 along new directions
d1 = np.hypot(v1[0] - camA[0], v1[1] - camA[1])
dirB = np.array([0.42 - 0.07, 0.36 - 0.14])
dirB /= np.linalg.norm(dirB)
w1 = tuple(np.array(camB) + dirB * d1)
d2 = np.hypot(v2[0] - v1[0], v2[1] - v1[1])
dir2 = np.array([0.58, 0.90]) - np.array(w1)
dir2 /= np.linalg.norm(dir2)
w2 = tuple(np.array(w1) + dir2 * d2)
arrow(ax, camB, w1, C_OFF, ls="--")
arrow(ax, w1, w2, C_OFF, ls="--")
arrow(ax, w2, lp, C_OFF, ls="--")
vol_vertex(ax, w1, C_OFF)
vol_vertex(ax, w2, C_OFF)

# equal-distance tick annotations
ax.annotate("same $t_1$", ((camA[0] + v1[0]) / 2 - 0.03, (camA[1] + v1[1]) / 2 + 0.05),
            fontsize=8, color=C_ANN)
ax.annotate("same $t_2$", ((w1[0] + w2[0]) / 2 + 0.02, (w1[1] + w2[1]) / 2 - 0.05),
            fontsize=8, color=C_ANN)
ax.annotate("mask [1,1]: both vertices\nmust stay volume scatters",
            (0.55, 0.16), fontsize=8, color=C_ANN)

# ------------------------------------------- (b) volume anchor -------------
ax = ax2

camA = (0.06, 0.30)
camB = (0.06, 0.12)
x1 = (0.34, 0.52)
anchor = (0.60, 0.60)
suf = (0.78, 0.83)
lp = (0.92, 0.72)
cam(ax, camA, C_BASE, "pixel i")
cam(ax, camB, C_OFF, "pixel j")
arrow(ax, camA, x1, C_BASE)
arrow(ax, x1, anchor, C_BASE)
vol_vertex(ax, x1, C_BASE)

# offset prefix (replayed) ends at y_{k-1}, then reconnects to the anchor
y1 = (0.30, 0.30)
arrow(ax, camB, y1, C_OFF, ls="--")
arrow(ax, y1, anchor, C_OFF, ls="--")
vol_vertex(ax, y1, C_OFF)

# anchor + unchanged suffix
ax.plot(*anchor, marker="o", ms=11, markerfacecolor="#2c8a5b",
        markeredgecolor="black", markeredgewidth=1.2, zorder=6)
ax.annotate("anchor $x_k$ (fixed\nworld space point)", (anchor[0] - 0.30, anchor[1] + 0.12),
            fontsize=8, color="#20603f")
arrow(ax, anchor, suf, "#2c8a5b", lw=2.2)
arrow(ax, suf, lp, "#2c8a5b", lw=2.2)
vol_vertex(ax, suf, "#2c8a5b")
light(ax, lp)
ax.annotate("suffix kept", ((anchor[0] + suf[0]) / 2 + 0.02, (anchor[1] + suf[1]) / 2 - 0.06),
            fontsize=8, color="#20603f")

# distance labels
ax.annotate("$d$", ((x1[0] + anchor[0]) / 2, (x1[1] + anchor[1]) / 2 + 0.03),
            fontsize=10, color=C_BASE)
ax.annotate("$d'$", ((y1[0] + anchor[0]) / 2 + 0.01, (y1[1] + anchor[1]) / 2 - 0.06),
            fontsize=10, color=C_OFF)
ax.annotate(r"$\left|\partial T / \partial x\right| = d^2 / d'^2$"
            "\n(no cosines: the anchor\nhas no orientation)",
            (0.58, 0.16), fontsize=8.5, color=C_ANN)

fig1.tight_layout()
fig1.savefig(os.path.join(OUT, "fig_shift_replay.png"), bbox_inches="tight")
plt.close(fig1)
fig2.tight_layout()
fig2.savefig(os.path.join(OUT, "fig_shift_anchor.png"), bbox_inches="tight")
plt.close(fig2)

# ============================ figure 2: airlight technique split ============
fig, ax = plt.subplots(figsize=(6.6, 3.0))
ax.add_patch(Rectangle((0.02, 0.05), 0.96, 0.9, facecolor=C_MED, edgecolor="none", zorder=0))
ax.set_xlim(0, 1)
ax.set_ylim(0, 1)
ax.axis("off")

camP = (0.06, 0.45)
anchorS = (0.86, 0.45)
vA = (0.38, 0.45)
lp = (0.55, 0.88)

# surface at d1
ax.plot([0.86, 0.86], [0.25, 0.65], color=C_SURF, lw=3)
ax.annotate("surface anchor,\ndepth $d_1$", (0.875, 0.30), fontsize=8, color=C_SURF)

cam(ax, camP, "#333333", "camera", dy=0.08)
# surface family: deterministic segment to the anchor
arrow(ax, camP, anchorS, "#777777", lw=1.4)
ax.annotate("surface family: deterministic, probability 1", (0.30, 0.36),
            fontsize=8, color="#555555")

# truncated pdf curve along the segment
t = np.linspace(0.06, 0.86, 128)
pdf = np.exp(-3.2 * (t - 0.06))
ax.plot(t, 0.52 + 0.28 * pdf, color="#1f77b4", lw=1.4)
ax.annotate(r"$p(t) = \sigma_t e^{-\sigma_t t}\, /\, (1 - e^{-\sigma_t d_1})$",
            (0.60, 0.64), fontsize=9, color="#1f77b4")

# airlight family vertex + light connection
vol_vertex(ax, vA, "#1f77b4")
ax.annotate("airlight family:\none scatter vertex per frame,\nsampled from the truncated density",
            (0.10, 0.13), fontsize=8, color="#1f77b4")
arrow(ax, vA, lp, "#1f77b4", ls="--")
light(ax, lp)

# t=1 light connection to the camera (competitor)
yv = (0.24, 0.72)
vol_vertex(ax, yv, "#e08214")
arrow(ax, lp, yv, "#e08214")
arrow(ax, yv, camP, "#e08214", ls="--")
ax.annotate("light traced $t{=}1$:\nlands anywhere on the segment,\nMIS paired with the airlight family",
            (0.05, 0.80), fontsize=8, color="#b05e00")

fig.tight_layout()
fig.savefig(os.path.join(OUT, "fig_airlight.png"), bbox_inches="tight")
plt.close(fig)

print("wrote fig_shift_replay.png, fig_shift_anchor.png, fig_airlight.png")
