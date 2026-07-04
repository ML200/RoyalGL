# -*- coding: utf-8 -*-
# Builds docs/shift_aware_spmis_draft.pdf - the DRAFT of the shift-aware
# neighbor selection paper (stochastic pairwise MIS with learned selection
# weights). Text-only for now; figure slots are marked in captions and get
# filled once the final experiment protocol runs on a Release build.
import os

from reportlab.lib.pagesizes import letter
from reportlab.lib.units import inch
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_JUSTIFY, TA_CENTER, TA_LEFT
from reportlab.platypus import (BaseDocTemplate, PageTemplate, Frame, Paragraph,
                                Spacer, Table, TableStyle, FrameBreak,
                                NextPageTemplate, KeepTogether)
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.fonts import addMapping

ROOT = r"C:\Users\Malte\Documents\GitHub\RoyalGL"
OUT = os.path.join(ROOT, "docs", "shift_aware_spmis_draft.pdf")

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
note_st = ParagraphStyle("note", parent=body_ni, fontName="TNR-I",
                         textColor=colors.Color(0.45, 0.1, 0.1))


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


S = []

# ================================================================= title ====
S.append(Paragraph("Shift-Aware Candidate Selection for Stochastic Pairwise MIS<br/>in Real-Time ReSTIR: "
                   "Realization, Limits, and Diagnostics", title_st))
S.append(Paragraph("DRAFT - RoyalGL research note, July 2026", author_st))
S.append(Paragraph(
    "<b>[Teaser figure slot]</b> Planned: equal-time comparison on a cluttered scene under camera motion - "
    "pair-mixture reuse / SPMIS with unconditional search / SPMIS with the tiny-run gate - full frames plus "
    "disocclusion and glossy crops, with per-crop MAPE. To be rendered by the final experiment protocol "
    "(Release build).",
    caption))
S.append(Spacer(1, 4))
S.append(NextPageTemplate("twocol"))
S.append(FrameBreak())

# ============================================================== abstract ====
S.append(Paragraph("<b>Abstract</b>", h1))
for p in [
    "Spatial reuse in ReSTIR draws a handful of neighbor pixels and merges their reservoirs with resampling "
    "MIS weights. Hedstrom et al. [2026] showed that the neighbors themselves may be chosen <i>based on their "
    "samples</i> without introducing bias, provided the selection probabilities P(i) are compensated inside "
    "stochastic resampling MIS weights; their SPMIS estimator selects proportionally to the <i>unshifted</i> "
    "source-pixel contribution c<sub>i</sub>&middot;p&#770;(X<sub>i</sub>)&middot;W<sub>i</sub>, and names selection "
    "proportional to the true post-shift resampling weight m<sub>i</sub>(Y<sub>i</sub>)&middot;p&#770;(Y<sub>i</sub>)"
    "&middot;W<sub>i</sub>&middot;|&part;T<sub>i</sub>/&part;X<sub>i</sub>| as an open problem: evaluating it "
    "needs the very shift mappings the method tries to avoid.",

    "We realize that selection density and evaluate it honestly. The missing factor - the shift survival "
    "ratio p&#770;(T(X))&middot;|&part;T/&part;X| / p&#770;(X) - is observed for free for every shift a ReSTIR "
    "renderer already evaluates; we learn it online as a per-pixel exponential moving average that reprojects "
    "with the temporal history and resets on disocclusion. Because the stochastic MIS weights compensate ANY "
    "positive selection distribution, a mispredicted score can only cost variance, never correctness. We "
    "further prove a first-moment lemma - the stochastic-MIS estimator remains unbiased under any "
    "candidate-drawing scheme whose per-draw marginals match P(i), not just i.i.d. multinomial sampling - "
    "which licenses antithetic stratified draws on the selection CDF and history-dependent selection "
    "policies.",

    "The empirical verdict is a finding in itself and splits by REGIME. On contribution-DENSE pools "
    "(Cornell-class scenes, where ~99.7% of a block run's entries carry nonzero selection mass), selection is "
    "SATURATED. The sound (bounded-ratio) form of survival-guided selection changes noise by "
    "at most &plusmn;2% at N&#771; &isin; {3, 8} on clutter, glossy, and disocclusion stress tests, and the "
    "naive form - selecting by the realized post-shift CONTRIBUTION - is actively harmful: its heavy-tailed "
    "scores concentrate P until the 1/(N&#771;P) compensation sprays fireflies (a failure mode we attribute "
    "with a defensive-mixture dose-response). Two byproducts outweigh the intended contribution. First, "
    "SPMIS's unconditional neighborhood search is a measured net harm on this scene class - probing replaces "
    "close partners with distant ones whose shifts fail more, monotonically in probe count and radius, and "
    "even wide fresh disocclusion strips reuse better from their own same-frame runs; gating the search to "
    "near-empty runs keeps its rescue role at zero steady-state tax. Second, a dual-direction pair diagnostic "
    "that runs BOTH shift directions of the same (canonical, partner) pair bounds the shift machinery's "
    "forward/backward asymmetry at 1e-7 locally - refuting asymmetry as the cause of the chain's small "
    "positive soak residual - and identifies the true mechanism: direction-symmetric FALSE-NEGATIVE shift "
    "failures feeding the stochastic weight functional, whose magnitude tracks the measured failure rate.",

    "On contribution-SPARSE pools the picture inverts. In a purpose-built scene whose emitter sits behind a "
    "two-bend chicane - every visible surface at least two bounces from the light, next-event estimation "
    "fully occluded, only ~1% of run entries carrying selection mass and ~44% of pixels sitting in runs with "
    "none at all - contribution-proportional selection is the difference between unusable and usable: the "
    "stratified pair-mixture baseline is WORSE than no spatial reuse at all (its rank picks feed dead "
    "neighbors into the mixture), while SPMIS selection cuts noise 49-67%. The same scene yields a second "
    "impossibility-flavored result: making the neighborhood SEARCH contribution-aware (trigger and probe "
    "weights on run selection mass) is irreparably biased (+16% - the estimator's accounting requires "
    "dead-run realizations to contribute exactly zero, and a value-dependent pool swap fires precisely "
    "there), which reframes SPMIS's value-blind confidence weighting as a correctness constraint and points "
    "to always-on union pools with exact per-draw compensation as the only unbiased rescue.",

    "Building on the cheap large-kernel machinery, we further demonstrate TEMPORAL-FREE ReSTIR: batching "
    "several stochastic candidates through one shift-pipeline sweep makes N&#771; = 16-24 candidates "
    "affordable, at which point spatial-only reuse MATCHES the per-frame quality of full "
    "temporal-plus-spatial ReSTIR - with uniform noise instead of a 5x-noisier disocclusion band, correct "
    "energy under motion instead of transient history-invalidation darkening, and no temporal lag or "
    "correlation trails, at roughly twice the (unoptimized) reuse cost.",
]:
    S.append(Paragraph(p, abstract))

