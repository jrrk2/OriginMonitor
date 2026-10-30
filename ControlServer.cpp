#include "ControlServer.hpp"
#include <algorithm>
#include "OriginBackend.hpp"
#include "DataDir.hpp"
#include "Sequencer.hpp"
#include "Astro.hpp"
#include "SkyFrame.hpp"

#include <QCoreApplication>
#include <QDebug>
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
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>

ControlServer::ControlServer(OriginBackend* backend, bool allowRaw, QObject* parent)
    : QObject(parent), m_backend(backend), m_seq(new Sequencer(backend, this)), m_allowRaw(allowRaw)
{
    connect(&m_server, &QTcpServer::newConnection, this, &ControlServer::onNewConnection);
    connect(m_backend, &OriginBackend::messageReceived, this, [this](const QJsonObject& m) {
        m_last[m["Source"].toString() + "/" + m["Command"].toString()] = m;
        if (m["Source"].toString() == "TaskController" && !m["ErrorMessage"].toString().isEmpty()) { m_tcError = m["ErrorMessage"].toString(); m_tcErrorMs = QDateTime::currentMSecsSinceEpoch(); }
        if (m["Type"].toString() == "Response")
            m_responses[m["SequenceID"].toInt()] = m;
        if (m["Source"].toString() == "Focuser" && m.contains("CalibrationLowerLimit")) {
            m_focuserLower = m["CalibrationLowerLimit"].toInt(m_focuserLower);
            m_focuserUpper = m["CalibrationUpperLimit"].toInt(m_focuserUpper);
        }
    });
    // Reconnect a link that dropped by itself. The telescope rebooting (asked for or not) or a Wi-Fi drop used to leave the daemon disconnected until a client sent "connect"
    // (2026-10-28: after a Reboot the telescope answered pings again within 25 s and took captures, but the daemon stayed down).
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &ControlServer::tryReconnect);
    // the bookkeeping is done here, not in tryReconnect(): an attempt's socket can finish connecting after that attempt stopped waiting for it (seen 2026-10-28 13:09)
    connect(m_backend, &OriginBackend::connected, this, [this]() {
        m_reconnect.stop();
        if (m_wantLink && m_linkLost.isValid()) {
            const qint64 down = m_linkLost.secsTo(QDateTime::currentDateTimeUtc());
            m_last.clear(); m_cache.clear();            // statuses from before the drop describe a telescope that may have rebooted since
            m_backend->recordEvent("reconnected", QString("telescope link restored after %1 s (%2 attempts)").arg(down).arg(m_reconnectTries), {{"down_s", double(down)}, {"attempts", m_reconnectTries}});
            QTimer::singleShot(2000, this, [this]() { if (m_backend->isConnected()) declareConfiguredFilter(); });
        }
        m_wantLink = true; m_linkLost = QDateTime(); m_reconnectTries = 0;
    });
    connect(m_backend, &OriginBackend::disconnected, this, [this]() {
        if (!m_wantLink || m_linkLost.isValid()) return;
        m_linkLost = QDateTime::currentDateTimeUtc(); m_reconnectTries = 0;
        qInfo() << "telescope link lost; reconnecting to" << m_backend->connectedHost() << "in 5 s";
        m_reconnect.start(5000);
    });
}

void ControlServer::tryReconnect()
{
    if (!m_wantLink || m_backend->isConnected() || m_backend->connectedHost().isEmpty()) return;
    m_reconnecting = true; ++m_reconnectTries;
    const bool up = m_backend->connectToTelescope(m_backend->connectedHost(), m_backend->connectedPort());   // success is handled by the connected() handler
    m_reconnecting = false;
    if (up || m_backend->isConnected()) return;
    const qint64 down = m_linkLost.isValid() ? m_linkLost.secsTo(QDateTime::currentDateTimeUtc()) : 0;
    if (m_reconnectTries == 1 || m_reconnectTries % 10 == 0) qInfo() << "reconnect attempt" << m_reconnectTries << "failed; link down" << down << "s";
    if (m_wantLink) m_reconnect.start(down < 300 ? 15000 : 60000);
}

bool ControlServer::listen(quint16 port)
{
    return m_server.listen(QHostAddress::LocalHost, port);   // loopback only
}

void ControlServer::onNewConnection()
{
    while (QTcpSocket* s = m_server.nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, this, &ControlServer::onReadyRead);
        // A client can hang up while a command runs in a nested event loop inside onReadyRead (this socket's readyRead emission is still on the stack). Deleting the socket then
        // crashes Qt when the emission unwinds (SIGSEGV in QIODevice::channelReadyRead, seen 2026-10-11), so a busy socket is only marked and deleted when its handler is done.
        connect(s, &QTcpSocket::disconnected, this, [this, s]() { m_buffers.remove(s); if (m_busy.contains(s)) { m_closing.insert(s); return; } s->deleteLater(); });
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
    if (m_closing.remove(key) && s) s->deleteLater();       // the client left meanwhile: now it is safe
}

