// Lens camera per Steinert, Dammertz, Hanika & Lensch, "General Spectral
// Camera Lens Simulation" (CGF 2011). Requires common.glsl.
//
// All lens math runs in the paper's tracing frame: millimeters, sensor
// plane at z=0, +z toward the scene (the mm -> m conversion to world space
// happens exactly once, on the final exit ray - the paper's sec. 4.1 scale
// separation). Surfaces are stored in walk order (rear element first);
// exact per-wavelength Snell refraction at every spherical cap reproduces
// all five Seidel aberrations plus chromatic aberration, and the optional
// stochastic Fresnel reflection branch produces the sec. 4.3 ghosts.

struct LensSurface
{
    vec4 geo;      // x=vertex z, y=signed radius (positive = center on image side), z=semi-diameter, w=isAperture
    vec4 mediumA;  // image-side medium (toward sensor), w=mode (1=Sellmeier, 0=Cauchy)
    vec4 mediumA2;
    vec4 mediumB;  // object-side medium
    vec4 mediumB2;
    vec4 aperture; // x=blade count, y=blade rotation, z=stop radius
};
layout(std430, binding = 13) readonly buffer LensSurfacesSSBO { LensSurface lensSurfaces[]; };

// Per-pixel pupil discs (paper sec. 4.2), computed by lens_pupil.comp:
// xy=center on the pupil plane (mm), z=radius (mm, 0 = fully vignetted),
// w unused.
layout(std430, binding = 14) buffer PixelPupilSSBO { vec4 pixelPupils[]; };

// Sellmeier / two-term Cauchy refractive index at lambda (nm).
float LensEta(vec4 coef, vec4 coef2, float lambdaNm)
{
    float um = lambdaNm * 1e-3;
    float l2 = um * um;
    if (coef.w > 0.5)
    {
        float n2 = 1.0 + coef.x * l2 / (l2 - coef2.x)
                       + coef.y * l2 / (l2 - coef2.y)
                       + coef.z * l2 / (l2 - coef2.z);
        return sqrt(max(n2, 1.0));
    }
    return coef2.w + coef.x / l2;
}

// Intersects a ray with the spherical cap (or plane, radius==0) of surface
// `s`, returning the hit position and the surface normal oriented against
// the ray. False when the element is missed or the hit lies outside its
// clear semi-diameter.
bool IntersectLensSurface(LensSurface s, vec3 O, vec3 D, out vec3 hitPos, out vec3 normal)
{
    float zVertex = s.geo.x;
    float radius = s.geo.y;
    float semiDiam = s.geo.z;

    if (abs(radius) < 1e-6)
    {
        // Flat surface (aperture stop plane).
        if (abs(D.z) < 1e-9) return false;
        float t = (zVertex - O.z) / D.z;
        if (t <= 1e-4) return false;
        hitPos = O + D * t;
        normal = vec3(0.0, 0.0, -sign(D.z));
    }
    else
    {
        // Sphere center: the paper's positive radius has its center of
        // curvature on the image side, which is -z in this frame.
        vec3 c = vec3(0.0, 0.0, zVertex - radius);
        vec3 oc = O - c;
        float b = dot(oc, D);
        float disc = b * b - (dot(oc, oc) - radius * radius);
        if (disc < 0.0) return false;
        float sq = sqrt(disc);

        // Of the two roots, take the one on the correct hemisphere: the
        // optical surface is the cap around the vertex, i.e. the sphere
        // point whose z-offset from the center has the sign of the radius.
        float t0 = -b - sq;
        float t1 = -b + sq;
        float t = 0.0;
        bool found = false;
        for (int pass = 0; pass < 2; ++pass)
        {
            t = (pass == 0) ? t0 : t1;
            if (t <= 1e-4) continue;
            vec3 p = O + D * t;
            if ((p.z - c.z) * sign(radius) > 0.0) { found = true; break; }
        }
        if (!found) return false;

        hitPos = O + D * t;
        normal = normalize(hitPos - c);
        if (dot(normal, D) > 0.0) normal = -normal;
    }

    return dot(hitPos.xy, hitPos.xy) <= semiDiam * semiDiam;
}

