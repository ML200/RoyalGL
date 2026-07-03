#pragma once

#include <glm/glm.hpp>
#include "optics/LensSystem.h"

namespace RoyalGL
{
    enum class CameraMode : int
    {
        Pinhole = 0,
        Lens = 1 // physical lens per Steinert et al. 2011
    };

    // User-tunable render settings, surfaced in the UI. Changing any of
    // these (compared via operator==) resets progressive accumulation.
    struct RenderSettings
    {
        int maxBounces = 6;
        float exposure = 1.0f;
        glm::vec3 backgroundColor{0.015f, 0.015f, 0.02f}; // much darker than the old sky-blue default, so small bright light sources actually pop
        float backgroundIntensity = 1.0f;
        int maxSamples = 0; // 0 = unlimited

        // Next-event estimation via the light tree, MIS-combined with BSDF
        // sampling (balance heuristic). Only affects the unidirectional
        // pipeline (bidir has its own s=1 strategy). Off = pure BSDF
        // sampling; both converge to the same image, which makes this a
        // useful A/B check.
        bool enableNEE = true;

        // Bidirectional path tracing (three compute passes, recursive MIS
        // weights - see docs/ARCHITECTURE.md). Converges to the same image
        // as the unidirectional pipeline; caustics through delta glass are
        // only reachable efficiently here (t=1 light tracing).
        bool enableBidir = true;

        // ReSTIR BDPT (docs/RESTIR_BDPT_PLAN.md): per-frame spatiotemporal
        // reservoir reuse over bidirectional path candidates. Forces the
        // bidirectional pipeline and full-frame (non-tiled) dispatch.
        // Pinhole cameras only - lens mode falls back to plain BDPT (the
        // stochastic pupil makes the primary hit a random variable, which
        // is Area-ReSTIR territory).
        bool enableRestir = true;
        // 0=off, then G-buffer normals / depth / motion vectors, reservoir
        // W / confidence / technique index (see shaders/restir_debug.comp).
        int restirDebugView = 0;
        // Reuse passes; disable both for pure per-pixel RIS (the Phase 1.1
        // unbiasedness baseline).
        bool restirTemporal = true;
        bool restirSpatial = true;
        int restirSpatialNeighbors = 1;
        float restirSpatialRadius = 30.0f; // pixels
        // Confidence (M) cap for all reuse passes (paper uses 20): bounds
        // the effective temporal sample count a reservoir can claim.
        // Lower = fresher (outliers and stale history wash out in fewer
        // frames), higher = smoother but longer-lived correlation. Default
        // 8: t=1 reuse between spatially distant anchors is heavy-tailed
        // at grazing incidence, and shorter chains recover faster.
        float restirConfidenceCap = 8.0f;
        // Phase 2 "lightweight BDPT": t=1 light tracing candidates binned
        // into per-pixel reservoirs through the LRM, with caustic paths in
        // a second reservoir. Off = Phase 1 camera-side techniques only.
        bool restirLightTracing = true;
        // Phase 4 full BDPT: s>=2 vertex connections against the compacted
        // global LVC. Off = lightweight mode; the MIS weights track the
        // active technique set either way.
        bool restirConnections = true;
        // Phase 5: recompute omega_tau at the destination anchor for every
        // shift (camera-side, t=1 reverse, caustic replay); off = copy
        // omega through shifts (paper Sec. 6.4). Default ON: with light
        // tracing enabled, every technique's omega carries the t=1
        // competitor seed N_L*d^2/cosIn1, which varies violently with the
        // anchor at grazing incidence - a stale copied omega under camera
        // motion breaks the temporal balance-heuristic partition by orders
        // of magnitude and W blows up on newly-visible sharp-angle
        // surfaces for ~confidence-cap frames. (The historical reason to
        // keep this off - the free-landing t=1 re-anchoring approximation
        // that recompute amplified - is gone since t=1 candidates are
        // anchored at creation.) See RESTIR_BDPT_PLAN.md.
        bool restirRecomputeMis = true;

        // Global: off = every pipeline overwrites the image with its latest
        // sample instead of averaging, so naive PT / NEE / BDPT / ReSTIR
        // per-frame quality can be compared live.
        bool accumulate = true;

        // Physical lens camera (Steinert et al. 2011). Lens mode renders
        // through the unidirectional pipeline regardless of enableBidir -
        // exact BDPT MIS through a lens is future work.
        CameraMode cameraMode = CameraMode::Pinhole;
        LensSettings lens;

        bool operator==(const RenderSettings& other) const
        {
            return maxBounces == other.maxBounces &&
                   exposure == other.exposure &&
                   backgroundColor == other.backgroundColor &&
                   backgroundIntensity == other.backgroundIntensity &&
                   maxSamples == other.maxSamples &&
                   enableNEE == other.enableNEE &&
                   enableBidir == other.enableBidir &&
                   enableRestir == other.enableRestir &&
                   restirDebugView == other.restirDebugView &&
                   restirTemporal == other.restirTemporal &&
                   restirSpatial == other.restirSpatial &&
                   restirSpatialNeighbors == other.restirSpatialNeighbors &&
                   restirSpatialRadius == other.restirSpatialRadius &&
                   restirConfidenceCap == other.restirConfidenceCap &&
                   restirLightTracing == other.restirLightTracing &&
                   restirConnections == other.restirConnections &&
                   restirRecomputeMis == other.restirRecomputeMis &&
                   accumulate == other.accumulate &&
                   cameraMode == other.cameraMode &&
                   lens == other.lens;
        }
        bool operator!=(const RenderSettings& other) const { return !(*this == other); }
    };
}
