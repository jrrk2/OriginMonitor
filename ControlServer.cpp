#include "ControlServer.hpp"
#include "OriginBackend.hpp"
#include "DataDir.hpp"
#include "Sequencer.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>

ControlServer::ControlServer(OriginBackend* backend, bool allowRaw, QObject* parent)
    : QObject(parent), m_backend(backend), m_seq(new Sequencer(backend, this)), m_allowRaw(allowRaw)
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
        connect(s, &QTcpSocket::disconnected, this, [this, s]() { m_buffers.remove(s); m_busy.remove(s); s->deleteLater(); });
    }
}

void ControlServer::onReadyRead()
{
    // handle() can run nested event loops (snapshot, goto, initialize) during which the client may hang up and the socket be deleted,
    // or send another line; so hold a QPointer, and let the call already in progress for this socket consume any lines that arrive meanwhile.
    QPointer<QTcpSocket> s = qobject_cast<QTcpSocket*>(sender());
    if (!s) return;
    m_buffers[s.data()] += s->readAll();
    if (m_busy.contains(s.data())) return;
    m_busy.insert(s.data());
    QTcpSocket* const key = s.data();
    int nl;
    while (s && (nl = m_buffers.value(key).indexOf('\n')) >= 0) {
        QByteArray line = m_buffers[key].left(nl);
        m_buffers[key].remove(0, nl + 1);
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        QJsonObject reply = doc.isObject() ? handle(doc.object()) : fail("invalid JSON: " + err.errorString());
        if (!s) break;                                  // client went away while the command ran
        s->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + "\n");
    }
    m_busy.remove(key);
}