void ControlServer::shutdown()
{
    m_wantLink = false; m_reconnect.stop();
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

// The filter is inserted by hand (no motor), so the firmware only knows what it was told. ~/.config/origin/filter holds "nebula" or "clear"; when present the server declares it
// after every connect and before every initialize, so that plate solves and centring pick exposures for the filter that is really fitted (the Celestron app does the same).
static QString configuredFilter()
{
    QFile f(QDir::homePath() + "/.config/origin/filter");
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QString n = QString::fromUtf8(f.readAll()).trimmed().toLower();
    return (n == "clear" || n == "nebula") ? n : QString();
}

QJsonObject ControlServer::applyFilter(const QString& name, bool force)
{
    if (!m_backend->isConnected()) return fail("not connected");
    const QString declared = name == "clear" ? "Clear" : "Nebula";
    if (!force) {
        m_backend->sendCommand("GetFilter", "Camera");
        const QJsonObject cur = waitResponse(m_backend->lastSequenceId(), 5000);
        if (!cur.isEmpty() && cur["Filter"].toString() == declared) return QJsonObject{{"ok", true}, {"filter", declared}, {"set", true}, {"already", true}};
    }
    m_backend->sendCommand("SetFilter", "Camera", QJsonObject{{"Filter", declared}});
    const QJsonObject set = waitResponse(m_backend->lastSequenceId(), 30000);   // the Celestron app's capture showed a reply about 11 s after the request, but that included the user's confirmation prompt in the app: the firmware's own latency is untested
    if (set.isEmpty()) return fail("no response to SetFilter");
    if (set["ErrorCode"].toInt() != 0) return QJsonObject{{"ok", false}, {"error", set["ErrorMessage"].toString("SetFilter failed")}, {"response", set}};
    m_backend->sendCommand("GetFilter", "Camera");
    const QJsonObject r = waitResponse(m_backend->lastSequenceId(), 5000);
    if (r.isEmpty()) return fail("no response to GetFilter");
    return QJsonObject{{"ok", true}, {"filter", r["Filter"].toString()}, {"set", declared == r["Filter"].toString()}};
}

QJsonObject ControlServer::declareConfiguredFilter()
{
    const QString cf = configuredFilter();
    if (cf.isEmpty()) return QJsonObject{{"ok", true}, {"configured", false}};
    QJsonObject r = applyFilter(cf, false); r["configured"] = cf;
    qInfo() << "filter" << cf << (r["ok"].toBool() ? (r["already"].toBool() ? "already declared" : "declared") : "NOT declared: " + r["error"].toString());
    return r;
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
    const QJsonObject filterInfo = declareConfiguredFilter();       // before the mount moves and aligns: a failure is reported but does not stop the initialize
    m_backend->sendCommand("RunInitialize", "TaskController", params);
    const QJsonObject ack = waitResponse(m_backend->lastSequenceId(), 8000);
    QJsonObject r{{"ok", false}, {"sent", params}, {"state_before", before["State"].toString()}};
    if (filterInfo["configured"].isString()) r["filter"] = filterInfo;
    if (ack.isEmpty()) { r["outcome"] = "rejected"; r["summary"] = "no acknowledgement from RunInitialize"; r["error"] = r["summary"]; return r; }
    r["ack"] = ack;
    if (ack["ErrorCode"].toInt() != 0) {
        r["outcome"] = "rejected"; r["error"] = ack["ErrorMessage"].toString();
        r["summary"] = QString("telescope refused RunInitialize (code %1): %2").arg(ack["ErrorCode"].toInt()).arg(ack["ErrorMessage"].toString());
        return r;
    }
    if (!req["wait"].toBool(true)) { r["ok"] = true; r["outcome"] = "started"; r["summary"] = "RunInitialize accepted; not waiting"; return r; }

    QElapsedTimer t; t.start(); const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    const int timeoutMs = req["timeout"].toInt(300) * 1000;
    QJsonArray steps; QString lastStep, state, stage, errorText; int pct = 0; bool sawInit = false;
    QJsonObject cur;
    while (t.elapsed() < timeoutMs) {
        QEventLoop loop; QTimer::singleShot(500, &loop, &QEventLoop::quit); loop.exec();
        if (!m_tcError.isEmpty() && m_tcErrorMs >= startMs) { errorText = m_tcError; cur = queryTaskController(); state = cur["State"].toString(); stage = cur["Stage"].toString(); break; }   // an error that the next status message overwrote
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
    } else if (!req["fake"].toBool(false) && !queryMount(5000)["IsAligned"].toBool()) {
        // IDLE and not initialising is not the same as aligned: a failed alignment also ends there (seen 2026-10-11: "Did not find enough stars")
        r["outcome"] = "failed"; r["error"] = "the mount is not aligned after the initialisation"; r["summary"] = QString("initialisation ended in %1 but the mount reports IsAligned false").arg(state);
    } else if (cur["IsReady"].toBool() || state == "IDLE") {
        r["ok"] = true; r["outcome"] = "initialized";
        r["summary"] = QString("initialised in %1 s; state %2%3, ready %4").arg(int(t.elapsed() / 1000)).arg(state, stage.isEmpty() ? "" : "/" + stage, cur["IsReady"].toBool() ? "yes" : "no");
    } else {
        r["outcome"] = "failed"; r["summary"] = QString("initialisation ended in unexpected state %1/%2 (not ready)").arg(state, stage);
    }
    return r;
}

// Statistics of one raw frame, for deciding whether it is worth a plate solve (or what exposure to try next). Percentiles and a histogram of the half-resolution
// luminance (ADU, 16-bit), the saturated fraction, background and noise, the stars the finder sees, and the sky rate (ADU/s above the black level: the
// twilight brightness, which is what the exposure has to follow). "verdict": ok | too_bright | too_short | few_stars | unreadable, with a suggested exposure that puts the
// sky about 6000 ADU above black (never more than maxExposureS).
QJsonObject ControlServer::frameStats(const QString& path, double exposureS, int bins, double maxExposureS, double targetAdu)
{
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return QJsonObject{{"ok", false}, {"verdict", "unreadable"}, {"error", "cannot open " + path}};
    const SkyFrame F = analyseFrame(f.readAll());
    if (!F.valid || F.lum.empty()) return QJsonObject{{"ok", false}, {"verdict", "unreadable"}, {"error", F.note.isEmpty() ? "not a raw 16-bit FITS frame" : F.note}};
    std::vector<float> v = F.lum; std::sort(v.begin(), v.end());
    auto pct = [&](double q) { return double(v[std::min(v.size() - 1, size_t(q * (v.size() - 1)))]); };
    const double fs = 65535.0, black = 4985.0;
    size_t sat = 0; for (float x : v) if (x >= 0.95 * fs) ++sat;
    const double satFrac = double(sat) / v.size();
    QJsonArray hist; { const int n = std::max(2, std::min(bins, 256)); std::vector<int> h(n, 0); for (float x : v) ++h[std::min(n - 1, int(x / fs * n))]; for (int c : h) hist.append(double(c) / v.size()); }
    int strong = 0; for (const SkyStar& s : F.stars) if (s.sigma >= 10) ++strong;
    const StreakInfo streaks = streakInfo(F);
    double starRadius = 0; { std::vector<float> h; for (size_t i = 0; i < F.stars.size() && i < 30; ++i) h.push_back(F.stars[i].hfr); if (!h.empty()) { std::sort(h.begin(), h.end()); starRadius = h[h.size() / 2]; } }
    const double skyRate = std::max(0.0, F.background - black) / std::max(exposureS, 1e-6);
    const double suggest = std::clamp(skyRate > 0 ? targetAdu / skyRate : maxExposureS, 1e-5, maxExposureS);
    QString verdict = "ok", why;
    double suggestOut = suggest;
    if (satFrac > 0.02 || F.background > 0.7 * fs) {
        verdict = "too_bright"; why = QString("saturated: %1% of the frame is at full scale, median %2 ADU").arg(satFrac * 100, 0, 'f', 1).arg(F.background, 0, 'f', 0);
        if (satFrac > 0.5) suggestOut = std::max(1e-5, exposureS / 30);      // clipped: the true brightness is hidden, so step down hard instead of trusting the rate
    }
    else if (int(F.stars.size()) < 12 && F.background - black > 12000 && exposureS > 2e-5) { verdict = "too_bright"; why = QString("sky is %1 ADU above black and no stars show; a shorter exposure may reveal them").arg(F.background - black, 0, 'f', 0); }
    else if (int(F.stars.size()) < 12) {
        if (exposureS < maxExposureS * 0.9 && F.background - black < 2500) { verdict = "too_short"; why = QString("only %1 stars and the sky is just %2 ADU above black").arg(F.stars.size()).arg(F.background - black, 0, 'f', 0); }
        else { verdict = "few_stars"; why = QString("only %1 stars (cloud, or the sky is still too bright for the stars)").arg(F.stars.size()); }
    }
    return QJsonObject{{"ok", true}, {"verdict", verdict}, {"why", why}, {"median_adu", std::round(F.background)}, {"noise_adu", std::round(F.noise * 10) / 10}, {"black_level", black},
                       {"percentiles", QJsonObject{{"p1", pct(0.01)}, {"p5", pct(0.05)}, {"p25", pct(0.25)}, {"p50", pct(0.5)}, {"p75", pct(0.75)}, {"p95", pct(0.95)}, {"p99", pct(0.99)}, {"max", double(v.back())}}},
                       {"saturated_fraction", std::round(satFrac * 10000) / 10000}, {"stars", int(F.stars.size())}, {"stars_sigma10", strong}, {"star_radius_px", std::round(starRadius * 10) / 10}, {"streaks", QJsonObject{{"elongated", streaks.elongated}, {"coherence", std::round(streaks.coherence * 100) / 100}, {"angle_deg", std::round(streaks.angleDeg)}, {"trailed", streaks.trailed}}}, {"exposure_s", exposureS},
                       {"sky_rate_adu_per_s", std::round(skyRate)}, {"suggested_exposure_s", suggestOut}, {"histogram", hist}};
}

// {"file"} or a fresh snapshot ({"exposure","iso","binning"}); {"bins"} (default 16), {"max_exposure"} (default 6 s).
QJsonObject ControlServer::doFrameStats(const QJsonObject& req)
{
    QString file = req["file"].toString(); double expo = req["exposure"].toDouble(0); QJsonObject snap;
    if (file.isEmpty()) {
        if (!m_backend->isConnected()) return fail("not connected");
        QJsonObject sreq{{"timeout", req["snapshot_timeout"].toInt(30)}};
        if (req.contains("dir")) sreq["dir"] = req["dir"];
        if (req.contains("exposure")) { sreq["exposure"] = req["exposure"]; sreq["iso"] = req["iso"].toInt(800); sreq["binning"] = req["binning"].toInt(1); }
        snap = doSnapshot(sreq); if (!snap["ok"].toBool()) return snap;
        file = snap["path"].toString(); expo = snap["exposure"].toDouble(expo);
    }
    QJsonObject r = frameStats(file, expo, req["bins"].toInt(16), req["max_exposure"].toDouble(6.0), req["target_adu"].toDouble(6000.0)); r["file"] = file;
    if (!snap.isEmpty()) r["snapshot"] = snap;
    return r;
}

// Remember the latest plate-solve attempt; a solution also tells the sky analysis where an unaligned mount is pointing.
void ControlServer::recordSolve(const QJsonObject& r)
{
    const QJsonObject fs = r["stats"].toObject();
    QJsonObject s{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"solved", r["solved"].toBool()}, {"skipped", r["skipped"].toBool()}, {"verdict", fs["verdict"]},
                  {"stars", fs["stars"]}, {"median_adu", fs["median_adu"]}, {"sky_rate", fs["sky_rate_adu_per_s"]}, {"exposure_s", fs["exposure_s"]}, {"suggested_exposure_s", fs["suggested_exposure_s"]},
                  {"error", r["error"]}, {"summary", r["summary"]}};
    for (const char* k : {"ra_hours", "dec_deg", "alt_deg", "az_deg", "tilt_alt_deg", "up_deg_east_of_north"}) if (r.contains(k)) s[k] = r[k];
    m_lastSolve = s;
    if (r["solved"].toBool()) m_backend->sky().setSolvedPointing(r["ra_deg"].toDouble() * 3.14159265358979323846 / 180.0, r["dec_deg"].toDouble() * 3.14159265358979323846 / 180.0, QDateTime::currentDateTimeUtc());
}