# ========================================================== introduction ====
S.append(Paragraph("1&nbsp;&nbsp;Introduction", h1))
for p in [
    "ReSTIR amortizes path sampling across pixels and frames by resampling: each pixel keeps one reservoir "
    "sample and improves it by merging candidates borrowed from spatiotemporal neighbors under generalized "
    "resampled importance sampling (GRIS). WHICH neighbors to borrow from has remained surprisingly primitive: "
    "production implementations still draw a few pixels uniformly from a disk and reject on G-buffer "
    "differences, essentially unchanged since Bitterli et al. [2020]. Uniform selection wastes shifts in "
    "exactly the situations where reuse matters most - when few nearby reservoirs hold contributing samples "
    "(disocclusions, sparse initial sampling), or when many hold bright samples whose shift into the receiver "
    "dies (shadow edges, glossy transport, geometric discontinuities).",

    "Stochastic pairwise MIS (SPMIS) [Hedstrom et al. 2026] is the enabling theory for doing better: reuse "
    "mathematically from a large pool of M inputs, evaluate only N&#771; of them drawn with probabilities P(i) "
    "that MAY depend on the samples, and multiply each drawn candidate's MIS weight by K(i)/(N&#771;P(i)). "
    "Any positive P yields an unbiased estimator; P only steers variance. SPMIS instantiates P with the "
    "source-pixel contribution - the sample's brightness where it ALREADY is - which ignores everything about "
    "the shift into the receiver. The authors state the ideal selection density (the post-shift resampling "
    "weight) and leave its approximation open. Concurrently, Junkins et al. [2026] select neighbors by an "
    "analytic G-buffer compatibility score - a shift-quality prior that ignores the samples - and name the "
    "combination with SPMIS as future work. Neither line uses the one signal a ReSTIR renderer produces in "
    "abundance every frame: the realized outcomes of the shifts it evaluates.",

    "We treat the ideal selection weight as a product of what SPMIS already uses (source contribution) and a "
    "shift-survival factor, estimate the latter by online learning from the renderer's own shift outcomes, "
    "and report what a careful evaluation actually shows. Our contributions:",
]:
    S.append(Paragraph(p, body if p is not p else body))
for b in [
    "&bull; The first realization of <b>survival-guided selection</b>: a per-pixel score learned online from "
    "realized shift outcomes, reprojected along temporal history and reset at disocclusions, folded into "
    "SPMIS's selection weights without extra rays and without bias for any predictor quality - together with "
    "the honest empirical verdict that block-local selection is SATURATED (the bounded-ratio form is a wash; "
    "the absolute-contribution form fireflies via 1/P inflation, with the failure mechanism attributed).",
    "&bull; A <b>first-moment lemma</b>: the stochastic resampling MIS weight remains unbiased under any "
    "drawing scheme with E[K(i)] = N&#771;P(i), covering stratified, antithetic, and history-dependent "
    "selection. This licenses <b>antithetic CDF draws</b> - stratification and importance selection compose.",
    "&bull; A data-driven <b>starvation gate</b> for SPMIS's neighborhood search: unconditional probing is a "
    "measured 23-45% steady-state noise tax on block-run pools (the own run survives the probe WRS with "
    "probability ~1/(1+probes)), and even wide fresh disocclusion strips prefer their own same-frame runs; "
    "restricting the search to near-empty runs (count &le; 2) keeps its rescue role at zero measured cost.",
    "&bull; A <b>dual-direction pair diagnostic</b> that evaluates both shift directions of the same "
    "(canonical, partner) pair each frame, plus an involutive fixed-stride pairing that keeps distant-pair "
    "ensembles exchangeable. It bounds the shift machinery's directional asymmetry at 1e-7 locally and "
    "identifies the chain's small positive residual as direction-symmetric false-negative shift failures "
    "amplified by the stochastic weight functional - with the failure-rate dose-response to match.",
    "&bull; An implementation on block-local sorted cluster runs with exact O(log M) CDF inversion "
    "(segmented scans in the existing histogram-stratification sort), integrated in a wavefront ReSTIR BDPT "
    "renderer; the stochastic chain is CHEAPER than the pair-mixture baseline it replaces, and batching "
    "candidates through one shift-pipeline sweep makes N&#771; = 16-24 affordable (the temporal-free regime).",
]:
    S.append(Paragraph(b, bullet))

# ======================================================== related work ======
S.append(Paragraph("2&nbsp;&nbsp;Related Work", h1))
for p in [
    "<b>ReSTIR and GRIS.</b> Reservoir resampling for direct lighting [Bitterli et al. 2020] generalizes to "
    "path tracing through GRIS [Lin et al. 2022], which introduces unbiased contribution weights, shift "
    "mappings with Jacobians, confidence weights, and the generalized balance and pairwise MIS heuristics we "
    "build on; see the course of Wyman et al. [2023]. Extensions cover indirect gathering, area sampling, "
    "subsurface scattering, bidirectional techniques, and volumes; all inherit Bitterli-style uniform neighbor "
    "draws with G-buffer rejection.",

    "<b>Neighbor selection.</b> Tokuyoshi [2023] gates reuse by fitted vMF similarity (biased, DI only). "
    "Salaun et al. [2025] keep the uniform marginal but draw neighbors antithetically from block-local "
    "contribution histograms; our renderer's prior spatial reuse implements this and serves as the pair-"
    "mixture baseline. Lin et al. [2026] pair pixels reciprocally to halve shift cost but keep random "
    "selection. Junkins et al. [2026] importance-sample neighbors by an analytic compatibility score built "
    "from G-buffer positions and normals - sample-independent by design so that standard MIS stays valid. "
    "SPMIS [Hedstrom et al. 2026] is the closest work and our baseline; we differ in the selection density "
    "(learned, shift-aware, target-sensitive at the cell level vs. unshifted source contribution), in the "
    "drawing scheme (antithetic stratified vs. i.i.d.), and in gating the neighborhood search.",

    "<b>Learned sampling decisions.</b> Offline, neural nets have selected gradient-domain shift neighbors "
    "[Josse et al. 2025; Manzi et al. 2014]. Online-trained tiny networks make per-point sampling decisions "
    "in real time for radiance caching [Muller et al. 2021], appearance models [Zeltner et al. 2024], "
    "many-light PMFs [Figueiredo et al. 2025], and visibility [Boksansky and Meister 2025]. Our score is "
    "deliberately simpler - a reprojected EMA - because the stochastic-MIS compensation removes the usual "
    "correctness pressure on the predictor; a fancier model only has to beat the EMA on variance per cost. "
    "Sample-dependent decisions that WOULD bias a plain estimator appearing safely inside a compensated one "
    "echoes weighted reservoir resampling theory [Chao 1982; Talbot et al. 2005].",
]:
    S.append(Paragraph(p, body))

