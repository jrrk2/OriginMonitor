#pragma once

#include "Astro.hpp"
#include "Weather.hpp"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

/**
 * Offline observing planner and exposure limits (no telescope connection needed). C++ replacement for tools/schedule.py and tools/fieldrot.py.
 * Sun, Moon and target altitudes come from Astro.cpp (low precision: ample for "is it dark, is it above the local horizon").
 */
namespace Planner {

struct Options {
    QDate date;                                   // local evening date; invalid = the evening whose night we are in now
    Astro::Site site;
    QString horizonFile;                          // JSON {"points": [[az_deg, min_alt_deg], ...]}; empty = flat at minAlt
    QString skyMapFile;                           // sky_map.json from SkyMonitor; empty = none
    double minAlt = 20, sunAlt = -18, moonSep = 30, minBlockMin = 45, maxBlockMin = 150, stepMin = 5;
    int top = 25;
    QString timeZone = "Europe/London";
    QVector<Weather::Hour> weather;               // hourly forecast; empty = ignore the weather
    double minQuality = 0.5;                      // hours forecast below this (see Weather::quality) are treated as unobservable
};

/// Whole-night plan as JSON: {ok, night, dark_start, dark_end, moon_illumination, horizon, plan[], targets[]} with ISO-8601 UTC times.
/// With a forecast in Options::weather the result also has a "weather" block (hourly cloud/quality over the dark window, rain_from) and each plan entry a mean forecast quality.
QJsonObject schedule(const Options& o);
/// Human-readable table of the same thing.
QString scheduleText(const QJsonObject& s, int top);
/// The evening date that "now" belongs to (before noon we are still in last evening's night).
QDate currentNightDate(const QString& timeZone);
/// Look a target up by key ("M31") or name among those observable tonight (minBlockMin 0); returns the target object or {ok:false}.
QJsonObject findTarget(const Options& o, const QString& name);
/// Sun and Moon events for one local day: {ok, date, ephemeris, moonrise, moonset (ISO UTC or null), moon_transit{time, alt_deg}, moon_illumination (at local midnight),
/// moon_phase, moon_waxing, sunrise, sunset}. Rise and set use the topocentric altitude of the centre at -(34' refraction + semi-diameter), found to a second.
QJsonObject sunMoonDay(const QDate& day, const Astro::Site& site, const QString& timeZone);
/// Backup targets for "partial cloud covers what I am pointing at": targets observable at `utc` for at least minLeftMin more minutes, at least minSepDeg away
/// (as seen in the sky, since clouds move with the sky's alt/az frame, not with the stars), ranked by priority, altitude, separation and time left; with each one's field-rotation exposure limit.
/// {ok, from:{...}, backups:[{key, name, ra_hours, dec_deg, priority, separation_deg, alt, az, minutes_left, t_max_s}]}. `avoid` lists keys already tried.
QJsonObject backupTargets(const Options& o, double raHours, double decDeg, const QDateTime& utc, double minSepDeg, int count, double minLeftMin, const QStringList& avoid);
/// Locate tools/horizon.json: $ORIGIN_HORIZON, ./tools, then next to the executable.
QString defaultHorizonFile();

/// Field-rotation limit for an alt-az mount. Table over `hours` from now, every 30 minutes: {ok, rows:[{utc, alt, az, rotation_deg_per_hour, t_max_s, t_max_1px_s}]}.
QJsonObject fieldRotation(double raHours, double decDeg, const Astro::Site& site, double tolPx, double hours);
/// Longest exposure (s) that keeps corner trail under tolPx at one instant; infinity when the target is due east/west. Returns alt/az/rotation too.
struct RotLimit { double tMax, alt, az, rotDegPerHour; };
RotLimit maxExposure(double raHours, double decDeg, const Astro::Site& site, const QDateTime& utc, double tolPx);

}   // namespace Planner
