#include "Sequencer.hpp"
#include <algorithm>
#include <cmath>
#include "DataDir.hpp"
#include "OriginBackend.hpp"

#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTime>

Sequencer::Sequencer(OriginBackend* backend, QObject* parent) : QObject(parent), m_b(backend)
{
    m_watchdog.setSingleShot(true); m_probeTimer.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &Sequencer::onWatchdog);
    connect(&m_probeTimer, &QTimer::timeout, this, &Sequencer::requestFrame);
}

QJsonObject Sequencer::start(const QJsonObject& req)
{
    if (m_active) return QJsonObject{{"ok", false}, {"error", "sequence already running"}};
    if (!m_b->isConnected()) return QJsonObject{{"ok", false}, {"error", "not connected"}};
    m_exposure = req["exposure"].toDouble(6); m_iso = req["iso"].toInt(800); m_binning = req["binning"].toInt(1);
    m_badLimit = qMax(1, req["bad_limit"].toInt(3)); m_probeMs = int(req["probe_s"].toDouble(30) * 1000);
    m_pipeline = req["pipeline"].toBool(true); m_early = req["early"].toBool(false); m_secondSent = false; m_reqTimes.clear(); m_reqExp.clear(); m_maxFrames = req["max_frames"].toInt(0);
    if (m_exposure <= 0 || m_exposure > 300) return QJsonObject{{"ok", false}, {"error", "exposure must be 0-300 s"}};
    m_untilMs = 0;
    if (req.contains("for_minutes")) m_untilMs = QDateTime::currentMSecsSinceEpoch() + qint64(req["for_minutes"].toDouble() * 60000);
    if (req.contains("until_local")) {
        const QTime t = QTime::fromString(req["until_local"].toString(), "HH:mm");
        if (!t.isValid()) return QJsonObject{{"ok", false}, {"error", "until_local must be HH:MM"}};
        QDateTime u(QDate::currentDate(), t);
        if (u < QDateTime::currentDateTime().addSecs(-60)) u = u.addDays(1);
        m_untilMs = u.toMSecsSinceEpoch();
    }
    m_frames = m_good = m_bad = m_badRun = m_probes = 0; m_cycles = 0; m_sumCaptureMs = m_sumAnalysisMs = m_sumCycleMs = m_sumCycleExpS = 0;
    m_haveLastReady = false; m_outstanding = m_awaitDl = 0; m_reqTimes.clear(); m_reqExp.clear(); m_expectAnalysis = false; m_sumDownloadMs = 0; m_state = State::Shooting; m_stopReason.clear(); m_lastVerdict.clear(); m_last = QJsonObject();
    QDir().mkpath(originDataDir());
    m_logPath = originDataDir() + "/sequence_" + QDate::currentDate().toString("yyyyMMdd") + ".jsonl";
    m_cImg = connect(m_b, &OriginBackend::tiffImageDownloaded, this, &Sequencer::onImage);
    m_cFoc = connect(m_b, &OriginBackend::focusEvaluated, this, &Sequencer::onFocus);
    m_cMsg = connect(m_b, &OriginBackend::messageReceived, this, &Sequencer::onMessage);
    m_hfrWin.clear(); m_hfrBase = req["focus_baseline"].toDouble(0); m_hfrMedian = 0; m_driftRun = m_drifts = 0;
    m_driftRatio = req["focus_drift"].toDouble(1.15); m_focusWin = qBound(3, req["focus_window"].toInt(8), 50); m_focusPause = req["focus_pause"].toBool(false);
    m_auto = req["auto_exposure"].toBool(false); m_minExp = req["min_exposure"].toDouble(2); m_maxExp = req["max_exposure"].toDouble(60);
    m_latDeg = req["lat_deg"].toDouble(52.2054); m_targetEff = std::clamp(req["target_efficiency"].toDouble(0.94), 0.5, 0.99);
    m_readNoise = req["read_noise_adu"].toDouble(580); m_skyVarPerAdu = req["sky_var_per_adu"].toDouble(310); m_blackAdu = req["black_level"].toDouble(4985);
    m_lastExpChangeMs = 0; m_autoInfo = QJsonObject();
    if (m_auto && (m_minExp <= 0 || m_maxExp < m_minExp || m_maxExp > 300)) return QJsonObject{{"ok", false}, {"error", "need 0 < min_exposure <= max_exposure <= 300"}};
    if (m_auto) m_exposure = std::clamp(m_exposure, m_minExp, m_maxExp);
    m_active = true;
    if (!req["resend_params"].toBool(true)) m_b->setResendCaptureParams(false);     // the FITS format is only kept when the parameters are resent every cycle
    m_b->setLiveStreamDisabled(true);
    m_b->recordEvent("sequence_started", QString("exposure %1 s%4 ISO %2, pipeline %3").arg(m_exposure).arg(m_iso).arg(m_pipeline ? "on" : "off")
                                             .arg(m_auto ? QString(" (auto %1-%2 s)").arg(m_minExp).arg(m_maxExp) : QString()));
    requestFrame();
    return QJsonObject{{"ok", m_active}, {"status", status()}};
}

