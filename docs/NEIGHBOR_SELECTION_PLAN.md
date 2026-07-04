# Shift-Aware Neighbor Selection for ReSTIR Spatial Reuse — research log

Goal: importance-sample WHICH neighbors spatial reuse draws its candidates
from, proportionally to (an estimate of) the ideal post-shift resampling
weight `m_i(Y_i)·pHat(Y_i)·W_i·|dT_i/dX_i|`, fed into stochastic pairwise
MIS (SPMIS, Hedstrom et al. EG 2026) so that sample-dependent selection
stays unbiased. Paper draft: `docs/make_neighbor_paper.py` →
`docs/shift_aware_spmis_draft.pdf`.

## Novelty position (from the July 2026 literature sweep)

- SPMIS (Hedstrom, Kettunen, Lin, Wyman, Li — CGF 45(2), EG 2026) selects
  candidates ∝ `c_i·pHat(X_i)·W_i` (UNSHIFTED source contribution) and
  names selection ∝ the post-shift weight verbatim as open future work.
  Unbiasedness needs only P(i) > 0 on the contribution support, P may
  depend on samples/UCWs (their Appendix B).
- Junkins et al. (HPG 2026, "Compatibility-Guided Neighbor Selection")
  select ∝ an analytic sample-INDEPENDENT G-buffer score (position +
  normal edge-stopping); they name "combine with SPMIS" as future work.
  Must be cited as concurrent work; a comparison is expected.
- Salaün et al. (SIGGRAPH 2025) = the antithetic histogram stratification
  our mode-0 spatial reuse implements (uniform marginal, no importance).
- Nobody uses the renderer's own realized shift outcomes for selection.
  Nobody composes stratified draws with the SPMIS compensation.

Claim: first realization of (an estimate of) the ideal selection density,
via an online-learned shift-success score, unbiased for ANY predictor
quality; plus a first-moment lemma (E[K(i)] = Ñ·P(i) suffices — any
correlated/stratified drawing scheme with correct per-draw marginals)
that licenses antithetic CDF draws and history-dependent P.

## What is implemented (ROYALGL_RESTIR_SPMODE)

- **mode 0** — pair mixture (previous behavior, Salaün-stratified ranks,
  K pair estimators vs the frozen canonical mixed 1/K). Untouched;
  regression-checked bit-band-identical soaks.
- **mode 1** — SPMIS baseline mapped onto the block/cluster-run
  infrastructure: ssort builds per-run inclusive CDFs of
  `s_i = min(c_i,cap)·pHat(X_i)·W_i` + run confidence sums cΣ (segmented
  Hillis-Steele over the sorted 256 entries; exact because equal keys are
  contiguous). sinit draws one candidate per round by exact CDF inversion
  (strict compare ⇒ P(z) = s_z/S exactly, zero-weight ranks unselectable),
  creates ONE forward job per round + ONE backward job in round 0 only
  (Ñc = 1 canonical estimate, Pc = 1/count uniform-over-run, z' may be
  self → identity synthesized in smerge). smerge folds everything into
  ONE reservoir chain with the scaled stochastic pairwise weights
  (`m̃_z = 1/(Ñ P(z)) · (ĉΣ/(ĉΣ+c_c)) · ĉ_z p̂←z / (ĉΣ p̂←z + c_c p̂)`,
  all non-canonical confidences scaled Ñ/M per their Sec. 4.3;
  `m̃_c = c_c/(ĉΣ+c_c) + count·β_z'`). Cell search (their Sec. 5.1):
  8 probes, growing radius (24px ×1.25), WRS by run confidence mass,
  unconditional.
- **mode 2** — ours = mode 1 plus:
  - learned per-pixel shift-success score v ∈ [0.05, 1] (EMA α = 1/8 of
    clamped survival ratios `pHat(T(X))·J / pHat(X)` from the shifts the
    pixel REQUESTS: backward z' (own sample out) + forward candidates
    (neighbor samples in); own-slot writes only, no atomics), folded
    into s_i; REPROJECTED along temporal pairing and RESET on
    disocclusion in tinit (critical: without this, disocclusion noise is
    WORSE than baseline — stale scores starve the only good sources);
  - antithetic stratified positions on the selection CDF (Sobol u per
    pixel/frame, ROYALGL_RESTIR_STRAT gates);
  - cell search gated on starvation (own-run mean confidence < cap/2)
    and weighted by Gaussian distance kernel (σ=32px) × probe score.

Env vars: `ROYALGL_RESTIR_SPMODE=0/1/2`, `ROYALGL_RESTIR_VSCORE=0`
(mode-2 ablation: drop score factor), `ROYALGL_RESTIR_CELLSEARCH=0`,
plus the existing `ROYALGL_RESTIR_STRAT`. New test infra:
`ROYALGL_STATS_MASK=1` (disocclusion-restricted stats line "MaskStats";
flag = tinit found no usable history, surface pixels only),
`ROYALGL_FIXED_DT=<s>` (frame-deterministic scripted motion for fair
A/B across configs with different frame cost), `ROYALGL_DUCKS=0..8`
(clutter variant of the fallback scene; default 0 keeps every recorded
soak reference valid).

Storage: arena grew 27 → 28 vec4/pixel (region [27N,28N): .x score EMA,
.y disocclusion flag, .z chosen reuse run as uintBits(pix+1), .w free).
Sort region repacked: .y = uintBits(start | count<<16), .z = cΣ,
.w = rank-indexed inclusive CDF. `PathTracer::ReadDisocclusionMask()`
reads the flags back for masked stats.

## Findings so far (Debug build, 1600×900, K=3, cap 8, decorr off)

- Temporal OFF (cell search off), per-frame relNoise:
  m0 0.187 / m1 0.140 / m2 0.137 (glass);
  conductor: 0.193 / 0.141 / 0.133 — the score's clearest win;
  roughglass: 0.190 / 0.139 / 0.137.
  Spatial pass: 13.2 / 11.0 / 10.7 ms (K+1 vs 2K shifts).
