// Unit checks for HostAlign: the model recovers a known tilted mount, the sky conversions round-trip, the focus fit finds a known vertex.
#include "../HostAlign.hpp"
#include <cmath>
#include <cstdio>
#include <random>
using namespace HostAlign;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: "); } else std::printf("ok:   "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

int main()
{
    // a mount 6 deg tilted toward az 40, altitude zero 41.2, flexure 0.6, cone 0.25, azimuth zero 75: truth model built by hand
    const double tilt = 6 * M_PI / 180, toward = 40 * M_PI / 180, azz = 75 * M_PI / 180;
    // rotation = Rz(azz) then tilt about horizontal axis perpendicular to 'toward'
    auto qmul = [](const double a[4], const double b[4], double o[4]) { o[0] = a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3]; o[1] = a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2]; o[2] = a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1]; o[3] = a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0]; };
    const double qz[4] = {std::cos(azz/2), 0, 0, std::sin(azz/2)};
    const double ax[3] = {-std::sin(toward), std::cos(toward), 0};      // axis such that up tips toward 'toward'
    const double qt[4] = {std::cos(tilt/2), ax[0]*std::sin(tilt/2), ax[1]*std::sin(tilt/2), 0};
    double q[4]; qmul(qt, qz, q);
    Model truth = Model::fromJson(QJsonObject{{"w", q[0]}, {"x", q[1]}, {"y", q[2]}, {"z", q[3]}, {"altitude_zero", 41.2}, {"flexure", 0.6}, {"cone", 0.25}});
    double td, tt; truth.baseTilt(&td, &tt);
    CHECK(std::fabs(td - 6) < 1e-6, "truth tilt %.3f deg toward %.1f", td, tt);

    std::mt19937 rng(7); std::normal_distribution<double> noise(0, 0.5 / 3600);   // 0.5" solve noise per axis
    QList<Point> pts;
    for (const Astro::AltAz& s : goldenSpiral(12, 25, 80)) {
        double e0, e1; truth.toEncoders(s.alt, s.az, &e0, &e1);
        const Astro::AltAz back = truth.toSky(e0, e1);
        if (separationDeg(back.alt, back.az, s.alt, s.az) > 1e-7) { ++fails; std::printf("FAIL: toEncoders/toSky round trip\n"); }
        pts.append({e0, e1, s.alt + noise(rng), s.az + noise(rng), QDateTime::currentDateTimeUtc()});
    }
    Model m = Model::fit(pts);
    double md, mt; m.baseTilt(&md, &mt);
    CHECK(m.rmsArcmin < 0.05, "12-point fit rms %.4f arcmin", m.rmsArcmin);
    CHECK(std::fabs(m.altitudeZero - 41.2) < 0.02 && std::fabs(m.flexure - 0.6) < 0.02 && std::fabs(m.cone - 0.25) < 0.02,
          "K %.4f F %.4f C %.4f (truth 41.2, 0.6, 0.25)", m.altitudeZero, m.flexure, m.cone);
    CHECK(std::fabs(md - 6) < 0.02 && std::fabs(mt - 40) < 0.5, "tilt %.3f toward %.2f (truth 6 toward 40)", md, mt);
    double worst = 0;
    for (const Astro::AltAz& s : goldenSpiral(50, 20, 85, 13, 13)) {
        double e0, e1, f0, f1; truth.toEncoders(s.alt, s.az, &e0, &e1); m.toEncoders(s.alt, s.az, &f0, &f1);
        const Astro::AltAz land = truth.toSky(f0, f1); worst = std::max(worst, separationDeg(land.alt, land.az, s.alt, s.az) * 60);
    }
    CHECK(worst < 0.2, "goto error over 50 other places: worst %.3f arcmin", worst);

    // three points: quick start, no flexure or cone
    Model m3 = Model::fit(pts.mid(0, 3));
    CHECK(m3.points == 3 && m3.flexure == 0 && m3.cone == 0, "3-point fit rms %.3f arcmin K %.3f", m3.rmsArcmin, m3.altitudeZero);

    // calibration: refine rejects a 5 degree outlier once fitted, replaces a point at the same place
    Calibration c; for (const Point& p : pts.mid(0, 6)) c.refine(p);
    Point bad = pts[7]; bad.alt += 5;
    CHECK(c.refine(bad) == -2 && c.pts.size() == 6, "outlier refused");
    Point again = pts[2]; again.alt += 0.001; c.refine(again);
    CHECK(c.pts.size() == 6, "same place replaces (n=%d)", int(c.pts.size()));
    const Calibration c2 = Calibration::fromJson(c.toJson());
    CHECK(c2.pts.size() == 6 && std::fabs(c2.model.altitudeZero - c.model.altitudeZero) < 1e-9, "json round trip");

    // sky: J2000 -> horizon -> J2000 with and without refraction
    const Astro::Site site; const QDateTime t = QDateTime(QDate(2026, 11, 12), QTime(22, 0), Qt::UTC);
    for (bool refr : {false, true}) {
        const Astro::AltAz h = toHorizon(150.0, 55.0, t, site, refr);
        double ra, dec; toJ2000(h.alt, h.az, t, site, refr, &ra, &dec);
        const double err = std::hypot((ra - 150.0) * std::cos(55 * M_PI / 180), dec - 55.0) * 3600;
        CHECK(err < (refr ? 2.0 : 0.01), "round trip refract=%d: %.3f arcsec (alt %.3f az %.3f)", refr, err, h.alt, h.az);
    }
    CHECK(std::fabs(refractionTrueToApparent(45) * 60 - 0.98) < 0.05, "refraction at 45 deg %.3f arcmin", refractionTrueToApparent(45) * 60);

    // focus: hyperbola with vertex 18900, min HFR 1.6 px, 1% noise
    std::normal_distribution<double> hn(0, 0.01);
    QList<double> pos, hfr;
    for (int x = 18500; x <= 19300; x += 100) { pos.append(x); hfr.append(std::sqrt(1.6 * 1.6 + std::pow((x - 18900) * 0.012, 2)) * (1 + hn(rng))); }
    FocusFit f = fitHyperbola(pos, hfr);
    CHECK(f.ok && std::fabs(f.best - 18900) < 15 && std::fabs(f.minHfr - 1.6) < 0.1, "focus vertex %.1f min %.3f r2 %.4f", f.best, f.minHfr, f.r2);
    QList<double> p2 = pos.mid(0, 4), h2 = hfr.mid(0, 4);      // one-sided sweep below the vertex
    FocusFit g = fitHyperbola(p2, h2);
    CHECK(!g.ok && g.edge == 1, "one-sided sweep says extend upward (%s)", qPrintable(g.why));

    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
