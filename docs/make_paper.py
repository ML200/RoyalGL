# -*- coding: utf-8 -*-
# Builds docs/volumetric_restir_paper.pdf.
# Figures: fig_exterior_ref/fig_uni_f8/fig_ours_f8 (teaser),
# fig_shift_replay/fig_shift_anchor/fig_airlight (docs/make_diagrams.py),
# plot_conv_col/plot_time_col/plot_ceil_col (docs/make_plots.py).
import os

from reportlab.lib.pagesizes import letter
from reportlab.lib.units import inch
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_JUSTIFY, TA_CENTER, TA_LEFT
from reportlab.platypus import (BaseDocTemplate, PageTemplate, Frame, Paragraph,
                                Spacer, Image, Table, TableStyle, FrameBreak,
                                NextPageTemplate, KeepTogether, PageBreak)
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.fonts import addMapping

ROOT = r"C:\Users\Malte\Documents\GitHub\RoyalGL"
OUT = os.path.join(ROOT, "docs", "volumetric_restir_paper.pdf")

FDIR = r"C:\Windows\Fonts"
pdfmetrics.registerFont(TTFont("TNR", os.path.join(FDIR, "times.ttf")))
pdfmetrics.registerFont(TTFont("TNR-B", os.path.join(FDIR, "timesbd.ttf")))
pdfmetrics.registerFont(TTFont("TNR-I", os.path.join(FDIR, "timesi.ttf")))
pdfmetrics.registerFont(TTFont("TNR-BI", os.path.join(FDIR, "timesbi.ttf")))
addMapping("TNR", 0, 0, "TNR")
addMapping("TNR", 1, 0, "TNR-B")
addMapping("TNR", 0, 1, "TNR-I")
addMapping("TNR", 1, 1, "TNR-BI")

PAGE_W, PAGE_H = letter
MARGIN = 0.75 * inch
COL_GAP = 0.24 * inch
COL_W = (PAGE_W - 2 * MARGIN - COL_GAP) / 2.0

body = ParagraphStyle("body", fontName="TNR", fontSize=9.3, leading=11.3,
                      alignment=TA_JUSTIFY, firstLineIndent=10, spaceAfter=1)
body_ni = ParagraphStyle("body_ni", parent=body, firstLineIndent=0)
bullet = ParagraphStyle("bullet", parent=body, firstLineIndent=0, leftIndent=12,
                        bulletIndent=2, spaceAfter=2)
abstract = ParagraphStyle("abstract", parent=body, fontSize=9.0, leading=10.9,
                          firstLineIndent=0, spaceAfter=4)
title_st = ParagraphStyle("title", fontName="TNR-B", fontSize=17.5, leading=21,
                          alignment=TA_CENTER, spaceAfter=6)
author_st = ParagraphStyle("author", fontName="TNR", fontSize=10.5, leading=13,
                           alignment=TA_CENTER, spaceAfter=8)
h1 = ParagraphStyle("h1", fontName="TNR-B", fontSize=10.8, leading=13,
                    spaceBefore=8, spaceAfter=3, alignment=TA_LEFT)
h2 = ParagraphStyle("h2", fontName="TNR-BI", fontSize=9.8, leading=12,
                    spaceBefore=6, spaceAfter=2, alignment=TA_LEFT)
caption = ParagraphStyle("caption", fontName="TNR", fontSize=8.0, leading=9.6,
                         alignment=TA_JUSTIFY, spaceBefore=3, spaceAfter=6,
                         textColor=colors.Color(0.15, 0.15, 0.15))
ref_st = ParagraphStyle("ref", fontName="TNR", fontSize=7.9, leading=9.4,
                        alignment=TA_LEFT, firstLineIndent=-10, leftIndent=10,
                        spaceAfter=1.5)
tcell = ParagraphStyle("tcell", fontName="TNR", fontSize=7.9, leading=9.4,
                       alignment=TA_LEFT)
tcell_b = ParagraphStyle("tcell_b", parent=tcell, fontName="TNR-B")
eq = ParagraphStyle("eq", fontName="TNR-I", fontSize=9.6, leading=12,
                    alignment=TA_CENTER, spaceBefore=4, spaceAfter=2)
where_st = ParagraphStyle("where", fontName="TNR", fontSize=8.6, leading=10.6,
                          alignment=TA_LEFT, leftIndent=16, spaceAfter=1)
code = ParagraphStyle("code", fontName="Courier", fontSize=7.0, leading=8.6,
                      alignment=TA_LEFT)


def fig(path, width):
    img = Image(os.path.join(ROOT, path))
    aspect = img.imageHeight / float(img.imageWidth)
    img.drawWidth = width
    img.drawHeight = width * aspect
    return img


def fig_row(paths, total_w, gap=4):
    w = (total_w - gap * (len(paths) - 1)) / len(paths)
    t = Table([[fig(p, w) for p in paths]],
              colWidths=[w + (gap if i < len(paths) - 1 else 0) for i in range(len(paths))])
    t.setStyle(TableStyle([
        ("LEFTPADDING", (0, 0), (-1, -1), 0),
        ("RIGHTPADDING", (0, 0), (-1, -1), gap),
        ("RIGHTPADDING", (-1, 0), (-1, -1), 0),
        ("TOPPADDING", (0, 0), (-1, -1), 0),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 0),
    ]))
    return t


def data_table(rows, col_widths, header_rows=1, header_cols=0):
    data = []
    for r, row in enumerate(rows):
        cells = []
        for c, cell in enumerate(row):
            st = tcell_b if (r < header_rows or c < header_cols) else tcell
            cells.append(Paragraph(cell, st))
        data.append(cells)
    t = Table(data, colWidths=col_widths)
    t.setStyle(TableStyle([
        ("LINEABOVE", (0, 0), (-1, 0), 0.8, colors.black),
        ("LINEBELOW", (0, header_rows - 1), (-1, header_rows - 1), 0.4, colors.black),
        ("LINEBELOW", (0, -1), (-1, -1), 0.8, colors.black),
        ("LEFTPADDING", (0, 0), (-1, -1), 2),
        ("RIGHTPADDING", (0, 0), (-1, -1), 2),
        ("TOPPADDING", (0, 0), (-1, -1), 1.5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 1.5),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
    ]))
    return t


def code_box(lines):
    cells = [[Paragraph(ln.replace(" ", "&nbsp;"), code)] for ln in lines]
    t = Table(cells, colWidths=[COL_W - 8])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), colors.Color(0.955, 0.955, 0.965)),
        ("BOX", (0, 0), (-1, -1), 0.5, colors.Color(0.6, 0.6, 0.6)),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 3),
        ("TOPPADDING", (0, 0), (-1, -1), 0.4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 0.4),
    ]))
    return t


S = []

# ================================================================= page 1 ===
S.append(Paragraph("Volume Anchored Shift Mappings for Bidirectional ReSTIR<br/>in Participating Media", title_st))
S.append(Paragraph("CLAUDE ET AL., RoyalGL", author_st))
S.append(fig_row(["fig_exterior_ref.png", "fig_uni_f8.png", "fig_ours_f8.png"],
                 PAGE_W - 2 * MARGIN))
S.append(Paragraph(
    "Fig. 1. A Cornell box immersed in a homogeneous scattering medium (σ<sub>s</sub> = 0.15, σ<sub>a</sub> = 0.02, "
    "Henyey Greenstein g = 0.3), viewed from outside so that a large part of the image shows the medium alone. "
    "<b>Left:</b> bidirectional path traced reference. <b>Middle:</b> unidirectional ReSTIR path tracing at the "
    "eighth rendered frame (1 spp per frame, spatiotemporal reuse): perceptual error 0.087, luminance variance "
    "0.78. <b>Right:</b> our bidirectional ReSTIR with volumetric shift mappings at the same frame: perceptual "
    "error 0.052, luminance variance 0.065 (twelve times lower). Scattered light along camera segments, including "
    "pixels whose rays hit no geometry, is sampled by techniques on both path ends and reused across frames.",
    caption))
