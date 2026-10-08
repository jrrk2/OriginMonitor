#pragma once

#include "Astro.hpp"
#include "LiveStacker.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QString>

/**
 * Per-frame sky analysis for an external decision maker (a Claude session driving the telescope through the control server).
 * The monitor never moves the telescope. For every snapshot it measures focus, transparency against the current target's stack,
 * per-tile star coverage (clouds and trees take bites out of the field), sky background and gradient (moon glow, twilight), and the
 * sun/moon/target geometry, adds the frame to the target's live stack, and returns one JSON record with a suggested verdict.
 *
 * Trees and other fixed obstructions show up as bad frames at the same azimuth/altitude on every visit; clouds drift. The monitor keeps a
 * persistent map of good/bad frames per 10-degree azimuth x 5-degree altitude bin (sky_map.json) so the caller can tell them apart.
 */
class SkyMonitor {
public:
    SkyMonitor();

    void setSite(const Astro::Site& site) { m_site = site; }
    Astro::Site site() const { return m_site; }
    void setBlackLevel(double adu) { m_blackLevel = adu; }
    void setAutoStack(bool on) { m_autoStack = on; }
    void setMinTransparency(double t) { m_minTransparency = t; }
    QJsonObject settings() const;

    /// Choose the stack frames are added to. A saved stack with the same name is resumed. Negative ra/dec leave the position unknown.
    QJsonObject setTarget(const QString& name, double raHours, double decDeg, bool reset);
    QString targetName() const { return m_target; }
    LiveStacker& stacker() { return m_stack; }
    QJsonObject stackSummary() const;
    QString stackDirectory() const;
    bool saveStack();

    /// Analyse one snapshot (raw 16-bit FITS bytes) taken at the mount position (ra, dec in radians).
    QJsonObject process(const QByteArray& fits, double exposureS, int iso, double raRad, double decRad, const QDateTime& utc);

    QJsonObject obstructionMap() const;

private:
    void noteSkyMap(double az, double alt, bool good, const QDateTime& utc);
    void loadSkyMap();
    void saveSkyMap() const;
    static QString mapKey(double az, double alt);

    Astro::Site m_site;
    double m_blackLevel = 4985.0;
    bool m_autoStack = true;
    double m_minTransparency = 0.5;
    QString m_target = "auto";
    bool m_targetKnown = false;
    Astro::Equatorial m_targetPos;
    LiveStacker m_stack;
    int m_sinceSave = 0, m_mapDirty = 0;
    QMap<QString, double> m_darkRate;          // "iso800" -> lowest (sky ADU per second) seen in dark conditions
    QJsonObject m_skyMap;                      // "az_alt" -> {good, bad, good_dates[], bad_dates[]}
};
