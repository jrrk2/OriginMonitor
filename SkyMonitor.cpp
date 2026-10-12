#include "SkyMonitor.hpp"
#include "FocusAnalyzer.hpp"
#include "DataDir.hpp"
#include "Image16.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxObstructionAlt = 60;       // a bin whose lower edge is above this cannot be a tree line or building
QString baseDir() { return originDataDir(); }
QString safeName(const QString& s) { QString o = s; for (QChar& c : o) if (!c.isLetterOrNumber() && c != '-' && c != '_') c = '_'; return o.isEmpty() ? "target" : o; }
}

SkyMonitor::SkyMonitor() { loadSkyMap(); loadHorizon(); }

void SkyMonitor::loadHorizon()
{
    QString file = qEnvironmentVariable("ORIGIN_HORIZON");
    if (file.isEmpty()) for (const QString& c : {QString("tools/horizon.json"), QCoreApplication::applicationDirPath() + "/tools/horizon.json", QCoreApplication::applicationDirPath() + "/../tools/horizon.json",
                                               QCoreApplication::applicationDirPath() + "/../../tools/horizon.json", QCoreApplication::applicationDirPath() + "/../../../tools/horizon.json"})
        if (QFileInfo::exists(c)) { file = c; break; }
    QFile f(file);
    if (file.isEmpty() || !f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object()["points"].toArray()) m_horizon.append({v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble()});
    std::sort(m_horizon.begin(), m_horizon.end());
}

double SkyMonitor::horizonAlt(double az) const
{
    if (m_horizon.isEmpty()) return 0;
    az = std::fmod(az, 360.0); if (az < 0) az += 360.0;
    QList<QPair<double, double>> p;
    for (const auto& q : m_horizon) p.append({q.first - 360, q.second});
    p += m_horizon;
    for (const auto& q : m_horizon) p.append({q.first + 360, q.second});
    for (int i = 0; i + 1 < p.size(); ++i)
        if (az >= p[i].first && az <= p[i + 1].first) { const double d = p[i + 1].first - p[i].first; return d > 0 ? p[i].second + (p[i + 1].second - p[i].second) * (az - p[i].first) / d : p[i].second; }
    return p.first().second;
}

QString SkyMonitor::classifyBad(double az, double alt, double excess, bool dark, bool haveDark, QStringList* why) const
{
    int obs = 0, cld = 0;
    const double hz = horizonAlt(az);
    if (hz > 0 && alt < hz + 1.0) { obs += 2; *why << QString("pointing alt %1 is at or below the horizon profile (%2 at az %3)").arg(alt, 0, 'f', 0).arg(hz, 0, 'f', 0).arg(az, 0, 'f', 0); }
    if (dark && haveDark) {
        if (excess < -0.3) { obs += 2; *why << QString("sky %1% darker than the clear-sky baseline: something opaque blocks the light").arg(int(-excess * 100)); }
        else if (excess > 0.3) { cld += 2; *why << QString("sky %1% brighter than the clear-sky baseline: cloud lit from below").arg(int(excess * 100)); }
    }
    const QJsonObject bin = m_skyMap[mapKey(az, alt)].toObject();
    const int good = bin["good"].toInt(), bad = bin["bad"].toInt(), badNights = bin["bad_dates"].toArray().size();
    if (good == 0 && bad >= 3 && badNights >= 2) { obs += 2; *why << QString("this direction was bad on %1 nights and never good").arg(badNights); }
    else if (good >= 5) { cld += 1; *why << QString("this direction has been clear before (%1 good frames)").arg(good); }
    if (alt > kMaxObstructionAlt) { cld += 2; *why << QString("alt %1 is too high for a tree line or building").arg(alt, 0, 'f', 0); }
    return obs >= cld + 2 ? "obstruction" : cld > obs ? "cloud" : "unknown";
}

QJsonObject SkyMonitor::settings() const
{
    return QJsonObject{{"lat_deg", m_site.latDeg}, {"lon_deg", m_site.lonDeg}, {"black_level", m_blackLevel}, {"auto_stack", m_autoStack},
                       {"min_transparency", m_minTransparency}, {"target", m_target}};
}