S.append(FrameBreak())

# ============================================================== abstract ====
S.append(Paragraph("<b>Abstract</b>", h1))
for p in [
    "Path space reuse through shift mappings has made ReSTIR the standard approach for path traced real time "
    "rendering of surfaces; recent work extends it to subsurface random walks inside translucent objects, and "
    "ReSTIR BDPT brings reuse to the full set of bidirectional techniques on surfaces. Reuse of paths that "
    "scatter in an unbounded medium has remained open. We present a bidirectional ReSTIR system with full "
    "volumetric path reuse in global homogeneous media, covering eye paths, next event estimation, light "
    "tracing, and vertex connections.",

    "Two properties separate this setting from surface ReSTIR and from ReSTIR for subsurface scattering. First, "
    "existing volumetric shifts anchor a path at surface vertices and carry interior scattering vertices along by "
    "replay. In an unbounded medium, important path classes have no surface to anchor at. We therefore make "
    "interior volume vertices reconnection anchors. Holding a scattering point fixed in space yields a "
    "reconnection Jacobian that is a pure ratio of squared distances, without cosine or orientation terms, and a "
    "per vertex scatter classification mask makes the mapping invertible. Second, under visibility buffer "
    "anchoring the camera walk cannot scatter on its primary segment, so scattered light along that segment "
    "(airlight) is invisible to camera techniques. We show that assigning this path class exclusively to light "
    "tracing produces an estimator with unbounded variance, and introduce a truncated camera technique that "
    "samples the primary segment directly, restores bounded variance through multiple importance sampling, and "
    "extends to pixels whose rays leave the scene.",

    "The estimator matches converged bidirectional references within 0.15 percent in all configurations, and "
    "temporal reuse under a static camera reproduces per pixel resampling exactly. On an exterior view dominated "
    "by the medium, the converged per frame estimate reaches a perceptual error that accumulated unidirectional "
    "path tracing needs 320 frames to match and accumulated BDPT 48 frames, and its luminance variance floor is "
    "10.8 times below unidirectional ReSTIR path tracing.",
]:
    S.append(Paragraph(p, abstract))

# ========================================================== introduction ====
S.append(Paragraph("1&#160;&#160;Introduction", h1))
S.append(Paragraph(
    "Resampled importance sampling and its spatiotemporal extension ReSTIR [Bitterli et al. 2020] amortize the "
    "cost of finding high contribution light paths across pixels and frames. ReSTIR GI [Ouyang et al. 2021] "
    "carried reuse to indirect illumination, and ReSTIR PT [Lin et al. 2022] "
    "generalized it to full path space through shift mappings, deterministic bijections that transport a path "
    "sampled in one pixel into the domain of another. This machinery was designed for surfaces: Jacobians convert "
    "solid angles through cosines and areas, reconnection vertices carry normals and roughness, and invertibility "
    "rests on delta lobe classification. A participating medium violates each assumption. A volume scattering "
    "vertex has no normal and no roughness, its sampling density is defined per unit length, and whether the "
    "vertex exists at all is a random outcome of free flight sampling that any replay must reproduce.", body_ni))
S.append(Paragraph(
    "Volumetric reuse in ReSTIR exists at two points so far. Volumetric ReSTIR [Lin et al. 2021] resamples short "
    "scattering chains into per froxel reservoirs for emissive media, without path space shift mappings. ReSTIR "
    "SSS [Werner et al. 2024] brought shift mapped reuse to volumetric random walks for subsurface scattering "
    "inside bounded translucent objects, with reconnection and delayed reconnection shifts selected by a "
    "heuristic or by sequential resampling passes, both introduced in that work; Galazios and Moustakas [2026] "
    "re evaluate the selection heuristics. Both systems are unidirectional. Bidirectionality itself has "
    "meanwhile reached surface ReSTIR: ReSTIR BDPT [Hedstrom et al. 2025] resamples surface paths across the "
    "full bidirectional technique set, and our system builds on its architecture (Sec. 2). What remains open is "
    "the medium. No existing system reuses paths whose vertices scatter in a volume bidirectionally, none covers "
    "an unbounded medium, and none addresses the question that dominates one: which technique samples the "
    "scattered light along the primary camera segment. In the scene of Fig. 1 this airlight carries a large "
    "share of the image energy, and for pixels beside the box it is the only energy. This paper answers these "
    "questions for a global homogeneous medium inside a bidirectional ReSTIR with technique set "
    "{s=0, s=1, t=1, s&#8805;2}. Our contributions:", body))
for b in [
    "<b>Volume vertices as reconnection anchors</b> (Sec. 4.2). The first reconnection shifts that anchor inside "
    "a medium rather than at a bounding surface. In the volume measure the anchor Jacobian is the textbook "
    "cosine free ratio d&#178;/d&#8242;&#178; [Pharr et al. 2023]; the contribution is the anchor role and its "
    "invertibility, plus a phase peak criterion that extends footprint based reconnection tests to media.",
    "<b>Replay with scatter classification masks</b> (Sec. 4.1). Free flight sampling becomes replayable inside "
    "seed deterministic walks, and an 8 bit per vertex mask guarantees that base and offset paths share the same "
    "vertex structure, which makes the mapping invertible.",
    "<b>A variance analysis and estimator for airlight</b> (Sec. 5). Under visibility buffer anchoring, granting "
    "light tracing exclusive responsibility for airlight yields an unbounded second moment. A truncated camera "
    "technique restores bounded variance and covers pixels without any surface anchor.",
    "<b>A volumetric bidirectional system</b> (Secs. 3, 6, 7). On the architecture of ReSTIR BDPT [Hedstrom "
    "et al. 2025], one substitution extends recursive MIS to mixed surface and volume paths, volume vertices "
    "enter the light vertex cache, light selection is made replay stable under camera motion, and temporal "
    "history is re paired by fog parallax where surface anchors disappear.",
]:
    S.append(Paragraph(b, bullet, bulletText="•"))

# ================================================== background/related ======
S.append(Paragraph("2&#160;&#160;Background and related work", h1))
S.append(Paragraph(
    "<b>GRIS and shift mappings.</b> Generalized resampled importance sampling [Lin et al. 2022] resamples "
    "candidates from other domains by mapping them through bijective shifts T with weights", body_ni))
S.append(Paragraph("w = m(T(x)) &#183; p&#770;(T(x)) &#183; W &#183; |&#8706;T/&#8706;x| ,", eq))
S.append(Paragraph(
    "where m is a multiple importance sampling weight that partitions unity over the techniques able to produce "
    "the mapped path, p&#770; is the resampling target function, W is the unbiased contribution weight of the "
    "input sample, and |&#8706;T/&#8706;x| is the Jacobian of the shift. The hybrid shift of ReSTIR PT replays a "
    "path from its random number seed until a vertex pair satisfies reconnection criteria, then reconnects to the "
    "cached vertex. Shift mappings originate in gradient domain rendering [Kettunen et al. 2015], and Wyman "
    "et al. [2023] survey their role in ReSTIR. Unbiasedness constrains the weights and the invertibility of T; "
    "it does not constrain variance, which is the subject of Sec. 5.", body_ni))