# ============================================================ background ====
S.append(Paragraph("3&nbsp;&nbsp;Background: GRIS and Stochastic Pairwise MIS", h1))
for p in [
    "GRIS merges M inputs (X<sub>i</sub>, W<sub>i</sub>, c<sub>i</sub>) into a target pixel by drawing index z "
    "proportionally to w<sub>i</sub> = m<sub>i</sub>(Y<sub>i</sub>)&middot;p&#770;(Y<sub>i</sub>)&middot;"
    "W<sub>i</sub>&middot;|&part;T<sub>i</sub>/&part;X<sub>i</sub>|, where Y<sub>i</sub> = T<sub>i</sub>"
    "(X<sub>i</sub>) is the shifted sample and m<sub>i</sub> a resampling MIS weight; the winner's unbiased "
    "contribution weight is W<sub>Y</sub> = &Sigma;w<sub>j</sub> / p&#770;(Y). Defensive pairwise MIS pairs "
    "each non-canonical input with the canonical pixel, at O(M) shifts.",

    "SPMIS replaces m<sub>i</sub> by the stochastic m&#771;<sub>i</sub> = K(i)/(N&#771;P(i))&middot;"
    "m<sub>i</sub>, where a multiset of N&#771; indices is drawn with replacement from P, and K(i) is i's "
    "multiplicity. E[m&#771;<sub>i</sub> | inputs] = m<sub>i</sub> makes the whole GRIS estimate unbiased for "
    "any P positive on the support of the contribution; P may depend on the inputs because the expectation is "
    "conditioned on them. The canonical weight is estimated separately with N&#771;<sub>c</sub> uniform "
    "draws (N&#771;<sub>c</sub> = 1 suffices), and all non-canonical confidences are scaled by N&#771;/M to "
    "keep the canonical sample from being drowned by stochastic-MIS variance. SPMIS instantiates "
    "P(i) &prop; c<sub>i</sub>&middot;p&#770;(X<sub>i</sub>)&middot;W<sub>i</sub> - the source-pixel "
    "contribution - and selects the reuse pool (a screen-space cell of M &le; 64 pixels hashed by tile, "
    "object and normal) by confidence-weighted WRS with growing search radius.",
]:
    S.append(Paragraph(p, body))

# ================================================================ method ====
S.append(Paragraph("4&nbsp;&nbsp;Method", h1))

S.append(Paragraph("4.1&nbsp;&nbsp;Factoring the ideal selection weight", h2))
for p in [
    "The ideal P(i) &prop; m<sub>i</sub>(Y<sub>i</sub>)&middot;p&#770;(Y<sub>i</sub>)&middot;W<sub>i</sub>"
    "&middot;|&part;T<sub>i</sub>/&part;X<sub>i</sub>| factors as",
]:
    S.append(Paragraph(p, body))
S.append(Paragraph(
    "P(i) &prop; [c<sub>i</sub>&middot;p&#770;(X<sub>i</sub>)&middot;W<sub>i</sub>] &middot; "
    "&rho;<sub>i</sub> &middot; &mu;<sub>i</sub>,&nbsp;&nbsp;&nbsp; "
    "&rho;<sub>i</sub> = p&#770;(T<sub>i</sub>(X<sub>i</sub>))&middot;|&part;T<sub>i</sub>/&part;X<sub>i</sub>| "
    "/ p&#770;(X<sub>i</sub>)", eq))
for p in [
    "where the bracket is SPMIS's source contribution (with c<sub>i</sub> in place of the constant), "
    "&rho;<sub>i</sub> is the shift survival ratio, and &mu;<sub>i</sub> collects the remaining MIS-weight "
    "variation, which is second order within a reuse pool of similar pixels (m<sub>i</sub> &asymp; "
    "c&Sigma;/(c&Sigma;+c<sub>c</sub>) whenever the shift roughly preserves the target). SPMIS sets "
    "&rho;<sub>i</sub>&mu;<sub>i</sub> = 1. Everything our method adds is an estimate of &rho;.",

    "&rho; is expensive to evaluate on demand - it IS the shift - but a wavefront ReSTIR renderer evaluates "
    "thousands of shifts per frame anyway, each yielding one realized &rho; sample for one (source, receiver) "
    "pair. The signal is already there; it only needs to be remembered in the right place.",
]:
    S.append(Paragraph(p, body))

S.append(Paragraph("4.2&nbsp;&nbsp;Online shift-survival scores", h2))
for p in [
    "Each pixel maintains a scalar score v, the exponential moving average (&alpha; = 1/8, tunable) of "
    "bounded survival ratios clamp(&rho;, 0.02, 1) observed on the backward canonical shift the SPMIS chain "
    "already runs every frame (the pixel's own sample leaving toward the uniformly drawn partner z'). The "
    "0.02 floor keeps a learned always-fails distinguishable from unlearned (0); writing only one's own "
    "score avoids atomics. The observation must be the survival RATIO, not the realized post-shift "
    "contribution: the SPMIS weight already carries source brightness, so an absolute observation "
    "double-counts it, and its heavy tail (it inherits the unbounded UCW) concentrates P so hard that the "
    "1/(N&#771;P) compensation sprays fireflies. We measured exactly that failure and its cure (Sec. 5).",

    "The score must track SURFACES, not pixels: under motion we copy it along the temporal reprojection "
    "pairing and reset it on disocclusion, exactly mirroring reservoir history. Without this, freshly "
    "revealed geometry inherits scores learned on the occluder, under-selects the only contributing "
    "sources, and the compensation converts the misprediction into variance spikes.",

    "Selection uses s<sub>i</sub> = min(c<sub>i</sub>, c<sub>cap</sub>)&middot;p&#770;(X<sub>i</sub>)&middot;"
    "W<sub>i</sub>&middot;(d + (1 - d)&middot;min(v<sub>i</sub>, 1)) with defensive mix d = 0.25 (tunable); "
    "unlearned pixels stay neutral. The mix bounds the worst-case weight inflation at 1/d and, together "
    "with s<sub>i</sub> = 0 &hArr; zero contribution, maintains the SPMIS support condition - so "
    "unbiasedness holds for ANY score values, learned, stale, or adversarial. Dependence of P on previous "
    "frames is covered by conditioning the first-moment argument on the history (Sec. 4.3).",
]:
    S.append(Paragraph(p, body))

