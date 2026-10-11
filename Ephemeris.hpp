#pragma once

#include <QDateTime>
#include <QString>

/**
 * High-accuracy Sun and Moon from JPL DE440 through NAIF's CSPICE (the same toolkit CometObserver uses), when the library was linked in
 * (OriginHeadless.pro defines ORIGIN_SPICE if it finds CSPICE) and the kernels can be found. Everything else (and every build without CSPICE) falls
 * back to the low-precision series in Astro.cpp, so nothing depends on this module being active; description() says which one is in use.
 *
 * Kernels: a leap-second kernel (naif0012.tls, shipped in tools/) and a planetary SPK (de440s.bsp or de440.bsp). Searched in $ORIGIN_SPICE_DIR, then
 * ~/naif/cspice, ~/cspice.old. Positions are apparent geocentric (light time and stellar aberration), J2000 equatorial.
 */
namespace Ephemeris {

bool available();                 // loads the kernels on first use; true when SPICE is active
QString description();            // e.g. "JPL DE440 via CSPICE (de440.bsp, naif0012.tls)" or "low-precision series (...)"

struct SunMoon {
    double sunRaHours = 0, sunDecDeg = 0, sunDistKm = 0;
    double moonRaHours = 0, moonDecDeg = 0, moonDistKm = 0;
    double moonIllumination = 0;  // illuminated fraction of the disc, 0..1
    double moonPhaseAngleDeg = 0; // Sun-Moon-Earth angle; 0 = full, 180 = new
};
bool sunMoon(const QDateTime& utc, SunMoon* out);      // false when SPICE is not active

}   // namespace Ephemeris