// Regular N-gon (or circular, blades==0) aperture containment.
bool InsideAperture(vec2 p, float radius, float blades, float rotation)
{
    float r2 = dot(p, p);
    if (r2 > radius * radius) return false;
    if (blades < 2.5) return true;
    float n = blades;
    // Distance to each blade edge of the regular polygon inscribed in the
    // circle of `radius`: inside iff within the apothem along every edge
    // normal.
    float apothem = radius * cos(PI / n);
    float ang = atan(p.y, p.x) - rotation;
    float sector = 2.0 * PI / n;
    float local = mod(ang + 0.5 * sector, sector) - 0.5 * sector;
    return sqrt(r2) * cos(local) <= apothem;
}

struct LensTraceResult
{
    vec3 origin;   // exit position (lens frame, mm)
    vec3 dir;      // exit direction
    bool valid;
    bool flared;   // at least one internal reflection happened
};

// Walks a ray through the element stack with exact per-wavelength Snell
// refraction, blade test at the stop, and optionally a stochastic Fresnel
// reflect/refract branch (the F / (1-F) choice probability cancels against
// the BSDF, so throughput is unchanged either way - with flare off,
// transmission keeps the (1-F) energy factor and ghosts are simply
// absent). `startK/startStep` select the entry side: (0, +1) enters from
// the sensor side and exits into the scene; (n-1, -1) enters at the front
// element and exits toward the sensor (`exitToSensor`), where the result
// is the ray past the rear element - the caller intersects z=0 itself.
LensTraceResult WalkLens(vec3 O, vec3 D, float lambdaNm, bool flareOn, int startK, int startStep,
                         bool exitToSensor, out float transmission)
{
    LensTraceResult res;
    res.valid = false;
    res.flared = false;
    transmission = 1.0;

    int n = int(lensSurfaces.length());
    int k = startK;
    int step = startStep;
    int reflections = 0;

    for (int iter = 0; iter < 64; ++iter)
    {
        if (k >= n)
        {
            if (exitToSensor) return res; // escaped out the front - discard
            res.origin = O;
            res.dir = D;
            res.valid = true;
            return res;
        }
        if (k < 0)
        {
            if (!exitToSensor) return res; // bounced back to the sensor side - discard
            res.origin = O;
            res.dir = D;
            res.valid = true;
            return res;
        }

        LensSurface s = lensSurfaces[k];

        vec3 hitPos, normal;
        if (!IntersectLensSurface(s, O, D, hitPos, normal)) return res;

        if (s.geo.w > 0.5)
        {
            // Aperture stop: flat, air both sides, blades decide.
            if (!InsideAperture(hitPos.xy, s.aperture.z, s.aperture.x, s.aperture.y)) return res;
            O = hitPos;
            k += step;
            continue;
        }

        // Media on either side of this surface, oriented by travel
        // direction: moving frontward we leave the image-side medium.
        bool frontward = (step > 0);
        float etaFrom = frontward ? LensEta(s.mediumA, s.mediumA2, lambdaNm)
                                  : LensEta(s.mediumB, s.mediumB2, lambdaNm);
        float etaTo = frontward ? LensEta(s.mediumB, s.mediumB2, lambdaNm)
                                : LensEta(s.mediumA, s.mediumA2, lambdaNm);

        float cosI = -dot(normal, D);
        float F = FresnelDielectric(cosI, etaFrom, etaTo);

        bool reflect;
        if (flareOn)
        {
            reflect = (RandomFloat() < F); // F/(1-F) cancels the choice pdf
        }
        else
        {
            reflect = (F >= 1.0); // TIR still reflects...
            if (reflect) return res; // ...which without flare ends the path
            transmission *= 1.0 - F;
        }

        if (reflect)
        {
            if (++reflections > 2) return res; // paper: 2-bounce ghosts dominate
            res.flared = true;
            D = normalize(D + 2.0 * cosI * normal);
            O = hitPos;
            step = -step;
            k += step;
            continue;
        }

        float etaRel = etaFrom / etaTo;
        vec3 refr = refract(D, normal, etaRel);
        if (refr == vec3(0.0)) return res; // TIR under stochastic F<1 rounding
        O = hitPos;
        D = normalize(refr);
        k += step;
    }

    return res;
}

LensTraceResult TraceLensFromSensor(vec3 O, vec3 D, float lambdaNm, bool flareOn, out float transmission)
{
    return WalkLens(O, D, lambdaNm, flareOn, 0, 1, false, transmission);
}

