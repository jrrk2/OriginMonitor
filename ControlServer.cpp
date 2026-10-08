#include "ControlServer.hpp"
#include "OriginBackend.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

ControlServer::ControlServer(OriginBackend* backend, bool allowRaw, QObject* parent)
    : QObject(parent), m_backend(backend), m_allowRaw(allowRaw)
{
    connect(&m_server, &QTcpServer::newConnection, this, &ControlServer::onNewConnection);
    connect(m_backend, &OriginBackend::messageReceived, this, [this](const QJsonObject& m) {
        m_last[m["Source"].toString() + "/" + m["Command"].toString()] = m;
        if (m["Type"].toString() == "Response")
            m_responses[m["SequenceID"].toInt()] = m;
        if (m["Source"].toString() == "Focuser" && m.contains("CalibrationLowerLimit")) {
            m_focuserLower = m["CalibrationLowerLimit"].toInt(m_focuserLower);
            m_focuserUpper = m["CalibrationUpperLimit"].toInt(m_focuserUpper);
        }
    });
}

bool ControlServer::listen(quint16 port)
{
    return m_server.listen(QHostAddress::LocalHost, port);   // loopback only
}

void ControlServer::onNewConnection()
{
    while (QTcpSocket* s = m_server.nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, this, &ControlServer::onReadyRead);
        connect(s, &QTcpSocket::disconnected, this, [this, s]() { m_buffers.remove(s); s->deleteLater(); });
    }
}

void ControlServer::onReadyRead()
{
    QTcpSocket* s = qobject_cast<QTcpSocket*>(sender());
    if (!s) return;
    m_buffers[s] += s->readAll();
    int nl;
    while ((nl = m_buffers[s].indexOf('\n')) >= 0) {
        QByteArray line = m_buffers[s].left(nl);
        m_buffers[s].remove(0, nl + 1);
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        QJsonObject reply = doc.isObject() ? handle(doc.object()) : fail("invalid JSON: " + err.errorString());
        s->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + "\n");
        s->flush();
    }
}

QJsonObject ControlServer::fail(const QString& message)
{
    return QJsonObject{{"ok", false}, {"error", message}};
}

QJsonObject ControlServer::waitResponse(int sequenceId, int timeoutMs)
{
    QElapsedTimer t; t.start();
    while (!m_responses.contains(sequenceId) && t.elapsed() < timeoutMs) {
        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!m_responses.contains(sequenceId)) return QJsonObject();
    return m_responses.take(sequenceId);
}

QJsonObject ControlServer::queryFocuser(int timeoutMs)
{
    m_backend->sendCommand("GetStatus", "Focuser");
    return waitResponse(m_backend->lastSequenceId(), timeoutMs);
}

QJsonObject ControlServer::queryMount(int timeoutMs)
{
    m_backend->sendCommand("GetStatus", "Mount");
    return waitResponse(m_backend->lastSequenceId(), timeoutMs);
}

// Slew to J2000 {"ra_hours", "dec_deg"} and wait for the mount to report IsGotoOver.
QJsonObject ControlServer::doGoto(const QJsonObject& req)
{
    if (!m_backend->isConnected()) return fail("not connected");
    if (!req.contains("ra_hours") || !req.contains("dec_deg")) return fail("need 'ra_hours' and 'dec_deg'");
    double ra = req["ra_hours"].toDouble(), dec = req["dec_deg"].toDouble();
    if (ra < 0 || ra >= 24 || dec < -90 || dec > 90) return fail("coordinates out of range");
    m_backend->gotoPosition(ra, dec);
    QJsonObject ack = waitResponse(m_backend->lastSequenceId(), 5000);
    if (ack.isEmpty()) return fail("no acknowledgement from GotoRaDec");
    if (ack["ErrorCode"].toInt() != 0) return fail("goto rejected: " + ack["ErrorMessage"].toString());

    QElapsedTimer t; t.start();
    int timeoutMs = req["timeout"].toInt(180) * 1000;
    bool started = false;                      // IsGotoOver may still be true for a moment after the ack
    QJsonObject cur;
    while (t.elapsed() < timeoutMs) {
        QEventLoop loop; QTimer::singleShot(1000, &loop, &QEventLoop::quit); loop.exec();
        cur = queryMount(3000);
        if (cur.isEmpty()) continue;
        if (!cur["IsGotoOver"].toBool()) started = true;
        if ((started || t.elapsed() > 8000) && cur["IsGotoOver"].toBool())
            return QJsonObject{{"ok", true}, {"mount", cur}, {"elapsed_ms", (int)t.elapsed()}};
    }
    return QJsonObject{{"ok", false}, {"error", "goto did not complete"}, {"mount", cur}};
}

