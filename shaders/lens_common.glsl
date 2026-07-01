// Shared lens-surface geometry primitives, used by both the primary (eye)
// ray tracer (shaders/pathtrace.comp, walks the LensSurfacesSSBO array
// back-to-front: sensor -> front element) and the flare/ghost light tracer
// (shaders/lens_flare.comp, walks it front-to-back: front element ->
// sensor). Requires "common.glsl" to already be included (for the
// LensSurface struct / lensSurfaces SSBO).
//
// All lens-surface math happens in local lens space, millimeters, with the
// sensor at local z=0 and the optical axis along local +z (front element at
// the largest z). Converting to/from world space (meters) happens exactly
// once, only on the final ray, in the caller - see docs/ARCHITECTURE.md for
// why intermediate lens math must stay entirely in millimeters.

// The 3 representative wavelengths (nm) used everywhere dispersion is
// approximated - mirrors RoyalGL::LensWavelengths::kNm (src/optics/Glass.h).
// IOR itself is precomputed CPU-side (LensSurface::iorRGB_z), so this is
// only needed where wavelength appears directly in a formula (aperture
// diffraction's L_i*lambda term - see shaders/lens_flare.comp).
const vec3 kLensWavelengthsNm = vec3(611.0, 550.0, 465.0);

// Intersects a ray with one spherical (radius != 0) or planar (radius == 0,
// e.g. the aperture stop) lens surface centered on the local optical axis
// at `zVertex`, clipped to `semiDiameter`. Returns false if there is no
// valid intersection within the surface's clear aperture.
bool IntersectLensSurface(vec3 O, vec3 D, float zVertex, float radius, float semiDiameter,
                           out vec3 hitPos, out vec3 normal)
{
    if (abs(radius) < 1e-6)
    {
        // Flat surface (aperture stop plane, or a planar element).
        if (abs(D.z) < 1e-9) return false;
        float t = (zVertex - O.z) / D.z;
        if (t <= 1e-5) return false;
        hitPos = O + t * D;
        normal = vec3(0.0, 0.0, -1.0);
    }
    else
    {
        vec3 C = vec3(0.0, 0.0, zVertex + radius);
        vec3 oc = O - C;
        float b = dot(oc, D);
        float c = dot(oc, oc) - radius * radius;
        float disc = b * b - c;
        if (disc < 0.0) return false;
        float sq = sqrt(disc);
        float t0 = -b - sq, t1 = -b + sq;
        // Picks the physically-correct root (the actual lens-element cap,
        // not the mathematical continuation of the full sphere on its far
        // side) for either curvature sign and either traversal direction
        // uniformly - the XOR of "ray heads toward +z" and "center is
        // behind this surface's vertex" (pbrt's RealisticCamera uses the
        // same rule). Hand-verified against two concrete cases (ray origin
        // inside vs. outside the sphere) after this being backwards (== not
        // != ) caused every ray to hit the wrong hemisphere and get
        // rejected - see docs/ARCHITECTURE.md.
        bool useCloserT = (D.z > 0.0) != (radius < 0.0);
        float t = useCloserT ? min(t0, t1) : max(t0, t1);
        if (t <= 1e-5) return false;
        hitPos = O + t * D;
        normal = normalize(hitPos - C);
        if (dot(normal, D) > 0.0) normal = -normal;
    }
    return dot(hitPos.xy, hitPos.xy) <= semiDiameter * semiDiameter;
}

// Exact vector-form Snell's law (Steinert et al. 2011, Eq. 2) - no paraxial
// approximation, so ray height is not limited and spherical/coma/
// astigmatism/chromatic aberration all emerge naturally from geometry.
// `eta` = etaFrom / etaTo. Returns false on total internal reflection
// (caller discards the sample - no NaN propagation).
bool RefractSnell(vec3 D, vec3 N, float eta, out vec3 refracted)
{
    float cosI = -dot(N, D);
    float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T > 1.0)
    {
        refracted = vec3(0.0);
        return false;
    }
    float cosT = sqrt(1.0 - sin2T);
    refracted = eta * D + (eta * cosI - cosT) * N;
    return true;
}