S.append(Paragraph("4.3&nbsp;&nbsp;First-moment lemma and antithetic CDF draws", h2))
for p in [
    "SPMIS draws the N&#771; candidates i.i.d. from P. Inspecting the unbiasedness proof shows only "
    "LINEARITY is used: E[m&#771;<sub>i</sub>] = E[K(i)]/(N&#771;P(i))&middot;m<sub>i</sub>, so the estimator "
    "is unbiased under ANY drawing scheme with E[K(i)] = N&#771;P(i), conditioned on the inputs and any "
    "auxiliary state P depends on. Draws may be mutually dependent; per-draw marginals matching P suffice. "
    "(Lemma and short proof to be spelled out in the final version; it also covers P depending on previous "
    "frames' data, since the identity holds pointwise in the conditioning.)",

    "We exploit this twice. First, we draw the k-th of K rounds at the antithetic stratified position "
    "pos<sub>k</sub> = (k even ? k/K + u : (k+1)/K - u) on the selection CDF, with one Sobol-sequence u per "
    "pixel and frame - each pos<sub>k</sub> is marginally uniform, so marginals match P exactly while "
    "successive draws stratify the selection mass and pair antithetically, composing Salaun-style "
    "stratification WITH importance selection (their method is the special case of a uniform target). "
    "Second, the learned scores make P depend on all previous frames through the EMA; the conditioned lemma "
    "makes that rigorous.",
]:
    S.append(Paragraph(p, body))

S.append(Paragraph("4.4&nbsp;&nbsp;Starvation-gated cell search", h2))
for p in [
    "SPMIS always reselects the reuse pool by confidence-weighted WRS over probed cells with growing radius "
    "(12 probes from 30 px, &times;1.25). On block-run pools that is a structural tax: the own run survives "
    "the probe WRS with probability roughly own-mass over total probed mass - about 1/(1+probes) at "
    "steady state - so most pixels swap close partners for runs up to ~350 px away whose shifts fail more. "
    "We measure the tax as monotone in BOTH probe count and radius (Sec. 5), and, more surprisingly, that "
    "even 64 px/frame fresh disocclusion strips reuse better from their own 256-entry same-frame runs than "
    "from distant converged runs: a 16&times;16 block of fresh RIS candidates is already a viable pool.",

    "The data therefore supports exactly one trigger: a pixel whose run is near-EMPTY (count &le; 2) has "
    "nothing local to reuse and can only gain from foreign runs. This tiny-run gate measures identical to "
    "no-search everywhere else while keeping the rescue role; it matters more once cluster keys split by "
    "orientation (Sec. 4.5), which multiplies partnerless slivers. Two tempting alternatives fail: gating "
    "on mean run confidence breaks under duplication-driven confidence caps (correlated regions probe "
    "forever), and gating on the temporal disocclusion flag hurts the very band it targets. The gate is a "
    "sample-value-independent test, so pool choice remains input-set selection needing no MIS compensation; "
    "the faithful unconditional search remains available as the baseline configuration.",

    "A third alternative fails for a deeper reason, and the failure is a result. On sparse scenes the "
    "natural upgrade is CONTRIBUTION-aware search: trigger when the own run's selection mass S is zero and "
    "weight probed runs by THEIR selection mass (confidence is uniform there, so the paper's WRS hops to "
    "equally dead runs). Measured, this brightens the sparse scene's converged mean by +16%, concentrated "
    "exactly in the dark pixels. The accounting argument is general: the fixed-pool estimator satisfies "
    "E[est] = P(dead pool)&middot;0 + P(live pool)&middot;E[est | live] = I, so realizations with a dead "
    "pool MUST contribute zero; a value-dependent pool swap that fires exactly on the dead branch replaces "
    "that zero with gathered energy whose full weight the live branches already carry, and no branch-local "
    "compensation exists. This is the pool-level form of the value-correlated-selection trap, and it "
    "reframes SPMIS's confidence-only search weighting as a CORRECTNESS constraint rather than a missed "
    "optimization. The unbiased route is to make foreign runs part of the pool UNCONDITIONALLY - a union "
    "pool over value-independently probed runs, drawn through the union's exact selection CDF so the value "
    "dependence sits inside the per-draw compensation where the theory allows it, with a deterministic "
    "distance kernel keeping the dense-scene tax out. We leave its implementation to future work.",
]:
    S.append(Paragraph(p, body))

S.append(Paragraph("4.5&nbsp;&nbsp;Implementation", h2))
for p in [
    "Our renderer's spatial reuse already sorts each jittered 16&times;16 block's post-temporal reservoirs "
    "by (cluster key, target luminance) for histogram stratification; a cluster key is the primary hit's "
    "instance, material, and world-normal octant (3 sign bits - the counterpart of SPMIS's objectID + "
    "quantized-normal cell hash; adding the normal term measures as a small consistent win on curved "
    "geometry). We extend the sort's epilogue with a segmented Hillis-Steele scan producing, per "
    "cluster run, the inclusive CDF of the selection weights s<sub>i</sub> and the run confidence sum "
    "c&Sigma; - the contiguous-equal-keys property makes the naive same-key scan exact. Selection inverts "
    "the CDF by binary search over ranks, O(log M) fetches; the drawn candidate's exact probability is the "
    "CDF difference over the run mass, with strict comparison making zero-weight ranks unselectable. Per "
    "K-round wavefront pass we create one forward shift job (the drawn candidate) plus, in round 0 only, one "
    "backward job for the N&#771;<sub>c</sub> = 1 canonical estimate: K + 1 shifts per pixel versus the "
    "pair-mixture baseline's 2K - the stochastic chain is CHEAPER at equal K. The merge folds canonical and "
    "candidates into one reservoir chain with the scaled stochastic pairwise weights, maintaining the "
    "final-form invariant so temporal reuse and resolve are untouched. The score lives in one persistent "
    "vec4 per pixel alongside the disocclusion flag and the chosen pool.",
]:
    S.append(Paragraph(p, body))

