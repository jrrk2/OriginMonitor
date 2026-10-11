#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

/**
 * Hourly weather forecast for the observing site from the Open-Meteo API (https://open-meteo.com, free, no key, non-commercial use),
 * cached on disk so the planner does not query it more than about once an hour. Used by the planner to drop hours that are
 * forecast cloudy, wet or foggy.
 */
namespace Weather {

struct Hour {
    QDateTime utc;                                // start of the hour
    double cloud = 0, low = 0, mid = 0, high = 0; // cloud cover, %
    double precipProb = 0, precip = 0;            // % and mm
    double visibilityM = 1e5, humidity = 0, dew = 0, temp = 0, wind = 0;   // m, %, degC, degC, m/s
    double quality = 1;                           // 0 (hopeless) .. 1 (clear and dry), see quality()
};

/// Fraction of the hour usable for imaging: low and mid cloud block, high cloud counts half, then rain chance, fog (visibility < 3 km),
/// dew risk (temperature within 1 degC of the dew point) and wind over 10 m/s each reduce it.
double quality(const Hour& h);

/// Fetch (or read the cache of) the forecast for the next `days` days. maxAgeMin 0 forces a fresh request. On failure falls back to a cache of any age.
/// Returns {ok, source, fetched, cached, hours:[...]}; hours are also parsed into `out`.
QJsonObject fetch(double latDeg, double lonDeg, int days, int maxAgeMin, const QString& cacheDir, QVector<Hour>* out);

/// The hour containing `utc` (nearest hour start), or nullptr when the forecast does not cover it.
const Hour* at(const QVector<Hour>& hours, const QDateTime& utc);

QJsonObject toJson(const Hour& h);

}   // namespace Weather