S.append(Paragraph(
    "<b>Bidirectional ReSTIR.</b> ReSTIR BDPT [Hedstrom et al. 2025] brought bidirectional path tracing into "
    "GRIS for surface scenes: resampling is formulated in an extended path space that records the sampling "
    "technique, a bidirectional hybrid shift maps paths between pixels, and light traced caustic paths, which no "
    "camera anchored shift can transport, accumulate in a dedicated caustics reservoir. Our system stands on "
    "this architecture and inherits the two reservoir layout, the technique aware resampling, and the free "
    "landing replay of caustic class paths (Sec. 7). Everything the medium touches is new: sampling techniques "
    "whose vertices lie in the volume and their MIS (Sec. 3), shifts that replay free flight decisions and "
    "anchor at volume vertices (Sec. 4), the airlight family and its pairing against t=1 (Sec. 5), and replay "
    "stable light selection with temporal pairing where no surface anchor exists (Sec. 6).", body))
S.append(Paragraph(
    "<b>Volumetric transport.</b> Radiative transfer attenuates radiance along segments by transmittance and adds "
    "scattering weighted by σ<sub>s</sub> and a phase function. Bidirectional and many vertex methods generalize "
    "to media [Georgiev et al. 2013; K&#345;iv&#225;nek et al. 2014], and null scattering formulations extend path integrals to heterogeneous "
    "media [Nov&#225;k et al. 2018]. Gradient domain methods have shifted volumetric paths in offline settings "
    "[Gruson et al. 2018].", body))
S.append(Paragraph(
    "<b>Volumetric reuse in ReSTIR.</b> Table 1 places the existing systems in this design space. Volumetric "
    "ReSTIR [Lin et al. 2021] targets emissive volumes with froxel reservoirs and no path space shifts. ReSTIR "
    "SSS [Werner et al. 2024] is the closest related method: it reuses camera paths that contain volumetric "
    "random walks through translucent objects. Its anchors are surface vertices, the entry and exit interactions "
    "of the walk, and the interior volume vertices ride along by replay. This design does not transfer to an "
    "unbounded medium. Airlight paths have no entry surface, and their first vertex is created by free flight "
    "sampling from the camera itself. Our anchored shift removes the surface from the anchor role, and our "
    "airlight technique covers the path class that exists only when the medium surrounds the camera. Both "
    "volumetric systems are unidirectional, while our system reuses light subpaths whose vertices may themselves "
    "be volume scattering events. The results in Sec. 8 show that this bidirectionality provides most of the "
    "quality in media.", body))

S.append(Spacer(1, 4))
S.append(data_table(
    [["", "Vol. ReSTIR [Lin 21]", "ReSTIR SSS [Werner 24]", "ReSTIR BDPT [Hedstrom 25]", "Ours"],
     ["Medium", "emissive volumes", "bounded objects (SSS)", "none (surface scenes)", "global homogeneous fog"],
     ["Reuse", "froxel RIS, no shifts", "GRIS shift mappings", "GRIS, technique aware path space",
      "GRIS shift mappings"],
     ["Directions", "camera only", "camera only", "camera and light",
      "camera and light {s=0, s=1, t=1, s&#8805;2}"],
     ["Shift anchor", "&#8212;", "surface entry and exit; interior walk replayed",
      "surface vertices, hybrid shift on both path ends",
      "surface <i>and</i> interior volume vertices"],
     ["Anchor Jacobian", "&#8212;", "surface, cos &#183; d&#178; ratios", "surface, cos &#183; d&#178; ratios",
      "volume measure, d&#178;/d&#8242;&#178;"],
     ["Inversion test", "&#8212;", "shift selection [Werner 24; Galazios 26]", "technique and delta classes",
      "scatter classification masks"],
     ["Airlight", "single scattering (froxels)", "n/a (interior media)", "n/a (no medium)",
      "truncated camera + t=1, MIS paired"],
     ["Rays without geometry", "n/a", "n/a", "n/a", "covered, temporal + spatial reuse"]],
    [COL_W * 0.19, COL_W * 0.175, COL_W * 0.21, COL_W * 0.20, COL_W * 0.225],
    header_rows=1, header_cols=1))
S.append(Paragraph(
    "Table 1. Path reuse systems closest to ours. ReSTIR SSS provides shift mapped reuse of volumetric walks, "
    "anchored at the bounding surfaces of the medium and restricted to camera paths; ReSTIR BDPT provides "
    "bidirectional reuse for surface scenes. Neither reuses paths that scatter in an unbounded medium around "
    "the camera.", caption))

# ======================================================== medium sampling ===
S.append(Paragraph("3&#160;&#160;Sampling a homogeneous medium", h1))
S.append(Paragraph(
    "For a homogeneous medium with scattering coefficient σ<sub>s</sub>, absorption coefficient σ<sub>a</sub>, "
    "and extinction σ<sub>t</sub> = σ<sub>s</sub> + σ<sub>a</sub>, transmittance and free flight sampling are "
    "analytic:", body_ni))
S.append(Paragraph(
    "Tr(d) = e<super>&#8722;σ&#8202;t&#8202;d</super> ,&#160;&#160;&#160;&#160; "
    "p(t) = σ<sub>t</sub> e<super>&#8722;σ&#8202;t&#8202;t</super> ,&#160;&#160;&#160;&#160; "
    "t(u) = &#8722;ln(1 &#8722; u) / σ<sub>t</sub> .", eq))
S.append(Paragraph(
    "The sampling is exact in the distance dimension. Passing a segment of length d has probability Tr(d), which "
    "cancels the transmittance of the integrand, and a scattering event with density σ<sub>t</sub>Tr(t) cancels "
    "all medium factors except the single scattering albedo. With exact Henyey Greenstein importance sampling, a "
    "volume vertex adds no variance beyond its directional choice. One free flight value is drawn per path "
    "segment from a dedicated stream indexed by vertex and purpose, so shifts can replay the decision (Sec. 4.1). "
    "Connection techniques evaluate the path at a volume vertex before an outgoing direction exists, so the "
    "factor 1/σ<sub>t</sub> of the distance density is applied at arrival, and each connection supplies its own "
    "σ<sub>s</sub> times phase term.", body))
S.append(Paragraph(
    "<b>MIS in media.</b> Recursive MIS quantities of the dVCM and dVC form [Georgiev et al. 2012] convert "
    "probability densities between measures with per vertex factors cos/d&#178;; volumetric bidirectional and "
    "many vertex methods perform the analogous conversions in media [Georgiev et al. 2013; K&#345;iv&#225;nek "
    "et al. 2014]. At a volume vertex the conversion target is the volume measure. A directional density "
    "p<sub>ω</sub> at the predecessor spreads over d&#178;, and the free flight sampler contributes its density "
    "σ<sub>t</sub>Tr(d) per unit length; with transmittance excluded from the ratios (below), the surviving per "
    "vertex factor replaces the cosine by σ<sub>t</sub>:", body))
S.append(Paragraph(
    "p<sub>area</sub> = p<sub>ω</sub> &#183; cos / d&#178; &#160;&#160;&#8594;&#160;&#160; "
    "p<sub>volume</sub> = p<sub>ω</sub> &#183; σ<sub>t</sub> / d&#178; .", eq))
S.append(Paragraph(
    "One recursion then covers both vertex types, and the symmetry of the phase function makes reverse "
    "probabilities available without extra state. Transmittance is excluded from all MIS ratios: every technique "
    "omits Tr of its own connection segment, which keeps the weights a valid partition of unity at a small cost "
    "in weight optimality inside dense media.", body))