// Scene-side entry (the t=1 light-tracing connection): O just outside the
// front element, D pointing rearward. On success returns the ray past the
// rear element; `sensorPosMm` is its z=0 plane crossing.
bool TraceLensToSensor(vec3 O, vec3 D, float lambdaNm, bool flareOn,
                       out vec2 sensorPosMm, out float transmission, out bool flared)
{
    int n = int(lensSurfaces.length());
    LensTraceResult res = WalkLens(O, D, lambdaNm, flareOn, n - 1, -1, true, transmission);
    sensorPosMm = vec2(0.0);
    flared = res.flared;
    if (!res.valid || res.dir.z >= -1e-6) return false;
    float t = -res.origin.z / res.dir.z;
    sensorPosMm = res.origin.xy + res.dir.xy * t;
    return true;
}

// ---------------------------------------------------------- spectral -----
// Continuous spectral sampling (paper sec. 2.2): one wavelength per path,
// uniform over [380, 720] nm; the contribution converts back to linear RGB
// through Gaussian fits of the CIE 1931 color matching functions (Wyman et
// al. 2013), normalized so a spectrally flat radiance stays (1,1,1).

const float LENS_LAMBDA_MIN = 380.0;
const float LENS_LAMBDA_MAX = 720.0;

float CieGauss(float x, float alpha, float mu, float s1, float s2)
{
    float s = (x < mu) ? s1 : s2;
    float t = (x - mu) / s;
    return alpha * exp(-0.5 * t * t);
}

vec3 CieXyz(float l)
{
    float x = CieGauss(l, 1.056, 599.8, 37.9, 31.0)
            + CieGauss(l, 0.362, 442.0, 16.0, 26.7)
            + CieGauss(l, -0.065, 501.1, 20.4, 26.2);
    float y = CieGauss(l, 0.821, 568.8, 46.9, 40.5)
            + CieGauss(l, 0.286, 530.9, 16.3, 31.1);
    float z = CieGauss(l, 1.217, 437.0, 11.8, 36.0)
            + CieGauss(l, 0.681, 459.0, 26.0, 13.8);
    return vec3(x, y, z);
}

// Wavelength importance sampling: a truncated Gaussian roughly matched to
// the combined color-matching functions. Uniform sampling makes the
// CIE->RGB weight swing between roughly -3 and +10 across the spectrum,
// which multiplies every light sample and shows up as colored fireflies;
// with this pdf the weight magnitude stays within a factor of ~3.
const float LENS_LAMBDA_MU = 555.0;
const float LENS_LAMBDA_SIGMA = 90.0;
const float LENS_LAMBDA_NORM = 0.9407; // integral of the Gaussian over [380,720]

float SampleLambda(out float pdf)
{
    float lambda = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        vec2 u = RandomFloat2();
        float g = sqrt(max(-2.0 * log(max(u.x, 1e-7)), 0.0)) * cos(2.0 * PI * u.y);
        lambda = LENS_LAMBDA_MU + LENS_LAMBDA_SIGMA * g;
        if (lambda >= LENS_LAMBDA_MIN && lambda <= LENS_LAMBDA_MAX) break;
        lambda = 0.0;
    }
    if (lambda == 0.0) lambda = LENS_LAMBDA_MU; // rejection fallback (rare)

    float t = (lambda - LENS_LAMBDA_MU) / LENS_LAMBDA_SIGMA;
    pdf = exp(-0.5 * t * t) / (LENS_LAMBDA_SIGMA * 2.50662827) / LENS_LAMBDA_NORM;
    return lambda;
}

vec3 SpectralWeightRGB(float lambdaNm, float pdf)
{
    vec3 xyz = CieXyz(lambdaNm) / (106.9 * pdf);
    return vec3( 3.2406 * xyz.x - 1.5372 * xyz.y - 0.4986 * xyz.z,
                -0.9689 * xyz.x + 1.8758 * xyz.y + 0.0415 * xyz.z,
                 0.0557 * xyz.x - 0.2040 * xyz.y + 1.0570 * xyz.z);
}

// -------------------------------------------------------- world frame ----
// The lens front vertex sits at the camera position; the optical axis runs
// along the camera forward vector; mm -> m exactly once here.
vec3 LensToWorld(vec3 pMm)
{
    vec3 rel = pMm - vec3(0.0, 0.0, uFrame.lensParams.z);
    return uFrame.camPos.xyz
         + (uFrame.camRight.xyz * rel.x + uFrame.camUp.xyz * rel.y + uFrame.camForward.xyz * rel.z) * 1e-3;
}

