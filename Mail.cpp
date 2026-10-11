#include "Mail.hpp"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <cstdio>

namespace Mail {

namespace {
const char* kScope = "offline_access https://graph.microsoft.com/Mail.Send";

QString dir() { return QDir::homePath() + "/.config/origin"; }
QString tokenPath() { return dir() + "/mail_token.json"; }

QJsonObject readJson(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject(); }
bool writePrivate(const QString& path, const QJsonObject& o)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(o).toJson()); return true;
}

// one blocking HTTPS request; returns {status, json}
struct Reply { int status = 0; QJsonObject json; QString error; };
Reply http(const QString& url, const QByteArray& body, const QByteArray& contentType, const QString& bearer, int timeoutS = 30)
{
    QNetworkAccessManager nam;
    QNetworkRequest rq{QUrl(url)};
    rq.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    if (!bearer.isEmpty()) rq.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    QNetworkReply* rep = nam.post(rq, body);
    QEventLoop loop; QTimer::singleShot(timeoutS * 1000, &loop, &QEventLoop::quit); QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
    Reply r;
    if (!rep->isFinished()) { r.error = "timed out"; rep->abort(); }
    else {
        r.status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.json = QJsonDocument::fromJson(rep->readAll()).object();
        if (r.status == 0) r.error = rep->errorString();
    }
    rep->deleteLater();
    return r;
}
QByteArray form(const QList<QPair<QString, QString>>& kv) { QUrlQuery q; for (const auto& p : kv) q.addQueryItem(p.first, p.second); return q.toString(QUrl::FullyEncoded).toUtf8(); }
QString errText(const Reply& r) { return !r.error.isEmpty() ? r.error : r.json["error_description"].toString(r.json["error"].isObject() ? r.json["error"].toObject()["message"].toString() : r.json["error"].toString("HTTP " + QString::number(r.status))); }
}

QString configPath() { return dir() + "/mail.conf"; }

QJsonObject login()
{
    const QJsonObject cfg = readJson(configPath());
    if (cfg["client_id"].toString().isEmpty()) return {{"ok", false}, {"error", "no client_id in " + configPath() + " (see Mail.hpp for the Entra app registration)"}};
    const QString tenant = cfg["tenant"].toString("common"), base = "https://login.microsoftonline.com/" + tenant + "/oauth2/v2.0/";
    const Reply d = http(base + "devicecode", form({{"client_id", cfg["client_id"].toString()}, {"scope", kScope}}), "application/x-www-form-urlencoded", QString());
    if (d.json["device_code"].toString().isEmpty()) return {{"ok", false}, {"error", errText(d)}};
    fprintf(stderr, "%s\n", qPrintable(d.json["message"].toString("Open " + d.json["verification_uri"].toString() + " and enter the code " + d.json["user_code"].toString())));
    fflush(stderr);
    const int interval = std::max(2, d.json["interval"].toInt(5)); QElapsedTimer t; t.start();
    while (t.elapsed() < qint64(d.json["expires_in"].toInt(900)) * 1000) {
        QThread::sleep(uint(interval));
        const Reply p = http(base + "token", form({{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}, {"client_id", cfg["client_id"].toString()}, {"device_code", d.json["device_code"].toString()}}), "application/x-www-form-urlencoded", QString());
        if (!p.json["refresh_token"].toString().isEmpty()) {
            writePrivate(tokenPath(), {{"refresh_token", p.json["refresh_token"]}, {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
            return {{"ok", true}, {"note", "signed in; refresh token saved to " + tokenPath()}};
        }
        const QString e = p.json["error"].toString();
        if (e != "authorization_pending" && e != "slow_down") return {{"ok", false}, {"error", errText(p)}};
    }
    return {{"ok", false}, {"error", "the code expired before sign-in"}};
}

QJsonObject send(const QString& subject, const QString& body, const QString& toIn)
{
    const QJsonObject cfg = readJson(configPath());
    if (cfg["client_id"].toString().isEmpty()) return {{"ok", false}, {"error", "mail is not configured: no client_id in " + configPath()}};
    const QString to = toIn.isEmpty() ? cfg["to"].toString() : toIn;
    if (to.isEmpty()) return {{"ok", false}, {"error", "no recipient: set \"to\" in " + configPath()}};
    const QString refresh = readJson(tokenPath())["refresh_token"].toString();
    if (refresh.isEmpty()) return {{"ok", false}, {"error", "not signed in: run OriginHeadless mail-login"}};
    const QString base = "https://login.microsoftonline.com/" + cfg["tenant"].toString("common") + "/oauth2/v2.0/";
    const Reply t = http(base + "token", form({{"grant_type", "refresh_token"}, {"client_id", cfg["client_id"].toString()}, {"refresh_token", refresh}, {"scope", kScope}}), "application/x-www-form-urlencoded", QString());
    if (t.json["access_token"].toString().isEmpty()) return {{"ok", false}, {"error", "token refresh failed: " + errText(t) + " (run OriginHeadless mail-login again)"}};
    if (!t.json["refresh_token"].toString().isEmpty()) writePrivate(tokenPath(), {{"refresh_token", t.json["refresh_token"]}, {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
    const QJsonObject msg{{"message", QJsonObject{{"subject", subject}, {"body", QJsonObject{{"contentType", "Text"}, {"content", body}}},
                                                   {"toRecipients", QJsonArray{QJsonObject{{"emailAddress", QJsonObject{{"address", to}}}}}}}}, {"saveToSentItems", true}};
    const Reply s = http("https://graph.microsoft.com/v1.0/me/sendMail", QJsonDocument(msg).toJson(QJsonDocument::Compact), "application/json", t.json["access_token"].toString());
    if (s.status == 202) return {{"ok", true}, {"to", to}, {"subject", subject}};
    return {{"ok", false}, {"error", QString("Graph sendMail failed (HTTP %1): %2").arg(s.status).arg(errText(s))}};
}

}   // namespace Mail
