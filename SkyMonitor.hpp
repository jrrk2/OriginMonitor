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
 * Trees and other fixed obstructions are told apart from cloud by evidence, not by the verdict alone: an opaque obstruction makes the sky darker than the
 * clear-sky baseline while cloud lit from below makes it brighter; a pointing at or below the known horizon profile is expected to be blocked; a direction that has
 * been bad on every visit and never good is an obstruction, one that has been good before is cloud. The record carries "cause" (obstruction | cloud | unknown) with
 * "cause_reasons". Only frames attributed to an obstruction count as "bad" in the persistent map of 10-degree azimuth x 5-degree altitude bins (sky_map.json);
 * cloud and unattributed frames are counted separately and never create an obstruction. The map cannot hold an obstruction above 60 degrees (nothing there is a tree line).
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
    enum class Kind { Good, Obstruction, Cloud, Unknown };
    void noteSkyMap(double az, double alt, Kind kind, const QDateTime& utc);
    void loadHorizon();
    double horizonAlt(double az) const;                // minimum usable altitude at this azimuth from the horizon file (0 when there is none)
    /// Decide why a frame that is not good is not good: a fixed obstruction (tree line, building) or cloud. Appends the evidence to `why`.
    QString classifyBad(double az, double alt, double excess, bool dark, bool haveDark, QStringList* why) const;
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
    QJsonObject m_skyMap;                      // "az_alt" -> {good, bad (obstruction only), cloud, unk, good_dates[], bad_dates[], cloud_dates[]}
    QList<QPair<double, double>> m_horizon;    // [az, min alt] from the horizon file
};