- Temporal ON, thin disocclusion bands (fixed-dt duck/dolly, band ≤ 1-2%
  of pixels): all contribution-aware selectors nearly saturate (runs
  still contain valid-history sources); ours −2%, SPMIS −1.5% masked vs
  m0. Wall-clock-paced fast duck (wider bands): masked m0 0.366 /
  m1 0.339 / m2 0.305. Equal-time favors modes 1/2 everywhere (frame
  total −12–35% depending on scene).
- Clutter scene (8 ducks): spatial 15.0 / 9.7 / 10.5 ms, masked noise
  parity — separation needs wider disocclusions than the thin dolly
  bands (next: stronger motion + Release build protocol).
- Unbiasedness soaks (locked camera): the SPMIS chain carries a SMALL REAL
  POSITIVE RESIDUAL. Same-convention references on the current build:
  RIS-only = temporal-only = mode-0 spatial-only = mode-0 T+S(≈) =
  **0.116474** (exact chain, repeatable to 1e-6). Mode 1 spatial-only
  0.116487 (+0.011%), mode-1 T+S 0.116518 (+0.038%, compounds through the
  temporal chain; stable to 10k samples). Mode 2 within noise of mode 1.

## The +0.011% SPMIS residual — investigation state (2026-07-04)

Everything ruled OUT by direct experiment:
- selection probabilities: uniform-P variant (P = 1/count exact, uniform
  rank draws) shows the SAME bias → the weighted CDF/its float precision
  is innocent (P matches the actual draw law by construction anyway);
- the draw-accounting identity E[Σ 1/(Ñ P count)] = contributing fraction
  holds (0.997 measured ≈ contributing-entry fraction);
- ssort run sums exact (cΣ/count ≡ 1.0 at unit confidences);
- xorshift selection: mode 0 with STRAT=0 (same rng pattern) is exact;
- estimator STRUCTURE: a CPU Monte Carlo of the exact chain (scaled
  pairwise weights, Ñc = 1 β-estimate, with-replacement draws, duplicate
  self-input, RIS-style correlated UCWs) is unbiased to < 1e-4;
- confidence handling: bias present with all-unit confidences;
- caustic pass, light tracing: bias present without both.

What is KNOWN about it:
- K = 1 is EXACT (0.116474); bias grows ~linearly in K−1 and with the
  γ-scaling removed (γ = 1: +0.06%) — overall it tracks
  ((ĉΣ−c_c)/(ĉΣ+c_c))², the asymmetry of the SPMIS weight functional
  away from the pair-mode 1:1 case;
- pixel-space it is a UNIFORM +0.01..0.02% brightening of diffuse
  surfaces (no caustic/emitter hotspots);
- the m̃c/β channel has ~0.75% total-energy sensitivity to the backward
  shift results (forcing pHatBack := p̂_c darkens by that much), so a
  ~1.5%-of-channel systematic in backward-vs-forward shift outcomes fully
  accounts for the residual.

LEADING HYPOTHESIS (2026-07-04, morning): a small pre-existing
forward/backward support or value asymmetry in the hybrid shift
machinery, amplified by the SPMIS functional.
=> REFUTED by the dual-direction diagnostic - see the "Residual
mechanism" section below for the measured replacement theory.

## Experiment-hygiene gotchas (cost hours; do not repeat)

- PowerShell env vars LEAK between script invocations in one process —
  soak.ps1/experiment.ps1 now clear env:ROYALGL_* first. Any experiment
  batch run before that fix that toggled different keys per iteration is
  suspect (the "v-score-only" conductor ablation was contaminated).
- Wall-clock-paced motion confounds A/B between configs of different
  speed (faster mode ⇒ smaller per-frame steps ⇒ easier reuse AND
  smaller disocclusion fraction). Use ROYALGL_FIXED_DT.
- The moving-duck test additionally depends on async BLAS rebuild timing
  (effective matrices lag wall-clock-dependently) — even with fixed dt,
  duck-motion masked fractions drift between modes. Camera dolly/orbit
  are the deterministic stressors.
- "Stats @" grep must anchor "] Stats @" (MaskStats lines match the bare
  pattern).
- First app start after a driver-level shader-cache invalidation takes
  >12 s before the first stats line; soak scripts need ≥25 s or a warmup
  run.

## Temporal-free round (2026-07-04, after user feedback)

New goal: match temporal+spatial quality with SPATIAL-ONLY reuse (drop
the temporal chain: no history, no disocclusion artifacts, no lag).
Target (static Cornell, per-frame relNoise, K=3 pair-mixture T+S): 0.0794.

Findings, in order:
- K sweep (unbatched): K=6 0.117 / K=8 0.104 / K=12 0.089 — width works
  but cost was linear at ~1.9 ms/candidate: every round ran the FULL
  maxLen-deep shift pipeline.
- ITERATED spatial passes (J passes chained via final->scratch copy,
  ROYALGL_RESTIR_SPATIAL_ITERS, running-mean shading across passes):
  NEGATIVE RESULT — J2K3 0.1275 vs single-pass K=6 0.117 at equal shifts.
  Within-frame aggregates share the run's samples; re-mixing them adds
  little diversity (temporal wins because history carries genuinely NEW
  samples). Machinery kept for ablations.
- BATCHED candidates (WF_SPATIAL_BATCH=4 per shift-pipeline sweep; job
  region now 5 slots/pixel, arena 64 vec4/pixel, queues 15N):
  K=16 spatial-only = 0.0811 static (target 0.0794), K=24 = 0.0725
  (beats it). Sweep overhead saved ~4 ms; the wall is real shift work
  (t=1 reverse-hybrid replays dominate on Cornell).