QString SkyMonitor::stackDirectory() const { return baseDir() + "/stacks/" + safeName(m_target); }

QJsonObject SkyMonitor::setTarget(const QString& name, double raHours, double decDeg, bool reset)
{
    if (m_stack.frames() > 0) saveStack();
    m_target = name.isEmpty() ? "auto" : name;
    m_targetKnown = raHours >= 0 && raHours < 24 && decDeg >= -90 && decDeg <= 90;
    if (m_targetKnown) m_targetPos = {raHours, decDeg};
    m_stack.reset(); m_sinceSave = 0;
    bool resumed = false;
    if (!reset) resumed = m_stack.load(stackDirectory());
    QJsonObject o{{"ok", true}, {"target", m_target}, {"resumed", resumed}, {"stack", m_stack.summary()}};
    return o;
}

bool SkyMonitor::saveStack() { return m_stack.frames() > 0 && m_stack.save(stackDirectory()); }

QJsonObject SkyMonitor::stackSummary() const
{
    QJsonObject o = m_stack.summary();
    o["target"] = m_target; o["directory"] = stackDirectory();
    return o;
}

QString SkyMonitor::mapKey(double az, double alt) { return QString("%1_%2").arg(int(std::floor(az / 10.0)) * 10).arg(int(std::floor(alt / 5.0)) * 5); }

void SkyMonitor::loadSkyMap()
{
    QFile f(baseDir() + "/sky_map.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    m_skyMap = o["bins"].toObject();
    const QJsonObject d = o["dark_rate"].toObject();
    for (auto it = d.begin(); it != d.end(); ++it) m_darkRate[it.key()] = it.value().toDouble();
}

void SkyMonitor::saveSkyMap() const
{
    QDir().mkpath(baseDir());
    QJsonObject d; for (auto it = m_darkRate.begin(); it != m_darkRate.end(); ++it) d[it.key()] = it.value();
    QFile f(baseDir() + "/sky_map.json");
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"bins", m_skyMap}, {"dark_rate", d}}).toJson(QJsonDocument::Indented));
}

void SkyMonitor::noteSkyMap(double az, double alt, Kind kind, const QDateTime& utc)
{
    if (alt < 0) return;
    const QString key = mapKey(az, alt);
    QJsonObject b = m_skyMap[key].toObject();
    const char* count = kind == Kind::Good ? "good" : kind == Kind::Obstruction ? "bad" : kind == Kind::Cloud ? "cloud" : "unk";
    b[count] = b[count].toInt() + 1;
    if (kind != Kind::Unknown) {
        const QString field = kind == Kind::Good ? "good_dates" : kind == Kind::Obstruction ? "bad_dates" : "cloud_dates", day = utc.date().toString(Qt::ISODate);
        QJsonArray dates = b[field].toArray();
        bool have = false; for (const auto& v : dates) if (v.toString() == day) have = true;
        if (!have) { dates.append(day); while (dates.size() > 30) dates.removeFirst(); }
        b[field] = dates;
    }
    m_skyMap[key] = b;
    if (++m_mapDirty >= 20) { saveSkyMap(); m_mapDirty = 0; }
}

void SkyMonitor::noteGlare(double az, double alt, double excess, const QDateTime& utc)
{
    if (alt < 0) return;
    const QString key = mapKey(az, alt);
    QJsonObject b = m_skyMap[key].toObject();
    b["ex_sum"] = b["ex_sum"].toDouble() + excess; b["ex_n"] = b["ex_n"].toInt() + 1;
    const QString day = utc.date().toString(Qt::ISODate); QJsonArray dates = b["ex_dates"].toArray();
    bool have = false; for (const auto& v : dates) if (v.toString() == day) have = true;
    if (!have) { dates.append(day); while (dates.size() > 30) dates.removeFirst(); }
    b["ex_dates"] = dates;
    m_skyMap[key] = b;
    if (++m_mapDirty >= 20) { saveSkyMap(); m_mapDirty = 0; }
}

