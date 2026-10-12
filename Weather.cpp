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
#include <csetjmp>
#include <cstdio>
#include <vector>
extern "C" {
#include <jpeglib.h>
}
#include <QDir>

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
    QJsonObject mj; for (auto it = h.models.constBegin(); it != h.models.constEnd(); ++it) mj[it.key()] = it.value();
    return {{"utc", h.utc.toString(Qt::ISODate)}, {"cloud", h.cloud}, {"low", h.low}, {"mid", h.mid}, {"high", h.high}, {"precip_prob", h.precipProb}, {"precip_mm", h.precip},
            {"visibility_km", r1(h.visibilityM / 1000)}, {"humidity", h.humidity}, {"dew_c", h.dew}, {"temp_c", h.temp}, {"wind_ms", h.wind}, {"gust_ms", h.gust}, {"quality", std::round(h.quality * 100) / 100}, {"q_min", std::round(h.qMin * 100) / 100}, {"q_max", std::round(h.qMax * 100) / 100}, {"q_model", std::round((h.qModel >= 0 ? h.qModel : h.quality) * 100) / 100}, {"obs_weight", std::round(h.obsWeight * 100) / 100}, {"models", mj}};
}

static const QStringList kModels = {"ukmo_seamless", "ecmwf_ifs025", "ecmwf_aifs025_single", "icon_seamless", "gfs_seamless", "meteofrance_seamless", "knmi_seamless"};