QJsonObject Sequencer::stop(const QString& reason)
{
    if (!m_active) return QJsonObject{{"ok", true}, {"status", status()}, {"note", "no sequence was running"}};
    if (m_outstanding > 0 || m_awaitDl > 0 || m_expectAnalysis) {
        beginDrain(reason);
        if (m_active) {
            QEventLoop loop; QTimer limit; limit.setSingleShot(true);
            connect(this, &Sequencer::stopped, &loop, &QEventLoop::quit); connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
            limit.start(int((m_exposure + 60) * 1000));
            loop.exec();
            if (m_active) { m_b->recordEvent("sequence_drain_timeout", "the frame in flight did not arrive in time; stopped anyway"); finish(reason); }
        }
    } else finish(reason);
    return QJsonObject{{"ok", true}, {"status", status()}};
}

void Sequencer::beginDrain(const QString& reason)
{
    if (!m_active) return;
    m_draining = true; m_drainReason = reason; m_probeTimer.stop();
    checkDrained();
}

void Sequencer::checkDrained()
{
    if (m_active && m_draining && m_outstanding <= 0 && m_awaitDl <= 0 && !m_expectAnalysis) finish(m_drainReason);
}

void Sequencer::finish(const QString& reason)
{
    m_active = false; m_draining = false; m_stopReason = reason; m_b->setResendCaptureParams(true);
    disconnect(m_cImg); disconnect(m_cFoc); disconnect(m_cMsg);
    m_watchdog.stop(); m_probeTimer.stop();
    m_b->recordEvent("sequence_stopped", QString("%1 after %2 frames").arg(reason).arg(m_frames));
    emit stopped();
}

void Sequencer::requestFrame()
{
    if (!m_active || m_draining || m_outstanding >= (m_early ? 2 : 1)) return;
    if (m_untilMs && QDateTime::currentMSecsSinceEpoch() >= m_untilMs) { beginDrain("end time reached"); return; }
    if (m_maxFrames && m_frames + m_awaitDl + m_outstanding >= m_maxFrames) { beginDrain("max_frames reached"); return; }
    if (!m_b->isConnected()) { finish("disconnected"); return; }
    m_reqTimes.append(QDateTime::currentMSecsSinceEpoch()); m_reqExp.append(m_exposure); ++m_outstanding;
    m_watchdog.start(int((m_exposure * 3 + 60) * 1000));
    // (an explicit setCameraBitDepth(16) here, tried 2026-10-10, disturbed the capture in progress: frames came back after 2 s with wrong levels; pipeline:false is the safe mode)
    if (!m_b->takeSnapshot(m_exposure, m_iso, m_binning)) { --m_outstanding; m_reqTimes.removeLast(); m_reqExp.removeLast(); finish("snapshot request failed"); }
}

void Sequencer::onWatchdog()
{
    if (!m_active) return;
    m_b->recordEvent("sequence_timeout", "no image arrived; capture cancelled, sequence stopped");
    m_b->abortExposure();          // the camera can hang mid-capture and then ignore every new request until it is cancelled (2026-10-28)
    finish("frame never arrived");
}