# ========================================================== shift mappings ==
S.append(Paragraph("4&#160;&#160;Volumetric shift mappings", h1))
S.append(Paragraph("4.1&#160;&#160;Replay with scatter classification masks", h2))
S.append(KeepTogether([
    fig("fig_shift_replay.png", COL_W),
    Paragraph(
        "Fig. 2. Replay. The offset path (orange) re runs the samplers of the base path (blue) from its seed. "
        "Free flight distances are identical because the medium is homogeneous, so volume vertices transport "
        "rigidly along the new directions. The stored mask requires every replayed vertex to keep its scatter "
        "classification.", caption),
]))
S.append(Paragraph(
    "The base mapping is random replay: the offset path re runs the samplers of the base path from its stored "
    "seed under the destination pixel. In a medium this includes the free flight values. A replayed value u "
    "produces the same distance t(u) along the offset ray, so volume vertices transport rigidly (Fig. 2). What "
    "can change is the classification. The surface distance of an offset segment may cross t, which turns a "
    "scattering event into a surface hit or the reverse, and every stored index of the reservoir would then refer "
    "to a different vertex structure. We store an 8 bit volume mask next to the existing delta mask, one bit per "
    "path vertex, and reject any shift whose replayed classification disagrees. The test costs one comparison "
    "per vertex and is exactly the condition that makes the mapping invertible on its domain. Listing 1 shows "
    "the additions relative to the GRIS prefix replay.", body_ni))
S.append(Spacer(1, 3))
S.append(KeepTogether([
    code_box([
        "procedure ReplayPrefix(seed, masks, dstPixel):",
        "  x[0] = primary vertex of dstPixel",
        "  for k = 1 .. r-1:",
        "    dSurf = trace(x[k-1], w[k])",
        "+   t = -ln(1 - u[k]) / sigma_t  // stream (k,DIST)",
        "+   isVol = (t < dSurf)",
        "+   if isVol != volumeMask[k]: fail  // structure",
        "    x[k] = isVol ? x[k-1] + t*w[k] : surface hit",
        "    if isDelta(x[k]) != deltaMask[k]: fail",
        "+   f *= Tr(min(t, dSurf))",
        "+   p *= isVol ? sigma_t * Tr(t) : Tr(dSurf)",
        "    w[k+1] = isVol ? SampleHG(k) : SampleBSDF(k)",
    ]),
    Paragraph(
        "Listing 1. Prefix replay. Lines marked + are the volumetric additions to the GRIS procedure: the free "
        "flight replay, the classification test against the stored mask, and the segment factors entering the "
        "numerator f and the replayed density p.", caption),
]))

S.append(Paragraph("4.2&#160;&#160;Volume vertices as reconnection anchors", h2))
S.append(KeepTogether([
    fig("fig_shift_anchor.png", COL_W),
    Paragraph(
        "Fig. 3. Reconnection at a volume anchor. The scattering vertex x<sub>k</sub> stays fixed in space while "
        "the prefix moves. The connection edge changes length from d to d&#8242;, the phase function and the "
        "transmittance of the edge are re evaluated, and the suffix (green) is kept. The Jacobian is "
        "d&#178;/d&#8242;&#178;.", caption),
]))
S.append(Paragraph(
    "Reconnection shifts keep an expensive path suffix and rebuild only the connection to a moved prefix. Prior "
    "work reserves the anchor role for surface vertices. We let an interior volume scattering vertex "
    "x<sub>k</sub> serve as the anchor (Fig. 3). A volume vertex has no orientation that could disagree with a "
    "new arrival direction and no roughness classification. The shift holds x<sub>k</sub> fixed and re evaluates "
    "everything that depends on the new arrival: the transmittance of the connection edge, the scatter of the "
    "predecessor toward the anchor, and the phase function of the anchor between the new arrival and its cached "
    "suffix direction. The cached MIS recursions extend unchanged under the substitution of Sec. 3.", body_ni))
S.append(Paragraph("<b>Jacobian.</b> Parametrize the base vertex x<sub>k</sub> by its generating variables at "
                   "the predecessor: a direction ω and a flight distance t. The vertex sweeps the volume element",
                   body))
S.append(Paragraph("dV = t&#178; dω dt .", eq))
S.append(Paragraph(
    "The shift T holds x<sub>k</sub> fixed while the predecessor moves, so the same point is parametrized by "
    "(ω&#8242;, t&#8242;) with t&#8242; = ||x<sub>k</sub> &#8722; y<sub>k&#8722;1</sub>||. Because dV does not "
    "change,", body_ni))
S.append(Paragraph(
    "t&#178; dω dt = t&#8242;&#178; dω&#8242; dt&#8242; &#160;&#160;&#8658;&#160;&#160; "
    "|&#8706;T/&#8706;x| = t&#178; / t&#8242;&#178; = d&#178; / d&#8242;&#178; ,", eq))
S.append(Paragraph("where", body_ni))
S.append(Paragraph("d = distance from the base predecessor x<sub>k&#8722;1</sub> to the anchor,", where_st))
S.append(Paragraph("d&#8242; = distance from the offset predecessor y<sub>k&#8722;1</sub> to the anchor.", where_st))
S.append(Paragraph(
    "The surface version of this factor carries an additional cosine ratio because a surface pins the point to a "
    "two dimensional set whose area element projects with the cosine of the arrival direction. A point in a "
    "medium has no such projection; the cosine free conversion between solid angle and volume measures is the "
    "standard one [Pharr et al. 2023]. The new element is not the measure but the anchor role: no prior GRIS "
    "shift reconnects at a vertex interior to a medium, and the masks of Sec. 4.1 are what keep such an anchor "
    "invertible. Freed of the cosine, the anchor is immune to the grazing angle Jacobian growth of "
    "surface reconnection. The free flight density does not appear: the change of measure is a property of the "
    "map, not of the sampler. Densities enter only the contribution weights and the MIS ratios, which is why the "
    "anchored mapping remains well defined in media whose distance sampling cannot be replayed, such as "
    "heterogeneous media sampled by tracking. Listing 2 shows the reconnection step.", body))
S.append(Spacer(1, 3))
S.append(KeepTogether([
    code_box([
        "procedure Reconnect(y[r-1], anchor x[r]):",
        "  dNew = |x[r] - y[r-1]|",
        "+ f *= Tr(dNew)              // transmittance",
        "+ if isVolume(x[r]):",
        "+   f *= sigma_s*HG(wIn',wSuf)  // phase re eval",
        "+   J *= d*d/(dNew*dNew)     // volume measure",
        "  else:",
        "    f *= BSDF(x[r], wIn', wSuf)",
        "    J *= (cos'/cos)*d*d/(dNew*dNew) // surface",
    ]),
    Paragraph(
        "Listing 2. Reconnection at the anchor. For a volume anchor the cosine ratio disappears and the phase "
        "function replaces the BSDF; the transmittance of the new edge is analytic.", caption),
]))
S.append(Paragraph(
    "<b>Acceptance criteria.</b> Reconnection close to a density peak is harmful, so we extend footprint based "
    "criteria [Lin et al. 2026] to media. The forward ray footprint test keeps its form with a unit cosine. The "
    "inverse test bounds the directional density of the anchor by the peak of the phase function,", body))
S.append(Paragraph(
    "p<sub>max</sub> = (1 + |g|) / (4π (1 &#8722; |g|)&#178;) ,", eq))
S.append(Paragraph(
    "reconnect only if&#160;&#160; d&#178; &#8805; T &#183; p<sub>max</sub> ,", eq))
S.append(Paragraph(
    "with T the standard fraction of the primary pixel footprint. For isotropic to moderately forward media the "
    "phase lobe is broad, so volume vertices are accepted early and often. They are convenient anchors by "
    "construction: no geometric singularities, no orientation flips, no roughness classes.", body_ni))