QJsonObject SkyMonitor::obstructionMap() const
{
    // glare: bins (at least 5 clear dark frames) whose mean sky excess is far above the typical bin: at least 1.6x the median and 0.4 above it (needs 4 such bins to have a median)
    QJsonArray glare;
    {
        QList<double> means; QList<QPair<QString, QPair<double, int>>> all;
        for (auto it = m_skyMap.begin(); it != m_skyMap.end(); ++it) { const QJsonObject b = it.value().toObject(); const int n = b["ex_n"].toInt(); if (n >= 5) { const double m = b["ex_sum"].toDouble() / n; means << m; all.append({it.key(), {m, n}}); } }
        if (means.size() >= 4) {
            std::sort(means.begin(), means.end()); const double med = means[means.size() / 2];
            for (const auto& e : all) if (e.second.first >= std::max(1.6 * med, med + 0.4)) glare.append(QJsonObject{{"bin", e.first}, {"mean_excess", std::round(e.second.first * 100) / 100}, {"frames", e.second.second}, {"median_excess", std::round(med * 100) / 100}});
        }
    }
    // A bin is an obstruction when frames attributed to an obstruction were seen on at least two different nights and no good frame was ever seen there.
    // Cloud frames are listed separately and never count; a candidate above 60 degrees is implausible and listed apart (nothing there is a tree line).
    QJsonArray suspect, mixed, implausible, profile;
    for (auto it = m_skyMap.begin(); it != m_skyMap.end(); ++it) {
        const QJsonObject b = it.value().toObject();
        const int good = b["good"].toInt(), bad = b["bad"].toInt(), badNights = b["bad_dates"].toArray().size();
        const QStringList p = it.key().split('_'); const int az = p.value(0).toInt(), alt = p.value(1).toInt();
        QJsonObject e{{"bin", it.key()}, {"good", good}, {"bad", bad}, {"bad_nights", badNights}, {"good_nights", b["good_dates"].toArray().size()},
                      {"cloud", b["cloud"].toInt()}, {"cloud_nights", b["cloud_dates"].toArray().size()}, {"unattributed", b["unk"].toInt()}};
        if (good == 0 && bad >= 3 && badNights >= 2) {
            if (alt >= kMaxObstructionAlt) implausible.append(e);
            else { suspect.append(e); profile.append(QJsonObject{{"az", az}, {"obstructed_up_to_alt", alt + 5}, {"horizon_file_alt", std::round(horizonAlt(az + 5))}}); }
        } else if (bad > 0 || b["cloud"].toInt() > 0) mixed.append(e);
    }
    return QJsonObject{{"ok", true}, {"likely_obstructed", suspect}, {"obstruction_profile", profile}, {"sometimes_bad", mixed}, {"implausible", implausible}, {"glare_bins", glare}, {"bins_total", m_skyMap.size()}};
}

// A picture of the frame for the eye (checking for trees, roofs and cloud): half-resolution colour binned again 2x2, black level removed, R and B scaled to the green
// median, asinh stretch to the 99.5th percentile. No background removal, so gradients and silhouettes stay visible. Written at most every 15 s, atomically.
static QString writePicture(const SkyFrame& F, double black, const QDateTime& utc, qint64* lastMs)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - *lastMs < 15000 || F.width < 8 || F.height < 8 || F.g.size() < size_t(F.width) * F.height) return QString();
    *lastMs = nowMs;
    const int w = F.width / 2, h = F.height / 2;
    std::vector<float> ch[3]; const std::vector<float>* src[3] = {&F.r, &F.g, &F.b};
    for (int c = 0; c < 3; ++c) { ch[c].resize(size_t(w) * h); for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const size_t i = size_t(2 * y) * F.width + 2 * x; const std::vector<float>& v = *src[c];
        ch[c][size_t(y) * w + x] = std::max(0.f, 0.25f * (v[i] + v[i + 1] + v[i + F.width] + v[i + F.width + 1]) - float(black)); } }
    auto med = [](std::vector<float> v) { if (v.empty()) return 0.f; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
    std::vector<float> sub[3]; for (int c = 0; c < 3; ++c) for (size_t i = 0; i < ch[c].size(); i += 11) sub[c].push_back(ch[c][i]);
    const float mg = med(sub[1]), gain[3] = {mg > 0 && med(sub[0]) > 0 ? mg / med(sub[0]) : 1.f, 1.f, mg > 0 && med(sub[2]) > 0 ? mg / med(sub[2]) : 1.f};
    std::vector<float> lum; for (size_t i = 0; i < ch[1].size(); i += 7) lum.push_back(ch[1][i]);
    std::sort(lum.begin(), lum.end()); const float top = std::max(lum.empty() ? 1.f : lum[size_t(0.995 * (lum.size() - 1))], 50.f), k = 12.f, norm = std::asinh(k);
    Image16 img = Image16::make(w, h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) {
        const float v = std::clamp(ch[c][size_t(y) * w + x] * gain[c] / top, 0.f, 1.f);
        img.row(y)[x * 3 + c] = quint16(std::clamp(std::asinh(k * v) / norm, 0.f, 1.f) * 65535.f);
    }
    const QString dir = originDataDir() + "/previews"; QDir().mkpath(dir);
    const QString path = dir + "/last_frame.png", tmp = path + ".tmp";
    if (!img.savePng(tmp)) return QString();
    QFile::remove(path); QFile::rename(tmp, path); (void)utc;
    return path;
}