// Regular N-sided polygon aperture test (apothem = apertureRadius, i.e. the
// polygon's flats are tangent to a circle of that radius, matching how
// physical iris blades are specified). bladeCount < 3 falls back to a plain
// circular aperture.
bool PointInsideApertureBlades(vec2 p, float apertureRadius, float bladeCount, float rotation)
{
    if (bladeCount < 3.0) return dot(p, p) <= apertureRadius * apertureRadius;
    float angle = atan(p.y, p.x) - rotation;
    float seg = (2.0 * PI) / bladeCount;
    float theta = mod(angle, seg) - 0.5 * seg;
    return length(p) <= apertureRadius / cos(theta);
}

// Unpolarized dielectric Fresnel reflectance (Hecht's Optics / pbrt's
// FrDielectric form). cosThetaI must be >= 0 (caller ensures this by
// flipping the normal as needed). Used only by the flare/ghost pass -
// the primary ray tracer always transmits (see TraceThroughLens).
float FresnelDielectric(float cosThetaI, float n1, float n2)
{
    float sinThetaI = sqrt(max(0.0, 1.0 - cosThetaI * cosThetaI));
    float sinThetaT = (n1 / n2) * sinThetaI;
    if (sinThetaT >= 1.0) return 1.0; // total internal reflection

    float cosThetaT = sqrt(max(0.0, 1.0 - sinThetaT * sinThetaT));
    float Rs = (n1 * cosThetaI - n2 * cosThetaT) / (n1 * cosThetaI + n2 * cosThetaT);
    float Rp = (n2 * cosThetaI - n1 * cosThetaT) / (n2 * cosThetaI + n1 * cosThetaT);
    return 0.5 * (Rs * Rs + Rp * Rp);
}

// ------------------------------------------------ World <-> lens space ----
// uFrame.camRight/camUp/camForward form an orthonormal basis (Camera's
// Forward/Right/Up are all normalized, mutually orthogonal), so its inverse
// rotation is just the transpose (per-axis dot products).

// Local lens mm-space -> world space. The mm->m conversion happens exactly
// once, here, on a final ray/point only - never on intermediate lens-
// surface math - to keep sphere-intersection subtractions well-conditioned
// (see docs/ARCHITECTURE.md).
vec3 WorldFromLensMM(vec3 localMM)
{
    vec3 localM = localMM * 0.001;
    return uFrame.camPos.xyz
         + uFrame.camRight.xyz   * localM.x
         + uFrame.camUp.xyz      * localM.y
         + uFrame.camForward.xyz * localM.z;
}

vec3 WorldDirFromLens(vec3 localDir)
{
    return normalize(uFrame.camRight.xyz * localDir.x
                    + uFrame.camUp.xyz    * localDir.y
                    + uFrame.camForward.xyz * localDir.z);
}

// World space -> local lens mm-space (inverse of WorldFromLensMM), used by
// the flare/ghost pass to bring a world-space light-tracing ray into the
// lens's local coordinate frame before walking its surfaces.
vec3 LensMMFromWorld(vec3 worldPos)
{
    vec3 rel = worldPos - uFrame.camPos.xyz;
    vec3 localM = vec3(dot(rel, uFrame.camRight.xyz), dot(rel, uFrame.camUp.xyz), dot(rel, uFrame.camForward.xyz));
    return localM * 1000.0;
}

vec3 LensDirFromWorld(vec3 worldDir)
{
    return normalize(vec3(dot(worldDir, uFrame.camRight.xyz), dot(worldDir, uFrame.camUp.xyz), dot(worldDir, uFrame.camForward.xyz)));
}

// Concentric disk sampling, used to pick a point on the rearmost element's
// clear aperture (the naive, correctness-first aperture-sampling strategy -
// see docs/ARCHITECTURE.md for the "pixel pupil" optimization this
// deliberately omits).
vec2 SampleApertureDiskMM(float diskRadiusMM, vec2 u)
{
    vec2 uOffset = 2.0 * u - 1.0;
    if (uOffset.x == 0.0 && uOffset.y == 0.0) return vec2(0.0);
    float r, theta;
    if (abs(uOffset.x) > abs(uOffset.y)) { r = uOffset.x; theta = 0.78539816 * (uOffset.y / uOffset.x); }
    else { r = uOffset.y; theta = 1.57079633 - 0.78539816 * (uOffset.x / uOffset.y); }
    return r * diskRadiusMM * vec2(cos(theta), sin(theta));
}
