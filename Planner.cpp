#include "Planner.hpp"
#include "MessierCatalog.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Planner {
namespace {

constexpr double kPi = 3.14159265358979323846;

const QHash<QString, int>& priorities()
{
    // popularity weight 1-10: how rewarding the object is for a 152 mm f/2.2 wide-field imager. Messier objects not listed get 3.
    static const QHash<QString, int> p = {
        {"M31", 10}, {"M42", 10}, {"M45", 9}, {"M33", 8}, {"M27", 8}, {"M57", 7}, {"M13", 8}, {"M51", 8}, {"M81", 7}, {"M82", 7}, {"M101", 6}, {"M8", 8}, {"M20", 7},
        {"M17", 8}, {"M16", 7}, {"M1", 6}, {"M44", 6}, {"M3", 6}, {"M92", 5}, {"M15", 6}, {"M2", 5}, {"M52", 4}, {"M103", 4}, {"M35", 5}, {"M36", 4}, {"M37", 4}, {"M38", 4},
        {"M39", 4}, {"M34", 4}, {"M76", 4}, {"M97", 4}, {"M65", 4}, {"M66", 4}, {"M64", 4}, {"M63", 4}, {"M104", 5}, {"M87", 4}, {"M106", 5}, {"M5", 5}, {"M11", 5},
        {"M110", 4}, {"M32", 4}, {"M41", 5}, {"M46", 4}, {"M47", 4}, {"M67", 4}, {"M93", 4}, {"M50", 3}};
    return p;
}

struct Target { QString name, key, type; double ra = 0, dec = 0, size = 0; int prio = 3; };

QList<Target> loadTargets()
{
    QList<Target> out;
    for (const MessierObject& m : MessierCatalog::getAllObjects()) {
        Target t; t.key = m.name; t.name = m.name + (m.common_name.isEmpty() ? "" : " " + m.common_name);
        t.ra = m.sky_position.ra_deg / 15.0; t.dec = m.sky_position.dec_deg; t.size = m.size_arcmin.width();
        t.prio = priorities().value(m.name, 3); t.type = "MESSIER";
        out.append(t);
    }
    // curated non-Messier extras: RA hours, Dec degrees, size arcmin, priority
    struct E { const char* n; double ra, dec, size; int p; };
    static const E extras[] = {
        {"NGC 7000 North America Nebula", 20.9783, 44.33, 120, 9}, {"IC 5070 Pelican Nebula", 20.8, 44.35, 60, 7},
        {"NGC 6992 Eastern Veil", 20.9367, 31.72, 60, 9}, {"NGC 6960 Western Veil", 20.76, 30.72, 70, 8},
        {"NGC 869/884 Double Cluster", 2.35, 57.13, 60, 8}, {"IC 1805 Heart Nebula", 2.5333, 61.45, 60, 7}, {"IC 1848 Soul Nebula", 2.8833, 60.43, 60, 6},
        {"NGC 281 Pacman Nebula", 0.8767, 56.62, 35, 6}, {"IC 1396 Elephant Trunk", 21.65, 57.5, 170, 7}, {"NGC 7380 Wizard Nebula", 22.78, 58.1, 25, 5},
        {"NGC 7293 Helix Nebula", 22.4947, -20.84, 25, 6}, {"NGC 891", 2.37, 42.35, 13, 5}, {"NGC 253 Sculptor Galaxy", 0.7925, -25.29, 27, 6},
        {"NGC 1499 California Nebula", 4.0, 36.42, 145, 7}, {"NGC 2244 Rosette Nebula", 6.5333, 4.95, 80, 8}, {"NGC 7662 Blue Snowball", 23.43, 42.54, 0.5, 3},
        {"NGC 457 Owl Cluster", 1.32, 58.3, 13, 4}, {"NGC 7822 Cepheus Nebula", 0.0, 68.0, 60, 4}, {"NGC 6888 Crescent Nebula", 20.2, 38.35, 20, 6},
        {"NGC 7331 + Stephan Quintet", 22.62, 34.4, 10, 5}, {"NGC 6946 Fireworks Galaxy", 20.58, 60.15, 11, 5}, {"NGC 4565 Needle Galaxy", 12.6, 25.99, 16, 4},
        {"NGC 2264 Cone/Christmas Tree", 6.68, 9.9, 60, 6}, {"Mel 20 Alpha Persei Cluster", 3.4, 49.0, 300, 4},
        {"IC 434 Horsehead / NGC 2024 Flame", 5.6833, -2.4, 90, 8}, {"NGC 1333", 3.4, 31.4, 10, 4}, {"NGC 7789 Caroline's Rose", 23.95, 56.7, 15, 4}};
    for (const E& e : extras) { Target t; t.name = t.key = e.n; t.ra = e.ra; t.dec = e.dec; t.size = e.size; t.prio = e.p; t.type = "EXTRA"; out.append(t); }
    return out;
}

// Minimum usable altitude by azimuth: circular interpolation of the horizon file, or a flat floor, raised by learned obstruction bins.
class Horizon {
public:
    Horizon(const QString& file, double flat, const QString& skyMap) : m_flat(flat)
    {
        if (!file.isEmpty()) {
            QFile f(file);
            if (f.open(QIODevice::ReadOnly)) {
                for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object()["points"].toArray())
                    m_pts.append({v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble()});
                std::sort(m_pts.begin(), m_pts.end());
            }
        }
        QFile m(skyMap);
        if (!skyMap.isEmpty() && m.open(QIODevice::ReadOnly)) {
            const QJsonObject bins = QJsonDocument::fromJson(m.readAll()).object()["bins"].toObject();
            for (auto it = bins.begin(); it != bins.end(); ++it) {
                const QJsonObject b = it.value().toObject();
                if (b["good"].toInt() == 0 && b["bad"].toInt() >= 3 && b["bad_dates"].toArray().size() >= 2) {
                    const QStringList p = it.key().split('_');
                    if (p.size() == 2) m_bins.append({p[0].toInt(), p[1].toInt()});
                }
            }
        }
    }
    bool hasFile() const { return !m_pts.isEmpty(); }
    int learnedBins() const { return m_bins.size(); }
    double minAlt(double az) const
    {
        az = std::fmod(az, 360.0); if (az < 0) az += 360.0;
        double h = m_flat;
        if (!m_pts.isEmpty()) {
            // points repeated at -360 and +360 so that the interpolation wraps through north
            QList<QPair<double, double>> p;
            for (const auto& q : m_pts) p.append({q.first - 360, q.second});
            p += m_pts;
            for (const auto& q : m_pts) p.append({q.first + 360, q.second});
            h = p.first().second;
            for (int i = 0; i + 1 < p.size(); ++i)
                if (az >= p[i].first && az <= p[i + 1].first) { const double d = p[i + 1].first - p[i].first; h = d > 0 ? p[i].second + (p[i + 1].second - p[i].second) * (az - p[i].first) / d : p[i].second; break; }
        }
        for (const auto& b : m_bins) if (az >= b.first && az < b.first + 10) h = std::max(h, double(b.second) + 5);   // an obstructed bin blocks up to its top edge
        return h;
    }
private:
    double m_flat;
    QList<QPair<double, double>> m_pts;
    QList<QPair<int, int>> m_bins;
};

