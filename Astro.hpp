#pragma once

#include <QDateTime>

/**
 * Low-precision positional astronomy for observing decisions (target altitude, sun and moon). Accuracies are a few arcminutes for the sun
 * and about 0.3 degrees for the moon, which is ample for "is it dark, where is the moon, is the target high enough".
 * All angles in degrees unless the name says otherwise; RA in hours.
 */
namespace Astro {

struct Site { double latDeg = 52.205; double lonDeg = 0.122; };   // east-positive; default Cambridge, UK (the mount's stored GPS site is wrong)
struct AltAz { double alt = 0, az = 0; };                         // azimuth from north through east
struct Equatorial { double raHours = 0, decDeg = 0; };

double julianDate(const QDateTime& utc);
double lstDegrees(double jd, double lonDeg);                      // local mean sidereal time
AltAz altAz(const Equatorial& eq, const Site& site, const QDateTime& utc);            // J2000 treated as of date (precession ignored: < 0.4 deg until 2050)
Equatorial sunPosition(const QDateTime& utc);
Equatorial moonPosition(const QDateTime& utc);                    // geocentric
double moonAltitude(const Site& site, const QDateTime& utc);       // topocentric (parallax applied)
double moonIllumination(const QDateTime& utc);                    // 0..1
double separationDeg(const Equatorial& a, const Equatorial& b);
double moonSeparationDeg(const Equatorial& target, const QDateTime& utc);

}   // namespace Astro
