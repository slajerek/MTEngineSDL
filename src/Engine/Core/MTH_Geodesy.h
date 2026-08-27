#pragma once
// Self-contained WGS-84 geodesics. No third-party library (store policy), no
// dependencies beyond <cmath>. C++17. Design: 2026-07-20-fu-b-gps-conditions #3.

namespace MTHWgs84 {
    inline constexpr double kA = 6378137.0;              // semi-major axis, m
    inline constexpr double kF = 1.0 / 298.257223563;    // flattening
    inline constexpr double kB = kA * (1.0 - kF);        // 6356752.314245 m
    inline constexpr double kCoincidentMeters = 1.0;     // #5.3
}

struct MTHGeodesicResult
{
    double meters         = 0.0;   // geodesic length on the spheroid
    double initialBearing = 0.0;   // forward azimuth at point 1, [0, 360)
    bool   coincident     = false; // within kCoincidentMeters (#5.3)
};

// Vincenty inverse. Returns false ONLY on non-convergence (near-antipodal); *out
// is then untouched. Never returns NaN. Callers treat false as distance UNKNOWN.
bool MTH_GeodesicInverse(double lat1Deg, double lon1Deg,
                         double lat2Deg, double lon2Deg,
                         MTHGeodesicResult *out);

double MTH_NormalizeDegrees(double deg);            // -> [0, 360)
double MTH_BearingDelta(double aDeg, double bDeg);  // signed b-a in [-180, +180); args must be normalized
