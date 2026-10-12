#pragma once

// A minimal read-only web server for OriginWatch: HTTP, or HTTPS when a certificate and key are given (QSslServer, Qt 6.4+). GET only, one request per connection.
// Access is limited to private-network peers (loopback, 10/8, 172.16/12, 192.168/16, link-local, fc00::/7), so a router that forwards the port by mistake does not publish it,
// and optionally to a shared token (?t=TOKEN once, then a cookie). It never writes anything and never talks to the telescope: the handler only shows what the OriginWatch window shows.

#include <QFile>
#include <QHostAddress>
#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

struct WebReply {
    int status = 200;
    QByteArray type = "text/html; charset=utf-8";
    QByteArray body;
};

class WebServer : public QObject {
public:
    using Handler = std::function<WebReply(const QString& path, const QUrlQuery& query)>;
    WebServer(Handler h, const QString& token, QObject* parent = nullptr) : QObject(parent), m_handler(std::move(h)), m_token(token.toUtf8()) {}

    // certFile/keyFile empty: plain HTTP. Returns false and says why in *err.
    bool start(quint16 port, const QString& certFile, const QString& keyFile, const QHostAddress& bind, QString* err)
    {
        if (certFile.isEmpty() != keyFile.isEmpty()) { *err = "give both --web-cert and --web-key"; return false; }
        if (!certFile.isEmpty()) {
            QFile cf(certFile), kf(keyFile);
            if (!cf.open(QIODevice::ReadOnly) || !kf.open(QIODevice::ReadOnly)) { *err = "cannot read the certificate or key file"; return false; }
            const QList<QSslCertificate> chain = QSslCertificate::fromData(cf.readAll(), QSsl::Pem);            // leaf first, then any intermediates
            QSslKey key(kf.readAll(), QSsl::Rsa, QSsl::Pem); if (key.isNull()) { kf.seek(0); key = QSslKey(kf.readAll(), QSsl::Ec, QSsl::Pem); }
            if (chain.isEmpty() || key.isNull()) { *err = "the certificate or key is not valid PEM (RSA or EC)"; return false; }
            QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
            cfg.setLocalCertificateChain(chain); cfg.setPrivateKey(key); cfg.setProtocol(QSsl::TlsV1_2OrLater); cfg.setPeerVerifyMode(QSslSocket::VerifyNone);      // never ask the browser for a client certificate
            auto* s = new QSslServer(this); s->setSslConfiguration(cfg); m_server = s; m_tls = true;
        } else m_server = new QTcpServer(this);
        if (!m_server->listen(bind, port)) { *err = m_server->errorString(); return false; }
        connect(m_server, &QTcpServer::newConnection, this, &WebServer::onConnection);
        connect(m_server, &QTcpServer::pendingConnectionAvailable, this, &WebServer::onConnection);        // QSslServer queues a socket only after the TLS handshake
        return true;
    }
    bool tls() const { return m_tls; }
    quint16 port() const { return m_server ? m_server->serverPort() : 0; }

    static bool privatePeer(const QHostAddress& a0)
    {
        QHostAddress a = a0;
        if (a.protocol() == QAbstractSocket::IPv6Protocol) { bool ok = false; const quint32 v4 = a.toIPv4Address(&ok); if (ok) a = QHostAddress(v4); }               // ::ffff:a.b.c.d
        if (a.isLoopback()) return true;
        if (a.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 v = a.toIPv4Address();
            return (v >> 24) == 10 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 16) == 0xA9FE;                  // 10/8, 172.16/12, 192.168/16, 169.254/16
        }
        return a.isInSubnet(QHostAddress("fc00::"), 7) || a.isInSubnet(QHostAddress("fe80::"), 10);
    }

private:
    void onConnection()
    {
        while (QTcpSocket* s = m_server->nextPendingConnection()) {
            if (!privatePeer(s->peerAddress())) { s->abort(); s->deleteLater(); continue; }
            auto* buf = new QByteArray; auto* timer = new QTimer(s); timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, s, [s] { s->abort(); });
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QObject::destroyed, [buf] { delete buf; });
            auto pump = [this, s, buf, timer] {
                buf->append(s->readAll());
                if (buf->size() > 8192) { s->abort(); return; }
                if (!buf->contains("\r\n\r\n")) return;
                timer->stop(); respond(s, *buf); buf->clear();
            };
            connect(s, &QTcpSocket::readyRead, this, pump);
            QTimer::singleShot(0, s, pump);                    // an SSL socket may already hold the request: its readyRead came before we connected
            timer->start(10000);
        }
    }
    void respond(QTcpSocket* s, const QByteArray& req)
    {
        const QList<QByteArray> lines = req.split('\n'); const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        WebReply r; QByteArray extra;
        if (first.size() < 2 || first[0] != "GET") { r.status = 405; r.type = "text/plain"; r.body = "read-only: GET only\n"; }
        else {
            const QUrl url = QUrl::fromEncoded(first[1]); const QUrlQuery q(url);
            bool ok = m_token.isEmpty();
            if (!ok && q.queryItemValue("t").toUtf8() == m_token) { ok = true; extra = "Set-Cookie: t=" + m_token + "; Path=/; HttpOnly; SameSite=Strict" + (m_tls ? "; Secure" : "") + "\r\n"; }
            for (const QByteArray& l : lines) if (!ok && l.trimmed().toLower().startsWith("cookie:") && l.contains("t=" + m_token)) ok = true;
            if (!ok) { r.status = 403; r.type = "text/plain"; r.body = "forbidden: open the address with ?t=TOKEN once\n"; }
            else r = m_handler(url.path(), q);
        }
        const char* reason = r.status == 200 ? "OK" : r.status == 403 ? "Forbidden" : r.status == 404 ? "Not Found" : r.status == 405 ? "Method Not Allowed" : "Error";
        s->write("HTTP/1.1 " + QByteArray::number(r.status) + " " + reason + "\r\nContent-Type: " + r.type + "\r\nContent-Length: " + QByteArray::number(r.body.size()) +
                 "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n" + extra + "\r\n" + r.body);
        s->disconnectFromHost();
    }
    Handler m_handler; QByteArray m_token; QTcpServer* m_server = nullptr; bool m_tls = false;
};