struct Row {
    Target t; QVector<bool> ok; QVector<double> alt, az; int i0 = 0, i1 = 0, peak = 0; double mins = 0, score = 0;
};

QString iso(const QDateTime& t) { return t.toUTC().toString("yyyy-MM-ddTHH:mm:ssZ"); }
QString cardinal(double z) { static const char* c[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}; return c[int(std::fmod(std::fmod(z + 22.5, 360.0) + 360.0, 360.0) / 45)]; }
QString g_textZone = "Europe/London";
QString hhmmUtc(const QString& isoZ)       // shown in the planning time zone (the name is historical)
{
    QDateTime t = QDateTime::fromString(isoZ, "yyyy-MM-ddTHH:mm:ssZ"); t.setTimeZone(QTimeZone::UTC);
    return t.toTimeZone(QTimeZone(g_textZone.toUtf8())).toString("HH:mm");
}

}   // namespace

QString defaultHorizonFile()
{
    const QString env = qEnvironmentVariable("ORIGIN_HORIZON");
    if (!env.isEmpty()) return env;
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString& c : {QString("tools/horizon.json"), app + "/tools/horizon.json", app + "/../tools/horizon.json", app + "/../../tools/horizon.json", app + "/../../../tools/horizon.json"})
        if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();
    return QString();
}