# ================================================================ airlight ==
S.append(Paragraph("5&#160;&#160;Airlight", h1))
S.append(KeepTogether([
    fig("fig_airlight.png", COL_W),
    Paragraph(
        "Fig. 4. The two camera families of a pixel. The surface family reaches the visibility buffer anchor "
        "with probability one. The airlight family samples one scattering vertex on the primary segment from the "
        "truncated free flight density and continues the walk from there. Light traced t=1 paths land anywhere "
        "on the segment and are weighted against the airlight family by multiple importance sampling.", caption),
]))
S.append(Paragraph(
    "Anchoring candidates at a deterministic visibility buffer means the camera walk never scatters between the "
    "camera and the first surface. Scattered light along this segment, called airlight, is then invisible to all "
    "camera techniques. A bidirectional system offers an apparent solution: vertices of light subpaths connect "
    "to the camera (t=1) and land in the pixel that sees them, so airlight is covered by light tracing, and "
    "since no other technique samples the class, its MIS weight is one. This partition is valid and its "
    "estimator is unusable, for a reason that whole image averages do not reveal.", body_ni))
S.append(Paragraph(
    "<b>Variance.</b> A t=1 connection from a scattering vertex at distance d to the camera carries an image to "
    "volume factor proportional to 1/d&#178;. The density with which light subpaths produce such vertices does "
    "not depend on d. Over the cone of points in front of a pixel, with volume element proportional to "
    "d&#178;&#8202;dd, the first and second moments of the estimator behave as", body))
S.append(Paragraph(
    "E[X] ~ &#8747; d<super>&#8722;2</super> d&#178; dd &lt; &#8734; ,&#160;&#160;&#160;&#160; "
    "E[X&#178;] ~ &#8747; d<super>&#8722;4</super> d&#178; dd = "
    "&#8747; d<super>&#8722;2</super> dd &#8594; &#8734; &#160;&#160;(d &#8594; 0) .", eq))
S.append(Paragraph(
    "The mean is correct while the variance diverges at the camera. Classical bidirectional path tracing "
    "contains the same singularity and is unaffected, because its eye paths also sample scattering on the "
    "primary segment and the balance heuristic sends the t=1 weight to zero as d approaches zero. A volumetric "
    "ReSTIR must recreate that competing technique.", body))
S.append(Paragraph(
    "<b>The truncated camera family.</b> We split the camera technique of a pixel into two families with "
    "disjoint supports (Fig. 4). The surface family is the standard anchored walk and keeps probability one, so "
    "behavior without a medium is unchanged. The airlight family is a second candidate walk whose first vertex "
    "samples the primary segment from the exponential conditioned on scattering before the anchor at depth "
    "d<sub>1</sub>:", body))
S.append(Paragraph(
    "p(t) = σ<sub>t</sub> e<super>&#8722;σ&#8202;t&#8202;t</super> / (1 &#8722; "
    "e<super>&#8722;σ&#8202;t&#8202;d&#8321;</super>) &#160;&#160; on [0, d<sub>1</sub>) ,", eq))
S.append(Paragraph(
    "inverted in closed form. Every pixel scatters once per frame, and the walk continues through the standard "
    "candidate blocks (next event estimation, vertex connections, emitter hits) from the volume vertex onward, "
    "feeding the same reservoir.", body_ni))
S.append(Paragraph(
    "<b>Weight derivation.</b> An airlight path whose first scattering vertex lies at distance d from the camera "
    "is sampled by exactly two techniques, the truncated camera family and a light traced t=1 connection, so the "
    "balance heuristic [Veach 1997] weights the light traced path by ω = p<sub>L</sub>&#8202;/(p<sub>L</sub> + "
    "p<sub>C</sub>) = 1/(1 + w<sub>L</sub>), with w<sub>L</sub> = p<sub>C</sub>&#8202;/p<sub>L</sub> the density "
    "ratio of the two techniques at the same path; the camera family receives the complementary weight. "
    "The camera family reaches the vertex once per pixel: the factor i&#178; converts the image plane density to "
    "solid angle, division by d&#178; converts solid angle to the volume measure, and the truncated free flight "
    "density contributes σ<sub>t</sub>&#8202;/(1 &#8722; Tr(d&#8321;)) per unit length once the transmittance of "
    "the camera segment leaves the ratio under the convention of Sec. 3. The light tracer generates the vertex "
    "with N<sub>L</sub> subpaths per frame, and the density of its remaining chain relative to the camera suffix "
    "accumulates through the recursion of Sec. 3 into dVCM + p<sub>rev</sub>&#8202;dVC, exactly as in a vertex "
    "connection weight. Together, the weight of a light traced t=1 airlight path becomes", body))
S.append(Paragraph(
    "ω = 1 / (1 + w<sub>L</sub>) ,&#160;&#160;&#160;&#160; w<sub>L</sub> = "
    "i&#178; σ<sub>t</sub> / ( d&#178; (1 &#8722; Tr(d<sub>1</sub>)) N<sub>L</sub> ) &#183; "
    "( dVCM + p<sub>rev</sub> dVC ) ,", eq))
S.append(Paragraph("where", body_ni))
S.append(Paragraph("i&#178; converts image plane area to solid angle at the camera,", where_st))
S.append(Paragraph("d is the distance from the scattering vertex to the camera,", where_st))
S.append(Paragraph("d<sub>1</sub> is the primary surface depth of the pixel the connection lands in,", where_st))
S.append(Paragraph("N<sub>L</sub> is the number of light subpaths per frame,", where_st))
S.append(Paragraph("dVCM, dVC are the recursive MIS quantities of the light subpath, and p<sub>rev</sub> is the "
                   "reverse phase probability.", where_st))
S.append(Paragraph(
    "As d approaches zero, ω is proportional to d&#178; and the weighted contribution stays bounded. This is the "
    "same mechanism that protects bidirectional path tracing.", body_ni))
S.append(Paragraph(
    "<b>Shifting the airlight family.</b> The first vertex replays by sliding along the destination ray: the "
    "stored value u is inverted under the destination depth d<sub>1</sub>&#8242;, the truncated densities enter "
    "both replayed density products, and a static camera reproduces the path with Jacobian one. Later vertices "
    "reconnect through the anchors of Sec. 4.2 or replay under the masks of Sec. 4.1, with bit zero of the mask "
    "marking the family. Light traced airlight keeps the free landing replay of caustic paths, and only its "
    "weight ω changes.", body_ni))
S.append(Paragraph(
    "<b>Rays without geometry.</b> For a primary miss, d<sub>1</sub> is unbounded and the truncation factor "
    "tends to one, so the family degenerates to the plain exponential. Pixels without any surface anchor sample "
    "airlight, receive t=1 landings, reuse temporally through the fog reprojection of Sec. 6, and reuse "
    "spatially through a dedicated fog cluster (Sec. 7). The exterior viewpoint of Fig. 1 exercises exactly "
    "this case.", body_ni))

# ===================================================== temporal behavior ====
S.append(Paragraph("6&#160;&#160;Temporal reuse of volumetric paths", h1))
S.append(Paragraph(
    "<b>Replay stable light selection.</b> Reuse of light traced paths replays the light subpath under the "
    "destination frame. A light selection that depends on the camera, such as a camera anchored tree descent, "
    "re descends from the destination camera during replay, and camera motion can then select a different light "
    "for the same seed. We sample the light subpath emitter from a camera independent power distribution with a "
    "single uniform value and a binary search. Replayed paths become identities in world space under camera "
    "motion, and the sampled selection probability equals the probability used in the MIS evaluation. Next event "
    "estimation on the camera side keeps its camera aware selection, since its light points are stored in the "
    "reservoir and never re selected by shifts.", body_ni))
S.append(Paragraph(
    "<b>Fog parallax pairing.</b> Temporal reuse pairs each pixel with one previous pixel, classically found by "
    "reprojecting the surface anchor and validated by surface agreement. Fog content lies in front of the "
    "anchor and moves with different parallax. Where the surface validation fails, for example in disocclusion "
    "regions under camera translation, the pixel is re paired by reprojecting the expected scattering depth of "
    "the truncated free flight distribution,", body))