- MOTION head-to-head (MOVE=4, FIXED_DT=0.08): spatial-only K=16
  whole-image 0.0794 ≤ T+S 0.0804, while T+S's disoccluded band (1.6% of
  pixels) sits at 0.4149 — 5x worse than spatial-only's uniform noise —
  and T+S loses ~0.9% energy transiently (history invalidation); the
  spatial-only mean stays correct.
- Mode-2 refinements (score/antithetic) wash out at K >= 12: selection
  quality stops mattering once draws cover the contributing set; the
  variance is shift-outcome-dominated.
- Cost status (Debug): spatial-only K=16 ~29-35 ms vs T+S reuse ~16-19 ms
  (~2x). Remaining cost levers: reconnection-only fast path in the shift
  kernels, cheaper t=1 replays, reciprocal shift sharing (ReSTIR PT
  Enhanced), Release build.
- CAVEAT (critical path now): the SPMIS-chain residual GROWS with K:
  +1.3e-5 (K=3) -> +3.3e-5 (K=8) -> ~+8e-5 (K=16, +0.07%). The
  shift-asymmetry diagnostic below must be resolved before the paper's
  headline config is credible.

Paper story update: "temporal-quality ReSTIR without temporal reuse" —
uniform per-frame noise at T+S level, no disocclusion artifacts, correct
energy under motion, at ~2x reuse cost (unoptimized). SPMIS's cheap
large-kernel machinery + batching is what makes K=16-24 feasible.

## Reservoir butterfly (2026-07-04, SPMODE=3) — THE efficient scaling

User pushback: linear-in-shifts scaling isn't enough. Answer: J levels of
reciprocal XOR-partner pair merges (partners at 1,1,2,2,4,4,... px,
alternating axes, per-frame jittered grid): each level costs ONE shift
per pixel — the reverse shift IS the partner's forward job (XOR is an
involution, the cluster test is mutual, so pairing is symmetric and both
directions are computed exactly once) — and aggregates hold up to 2^J
disjoint initial samples. Levels ping-pong scratch <-> final; the last
level mirrors its path result into the final region IN-SHADER (a host
region copy loses the caustic reservoir that lives there: -0.3%, fixed).
Deterministic index-based pairing is sample-value-independent (the
chained-merge/value-correlation trap does not apply), and the 2-way
balance-heuristic merge is the SYMMETRIC functional:
**butterfly soaks are exact (0.116476-477 vs reference 0.116474, both
parities) — no SPMIS-chain residual.**

Results (spatial-only, per-frame relNoise, static | fast motion):
- T+S reference (m0, K=3):        0.0794 | 0.0804   reuse ~14.5 ms
- butterfly J=3 (3 shifts/px!):   0.0788 | 0.0748   spatial  9.5-9.7 ms
- butterfly J=5:                  0.0780 |   -      spatial 14.4 ms
- SPMIS chain K=16 (for scale):   0.0811 |   -      spatial 29 ms
J>=4 plateaus (J=6 0.0812): NOT the confidence cap (cap=32 J=5: 0.0795)
— longer-distance shift failure + intra-group correlation. Sweet spot
J=3-5. Caveat for the paper: relNoise's 4-neighbor window under-reports
block-scale correlation for large groups; validate blotchiness with the
dupmap / accumulated soaks (accumulated J=5 converges FASTER than all
other configs: 0.0299 @1536 vs 0.0311-0.0313) and visual crops.

=> Headline: spatial-only butterfly BEATS temporal+spatial quality at
~25-35% LOWER total frame cost, exactly unbiased, zero temporal
artifacts. K (neighbors setting) = level count in mode 3.

## Butterfly removal + two-stage selection round (2026-07-04, later)

- BUTTERFLY REMOVED on user feedback: the 2^J-pixel groups sharing
  aggregates read as RESOLUTION LOSS despite exact means and good noise
  numbers (relNoise's blindness to block correlation flagged earlier was
  precisely the failure). Code parked under `#if 0` in smerge + log notes;
  the batching infra (WF_SPATIAL_BATCH, 64-vec4 arena) stays.
- MODE-2 TWO-STAGE SELECTION implemented (ROYALGL_RESTIR_TWOSTAGE,
  default OFF): T = 16 CDF pre-candidates, finalists drawn half-baseline
  (prop. to q) / half by a ray-free reconnection SURVIVAL-RATIO proxy
  (target-side over source-side cos/d^2 toward the stored rc vertex,
  clamped [1/8, 8], x learned score), with exact nested compensation
  1/(P2 q T N~_sweep sweeps). Mechanics PROVEN exact (g := q degenerate
  test reproduces the plain chain bit-for-band). Findings on Cornell,
  K = 8, temporal off: plain q-selection 0.104; two-stage uniform-anchor
  0.115; q-anchor (unbounded compensation) 0.143 + fireflies; pure
  absolute-proxy 0.45 (catastrophic - stage-1 q already carries source
  brightness, stage 2 must be a RATIO). Conclusion: on geometrically
  simple scenes survival ratios are ~1 and the compensation variance
  outweighs the guidance at K >= 8; the mechanism needs scenes with
  strong survival variation (curvature, glossy anchors, shadow edges,
  clutter) to earn its keep - exactly where the score already won at
  K = 3. Tune there before enabling by default; possible refinements:
  anchor-mixture weight schedule by measured proxy dispersion, BSDF-aware
  proxy for glossy anchors, per-slot clamp learned from realized ratios.
- jb.w CONVENTION unified: sinit always stores the COMPLETE stochastic-
  MIS scale (1/(N~ P) single-stage, full nested factor two-stage); smerge
  applies it verbatim. (A semantic mismatch here briefly rendered mode-2
  defaults 3x dark - caught by soak.)
- Verified after all changes: m0 default 0.116486, m1/m2 K=8 spatial-only
  0.116509 (the known K-scaling chain residual; investigation queue
  unchanged).

## Reset to faithful baseline (2026-07-04, latest)

