#include "Ephemeris.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <cmath>

#ifdef ORIGIN_SPICE
extern "C" {
#include "SpiceUsr.h"
}
#endif

namespace Ephemeris {

namespace {
QMutex g_mutex;
bool g_tried = false, g_ok = false;
QHash<QString, QByteArray> g_small;            // small bodies loaded from their own SPK files: lower-case name or alias -> NAIF id
QStringList g_smallNames, g_smallFiles;         // their main names, and the SPK files already furnished
QByteArray g_earthFrame;                        // body-fixed Earth frame for observer positions: ITRF93 (high-precision PCK) or IAU_EARTH (pck00010.tpc); empty = none
QString g_desc = "low-precision series (CSPICE not linked)";

QString firstExisting(const QStringList& c) { for (const QString& f : c) if (QFileInfo::exists(f)) return QFileInfo(f).absoluteFilePath(); return QString(); }

#ifdef ORIGIN_SPICE
void load()
{
    g_tried = true;
    const QString home = QDir::homePath(), app = QCoreApplication::applicationDirPath(), env = qEnvironmentVariable("ORIGIN_SPICE_DIR");
    QStringList dirs; if (!env.isEmpty()) dirs << env;
    dirs << home + "/naif/cspice" << home + "/cspice.old" << QDir::currentPath() + "/tools" << app + "/tools" << app + "/../tools" << app + "/../../tools" << app + "/../../../tools";
    QStringList lsk, spk;
    for (const QString& d : dirs) { lsk << d + "/naif0012.tls"; spk << d + "/de440s.bsp"; }
    for (const QString& d : dirs) { spk << d + "/de440.bsp"; spk << d + "/de430_1850-2150.bsp"; }
    for (const QString& d : dirs) lsk << d + "/naif0009.tls";                       // old: lacks the leap seconds after 2009 (a few seconds of ET)
    const QString l = firstExisting(lsk), s = firstExisting(spk);
    if (l.isEmpty() || s.isEmpty()) { g_desc = QString("low-precision series (no SPICE kernels found: %1)").arg(l.isEmpty() ? "leap seconds naif0012.tls" : "planetary de440s.bsp / de440.bsp"); return; }
    erract_c("SET", 0, const_cast<SpiceChar*>("RETURN"));
    errprt_c("SET", 0, const_cast<SpiceChar*>("NONE"));
    furnsh_c(l.toUtf8().constData()); furnsh_c(s.toUtf8().constData());
    if (failed_c()) { reset_c(); g_desc = "low-precision series (SPICE kernels failed to load)"; return; }
    g_ok = true;
    // the Earth's orientation and shape, for positions seen from a place on the ground (spkcpo_c): optional
    QStringList bpc, tpc; for (const QString& d : dirs) { bpc << d + "/earth_latest_high_prec.bpc"; tpc << d + "/pck00010.tpc"; }
    const QString t = firstExisting(tpc), hp = firstExisting(bpc);
    if (!t.isEmpty()) { furnsh_c(t.toUtf8().constData()); g_earthFrame = "IAU_EARTH"; }
    if (!hp.isEmpty()) { furnsh_c(hp.toUtf8().constData()); g_earthFrame = "ITRF93"; }
    if (failed_c()) { reset_c(); g_earthFrame.clear(); }
    g_desc = QString("JPL %1 via CSPICE (%2, %3)").arg(QFileInfo(s).baseName().toUpper(), QFileInfo(s).fileName(), QFileInfo(l).fileName());
}
#endif
}   // namespace

bool available()
{
    QMutexLocker lock(&g_mutex);
#ifdef ORIGIN_SPICE
    if (!g_tried) load();
#endif
    return g_ok;
}

QString description() { available(); QMutexLocker lock(&g_mutex); return g_desc; }

bool sunMoon(const QDateTime& utc, SunMoon* out)
{
#ifdef ORIGIN_SPICE
    if (!available()) return false;
    QMutexLocker lock(&g_mutex);                                  // CSPICE is not thread safe
    SpiceDouble et = 0, sun[3], moon[3], lt = 0;
    const QString t = utc.toUTC().toString("yyyy-MM-ddTHH:mm:ss.zzz");
    str2et_c(t.toUtf8().constData(), &et);
    spkpos_c("SUN", et, "J2000", "LT+S", "EARTH", sun, &lt);
    spkpos_c("MOON", et, "J2000", "LT+S", "EARTH", moon, &lt);
    if (failed_c()) { reset_c(); return false; }
    SpiceDouble r, ra, dec;
    recrad_c(sun, &r, &ra, &dec);  out->sunDistKm = r; out->sunRaHours = ra * 12.0 / M_PI; out->sunDecDeg = dec * 180.0 / M_PI;
    recrad_c(moon, &r, &ra, &dec); out->moonDistKm = r; out->moonRaHours = ra * 12.0 / M_PI; out->moonDecDeg = dec * 180.0 / M_PI;
    // phase angle: at the Moon, between the directions to the Sun and to the Earth
    double toSun[3], toEarth[3];
    for (int i = 0; i < 3; ++i) { toSun[i] = sun[i] - moon[i]; toEarth[i] = -moon[i]; }
    const double c = vdot_c(toSun, toEarth) / (vnorm_c(toSun) * vnorm_c(toEarth));
    const double phase = std::acos(std::max(-1.0, std::min(1.0, c)));
    out->moonPhaseAngleDeg = phase * 180.0 / M_PI;
    out->moonIllumination = (1.0 + std::cos(phase)) / 2.0;
    return true;
#else
    Q_UNUSED(utc) Q_UNUSED(out)
    return false;
#endif
}

bool topocentric(const QString& name, const QDateTime& utc, double latDeg, double lonDeg, double heightM, Body* out)
{
#ifdef ORIGIN_SPICE
    static const QHash<QString, QByteArray> id = {{"sun", "SUN"}, {"moon", "MOON"}, {"mercury", "MERCURY"}, {"venus", "VENUS"}, {"mars", "MARS BARYCENTER"}, {"jupiter", "JUPITER BARYCENTER"},
                                                  {"saturn", "SATURN BARYCENTER"}, {"uranus", "URANUS BARYCENTER"}, {"neptune", "NEPTUNE BARYCENTER"}, {"pluto", "PLUTO BARYCENTER"}};
    const QString key = name.trimmed().toLower();
    if (!available()) return false;
    QMutexLocker lock(&g_mutex);
    if (g_earthFrame.isEmpty()) return false;
    const bool small = g_small.contains(key);
    if (!small && !id.contains(key)) return false;
    SpiceDouble et = 0, radii[3], obs[3], st[6], s[6], lt = 0; SpiceInt n = 0;
    str2et_c(utc.toUTC().toString("yyyy-MM-ddTHH:mm:ss.zzz").toUtf8().constData(), &et);
    bodvrd_c("EARTH", "RADII", 3, &n, radii);
    const double f = (radii[0] - radii[2]) / radii[0];
    georec_c(lonDeg * M_PI / 180.0, latDeg * M_PI / 180.0, heightM / 1000.0, radii[0], f, obs);       // geodetic -> Earth-fixed, km
    const char* corr = small ? "LT" : "LT+S";                                                          // as in body()
    spkcpo_c(small ? g_small[key].constData() : id[key].constData(), et, "J2000", "OBSERVER", corr, obs, "EARTH", g_earthFrame.constData(), st, &lt);
    spkcpo_c("SUN", et, "J2000", "OBSERVER", "LT+S", obs, "EARTH", g_earthFrame.constData(), s, &lt);
    if (failed_c()) { reset_c(); return false; }
    SpiceDouble r, ra, dec;
    recrad_c(st, &r, &ra, &dec);
    out->distKm = r; out->raHours = ra * 12.0 / M_PI; out->decDeg = dec * 180.0 / M_PI; out->xKm = st[0]; out->yKm = st[1]; out->zKm = st[2];
    out->elongationDeg = vsep_c(st, s) * 180.0 / M_PI;
    double toSun[3], toObs[3];
    for (int i = 0; i < 3; ++i) { toSun[i] = s[i] - st[i]; toObs[i] = -st[i]; }
    const double phase = vsep_c(toSun, toObs);
    out->phaseAngleDeg = phase * 180.0 / M_PI; out->illumination = (1.0 + std::cos(phase)) / 2.0;
    return true;
#else
    Q_UNUSED(name) Q_UNUSED(utc) Q_UNUSED(latDeg) Q_UNUSED(lonDeg) Q_UNUSED(heightM) Q_UNUSED(out)
    return false;
#endif
}

bool loadSmallBody(const QStringList& names, const QString& spkFile, int naifId, QString* error)
{
#ifdef ORIGIN_SPICE
    if (names.isEmpty()) { if (error) *error = "no name given"; return false; }
    if (!available()) { if (error) *error = "SPICE is not active: " + description(); return false; }
    if (!QFileInfo::exists(spkFile)) { if (error) *error = "no SPK file " + spkFile; return false; }
    QMutexLocker lock(&g_mutex);
    const QString abs = QFileInfo(spkFile).absoluteFilePath();
    if (!g_smallFiles.contains(abs)) {
        furnsh_c(abs.toUtf8().constData());
        if (failed_c()) { reset_c(); if (error) *error = "SPICE could not load " + spkFile; return false; }
        g_smallFiles << abs;
    }
    for (const QString& n : names) if (!n.trimmed().isEmpty()) g_small[n.trimmed().toLower()] = QByteArray::number(naifId);
    if (!g_smallNames.contains(names.first())) g_smallNames << names.first();
    return true;
#else
    Q_UNUSED(names) Q_UNUSED(spkFile) Q_UNUSED(naifId)
    if (error) *error = "built without CSPICE";
    return false;
#endif
}

QStringList bodyNames() { QMutexLocker lock(&g_mutex); return QStringList{"sun", "moon", "mercury", "venus", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"} + g_smallNames; }

bool body(const QString& name, const QDateTime& utc, Body* out)
{
#ifdef ORIGIN_SPICE
    static const QHash<QString, QByteArray> id = {{"sun", "SUN"}, {"moon", "MOON"}, {"mercury", "MERCURY"}, {"venus", "VENUS"}, {"mars", "MARS BARYCENTER"}, {"jupiter", "JUPITER BARYCENTER"},
                                                  {"saturn", "SATURN BARYCENTER"}, {"uranus", "URANUS BARYCENTER"}, {"neptune", "NEPTUNE BARYCENTER"}, {"pluto", "PLUTO BARYCENTER"}};
    const QString key = name.trimmed().toLower();
    if (!available()) return false;
    QMutexLocker lock(&g_mutex);
    const bool small = g_small.contains(key);
    if (!small && !id.contains(key)) return false;
    SpiceDouble et = 0, p[3], s[3], lt = 0;
    str2et_c(utc.toUTC().toString("yyyy-MM-ddTHH:mm:ss.zzz").toUtf8().constData(), &et);
    // a small body is wanted where the stars around it are in the frame: astrometric (light time only), like catalogue positions and plate solutions;
    // stellar aberration (up to 20") moves the field and the asteroid together
    spkpos_c(small ? g_small[key].constData() : id[key].constData(), et, "J2000", small ? "LT" : "LT+S", "EARTH", p, &lt);
    spkpos_c("SUN", et, "J2000", "LT+S", "EARTH", s, &lt);
    if (failed_c()) { reset_c(); return false; }
    SpiceDouble r, ra, dec;
    recrad_c(p, &r, &ra, &dec);
    out->distKm = r; out->raHours = ra * 12.0 / M_PI; out->decDeg = dec * 180.0 / M_PI; out->xKm = p[0]; out->yKm = p[1]; out->zKm = p[2];
    out->elongationDeg = vsep_c(p, s) * 180.0 / M_PI;
    double toSun[3], toEarth[3];
    for (int i = 0; i < 3; ++i) { toSun[i] = s[i] - p[i]; toEarth[i] = -p[i]; }
    const double phase = vsep_c(toSun, toEarth);
    out->phaseAngleDeg = phase * 180.0 / M_PI; out->illumination = (1.0 + std::cos(phase)) / 2.0;
    return true;
#else
    Q_UNUSED(name) Q_UNUSED(utc) Q_UNUSED(out)
    return false;
#endif
}

}   // namespace Ephemeris