# ========================================================== preliminary =====
S.append(Paragraph("5&nbsp;&nbsp;Preliminary Results", h1))
S.append(Paragraph(
    "All numbers below are from the development renderer (RoyalGL, GL compute wavefront ReSTIR BDPT) at "
    "1600&times;900 on an RTX GPU, Debug host build - RELATIVE comparisons are meaningful, absolute timings "
    "are not final. Scenes are Cornell-box variants with one glass duck (plus, where noted, 8 clutter ducks "
    "of varied materials); relNoise is the mean absolute 4-neighbor high-frequency residual over the image "
    "mean; \"masked\" restricts it to pixels whose temporal pass found no usable history. The SPARSE-POOL "
    "rows use LightMaze, a purpose-built scene (emitter alcove behind a two-bend chicane; a geometric "
    "invariant guarantees no straight emitter-to-door line, so NEE is fully occluded and all visible light "
    "is >= 2 bounces). Confidence cap 8; "
    "decorrelation off; normal-octant cluster keys and the tiny-run search gate are the defaults everywhere "
    "unless a row says otherwise. All motion tests are FRAME-DETERMINISTIC (fixed per-frame time step, "
    "scripted lateral dolly or rocking orbit; stats fire on the frame counter), so cross-configuration "
    "comparisons are exactly frame-paired - wall-clock pacing confounds motion A/Bs between configurations "
    "of different speed and object-animation tests inherit BLAS-rebuild timing nondeterminism.", note_st))

S.append(Spacer(1, 4))
S.append(KeepTogether([
    data_table([
        ["Temporal OFF, static, N&#771; = 3", "pair-mixture", "SPMIS [H26]", "+ score + lemma draws"],
        ["glass duck: relNoise", "0.192", "0.158 (-18%)", "0.158"],
        ["conductor duck: relNoise", "0.197", "0.162 (-18%)", "0.161"],
        ["rough glass duck: relNoise", "0.192", "0.158 (-18%)", "0.158"],
        ["spatial pass GPU time", "9.1-9.7 ms", "7.3-7.6 ms", "7.3-7.4 ms"],
    ], [COL_W * 0.38, COL_W * 0.20, COL_W * 0.21, COL_W * 0.21]),
    Paragraph("Table 1. Spatial-reuse-only quality (per-frame relNoise, one duck, locked camera). "
              "Contribution-proportional selection with stochastic pairwise MIS is the win that survives "
              "every test: -18% noise at -20% cost versus the histogram-stratified pair mixture, uniformly "
              "across materials. The full mode-2 column (bounded survival score + antithetic stratified "
              "CDF positions) matches SPMIS to the third digit: local sorted runs are already ranked well "
              "enough by source contribution, and on Cornell-class blocks the selection mass is locally "
              "uniform enough that stratifying the CDF draws adds nothing measurable either - the lemma's "
              "value here is the license, exercised at zero cost, not a variance win.", caption),
]))

S.append(KeepTogether([
    data_table([
        ["Clutter + conductor, temporal ON", "SPMIS [H26]", "+ survival score", "masked (SPMIS / score)"],
        ["dolly, N&#771; = 3", "0.0984", "0.0987", "0.458 / 0.461"],
        ["dolly, N&#771; = 8", "0.0829", "0.0831", "0.466 / 0.464"],
        ["orbit, N&#771; = 3", "0.0973", "0.0986", "0.299 / 0.300"],
        ["orbit, N&#771; = 8", "0.0762", "0.0777", "0.274 / 0.276"],
        ["orbit N&#771;=8, ABSOLUTE score", "-", "0.0889, hot 319", "0.276"],
        ["... defensive mix 0.5 / 0.75", "-", "0.0830 / 0.0791, hot 51 / 17", "-"],
        ["... score factor off", "= SPMIS exactly", "0.0762, hot 18", "0.274"],
    ], [COL_W * 0.34, COL_W * 0.20, COL_W * 0.26, COL_W * 0.20]),
    Paragraph("Table 2. Prove-or-kill on the score's home turf: 8 clutter ducks + conductor main duck, "
              "frame-deterministic dolly (parallax silhouettes) and 30&deg;/s rocking orbit (64 px/frame "
              "fresh edge strips, ~2% of pixels), whole-image / masked per-frame relNoise. The bounded-"
              "ratio score is a WASH (&plusmn;2%, masked included): local runs are already ranked well "
              "enough by c&middot;p&#770;&middot;W, and N&#771; &isin; {3,8} draws cover the contributing "
              "set - per-pixel shift survival carries no exploitable extra structure here at any EMA rate "
              "we swept. The ABSOLUTE realized-contribution observation (the naive reading of the ideal "
              "density) is actively harmful: heavy-tailed scores concentrate P and the 1/(N&#771;P) "
              "compensation inflates reservoir weights into fireflies (hot = pixels brighter than any "
              "emitter). The defensive-mixture dose-response (mix 0.25 &rarr; 0.5 &rarr; 0.75 gives hot "
              "319 &rarr; 51 &rarr; 17 and noise 0.0889 &rarr; 0.0830 &rarr; 0.0791 &rarr; 0.0762 at "
              "mix 1) attributes the loss entirely to the score signal; the antithetic draws are free "
              "(score-off equals SPMIS to the fourth digit).", caption),
]))