User call: the novel selection layers underperform plain SPMIS on our
scenes - REMOVED from the reachable surface (SPMODE clamped 0/1, UI 2
entries; mode-2 code paths are dead behind the clamp and the default-off
TWOSTAGE flag - delete in a cleanup pass). Paper audit done against the
full text: estimator faithful (Eq. 15/16/18/19, N~/M scaling, N~c=1,
with-replacement, streaming merge); cell search corrected to the paper's
12 iterations / 30px / x1.25. REMAINING DEVIATIONS: (a) cells = 16x16
sorted cluster runs keyed instance+material vs the paper's 8x8 tiles
hashed objectID + QUANTIZED NORMAL - on curved geometry their cells
split by orientation, ours do not; adding a 2-3 bit quantized-normal
term to WfClusterKey is the top faithfulness item (affects mode-0
clustering too - needs its own soak round); (b) the search-config-
dependent soak-mean drift (paper-search m1 temporal-on: 0.116438, BELOW
reference - the residual flips sign with reuse distance, further
implicating the shift-machinery asymmetry, not the chain math).

## Search-radius regression: attributed and fixed (2026-07-04, gate round)

The "paper search made MOVE=4 60% noisier" regression is fully attributed
with deterministic dolly/orbit + FIXED_DT (all mode 1, K=3, whole-image
per-frame relNoise; masked = disocclusion-band):

- dolly 0.25 (thin bands): off 0.0894 / it8_r24 0.1098 / it8_r30 0.1152 /
  it12_r24 0.1232 / it12_r30 0.1289. Monotone in BOTH knobs; search OFF
  is best; masked noise FLAT (0.45-0.47) across all configs.
- orbit 30 deg/s (WIDE 64px/frame fresh strips, maskFrac 1.9%): off
  whole 0.0884 masked 0.298; unconditional 12/30 whole 0.1289 masked
  0.329. Even the fresh strip reuses better from its own run's 256
  same-frame candidates than from distant converged runs.

ROOT CAUSE: the paper's search is UNCONDITIONAL WRS over probed runs -
the own run survives with P ~ own/(own + sum probes) ~ 1/(1+iters), so at
steady state ~90% of pixels swap close partners for foreign runs up to
350px away (30 * 1.25^11); long shifts fail more, and SPMIS's cS:cc
weighting amplifies failed-shift cost. 8/24 was a milder dose of the same
tax, not a better shape.

FIX (new default): ROYALGL_RESTIR_SEARCH_GATE=1 - probe ONLY near-empty
runs (count <= 2). Measured == no-search everywhere on Cornell (dolly
0.0893/0.453, orbit 0.0884/0.299) while keeping rescue for partnerless
pixels (cluster slivers; will matter more once WfClusterKey splits by
normal). GATE=2 (also probe on the tinit disocc flag) HURTS the band it
targets (masked 0.328 vs 0.298) - rejected. GATE=0 = faithful paper
config, kept for baseline comparisons. Confidence-threshold gating
(mean c < cap/2) was tried and rejected: ROYALGL_RESTIR_DECORR keeps
effective confidence low in correlated regions, so it probes forever
there (+4% whole-image under orbit).

Env plumbing added (no rebuilds for sweeps): ROYALGL_RESTIR_SEARCH_ITERS
(bits 16-20 of restirParams.y), _SEARCH_RADIUS px (21-28), _SEARCH_GATE
(29-30); new FrameUBO lane spmisParams: .x = mode-2 score EMA rate
(ROYALGL_RESTIR_EMA, default 1/8), .y = mode-2 defensive mix
(ROYALGL_RESTIR_DEFMIX, default 0.25), .zw reserved for diagnostics.

Soak note (gate1 default): locked-cam m1 T+S sits at 0.116544 (+0.06% vs
the 0.116474 exact reference) - ABOVE reference with short-distance
reuse, while the unconditional-search band 0.116438-0.116450 sits BELOW.
The sign flip with reuse distance is now reproduced ON DEMAND by the
gate toggle alone: one more independent pointer at the forward/backward
shift asymmetry (see diagnostic below).

## History-guided selection: KILLED with full attribution (2026-07-04)

Prove-or-kill on its chosen home turf: DUCKS=8 clutter + conductor main
duck, deterministic dolly (parallax silhouettes) + orbit 30 (wide fresh
strips), masked stats, K=3 and K=8, gate1 search default, whole-image
per-frame relNoise / masked relNoise:

1. OUTGOING-CONTRIBUTION observation (the 2026-07-04 "final round"
   design, EMA of pHat(T(X))*|J|*W): loses everywhere - dolly +4%,
   orbit +14-17%, masked never better - and EXPLODES fireflies under
   orbit (hot 319-400 vs <= 45 for mode 1). Mechanism confirmed by
   dose-response: defensive mix 0.25/0.50/0.75 gives hot 319/51/17 and
   relNoise 0.0889/0.0830/0.0791 -> 0.0762 at mix=1 == mode 1 exactly.
   The observation is an absolute, heavy-tailed contribution estimate
   that double-counts source brightness already in the SPMIS weight
   c*pHat*W; concentration of P inflates the 1/(N~ P) MIS weights.
   Same failure class as the two-stage absolute-proxy catastrophe.
2. VSCORE=0 (antithetic draws only) == mode 1 to the fourth digit
   (0.0762/hot 18/mask 0.274 identical): the draws are free, the score
   was 100% of the loss.
3. RATIO observation (fixed design: bounded survival EMA
   clamp(pHat(T(X))*|J|/pHat(X), 0.02, 1), multiplicative on the SPMIS
   weight, defensive floor dm + (1-dm)*A): pathology gone (hot back to
   baseline), but a WASH - dolly K3 0.0987 vs 0.0984, K8 0.0831 vs
   0.0829; orbit K3 0.0986 vs 0.0973, K8 0.0777 vs 0.0762; masked equal
   (band scores are RESET, the prior lives in surviving neighbors -
   measured effect ~-2% masked at best, paid back whole-image).