static bool parse(const QJsonObject& d, const QJsonObject& multi, QVector<Hour>* out)
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
        x.humidity = col("relative_humidity_2m", i, 0); x.dew = col("dew_point_2m", i, 0); x.temp = col("temperature_2m", i, 10); x.wind = col("wind_speed_10m", i, 0); x.gust = col("wind_gusts_10m", i, x.wind);
        x.quality = quality(x);
        // several models: each one's cloud layers with the shared rain/visibility/dew/wind; blend = mean of the per-model qualities and layers
        const QJsonObject mh = multi["hourly"].toObject(); const QJsonArray mt = mh["time"].toArray();
        if (!mt.isEmpty() && i < mt.size() && mt[i] == t[i]) {
            double sq = 0, sl = 0, sm = 0, sh = 0, sc = 0; int n = 0; x.qMin = 1; x.qMax = 0;
            for (const QString& m : kModels) {
                const QJsonArray al = mh["cloud_cover_low_" + m].toArray(), am = mh["cloud_cover_mid_" + m].toArray(), ah = mh["cloud_cover_high_" + m].toArray(), ac = mh["cloud_cover_" + m].toArray();
                if (i >= al.size() || al[i].isNull() || i >= am.size() || am[i].isNull() || i >= ah.size() || ah[i].isNull()) continue;
                Hour y = x; y.low = al[i].toDouble(); y.mid = am[i].toDouble(); y.high = ah[i].toDouble();
                const double qm = quality(y); x.models[m] = std::round(qm * 100) / 100;
                sq += qm; sl += y.low; sm += y.mid; sh += y.high; sc += (i < ac.size() && !ac[i].isNull()) ? ac[i].toDouble() : y.low; ++n; x.qMin = std::min(x.qMin, qm); x.qMax = std::max(x.qMax, qm);
            }
            if (n >= 2) { x.quality = sq / n; x.low = sl / n; x.mid = sm / n; x.high = sh / n; x.cloud = sc / n; } else { x.models.clear(); x.qMin = x.qMax = x.quality; }
        } else x.qMin = x.qMax = x.quality;
        for (const QString& m : x.models.keys()) {
            const QJsonArray al = multi["hourly"].toObject()["cloud_cover_low_" + m].toArray(), am = multi["hourly"].toObject()["cloud_cover_mid_" + m].toArray(), ah = multi["hourly"].toObject()["cloud_cover_high_" + m].toArray();
            const QJsonArray ac = multi["hourly"].toObject()["cloud_cover_" + m].toArray();
            x.layers[m] = {al[i].toDouble(), am[i].toDouble(), ah[i].toDouble(), i < ac.size() && !ac[i].isNull() ? ac[i].toDouble() : al[i].toDouble()};
        }
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
    auto done = [&](const QJsonObject& data, const QJsonObject& multi, bool fromCache, qint64 when, const QString& note) {
        QVector<Hour> tmp; QVector<Hour>& v = out ? *out : tmp;
        if (!parse(data, multi, &v)) return QJsonObject{{"ok", false}, {"error", "forecast had no hourly data"}};
        const QJsonObject nc = metarNowcast(lat, lon, cacheDir, 15, &v);
        QJsonArray hs; for (const Hour& h : v) hs.append(toJson(h));
        QJsonObject r{{"ok", true}, {"source", multi.isEmpty() ? "Open-Meteo (open-meteo.com)" : "Open-Meteo (open-meteo.com), blend of 7 models"}, {"cached", fromCache}, {"fetched", QDateTime::fromMSecsSinceEpoch(when, QTimeZone::UTC).toString(Qt::ISODate)}, {"hours", hs}};
        if (!note.isEmpty()) r["note"] = note;
        if (nc["ok"].toBool()) r["metar"] = nc;
        return r;
    };
    if (maxAgeMin > 0 && fetchedMs && now - fetchedMs < qint64(maxAgeMin) * 60000 && cached.contains("data")) return done(cached["data"].toObject(), cached["multi"].toObject(), true, fetchedMs, QString());

    QUrl url("https://api.open-meteo.com/v1/forecast");
    QUrlQuery q;
    q.addQueryItem("latitude", QString::number(lat, 'f', 4)); q.addQueryItem("longitude", QString::number(lon, 'f', 4));
    q.addQueryItem("hourly", "cloud_cover,cloud_cover_low,cloud_cover_mid,cloud_cover_high,precipitation_probability,precipitation,visibility,relative_humidity_2m,dew_point_2m,temperature_2m,wind_speed_10m,wind_gusts_10m");
    q.addQueryItem("forecast_days", QString::number(std::max(1, std::min(days, 7)))); q.addQueryItem("past_days", "1");
    q.addQueryItem("timezone", "UTC"); q.addQueryItem("wind_speed_unit", "ms");
    url.setQuery(q);
    auto get = [](const QUrl& u, QString* err) {
        QNetworkAccessManager nam;
        QNetworkReply* rep = nam.get(QNetworkRequest(u));
        QEventLoop loop; QTimer::singleShot(15000, &loop, &QEventLoop::quit); QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        if (!rep->isFinished()) { *err = "timed out"; rep->abort(); }
        else if (rep->error() != QNetworkReply::NoError) *err = rep->errorString();
        const QByteArray body = err->isEmpty() ? rep->readAll() : QByteArray();
        rep->deleteLater();
        return body;
    };
    QString err;
    const QByteArray body = get(url, &err);
    if (err.isEmpty()) {
        const QJsonObject data = QJsonDocument::fromJson(body).object();
        if (data.contains("hourly")) {
            // second request: cloud layers from several models (the first, best-match request supplies rain, visibility, dew and wind)
            QUrl murl("https://api.open-meteo.com/v1/forecast"); QUrlQuery mq;
            mq.addQueryItem("latitude", QString::number(lat, 'f', 4)); mq.addQueryItem("longitude", QString::number(lon, 'f', 4));
            mq.addQueryItem("hourly", "cloud_cover,cloud_cover_low,cloud_cover_mid,cloud_cover_high"); mq.addQueryItem("models", kModels.join(","));
            mq.addQueryItem("forecast_days", QString::number(std::max(1, std::min(days, 7)))); mq.addQueryItem("past_days", "1"); mq.addQueryItem("timezone", "UTC");
            murl.setQuery(mq);
            QString merr; const QJsonObject multi = QJsonDocument::fromJson(get(murl, &merr)).object();
            const QJsonObject usable = (merr.isEmpty() && multi.contains("hourly")) ? multi : QJsonObject();
            QFile f(cachePath);
            if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"fetched_ms", double(now)}, {"data", data}, {"multi", usable}}).toJson(QJsonDocument::Compact));
            return done(data, usable, false, now, usable.isEmpty() ? "multi-model request failed (" + merr + "); single best-match model" : QString());
        }
        err = "unexpected reply: " + data["reason"].toString();
    }
    if (cached.contains("data")) return done(cached["data"].toObject(), cached["multi"].toObject(), true, fetchedMs, "forecast request failed (" + err + "); using the cached copy");
    return {{"ok", false}, {"error", "weather forecast unavailable: " + err}};
}

