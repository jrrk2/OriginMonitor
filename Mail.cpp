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
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
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
// RFC 2047 encoded-word for a header value that is not plain ASCII
QByteArray headerText(const QString& s)
{
    const QByteArray u = s.toUtf8();
    for (char c : u) if (uchar(c) < 0x20 || uchar(c) > 0x7e) return "=?UTF-8?B?" + u.toBase64() + "?=";
    return u;
}

// the briefing as an RFC 5322 message: UTF-8 text, base64 body (safe for any content and line length), CRLF line ends
QByteArray smtpMessage(const QString& fromName, const QString& from, const QString& to, const QString& subject, const QString& body)
{
    const QString domain = from.section('@', 1);
    QByteArray m;
    m += "From: " + headerText(fromName) + " <" + from.toUtf8() + ">\r\n";
    m += "To: <" + to.toUtf8() + ">\r\n";
    m += "Subject: " + headerText(subject) + "\r\n";
    m += "Date: " + QDateTime::currentDateTime().toString(Qt::RFC2822Date).toUtf8() + "\r\n";
    m += "Message-ID: <" + QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8() + "@" + domain.toUtf8() + ">\r\n";
    m += "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
    const QByteArray b = body.toUtf8().toBase64();
    for (int i = 0; i < b.size(); i += 76) m += b.mid(i, 76) + "\r\n";
    return m;
}

QJsonObject sendSmtp(const QJsonObject& smtp, const QString& to, const QString& subject, const QString& body)
{
    const QString url = smtp["url"].toString(), from = smtp["from"].toString();
    if (url.isEmpty() || !from.contains('@')) return {{"ok", false}, {"error", "smtp in " + configPath() + " needs \"url\" and \"from\""}};
    QStringList args{"-sS", "--max-time", "60", "--ssl-reqd", "--url", url, "--mail-from", from, "--mail-rcpt", to, "--upload-file", "-"};
    if (!smtp["user"].toString().isEmpty()) {
        QFile pf(dir() + "/" + smtp["password_file"].toString("smtp.password"));
        if (!pf.open(QIODevice::ReadOnly)) return {{"ok", false}, {"error", "cannot read " + pf.fileName()}};
        args << "--user" << smtp["user"].toString() + ":" + QString::fromUtf8(pf.readAll()).trimmed();   // trimmed: the file's newline is not part of it
    }
    QProcess curl;
    curl.start("curl", args);
    if (!curl.waitForStarted(10000)) return {{"ok", false}, {"error", "cannot run curl: " + curl.errorString()}};
    curl.write(smtpMessage(smtp["from_name"].toString("Origin observatory"), from, to, subject, body));
    curl.closeWriteChannel();
    if (!curl.waitForFinished(90000)) { curl.kill(); return {{"ok", false}, {"error", "curl timed out sending to " + url}}; }
    if (curl.exitStatus() == QProcess::NormalExit && curl.exitCode() == 0) return {{"ok", true}, {"to", to}, {"subject", subject}, {"via", url}};
    return {{"ok", false}, {"error", QString("SMTP send failed (curl exit %1): %2").arg(curl.exitCode()).arg(QString::fromUtf8(curl.readAllStandardError()).trimmed())}};
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
    const QString to = toIn.isEmpty() ? cfg["to"].toString() : toIn;
    if (cfg["smtp"].isObject()) {
        if (to.isEmpty()) return {{"ok", false}, {"error", "no recipient: set \"to\" in " + configPath()}};
        return sendSmtp(cfg["smtp"].toObject(), to, subject, body);
    }
    if (cfg["client_id"].toString().isEmpty()) return {{"ok", false}, {"error", "mail is not configured: no smtp or client_id in " + configPath()}};
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
