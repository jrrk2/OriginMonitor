#include "Weather.hpp"

#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>

namespace Weather {

double quality(const Hour& h)
{
    double q = (1 - h.low / 100) * (1 - h.mid / 100) * (1 - 0.5 * h.high / 100);
    if (h.precipProb >= 40 || h.precip >= 0.1) q *= 0.2;
    if (h.visibilityM < 3000) q *= 0.3;
    if (h.temp - h.dew < 1.0) q *= 0.7;
    if (h.wind > 10) q *= 0.7;
    return std::max(0.0, std::min(1.0, q));
}

QJsonObject toJson(const Hour& h)
{
    auto r1 = [](double v) { return std::round(v * 10) / 10; };
    return {{"utc", h.utc.toString(Qt::ISODate)}, {"cloud", h.cloud}, {"low", h.low}, {"mid", h.mid}, {"high", h.high}, {"precip_prob", h.precipProb}, {"precip_mm", h.precip},
            {"visibility_km", r1(h.visibilityM / 1000)}, {"humidity", h.humidity}, {"dew_c", h.dew}, {"temp_c", h.temp}, {"wind_ms", h.wind}, {"quality", std::round(h.quality * 100) / 100}};
}

static bool parse(const QJsonObject& d, QVector<Hour>* out)
{
    const QJsonObject h = d["hourly"].toObject();
    const QJsonArray t = h["time"].toArray();
    if (t.isEmpty()) return false;
    auto col = [&](const char* k, int i, double def) { const QJsonArray a = h[k].toArray(); return i < a.size() && !a[i].isNull() ? a[i].toDouble() : def; };
    out->clear();
    for (int i = 0; i < t.size(); ++i) {
        Hour x;
        x.utc = QDateTime::fromString(t[i].toString(), "yyyy-MM-ddTHH:mm"); x.utc.setTimeZone(QTimeZone::UTC);
        x.cloud = col("cloud_cover", i, 0); x.low = col("cloud_cover_low", i, 0); x.mid = col("cloud_cover_mid", i, 0); x.high = col("cloud_cover_high", i, 0);
        x.precipProb = col("precipitation_probability", i, 0); x.precip = col("precipitation", i, 0); x.visibilityM = col("visibility", i, 1e5);
        x.humidity = col("relative_humidity_2m", i, 0); x.dew = col("dew_point_2m", i, 0); x.temp = col("temperature_2m", i, 10); x.wind = col("wind_speed_10m", i, 0);
        x.quality = quality(x);
        if (x.utc.isValid()) out->append(x);
    }
    return !out->isEmpty();
}

QJsonObject fetch(double lat, double lon, int days, int maxAgeMin, const QString& cacheDir, QVector<Hour>* out)
{
    const QString cachePath = QString("%1/weather_%2_%3.json").arg(cacheDir).arg(lat, 0, 'f', 2).arg(lon, 0, 'f', 2);
    QJsonObject cached; qint64 fetchedMs = 0;
    QFile cf(cachePath);
    if (cf.open(QIODevice::ReadOnly)) { cached = QJsonDocument::fromJson(cf.readAll()).object(); fetchedMs = qint64(cached["fetched_ms"].toDouble()); cf.close(); }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto done = [&](const QJsonObject& data, bool fromCache, qint64 when, const QString& note) {
        QVector<Hour> tmp; QVector<Hour>& v = out ? *out : tmp;
        if (!parse(data, &v)) return QJsonObject{{"ok", false}, {"error", "forecast had no hourly data"}};
        QJsonArray hs; for (const Hour& h : v) hs.append(toJson(h));
        QJsonObject r{{"ok", true}, {"source", "Open-Meteo (open-meteo.com)"}, {"cached", fromCache}, {"fetched", QDateTime::fromMSecsSinceEpoch(when, QTimeZone::UTC).toString(Qt::ISODate)}, {"hours", hs}};
        if (!note.isEmpty()) r["note"] = note;
        return r;
    };
    if (maxAgeMin > 0 && fetchedMs && now - fetchedMs < qint64(maxAgeMin) * 60000 && cached.contains("data")) return done(cached["data"].toObject(), true, fetchedMs, QString());

    QUrl url("https://api.open-meteo.com/v1/forecast");
    QUrlQuery q;
    q.addQueryItem("latitude", QString::number(lat, 'f', 4)); q.addQueryItem("longitude", QString::number(lon, 'f', 4));
    q.addQueryItem("hourly", "cloud_cover,cloud_cover_low,cloud_cover_mid,cloud_cover_high,precipitation_probability,precipitation,visibility,relative_humidity_2m,dew_point_2m,temperature_2m,wind_speed_10m");
    q.addQueryItem("forecast_days", QString::number(std::max(1, std::min(days, 7)))); q.addQueryItem("past_days", "1");
    q.addQueryItem("timezone", "UTC"); q.addQueryItem("wind_speed_unit", "ms");
    url.setQuery(q);
    QNetworkAccessManager nam;
    QNetworkReply* rep = nam.get(QNetworkRequest(url));
    QEventLoop loop; QTimer::singleShot(15000, &loop, &QEventLoop::quit); QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QString err;
    if (!rep->isFinished()) { err = "timed out"; rep->abort(); }
    else if (rep->error() != QNetworkReply::NoError) err = rep->errorString();
    const QByteArray body = err.isEmpty() ? rep->readAll() : QByteArray();
    rep->deleteLater();
    if (err.isEmpty()) {
        const QJsonObject data = QJsonDocument::fromJson(body).object();
        if (data.contains("hourly")) {
            QFile f(cachePath);
            if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"fetched_ms", double(now)}, {"data", data}}).toJson(QJsonDocument::Compact));
            return done(data, false, now, QString());
        }
        err = "unexpected reply: " + data["reason"].toString();
    }
    if (cached.contains("data")) return done(cached["data"].toObject(), true, fetchedMs, "forecast request failed (" + err + "); using the cached copy");
    return {{"ok", false}, {"error", "weather forecast unavailable: " + err}};
}

const Hour* at(const QVector<Hour>& hours, const QDateTime& utc)
{
    const Hour* best = nullptr; qint64 bestD = 1800 + 1;
    for (const Hour& h : hours) { const qint64 d = std::llabs(h.utc.secsTo(utc.toUTC())); if (d < bestD) { bestD = d; best = &h; } }
    return best;
}

}   // namespace Weather