static QByteArray httpGet(const QUrl& u, QString* err)
{
    QNetworkAccessManager nam;
    QNetworkReply* rep = nam.get(QNetworkRequest(u));
    QEventLoop loop; QTimer::singleShot(10000, &loop, &QEventLoop::quit); QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (!rep->isFinished()) { *err = "timed out"; rep->abort(); }
    else if (rep->error() != QNetworkReply::NoError) *err = rep->errorString();
    const QByteArray body = err->isEmpty() ? rep->readAll() : QByteArray();
    rep->deleteLater();
    return body;
}

// --- Institute of Astronomy all-sky camera (tel05.ast.cam.ac.uk, a 576x720 greyscale fisheye frame every ~10 s) ----------------------------------------

namespace {
struct JpegErr { jpeg_error_mgr pub; jmp_buf jb; };
bool decodeGray(const QByteArray& data, int* w, int* h, std::vector<unsigned char>* px)
{
    jpeg_decompress_struct c; JpegErr err; c.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = [](j_common_ptr cp) { longjmp(reinterpret_cast<JpegErr*>(cp->err)->jb, 1); };
    if (setjmp(err.jb)) { jpeg_destroy_decompress(&c); return false; }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, reinterpret_cast<const unsigned char*>(data.constData()), data.size());
    jpeg_read_header(&c, TRUE); c.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&c);
    *w = int(c.output_width); *h = int(c.output_height); px->assign(size_t(*w) * *h, 0);
    while (c.output_scanline < c.output_height) { JSAMPROW row = px->data() + size_t(c.output_scanline) * *w; jpeg_read_scanlines(&c, &row, 1); }
    jpeg_finish_decompress(&c); jpeg_destroy_decompress(&c);
    return true;
}
}

// Low-precision solar altitude (degrees), enough to tell day from twilight from night for the all-sky camera's trust.
static double sunAltitudeDeg(double latDeg, double lonDeg, const QDateTime& utc)
{
    const double kPi = 3.14159265358979323846, n = utc.toMSecsSinceEpoch() / 86400000.0 + 2440587.5 - 2451545.0;
    auto wrap = [](double d) { d = std::fmod(d, 360.0); return d < 0 ? d + 360.0 : d; };
    const double L = wrap(280.460 + 0.9856474 * n), g = wrap(357.528 + 0.9856003 * n) * kPi / 180.0, lam = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * kPi / 180.0, eps = (23.439 - 0.0000004 * n) * kPi / 180.0;
    const double ra = std::atan2(std::cos(eps) * std::sin(lam), std::cos(lam)), dec = std::asin(std::sin(eps) * std::sin(lam));
    const double gmst = wrap(280.46061837 + 360.98564736629 * n), ha = (gmst + lonDeg) * kPi / 180.0 - ra, lat = latDeg * kPi / 180.0;
    return std::asin(std::sin(dec) * std::sin(lat) + std::cos(dec) * std::cos(lat) * std::cos(ha)) * 180.0 / kPi;
}