// The telescope's own tilt sensor (OrientationSensor notifications), valid before the mount has aligned. Whole degrees.
double ControlServer::tiltAltitude(double* ageSec) const
{
    const TelescopeData& d = m_backend->getDataProcessor()->getData();
    *ageSec = d.orientationLastUpdate.isValid() ? d.orientationLastUpdate.msecsTo(QDateTime::currentDateTime()) / 1000.0 : 1e9;
    return d.orientation.altitude;
}

// The GUI's unpark without the initialisation: raise the tube at +AltRate (no azimuth motion) until the tilt sensor reads {"alt_deg"} (default 60),
// checking every 0.5 s. Stops on arrival, after {"timeout"} s (default 90), or when the tilt reading is older than {"max_age"} s (default 5), and refuses to
// move at all without a fresh reading. The azimuth is unknown until the first plate solve (see solve).
QJsonObject ControlServer::doUnpark(const QJsonObject& req)
{
    if (!m_backend->isConnected()) return fail("not connected");
    const double target = req["alt_deg"].toDouble(60.0), maxAge = req["max_age"].toDouble(5.0);
    const int rate = req["rate"].toInt(9), timeoutMs = req["timeout"].toInt(90) * 1000;
    if (target < 10 || target > 85) return fail("alt_deg must be between 10 and 85");
    if (rate < 1 || rate > 9) return fail("rate must be 1..9");
    auto stop = [this] { for (int i = 0; i < 2; ++i) { m_backend->sendCommand("Slew", "Mount", QJsonObject{{"AltRate", 0}, {"AzmRate", 0}}); QEventLoop l; QTimer::singleShot(300, &l, &QEventLoop::quit); l.exec(); } };
    double age = 0, alt = 0; QElapsedTimer w; w.start();
    while (w.elapsed() < 8000) { alt = tiltAltitude(&age); if (age <= maxAge) break; QEventLoop l; QTimer::singleShot(250, &l, &QEventLoop::quit); l.exec(); }
    if (age > maxAge) return fail("no recent tilt reading from the telescope (OrientationSensor); not moving");
    const double start = alt;
    if (alt >= target) return QJsonObject{{"ok", true}, {"outcome", "already_raised"}, {"tilt_alt_deg", alt}, {"summary", QString("tilt already reads %1 deg; nothing to do").arg(alt)}};
    m_backend->sendCommand("Slew", "Mount", QJsonObject{{"AltRate", rate}, {"AzmRate", 0}});
    QElapsedTimer t; t.start(); QString why;
    while (true) {
        QEventLoop l; QTimer::singleShot(500, &l, &QEventLoop::quit); l.exec();
        alt = tiltAltitude(&age);
        if (age > maxAge) { why = QString("tilt reading went stale (%1 s old)").arg(age, 0, 'f', 0); break; }
        if (alt >= target) break;
        if (t.elapsed() > timeoutMs) { why = "timed out before the tilt reached the target"; break; }
    }
    stop();
    QJsonObject r{{"start_tilt_deg", start}, {"tilt_alt_deg", alt}, {"target_deg", target}, {"elapsed_s", int(t.elapsed() / 1000)}};
    if (!why.isEmpty()) { r["ok"] = false; r["outcome"] = "stopped"; r["error"] = why; r["summary"] = "stopped: " + why; return r; }
    r["ok"] = true; r["outcome"] = "raised"; r["summary"] = QString("raised from %1 to %2 deg in %3 s; azimuth unknown until a plate solve").arg(start).arg(alt).arg(int(t.elapsed() / 1000));
    return r;
}