QDate currentNightDate(const QString& timeZone)
{
    return QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone(timeZone.toUtf8())).addSecs(-12 * 3600).date();
}

QJsonObject schedule(const Options& o)
{
    const QTimeZone zone(o.timeZone.toUtf8());
    if (!zone.isValid()) return {{"ok", false}, {"error", "unknown timezone " + o.timeZone}};
    const QDate day = o.date.isValid() ? o.date : currentNightDate(o.timeZone);
    const int n = int(14 * 60 / o.stepMin);
    const QDateTime t0 = QDateTime(day, QTime(17, 0), zone);
    QVector<QDateTime> times(n); QVector<double> sun(n), moonAlt(n), illum(n); QVector<bool> dark(n);
    QVector<Astro::Equatorial> moonEq(n);
    for (int i = 0; i < n; ++i) {
        times[i] = t0.addSecs(qint64(i * o.stepMin * 60)).toUTC();
        sun[i] = Astro::altAz(Astro::sunPosition(times[i]), o.site, times[i]).alt;
        moonAlt[i] = Astro::moonAltitude(o.site, times[i]); moonEq[i] = Astro::moonPosition(times[i]);
        illum[i] = Astro::moonIllumination(times[i]);
        dark[i] = sun[i] < o.sunAlt;
    }
    QList<int> d; for (int i = 0; i < n; ++i) if (dark[i]) d.append(i);
    if (d.isEmpty()) return {{"ok", false}, {"error", QString("The Sun never reaches %1 deg tonight; try sun_alt -12 (nautical)").arg(o.sunAlt)}};

    const Horizon hz(!o.horizonFile.isEmpty() && QFileInfo::exists(o.horizonFile) ? o.horizonFile : QString(), o.minAlt, o.skyMapFile);
    const QString src = hz.hasFile() ? "horizon file " + o.horizonFile : QString("flat %1 deg PLACEHOLDER (no horizon file)").arg(o.minAlt);

    QList<Row> rows;
    for (const Target& tg : loadTargets()) {
        Row r; r.t = tg; r.ok.resize(n); r.alt.resize(n); r.az.resize(n);
        bool any = false;
        for (int i = 0; i < n; ++i) {
            const Astro::AltAz aa = Astro::altAz({tg.ra, tg.dec}, o.site, times[i]);
            r.alt[i] = aa.alt; r.az[i] = aa.az;
            const double sep = Astro::separationDeg({tg.ra, tg.dec}, moonEq[i]);
            r.ok[i] = dark[i] && aa.alt > hz.minAlt(aa.az) && (sep > o.moonSep || moonAlt[i] < -2);
            any |= r.ok[i];
        }
        if (!any) continue;
        int bestN = 0, bestS = 0, bestE = 0, cur = 0, s = 0;                   // longest contiguous run
        for (int i = 0; i < n; ++i) {
            if (r.ok[i]) { if (cur == 0) s = i; ++cur; if (cur > bestN) { bestN = cur; bestS = s; bestE = i; } } else cur = 0;
        }
        if (bestN * o.stepMin < o.minBlockMin) continue;
        r.i0 = bestS; r.i1 = bestE; r.mins = bestN * o.stepMin;
        r.peak = bestS; for (int i = bestS; i <= bestE; ++i) if (r.alt[i] > r.alt[r.peak]) r.peak = i;
        r.score = tg.prio * std::min(1.0, r.mins / 180) * (0.6 + 0.4 * std::min(1.0, r.alt[r.peak] / 60));
        rows.append(r);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });

    // greedy plan: each target used once; best-scoring one that stays up >= min block, favouring ones about to set; move on after max block
    struct Slot { int s, e; int row; };
    QList<Slot> plan; QSet<QString> used;
    const int need = int(o.minBlockMin / o.stepMin), cap = std::max(1, int(o.maxBlockMin / o.stepMin));
    for (int i = d.first(); i <= d.last();) {
        double bestV = -1; int bestRow = -1, bestRun = 0;
        for (int k = 0; k < rows.size(); ++k) {
            const Row& r = rows[k];
            if (used.contains(r.t.key) || !r.ok[i]) continue;
            int run = 0; while (i + run < n && r.ok[i + run]) ++run;
            if (run < need) continue;
            const int m = std::min(run, cap); double mean = 0; for (int j = 0; j < m; ++j) mean += r.alt[i + j]; mean /= m;
            const double v = r.t.prio * (0.5 + 0.5 * std::min(1.0, mean / 60)) + (run <= 1.5 * cap ? 2 : 0);
            if (bestRow < 0 || v > bestV) { bestV = v; bestRow = k; bestRun = run; }
        }
        if (bestRow < 0) { ++i; continue; }
        const int len = std::min(bestRun, cap);
        plan.append({i, i + len - 1, bestRow}); used.insert(rows[bestRow].t.key); i += len;
    }

    auto info = [&](const Row& r) {
        return QJsonObject{{"name", r.t.name}, {"key", r.t.key}, {"ra_hours", r.t.ra}, {"dec_deg", r.t.dec}, {"priority", r.t.prio}, {"size_arcmin", r.t.size},
                           {"window_start", iso(times[r.i0])}, {"window_end", iso(times[r.i1])}, {"peak", iso(times[r.peak])}, {"peak_alt", std::round(r.alt[r.peak] * 10) / 10},
                           {"peak_az", std::round(r.az[r.peak])}, {"minutes", r.mins}};
    };
    QJsonArray planJ, targetsJ;
    for (const Slot& s : plan) {
        QJsonObject e = info(rows[s.row]); e["start"] = iso(times[s.s]); e["end"] = iso(times[s.e]);
        e["alt_start"] = std::round(rows[s.row].alt[s.s]); e["alt_end"] = std::round(rows[s.row].alt[s.e]);
        e["az_start"] = std::round(rows[s.row].az[s.s]); e["az_end"] = std::round(rows[s.row].az[s.e]); e["minutes"] = (s.e - s.s + 1) * o.stepMin;
        planJ.append(e);
    }
    for (const Row& r : rows) targetsJ.append(info(r));
    return QJsonObject{{"ok", true}, {"night", day.toString(Qt::ISODate)}, {"site", QJsonObject{{"lat_deg", o.site.latDeg}, {"lon_deg", o.site.lonDeg}}},
                       {"dark_start", iso(times[d.first()])}, {"dark_end", iso(times[d.last()])}, {"moon_illumination", std::round(illum[d.first()] * 100) / 100},
                       {"horizon", src + (hz.learnedBins() ? QString(" + %1 learned bins").arg(hz.learnedBins()) : QString())}, {"plan", planJ}, {"targets", targetsJ}};
}