vec3 LensDirToWorld(vec3 dMm)
{
    return normalize(uFrame.camRight.xyz * dMm.x + uFrame.camUp.xyz * dMm.y + uFrame.camForward.xyz * dMm.z);
}

vec3 LensWorldToLocal(vec3 pWorld)
{
    vec3 rel = (pWorld - uFrame.camPos.xyz) * 1e3; // m -> mm
    return vec3(dot(rel, uFrame.camRight.xyz), dot(rel, uFrame.camUp.xyz),
                dot(rel, uFrame.camForward.xyz) + uFrame.lensParams.z);
}

vec3 LensWorldDirToLocal(vec3 dWorld)
{
    return vec3(dot(dWorld, uFrame.camRight.xyz), dot(dWorld, uFrame.camUp.xyz),
                dot(dWorld, uFrame.camForward.xyz));
}

// Sensor position (mm) for a pixel's ndc coordinates: pre-inverted relative
// to the pinhole convention because the lens forms an inverted image - the
// double inversion lands the picture right-side-up.
vec2 SensorPosMm(vec2 ndc)
{
    return vec2(-ndc.x * uFrame.lensParams.x, ndc.y * uFrame.lensParams.y);
}

// Inverse of SensorPosMm + pixel lookup, for splatting the t=1 walk's
// sensor crossing. False when the point lies off the sensor.
bool SensorPosToPixel(vec2 posMm, out ivec2 pixel)
{
    vec2 ndc = vec2(-posMm.x / uFrame.lensParams.x, posMm.y / uFrame.lensParams.y);
    if (abs(ndc.x) >= 1.0 || abs(ndc.y) >= 1.0) return false;
    vec2 size = vec2(uFrame.frameInfo.xy);
    pixel = clamp(ivec2((ndc * 0.5 + 0.5) * size), ivec2(0), ivec2(size) - 1);
    return true;
}

// Shared lens eye-ray generation (unidirectional kernel and BDPT eye pass):
// pixel pupil sample (sec. 4.2) + spectral wavelength + full lens walk.
// `sensorFactor` is the sensor-side measurement weight for the whole path:
// transmission x cos^4 falloff x pupil-area fraction x CIE wavelength
// weight (irradiance-proportional: natural + optical vignetting and bokeh
// weighting emerge; the absolute scale folds into exposure).
bool LensGenerateEyeRay(ivec2 pixel, vec2 ndc, out Ray rayWorld, out vec3 sensorFactor)
{
    vec4 pupil = pixelPupils[uint(pixel.y) * uFrame.frameInfo.x + uint(pixel.x)];
    if (pupil.z <= 0.0) return false; // fully vignetted pixel

    vec3 sensorPos = vec3(SensorPosMm(ndc), 0.0);

    vec2 r = RandomFloat2();
    float rad = pupil.z * sqrt(r.x);
    float phi = 2.0 * PI * r.y;
    vec3 pupilPoint = vec3(pupil.xy + vec2(rad * cos(phi), rad * sin(phi)), uFrame.lensParams.w);

    vec3 dir = normalize(pupilPoint - sensorPos);
    float lambdaPdf;
    float lambda = SampleLambda(lambdaPdf);

    // Eye-side walks are always transmission-only: ghost paths belong
    // exclusively to the t=1 light-tracing strategy (paper sec. 4.3),
    // which samples them with a dedicated per-connection sample count and
    // MIS weight 1 - eye-side stochastic branches were pure fireflies.
    float transmission;
    LensTraceResult lt = TraceLensFromSensor(sensorPos, dir, lambda, false, transmission);
    if (!lt.valid) return false;

    rayWorld = MakeRay(LensToWorld(lt.origin), LensDirToWorld(lt.dir));

    float cosTheta = dir.z;
    float cos2 = cosTheta * cosTheta;
    float rearSemi = uFrame.lensParams2.z;
    float geomW = (pupil.z * pupil.z) / (rearSemi * rearSemi) * cos2 * cos2;
    sensorFactor = transmission * geomW * SpectralWeightRGB(lambda, lambdaPdf);
    return true;
}