// Plate solve with astrometry.net (solve-field, -z2 because of the one-shot-colour filter): a fresh snapshot, or {"file"}. Returns the J2000 field centre, rotation, size and,
// with the site and the time, the altitude and azimuth of the tube, checked against the tilt sensor. Works before the mount has aligned (the camera does).
// Needs index files for fields of 20-120 arcmin (default dir $HOME/astrometry_indices, or ORIGIN_INDEX_DIR or {"index_dir"}).
QJsonObject ControlServer::doSolve(const QJsonObject& req)
{
    QString file = req["file"].toString(); QJsonObject snap;
    const QDateTime when = QDateTime::currentDateTimeUtc();
    if (file.isEmpty()) {
        if (!m_backend->isConnected()) return fail("not connected");
        QJsonObject sreq{{"timeout", req["snapshot_timeout"].toInt(30)}};
        if (req.contains("exposure")) { sreq["exposure"] = req["exposure"]; sreq["iso"] = req["iso"].toInt(800); sreq["binning"] = req["binning"].toInt(1); }
        snap = doSnapshot(sreq);
        if (!snap["ok"].toBool()) return snap;
        file = snap["path"].toString();
        if (!file.endsWith(".fits", Qt::CaseInsensitive)) return QJsonObject{{"ok", false}, {"error", "the snapshot came back as " + QFileInfo(file).suffix() + ", not FITS (the first frame after a slew can); try again"}, {"snapshot", snap}};
    }
    if (!QFile::exists(file)) return fail("no such file: " + file);
    // cheap check first: a saturated, too-short or starless frame cannot be solved, and solve-field would burn 30-90 s finding that out
    const QJsonObject fs = frameStats(file, snap.isEmpty() ? req["exposure"].toDouble(1.0) : snap["exposure"].toDouble(1.0), 8, req["max_exposure"].toDouble(6.0));
    if (!req["force"].toBool() && fs["verdict"].toString() != "ok") {
        QJsonObject r{{"ok", false}, {"solved", false}, {"skipped", true}, {"file", file}, {"stats", fs}, {"verdict", fs["verdict"]}, {"suggested_exposure_s", fs["suggested_exposure_s"]}};
        if (!snap.isEmpty()) r["snapshot"] = snap;
        r["error"] = "not worth solving: " + fs["verdict"].toString() + (fs["why"].toString().isEmpty() ? QString() : " (" + fs["why"].toString() + ")"); return r;
    }
    QString idx = req["index_dir"].toString(); if (idx.isEmpty()) idx = qEnvironmentVariable("ORIGIN_INDEX_DIR", QDir::homePath() + "/astrometry_indices");
    if (!QDir(idx).exists()) return fail("index directory not found: " + idx + " (astrometry.net index files for 20-120 arcmin fields)");
    QTemporaryDir tmp; if (!tmp.isValid()) return fail("no temporary directory");
    const int cpu = req["cpulimit"].toInt(60);
    QFile cfg(tmp.filePath("solve.cfg")); if (!cfg.open(QIODevice::WriteOnly)) return fail("cannot write solver config");
    cfg.write(QString("cpulimit %1\nadd_path %2\n%3autoindex\n").arg(cpu).arg(idx, QDir("/usr/share/astrometry").exists() ? "add_path /usr/share/astrometry\n" : "").toUtf8()); cfg.close();
    QProcess p; p.setProcessChannelMode(QProcess::MergedChannels);
    p.start("solve-field", {"--config", cfg.fileName(), "-z", QString::number(req["downsample"].toInt(2)), "--overwrite", "--no-plots", "--scale-units", "arcsecperpix",
                            "--scale-low", QString::number(req["scale_low"].toDouble(1.3)), "--scale-high", QString::number(req["scale_high"].toDouble(3.2)), "-D", tmp.path(), file});
    if (!p.waitForStarted(5000)) return fail("cannot run solve-field (is astrometry.net installed?)");
    {   // wait on the event loop, not with waitForFinished: a blocked loop stops the websocket answering the telescope's pings and it hangs up
        QEventLoop loop; QTimer limit; limit.setSingleShot(true);
        QObject::connect(&p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop, &QEventLoop::quit);
        QObject::connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
        limit.start((cpu + 30) * 1000);
        if (p.state() != QProcess::NotRunning) loop.exec();
        if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(2000); return QJsonObject{{"ok", false}, {"solved", false}, {"error", "no solution within the time limit"}, {"file", file}}; }
    }
    const QString out = QString::fromUtf8(p.readAll());
    const QRegularExpressionMatch c = QRegularExpression(R"(Field center: \(RA,Dec\) = \(([-\d.]+), ([-\d.]+)\) deg)").match(out);
    QJsonObject r{{"file", file}, {"stats", fs}};
    if (!snap.isEmpty()) r["snapshot"] = snap;
    if (!c.hasMatch()) { r["ok"] = false; r["solved"] = false; r["error"] = "no solution (too few stars, cloud, or no matching index)"; return r; }
    const double ra = c.captured(1).toDouble(), dec = c.captured(2).toDouble();
    const QRegularExpressionMatch rot = QRegularExpression(R"(up is ([-\d.]+) degrees E of N)").match(out), sz = QRegularExpression(R"(Field size: ([\d.]+) x ([\d.]+) arcminutes)").match(out);
    const Astro::Site site{req["lat_deg"].toDouble(52.2054), req["lon_deg"].toDouble(0.1218)};
    const Astro::AltAz aa = Astro::altAz({ra / 15.0, dec}, site, when);
    double age = 0; const double tilt = tiltAltitude(&age);
    r["ok"] = true; r["solved"] = true; r["ra_deg"] = ra; r["ra_hours"] = ra / 15.0; r["dec_deg"] = dec; r["alt_deg"] = aa.alt; r["az_deg"] = aa.az;
    if (rot.hasMatch()) r["up_deg_east_of_north"] = rot.captured(1).toDouble();
    if (sz.hasMatch()) { r["field_arcmin"] = QJsonArray{sz.captured(1).toDouble(), sz.captured(2).toDouble()}; }
    if (age < 600) { r["tilt_alt_deg"] = tilt; r["tilt_age_s"] = age; r["tilt_minus_solved_deg"] = tilt - aa.alt; }
    r["summary"] = QString("solved: RA %1 h, Dec %2 deg; tube at altitude %3, azimuth %4 deg%5").arg(ra / 15.0, 0, 'f', 3).arg(dec, 0, 'f', 2).arg(aa.alt, 0, 'f', 1).arg(aa.az, 0, 'f', 1)
                       .arg(age < 600 ? QString(" (tilt sensor says %1)").arg(tilt) : QString());
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
    // explicit values go straight into the capture request; takeSingleSnapshot uses the last values the camera has confirmed, which lag a request behind
    const bool sent = req.contains("exposure") ? m_backend->takeSnapshot(req["exposure"].toDouble(), req["iso"].toInt(800), req["binning"].toInt(1), !m_seq->active()) : m_backend->takeSingleSnapshot();
    if (!sent) { disconnect(conn); return fail("snapshot request failed"); }
    loop.exec();
    disconnect(conn);
    // a PNG instead of the raw FITS (seen on the first frame after a slew, and whenever the telescope has reset the capture parameters): ask once more, now that they were resent
    if (done && result["ok"].toBool() && req.contains("exposure") && !req["_retry"].toBool() && !result["path"].toString().endsWith(".fits", Qt::CaseInsensitive)) {
        QEventLoop pause; QTimer::singleShot(1500, &pause, &QEventLoop::quit); pause.exec();
        QJsonObject again = req; again["_retry"] = true; return doSnapshot(again);
    }
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
                                                   "sky_map", "preview", "monitor", "focuser_status", "focuser_move", "focuser_settings", "filter", "initialize", "unpark", "solve", "frame_stats", "sky_note", "sequence", "cancel_capture", "reboot", "send", "cache", "quit"}}};
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
        for (int i = 0; i < 40 && m_reconnecting; ++i) { QEventLoop loop; QTimer::singleShot(500, &loop, &QEventLoop::quit); loop.exec(); }   // the automatic reconnect owns the socket just now
        bool ok = m_backend->connectToTelescope(host, req["port"].toInt(80));
        if (!ok) return fail("connection failed");
        QJsonObject out{{"ok", true}, {"host", host}};
        const QJsonObject f = declareConfiguredFilter();                // the fitted filter, before anything is solved or initialized
        if (f["configured"].isString()) out["filter"] = f;
        return out;
    }

    if (cmd == "status") {
        QJsonObject r{{"ok", true}, {"connected", m_backend->isConnected()}, {"host", m_backend->connectedHost()},
                      {"exposure", m_backend->currentExposure()}, {"iso", m_backend->currentISO()},
                      {"binning", m_backend->currentBinning()}, {"snapshot_in_progress", m_backend->snapshotInProgress()}};
        if (!m_backend->isConnected() && m_wantLink) { r["reconnecting"] = true; r["reconnect_attempts"] = m_reconnectTries; if (m_linkLost.isValid()) r["link_down_s"] = double(m_linkLost.secsTo(QDateTime::currentDateTimeUtc())); }
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
        QJsonObject r{{"ok", true}, {"last", m_backend->lastFocus()}, {"solve", m_lastSolve}};
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
        double age = 0; const double tilt = tiltAltitude(&age);
        QJsonObject out{{"ok", true}, {"mount", r}};
        if (age < 1e8) { out["tilt_alt_deg"] = tilt; out["tilt_age_s"] = std::round(age * 10) / 10; }      // the telescope's tilt sensor; valid before alignment
        return r.isEmpty() ? fail("no response") : out;
    }
    // the sequencer owns the camera and the mount while it runs
    if (m_seq->active() && (cmd == "snapshot" || cmd == "goto" || cmd == "focuser_move" || cmd == "initialize" || cmd == "set" || cmd == "unpark" || cmd == "solve" || (cmd == "frame_stats" && !req.contains("file"))))
        return fail("a sequence is running; stop it first ({\"cmd\":\"sequence\",\"action\":\"stop\"})");
    if (cmd == "sequence") {                          // action start|stop|status; see Sequencer.hpp
        const QString action = req["action"].toString("status");
        if (action == "start") return m_seq->start(req);
        if (action == "stop") return m_seq->stop(req["reason"].toString("stopped by supervisor"));
        return QJsonObject{{"ok", true}, {"status", m_seq->status()}};
    }
    if (cmd == "goto") return doGoto(req);
    if (cmd == "initialize") return doInitialize(req);
    if (cmd == "unpark") return doUnpark(req);
    if (cmd == "solve") { const QJsonObject r = doSolve(req); recordSolve(r); return r; }
    if (cmd == "frame_stats") return doFrameStats(req);
    if (cmd == "sky_note") {                          // {"az","alt","obstructed"}: evidence from a twilight survey for the learned obstruction map
        if (!req.contains("az") || !req.contains("alt")) return fail("need 'az' and 'alt'");
        m_backend->sky().noteSurvey(req["az"].toDouble(), req["alt"].toDouble(), req["obstructed"].toBool(), QDateTime::currentDateTimeUtc());
        return QJsonObject{{"ok", true}};
    }

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

    if (cmd == "filter") {                            // {"name":"clear|nebula"} declares the hand-inserted filter to the firmware (no motor); without "name" it reads the setting back
        if (!m_backend->isConnected()) return fail("not connected");
        if (req.contains("name")) {
            const QString n = req["name"].toString().trimmed().toLower();
            if (n != "clear" && n != "nebula") return fail("filter name must be clear or nebula");
            return applyFilter(n, true);
        }
        m_backend->sendCommand("GetFilter", "Camera");
        const QJsonObject r = waitResponse(m_backend->lastSequenceId(), 5000);
        if (r.isEmpty()) return fail("no response to GetFilter");
        return QJsonObject{{"ok", true}, {"filter", r["Filter"].toString()}};
    }
    // A capture the camera never finished (2026-10-28 00:53: TaskController stuck in SAMPLE_CAPTURE for hours, every later RunSampleCapture ignored while the link stayed up):
    // send CancelImaging (or AbortAll) and wait for a TaskController status that has left the capture. {"wait": seconds (default 20), "command": "CancelImaging" | "AbortAll"}
    if (cmd == "cancel_capture") {
        if (!m_backend->isConnected()) return fail("not connected");
        if (m_seq->active()) return fail("a sequence is running: stop it first");
        const QJsonObject before = m_last.value("TaskController/GetStatus");
        const auto stuck = [](const QJsonObject& s) { return s["State"].toString() == "SAMPLE_CAPTURE" && s["Stage"].toString() == "IN_PROGRESS"; };
        const QString how = req["command"].toString("CancelImaging");
        if (how != "CancelImaging" && how != "AbortAll") return fail("command must be CancelImaging or AbortAll");
        const QString dest = req["destination"].toString("TaskController");
        if (!QStringList{"TaskController", "MountTaskController", "Camera", "ImageServer", "System"}.contains(dest)) return fail("destination must be TaskController, MountTaskController, Camera, ImageServer or System");
        if (how == "CancelImaging" && dest == "TaskController") m_backend->abortExposure(); else m_backend->sendCommand(how, dest);
        const QJsonObject ack = waitResponse(m_backend->lastSequenceId(), 8000);
        QElapsedTimer t; t.start(); QJsonObject now = before; bool fresh = false;
        const int waitMs = req["wait"].toInt(20) * 1000;
        while (t.elapsed() < waitMs) {
            QEventLoop loop; QTimer::singleShot(500, &loop, &QEventLoop::quit); loop.exec();
            const QJsonObject cur = m_last.value("TaskController/GetStatus");
            if (cur["SequenceID"].toInt() != before["SequenceID"].toInt()) { now = cur; fresh = true; if (!stuck(now)) break; }
        }
        const bool cleared = fresh && !stuck(now);
        return QJsonObject{{"ok", cleared}, {"command", how}, {"destination", dest}, {"cleared", cleared}, {"was_stuck", stuck(before)}, {"state_before", before["State"].toString() + "/" + before["Stage"].toString()},
                           {"state_now", fresh ? now["State"].toString() + "/" + now["Stage"].toString() : QString("no new status")}, {"ack", ack.isEmpty() ? QJsonValue("none") : QJsonValue(ack)},
                           {"error", cleared ? QString() : QString("the telescope still reports a capture in progress")}};
    }

    if (cmd == "reboot") {
        // Reboot (System): the only command found that clears a camera stuck in SAMPLE_CAPTURE (2026-10-28; CancelImaging is acknowledged and ignored, AbortAll is unknown).
        // Accepted even mid-exposure. The link drops at once, the telescope is off the network for about 15-25 s, the server reconnects by itself; the mount comes back NOT aligned.
        if (!m_backend->isConnected()) return fail("not connected");
        if (m_seq->active()) return fail("a sequence is running: stop it first");
        m_backend->recordEvent("telescope_reboot", req["reason"].toString("requested"));
        m_backend->sendCommand("Reboot", "System");
        const QJsonObject ack = waitResponse(m_backend->lastSequenceId(), 8000);
        const bool ok = !ack.isEmpty() && ack["ErrorCode"].toInt() == 0;
        return QJsonObject{{"ok", ok}, {"ack", ack.isEmpty() ? QJsonValue("none") : QJsonValue(ack)}, {"host", m_backend->connectedHost()},
                           {"error", ok ? QString() : QString("the telescope did not accept the reboot")}};
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