S.append(KeepTogether([
    data_table([
        ["Cell search (SPMIS, N&#771;=3)", "dolly whole", "orbit whole", "orbit masked"],
        ["no search", "0.0894", "0.0884", "0.298"],
        ["unconditional, 8 probes / 24 px", "0.1098", "-", "-"],
        ["unconditional, 8 / 30 px", "0.1152", "-", "-"],
        ["unconditional, 12 / 24 px", "0.1232", "-", "-"],
        ["unconditional, 12 / 30 px (paper)", "0.1289", "0.1289", "0.329"],
        ["tiny-run gate (count &le; 2)", "0.0893", "0.0884", "0.299"],
        ["disocclusion-flag gate", "0.0894", "0.0922", "0.328"],
    ], [COL_W * 0.40, COL_W * 0.19, COL_W * 0.19, COL_W * 0.22]),
    Paragraph("Table 3. The neighborhood search is a measured steady-state tax on block-run pools: "
              "whole-image noise rises monotonically in BOTH probe count and initial radius (frame-paired "
              "deterministic dolly), the own run surviving the probe WRS with probability ~1/(1+probes). "
              "The masked column is the wide fresh strip of a 30&deg;/s orbit - the search's intended "
              "beneficiary - where distant converged runs LOSE to the strip's own 256-entry same-frame "
              "run, and even a gate that fires exactly there (disocclusion flag) hurts the band it "
              "targets. Only near-empty runs benefit from foreign pools: the tiny-run gate equals "
              "no-search everywhere measured while keeping that rescue.", caption),
]))

S.append(KeepTogether([
    data_table([
        ["LightMaze (sparse pools, temporal OFF)", "relNoise K=3", "relNoise K=8"],
        ["no spatial reuse (RIS only)", "1.48", "1.48"],
        ["pair-mixture baseline", "1.69", "1.58"],
        ["SPMIS selection", "0.865 (-49%)", "0.519 (-67%)"],
        ["+ survival score", "0.865", "0.513"],
        ["SPMIS + unconditional search", "-", "1.18"],
        ["SPMIS + contribution-aware search", "-", "0.546, mean +16% (BIASED)"],
    ], [COL_W * 0.42, COL_W * 0.26, COL_W * 0.32]),
    Paragraph("Table 4. The sparse-pool regime (LightMaze: emitter behind a two-bend chicane, all visible "
              "surfaces &ge; 2 bounces from the light, NEE fully occluded; contributing-entry fraction of "
              "reuse runs 1.1% vs 99.7% on Cornell, ~44% of pixels in zero-mass runs; per-frame relNoise, "
              "locked camera). Selection inverts from irrelevant to decisive: the pair mixture is WORSE "
              "than no reuse (rank-stratified picks feed dead neighbors into the 1/K mixture), SPMIS "
              "selection more than halves noise, and in accumulation the SPMIS chain reaches 2.4x lower "
              "error at half the samples. The survival score remains a wash even here - finding carriers "
              "is the bottleneck, and c&middot;p&#770;&middot;W already finds them. The unconditional "
              "search stays counterproductive (contribution-blind probes hop to equally dead runs at "
              "longer shift distance), and the contribution-aware variant is BIASED per the Sec. 4.4 "
              "accounting argument - its +16% lands exactly in the dark pixels. Unbiasedness soaks of the "
              "shipped chains hold on this scene (SPMIS spatial-only and temporal+spatial both match the "
              "RIS-only mean to 2e-5; dead runs generate no false-negative events).", caption),
]))

S.append(KeepTogether([
    data_table([
        ["Long-average soak (static)", "mean luminance"],
        ["RIS-only / temporal-only / pair-mixture", "0.116474 (exact, repeatable 1e-6)"],
        ["SPMIS chain, N&#771; = 1", "0.116474 (exact)"],
        ["SPMIS spatial-only, N&#771; = 8, local runs", "0.116509 (+0.030%)"],
        ["... involutive 48 px partner runs", "0.116611 (+0.118%)"],
        ["... involutive 96 px partner runs", "0.116622 (+0.127%)"],
        ["SPMIS T+S, gated search (default)", "0.116550 (+0.065%)"],
        ["SPMIS T+S, unconditional search", "0.116379 (-0.082%)"],
        ["survival score (mode 2)", "= SPMIS band (policy moves no energy)"],
    ], [COL_W * 0.52, COL_W * 0.48]),
    Paragraph("Table 5. Unbiasedness soaks (locked camera) and the residual's attribution. The stochastic "
              "chain carries a small residual whose SIGN follows the reuse regime. Prior bisection rules "
              "out selection probabilities, CDF construction, draw accounting, and the estimator structure "
              "(a CPU Monte Carlo of the exact chain is unbiased). A dual-direction diagnostic - both "
              "shift directions of the SAME (canonical, z') pair each frame, with involutive fixed-stride "
              "pairing to keep distant ensembles exchangeable - REFUTES the machinery-asymmetry hypothesis: "
              "support-failure classes balance to 1.4e-4, value ratios to 1e-5, and the canonical-weight "
              "beta channel evaluated through both directions matches to 1e-7 at local pairs, where the "
              "chain is nonetheless +3.0e-4 elevated. Instead the residual tracks the direction-SYMMETRIC "
              "support-failure rate (1.34%/class local &rarr; 3.2% at 48 px &rarr; 3.8% at 96 px, against "
              "+3.0e-4 &rarr; +11.8e-4 &rarr; +12.7e-4): false-negative failures feed the stochastic weight "
              "functional asymmetrically in ENERGY - a failed backward shift awards the canonical the full "
              "prior share (forcing all backward shifts to fail elevates the mean by 72%, the channel's "
              "headroom), while a failed forward shift merely loses one candidate's contribution. Balanced "
              "counts, unbalanced magnitudes; the net is positive for local pools, and the unconditional "
              "search flips it negative because its role-asymmetric pool choice adds a +7.2% forward-"
              "failure excess. The scaling ((c&Sigma;-c<sub>c</sub>)/(c&Sigma;+c<sub>c</sub>))&sup2; "
              "explains N&#771; = 1 exactness. The fix is therefore not weight symmetrization but raising "
              "real shift success (fewer false negatives) - the same lever that dominates variance.", caption),
]))