S.append(Paragraph(
    "E[t] = 1/σ<sub>t</sub> &#8722; d<sub>1</sub> Tr(d<sub>1</sub>) / (1 &#8722; Tr(d<sub>1</sub>)) ,", eq))
S.append(Paragraph(
    "a closed form function of the depth buffer alone. Pairing must not depend on realized reservoir content, "
    "or the expectation of the MIS weights no longer sums to one; this quantity satisfies that requirement. The "
    "fog pairing reuses only the airlight family, symmetrically in both merge directions, because that family re "
    "anchors to the destination ray and never references the surface. Pixels without geometry use the same "
    "reprojection with the untruncated mean and validate against other such pixels.", body_ni))
S.append(Paragraph(
    "<b>Validation status.</b> The quantitative evaluation of Sec. 8 uses a static camera, under which the "
    "pairing mechanisms above never trigger. Camera motion is exercised by the identity assertions of Sec. 7 "
    "and by the qualitative orbit tests of Sec. 8; a quantitative moving camera study, in particular of "
    "disocclusion where fog pairing activates, remains open (Sec. 9).", body))

# =========================================================== implementation =
S.append(Paragraph("7&#160;&#160;Implementation", h1))
S.append(Paragraph(
    "We integrate the medium into a wavefront renderer (OpenGL compute, GPU driven indirect dispatch, software "
    "BVH traversal) whose ReSTIR passes are split at every ray boundary. Per pixel there are two reservoirs, one "
    "for camera anchored paths and one for caustic class paths, following the caustics reservoir design of "
    "ReSTIR BDPT [Hedstrom et al. 2025], and the medium adds no further reservoirs. Spatial candidates are drawn "
    "with the antithetic stratified pattern of Sala&#252;n et al. [2025], clustered by primary instance and "
    "material. Pixels whose rays hit no geometry form a dedicated fog cluster: their merges restrict to the "
    "airlight family, which re anchors to the destination ray, and contribute to the marginalized shading "
    "estimate only, while the reservoir carried into the next frame remains the temporal chain's own. Feeding "
    "spatial aggregates back through the temporal history of these pixels measurably drains the light traced "
    "path classes that cannot shift between rays; shading only mixing is unbiased on top of the per pixel "
    "temporal chain. Each "
    "ray cast round consumes one free flight value from a stream indexed by vertex; if the sampled distance "
    "undercuts the surface hit, the shade round synthesizes a volume vertex and sets its mask bit. The vertex "
    "then runs the same candidate blocks as a surface vertex with three substitutions: the phase function "
    "replaces the BSDF, contribution cosines become one, and MIS measure factors become σ<sub>t</sub>. Light "
    "subpaths mirror this, and volume vertices enter the connection vertex cache under a sentinel material "
    "identifier.", body_ni))
S.append(Paragraph(
    "The airlight family runs as a second walk per pixel: the host clears the queue control block after the "
    "surface walk and dispatches the initialization kernel in a second mode that seeds the truncated scattering "
    "vertex, while the reservoir chain of the pixel persists across the two walks. The second walk draws from a "
    "derived seed stored in the replay seed slot, so shift kernels replay it without special cases; mask bit "
    "zero selects the truncated inversion at shift time. The path reservoir stays at 160 bytes: the technique "
    "word stores s and t in four bits each under a path length cap of eight, which frees the byte for the "
    "volume mask.", body))
S.append(Paragraph(
    "Replay rounds re derive the free flight decision on the same stream and compare it with the mask; segment "
    "factors enter the numerator and the replayed density exactly as at creation. Identity shifts therefore "
    "reproduce candidates bit for bit, and our regression suite asserts that temporal reuse under a static "
    "camera equals per pixel resampling to the last digit. This determinism is a property of the implementation "
    "under test, whose wavefront passes merge each pixel in a fixed order; the suite verifies it on the driver "
    "and GPU of Sec. 8, and we do not claim it portably across compilers or hardware. All reuse merges are "
    "unchanged by the volumetric extension.", body))

# ================================================================= results ==
S.append(Paragraph("8&#160;&#160;Results", h1))
S.append(Paragraph(
    "All measurements run at 1600&#215;900 on an RTX 5090, and all scenes are static. Figs. 5&#8211;7 and "
    "Table 2 use the scene of Fig. 1, with the camera outside the box so that a large image region shows the "
    "medium alone, including rays that hit no geometry; Fig. 8 extends the comparison to two further scenes. "
    "Ground truth is bidirectional path tracing at 4096 samples per pixel. The error metric is the mean "
    "FLIP error of the displayed luminance: each image passes through the exact display transform of the "
    "renderer (exposure, ACES tone mapping, gamma 1/2.2) and the Rec. 709 luminance of the result is compared "
    "with FLIP, which models the visibility of differences to a human observer. Mean squared metrics on linear "
    "radiance cannot distinguish dense high frequency grain from smooth, temporally reused error, although the "
    "two differ strongly in perceived quality; where a variance statement is useful we additionally report "
    "relative MSE of linear luminance. FLIP is a bounded perceptual scale, so ratios between methods are "
    "compressed by design and the frame counts at which methods reach equal error carry the comparison. We "
    "compare four estimators. Two are progressive references with frame averaging: bidirectional path tracing "
    "(BDPT, 21 ms per sample) and unidirectional path tracing (29 ms). Two are ReSTIR estimators shown as per "
    "frame estimates without any frame averaging, since temporal reuse is their accumulation: ReSTIR PT, our "
    "system with light tracing and vertex connections disabled (55 ms), and the full bidirectional system with "
    "volumetric shifts (59 ms).", body_ni))
S.append(KeepTogether([
    fig("plot_conv_col.png", COL_W),
    Paragraph(
        "Fig. 5. Perceptual error per frame index. Our per frame estimate has the lowest error of all four "
        "methods across the whole range: at frame 1, mean FLIP 0.118 versus 0.169 for ReSTIR PT and 0.175 for "
        "one sample of BDPT; by frame 10 it reaches 0.049, where ReSTIR PT is at 0.083. In linear luminance "
        "variance the same frame 10 gap is a factor of eleven (0.060 versus 0.65).", caption),
]))
S.append(KeepTogether([
    fig("plot_time_col.png", COL_W),
    Paragraph(
        "Fig. 6. Perceptual error at equal render time over the first twenty frames of the slowest method. Our "
        "per frame estimate leads every method until roughly one second, where accumulated BDPT catches up on "
        "this static scene; the unidirectional pair stays well above both throughout. Any camera or scene "
        "motion restarts the accumulated references from the top of the plot, while per frame reuse restarts "
        "near its floor.", caption),
]))
S.append(KeepTogether([
    fig("plot_ceil_col.png", COL_W),
    Paragraph(
        "Fig. 7. The per frame error floor. ReSTIR per frame estimates flatten when temporal reuse saturates "
        "the confidence cap (here 8): ReSTIR PT at mean FLIP 0.076, ours at 0.045. The same floors differ by a "
        "factor of 10.8 in linear luminance variance; the perceptual scale is bounded and compresses ratios. "
        "Accumulated path tracing needs 320 frames to reach our per frame floor and accumulated BDPT 48 frames, "
        "after which frame averaging keeps descending, as expected on a static scene.", caption),
]))

