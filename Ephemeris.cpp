#include "Ephemeris.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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

}   // namespace Ephemeris
