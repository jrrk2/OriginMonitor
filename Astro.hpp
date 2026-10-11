#pragma once

#include <QDateTime>

/**
 * Positional astronomy for observing decisions (target altitude, sun and moon). Precession is applied; the Sun and Moon come from JPL DE440 through
 * CSPICE (see Ephemeris.hpp) when available, accurate to arcseconds, else from a low-precision series (a few arcminutes for the sun, about 0.3 degrees
 * for the moon). Target and body positions are J2000 throughout.
 * All angles in degrees unless the name says otherwise; RA in hours.
 */
namespace Astro {

struct Site { double latDeg = 52.205; double lonDeg = 0.122; };   // east-positive; default Cambridge, UK (the mount's stored GPS site is wrong)
struct AltAz { double alt = 0, az = 0; };                         // azimuth from north through east
struct Equatorial { double raHours = 0, decDeg = 0; };

double julianDate(const QDateTime& utc);
double lstDegrees(double jd, double lonDeg);                      // local mean sidereal time
AltAz altAz(const Equatorial& eq, const Site& site, const QDateTime& utc);            // eq is J2000; precessed to the date (nutation, aberration, refraction ignored: tens of arcseconds)
Equatorial sunPosition(const QDateTime& utc);                     // J2000; JPL DE440 through CSPICE when Ephemeris is active, else a low-precision series
Equatorial moonPosition(const QDateTime& utc);                    // J2000, geocentric
double moonDistanceKm(const QDateTime& utc);
Equatorial precess(const Equatorial& eq, double jdFrom, double jdTo);   // mean equator and equinox of jdFrom -> of jdTo (IAU 1976)
double moonAltitude(const Site& site, const QDateTime& utc);       // topocentric (parallax applied)
double moonIllumination(const QDateTime& utc);                    // 0..1
double separationDeg(const Equatorial& a, const Equatorial& b);
double moonSeparationDeg(const Equatorial& target, const QDateTime& utc);

}   // namespace Astro
