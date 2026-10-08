#include "Astro.hpp"

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

AltAz altAz(const Equatorial& eq, const Site& site, const QDateTime& utc)
{
    const double ha = rad(lstDegrees(julianDate(utc), site.lonDeg) - eq.raHours * 15.0);
    const double dec = rad(eq.decDeg), lat = rad(site.latDeg);
    const double sinAlt = std::sin(dec) * std::sin(lat) + std::cos(dec) * std::cos(lat) * std::cos(ha);
    const double alt = std::asin(std::clamp(sinAlt, -1.0, 1.0));
    const double y = -std::cos(dec) * std::sin(ha);
    const double x = std::sin(dec) * std::cos(lat) - std::cos(dec) * std::sin(lat) * std::cos(ha);
    return {deg(alt), wrap360(deg(std::atan2(y, x)))};
}

Equatorial sunPosition(const QDateTime& utc)
{
    const double n = julianDate(utc) - 2451545.0;
    const double L = wrap360(280.460 + 0.9856474 * n), g = rad(wrap360(357.528 + 0.9856003 * n));
    const double lam = rad(L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g));
    const double eps = rad(23.439 - 0.0000004 * n);
    const double ra = std::atan2(std::cos(eps) * std::sin(lam), std::cos(lam));
    return {wrap360(deg(ra)) / 15.0, deg(std::asin(std::sin(eps) * std::sin(lam)))};
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

Equatorial moonPosition(const QDateTime& utc) { return moonGeo(utc).eq; }

double moonAltitude(const Site& site, const QDateTime& utc)
{
    const MoonGeo g = moonGeo(utc);
    const double alt = altAz(g.eq, site, utc).alt;
    return alt - g.parallaxDeg * std::cos(rad(alt));
}

double separationDeg(const Equatorial& a, const Equatorial& b)
{
    const double d1 = rad(a.decDeg), d2 = rad(b.decDeg), dra = rad((a.raHours - b.raHours) * 15.0);
    const double c = std::sin(d1) * std::sin(d2) + std::cos(d1) * std::cos(d2) * std::cos(dra);
    return deg(std::acos(std::clamp(c, -1.0, 1.0)));
}

double moonIllumination(const QDateTime& utc)
{
    const double elong = rad(separationDeg(sunPosition(utc), moonPosition(utc)));
    return (1.0 - std::cos(elong)) / 2.0;      // adequate: the moon's latitude is under 5 degrees
}

double moonSeparationDeg(const Equatorial& target, const QDateTime& utc)
{
    return separationDeg(target, moonPosition(utc));
}

}   // namespace Astro