void ControlServer::shutdown()
{
    if (m_seq->active()) m_seq->stop("app quitting");
    m_backend->sky().saveStack();
    QTimer::singleShot(100, qApp, &QCoreApplication::quit);
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

// TaskController status arrives as notifications (a GetStatus request is not answered), so return the newest one received
QJsonObject ControlServer::queryTaskController(int)
{
    return m_last.value("TaskController/GetStatus");
}

// RunInitialize on the TaskController with an explicit site and zone ({"lat_deg","lon_deg","timezone","fake","wait","timeout"}).
// The mount moves (it homes and aligns), so this is a deliberate action. Returns "outcome" (initialized, rejected, failed, timeout, started)
// and "summary" in words; "steps" lists the distinct InitializationInfo.CurrentStep values seen, in order.
QJsonObject ControlServer::doInitialize(const QJsonObject& req)
{
    if (!m_backend->isConnected()) return fail("not connected");
    const double lat = req["lat_deg"].toDouble(52.2054), lon = req["lon_deg"].toDouble(0.1218);
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return fail("site out of range");
    const QString zoneId = req["timezone"].toString("Europe/London");
    const QTimeZone zone(zoneId.toUtf8());
    if (!zone.isValid()) return fail("unknown timezone: " + zoneId);
    const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(zone);     // wall-clock time in the stated zone, as the GUI sends it

    QJsonObject params{{"Date", now.toString("dd MM yyyy")}, {"Time", now.toString("HH:mm:ss")}, {"TimeZone", zoneId},
                       {"Latitude", lat * 3.14159265358979323846 / 180.0}, {"Longitude", lon * 3.14159265358979323846 / 180.0},
                       {"FakeInitialize", req["fake"].toBool(false)}};
    const QJsonObject before = queryTaskController();
    m_backend->sendCommand("RunInitialize", "TaskController", params);
    const QJsonObject ack = waitResponse(m_backend->lastSequenceId(), 8000);
    QJsonObject r{{"ok", false}, {"sent", params}, {"state_before", before["State"].toString()}};
    if (ack.isEmpty()) { r["outcome"] = "rejected"; r["summary"] = "no acknowledgement from RunInitialize"; r["error"] = r["summary"]; return r; }
    r["ack"] = ack;
    if (ack["ErrorCode"].toInt() != 0) {
        r["outcome"] = "rejected"; r["error"] = ack["ErrorMessage"].toString();
        r["summary"] = QString("telescope refused RunInitialize (code %1): %2").arg(ack["ErrorCode"].toInt()).arg(ack["ErrorMessage"].toString());
        return r;
    }
    if (!req["wait"].toBool(true)) { r["ok"] = true; r["outcome"] = "started"; r["summary"] = "RunInitialize accepted; not waiting"; return r; }

    QElapsedTimer t; t.start();
    const int timeoutMs = req["timeout"].toInt(300) * 1000;
    QJsonArray steps; QString lastStep, state, stage, errorText; int pct = 0; bool sawInit = false;
    QJsonObject cur;
    while (t.elapsed() < timeoutMs) {
        QEventLoop loop; QTimer::singleShot(2000, &loop, &QEventLoop::quit); loop.exec();
        cur = queryTaskController();
        if (cur.isEmpty() || cur["State"].toString().isEmpty()) continue;       // no usable reply while the mount is busy: keep waiting
        state = cur["State"].toString(); stage = cur["Stage"].toString();
        const QJsonObject info = cur["InitializationInfo"].toObject();
        const QString step = info["CurrentStep"].toString();
        if (!step.isEmpty() && step != lastStep) { steps.append(step); lastStep = step; }
        if (info.contains("PercentageComplete")) pct = info["PercentageComplete"].toInt();
        if (state == "INITIALIZING") sawInit = true;
        if (cur.contains("ErrorMessage") && !cur["ErrorMessage"].toString().isEmpty()) { errorText = cur["ErrorMessage"].toString(); break; }
        // finished: left INITIALIZING (after having been in it, or 15 s in with the state already settled) and the task controller is idle and ready
        if (state != "INITIALIZING" && (sawInit || t.elapsed() > 15000)) break;
    }
    r["state"] = state; r["stage"] = stage; r["is_ready"] = cur["IsReady"].toBool(); r["percent"] = pct; r["steps"] = steps; r["elapsed_s"] = int(t.elapsed() / 1000);
    if (!errorText.isEmpty()) {
        r["outcome"] = "failed"; r["error"] = errorText;
        r["summary"] = QString("initialisation failed during %1: %2").arg(lastStep.isEmpty() ? "an unknown step" : lastStep, errorText);
    } else if (state == "INITIALIZING" || cur.isEmpty()) {
        r["outcome"] = "timeout"; r["summary"] = QString("still %1 after %2 s (step %3, %4%)").arg(cur.isEmpty() ? "silent" : "INITIALIZING").arg(timeoutMs / 1000).arg(lastStep.isEmpty() ? "?" : lastStep).arg(pct);
    } else if (cur["IsReady"].toBool() || state == "IDLE") {
        r["ok"] = true; r["outcome"] = "initialized";
        r["summary"] = QString("initialised in %1 s; state %2%3, ready %4").arg(int(t.elapsed() / 1000)).arg(state, stage.isEmpty() ? "" : "/" + stage, cur["IsReady"].toBool() ? "yes" : "no");
    } else {
        r["outcome"] = "failed"; r["summary"] = QString("initialisation ended in unexpected state %1/%2 (not ready)").arg(state, stage);
    }
    return r;
}

QJsonObject ControlServer::doSnapshot(const QJsonObject& req)
{
    if (!m_backend->isConnected()) return fail("not connected");
    QString dir = req["dir"].toString(originDataDir());
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

int ControlServer::cacheTtlMs(const QString& cmd)
{
    static const QHash<QString, int> ttl = {{"status", 2000}, {"mount_status", 2000}, {"focuser_status", 2000}, {"focuser_settings", 30000}, {"focus_status", 1000},
                                            {"analysis", 1000}, {"events", 1000}, {"sky_map", 30000}, {"target", 2000}};
    return ttl.value(cmd, 0);
}

QJsonObject ControlServer::handle(const QJsonObject& req)
{
    const QString cmd = req["cmd"].toString();
    if (cmd == "cache") {
        if (req["clear"].toBool()) m_cache.clear();
        return QJsonObject{{"ok", true}, {"enabled", m_cacheOn}, {"entries", m_cache.size()}, {"hits", double(m_hits)}, {"misses", double(m_misses)}};
    }
    if (cmd == "commands" || cmd == "help")
        return QJsonObject{{"ok", true}, {"note", "server commands; the OriginHeadless CLI has a fuller catalogue: OriginHeadless commands"},
                           {"commands", QJsonArray{"ping", "connect", "status", "manual", "auto", "set", "snapshot", "mount_status", "goto", "focus_status", "analysis", "events", "target", "stack",
                                                   "sky_map", "preview", "monitor", "focuser_status", "focuser_move", "focuser_settings", "initialize", "sequence", "send", "cache", "quit"}}};
    int ttl = cacheTtlMs(cmd);
    if (cmd == "target" && req.contains("name")) ttl = 0;                       // that form changes the target
    if (!m_cacheOn || ttl == 0) {
        const bool readOnly = cmd == "ping" || cmd == "preview" || (cmd == "sequence" && req["action"].toString("status") == "status");
        if (!readOnly) m_cache.clear();                                      // anything that may change state invalidates what was cached
        return handleUncached(req);
    }
    QJsonObject keyObj = req; keyObj.remove("fresh");
    const QByteArray key = QJsonDocument(keyObj).toJson(QJsonDocument::Compact);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!req["fresh"].toBool()) {
        const auto it = m_cache.constFind(key);
        if (it != m_cache.constEnd() && now - it->atMs < ttl) {
            ++m_hits;
            QJsonObject r = it->reply; r["cached"] = true; r["age_ms"] = double(now - it->atMs);
            return r;
        }
    }
    ++m_misses;
    QJsonObject r = handleUncached(req);
    if (r["ok"].toBool()) m_cache[key] = {r, QDateTime::currentMSecsSinceEpoch()};
    r["cached"] = false;
    return r;
}

QJsonObject ControlServer::handleUncached(const QJsonObject& req)
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
    if (cmd == "preview") {                           // render the live stack to a PNG without saving the stack state (read-only; for monitors)
        SkyMonitor& sky = m_backend->sky();
        QJsonObject r{{"ok", true}, {"stack", sky.stackSummary()}, {"target", sky.targetName()}};
        const Image16 img = sky.stacker().preview();
        if (img.isNull()) { r["note"] = "the live stack is empty"; return r; }
        const QString dir = originDataDir() + "/previews"; QDir().mkpath(dir);
        const QString path = dir + "/live_preview.png";
        if (!img.savePng(path)) return fail("could not write " + path);
        r["preview"] = path; r["rendered"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
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
    // the sequencer owns the camera and the mount while it runs
    if (m_seq->active() && (cmd == "snapshot" || cmd == "goto" || cmd == "focuser_move" || cmd == "initialize" || cmd == "set"))
        return fail("a sequence is running; stop it first ({\"cmd\":\"sequence\",\"action\":\"stop\"})");
    if (cmd == "sequence") {                          // action start|stop|status; see Sequencer.hpp
        const QString action = req["action"].toString("status");
        if (action == "start") return m_seq->start(req);
        if (action == "stop") return m_seq->stop(req["reason"].toString("stopped by supervisor"));
        return QJsonObject{{"ok", true}, {"status", m_seq->status()}};
    }
    if (cmd == "goto") return doGoto(req);
    if (cmd == "initialize") return doInitialize(req);

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

    if (cmd == "quit") { shutdown(); return QJsonObject{{"ok", true}}; }

    return fail("unknown command: " + cmd);
}
