#pragma once

#include <QDateTime>
#include <array>
#include <QJsonObject>
#include <QMap>
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
    double visibilityM = 1e5, humidity = 0, dew = 0, temp = 0, wind = 0, gust = 0;   // m, %, degC, degC, m/s, m/s
    double quality = 1;                           // 0 (hopeless) .. 1 (clear and dry), see quality(); with several models the mean of their qualities
    QMap<QString, double> models;                 // quality according to each forecast model (empty when only one was available)
    double qMin = 1, qMax = 1;                    // range of that spread
    QMap<QString, std::array<double, 4>> layers;  // each model's low/mid/high/total cloud, %
    double qModel = -1;                           // quality before the METAR nowcast was applied (-1: not adjusted)
    double obsWeight = 0;                         // 0..1 how much the nearby airport observations shaped this hour (fades over a few hours)
};

/// Fraction of the hour usable for imaging: low and mid cloud block, high cloud counts half, then rain chance, fog (visibility < 3 km),
/// dew risk (temperature within 1 degC of the dew point) and wind over 10 m/s each reduce it.
double quality(const Hour& h);

/// The cloud layers come from several models (Met Office, ECMWF, ECMWF AIFS, ICON, GFS, Meteo-France, KNMI) because single models disagree by 50 points or more
/// for a given hour; low/mid/high are their means, quality is the mean of the per-model qualities, models/qMin/qMax keep the spread.
/// Fetch (or read the cache of) the forecast for the next `days` days. maxAgeMin 0 forces a fresh request. On failure falls back to a cache of any age.
/// Returns {ok, source, fetched, cached, hours:[...]}; hours are also parsed into `out`.
QJsonObject fetch(double latDeg, double lonDeg, int days, int maxAgeMin, const QString& cacheDir, QVector<Hour>* out);

/// The hour containing `utc` (nearest hour start), or nullptr when the forecast does not cover it.
const Hour* at(const QVector<Hour>& hours, const QDateTime& utc);

/// Precipitation (mm) per 15 minutes for the next `hours` hours (Open-Meteo minutely_15; covers the UK), never cached. Empty on failure.
QVector<QPair<QDateTime, double>> nowcast(double latDeg, double lonDeg, int hours);

/// METAR nowcast: the latest reports from airports within about 60 km give the actual low/mid cloud cover now. Models whose current low/mid cover
/// is far from it are down-weighted, with the weight fading over about 3 hours, so the blend follows the models that are right today.
/// Applied automatically inside fetch(); this returns the observation used ({ok, cover, stations:[...], obs_time}) for display.
QJsonObject metarNowcast(double latDeg, double lonDeg, const QString& cacheDir, int maxAgeMin, QVector<Hour>* hours);

QJsonObject toJson(const Hour& h);

}   // namespace Weather