void Sequencer::onMessage(const QJsonObject& m)
{
    if (!m_active || m_outstanding <= 0) return;
    const QString src = m["Source"].toString(), cmd = m["Command"].toString(), type = m["Type"].toString();
    // the telescope refused a request (typically the early one, made while it is still saving the previous frame)
    if (src == "TaskController" && cmd == "RunSampleCapture" && type == "Response" && m["ErrorCode"].toInt() != 0) {
        --m_outstanding; if (!m_reqTimes.isEmpty()) { m_reqTimes.removeLast(); m_reqExp.removeLast(); }
        if (m_early) {
            m_early = false; m_secondSent = false;
            m_b->recordEvent("sequence_early_rejected", "telescope refused an early RunSampleCapture (" + m["ErrorMessage"].toString() + "): early mode off");
        } else { m_b->recordEvent("sequence_error", "RunSampleCapture refused: " + m["ErrorMessage"].toString()); if (m_draining) checkDrained(); else finish("capture refused"); }
        return;
    }
    if (src != "ImageServer" || cmd != "NewImageReady" || type != "Notification") return;
    const QString kind = m["ImageType"].toString();
    if (kind == "LIVE") {                                              // the exposure itself has ended; the FITS is still being written
        if (m_early && m_outstanding == 1 && !m_secondSent && m_state == State::Shooting) { m_secondSent = true; requestFrame(); }
        return;
    }
    if (kind != "SAMPLE_CAPTURE") return;
    --m_outstanding; ++m_awaitDl;
    m_curCaptureMs = m_reqTimes.isEmpty() ? 0 : double(QDateTime::currentMSecsSinceEpoch() - m_reqTimes.takeFirst());   // request to FITS ready
    m_curExp = m_reqExp.isEmpty() ? m_exposure : m_reqExp.takeFirst();          // the exposure this frame was taken with (auto exposure may have moved on)
    m_secondSent = m_outstanding > 0;
    if (m_outstanding == 0) m_watchdog.stop(); else m_watchdog.start(int((m_exposure * 3 + 60) * 1000));
    m_curCycleMs = 0;
    if (m_haveLastReady && m_state == State::Shooting) { m_curCycleMs = m_readyT.elapsed(); m_sumCycleMs += m_curCycleMs; m_sumCycleExpS += m_curExp; ++m_cycles; }
    m_readyT.start(); m_haveLastReady = true; m_dlT.start();
    // this notification arrives before the backend starts the download: start the next exposure now so download and analysis overlap it
    if (m_pipeline && m_state == State::Shooting) requestFrame();
}