QJsonObject SkyMonitor::process(const QByteArray& fits, double exposureS, int iso, double raRad, double decRad, const QDateTime& utc)
{
    SkyFrame F = analyseFrame(fits);
    FocusResult focus = focusFromFrame(F);
    QJsonObject o = focus.toJson();
    o["time"] = utc.toString(Qt::ISODateWithMs);
    o["exposure"] = exposureS; o["iso"] = iso;
    if (!F.valid) { o["verdict"] = "unreadable"; o["reasons"] = QJsonArray{F.note}; return o; }

    { const QString pic = writePicture(F, m_blackLevel, utc, &m_pictureMs); if (!pic.isEmpty()) { o["picture"] = pic; o["picture_time"] = utc.toString(Qt::ISODate); } }

    // --- geometry. An unaligned mount reports meaningless RA/Dec: use the plate-solved pointing when there is a recent one, else the geometry is unknown.
    QString source = "mount"; double useRa = raRad, useDec = decRad;
    if (!m_mountAligned) {
        if (m_fixAt.isValid() && m_fixAt.secsTo(utc) < 1800 && m_fixAt <= utc) { useRa = m_fixRa; useDec = m_fixDec; source = "plate_solve"; }
        else source = "unknown";
    }
    o["pointing_source"] = source;
    const Astro::Equatorial pointing{useRa * 12.0 / kPi, useDec * 180.0 / kPi};
    const Astro::AltAz aa = Astro::altAz(pointing, m_site, utc);
    const double sunAlt = Astro::altAz(Astro::sunPosition(utc), m_site, utc).alt;
    const double moonAlt = Astro::moonAltitude(m_site, utc), moonIll = Astro::moonIllumination(utc), moonSep = Astro::moonSeparationDeg(pointing, utc);
    if (source != "unknown") { o["ra_hours"] = pointing.raHours; o["dec_deg"] = pointing.decDeg; o["alt_deg"] = aa.alt; o["az_deg"] = aa.az; }
    o["sun_alt_deg"] = sunAlt; o["moon_alt_deg"] = moonAlt; o["moon_illumination"] = moonIll; o["moon_separation_deg"] = moonSep;
    o["target"] = m_target;
    double offTarget = 0;
    if (m_targetKnown) { offTarget = Astro::separationDeg(pointing, m_targetPos); o["off_target_deg"] = offTarget; }

    // --- sky level and gradient
    const double bgRate = std::max(0.0, F.background - m_blackLevel) / std::max(exposureS, 1e-6);
    const double gradient = skyGradient(F);
    o["sky_rate"] = bgRate; o["sky_gradient_sigma"] = gradient;
    o["stars_detected"] = int(F.stars.size()); o["layout_green_main_diagonal"] = F.greenOnMainDiagonal;
    const QString isoKey = QString("iso%1").arg(iso);
    const bool dark = sunAlt < -18 && moonAlt < -5;
    if (dark && bgRate > 0 && (!m_darkRate.contains(isoKey) || bgRate < m_darkRate[isoKey])) { m_darkRate[isoKey] = bgRate; ++m_mapDirty; }
    double excess = 0;
    if (m_darkRate.contains(isoKey) && m_darkRate[isoKey] > 0) { excess = bgRate / m_darkRate[isoKey] - 1.0; o["sky_excess_over_dark"] = excess; }

    // --- trailed stars: many sources but almost none round (seen 2026-10-11: the mount tracked at about half the sidereal rate for 6 minutes after a goto and the frames read "cloudy").
    // That is the mount, not the sky: say so, keep the frame out of the stack and the obstruction map, and let the supervisor re-centre instead of leaving for a backup target.
    const StreakInfo streaks = streakInfo(F);
    o["streaks"] = QJsonObject{{"elongated", streaks.elongated}, {"coherence", std::round(streaks.coherence * 100) / 100}, {"angle_deg", std::round(streaks.angleDeg)}};
    if (streaks.trailed && focus.stars <= 5 && source != "unknown") {
        o["verdict"] = "trailed"; o["cause"] = "mount";
        o["reasons"] = QJsonArray{QString("%1 elongated sources all pointing the same way (%2 deg, coherence %3) and only %4 round stars: trailed (the mount moved during the exposure)").arg(streaks.elongated).arg(streaks.angleDeg, 0, 'f', 0).arg(streaks.coherence, 0, 'f', 2).arg(focus.stars)};
        o["cause_reasons"] = QJsonArray{"trailed stars point at the mount or focus, not cloud"}; o["notes"] = QJsonArray(); o["stack"] = stackSummary();
        return o;
    }

    // --- defocus: plenty of stars but all of them big (donuts). The telescope's own autofocus after a goto can leave the focuser far off when it ran in cloud (2026-10-11: HFR 6+ px instead of
    // 2.4, the frames read "unknown" with 0 stars for an hour). A re-centring goto repeats that autofocus, which works once stars are visible.
    if (!streaks.trailed && focus.stars <= 3 && F.stars.size() >= 15 && source != "unknown") {
        std::vector<float> h; for (size_t i = 0; i < F.stars.size() && i < 30; ++i) h.push_back(F.stars[i].hfr);
        std::sort(h.begin(), h.end()); const float med = h[h.size() / 2];
        if (med >= 2.6f) {                                  // half-resolution pixels: focused is about 1.2, donuts 3 to 5
            o["verdict"] = "defocused"; o["cause"] = "focus";
            o["reasons"] = QJsonArray{QString("%1 stars but the brightest have a median radius of %2 px at half resolution (focused is about 1.2): out of focus, not cloud").arg(F.stars.size()).arg(med, 0, 'f', 1)};
            o["cause_reasons"] = QJsonArray{"defocused stars point at the focuser, not cloud"}; o["notes"] = QJsonArray(); o["stack"] = stackSummary();
            return o;
        }
    }

    // --- no usable pointing (mount not aligned, no plate solve): nothing about the geometry, the stack or the obstruction map can be judged
    if (source == "unknown") {
        o["verdict"] = "no_pointing";
        o["reasons"] = QJsonArray{QString("pointing unknown: the mount is not aligned and there is no plate solve; %1 stars detected").arg(F.stars.size())};
        o["notes"] = QJsonArray(); o["stack"] = stackSummary();
        return o;
    }

    // --- registration against the target's stack and per-tile coverage
    QStringList reasons; QString verdict = "clear";
    LiveStacker::Report rep;
    const bool haveStack = m_stack.hasReference();
    const bool pointingOk = !m_targetKnown || offTarget < 0.5;
    if (!pointingOk) { verdict = "off_target"; reasons << QString("mount is %1 deg from the target position").arg(offTarget, 0, 'f', 2); }
    if (haveStack && pointingOk) {
        rep = m_stack.assess(F, exposureS);                 // register and measure first; only frames judged usable are stacked
        QJsonObject reg{{"inliers", rep.inliers}, {"rms_px", rep.rms}, {"rotation_deg", rep.rotationDeg}, {"shift_px", rep.shiftPx}, {"matched_fraction", rep.matchedFraction},
                        {"transparency", rep.transparency}, {"raw_transparency", rep.rawTransparency}, {"stacked", rep.accepted && m_autoStack}, {"frames_in_stack", rep.frames}};
        if (!rep.reason.isEmpty()) reg["refused_because"] = rep.reason;
        QJsonArray cov; double worst = 1; int weak = 0;
        for (int i = 0; i < LiveStacker::Report::kTilesX * LiveStacker::Report::kTilesY; ++i) {
            const double c = rep.tileExpected[i] >= 3 ? double(rep.tileFound[i]) / rep.tileExpected[i] : -1.0;
            cov.append(c);
            if (c >= 0) { worst = std::min(worst, c); if (c < 0.3) ++weak; }
        }
        reg["tile_coverage"] = cov; reg["tile_grid"] = QString("%1x%2 row-major from top-left of the stack frame").arg(LiveStacker::Report::kTilesX).arg(LiveStacker::Report::kTilesY);
        o["registration"] = reg;
        // Many round, sharp stars that will not register to a SMALL stack mean the reference is bad (seen 2026-10-11: a half-trailed first frame became the reference and every later
        // good frame read "cloudy"), not that the sky is cloudy. After 3 such frames the small stack is dropped and the next frame seeds a new one.
        const bool sharpSky = focus.valid && focus.stars >= 40 && focus.hfrPx > 0 && focus.hfrPx < 4.5;
        if ((rep.inliers == 0 || rep.reason == "no star-pattern match") && sharpSky && (m_stack.frames() <= 10 || m_stack.summary()["total_exposure_s"].toDouble() < 300.0)) {   // a small stack: not worth keeping over sharp frames that will not match it
            if (++m_regFailRun >= 3) { m_stack.reset(); m_regFailRun = 0; verdict = "unknown"; reasons << QString("the stack reference was bad (%1 sharp stars would not register): replaced, the next frame starts a new one").arg(focus.stars); }
            else { verdict = "unknown"; reasons << QString("%1 sharp stars but no match to the stack reference (%2 of 3 before it is replaced)").arg(focus.stars).arg(m_regFailRun); }
        } else if (rep.inliers == 0 || rep.reason == "no star-pattern match") {
            m_regFailRun = 0;
            verdict = "cloudy"; reasons << QString("cannot register: %1 stars detected").arg(F.stars.size());
        } else if (rep.rawTransparency > 0) {
            if (rep.transparency < 0.5) { verdict = "cloudy"; reasons << QString("transparency %1").arg(rep.transparency, 0, 'f', 2); }
            else if (rep.transparency < 0.8) { verdict = "hazy"; reasons << QString("transparency %1").arg(rep.transparency, 0, 'f', 2); }
            if (weak >= 2 && verdict != "cloudy") { verdict = "patchy"; reasons << QString("%1 of 12 tiles have under 30% of their stars (cloud edge or obstruction)").arg(weak); }
        }
    } else if (!haveStack && pointingOk) {
        // First frame of a target: becomes the reference if it has enough stars and the sky is not lit by cloud. A reference seeded
        // in cloud makes every later clear frame fail to register and read as cloudy (seen 2026-10-09).
        const bool goodRef = focus.valid && focus.stars >= 25 && focus.hfrPx > 0 && focus.hfrPx < 4.0;
        if (!goodRef) {
            // a reference must be a sharp, round-star frame: a trailed or defocused one would make every later good frame fail to register
            o["registration"] = QJsonObject{{"reference_frame", false}, {"stacked", false}, {"frames_in_stack", 0}};
            verdict = "unknown"; reasons << QString("not a good reference frame (%1 round stars, HFR %2 px)").arg(focus.stars).arg(focus.hfrPx, 0, 'f', 1);
        } else if (excess > 2.0) {
            o["registration"] = QJsonObject{{"reference_frame", false}, {"stacked", false}, {"frames_in_stack", 0}};
            verdict = "cloudy"; reasons << QString("sky %1% above the dark baseline: not used as the stack reference").arg(int(excess * 100));
        } else {
            rep = m_autoStack ? m_stack.add(F, exposureS, m_minTransparency) : LiveStacker::Report();
            o["registration"] = QJsonObject{{"reference_frame", rep.accepted}, {"stacked", rep.accepted}, {"frames_in_stack", rep.frames}};
            if (!rep.accepted && m_autoStack) { verdict = F.stars.size() < 12 ? "cloudy" : "unknown"; reasons << rep.reason; }
            if (F.stars.size() < 25) { verdict = "cloudy"; reasons << QString("only %1 stars detected").arg(F.stars.size()); }
        }
    }

    // Stack the frame only if it is a usable one: clouds, patchy obstruction and wrong pointing would dilute the stack
    const bool stackable = haveStack && pointingOk && (verdict == "clear" || verdict == "hazy");
    if (stackable && m_autoStack) {
        const LiveStacker::Report added = m_stack.add(F, exposureS, m_minTransparency);
        rep.accepted = added.accepted; rep.frames = added.frames;
        QJsonObject reg = o["registration"].toObject(); reg["stacked"] = added.accepted; reg["frames_in_stack"] = added.frames;
        if (!added.reason.isEmpty()) reg["refused_because"] = added.reason;
        o["registration"] = reg;
    } else if (haveStack) {
        QJsonObject reg = o["registration"].toObject(); reg["stacked"] = false; reg["frames_in_stack"] = m_stack.frames();
        if (verdict != "clear" && verdict != "hazy") reg["refused_because"] = "frame judged " + verdict;
        o["registration"] = reg;
    }

    // --- sky brightness context (never overrides cloud, but explains it and flags what the moon or dawn are doing)
    QStringList notes;
    if (sunAlt > -12) notes << (sunAlt > -6 ? "bright twilight" : "twilight");
    if (moonAlt > 0 && moonIll > 0.05 && (excess > 0.3 || gradient > 8)) notes << QString("moon glow (moon alt %1, illumination %2, separation %3 deg)").arg(moonAlt, 0, 'f', 0).arg(moonIll, 0, 'f', 2).arg(moonSep, 0, 'f', 0);
    else if (excess > 0.5 && dark) notes << "sky brighter than the dark baseline without the moon or sun up (light pollution, thin cloud lit from below, or dew)";
    if (verdict == "clear" && !notes.isEmpty() && (moonAlt > 0 && moonIll > 0.05 && (excess > 0.3 || gradient > 8))) verdict = "moon_glow";
    if (verdict == "clear" && sunAlt > -12) verdict = "twilight";
    if (aa.alt < 20) notes << QString("low altitude (%1 deg): extinction, murk and obstructions likely").arg(aa.alt, 0, 'f', 0);
    o["notes"] = QJsonArray::fromStringList(notes);
    if (focus.valid && haveStack) { /* focus drift is left to the caller: compare hfr_px with earlier frames */ }

    // --- obstruction memory: only frames whose geometry is trustworthy count
    const bool good = verdict == "clear" || verdict == "moon_glow" || verdict == "twilight" || verdict == "hazy";
    QString cause = "none";
    if (!good && pointingOk && verdict != "unknown") {
        QStringList why;
        cause = classifyBad(aa.az, aa.alt, excess, dark, m_darkRate.contains(isoKey), &why);
        o["cause"] = cause; o["cause_reasons"] = QJsonArray::fromStringList(why);
    }
    if (good && verdict == "clear" && dark && m_darkRate.contains(isoKey) && pointingOk) noteGlare(aa.az, aa.alt, excess, utc);
    if (pointingOk && verdict != "unknown")
        noteSkyMap(aa.az, aa.alt, good ? Kind::Good : cause == "obstruction" ? Kind::Obstruction : cause == "cloud" ? Kind::Cloud : Kind::Unknown, utc);
    o["sky_map_bin"] = mapKey(aa.az, aa.alt);
    const QJsonObject bin = m_skyMap[mapKey(aa.az, aa.alt)].toObject();
    o["sky_map_history"] = QJsonObject{{"good", bin["good"].toInt()}, {"bad", bin["bad"].toInt()}, {"cloud", bin["cloud"].toInt()}, {"bad_nights", bin["bad_dates"].toArray().size()}, {"good_nights", bin["good_dates"].toArray().size()}};

    o["verdict"] = verdict; o["reasons"] = QJsonArray::fromStringList(reasons);
    o["stack"] = stackSummary();
    if (stackable && m_autoStack && rep.accepted && ++m_sinceSave >= 10) { saveStack(); m_sinceSave = 0; }
    if (m_mapDirty >= 20) { saveSkyMap(); m_mapDirty = 0; }
    return o;
}
