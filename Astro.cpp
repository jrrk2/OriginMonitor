#include "Astro.hpp"
#include "Ephemeris.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
inline double rad(double d) { return d * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }
inline double wrap360(double d) { d = std::fmod(d, 360.0); return d < 0 ? d + 360.0 : d; }
}

namespace Astro {

double julianDate(const QDateTime& utc)
{
    return utc.toMSecsSinceEpoch() / 86400000.0 + 2440587.5;
}

double lstDegrees(double jd, double lonDeg)
{
    const double d = jd - 2451545.0, T = d / 36525.0;
    const double gmst = 280.46061837 + 360.98564736629 * d + 0.000387933 * T * T;
    return wrap360(gmst + lonDeg);
}

Equatorial precess(const Equatorial& eq, double jdFrom, double jdTo)
{
    const double T = (jdFrom - 2451545.0) / 36525.0, t = (jdTo - jdFrom) / 36525.0;
    const double as = kPi / 648000.0;                                         // arcsecond in radians
    const double zeta = ((2306.2181 + 1.39656 * T - 0.000139 * T * T) * t + (0.30188 - 0.000344 * T) * t * t + 0.017998 * t * t * t) * as;
    const double z = ((2306.2181 + 1.39656 * T - 0.000139 * T * T) * t + (1.09468 + 0.000066 * T) * t * t + 0.018203 * t * t * t) * as;
    const double theta = ((2004.3109 - 0.85330 * T - 0.000217 * T * T) * t - (0.42665 + 0.000217 * T) * t * t - 0.041833 * t * t * t) * as;
    const double a0 = rad(eq.raHours * 15.0), d0 = rad(eq.decDeg);
    const double A = std::cos(d0) * std::sin(a0 + zeta);
    const double B = std::cos(theta) * std::cos(d0) * std::cos(a0 + zeta) - std::sin(theta) * std::sin(d0);
    const double C = std::sin(theta) * std::cos(d0) * std::cos(a0 + zeta) + std::cos(theta) * std::sin(d0);
    return {wrap360(deg(std::atan2(A, B) + z)) / 15.0, deg(std::asin(std::clamp(C, -1.0, 1.0)))};
}

AltAz altAz(const Equatorial& eq, const Site& site, const QDateTime& utc)
{
    const double jd = julianDate(utc);
    const Equatorial d = precess(eq, 2451545.0, jd);
    const double ha = rad(lstDegrees(jd, site.lonDeg) - d.raHours * 15.0);
    const double dec = rad(d.decDeg), lat = rad(site.latDeg);
    const double sinAlt = std::sin(dec) * std::sin(lat) + std::cos(dec) * std::cos(lat) * std::cos(ha);
    const double alt = std::asin(std::clamp(sinAlt, -1.0, 1.0));
    const double y = -std::cos(dec) * std::sin(ha);
    const double x = std::sin(dec) * std::cos(lat) - std::cos(dec) * std::sin(lat) * std::cos(ha);
    return {deg(alt), wrap360(deg(std::atan2(y, x)))};
}

namespace {
Equatorial sunOfDate(const QDateTime& utc)
{
    const double n = julianDate(utc) - 2451545.0;
    const double L = wrap360(280.460 + 0.9856474 * n), g = rad(wrap360(357.528 + 0.9856003 * n));
    const double lam = rad(L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g));
    const double eps = rad(23.439 - 0.0000004 * n);
    const double ra = std::atan2(std::cos(eps) * std::sin(lam), std::cos(lam));
    return {wrap360(deg(ra)) / 15.0, deg(std::asin(std::sin(eps) * std::sin(lam)))};
}
}

Equatorial sunPosition(const QDateTime& utc)
{
    Ephemeris::SunMoon sm;
    if (Ephemeris::sunMoon(utc, &sm)) return {sm.sunRaHours, sm.sunDecDeg};
    return precess(sunOfDate(utc), julianDate(utc), 2451545.0);       // the series is of date
}

namespace {
struct MoonGeo { Astro::Equatorial eq; double parallaxDeg; };
MoonGeo moonGeo(const QDateTime& utc)
{
    const double T = (julianDate(utc) - 2451545.0) / 36525.0;
    auto s = [&](double a, double b) { return std::sin(rad(a + b * T)); };
    auto c = [&](double a, double b) { return std::cos(rad(a + b * T)); };
    const double lam = 218.32 + 481267.881 * T + 6.29 * s(135.0, 477198.87) - 1.27 * s(259.3, -413335.36) + 0.66 * s(235.7, 890534.22)
                     + 0.21 * s(269.9, 954397.74) - 0.19 * s(357.5, 35999.05) - 0.11 * s(186.5, 966404.03);
    const double bet = 5.13 * s(93.3, 483202.02) + 0.28 * s(228.2, 960400.89) - 0.28 * s(318.3, 6003.15) - 0.17 * s(217.6, -407332.21);
    const double par = 0.9508 + 0.0518 * c(135.0, 477198.87) + 0.0095 * c(259.3, -413335.36) + 0.0078 * c(235.7, 890534.22) + 0.0028 * c(269.9, 954397.74);
    const double l = std::cos(rad(bet)) * std::cos(rad(lam));
    const double m = 0.9175 * std::cos(rad(bet)) * std::sin(rad(lam)) - 0.3978 * std::sin(rad(bet));
    const double n = 0.3978 * std::cos(rad(bet)) * std::sin(rad(lam)) + 0.9175 * std::sin(rad(bet));
    return {{wrap360(deg(std::atan2(m, l))) / 15.0, deg(std::asin(n))}, par};
}
}

Equatorial moonPosition(const QDateTime& utc)
{
    Ephemeris::SunMoon sm;
    if (Ephemeris::sunMoon(utc, &sm)) return {sm.moonRaHours, sm.moonDecDeg};
    return precess(moonGeo(utc).eq, julianDate(utc), 2451545.0);      // the series is of date
}

double moonDistanceKm(const QDateTime& utc)
{
    Ephemeris::SunMoon sm;
    if (Ephemeris::sunMoon(utc, &sm)) return sm.moonDistKm;
    return 6378.14 / std::sin(rad(moonGeo(utc).parallaxDeg));
}

double moonAltitude(const Site& site, const QDateTime& utc)
{
    const double alt = altAz(moonPosition(utc), site, utc).alt;
    const double par = deg(std::asin(6378.14 / moonDistanceKm(utc)));         // horizontal parallax
    return alt - par * std::cos(rad(alt));
}

double separationDeg(const Equatorial& a, const Equatorial& b)
{
    const double d1 = rad(a.decDeg), d2 = rad(b.decDeg), dra = rad((a.raHours - b.raHours) * 15.0);
    const double c = std::sin(d1) * std::sin(d2) + std::cos(d1) * std::cos(d2) * std::cos(dra);
    return deg(std::acos(std::clamp(c, -1.0, 1.0)));
}

double moonIllumination(const QDateTime& utc)
{
    Ephemeris::SunMoon sm;
    if (Ephemeris::sunMoon(utc, &sm)) return sm.moonIllumination;
    const double elong = rad(separationDeg(sunPosition(utc), moonPosition(utc)));
    return (1.0 - std::cos(elong)) / 2.0;      // adequate: the moon's latitude is under 5 degrees
}

double moonSeparationDeg(const Equatorial& target, const QDateTime& utc)
{
    return separationDeg(target, moonPosition(utc));
}

}   // namespace Astro