QString scheduleText(const QJsonObject& s, int top)
{
    if (!s["ok"].toBool()) return s["error"].toString();
    QString out = QString("Night of %1; dark %2 to %3 local; moon %4% lit; %5\n").arg(s["night"].toString(), hhmmUtc(s["dark_start"].toString()), hhmmUtc(s["dark_end"].toString()))
                      .arg(int(s["moon_illumination"].toDouble() * 100)).arg(s["horizon"].toString());
    out += QString("\n%1 %2 %3 %4 %5 %6 %7\n").arg("target", -38).arg("pri", 3).arg("window (local)", 13).arg("mins", 5).arg("peak", 6).arg("alt", 4).arg("az", 9);
    int k = 0;
    for (const QJsonValue& v : s["targets"].toArray()) {
        if (k++ >= top) break;
        const QJsonObject r = v.toObject();
        out += QString("%1 %2 %3-%4 %5 %6 %7 %8 %9\n").arg(r["name"].toString().left(38), -38).arg(r["priority"].toInt(), 3).arg(hhmmUtc(r["window_start"].toString()), hhmmUtc(r["window_end"].toString()))
                   .arg(r["minutes"].toDouble(), 5, 'f', 0).arg(hhmmUtc(r["peak"].toString()), 6).arg(r["peak_alt"].toDouble(), 4, 'f', 0).arg(r["peak_az"].toDouble(), 4, 'f', 0).arg(cardinal(r["peak_az"].toDouble()), 3);
    }
    out += "\nPlan:\n";
    for (const QJsonValue& v : s["plan"].toArray()) {
        const QJsonObject r = v.toObject();
        out += QString("  %1-%2  %3 alt %4->%5, az %6->%7  (%8 min)\n").arg(hhmmUtc(r["start"].toString()), hhmmUtc(r["end"].toString())).arg(r["name"].toString().left(36), -36)
                   .arg(r["alt_start"].toInt()).arg(r["alt_end"].toInt()).arg(r["az_start"].toInt()).arg(r["az_end"].toInt()).arg(r["minutes"].toInt());
    }
    if (s["plan"].toArray().isEmpty()) out += "  nothing observable for a useful block tonight\n";
    return out;
}

