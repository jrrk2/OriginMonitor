#include "Planner.hpp"
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include "Ephemeris.hpp"
#include "MessierCatalog.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
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

struct Target { QString name, key, type, seen; double ra = 0, dec = 0, size = 0; int prio = 3; double faint = 0.5; bool boosted = false, narrowband = false; };   // faint: 0 = bright (high surface brightness: clusters, compact nebulae), 1 = faint (extended nebulae)

QMap<QString, int> g_boost;   // catalogue id -> priority added (the user's wish list, e.g. M1 +5)
QString g_stacksDir;          // where the live stacks of earlier nights are kept; empty = no novelty preference

// "M31", "NGC 6992 Eastern Veil", "NGC6992_Veil" -> the catalogue id ("M31", "NGC6992"); the Veil's two halves share one id
QString catalogId(const QString& s)
{
    static const QRegularExpression re("^\\s*(M|NGC|IC|SH2|C|B|LDN|VDB|ARP|ABELL|MEL|COLLINDER)[ _-]*(\\d+)", QRegularExpression::CaseInsensitiveOption);
    if (s.contains("veil", Qt::CaseInsensitive)) return "VEIL";
    const QRegularExpressionMatch m = re.match(s);
    return m.hasMatch() ? m.captured(1).toUpper() + m.captured(2) : QString();
}

