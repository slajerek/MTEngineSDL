#include "MTH_Geodesy.h"
#include <cmath>

// Disable FP contraction (FMA) for this TU. The coincident early-out relies on the
// EXACT `sinSigma == 0.0` comparison (#3.4): for bit-identical points, the term
// cosU1*sinU2 - sinU1*cosU2 must cancel to exactly 0. FMA fuses that into a
// higher-precision multiply-add that does NOT cancel, so the branch would miss and
// the distance come out ~1e-10 instead of 0 -- and, worse, platform-dependently
// (ARM64 has FMA, some x86 targets do not). Turning contraction off makes the
// exact-zero coincident result deterministic across platforms. It does not affect
// the near-antipodal non-convergence path (sin(pi) is ~1.2e-16, not 0, either way).
#pragma STDC FP_CONTRACT OFF

double MTH_NormalizeDegrees(double deg)
{
    double r = std::fmod(deg, 360.0);
    if (r < 0.0) r += 360.0;
    return r;   // [0, 360)
}

double MTH_BearingDelta(double aDeg, double bDeg)   // precondition: both normalized (#5.2)
{
    return std::fmod(bDeg - aDeg + 540.0, 360.0) - 180.0;   // [-180, +180)
}

bool MTH_GeodesicInverse(double lat1Deg, double lon1Deg,
                         double lat2Deg, double lon2Deg, MTHGeodesicResult *out)
{
    using namespace MTHWgs84;
    const double toRad = 3.14159265358979323846 / 180.0;
    const double f = kF, a = kA, b = kB;
    const double U1 = std::atan((1.0 - f) * std::tan(lat1Deg * toRad));
    const double U2 = std::atan((1.0 - f) * std::tan(lat2Deg * toRad));
    const double sinU1 = std::sin(U1), cosU1 = std::cos(U1);
    const double sinU2 = std::sin(U2), cosU2 = std::cos(U2);
    const double L = (lon2Deg - lon1Deg) * toRad;

    double lambda = L, lambdaPrev = 0.0;
    double sinSigma = 0, cosSigma = 0, sigma = 0, cos2Alpha = 0, cos2SigmaM = 0, cosLambda = 0, sinLambda = 0;
    bool converged = false;
    for (int i = 0; i < 200; ++i)
    {
        sinLambda = std::sin(lambda); cosLambda = std::cos(lambda);
        const double t1 = cosU2 * sinLambda;
        const double t2 = cosU1 * sinU2 - sinU1 * cosU2 * cosLambda;
        sinSigma = std::sqrt(t1 * t1 + t2 * t2);
        if (sinSigma == 0.0)   // coincident points (exact -- #3.4); bearing undefined
        {
            if (out) { out->meters = 0.0; out->initialBearing = 0.0; out->coincident = true; }
            return true;
        }
        cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cosLambda;
        sigma    = std::atan2(sinSigma, cosSigma);
        const double sinAlpha = cosU1 * cosU2 * sinLambda / sinSigma;
        cos2Alpha  = 1.0 - sinAlpha * sinAlpha;
        cos2SigmaM = (cos2Alpha == 0.0) ? 0.0 : cosSigma - 2.0 * sinU1 * sinU2 / cos2Alpha;  // 0 on the equator
        const double C = f / 16.0 * cos2Alpha * (4.0 + f * (4.0 - 3.0 * cos2Alpha));
        lambdaPrev = lambda;
        lambda = L + (1.0 - C) * f * sinAlpha *
                 (sigma + C * sinSigma * (cos2SigmaM + C * cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM)));
        if (std::fabs(lambda - lambdaPrev) < 1e-12) { converged = true; break; }
    }
    if (!converged) return false;   // near-antipodal (#3.3): *out untouched, never NaN

    const double u2 = cos2Alpha * (a * a - b * b) / (b * b);
    const double A = 1.0 + u2 / 16384.0 * (4096.0 + u2 * (-768.0 + u2 * (320.0 - 175.0 * u2)));
    const double B = u2 / 1024.0 * (256.0 + u2 * (-128.0 + u2 * (74.0 - 47.0 * u2)));
    const double deltaSigma = B * sinSigma * (cos2SigmaM + B / 4.0 *
        (cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM)
         - B / 6.0 * cos2SigmaM * (-3.0 + 4.0 * sinSigma * sinSigma) * (-3.0 + 4.0 * cos2SigmaM * cos2SigmaM)));
    const double s = b * A * (sigma - deltaSigma);
    const double alpha1 = std::atan2(cosU2 * sinLambda, cosU1 * sinU2 - sinU1 * cosU2 * cosLambda);
    if (out)
    {
        out->meters         = s;
        out->initialBearing = MTH_NormalizeDegrees(alpha1 / toRad);
        out->coincident     = (s < kCoincidentMeters);
    }
    return true;
}