4. EMA rate sweep (learning-speed hypothesis, 1 obs/frame vs 25-frame
   re-disocclusion cadence): alpha=1/2 best of the family (orbit K8
   0.0771) but still > mode 1; alpha=1 worse (0.0797, single-sample
   noise). Learning speed is NOT the bottleneck.

Verdict: selection among LOCAL runs is saturated - c*pHat*W already
ranks a 256-entry block run well enough that K in {3,8} draws cover the
contributing set; per-pixel shift survival carries no exploitable extra
structure on these scenes at any observation design or dose. The
m-factor / incoming-channel refinements were not pursued: they refine a
signal whose cleanest bounded form is already a wash (and the ratio IS
the survival part of the ideal density - what remains is contested-ness,
which the pairwise weights handle post-hoc anyway). Additionally,
history-guided selection is structurally incompatible with the
temporal-FREE headline mode: score reprojection rides tinit's temporal
pairing, which does not run there.

Kept in-tree: the ratio-form mode 2 (honest ablation arm for the paper's
negative-result section - SPMIS named the ideal selection density as
open future work; we demonstrate both naive realizations of it and why
they fail: absolute -> 1/P fireflies, bounded ratio -> saturation wash).
Paper framing: the selection DENSITY is not where the headroom is;
shift SUCCESS (deeper-reconnection fallback) and shift COST are.

## Quantized-normal cluster key: adopted + re-baseline (2026-07-04)

WfClusterKey now keys (instance << 8 | material) | octant << 24, where
octant = 3 sign bits of the world-space shading normal
(ROYALGL_RESTIR_CLUSTER_NORMAL, default ON; bit 31 of restirParams.y).
Closes faithfulness deviation (a) vs paper Sec. 5.1 (they hash objectID
+ quantized normal); our cells remain 16x16 sorted runs, not 8x8 hashed
tiles (documented structural deviation, unchanged).

A/B (conductor duck; static temporal-off + DUCKS=8 dolly; modes 0/1):
small consistent WIN everywhere - stat m0 0.1979->0.1973, stat m1
0.1622->0.1615, dolly m0 0.0943->0.0939, dolly m1 0.0984->0.0977 (4/4
cells; duck is a small screen fraction, so duck-local gains are ~20x
larger than the whole-image deltas). Means/masks/hot unchanged.
Synergy with gate1: orientation-split runs produce more count<=2
slivers, exactly the population the gated search rescues.

SOAK RE-BASELINE (locked cam, DECORR=0, 55s, normal key ON, gate1):
- RIS-only                      0.116474  (anchor, exact, unchanged)
- mode-0 spatial-only           0.116476  (exact band, unchanged)
- mode-0 T+S default            0.116481  (old band 0.116473-0.116482)
- mode-1 T+S gate1 (default)    0.116550  (+0.065%, was 0.116544 pre-key)
- mode-1 T+S gate0 (faithful)   0.116379  (-0.08% - MORE negative than
  the pre-key unconditional band 0.116438-0.116450: normal-split runs
  shrink own-run WRS mass, so unconditional search hops MORE/further;
  yet another reuse-distance dose-response of the residual sign)
- mode-2 T+S (ratio score)      0.116547  (== mode 1 gated; policy
  moves no energy, as designed)
- conductor mode-0 T+S          0.116620  (old band 0.116615-0.116619)
- fog interior default T+S      0.091581  (vs BDPT truth 0.09154,
  +0.04%; NOT the normal key - key-off run reads 0.091582. The Table-2
  era 0.091456 (-0.09%) does not reproduce on the neighbor-round source
  state; same known build-vs-band class, now on the truth's other side
  and closer to it.)

## Residual mechanism: asymmetry hypothesis REFUTED, replaced (2026-07-04)

TOOLING (kept in-tree): ROYALGL_RESTIR_SHIFTDIAG=1..6 - round 1 re-uses
the idle backward job slot to run the FORWARD shift z'->c of the SAME
pair whose backward shift c->z' ran in round 0 (mode 1, temporal off,
K>=5); tallies accumulate cross-frame in the learn region, host logs
"ShiftDiag" lines. ROYALGL_RESTIR_SHIFTDIAG_DIST=<px> replaces the reuse
run by an INVOLUTIVE fixed-stride partner (x-cells pair mutually), so
distant-pair ensembles stay exchangeable - the cell search's chosen runs
are role-asymmetric and confound direction tests (measured: +7.2%
fwd-fail excess under gate0 search, an ENSEMBLE property, not machinery).
Sub-modes: 1 = paired support-class counts, 2 = valid/both-ok, 3 = sum
log(rF/rB), 4 = sum rB / sum rF, 5 = m~_c's count*beta evaluated through
both directions (the exact bias channel), 6 = force pHatBack:=0
(deliberately biased headroom probe).

MEASUREMENTS (locked cam, spatial-only m1 K=8, 1600x900, ~800k pairs x
1100-1600 frames each):
- LOCAL pairs (own 16x16 run, exchangeable): fwd-only-fail 1.342% vs
  bwd-only-fail 1.343% (balanced to 1.4e-4 rel); mean log(rF/rB)
  = -1.2e-5; count*beta through bwd vs mirrored fwd: 7.59189e7 vs
  7.59190e7 (SYMMETRIC TO 1e-7). Chain mean +3.0e-4 rel elevated.
  => NO direction asymmetry where the local residual lives. REFUTED.
- INVOLUTIVE DISTANT pairs: 48px: fail rate 3.18%/class, direction gap
  +4.5e-4; 96px: 3.78%/class, gap -3.8e-4 (sign-UNSTABLE, second
  order). Chain means: +1.18e-3 (48px), +1.27e-3 (96px) rel - the
  residual scales with the SYMMETRIC failure rate, not with any
  direction gap.
- Headroom probe (diag 6, pHatBack:=0 for all pairs): mean 0.2006 =
  +72%, hot 2756. beta(0) awards m~_c the FULL prior share
  cZs/(cS+cC) (sums to m~_c = 1 over the run) while forward candidates
  still merge - the beta channel has enormous per-event headroom.