/// Cloud fraction (0..1) over the central 75% of the fisheye radius (zenith angle up to about 65 deg, which clears the dome, wall and trees at the edge), from the latest frame.
/// Daytime/twilight method calibrated on one frame: pixels more than 25 grey levels above the 20th percentile (clear sky, capped at 90) count as cloud.
/// Returns ok=false when the frame is stale or too dark for that method (night frames are archived under allsky/ for calibrating a night method).
static QJsonObject allSkyCover(const QString& dir, double* cover, QDateTime* when, double sunAlt)
{
    const QString img = dir + "/allsky_latest.jpg", meta = dir + "/allsky_latest.json";
    QJsonObject m; { QFile f(meta); if (f.open(QIODevice::ReadOnly)) m = QJsonDocument::fromJson(f.readAll()).object(); }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!QFile::exists(img) || nowMs - qint64(m["fetched_ms"].toDouble()) > 5 * 60000) {
        QNetworkAccessManager nam; QNetworkRequest rq{QUrl("http://tel05.ast.cam.ac.uk/images/latest_image.jpg")};
        QNetworkReply* rep = nam.get(rq);
        QEventLoop loop; QTimer::singleShot(10000, &loop, &QEventLoop::quit); QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
        if (rep->isFinished() && rep->error() == QNetworkReply::NoError) {
            const QByteArray body = rep->readAll(); const QDateTime lm = rep->header(QNetworkRequest::LastModifiedHeader).toDateTime().toUTC();
            if (body.size() > 1000) {
                QFile f(img); if (f.open(QIODevice::WriteOnly)) { f.write(body); f.close(); }
                m = QJsonObject{{"fetched_ms", double(nowMs)}, {"modified", lm.isValid() ? lm.toString(Qt::ISODate) : QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
                QFile mf(meta); if (mf.open(QIODevice::WriteOnly)) mf.write(QJsonDocument(m).toJson(QJsonDocument::Compact));
                // archive a frame every 10 minutes (about 80 KB each, kept for 400 days) so tools/sky_history.py can line them up with the telescope's frames, the forecast and the labels
                QDir ad(dir + "/allsky"); ad.mkpath(".");
                const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmm");
                const QStringList have = ad.entryList({"*.jpg"}, QDir::Files, QDir::Name);
                if (have.isEmpty() || QDateTime::fromString(have.last().left(13), "yyyyMMdd_HHmm").secsTo(QDateTime::currentDateTimeUtc()) >= 600) QFile::copy(img, ad.filePath(stamp + ".jpg"));
                for (const QString& n : have) if (QDateTime::fromString(n.left(13), "yyyyMMdd_HHmm").secsTo(QDateTime::currentDateTimeUtc()) > 400 * 86400LL) ad.remove(n);
            }
        } else rep->abort();
        rep->deleteLater();
    }
    QFile f(img); if (!f.open(QIODevice::ReadOnly)) return {{"ok", false}, {"error", "all-sky image unavailable"}};
    const QDateTime mod = QDateTime::fromString(m["modified"].toString(), Qt::ISODate);
    if (!mod.isValid() || mod.secsTo(QDateTime::currentDateTimeUtc()) > 1200) return {{"ok", false}, {"error", "all-sky image is stale"}};
    int w = 0, h = 0; std::vector<unsigned char> px;
    if (!decodeGray(f.readAll(), &w, &h, &px)) return {{"ok", false}, {"error", "all-sky image unreadable"}};
    std::vector<unsigned char> v; const double cx = w / 2.0, cy = h / 2.0, rmax = 0.75 * w / 2.0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (std::hypot(x - cx, y - cy) < rmax) v.push_back(px[size_t(y) * w + x]);
    if (v.size() < 1000) return {{"ok", false}, {"error", "all-sky image has an unexpected size"}};
    std::vector<unsigned char> sorted = v; std::sort(sorted.begin(), sorted.end());
    const double p20 = sorted[sorted.size() / 5], med = sorted[sorted.size() / 2];
    if (med < 45) return {{"ok", false}, {"error", "all-sky image too dark for the daytime method"}, {"median", med}};
    // The camera auto-exposes and cloud contrast falls as the light does, so the margin over clear sky shrinks in twilight: 25 grey levels by day, down to ~9 at the Sun -5 deg
    // (provisional, fitted to two labelled frames: 16:23 BST about 30% cloud and 18:44 BST overcast, 2026-10-11).
    const double margin = 25.0 * std::clamp((sunAlt + 8.0) / 8.0, 0.35, 1.0);
    const double thr = std::min(p20, 90.0) + margin; size_t n = 0; for (unsigned char c : v) if (c > thr) ++n;
    *cover = double(n) / v.size(); *when = mod;
    return {{"ok", true}, {"cover", *cover}, {"median", med}, {"p20", p20}, {"margin", margin}, {"sun_alt", sunAlt}, {"image_time", mod.toString(Qt::ISODate)}};
}

QJsonObject metarNowcast(double lat, double lon, const QString& cacheDir, int maxAgeMin, QVector<Hour>* hours)
{
    // reports from the last 3 hours of stations in a box around the site (aviationweather.gov, free, no key); cached so repeated calls are cheap
    const QString path = QString("%1/metar_%2_%3.json").arg(cacheDir).arg(lat, 0, 'f', 2).arg(lon, 0, 'f', 2);
    QJsonArray reports; qint64 fetchedMs = 0;
    QFile cf(path);
    if (cf.open(QIODevice::ReadOnly)) { const QJsonObject c = QJsonDocument::fromJson(cf.readAll()).object(); reports = c["reports"].toArray(); fetchedMs = qint64(c["fetched_ms"].toDouble()); cf.close(); }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!fetchedMs || nowMs - fetchedMs > qint64(maxAgeMin) * 60000) {
        QUrl url("https://aviationweather.gov/api/data/metar"); QUrlQuery q;
        q.addQueryItem("bbox", QString("%1,%2,%3,%4").arg(lat - 0.7, 0, 'f', 2).arg(lon - 1.1, 0, 'f', 2).arg(lat + 0.7, 0, 'f', 2).arg(lon + 1.1, 0, 'f', 2));
        q.addQueryItem("format", "json"); q.addQueryItem("hours", "3");
        url.setQuery(q);
        QString err; const QJsonArray fresh = QJsonDocument::fromJson(httpGet(url, &err)).array();
        if (err.isEmpty() && !fresh.isEmpty()) {
            reports = fresh; fetchedMs = nowMs;
            QFile f(path); if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"fetched_ms", double(nowMs)}, {"reports", reports}}).toJson(QJsonDocument::Compact));
            QFile hf(cacheDir + "/metar_history.jsonl"); if (hf.open(QIODevice::Append)) hf.write(QJsonDocument(QJsonObject{{"fetched_ms", double(nowMs)}, {"reports", reports}}).toJson(QJsonDocument::Compact) + "\n");   // every fetch is kept: observed cover history to set against the forecast
        }
    }
    if (reports.isEmpty() || nowMs - fetchedMs > 3 * 3600 * 1000LL) return {{"ok", false}, {"error", "no METAR reports"}};

    // latest report per station within 60 km and 90 minutes; cover from the highest-covering layer below 20000 ft (automatic stations report to 12000 ft, so thin high cloud is invisible)
    auto oktas = [](const QString& c) { return c == "OVC" || c == "VV" ? 1.0 : c == "BKN" ? 0.75 : c == "SCT" ? 0.44 : c == "FEW" ? 0.19 : 0.0; };
    QMap<QString, QJsonObject> latest;
    for (const QJsonValue& v : reports) {
        const QJsonObject r = v.toObject(); const QString id = r["icaoId"].toString();
        if (!r.contains("obsTime") || (latest.contains(id) && latest[id]["obsTime"].toDouble() >= r["obsTime"].toDouble())) continue;
        latest[id] = r;
    }
    const double kmLat = 111.2, kmLon = 111.2 * std::cos(lat * 3.14159265358979 / 180);
    double sw = 0, sc = 0; QDateTime newest; QJsonArray st;
    for (const QJsonObject& r : latest) {
        const QDateTime t = QDateTime::fromSecsSinceEpoch(qint64(r["obsTime"].toDouble()), QTimeZone::UTC);
        const double d = std::hypot((r["lat"].toDouble() - lat) * kmLat, (r["lon"].toDouble() - lon) * kmLon);
        if (d > 60 || t.secsTo(QDateTime::currentDateTimeUtc()) > 5400) continue;
        double c = 0; for (const QJsonValue& l : r["clouds"].toArray()) if (l.toObject()["base"].isNull() || l.toObject()["base"].toDouble() < 20000) c = std::max(c, oktas(l.toObject()["cover"].toString()));
        const double w = 1.0 / ((d + 10) * (d + 10));
        sw += w; sc += w * c; if (!newest.isValid() || t > newest) newest = t;
        st.append(QJsonObject{{"id", r["icaoId"]}, {"km", std::round(d)}, {"cover", c}, {"raw", r["rawOb"]}});
    }
    // a sky report from the observer at the site (sky-report command) outweighs every airport
    QFile sf(cacheDir + "/sky_report.json");
    if (sf.open(QIODevice::ReadOnly)) {
        const QJsonObject r = QJsonDocument::fromJson(sf.readAll()).object(); sf.close();
        const QDateTime t = QDateTime::fromString(r["time"].toString(), Qt::ISODate);
        if (t.isValid() && t.secsTo(QDateTime::currentDateTimeUtc()) < 5400) {
            const double w = 1.0;                                    // the observer looking at the sky outweighs the camera (0.1) and every airport (under 0.01 each)
            sw += w; sc += w * r["cover_pct"].toDouble() / 100; if (!newest.isValid() || t > newest) newest = t;
            st.append(QJsonObject{{"id", "observer"}, {"km", 0}, {"cover", r["cover_pct"].toDouble() / 100}, {"raw", "sky-report"}});
        }
    }
    { double ac = 0; QDateTime at; const double sunAlt = sunAltitudeDeg(lat, lon, QDateTime::currentDateTimeUtc());
      const QJsonObject a = allSkyCover(cacheDir, &ac, &at, sunAlt);
      // the camera is trusted fully by day; in twilight its auto-exposed picture is a weaker guide, so its weight falls to 15% by the Sun -6 deg (the airports and models count for more)
      const double wCam = 0.1 * std::clamp((sunAlt + 6.0) / 6.0, 0.15, 1.0);
      if (a["ok"].toBool()) { sw += wCam; sc += wCam * ac; if (!newest.isValid() || at > newest) newest = at; st.append(QJsonObject{{"id", "allsky"}, {"km", 0}, {"cover", ac}, {"raw", "IoA all-sky camera, " + at.toString(Qt::ISODate)}}); }
      else st.append(QJsonObject{{"id", "allsky"}, {"error", a["error"]}}); }
    if (sw <= 0) return {{"ok", false}, {"error", "no recent METAR within 60 km"}};
    const double obs = sc / sw * 100;   // % low/mid cover now

    if (hours && !hours->isEmpty()) {
        // models' own low+mid cover at the observation hour decides their weight (Gaussian, sigma 25 points), fading over ~3 h
        const Hour* ref = at(*hours, newest);
        QMap<QString, double> g;
        if (ref) for (auto it = ref->layers.constBegin(); it != ref->layers.constEnd(); ++it) {
            const double mc = 100 * (1 - (1 - it.value()[0] / 100) * (1 - it.value()[1] / 100)), z = (mc - obs) / 25;
            g[it.key()] = std::max(0.03, std::exp(-0.5 * z * z));
        }
        if (!g.isEmpty()) for (Hour& h : *hours) {
            const double lead = std::max(0.0, (h.utc.addSecs(1800).toSecsSinceEpoch() - newest.toSecsSinceEpoch()) / 3600.0);
            const double fade = std::exp(-lead / 3.0);
            if (fade < 0.05 || h.layers.isEmpty()) continue;
            double sw2 = 0, q = 0, lo = 0, mi = 0, hi = 0, tc = 0;
            for (auto it = h.layers.constBegin(); it != h.layers.constEnd(); ++it) {
                const double w = fade * g.value(it.key(), 1.0) + (1 - fade);
                Hour y = h; y.low = it.value()[0]; y.mid = it.value()[1]; y.high = it.value()[2];
                sw2 += w; q += w * quality(y); lo += w * y.low; mi += w * y.mid; hi += w * y.high; tc += w * it.value()[3];
            }
            if (sw2 <= 0) continue;
            h.qModel = h.quality;
            // persistence: the models' re-weighted low+mid cover is also pulled towards what is observed now (90% at lead 0, fading over ~3 h), since a cloud field
            // seen overhead now is a better guide to the next hour or two than any model; the layers are scaled and the quality follows
            Hour a = h; a.low = lo / sw2; a.mid = mi / sw2; a.high = hi / sw2;
            const double mc = 100 * (1 - (1 - a.low / 100) * (1 - a.mid / 100)), mc2 = mc + 0.9 * fade * (obs - mc), k = mc > 0.5 ? std::max(0.0, mc2) / mc : 1.0;
            Hour b = a; b.low = std::min(100.0, a.low * k); b.mid = std::min(100.0, a.mid * k);
            const double qa = quality(a), qb = quality(b);
            h.quality = std::max(0.0, std::min(1.0, qa > 0.01 ? (q / sw2) * qb / qa : qb));
            h.low = b.low; h.mid = b.mid; h.high = b.high; h.cloud = std::max(0.0, tc / sw2 + 0.9 * fade * (obs - tc / sw2)); /* the camera sees all cloud, so the total goes to it directly */ h.obsWeight = fade;
        }
    }
    return {{"ok", true}, {"cover_pct", std::round(obs)}, {"obs_time", newest.toString(Qt::ISODate)}, {"stations", st}};
}