void Sequencer::onImage(const QString& remotePath, const QByteArray& data, double, double, double)
{
    if (!m_active || m_awaitDl <= 0) return;           // a snapshot someone else asked for
    --m_awaitDl;
    m_curDownloadMs = m_dlT.elapsed(); m_sumDownloadMs += m_curDownloadMs;
    QString ext = QFileInfo(remotePath).suffix().toLower(); if (ext.isEmpty()) ext = "fits";
    m_curPath = QString("%1/snapshot_%2.%3").arg(originDataDir(), QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"), ext);
    QFile f(m_curPath);
    if (!(f.open(QIODevice::WriteOnly) && f.write(data) == data.size())) { m_curPath.clear(); m_b->recordEvent("sequence_error", "could not write " + f.fileName()); }
    m_expectAnalysis = true; m_anaT.start();
}

void Sequencer::onFocus(const QJsonObject& o)
{
    if (!m_active || !m_expectAnalysis) return;
    m_expectAnalysis = false;
    const double analysisMs = m_anaT.elapsed();
    const QString verdict = o["verdict"].toString();
    if (verdict == "settling") {              // the telescope took the frame with the previous exposure or ISO: not a verdict on the sky, not counted, not stacked
        m_b->recordEvent("sequence_settling", o["reasons"].toArray().first().toString());
        QFile slog(m_logPath);
        if (slog.open(QIODevice::Append)) slog.write(QJsonDocument(QJsonObject{{"time", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}, {"state", "settling"}, {"file", m_curPath}, {"verdict", verdict}, {"reasons", o["reasons"]},
                                                                                {"exposure", m_curExp}, {"cycle_ms", 0}}).toJson(QJsonDocument::Compact) + "\n");
        if (m_draining) checkDrained(); else if (m_outstanding == 0 && m_state == State::Shooting) requestFrame();
        return;
    }
    const bool good = verdict == "clear" || verdict == "hazy" || verdict == "moon_glow" || verdict == "twilight";   // as SkyMonitor's "good": moonlit frames stack (2026-10-21)
    ++m_frames; good ? ++m_good : ++m_bad; m_badRun = good ? 0 : m_badRun + 1; m_lastVerdict = verdict;
    m_sumCaptureMs += m_curCaptureMs; m_sumAnalysisMs += analysisMs;
    const QJsonObject reg = o["registration"].toObject();
    QJsonObject rec{{"time", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}, {"state", m_state == State::Shooting ? "shooting" : "probing"},
                    {"file", m_curPath}, {"verdict", verdict}, {"stars", o["stars"].toInt()}, {"hfr_px", o["hfr_px"].toDouble()}, {"sky_rate", o["sky_rate"].toDouble()},
                    {"cause", o["cause"].toString()}, {"reasons", o["reasons"]}, {"transparency", reg["transparency"].toDouble()}, {"stacked", reg["stacked"].toBool()}, {"frames_in_stack", reg["frames_in_stack"].toInt()},
                    {"capture_ms", m_curCaptureMs}, {"download_ms", m_curDownloadMs}, {"analysis_ms", analysisMs}, {"cycle_ms", m_curCycleMs}, {"exposure", m_curExp}};
    bool drift = false;
    const double hfr = o["hfr_px"].toDouble();
    if (good && o["stars"].toInt() >= 30 && hfr > 0) {
        m_hfrWin.append(hfr); while (m_hfrWin.size() > m_focusWin) m_hfrWin.removeFirst();
        if (m_hfrWin.size() == m_focusWin) {
            QList<double> v = m_hfrWin; std::sort(v.begin(), v.end()); m_hfrMedian = v[v.size() / 2];
            if (m_hfrBase <= 0) m_hfrBase = m_hfrMedian;
            else if (m_hfrMedian > m_hfrBase * m_driftRatio) drift = ++m_driftRun >= 5;
            else m_driftRun = 0;
        }
    }
    if (m_hfrBase > 0) { rec["focus_median_px"] = m_hfrMedian; rec["focus_ratio"] = m_hfrMedian / m_hfrBase; }
    m_last = rec;
    QFile log(m_logPath);
    if (log.open(QIODevice::Append)) log.write(QJsonDocument(rec).toJson(QJsonDocument::Compact) + "\n");

    if (!good && o["cause"].toString() == "obstruction" && m_lastCause != "obstruction")
        m_b->recordEvent("sequence_obstructed", QString("frame blocked by a fixed obstruction, not cloud (%1): consider another target").arg(o["cause_reasons"].toArray().first().toString()));
    m_lastCause = good ? QString() : o["cause"].toString();
    if (drift) {
        ++m_drifts; m_driftRun = 0; m_hfrWin.clear();
        m_b->recordEvent("sequence_focus_drift", QString("median HFR %1 px is %2 x the baseline %3 px: refocus").arg(m_hfrMedian, 0, 'f', 2).arg(m_hfrMedian / m_hfrBase, 0, 'f', 2).arg(m_hfrBase, 0, 'f', 2));
        if (m_focusPause) { beginDrain("focus drift"); if (!m_active) return; }
    }
    if (m_draining) { checkDrained(); return; }
    if (good && m_auto && m_state == State::Shooting) adjustExposure(o);
    if (m_state == State::Shooting && m_badRun >= m_badLimit) {
        m_state = State::Probing;
        m_b->recordEvent("sequence_cloud", QString("%1 bad frames in a row (last verdict %2): probing every %3 s").arg(m_badRun).arg(verdict).arg(m_probeMs / 1000));
    } else if (m_state == State::Probing) {
        ++m_probes;
        if (good) { m_state = State::Shooting; m_badRun = 0; m_b->recordEvent("sequence_clear", "sky clear again: shooting"); }
    }
    if (m_outstanding == 0) {
        if (m_state == State::Shooting) requestFrame();
        else m_probeTimer.start(m_probeMs);
    }
}

void Sequencer::adjustExposure(const QJsonObject& o)
{
    if (!o.contains("alt_deg") || !o.contains("az_deg")) return;            // no pointing for this frame: keep the exposure
    constexpr double kPi = 3.14159265358979323846, omega = 7.2921159e-5, rCorner = 1840.0, tolPx = 2.0;   // as Planner::maxExposure: half-diagonal of the 3056x2048 frame
    const double alt = o["alt_deg"].toDouble() * kPi / 180, az = o["az_deg"].toDouble() * kPi / 180;
    const double w = std::fabs(omega * std::cos(m_latDeg * kPi / 180) * std::cos(az) / std::max(std::cos(alt), 0.02));
    const double tRot = w < 1e-12 ? 1e9 : 0.85 * tolPx / (rCorner * w);
    const double sky = std::max(o["sky_rate"].toDouble(), 1.0);                // ADU/s above black
    const double tSat = (32768.0 - m_blackAdu) / sky;
    // sky variance sky*t*v against read noise rn^2: the share of the noise variance that is sky reaches eff at t = eff/(1-eff) * rn^2 / (v * sky)
    const double tEff = m_targetEff / (1 - m_targetEff) * m_readNoise * m_readNoise / (m_skyVarPerAdu * sky);
    struct L { double t; const char* why; } lims[] = {{tRot, "field rotation"}, {tSat, "sky level"}, {tEff, "read noise swamped"}, {m_maxExp, "max_exposure"}};
    const L* best = &lims[0];
    for (const L& l : lims) if (l.t < best->t) best = &l;
    double t = std::max(m_minExp, best->t);
    t = t >= 10 ? std::floor(t) : std::floor(t * 2) / 2;                      // whole seconds, half seconds under 10 s
    m_autoInfo = QJsonObject{{"chosen_s", t}, {"limit", best->why}, {"rotation_s", std::round(std::min(tRot, 9999.0) * 10) / 10}, {"sky_s", std::round(tSat * 10) / 10},
                             {"efficiency_s", std::round(tEff * 10) / 10}, {"max_s", m_maxExp}, {"alt_deg", std::round(o["alt_deg"].toDouble())}, {"az_deg", std::round(o["az_deg"].toDouble())}, {"sky_rate", std::round(sky)}};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool shorter = t < m_exposure - 0.01;
    const bool longer = t >= m_exposure * 1.15 && (m_lastExpChangeMs == 0 || now - m_lastExpChangeMs >= 180000);
    if (!shorter && !longer) return;
    m_b->recordEvent("sequence_exposure", QString("exposure %1 s -> %2 s (limit: %3; rotation %4 s, sky %5 s, read noise %6 s, alt %7)")
                                              .arg(m_exposure).arg(t).arg(best->why).arg(std::min(tRot, 9999.0), 0, 'f', 0).arg(tSat, 0, 'f', 0).arg(tEff, 0, 'f', 0).arg(o["alt_deg"].toDouble(), 0, 'f', 0));
    m_exposure = t; m_lastExpChangeMs = now;
}

QJsonObject Sequencer::status() const
{
    const int n = m_frames;
    QJsonObject s{{"active", m_active}, {"state", m_state == State::Shooting ? "shooting" : "probing"}, {"frames", n}, {"good", m_good}, {"bad", m_bad},
                  {"bad_run", m_badRun}, {"probes", m_probes}, {"exposure", m_exposure}, {"iso", m_iso}, {"pipeline", m_pipeline}, {"early", m_early}, {"outstanding", m_outstanding}, {"last_verdict", m_lastVerdict},
                  {"log", m_logPath}, {"stop_reason", m_stopReason}, {"last", m_last},
                  {"focus", QJsonObject{{"baseline_px", m_hfrBase}, {"median_px", m_hfrMedian}, {"window", m_hfrWin.size()}, {"drifts", m_drifts}, {"drift_ratio", m_driftRatio}, {"pause", m_focusPause}}}};
    if (m_auto) { QJsonObject a = m_autoInfo; a["min_s"] = m_minExp; a["max_s"] = m_maxExp; a["target_efficiency"] = m_targetEff; s["auto_exposure"] = a; }
    if (n > 0) { s["avg_capture_ms"] = m_sumCaptureMs / n; s["avg_analysis_ms"] = m_sumAnalysisMs / n; s["avg_download_ms"] = m_sumDownloadMs / n; }
    if (m_cycles > 0) { s["avg_cycle_ms"] = m_sumCycleMs / m_cycles; s["duty_cycle"] = m_sumCycleExpS * 1000.0 / m_sumCycleMs; }   // exposure time over wall time: the exposure changes under auto exposure
    return s;
}