QJsonObject findTarget(const Options& base, const QString& name)
{
    Options o = base; o.minBlockMin = 0;
    const QJsonObject s = schedule(o);
    if (!s["ok"].toBool()) return s;
    const QString n = name.toLower();
    const QJsonArray ts = s["targets"].toArray();
    for (const QJsonValue& v : ts) { QJsonObject t = v.toObject(); if (n == t["key"].toString().toLower() || n == t["name"].toString().toLower()) { QJsonObject r = t; r["ok"] = true; return r; } }
    for (const QJsonValue& v : ts) { QJsonObject t = v.toObject(); if (t["key"].toString().toLower().startsWith(n + " ") || t["name"].toString().toLower().contains(n)) { t["ok"] = true; return t; } }
    return {{"ok", false}, {"error", QString("%1 is not observable tonight (or unknown); see the 'now' command").arg(name)}};
}

RotLimit maxExposure(double raHours, double decDeg, const Astro::Site& site, const QDateTime& utc, double tolPx)
{
    constexpr double omega = 7.2921159e-5, rCorner = 1840.0;       // sidereal rate rad/s; half-diagonal of the 3056x2048 frame in px
    const Astro::AltAz aa = Astro::altAz({raHours, decDeg}, site, utc);
    const double alt = aa.alt * kPi / 180, az = aa.az * kPi / 180;
    const double w = omega * std::cos(site.latDeg * kPi / 180) * std::cos(az) / std::cos(alt);
    const double tmax = std::fabs(w) < 1e-12 ? std::numeric_limits<double>::infinity() : tolPx / (rCorner * std::fabs(w));
    return {tmax, aa.alt, aa.az, w * 180 / kPi * 3600};
}

QJsonObject fieldRotation(double raHours, double decDeg, const Astro::Site& site, double tolPx, double hours)
{
    QJsonArray rows; const QDateTime now = QDateTime::currentDateTimeUtc();
    auto num = [](double v) { return std::isfinite(v) ? QJsonValue(std::round(v * 10) / 10) : QJsonValue(); };
    for (int m = 0; m <= int(hours * 60); m += 30) {
        const QDateTime t = now.addSecs(m * 60);
        const RotLimit a = maxExposure(raHours, decDeg, site, t, tolPx), b = maxExposure(raHours, decDeg, site, t, 1.0);
        rows.append(QJsonObject{{"utc", iso(t)}, {"alt", std::round(a.alt)}, {"az", std::fmod(std::round(a.az), 360.0)}, {"rotation_deg_per_hour", num(a.rotDegPerHour)}, {"t_max_s", num(a.tMax)}, {"t_max_1px_s", num(b.tMax)}, {"low", a.alt <= 25}});
    }
    return {{"ok", true}, {"ra_hours", raHours}, {"dec_deg", decDeg}, {"tolerance_px", tolPx}, {"note", "1 px = 1.48 arcsec; t_max is for a star at the frame corner; null = unlimited (due east/west)"}, {"rows", rows}};
}

}   // namespace Planner