QVector<QPair<QDateTime, double>> nowcast(double lat, double lon, int hours)
{
    QVector<QPair<QDateTime, double>> out;
    QUrl url("https://api.open-meteo.com/v1/forecast"); QUrlQuery q;
    q.addQueryItem("latitude", QString::number(lat, 'f', 4)); q.addQueryItem("longitude", QString::number(lon, 'f', 4));
    q.addQueryItem("minutely_15", "precipitation"); q.addQueryItem("forecast_minutely_15", QString::number(std::max(1, hours) * 4 + 4)); q.addQueryItem("timezone", "UTC");
    url.setQuery(q);
    QNetworkAccessManager nam; QNetworkReply* rep = nam.get(QNetworkRequest(url));
    QEventLoop loop; QTimer::singleShot(15000, &loop, &QEventLoop::quit); QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
    if (rep->isFinished() && rep->error() == QNetworkReply::NoError) {
        const QJsonObject m = QJsonDocument::fromJson(rep->readAll()).object()["minutely_15"].toObject();
        const QJsonArray t = m["time"].toArray(), p = m["precipitation"].toArray();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (int i = 0; i < t.size() && i < p.size(); ++i) {
            QDateTime d = QDateTime::fromString(t[i].toString(), "yyyy-MM-ddTHH:mm"); d.setTimeZone(QTimeZone::UTC);
            if (d >= now.addSecs(-900) && d <= now.addSecs(hours * 3600)) out.append({d, p[i].toDouble()});
        }
    } else rep->abort();
    rep->deleteLater();
    return out;
}

const Hour* at(const QVector<Hour>& hours, const QDateTime& utc)
{
    const Hour* best = nullptr; qint64 bestD = 1800 + 1;
    for (const Hour& h : hours) { const qint64 d = std::llabs(h.utc.secsTo(utc.toUTC())); if (d < bestD) { bestD = d; best = &h; } }
    return best;
}

}   // namespace Weather