REPLACEMENT MECHANISM (all observations fit): the residual is
FALSE-NEGATIVE SUPPORT FAILURES x the SPMIS weight functional's
imbalance, direction-symmetric in COUNT but not in per-event ENERGY:
- a backward false-fail INFLATES m~_c by count*[beta(0)-beta(true)]
  (positive energy, huge headroom);
- a forward false-fail LOSES that candidate's m~_z contribution
  (negative energy, sample-brightness-sized);
- equal counts, unequal magnitudes => net bias. Local net positive
  (m~_c side wins); the cell search's role-asymmetric ensemble adds
  +7.2% fwd-fail excess => candidate-loss dominates => T+S gate0 sits
  BELOW reference (sign flip explained); K-scaling via the
  ((cS-cC)/(cS+cC))^2 functional factor (K=1: cS=cC => 0, matches K=1
  exactness); distance scaling via the failure rate (1.34% -> 3.18% ->
  3.78% tracks +3.0e-4 -> +1.18e-3 -> +1.27e-3).

FIX DIRECTIONS (ranked): (1) raise real shift success - the
deeper-reconnection fallback (fresh angle #1) now doubles as the BIAS
fix, since false negatives ARE failed shifts of reachable paths;
(2) defensive beta on backward failure (cap the awarded prior share) -
biased both directions, needs care; (3) paper: report residual +
mechanism honestly; the estimator math itself is verified clean (CPU MC)
and the diagnostic bounds the machinery's direction asymmetry at 1e-7
locally.

## Cleanup + paper refresh round (2026-07-04, end of session)

- DEAD CODE REMOVED: the mode-2 two-stage block (sinit 513 -> 383 lines,
  TWOSTAGE flag/env/settings gone; findings preserved in this log), the
  butterfly #if 0 block in smerge (598 -> 498 lines; tombstone comment
  points at git history). Flag bit 16384 retired. Post-cleanup soaks
  bit-consistent with the re-baseline: m0 0.116483 / m1 0.116550 /
  m2 0.116551.
- ANTITHETIC LEMMA DRAWS re-added to the reachable surface, mode 2 only
  (mode 1 stays faithful-i.i.d.): stratified antithetic positions on the
  selection CDF, gated by ROYALGL_RESTIR_STRAT. Measured a WASH at K=3
  on Cornell blocks (glass 0.1575 vs 0.1578; conductor/roughglass same)
  - consistent with the old mode-0 stratification finding (blocks
  locally uniform). Kept: zero cost, theoretically licensed, honest
  "license not win" framing in the paper. m2 soak 0.116533 (within the
  4e-5 inconclusive band of the m1/m2 references).
- The old Table-1-era m1 numbers (0.140-class) do NOT reproduce on the
  batched chain (0.158-class); m0 matches. Era archaeology not pursued
  (unbatched chain, different draw/RNG structure). All paper numbers are
  now current-build self-consistent (Table-1 batch: glass/conductor/
  roughglass x m0/m1/m2, spatial GPU 9.1-9.7 / 7.3-7.6 / 7.3-7.4 ms).
- PAPER REWRITTEN to the honest post-findings story (6 pages,
  make_neighbor_paper.py; build with `py`): title now "...Realization,
  Limits, and Diagnostics"; abstract/intro/method/discussion carry the
  saturation verdict, the firefly mechanism + dose-response, the search
  tax + tiny-run gate, the 1e-7 asymmetry bound + false-negative
  mechanism, and the temporal-free headline. New Table 3 (search
  sweep); Table 4 = soak/residual attribution; Tables 5a/5b unchanged
  data with updated caveats (butterfly marked shelved-for-resolution-
  loss, numbers reproducible from git).

## LightMaze: the sparse-carrier scene (2026-07-04, user-directed)

Motivation (user): the selection machinery "does nothing" on Cornell
because pools are contribution-dense - build a scene where the light
tree can't carry, only sparse pixels hold signal. Result:
assets/scenes/LightMaze.glb (docs/make_maze_scene.py, 170 tris): a
diffuse room lit ONLY through a two-bend chicane from a hidden emitter
alcove. No-direct-line invariant (proved in the generator header):
emitter->door segments cross the baffle plane at z <= 0.249 < 0.35 =
baffle top, so camera-side NEE is fully occluded and every visible
surface is >= 2 bounces from the emitter. Camera
ROYALGL_CAM="1.45,1.25,1.5,-1.2,0.7,-1.0"; emitter strength 4000;
means: BDPT truth 0.015824, p50 0.0033 (parse with MinMean 0.001 - the
old 0.02 black-frame filter rejects the whole scene).

REGIME VERIFIED: SpmisIdent (contributing-entry fraction of reuse runs)
= 0.0106 vs 0.997 on Cornell - 94x sparser; ~44% of pixels sit in runs
with ZERO selection mass per frame (ident n / pixel count).

SELECTION FINALLY SEPARATES (temporal-off, per-frame relNoise):
- RIS only 1.48; m0 pair-mixture K3 1.69 / K8 1.58 - WORSE than no
  reuse (stratified rank picks feed dead neighbors into the 1/K mix);
- m1 SPMIS K3 0.865 (-49% vs m0) / K8 0.519 (-67%); m2 == m1 (survival
  among carriers is still not the bottleneck - FINDING carriers is,
  and c*pHat*W does that);
- unconditional search K8 1.18 vs gated 0.519: the paper's
  confidence-weighted probes are BLIND to contribution (confidence is
  uniform), so hops land on equally dead runs at longer shift distance.
Accumulated: RIS 0.133 @5.9k spp vs m1 K8 toff 0.055 @3.1k vs m1 T+S
0.025 @2.8k. Soaks: RIS 0.015886 = m1 toff 0.015882 = m1 T+S 0.015882
(chain clean here; dead runs generate no false-negative events). RIS vs
BDPT +0.39% = this scene's pixel-center V-buffer convention gap (larger
than Cornell's +-0.2%; renderer-internal comparisons unaffected).