S.append(KeepTogether([
    data_table([
        ["Temporal-free (spatial-only, batched)", "static", "moving scene", "disocc. band", "reuse cost"],
        ["temporal + spatial (K=3, reference)", "0.079", "0.080", "0.415", "16-19 ms"],
        ["spatial-only SPMIS chain, K = 16", "0.081", "0.079", "= whole image", "29-35 ms"],
        ["spatial-only SPMIS chain, K = 24", "0.073", "-", "= whole image", "50 ms"],
    ], [COL_W * 0.36, COL_W * 0.14, COL_W * 0.16, COL_W * 0.17, COL_W * 0.17]),
    Paragraph("Table 6a. Temporal-free ReSTIR via WIDE reuse (per-frame relNoise; batched candidates). "
              "Spatial-only reuse at N&#771; = 16 matches temporal+spatial everywhere while having NO "
              "disocclusion band at all (temporal+spatial's history-starved pixels, 1.6% of the frame "
              "under this motion, are 5x noisier than its average and it transiently loses ~0.9% energy "
              "to history invalidation; spatial-only keeps the reference mean). Iterated spatial passes "
              "were tried and REJECTED: re-mixing within-frame aggregates adds almost no sample diversity "
              "(J=2xK=3 is WORSE than one pass of K=6) - width plus batching is the right structure, and "
              "the missing factor-2 in cost is shift-kernel engineering (reconnection fast path, cheaper "
              "light-path replays, reciprocal sharing), not estimator design. Caveat: the stochastic-chain "
              "residual of Table 5 grows with N&#771; (~+0.07% at 16); its mechanism is now identified "
              "(false-negative shift failures), so the same shift-success work that closes the cost gap "
              "is expected to shrink it - it must land before this configuration is submission-credible.", caption),
]))

S.append(KeepTogether([
    data_table([
        ["Reservoir butterfly (spatial-only)", "static", "moving scene", "spatial cost", "soak mean"],
        ["temporal + spatial (K=3, reference)", "0.0794", "0.0804", "14.5 ms (reuse)", "exact"],
        ["butterfly J = 3 (3 shifts/pixel)", "0.0788", "0.0748", "9.6 ms", "0.116476 (exact)"],
        ["butterfly J = 5", "0.0780", "-", "14.4 ms", "0.116477 (exact)"],
        ["SPMIS chain K = 16 (for scale)", "0.0811", "0.0794", "29-35 ms", "+0.07% residual"],
    ], [COL_W * 0.34, COL_W * 0.13, COL_W * 0.16, COL_W * 0.19, COL_W * 0.18]),
    Paragraph("Table 6b. The RESERVOIR BUTTERFLY: J levels of reciprocal XOR-partner pair merges "
              "(partner distance doubling per level, alternating axes, per-frame-jittered grid). Each "
              "level costs ONE shift per pixel - the reverse shift IS the partner's forward shift, since "
              "XOR pairing is an involution and the cluster test is mutual - and aggregates hold up to "
              "2<super>J</super> disjoint initial samples: effective samples scale EXPONENTIALLY in shift "
              "count. Index-based pairing never inspects sample values (no compensation needed) and the "
              "two-way balance-heuristic merge is the symmetric functional: butterfly soaks sit exactly on "
              "the unbiased reference, with no stochastic-MIS residual. At three shifts per pixel the "
              "spatial-only butterfly matches temporal+spatial statically and clearly beats it under "
              "motion, at ~25-35% lower total frame cost, with uniform noise (no disocclusion band), "
              "correct energy under motion, and no temporal lag. Deeper butterflies plateau (long-distance "
              "shift failure and intra-group correlation, not the confidence cap); J = 3-5 is the sweet "
              "spot. Block-scale correlation needs an honest metric beyond high-frequency residuals - "
              "duplication maps and accumulated-convergence checks (butterfly J=5 accumulates FASTER than "
              "every other configuration) are planned for the final protocol. In our renderer the "
              "butterfly is currently shelved on exactly that ground: the 2<super>J</super>-pixel groups "
              "share aggregates, which reads as a resolution loss up close even where the residual metric "
              "improves - the numbers above are reproducible from the archived implementation.", caption),
]))

# ============================================================ discussion ====
S.append(Paragraph("6&nbsp;&nbsp;Discussion, Limitations, Plan", h1))
for p in [
    "<b>What is established.</b> The SPMIS reformulation of our spatial reuse is implemented, passes soaks, "
    "and is both faster and substantially lower-variance than the histogram-stratified pair-mixture baseline "
    "whenever spatial reuse carries real load (-18% noise at -20% cost, Table 1) - with batching, it also "
    "unlocks the temporal-free wide-reuse regime (Table 5a). On top of that baseline, however, SELECTION "
    "refinement is saturated on block-local sorted runs: the survival score in its sound bounded-ratio form "
    "is a wash at every dose, EMA rate, and scenario we measured (Table 2), stratified CDF draws add nothing "
    "on locally-uniform blocks (Table 1), and the naive absolute form of the ideal density is actively "
    "harmful through 1/(N&#771;P) weight inflation - a failure mode we attribute conclusively via its "
    "defensive-mixture dose-response. The unconditional neighborhood search is a measured net harm on this "
    "scene class at every shape (Table 3); the tiny-run gate keeps its rescue role for free. The chain's "
    "small soak residual is attributed by the dual-direction diagnostic: NOT directional machinery "
    "asymmetry (bounded at 1e-7 locally), but direction-symmetric false-negative shift failures whose "
    "energy imbalance the stochastic weight functional amplifies (Table 4).",

    "<b>What the findings mean.</b> The regime split is the paper's organizing fact. WHERE a pool holds "
    "carriers, variance is dominated by SHIFT OUTCOMES, not by which of 256 same-cluster candidates gets "
    "drawn - so survival prediction, stratified draws, and search guidance all wash out, and the right "
    "lever is fixing survival itself (deeper-reconnection fallbacks; the same false-negative failures are "
    "the identified source of the soak residual). WHERE pools are starved of carriers, plain "
    "contribution-proportional selection is decisive (Table 4) - no learned refinement needed, since "
    "c&middot;p&#770;&middot;W already separates the ~1% of carriers from the dead mass. What remains "
    "genuinely open between the regimes is RESCUING zero-mass pools, and the biased contribution-aware "
    "search shows this cannot be done by pool-choice heuristics: it requires the union-pool construction "
    "with exact per-draw compensation.",

    "<b>What is missing for the paper.</b> (1) The union-pool rescue (Sec. 4.4) - the only identified "
    "unbiased mechanism addressing the ~44% zero-mass pixels of the sparse regime. (2) The "
    "deeper-reconnection fallback, which serves double duty (variance and residual). (3) The "
    "frame-deterministic equal-time protocol on a Release build with MAPE against long references, plus "
    "production-scale scenes between the two regime poles (Bistro-class geometry, foliage). (4) A "
    "comparison against Junkins-style analytic compatibility as a selection weight, which their and our "
    "results suggest should also saturate on dense pools and miss carriers on sparse ones (it is "
    "sample-independent by design).",

    "<b>Extensions.</b> Per-target class CDFs (partitioning runs by sub-tile or normal cluster with per-class "
    "subtotals) would make P target-dependent with exact probabilities at O(L) extra work; given the "
    "saturation result, we expect them to pay off only across strong survival gradients. A tiny-MLP score "
    "head is a drop-in EMA replacement under the same unbiasedness guarantee, with the same caveat. The "
    "dual-direction diagnostic generalizes to any GRIS system with paired shift directions and turns "
    "residual-hunting from bisection into measurement. The method is orthogonal to our volumetric ReSTIR "
    "work: fog-cluster runs already flow through the same CDF machinery and a volumetric evaluation is "
    "planned as an addition, not a dependency.",
]:
    S.append(Paragraph(p, body))