# Scene/method matrix as a full-width figure at the top of the next Results
# page; the remaining Sec. 8 text flows in two columns beneath it.
S.append(NextPageTemplate("Matrix"))
S.append(PageBreak())
S.append(fig("fig_matrix.png", PAGE_W - 2 * MARGIN))
S.append(Paragraph(
    "Fig. 8. Scene and method matrix. Three fog scenes rendered by four estimators without frame averaging: "
    "unidirectional path tracing (one sample), bidirectional path tracing (one sample), ReSTIR PT (per frame "
    "estimate at frame 8), and our bidirectional ReSTIR with volumetric shifts (frame 8). Numbers are mean FLIP "
    "of the displayed luminance against a converged BDPT reference at 4096 samples per pixel; insets zoom the "
    "regions marked on the reference, with per crop means. <b>Top:</b> the exterior view of Fig. 1 "
    "(σ<sub>s</sub> = 0.15, σ<sub>a</sub> = 0.02, g = 0.3). <b>Middle:</b> an interior view of the same box "
    "with the glass duck. <b>Bottom:</b> a biconvex glass lens under a small snooted emitter in denser forward "
    "scattering fog (σ<sub>s</sub> = 0.30, σ<sub>a</sub> = 0.02, g = 0.5): the lens focuses the light into a "
    "converging volumetric caustic with a floor caustic at the focus, and the lens shadow rings the spot. The "
    "beam reaches the camera almost exclusively through scattering vertices connected to light subpaths, so "
    "unidirectional per frame estimates barely detect it, while the bidirectional reservoirs resolve both the "
    "beam and its focus.", caption))
S.append(NextPageTemplate("Later"))
S.append(FrameBreak())
S.append(Paragraph(
    "<b>Unbiasedness.</b> Table 2 lists converged means. Configurations without reuse, with temporal reuse "
    "only, and with spatial reuse only agree with the per pixel resampling baseline; temporal reuse under a "
    "static camera reproduces it exactly. The complete configuration lies within 0.09 percent of the "
    "bidirectional reference, absorption only media isolate the transmittance handling, and the exterior "
    "viewpoint exercises airlight on rays without geometry.", body_ni))
S.append(Paragraph(
    "<b>Replay and anchored reconnection.</b> In a homogeneous medium the two mappings of Sec. 4 perform "
    "equivalently in our tests, including under camera orbit and with g = 0.7. This follows from the exactness "
    "of exponential replay: distances are invariant under the shift, so replayed volumetric prefixes transport "
    "rigidly and rarely fail classification. The anchored mapping is motivated by three properties. Its "
    "Jacobian never references the distance density, so it remains valid in media where distances cannot be "
    "replayed, such as heterogeneous media sampled by tracking. It preserves suffixes when the prefix geometry "
    "changes, where full replay decorrelates, for example with moving geometry. And it lets volume vertices "
    "participate in the reconnection criteria, so mixed surface and volume paths can choose the anchor with the "
    "broadest lobe, which in our scenes is frequently the volume vertex.", body_ni))
S.append(Paragraph(
    "<b>Cost.</b> With the medium enabled, the full ReSTIR frame costs 59 ms on the exterior view against 29 ms "
    "with the medium disabled. The airlight family and the fog cluster spatial rounds account for most of the "
    "difference: airlight walks have no emitter terminals and survive to the path length cap. Replayable "
    "Russian roulette over the airlight walk is a direct optimization path. The medium itself requires no "
    "tracking and no density textures.", body_ni))
S.append(Paragraph(
    "<b>Scene matrix.</b> Fig. 8 compares the four per frame estimators across three scenes: the exterior view "
    "of Fig. 1, an interior view of the same box dominated by surfaces and the glass duck, and a lens scene in "
    "a denser, more forward scattering medium (σ<sub>s</sub> = 0.30, g = 0.5), where a small snooted emitter "
    "focuses through a biconvex glass lens into a converging volumetric caustic that crosses the floor at its "
    "focus. The ranking is identical in all three: our per frame estimate at frame 8 reaches mean FLIP "
    "0.052 / 0.050 / 0.113 against 0.087 / 0.072 / 0.203 for unidirectional ReSTIR PT, with single samples of "
    "path tracing and BDPT far behind. The gap is largest exactly where transport is hardest: on the lens beam "
    "crop our estimate reaches 0.165 where one sample of BDPT sits at 0.316 and path tracing at 0.402. The "
    "beam is nearly invisible to camera side sampling and is carried by light traced and connected paths, "
    "which only the bidirectional reservoirs can reuse across pixels and frames.", body))
S.append(KeepTogether([
    Spacer(1, 4),
    data_table(
        [["Configuration", "Ours", "Reference", "&#916;"],
         ["medium disabled (surface suite)", "0.116489", "0.116423 &#8211; 0.116535", "in band"],
         ["absorption only (σ<sub>s</sub> = 0)", "0.08616", "0.08604", "+0.14%"],
         ["full medium, no reuse", "0.09153", "0.09154", "&#8722;0.01%"],
         ["full medium, temporal reuse", "0.09153", "= no reuse", "0"],
         ["full medium, temporal + spatial", "0.09146", "0.09154", "&#8722;0.09%"],
         ["exterior camera (rays without geometry)", "0.06571", "0.06568", "+0.05%"]],
        [COL_W * 0.42, COL_W * 0.18, COL_W * 0.26, COL_W * 0.14]),
    Paragraph(
        "Table 2. Converged image means (luminance) against bidirectional references. Interior camera unless "
        "noted; fog σ<sub>s</sub> = 0.15, σ<sub>a</sub> = 0.02, g = 0.3.", caption),
]))

# ============================================================= limitations ==
S.append(Paragraph("9&#160;&#160;Limitations and future work", h1))
S.append(Paragraph(
    "The medium is global and homogeneous; media interfaces and heterogeneity are not supported. Heterogeneous "
    "free flight sampling breaks distance replay, which makes the anchored mapping the natural foundation; open "
    "problems are a replayable tracking scheme for the prefix and transmittance estimation along reconnection "
    "edges. Pixels without geometry reuse spatially only into the shading estimate, not into the reservoir "
    "chain, and never pair with surface pixels across silhouettes; both extensions require a supply aware "
    "confidence accounting. The reservoir of a pixel holds "
    "one scattering depth of a line integral; dedicated reservoirs over the primary segment could lower the "
    "airlight floor further. Our MIS ratios omit transmittance, and dense media may justify a bounded "
    "approximation of it.", body_ni))
S.append(Paragraph(
    "The evaluation itself has boundaries. The convergence study of Figs. 5&#8211;7 uses one scene and one "
    "medium parameter set (with the absorption only and g = 0.7 variants of Sec. 8), one resolution, and one "
    "GPU; the matrix of Fig. 8 adds an interior view and a denser, more anisotropic lens scene, but a "
    "systematic sweep over optical depth and anisotropy, and a comparison against froxel based volumetric "
    "ReSTIR [Lin et al. 2021], would locate the regime boundaries of the approach. The variance analysis of Sec. 5 is validated through the complete "
    "system rather than in isolation: an ablation that grants light tracing exclusive airlight responsibility "
    "and measures the near camera variance directly would exhibit the failure mode the truncated family "
    "removes, and classification rejection rates of replay near surfaces are likewise not reported. The "
    "temporal machinery of Sec. 6 is measured under a static camera only; a moving camera study with "
    "disocclusion is the outstanding validation.", body))

# ============================================================== conclusion ==
S.append(Paragraph("10&#160;&#160;Conclusion", h1))
S.append(Paragraph(
    "We presented the first bidirectional ReSTIR whose path reuse extends into an unbounded participating "
    "medium. Interior volume "
    "vertices act as reconnection anchors with a Jacobian that is a pure ratio of squared distances, replay "
    "reproduces free flight sampling under scatter classification masks, and scattered light along camera "
    "segments is sampled by a truncated camera technique whose multiple importance sampling weight removes an "
    "otherwise unbounded variance in the light traced estimator. The system is validated against bidirectional "
    "references and covers rays that hit no geometry. On a view dominated by the medium, its converged per "
    "frame estimate reaches a perceptual error that accumulated path tracing needs 320 frames to match, and its "
    "luminance variance floor lies 10.8 times below unidirectional ReSTIR path tracing.", body_ni))