GATE 3 NEGATIVE RESULT (measured + understood): contribution-aware
search (trigger on own-run S == 0, probes WRS-weighted by run S) is
BIASED +15.6% on the accumulated mean (0.018355 vs 0.015886), p50
doubles - the brightening is exactly the dead-branch pixels. THEORY:
the baseline's unbiasedness accounting is E[est] = P(dead)*0 +
P(live)*E[est|live] = I; a value-dependent pool swap that fires exactly
on the dead branch replaces the 0 with gathered energy the live
branches already carry at full weight. No branch-local compensation
exists - this is the pool-level form of the chained-merge
value-correlation trap, and it explains WHY the paper's search weights
by confidence: value-blindness is a correctness constraint, not a
missed optimization. Kept as ROYALGL_RESTIR_SEARCH_GATE=3 with BIASED
warnings (ablation/demonstration only).

UNBIASED ROAD (top of the fresh-angles queue now): ALWAYS-ON UNION
pools - probe positions value-independent, the union of own+probed
runs forms ONE selection CDF, draws compensated by the union's exact
P(z) = s_z / S_union (value dependence lives inside the per-draw
compensation, where SPMIS theory allows it). Needs: two-level CDF
inversion, union cS bookkeeping, and a deterministic distance kernel on
probe radius (value-independent) to avoid re-importing the dense-scene
distance tax. Expected: rescues the 44% dead-run population unbiasedly.

Infra added: ROYALGL_EXPOSURE env (figure renders on dark scenes).

## Beyond-SPMIS design round (2026-07-04, user: "unbiased, better than
## SPMIS, may work completely differently")

Options weighed against this project's measured constraints:
(A) SCRAMBLED MERGE TREES (butterfly descendant): rejected after a
    correctness analysis - the exponential aggregation REQUIRES
    disjoint merge trees (merging aggregates that share an ancestor
    breaks the UCW independence the pairwise weights assume; the
    measured 4%-dark chained-merge trap is exactly this), disjointness
    forces nested groups, and nested groups mean 2^J pixels share one
    leaf set = the resolution loss the butterfly was removed for.
    Within-group pairing scrambles keep the same leaf sets (cosmetic);
    cross-group random matchings collide trees (bias). A defensive
    per-pixel final pair dilutes blocks only by giving away the
    aggregation weight (block amplitude (cB/(cA+cB))/2^(J/2) vs own
    term cA/(cA+cB) - hiding blocks needs cB ~ 1-2, which caps the
    variance win at the same factor). Exponential sharing and block
    correlation are two faces of one structure. Parked with theory.
(B) UNION-POOL SPMIS with PROBE-MEASURED ideal weights - CHOSEN (user
    direction: sample neighbors ~ m_i(Y_i) pHat(Y_i) W_i |J_i| and feed
    them to SPMIS). Analysis first (reported in chat): with P ~ the
    true post-shift weight w_i, every draw contributes S_w/N~ exactly -
    selection variance is ZERO and an N~=1 chain reproduces full O(M)
    pairwise GRIS output; evaluating w needs the M shifts, so the whole
    game is estimate-then-compensate (any estimate is unbiased since
    the ACTUAL P is known; support floors mandatory). Our v1/v2 kills
    map onto the formula: v1 corrupted the algebra (absolute estimate
    multiplied INTO a weight already carrying pHat*W), v2 estimated a
    term (rho) that does not vary on local dense runs. The regime where
    the terms DO vary = wide/heterogeneous/union pools.

    DESIGN (ROYALGL_RESTIR_PSEL, mode 1, default off):
    - psel pass, per 16x16 block, once per frame: R = SearchIters probe
      positions around the block center (radius SearchRadius * 1.25^i,
      golden-angle directions, (frame,block) hashed - value-INDEPENDENT
      membership, no gate-3 dead-branch trap). Each probe resolves the
      landing pixel's run, dedups, records (runPix|count, key, cSigma_r,
      S_r) and ONE representative shift job: the run's top-pHat entry
      shifted into the block-center pixel.
    - probe pipeline sweep (45-72k jobs, ~1 sweep fixed cost), then
      pfin folds results into the per-block table: w^_r = S_r *
      clamp(rho*, 0, 8) * M*, rho* = pHat(Y)J/pHat(X*) from the probe,
      M* = cS pHat<-/(cS pHat<- + cC pHat(Y)) with block-center
      canonical as receiver rep - ALL FOUR ideal factors at run
      granularity, measured this frame, target(block)-aware. Table in
      new arena region [64N,65N): 2 vec4/entry, 32/block.
    - sinit round 0 per pixel: pool = own run + key-matching table
      entries (dedup vs own); pool sums M_pool, cSigma_pool stored in
      learn .w/.x; z' backward draw uniform over M_pool.
    - draws (all rounds): two-level with EXACT mixed P: with prob
      DEFMIX baseline (run ~ S_r, entry ~ s_z/S_r => P = s_z/S_pool),
      else guided (run ~ w^_r incl. analytic own-run entry rho=1, entry
      ~ s_z/S_r). jb.w = 1/(N~ P(z)) verbatim - SPMIS downstream
      untouched.
    - smerge under PSEL: countF = M_pool, cS = gamma cSigma_pool
      (learn region), chosen-run/cell-search machinery bypassed.
    - Support: P >= DEFMIX * s_z/S_pool > 0 on the pool's contributing
      set; all-dead pool => no draws (zero branch stays zero).
    Expected: degenerates to ~plain SPMIS on dense scenes (probes say
    rho~0 at distance -> guided mass stays local; baseline share keeps
    support), rescues sparse dead runs THROUGH the union with
    principled weighting. Cost ~0.4% extra shifts + one sweep overhead.
(C) Visibility-free targets + winner ray: highest ceiling on shift
    COST, but touches the #1 historical bias source (pHat
    conventions); after (B).