# ============================================================ references ====
S.append(Paragraph("References", h1))
for r in [
    "Bitterli, B., Wyman, C., Pharr, M., Shirley, P., Lefohn, A., Jarosz, W. 2020. Spatiotemporal reservoir "
    "resampling for real-time ray tracing with dynamic direct lighting. ACM TOG 39(4).",
    "Boksansky, J., Meister, D. 2025. Neural visibility cache for real-time light sampling. arXiv:2506.05930.",
    "Chao, M.T. 1982. A general purpose unequal probability sampling plan. Biometrika 69.",
    "Figueiredo, P., He, Q., et al. 2025. Neural importance sampling of many lights. SIGGRAPH 2025.",
    "Hedstrom, T., Kettunen, M., Lin, D., Wyman, C., Li, T.-M. 2026. Stochastic pairwise MIS for unbiased "
    "large-kernel reuse in real-time. CGF 45(2) (Eurographics 2026).",
    "Josse, M., Litalien, J., Gruson, A. 2025. Adaptive neural kernels for gradient-domain rendering. "
    "SIGGRAPH Asia 2025.",
    "Junkins, O., Kettunen, M., Lin, D., Ramamoorthi, R., Wyman, C. 2026. Compatibility-guided neighbor "
    "selection for ReSTIR. PACMCGIT 9(4) (HPG 2026).",
    "Kettunen, M., Lin, D., Ramamoorthi, R., Bashford-Rogers, T., Wyman, C. 2023. Conditional resampled "
    "importance sampling and ReSTIR. SIGGRAPH Asia 2023.",
    "Lin, D., Kettunen, M., Bitterli, B., Pantaleoni, J., Yuksel, C., Wyman, C. 2022. Generalized resampled "
    "importance sampling: foundations of ReSTIR. ACM TOG 41(4).",
    "Lin, D., Kettunen, M., Wyman, C. 2026. ReSTIR PT enhanced: algorithmic advances for faster and more "
    "robust ReSTIR path tracing. PACMCGIT 9(1) (I3D 2026).",
    "Manzi, M., Rousselle, F., Kettunen, M., Lehtinen, J., Zwicker, M. 2014. Improved sampling for "
    "gradient-domain Metropolis light transport. ACM TOG 33(6).",
    "Muller, T., Rousselle, F., Novak, J., Keller, A. 2021. Real-time neural radiance caching for path "
    "tracing. ACM TOG 40(4).",
    "Ouyang, Y., Liu, S., Kettunen, M., Pharr, M., Pantaleoni, J. 2021. ReSTIR GI: path resampling for "
    "real-time path tracing. CGF 40(8).",
    "Salaun, C., Balint, M., Belcour, L., Heitz, E., Singh, G., Myszkowski, K. 2025. Histogram "
    "stratification for spatio-temporal reservoir sampling. SIGGRAPH 2025.",
    "Sawhney, R., Lin, D., Kettunen, M., Bitterli, B., Ramamoorthi, R., Wyman, C., Pharr, M. 2024. "
    "Decorrelating ReSTIR samplers via MCMC mutations. ACM TOG 43(1).",
    "Talbot, J., Cline, D., Egbert, P. 2005. Importance resampling for global illumination. EGSR 2005.",
    "Tokuyoshi, Y. 2023. Efficient spatial resampling using the PDF similarity. PACMCGIT 6(1) (I3D 2023).",
    "Wyman, C., Kettunen, M., Lin, D., Bitterli, B., Yuksel, C., Jarosz, W., Kozlowski, P. 2023. A gentle "
    "introduction to ReSTIR: path reuse in real-time. SIGGRAPH 2023 Courses.",
    "Zeltner, T., Rousselle, F., Weidlich, A., et al. 2024. Real-time neural appearance models. ACM TOG 43(3).",
    "Zhang, S., Lin, D., Kettunen, M., Yuksel, C., Wyman, C. 2024. Area ReSTIR: resampling for real-time "
    "defocus and antialiasing. ACM TOG 43(4).",
]:
    S.append(Paragraph(r, ref_st))

# ================================================================ build =====
doc = BaseDocTemplate(OUT, pagesize=letter, leftMargin=MARGIN, rightMargin=MARGIN,
                      topMargin=MARGIN, bottomMargin=MARGIN,
                      title="Shift-Aware Candidate Selection for Stochastic Pairwise MIS (draft)")

full = Frame(MARGIN, MARGIN, PAGE_W - 2 * MARGIN, PAGE_H - 2 * MARGIN, id="full")
col1 = Frame(MARGIN, MARGIN, COL_W, PAGE_H - 2 * MARGIN, id="col1")
col2 = Frame(MARGIN + COL_W + COL_GAP, MARGIN, COL_W, PAGE_H - 2 * MARGIN, id="col2")

doc.addPageTemplates([
    PageTemplate(id="first", frames=[Frame(MARGIN, PAGE_H - MARGIN - 3.4 * inch,
                                           PAGE_W - 2 * MARGIN, 3.4 * inch, id="head"),
                                     Frame(MARGIN, MARGIN, COL_W,
                                           PAGE_H - 2 * MARGIN - 3.4 * inch, id="f1"),
                                     Frame(MARGIN + COL_W + COL_GAP, MARGIN, COL_W,
                                           PAGE_H - 2 * MARGIN - 3.4 * inch, id="f2")]),
    PageTemplate(id="twocol", frames=[col1, col2]),
])

doc.build(S)
print("wrote", OUT)