S.append(Paragraph("References", h1))
for r in [
    "Benedikt Bitterli, Chris Wyman, Matt Pharr, Peter Shirley, Aaron Lefohn, and Wojciech Jarosz. 2020. "
    "Spatiotemporal reservoir resampling for real-time ray tracing with dynamic direct lighting. <i>ACM Trans. "
    "Graph.</i> 39, 4.",
    "Theodoros Galazios and Konstantinos Moustakas. 2026. Towards optimal shift mapping heuristics for "
    "ReSTIR-SSS. In <i>Extended Reality (XR Salento 2025), LNCS</i> 15737. Springer, 298&#8211;308.",
    "Iliyan Georgiev, Jaroslav K&#345;iv&#225;nek, Tom&#225;&#353; Davidovi&#269;, and Philipp Slusallek. 2012. "
    "Light transport simulation with vertex connection and merging. <i>ACM Trans. Graph.</i> 31, 6.",
    "Iliyan Georgiev, Jaroslav K&#345;iv&#225;nek, Toshiya Hachisuka, Derek Nowrouzezahrai, and Wojciech Jarosz. "
    "2013. Joint importance sampling of low-order volumetric scattering. <i>ACM Trans. Graph.</i> 32, 6.",
    "Adrien Gruson, Binh-Son Hua, Nicolas Vibert, Derek Nowrouzezahrai, and Toshiya Hachisuka. 2018. "
    "Gradient-domain volumetric photon density estimation. <i>ACM Trans. Graph.</i> 37, 4.",
    "Trevor Hedstrom, Markus Kettunen, Daqi Lin, Chris Wyman, and Tzu-Mao Li. 2025. ReSTIR BDPT: Bidirectional "
    "ReSTIR path tracing with caustics. <i>ACM Trans. Graph.</i> 44, 5.",
    "Markus Kettunen, Marco Manzi, Miika Aittala, Jaakko Lehtinen, Fr&#233;do Durand, and Matthias Zwicker. 2015. "
    "Gradient-domain path tracing. <i>ACM Trans. Graph.</i> 34, 4.",
    "Jaroslav K&#345;iv&#225;nek, Iliyan Georgiev, Toshiya Hachisuka, Petr V&#233;voda, Martin &#352;ik, Derek "
    "Nowrouzezahrai, and Wojciech Jarosz. 2014. Unifying points, beams and paths in volumetric light transport "
    "simulation. <i>ACM Trans. Graph.</i> 33, 4.",
    "Daqi Lin, Chris Wyman, and Cem Yuksel. 2021. Fast volume rendering with spatiotemporal reservoir resampling. "
    "<i>ACM Trans. Graph.</i> 40, 6.",
    "Daqi Lin, Markus Kettunen, Benedikt Bitterli, Jacopo Pantaleoni, Cem Yuksel, and Chris Wyman. 2022. "
    "Generalized resampled importance sampling: foundations of ReSTIR. <i>ACM Trans. Graph.</i> 41, 4.",
    "Daqi Lin, Markus Kettunen, and Chris Wyman. 2026. ReSTIR PT Enhanced: Algorithmic advances for faster and "
    "more robust ReSTIR path tracing. <i>Proc. ACM Comput. Graph. Interact. Tech.</i> 9, 1.",
    "Jan Nov&#225;k, Iliyan Georgiev, Johannes Hanika, and Wojciech Jarosz. 2018. Monte Carlo methods for "
    "volumetric light transport simulation. <i>Computer Graphics Forum</i> 37, 2.",
    "Yaobin Ouyang, Shiqiu Liu, Markus Kettunen, Matt Pharr, and Jacopo Pantaleoni. 2021. ReSTIR GI: Path "
    "resampling for real-time path tracing. <i>Computer Graphics Forum</i> 40.",
    "Matt Pharr, Wenzel Jakob, and Greg Humphreys. 2023. <i>Physically Based Rendering: From Theory to "
    "Implementation</i> (4th ed.). MIT Press.",
    "Corentin Sala&#252;n, Martin Balint, Laurent Belcour, Eric Heitz, Gurprit Singh, and Karol Myszkowski. 2025. "
    "Histogram stratification for spatio-temporal reservoir sampling. In <i>SIGGRAPH Conference Papers</i>.",
    "Eric Veach. 1997. <i>Robust Monte Carlo Methods for Light Transport Simulation.</i> Ph.D. thesis, Stanford "
    "University.",
    "Mirco Werner, Vincent Sch&#252;&#223;ler, and Carsten Dachsbacher. 2024. ReSTIR subsurface scattering for "
    "real-time path tracing. <i>Proc. ACM Comput. Graph. Interact. Tech. (HPG)</i> 7, 3.",
    "Chris Wyman, Markus Kettunen, Daqi Lin, Benedikt Bitterli, Cem Yuksel, Wojciech Jarosz, and Pawel Kozlowski. "
    "2023. A gentle introduction to ReSTIR path reuse in real time. In <i>SIGGRAPH Courses</i>.",
]:
    S.append(Paragraph(r, ref_st))


def on_page(canv, doc):
    canv.saveState()
    canv.setFont("TNR", 8.5)
    canv.drawCentredString(PAGE_W / 2.0, 0.45 * inch, str(doc.page))
    canv.restoreState()


doc = BaseDocTemplate(OUT, pagesize=letter, leftMargin=MARGIN, rightMargin=MARGIN,
                      topMargin=MARGIN, bottomMargin=MARGIN,
                      title="Volume Anchored Shift Mappings for Bidirectional ReSTIR in Participating Media",
                      author="Claude et al.")

TOP_H = 3.05 * inch
first_frames = [
    Frame(MARGIN, PAGE_H - MARGIN - TOP_H, PAGE_W - 2 * MARGIN, TOP_H, id="top",
          leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
    Frame(MARGIN, MARGIN, COL_W, PAGE_H - 2 * MARGIN - TOP_H - 8, id="c1",
          leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
    Frame(MARGIN + COL_W + COL_GAP, MARGIN, COL_W, PAGE_H - 2 * MARGIN - TOP_H - 8, id="c2",
          leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
]
later_frames = [
    Frame(MARGIN, MARGIN, COL_W, PAGE_H - 2 * MARGIN, id="l1",
          leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
    Frame(MARGIN + COL_W + COL_GAP, MARGIN, COL_W, PAGE_H - 2 * MARGIN, id="l2",
          leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
]
# Matrix page: full-width figure frame on top, two text columns beneath
# (same construction as the title page).
MATRIX_TOP_H = 7.5 * inch
matrix_frames = [
    Frame(MARGIN, PAGE_H - MARGIN - MATRIX_TOP_H, PAGE_W - 2 * MARGIN, MATRIX_TOP_H, id="mtop",
          leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
    Frame(MARGIN, MARGIN, COL_W, PAGE_H - 2 * MARGIN - MATRIX_TOP_H - 8, id="mc1",
          leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
    Frame(MARGIN + COL_W + COL_GAP, MARGIN, COL_W, PAGE_H - 2 * MARGIN - MATRIX_TOP_H - 8, id="mc2",
          leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0),
]
doc.addPageTemplates([PageTemplate(id="First", frames=first_frames, onPage=on_page),
                      PageTemplate(id="Later", frames=later_frames, onPage=on_page),
                      PageTemplate(id="Matrix", frames=matrix_frames, onPage=on_page)])
S.insert(0, NextPageTemplate("Later"))
doc.build(S)
print("wrote", OUT)
