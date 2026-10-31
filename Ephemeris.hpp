#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

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

// Any Sun, Moon or planet, geocentric apparent (light time and stellar aberration), J2000 equatorial. name: sun, moon, mercury, venus, mars, jupiter, saturn,
// uranus, neptune, pluto (case-insensitive). The giant planets use their system barycentre, which is within a fraction of an arcsecond of the planet from Earth.
struct Body {
    double raHours = 0, decDeg = 0, distKm = 0;
    double xKm = 0, yKm = 0, zKm = 0;                      // geocentric J2000 vector, for a topocentric correction
    double elongationDeg = 0;                              // from the Sun
    double phaseAngleDeg = 0, illumination = 1;            // Sun-body-Earth angle; illuminated fraction of the disc
};
bool body(const QString& name, const QDateTime& utc, Body* out);
// The same seen from a place on the ground (geodetic latitude/longitude, height above the ellipsoid), with the Earth's shape and orientation from a PCK
// (earth_latest_high_prec.bpc if present, else pck00010.tpc) and light time from that place: exact topocentric, which matters for the Moon and close
// asteroids (Apophis in 2029 passes at ~6 Earth radii, parallax of several degrees). False when no PCK was found: use body() plus a correction then.
bool topocentric(const QString& name, const QDateTime& utc, double latDeg, double lonDeg, double heightM, Body* out);
// An asteroid or comet from its own SPK file (JPL Horizons makes them: tools/fetch_spk.py). Afterwards body(name, ...) gives its position, astrometric
// (light time only, no stellar aberration) so that it matches the star field around it; the planets above stay apparent.
// names: the main name first, then aliases (any of them works in body()); loading the same file again only adds names.
bool loadSmallBody(const QStringList& names, const QString& spkFile, int naifId, QString* error = nullptr);      // false for an unknown name or when SPICE is not active
QStringList bodyNames();

}   // namespace Ephemeris