// Novelty: the user prefers new material and targets with too little of it. Enough depends on surface brightness: 15 min for the brightest (faintness 0: clusters,
// compact planetaries) up to an hour for the faintest extended nebulae (faintness 1), linear between. A target's coverage is the exposure of all its stacks added up;
// copies are left out (names with "_final_" or "backup", or ending in a _YYYYMMDD_HHMMSS timestamp: saved before a reset or a backup, not new data).
//   under its need (or never imaged): +4   enough, last imaged over 30 days ago: +0   within 30 days: -4   within 3 days: -5   (priority never below 1)
void applyNovelty(QList<Target>& targets)
{
    static const QRegularExpression copy("_final_|backup|_\\d{8}_\\d{6}$", QRegularExpression::CaseInsensitiveOption);
    QHash<QString, QDateTime> last; QHash<QString, double> covered;
    if (g_stacksDir.isEmpty()) { for (Target& t : targets) { const QString id = catalogId(t.key.isEmpty() ? t.name : t.key); if (g_boost.contains(id)) { t.prio += g_boost[id]; t.boosted = true; } } return; }
    for (const QFileInfo& d : QDir(g_stacksDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString id = catalogId(d.fileName()); if (id.isEmpty() || copy.match(d.fileName()).hasMatch()) continue;
        // state.bin: magic, width, height, frames as 32-bit ints, then the total exposure as a double
        QFile sf(d.absoluteFilePath() + "/state.bin"); double exposureS = 0;
        if (sf.open(QIODevice::ReadOnly)) { const QByteArray h = sf.read(24); if (h.size() == 24) memcpy(&exposureS, h.constData() + 16, 8); }
        if (!(exposureS > 0)) continue;
        covered[id] += exposureS;
        if (!last.contains(id) || d.lastModified() > last[id]) last[id] = d.lastModified();
    }
    const QDateTime now = QDateTime::currentDateTime();
    for (Target& t : targets) {
        const QString id = catalogId(t.key.isEmpty() ? t.name : t.key);
        if (g_boost.contains(id)) { t.prio += g_boost[id]; t.boosted = true; }   // the user's own priority boost: it wins whenever the target is observable (and the Moon does not adjust it)
        if (id.isEmpty() || !last.contains(id)) { t.prio += 4; t.seen = "new"; continue; }
        const double need = 900.0 + 2700.0 * std::clamp(t.faint, 0.0, 1.0);
        if (covered[id] < need) { t.prio += 4; t.seen = QString("%1 of %2 min").arg(covered[id] / 60, 0, 'f', 0).arg(need / 60, 0, 'f', 0); continue; }   // under-covered counts as new
        const double days = last[id].secsTo(now) / 86400.0;
        t.seen = QString("imaged %1 days ago").arg(days, 0, 'f', days < 10 ? 1 : 0);
        t.prio = std::max(1, t.prio + (days < 3 ? -5 : days < 30 ? -4 : 0));
    }
}

// Narrowband filter (~/.config/origin/filter = "nebula": the hand-fitted Antlia quad-band passes H-alpha, H-beta, OIII and SII and blocks most moonlight and continuum).
// Emission targets keep their priority and feel the Moon half as much; everything else (clusters, reflection nebulae, galaxies with little H-alpha) loses kFilterPenalty.
const int kFilterPenalty = 5;
bool emissionTarget(const QString& id)
{
    static const QSet<QString> ids = {
        // H II regions, supernova remnants and planetaries
        "M1", "M8", "M16", "M17", "M20", "M27", "M42", "M43", "M57", "M76", "M97", "NGC7000", "IC5070", "VEIL", "IC1805", "IC1848", "NGC281", "IC1396", "NGC7380",
        "NGC7293", "NGC1499", "NGC2244", "NGC7662", "NGC7822", "NGC6888", "NGC2264", "IC434", "IC405", "NGC2174",
        // galaxies with significant H-alpha: starburst winds and big H II regions
        "M82", "M33", "M51", "M101", "M106", "M83", "NGC6946"};
    return ids.contains(id);
}
bool narrowbandFilter()
{
    QFile f(QDir::homePath() + "/.config/origin/filter");
    return f.open(QIODevice::ReadOnly) && f.readAll().trimmed().toLower() == "nebula";
}
void applyFilter(QList<Target>& targets)
{
    if (!narrowbandFilter()) return;
    for (Target& t : targets) {
        t.narrowband = true;
        if (t.boosted || emissionTarget(catalogId(t.key.isEmpty() ? t.name : t.key))) continue;
        t.prio = std::max(1, t.prio - kFilterPenalty); t.seen += t.seen.isEmpty() ? "no emission (filter)" : "; no emission (filter)";
    }
}

// Integrated visual magnitudes of the Messier objects (the catalogue's own "magnitude" field is the central star's, e.g. 14.1 for M27, so it cannot give a surface brightness)
const QHash<QString, double>& integratedMagnitudes()
{
    static const QHash<QString, double> m = {{"M1", 8.4}, {"M2", 6.5}, {"M3", 6.2}, {"M4", 5.6}, {"M5", 5.7}, {"M6", 4.2}, {"M7", 3.3}, {"M8", 6.0}, {"M9", 7.7}, {"M10", 6.6}, {"M11", 6.3}, {"M12", 6.7}, {"M13", 5.8}, {"M14", 7.6}, {"M15", 6.2}, {"M16", 6.0}, {"M17", 6.0}, {"M18", 6.9}, {"M19", 6.8}, {"M20", 6.3}, {"M21", 5.9}, {"M22", 5.1}, {"M23", 5.5}, {"M24", 2.5}, {"M25", 4.6}, {"M26", 8.0}, {"M27", 7.4}, {"M28", 6.8}, {"M29", 6.6}, {"M30", 7.2}, {"M31", 3.4}, {"M32", 8.1}, {"M33", 5.7}, {"M34", 5.5}, {"M35", 5.3}, {"M36", 6.0}, {"M37", 5.6}, {"M38", 6.4}, {"M39", 4.6}, {"M40", 8.4}, {"M41", 4.5}, {"M42", 4.0}, {"M43", 9.0}, {"M44", 3.7}, {"M45", 1.6}, {"M46", 6.1}, {"M47", 4.4}, {"M48", 5.8}, {"M49", 8.4}, {"M50", 5.9}, {"M51", 8.4}, {"M52", 6.9}, {"M53", 7.6}, {"M54", 7.6}, {"M55", 6.3}, {"M56", 8.3}, {"M57", 8.8}, {"M58", 9.7}, {"M59", 9.6}, {"M60", 8.8}, {"M61", 9.7}, {"M62", 6.5}, {"M63", 8.6}, {"M64", 8.5}, {"M65", 9.3}, {"M66", 8.9}, {"M67", 6.1}, {"M68", 7.3}, {"M69", 7.6}, {"M70", 7.9}, {"M71", 8.2}, {"M72", 9.2}, {"M73", 9.0}, {"M74", 9.4}, {"M75", 8.5}, {"M76", 10.1}, {"M77", 8.9}, {"M78", 8.3}, {"M79", 7.7}, {"M80", 7.3}, {"M81", 6.9}, {"M82", 8.4}, {"M83", 7.6}, {"M84", 9.1}, {"M85", 9.1}, {"M86", 8.9}, {"M87", 8.6}, {"M88", 9.6}, {"M89", 9.8}, {"M90", 9.5}, {"M91", 10.2}, {"M92", 6.4}, {"M93", 6.2}, {"M94", 8.2}, {"M95", 9.7}, {"M96", 9.2}, {"M97", 9.9}, {"M98", 10.1}, {"M99", 9.9}, {"M100", 9.3}, {"M101", 7.9}, {"M102", 9.9}, {"M103", 7.4}, {"M104", 8.0}, {"M105", 9.3}, {"M106", 8.4}, {"M107", 7.9}, {"M108", 10.0}, {"M109", 9.8}, {"M110", 8.5}};
    return m;
}

// Faintness 0..1 from the mean surface brightness (integrated magnitude plus 2.5 log10 of the area in square arcminutes, mag/arcmin^2): 12.5 or brighter -> 0, 15 or fainter -> 1.
// M1 11.6, M45 11.6, M57 about 11, M13 12.0, M42 12.7, M31 13.3, M33 14.1, M101 14.9. Unknown magnitude or size -> 0.5.
double faintness(double mag, double w, double h)
{
    if (!(mag > -5 && mag < 20) || !(w > 0)) return 0.5;
    const double area = 0.7854 * w * (h > 0 ? h : w);
    return std::clamp((mag + 2.5 * std::log10(std::max(area, 0.25)) - 12.5) / 2.5, 0.0, 1.0);
}

QList<Target> loadTargets()
{
    QList<Target> out;
    for (const MessierObject& m : MessierCatalog::getAllObjects()) {
        Target t; t.key = m.name; t.name = m.name + (m.common_name.isEmpty() ? "" : " " + m.common_name);
        t.ra = m.sky_position.ra_deg / 15.0; t.dec = m.sky_position.dec_deg; t.size = m.size_arcmin.width();
        t.prio = priorities().value(m.name, 3); t.type = "MESSIER";
        t.faint = faintness(integratedMagnitudes().value(m.name, 99.0), m.size_arcmin.width(), m.size_arcmin.height());
        out.append(t);
    }
    // curated non-Messier extras: RA hours, Dec degrees, size arcmin, priority
    struct E { const char* n; double ra, dec, size; int p; double f; };   // f: faintness estimate, 0 bright .. 1 faint
    static const E extras[] = {
        {"NGC 7000 North America Nebula", 20.9783, 44.33, 120, 9, 1.0}, {"IC 5070 Pelican Nebula", 20.8, 44.35, 60, 7, 1.0},
        {"NGC 6992 Eastern Veil", 20.9367, 31.72, 60, 9, 0.9}, {"NGC 6960 Western Veil", 20.76, 30.72, 70, 8, 0.9},
        {"NGC 869/884 Double Cluster", 2.35, 57.13, 60, 8, 0.0}, {"IC 1805 Heart Nebula", 2.5333, 61.45, 60, 7, 0.9}, {"IC 1848 Soul Nebula", 2.8833, 60.43, 60, 6, 0.9},
        {"NGC 281 Pacman Nebula", 0.8767, 56.62, 35, 6, 0.8}, {"IC 1396 Elephant Trunk", 21.65, 57.5, 170, 7, 0.9}, {"NGC 7380 Wizard Nebula", 22.78, 58.1, 25, 5, 0.8},
        {"NGC 7293 Helix Nebula", 22.4947, -20.84, 25, 6, 0.6}, {"NGC 891", 2.37, 42.35, 13, 5, 0.7}, {"NGC 253 Sculptor Galaxy", 0.7925, -25.29, 27, 6, 0.3},
        {"NGC 1499 California Nebula", 4.0, 36.42, 145, 7, 1.0}, {"NGC 2244 Rosette Nebula", 6.5333, 4.95, 80, 8, 0.8}, {"NGC 7662 Blue Snowball", 23.43, 42.54, 0.5, 3, 0.0},
        {"NGC 457 Owl Cluster", 1.32, 58.3, 13, 4, 0.0}, {"NGC 7822 Cepheus Nebula", 0.0, 68.0, 60, 4, 0.9}, {"NGC 6888 Crescent Nebula", 20.2, 38.35, 20, 6, 0.8},
        {"NGC 7331 + Stephan Quintet", 22.62, 34.4, 10, 5, 0.7}, {"NGC 6946 Fireworks Galaxy", 20.58, 60.15, 11, 5, 0.7}, {"NGC 4565 Needle Galaxy", 12.6, 25.99, 16, 4, 0.6},
        {"NGC 2264 Cone/Christmas Tree", 6.68, 9.9, 60, 6, 0.8}, {"Mel 20 Alpha Persei Cluster", 3.4, 49.0, 300, 4, 0.0},
        {"IC 434 Horsehead / NGC 2024 Flame", 5.6833, -2.4, 90, 8, 0.9}, {"NGC 1333", 3.4, 31.4, 10, 4, 0.6}, {"NGC 7789 Caroline's Rose", 23.95, 56.7, 15, 4, 0.0}};
    for (const E& e : extras) { Target t; t.name = t.key = e.n; t.ra = e.ra; t.dec = e.dec; t.size = e.size; t.prio = e.p; t.faint = e.f; t.type = "EXTRA"; out.append(t); }
    applyNovelty(out);
    applyFilter(out);
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
            {   // glare from neighbours' lights: bins whose clear-sky background is far above the typical bin (see SkyMonitor::obstructionMap) are avoided like obstructions
                QList<double> means; QList<QPair<QString, double>> all;
                for (auto it = bins.begin(); it != bins.end(); ++it) { const QJsonObject b = it.value().toObject(); const int n = b["ex_n"].toInt(); if (n >= 5) { const double mm = b["ex_sum"].toDouble() / n; means << mm; all.append({it.key(), mm}); } }
                if (means.size() >= 4) {
                    std::sort(means.begin(), means.end()); const double med = means[means.size() / 2];
                    for (const auto& e : all) if (e.second >= std::max(1.6 * med, med + 0.4)) { const QStringList p = e.first.split('_'); if (p.size() == 2 && p[1].toInt() < 60) m_bins.append({p[0].toInt(), p[1].toInt()}); }
                }
            }
            for (auto it = bins.begin(); it != bins.end(); ++it) {
                const QJsonObject b = it.value().toObject();
                if (b["good"].toInt() == 0 && b["bad"].toInt() >= 3 && b["bad_dates"].toArray().size() >= 2) {
                    const QStringList p = it.key().split('_');
                    if (p.size() == 2 && p[1].toInt() < 60) m_bins.append({p[0].toInt(), p[1].toInt()});     // above 60 deg it cannot be a tree line
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

// How well the Moon suits a target over steps [i, i+m): faint targets want a dark sky, bright ones can take the glow (and should use it, leaving the dark hours of a new moon to the faint ones).
// 1 = ideal (faint with no glow, or bright under strong glow), 0 = worst. The planner moves a priority by kMoonWeight * (fit - 0.5).
constexpr double kMoonWeight = 8.0;
double moonFit(double faint, const QVector<double>& glow, int i, int m)
{
    if (m <= 0) return 0.5;
    double g = 0; int c = 0; for (int j = i; j < i + m && j < glow.size(); ++j) { g += glow[j]; ++c; }
    g = c ? g / c : 0;
    return (1 - g) * faint + g * (1 - faint);
}

// the priority change for the Moon, none for a target the user boosted
double moonAdjust(const struct Target& t, const QVector<double>& glow, int i, int m)
{
    if (t.boosted) return 0.0;
    const double w = t.narrowband && emissionTarget(catalogId(t.key.isEmpty() ? t.name : t.key)) ? 0.5 : 1.0;   // the filter blocks most moonlight for emission targets
    return w * kMoonWeight * (moonFit(t.faint, glow, i, m) - 0.5);
}

struct Row {
    Target t; QVector<bool> ok; QVector<double> alt, az, glow; int i0 = 0, i1 = 0, peak = 0; double mins = 0, score = 0;
};

QString iso(const QDateTime& t) { return t.toUTC().toString("yyyy-MM-ddTHH:mm:ssZ"); }
QString cardinal(double z) { static const char* c[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}; return c[int(std::fmod(std::fmod(z + 22.5, 360.0) + 360.0, 360.0) / 45)]; }
QString g_textZone = "Europe/London";
QString hhmmUtc(const QString& isoZ)       // shown in the planning time zone (the name is historical)
{
    QDateTime t = QDateTime::fromString(isoZ, "yyyy-MM-ddTHH:mm:ssZ"); t.setTimeZone(QTimeZone::UTC);
    return t.toTimeZone(QTimeZone(g_textZone.toUtf8())).toString("HH:mm");
}

double azAltSepDeg(double alt1, double az1, double alt2, double az2)   // angle between two directions given as alt/az, degrees
{
    const double r = M_PI / 180, v = std::sin(alt1 * r) * std::sin(alt2 * r) + std::cos(alt1 * r) * std::cos(alt2 * r) * std::cos((az1 - az2) * r);
    return std::acos(std::max(-1.0, std::min(1.0, v))) / r;
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

QString catalogueId(const QString& s) { return catalogId(s); }
void setStacksDir(const QString& dir) { g_stacksDir = dir; }
void setBoosts(const QMap<QString, int>& boosts) { g_boost.clear(); for (auto it = boosts.begin(); it != boosts.end(); ++it) g_boost[catalogId(it.key()).isEmpty() ? it.key().toUpper() : catalogId(it.key())] = it.value(); }

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

    QVector<double> wq(n, 1.0);                                                 // forecast quality of each step
    if (!o.weather.isEmpty()) for (int i = 0; i < n; ++i) { const Weather::Hour* h = Weather::at(o.weather, times[i]); wq[i] = h ? h->quality : 1.0; }

    QList<Row> rows;
    for (const Target& tg : loadTargets()) {
        if (!o.skip.isEmpty() && o.skip.contains(catalogId(tg.key.isEmpty() ? tg.name : tg.key))) continue;     // the user's "not tonight"
        Row r; r.t = tg; r.ok.resize(n); r.alt.resize(n); r.az.resize(n); r.glow.resize(n);
        bool any = false;
        for (int i = 0; i < n; ++i) {
            const Astro::AltAz aa = Astro::altAz({tg.ra, tg.dec}, o.site, times[i]);
            r.alt[i] = aa.alt; r.az[i] = aa.az;
            const double sep = Astro::separationDeg({tg.ra, tg.dec}, moonEq[i]);
            // sky brightening by the Moon on this target: illumination, how high the Moon is, and how close it is to the target (0 none .. 1 strong)
            r.glow[i] = std::clamp(illum[i] * std::max(0.0, std::sin(moonAlt[i] * kPi / 180)) * (1.5 - std::min(sep, 90.0) / 90.0), 0.0, 1.0);
            r.ok[i] = dark[i] && aa.alt > hz.minAlt(aa.az) && (sep > o.moonSep || moonAlt[i] < -2) && wq[i] >= o.minQuality;
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
        r.score = (tg.prio + moonAdjust(tg, r.glow, bestS, bestE - bestS + 1)) * std::min(1.0, r.mins / 180) * (0.6 + 0.4 * std::min(1.0, r.alt[r.peak] / 60));
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
            const double v = (r.t.prio + moonAdjust(r.t, r.glow, i, m)) * (0.5 + 0.5 * std::min(1.0, mean / 60)) + (run <= 1.5 * cap ? 2 : 0);
            if (bestRow < 0 || v > bestV) { bestV = v; bestRow = k; bestRun = run; }
        }
        if (bestRow < 0) { ++i; continue; }
        const int len = std::min(bestRun, cap);
        plan.append({i, i + len - 1, bestRow}); used.insert(rows[bestRow].t.key); i += len;
    }

    auto info = [&](const Row& r) {
        return QJsonObject{{"name", r.t.name}, {"key", r.t.key}, {"ra_hours", r.t.ra}, {"dec_deg", r.t.dec}, {"priority", r.t.prio}, {"novelty", r.t.seen}, {"size_arcmin", r.t.size}, {"faintness", std::round(r.t.faint * 100) / 100}, {"moon_fit", std::round(moonFit(r.t.faint, r.glow, r.i0, r.i1 - r.i0 + 1) * 100) / 100},
                           {"window_start", iso(times[r.i0])}, {"window_end", iso(times[r.i1])}, {"peak", iso(times[r.peak])}, {"peak_alt", std::round(r.alt[r.peak] * 10) / 10},
                           {"peak_az", std::round(r.az[r.peak])}, {"minutes", r.mins}};
    };
    QJsonArray planJ, targetsJ;
    // backups for each block: other targets observable through most of it, in a different part of the sky (partial cloud over the primary should not cover them)
    auto backupsFor = [&](const Slot& s) {
        struct Cand { double v; int row; double sep, alt, az; int cov; };
        const int len = s.e - s.s + 1, mid = (s.s + s.e) / 2;
        QList<Cand> cs;
        for (int k = 0; k < rows.size(); ++k) {
            if (k == s.row) continue;
            int cov = 0; for (int i = s.s; i <= s.e; ++i) cov += rows[k].ok[i] ? 1 : 0;
            if (cov < 0.6 * len) continue;
            const double sep = azAltSepDeg(rows[s.row].alt[mid], rows[s.row].az[mid], rows[k].alt[mid], rows[k].az[mid]);
            if (sep < 25 || !rows[k].ok[mid]) continue;
            cs.append({rows[k].t.prio * (0.5 + 0.5 * std::min(1.0, rows[k].alt[mid] / 60)) * std::min(1.0, sep / 50) * (double(cov) / len), k, sep, rows[k].alt[mid], rows[k].az[mid], cov});
        }
        std::stable_sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) { return a.v > b.v; });
        QJsonArray out;
        for (int i = 0; i < cs.size() && i < 3; ++i) {
            const Cand& k = cs[i]; const Row& r = rows[k.row];
            const RotLimit lim = maxExposure(r.t.ra, r.t.dec, o.site, times[mid], 2.0);
            QJsonObject b{{"key", r.t.key}, {"name", r.t.name}, {"ra_hours", r.t.ra}, {"dec_deg", r.t.dec}, {"priority", r.t.prio}, {"faintness", std::round(r.t.faint * 100) / 100}, {"separation_deg", std::round(k.sep)},
                          {"alt", std::round(k.alt)}, {"az", std::round(k.az)}, {"covered_minutes", k.cov * int(o.stepMin)}};
            if (std::isfinite(lim.tMax)) b["t_max_s"] = std::round(lim.tMax * 10) / 10;
            out.append(b);
        }
        return out;
    };
    for (const Slot& s : plan) {
        QJsonObject e = info(rows[s.row]); e["backups"] = backupsFor(s); e["start"] = iso(times[s.s]); e["end"] = iso(times[s.e]);
        e["alt_start"] = std::round(rows[s.row].alt[s.s]); e["alt_end"] = std::round(rows[s.row].alt[s.e]);
        if (!o.weather.isEmpty()) { double m = 0; for (int j = s.s; j <= s.e; ++j) m += wq[j]; e["forecast_quality"] = std::round(m / (s.e - s.s + 1) * 100) / 100; }
        e["az_start"] = std::round(rows[s.row].az[s.s]); e["az_end"] = std::round(rows[s.row].az[s.e]); e["minutes"] = (s.e - s.s + 1) * o.stepMin;
        planJ.append(e);
    }
    for (const Row& r : rows) targetsJ.append(info(r));
    QJsonObject wx;
    if (!o.weather.isEmpty()) {
        QJsonArray hrs; QString rainFrom; int usable = 0, total = 0;
        QDateTime lastHour;
        for (int i = d.first(); i <= d.last(); ++i) {
            const Weather::Hour* h = Weather::at(o.weather, times[i]); if (!h) continue;
            ++total; if (wq[i] >= o.minQuality) ++usable;
            if (rainFrom.isEmpty() && (h->precipProb >= 50 || h->precip >= 0.2)) rainFrom = iso(h->utc);
            if (h->utc != lastHour) { hrs.append(Weather::toJson(*h)); lastHour = h->utc; }
        }
        wx = {{"min_quality", o.minQuality}, {"usable_minutes", usable * int(o.stepMin)}, {"dark_minutes", total * int(o.stepMin)}, {"hours", hrs}};
        if (!rainFrom.isEmpty()) wx["rain_from"] = rainFrom;
        if (rows.isEmpty() && total > 0) wx["note"] = "the forecast leaves no observable window; rerun with --weather off to plan without it";
    }
    QJsonObject result{{"ok", true}, {"ephemeris", Ephemeris::description()}, {"night", day.toString(Qt::ISODate)}, {"site", QJsonObject{{"lat_deg", o.site.latDeg}, {"lon_deg", o.site.lonDeg}}},
                       {"dark_start", iso(times[d.first()])}, {"dark_end", iso(times[d.last()])}, {"moon_illumination", std::round(illum[d.first()] * 100) / 100},
                       {"horizon", src + (hz.learnedBins() ? QString(" + %1 learned bins").arg(hz.learnedBins()) : QString())}, {"plan", planJ}, {"targets", targetsJ}};
    if (!wx.isEmpty()) result["weather"] = wx;
    return result;
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
    const QJsonObject wx = s["weather"].toObject();
    if (!wx.isEmpty()) {
        out += QString("\nForecast (Open-Meteo): %1 of %2 dark minutes usable (quality >= %3)").arg(int(wx["usable_minutes"].toDouble())).arg(int(wx["dark_minutes"].toDouble())).arg(wx["min_quality"].toDouble());
        if (wx.contains("rain_from")) out += "; rain from " + hhmmUtc(wx["rain_from"].toString()) + " local";
        out += "\n  hour  cloud(low/mid/high)  rain  quality\n";
        for (const QJsonValue& v : wx["hours"].toArray()) {
            const QJsonObject h = v.toObject();
            out += QString("  %1  %2% (%3/%4/%5)  %6%  %7\n").arg(hhmmUtc(h["utc"].toString())).arg(h["cloud"].toDouble(), 3, 'f', 0).arg(h["low"].toDouble(), 0, 'f', 0).arg(h["mid"].toDouble(), 0, 'f', 0)
                       .arg(h["high"].toDouble(), 0, 'f', 0).arg(h["precip_prob"].toDouble(), 0, 'f', 0).arg(h["quality"].toDouble(), 0, 'f', 2);
        }
        if (wx.contains("note")) out += "  " + wx["note"].toString() + "\n";
    }
    out += "\nPlan:\n";
    for (const QJsonValue& v : s["plan"].toArray()) {
        const QJsonObject r = v.toObject();
        out += QString("  %1-%2  %3 alt %4->%5, az %6->%7  (%8 min)\n").arg(hhmmUtc(r["start"].toString()), hhmmUtc(r["end"].toString())).arg(r["name"].toString().left(36), -36)
                   .arg(r["alt_start"].toInt()).arg(r["alt_end"].toInt()).arg(r["az_start"].toInt()).arg(r["az_end"].toInt()).arg(r["minutes"].toInt());
        QStringList bs;
        for (const QJsonValue& bv : r["backups"].toArray()) { const QJsonObject b = bv.toObject(); bs << QString("%1 (%2 deg away, alt %3%4)").arg(b["key"].toString()).arg(b["separation_deg"].toInt()).arg(b["alt"].toInt()).arg(b.contains("t_max_s") ? QString(", max %1 s").arg(b["t_max_s"].toDouble()) : QString()); }
        if (!bs.isEmpty()) out += "            backups: " + bs.join("; ") + "\n";
    }
    if (s["plan"].toArray().isEmpty()) out += "  nothing observable for a useful block tonight\n";
    return out;
}

QJsonObject sunMoonDay(const QDate& day, const Astro::Site& site, const QString& timeZone)
{
    const QTimeZone zone(timeZone.toUtf8());
    if (!zone.isValid()) return {{"ok", false}, {"error", "unknown timezone " + timeZone}};
    const QDateTime t0 = QDateTime(day, QTime(0, 0), zone).toUTC();
    const double kRad = M_PI / 180;
    auto moonAlt = [&](const QDateTime& t) { return Astro::moonAltitude(site, t); };
    auto moonH0 = [&](const QDateTime& t) { return -(34.0 / 60 + std::asin(1737.4 / Astro::moonDistanceKm(t)) / kRad); };
    auto sunAlt = [&](const QDateTime& t) { return Astro::altAz(Astro::sunPosition(t), site, t).alt; };
    auto crossing = [&](auto alt, auto h0, bool rising) -> QJsonValue {          // first crossing of h0 in the day, bisected to one second
        QDateTime a = t0; double fa = alt(a) - h0(a);
        for (int m = 1; m <= 24 * 60; ++m) {
            const QDateTime b = t0.addSecs(m * 60); const double fb = alt(b) - h0(b);
            if ((rising && fa < 0 && fb >= 0) || (!rising && fa >= 0 && fb < 0)) {
                QDateTime lo = a, hi = b;
                for (int i = 0; i < 12; ++i) { const QDateTime mid = lo.addMSecs(lo.msecsTo(hi) / 2); const double fm = alt(mid) - h0(mid); if ((fm >= 0) == rising) hi = mid; else lo = mid; }
                return iso(hi);
            }
            a = b; fa = fb;
        }
        return QJsonValue();
    };
    QJsonObject r{{"ok", true}, {"date", day.toString(Qt::ISODate)}, {"time_zone", timeZone}, {"ephemeris", Ephemeris::description()}};
    r["moonrise"] = crossing(moonAlt, moonH0, true); r["moonset"] = crossing(moonAlt, moonH0, false);
    r["sunrise"] = crossing(sunAlt, [](const QDateTime&) { return -0.833; }, true); r["sunset"] = crossing(sunAlt, [](const QDateTime&) { return -0.833; }, false);
    QDateTime best = t0; double bestAlt = -90;                                  // transit = highest altitude of the day
    for (int m = 0; m <= 24 * 60; m += 2) { const QDateTime t = t0.addSecs(m * 60); const double a = moonAlt(t); if (a > bestAlt) { bestAlt = a; best = t; } }
    for (int s = -120; s <= 120; s += 5) { const QDateTime t = best.addSecs(s); const double a = moonAlt(t); if (a > bestAlt) { bestAlt = a; best = t; } }
    r["moon_transit"] = QJsonObject{{"time", iso(best)}, {"alt_deg", std::round(bestAlt * 10) / 10}};
    const QDateTime mid = t0.addSecs(12 * 3600);                                 // illumination and phase at local midnight (end of the day)
    const QDateTime night = t0.addSecs(24 * 3600);
    const double ill = Astro::moonIllumination(night);
    const Astro::Equatorial sun = Astro::sunPosition(night), moon = Astro::moonPosition(night);
    double dra = moon.raHours - sun.raHours; while (dra > 12) dra -= 24; while (dra < -12) dra += 24;
    const bool waxing = dra > 0;
    QString name = ill < 0.02 ? "new" : ill > 0.98 ? "full" : ill < 0.45 ? (waxing ? "waxing crescent" : "waning crescent") : ill < 0.55 ? (waxing ? "first quarter" : "last quarter") : (waxing ? "waxing gibbous" : "waning gibbous");
    r["moon_illumination"] = std::round(ill * 1000) / 1000; r["moon_waxing"] = waxing; r["moon_phase"] = name;
    Q_UNUSED(mid)
    return r;
}

QJsonObject backupTargets(const Options& base, double raHours, double decDeg, const QDateTime& utc, double minSepDeg, int count, double minLeftMin, const QStringList& avoid)
{
    Options o = base; o.minBlockMin = 0;
    const QJsonObject s = schedule(o);
    if (!s["ok"].toBool()) return s;
    const Astro::AltAz cur = Astro::altAz({raHours, decDeg}, o.site, utc);
    struct Cand { double v; QJsonObject j; };
    QList<Cand> cs;
    for (const QJsonValue& tv : s["targets"].toArray()) {
        const QJsonObject t = tv.toObject();
        if (avoid.contains(t["key"].toString(), Qt::CaseInsensitive)) continue;
        QDateTime w0 = QDateTime::fromString(t["window_start"].toString(), "yyyy-MM-ddTHH:mm:ssZ"), w1 = QDateTime::fromString(t["window_end"].toString(), "yyyy-MM-ddTHH:mm:ssZ");
        w0.setTimeZone(QTimeZone::UTC); w1.setTimeZone(QTimeZone::UTC);
        if (!(w0 <= utc && utc <= w1)) continue;
        const double left = utc.secsTo(w1) / 60.0;
        if (left < minLeftMin) continue;
        const Astro::AltAz aa = Astro::altAz({t["ra_hours"].toDouble(), t["dec_deg"].toDouble()}, o.site, utc);
        const double sep = azAltSepDeg(cur.alt, cur.az, aa.alt, aa.az);
        if (sep < minSepDeg) continue;
        const RotLimit lim = maxExposure(t["ra_hours"].toDouble(), t["dec_deg"].toDouble(), o.site, utc, 2.0);
        QJsonObject j{{"key", t["key"]}, {"name", t["name"]}, {"ra_hours", t["ra_hours"]}, {"dec_deg", t["dec_deg"]}, {"priority", t["priority"]}, {"separation_deg", std::round(sep)},
                      {"alt", std::round(aa.alt)}, {"az", std::round(aa.az)}, {"minutes_left", std::round(left)}};
        if (std::isfinite(lim.tMax)) j["t_max_s"] = std::round(lim.tMax * 10) / 10;
        cs.append({t["priority"].toDouble() * (0.5 + 0.5 * std::min(1.0, aa.alt / 60)) * std::min(1.0, sep / 50) * std::min(1.0, left / 90), j});
    }
    std::stable_sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) { return a.v > b.v; });
    QJsonArray out; for (int i = 0; i < cs.size() && i < count; ++i) out.append(cs[i].j);
    QJsonObject r{{"ok", true}, {"from", QJsonObject{{"ra_hours", raHours}, {"dec_deg", decDeg}, {"alt", std::round(cur.alt)}, {"az", std::round(cur.az)}}}, {"backups", out}};
    if (out.isEmpty()) r["note"] = "no other target is observable now, far enough away and for long enough; relax --min-sep or --min-left, or --weather off";
    return r;
}

QJsonObject findTarget(const Options& base, const QString& name)
{
    Options o = base; o.minBlockMin = 0; o.skip.clear();
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