(D) Deeper-reconnection fallback: orthogonal multiplier (shift
    success = variance AND the residual mechanism); next after (B).

## PSEL v1 results (2026-07-04, end of session): unbiased, and it found
## the real wall

Implemented as designed (psel/pfin kernels, arena 65 vec4/px, table
region [64N,65N), two-level exact-P draws, smerge pool overrides).
Debug trail worth keeping:
1. B.w run-identity packing was missing -> entry draws used rank range
   [0,count) of the wrong block: -1.5e-4 soak + emitter W wobble. Fixed.
2. psel dropped zero-mass runs from the table -> pool MEMBERSHIP
   conditioned on contribution -> maze +8.7% bright with p50 doubling,
   EXACTLY the gate-3 dead-branch signature at pool level (second
   independent confirmation of the accounting theory). Fixed: dead runs
   stay members (unselectable entries, but their count/confidence join
   M_pool/cS and the backward-draw domain).
After both fixes: maze soak 0.0158831 == RIS 0.015886 (unbiased),
Cornell soak 0.116456-58 (the known distance-residual class, within
the 4e-5 band of reference), DEFMIX=1 == mixed (guidance adds no bias).

PERFORMANCE VERDICT: NEGATIVE in this functional. Maze K8 per-frame
0.93 vs plain m1 0.52 (accumulated ~2x worse); Cornell K3 0.207 vs
0.158 with per-frame hot ~160 (bounded 1/dm inflation tails).
ROOT CAUSE (design-level, not a bug): the SPMIS Sec-4.3 scaling
gamma = N~/M_pool distributes the non-canonical share BY MEMBER COUNT:
each candidate's pairwise weight carries c_z/cSigma_pool, so a union of
12 dead runs + 1 carrier gives the carrier 1/13 of the weight it had as
a lone pool while the canonical keeps the ceded share via the beta
channel (z' uniformly hits dead members whose backward pHat is
positive). Unbiased, but reuse efficiency collapses proportionally to
pool dilution - the functional ASSUMES homogeneous pools ("cells of
similar pixels"). The ideal-density guidance sits on top of a chain
that spends its confidence budget by headcount, so no selection quality
can rescue a dead-heavy union.

FORK FOR THE NEXT ROUND:
(i) small value-independent unions (own + 1-2 distance-kernel-chosen
    probe runs): bounded dilution, proportionally bounded rescue -
    cheap to try, unambitious;
(ii) PER-RUN pool functional: per-run cS_r in the candidate
    denominators + a per-run canonical channel (N~c = R backward
    estimates - the probe machinery already pays R shifts per block;
    reverse-direction probes could estimate the per-run beta_r sums).
    This is a real estimator extension: needs the partition written
    down and the CPU-MC toy verification FIRST (project hygiene rule).
Everything stays env-gated (ROYALGL_RESTIR_PSEL default off); no
default-path behavior changed (off-regression soak 0.116510 checked).

## Fresh angles for spatial reuse itself (with SPMIS in use)

Ranked by evidence from this project's own measurements (variance is
shift-OUTCOME dominated, cost is shift-RAY dominated, selection is
near-saturated at K >= 8):
1. **Deeper-reconnection fallback**: when the rc-at-x1 shift fails the
   Jacobian/footprint gates, retry reconnecting at x2 instead of failing
   the whole shift. Directly raises shift success - the actual variance
   driver everywhere we measured. Touches shiftstep only; unbiased if
   the fallback criterion is symmetric (same footprint logic).
2. **Visibility-free spatial target + one winner ray**: resample the
   whole spatial chain against p^* (no visibility), trace ONE shadow ray
   for the final winner (classic ReSTIR-DI, generalized to PT
   reconnections). Makes candidates ~ALU-only; K=16 at a few ms. Needs
   care with p-hat conventions (the codebase's #1 historical bias
   source) and loses the marginalized-chroma trick (winner-only shade).
3. **History-guided selection policy (not history samples)**: reproject
   the PREVIOUS frame's run confidence/luminance stats to steer cell
   choice and CDF weights. Sample-value-independent w.r.t. the current
   frame => no MIS compensation, no ghosting of values (only of the
   sampling policy); attacks disocclusion selection where the current
   frame has no information yet.
4. **Anti-correlated draws across neighboring pixels**: offset each
   pixel's antithetic CDF strata by a blue-noise mask so adjacent pixels
   reuse complementary candidates - blue-noise-distributed reuse error,
   better denoiser input at zero cost. (Marginals unchanged => unbiased.)
5. **Class-split spatial reuse**: separate DI/GI (or short/long path)
   reservoirs with per-class radii and N~ - the single mixed reservoir
   couples reuse ranges that want different kernels.

## Next steps

0. PRIORITY (updated after the diagnostic round): deeper-reconnection
   fallback in shiftstep - it is BOTH the top variance lever (shift
   success) and the residual-bias fix (false-negative support failures
   are the mechanism; see "Residual mechanism"). Then shift COST:
   reconnection-only fast path + cheaper t=1 replays to close the 2x
   gap to T+S reuse cost.
2. Release build + frame-deterministic equal-time protocol; MAPE vs long
   BDPT references (ROYALGL_EXPORT_SERIES); wider-disocclusion scenes
   (clutter + stronger dolly/orbit, possibly a loaded glTF).
3. Clean ablation matrix for mode 2 (score / antithetic / gate / kernel,
   K and cap sweeps, EMA rate + floor sweeps).
4. Junkins-style analytic compatibility term as an alternative/additive
   selection factor (needs only G-buffer reads in ssort).
5. Optional: per-target class CDFs (sub-tile or normal-cluster classes
   with per-class subtotals) for exact target-dependent P; tiny-MLP score
   head as EMA replacement.
6. Volumetric evaluation (fog-cluster runs already flow through the CDF
   machinery) — as an addition; keep orthogonal to the volume paper.