QJsonObject ControlServer::doSnapshot(const QJsonObject& req)
{
    if (!m_backend->isConnected()) return fail("not connected");
    QString dir = req["dir"].toString(QDir::homePath() + "/Pictures/CelestronOrigin");
    QDir().mkpath(dir);
    int timeoutMs = req["timeout"].toInt(30) * 1000;

    QJsonObject result; bool done = false;
    QEventLoop loop;
    auto conn = connect(m_backend, &OriginBackend::tiffImageDownloaded, this,
        [&](const QString& remote, const QByteArray& data, double ra, double dec, double exposure) {
            QString ext = QFileInfo(remote).suffix().toLower();
            if (ext.isEmpty()) ext = "tiff";
            QString path = QString("%1/snapshot_%2.%3").arg(dir,
                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"), ext);
            QFile f(path);
            if (f.open(QIODevice::WriteOnly) && f.write(data) == data.size()) {
                result = {{"ok", true}, {"path", path}, {"bytes", data.size()}, {"remote", remote},
                          {"ra_rad", ra}, {"dec_rad", dec}, {"exposure", exposure}};
            } else {
                result = fail("could not write " + path);
            }
            done = true; loop.quit();
        });
    QTimer timer; timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    if (!m_backend->takeSingleSnapshot()) { disconnect(conn); return fail("snapshot request failed"); }
    loop.exec();
    disconnect(conn);
    return done ? result : fail("timed out waiting for snapshot");
}

QJsonObject ControlServer::doFocuserMove(const QJsonObject& req)
{
    if (!m_backend->isConnected()) return fail("not connected");
    if (!req.contains("position")) return fail("missing 'position'");
    int target = req["position"].toInt();
    QJsonObject st = queryFocuser();
    if (st.isEmpty()) return fail("no focuser status");
    int current = st["Position"].toInt();
    if (st.contains("CalibrationLowerLimit")) {
        m_focuserLower = st["CalibrationLowerLimit"].toInt();
        m_focuserUpper = st["CalibrationUpperLimit"].toInt();
    }
    if (target < m_focuserLower || target > m_focuserUpper)
        return fail(QString("position %1 outside travel limits %2..%3").arg(target).arg(m_focuserLower).arg(m_focuserUpper));
    int maxStep = req["max_step"].toInt(3000);                 // guard against typos
    if (qAbs(target - current) > maxStep)
        return fail(QString("move of %1 exceeds max_step %2 (pass max_step to override)").arg(qAbs(target - current)).arg(maxStep));

    QJsonObject params; params["Value"] = target;
    m_backend->sendCommand("MoveTo", "Focuser", params);
    QJsonObject ack = waitResponse(m_backend->lastSequenceId(), 5000);
    if (ack.isEmpty()) return fail("no acknowledgement from MoveTo");
    if (ack["ErrorCode"].toInt() != 0) return fail("MoveTo rejected: " + ack["ErrorMessage"].toString());

    QElapsedTimer t; t.start();
    int timeoutMs = req["timeout"].toInt(60) * 1000;
    QJsonObject cur;
    while (t.elapsed() < timeoutMs) {
        QEventLoop loop; QTimer::singleShot(400, &loop, &QEventLoop::quit); loop.exec();
        cur = queryFocuser(3000);
        if (!cur.isEmpty() && cur["IsMoveToOver"].toBool() && qAbs(cur["Position"].toInt() - target) <= 2)
            return QJsonObject{{"ok", true}, {"from", current}, {"to", cur["Position"].toInt()}, {"elapsed_ms", (int)t.elapsed()}};
    }
    return QJsonObject{{"ok", false}, {"error", "move did not complete"}, {"from", current}, {"now", cur["Position"].toInt()}};
}

QJsonObject ControlServer::handle(const QJsonObject& req)
{
    const QString cmd = req["cmd"].toString();

    if (cmd == "ping") return QJsonObject{{"ok", true}, {"pong", true}};

    if (cmd == "connect") {
        QString host = req["host"].toString();
        if (host.isEmpty()) return fail("missing 'host'");
        bool ok = m_backend->connectToTelescope(host, req["port"].toInt(80));
        return ok ? QJsonObject{{"ok", true}, {"host", host}} : fail("connection failed");
    }

    if (cmd == "status") {
        QJsonObject r{{"ok", true}, {"connected", m_backend->isConnected()}, {"host", m_backend->connectedHost()},
                      {"exposure", m_backend->currentExposure()}, {"iso", m_backend->currentISO()},
                      {"binning", m_backend->currentBinning()}, {"snapshot_in_progress", m_backend->snapshotInProgress()}};
        QJsonObject last;
        for (auto it = m_last.constBegin(); it != m_last.constEnd(); ++it) {
            const QString& k = it.key();
            if (k == "Focuser/GetStatus" || k == "TaskController/GetStatus" || k == "Camera/GetCaptureParameters")
                last[k] = it.value();
        }
        r["last"] = last;
        return r;
    }

    if (cmd == "manual") return QJsonObject{{"ok", m_backend->setCameraManualMode()}};
    if (cmd == "auto")   return QJsonObject{{"ok", m_backend->setCameraAutoMode()}};

    if (cmd == "set") {
        if (!m_backend->isConnected()) return fail("not connected");
        if (req.contains("exposure")) m_backend->setCameraExposure(req["exposure"].toDouble());
        if (req.contains("iso"))      m_backend->setCameraISO(req["iso"].toInt());
        if (req.contains("binning"))  m_backend->setCameraBinning(req["binning"].toInt());
        return QJsonObject{{"ok", true}};
    }

    if (cmd == "snapshot") return doSnapshot(req);
    if (cmd == "analysis") {                          // latest per-frame sky analysis, plus the newest "history" records
        QJsonObject r{{"ok", true}, {"last", m_backend->lastFocus()}};
        if (req["history"].toInt() > 0) r["history"] = m_backend->focusHistory(req["history"].toInt());
        return r;
    }
    if (cmd == "events") return m_backend->eventsSince(qint64(req["since"].toDouble(0)), req["limit"].toInt(200));
    if (cmd == "target") {                            // choose the live stack; resumes a saved stack of the same name unless reset
        SkyMonitor& sky = m_backend->sky();
        if (!req.contains("name")) return QJsonObject{{"ok", true}, {"target", sky.targetName()}, {"stack", sky.stackSummary()}};
        return sky.setTarget(req["name"].toString(), req.contains("ra_hours") ? req["ra_hours"].toDouble() : -1, req.contains("dec_deg") ? req["dec_deg"].toDouble() : -999, req["reset"].toBool());
    }
    if (cmd == "stack") {                             // summary of the live stack; saves state and preview.png and returns their paths
        SkyMonitor& sky = m_backend->sky();
        QJsonObject r{{"ok", true}, {"stack", sky.stackSummary()}};
        if (sky.stacker().frames() > 0 && sky.saveStack()) r["preview"] = sky.stackDirectory() + "/preview.png";
        return r;
    }
    if (cmd == "sky_map") return m_backend->sky().obstructionMap();
    if (cmd == "monitor") {                           // site, black level, auto-stacking and the transparency floor
        SkyMonitor& sky = m_backend->sky();
        if (req.contains("lat_deg") || req.contains("lon_deg")) { Astro::Site s = sky.site(); if (req.contains("lat_deg")) s.latDeg = req["lat_deg"].toDouble(); if (req.contains("lon_deg")) s.lonDeg = req["lon_deg"].toDouble(); sky.setSite(s); }
        if (req.contains("black_level")) sky.setBlackLevel(req["black_level"].toDouble());
        if (req.contains("auto_stack")) sky.setAutoStack(req["auto_stack"].toBool());
        if (req.contains("min_transparency")) sky.setMinTransparency(req["min_transparency"].toDouble());
        return QJsonObject{{"ok", true}, {"settings", sky.settings()}};
    }
    if (cmd == "focus_status") {                      // last focus score, plus the newest "history" entries (default 0)
        QJsonObject r{{"ok", true}, {"last", m_backend->lastFocus()}};
        if (req["history"].toInt() > 0) r["history"] = m_backend->focusHistory(req["history"].toInt());
        return r;
    }

    if (cmd == "mount_status") {
        if (!m_backend->isConnected()) return fail("not connected");
        QJsonObject r = queryMount();
        return r.isEmpty() ? fail("no response") : QJsonObject{{"ok", true}, {"mount", r}};
    }
    if (cmd == "goto") return doGoto(req);

    if (cmd == "focuser_status") {
        if (!m_backend->isConnected()) return fail("not connected");
        QJsonObject r = queryFocuser();
        return r.isEmpty() ? fail("no response") : QJsonObject{{"ok", true}, {"focuser", r}};
    }
    if (cmd == "focuser_move") return doFocuserMove(req);
    if (cmd == "focuser_settings") {
        if (!m_backend->isConnected()) return fail("not connected");
        m_backend->getFocuserAdvancedSettings();
        QJsonObject r = waitResponse(m_backend->lastSequenceId(), 5000);
        return r.isEmpty() ? fail("no response") : QJsonObject{{"ok", true}, {"settings", r}};
    }

    if (cmd == "send") {
        if (!m_allowRaw) return fail("raw commands disabled (start with --control-raw)");
        if (!m_backend->isConnected()) return fail("not connected");
        m_backend->sendCommand(req["command"].toString(), req["destination"].toString(), req["params"].toObject());
        QJsonObject r = waitResponse(m_backend->lastSequenceId(), req["timeout"].toInt(10) * 1000);
        return r.isEmpty() ? fail("no response") : QJsonObject{{"ok", true}, {"response", r}};
    }

    if (cmd == "quit") { m_backend->sky().saveStack(); QTimer::singleShot(100, qApp, &QCoreApplication::quit); return QJsonObject{{"ok", true}}; }

    return fail("unknown command: " + cmd);
}
