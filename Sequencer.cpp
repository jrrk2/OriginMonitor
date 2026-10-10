#include "Sequencer.hpp"
#include "DataDir.hpp"
#include "OriginBackend.hpp"

#include <QDateTime>
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
    m_pipeline = req["pipeline"].toBool(true); m_early = req["early"].toBool(false); m_secondSent = false; m_reqTimes.clear(); m_maxFrames = req["max_frames"].toInt(0);
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
    m_frames = m_good = m_bad = m_badRun = m_probes = 0; m_cycles = 0; m_sumCaptureMs = m_sumAnalysisMs = m_sumCycleMs = 0;
    m_haveLastReady = false; m_outstanding = m_awaitDl = 0; m_reqTimes.clear(); m_expectAnalysis = false; m_sumDownloadMs = 0; m_state = State::Shooting; m_stopReason.clear(); m_lastVerdict.clear(); m_last = QJsonObject();
    QDir().mkpath(originDataDir());
    m_logPath = originDataDir() + "/sequence_" + QDate::currentDate().toString("yyyyMMdd") + ".jsonl";
    m_cImg = connect(m_b, &OriginBackend::tiffImageDownloaded, this, &Sequencer::onImage);
    m_cFoc = connect(m_b, &OriginBackend::focusEvaluated, this, &Sequencer::onFocus);
    m_cMsg = connect(m_b, &OriginBackend::messageReceived, this, &Sequencer::onMessage);
    m_active = true;
    if (!req["resend_params"].toBool(true)) m_b->setResendCaptureParams(false);     // the FITS format is only kept when the parameters are resent every cycle
    m_b->recordEvent("sequence_started", QString("exposure %1 s ISO %2, pipeline %3").arg(m_exposure).arg(m_iso).arg(m_pipeline ? "on" : "off"));
    requestFrame();
    return QJsonObject{{"ok", m_active}, {"status", status()}};
}

QJsonObject Sequencer::stop(const QString& reason)
{
    if (!m_active) return QJsonObject{{"ok", true}, {"status", status()}, {"note", "no sequence was running"}};
    finish(reason);
    return QJsonObject{{"ok", true}, {"status", status()}};
}

void Sequencer::finish(const QString& reason)
{
    m_active = false; m_stopReason = reason; m_b->setResendCaptureParams(true);
    disconnect(m_cImg); disconnect(m_cFoc); disconnect(m_cMsg);
    m_watchdog.stop(); m_probeTimer.stop();
    m_b->recordEvent("sequence_stopped", QString("%1 after %2 frames").arg(reason).arg(m_frames));
}

void Sequencer::requestFrame()
{
    if (!m_active || m_outstanding >= (m_early ? 2 : 1)) return;
    if (m_untilMs && QDateTime::currentMSecsSinceEpoch() >= m_untilMs) { finish("end time reached"); return; }
    if (m_maxFrames && m_frames >= m_maxFrames) { finish("max_frames reached"); return; }
    if (!m_b->isConnected()) { finish("disconnected"); return; }
    m_reqTimes.append(QDateTime::currentMSecsSinceEpoch()); ++m_outstanding;
    m_watchdog.start(int((m_exposure * 3 + 60) * 1000));
    // (an explicit setCameraBitDepth(16) here, tried 2026-10-10, disturbed the capture in progress: frames came back after 2 s with wrong levels; pipeline:false is the safe mode)
    if (!m_b->takeSnapshot(m_exposure, m_iso, m_binning)) { --m_outstanding; m_reqTimes.removeLast(); finish("snapshot request failed"); }
}

void Sequencer::onWatchdog()
{
    if (!m_active) return;
    m_b->recordEvent("sequence_timeout", "no image arrived; sequence stopped");
    finish("frame never arrived");
}

void Sequencer::onMessage(const QJsonObject& m)
{
    if (!m_active || m_outstanding <= 0) return;
    const QString src = m["Source"].toString(), cmd = m["Command"].toString(), type = m["Type"].toString();
    // the telescope refused a request (typically the early one, made while it is still saving the previous frame)
    if (src == "TaskController" && cmd == "RunSampleCapture" && type == "Response" && m["ErrorCode"].toInt() != 0) {
        --m_outstanding; if (!m_reqTimes.isEmpty()) m_reqTimes.removeLast();
        if (m_early) {
            m_early = false; m_secondSent = false;
            m_b->recordEvent("sequence_early_rejected", "telescope refused an early RunSampleCapture (" + m["ErrorMessage"].toString() + "): early mode off");
        } else { m_b->recordEvent("sequence_error", "RunSampleCapture refused: " + m["ErrorMessage"].toString()); finish("capture refused"); }
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
    m_secondSent = m_outstanding > 0;
    if (m_outstanding == 0) m_watchdog.stop(); else m_watchdog.start(int((m_exposure * 3 + 60) * 1000));
    m_curCycleMs = 0;
    if (m_haveLastReady && m_state == State::Shooting) { m_curCycleMs = m_readyT.elapsed(); m_sumCycleMs += m_curCycleMs; ++m_cycles; }
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
    const bool good = verdict == "clear" || verdict == "hazy";
    ++m_frames; good ? ++m_good : ++m_bad; m_badRun = good ? 0 : m_badRun + 1; m_lastVerdict = verdict;
    m_sumCaptureMs += m_curCaptureMs; m_sumAnalysisMs += analysisMs;
    const QJsonObject reg = o["registration"].toObject();
    QJsonObject rec{{"time", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}, {"state", m_state == State::Shooting ? "shooting" : "probing"},
                    {"file", m_curPath}, {"verdict", verdict}, {"stars", o["stars"].toInt()}, {"hfr_px", o["hfr_px"].toDouble()}, {"sky_rate", o["sky_rate"].toDouble()},
                    {"transparency", reg["transparency"].toDouble()}, {"stacked", reg["stacked"].toBool()}, {"frames_in_stack", reg["frames_in_stack"].toInt()},
                    {"capture_ms", m_curCaptureMs}, {"download_ms", m_curDownloadMs}, {"analysis_ms", analysisMs}, {"cycle_ms", m_curCycleMs}, {"exposure", m_exposure}};
    m_last = rec;
    QFile log(m_logPath);
    if (log.open(QIODevice::Append)) log.write(QJsonDocument(rec).toJson(QJsonDocument::Compact) + "\n");

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

QJsonObject Sequencer::status() const
{
    const int n = m_frames;
    QJsonObject s{{"active", m_active}, {"state", m_state == State::Shooting ? "shooting" : "probing"}, {"frames", n}, {"good", m_good}, {"bad", m_bad},
                  {"bad_run", m_badRun}, {"probes", m_probes}, {"exposure", m_exposure}, {"iso", m_iso}, {"pipeline", m_pipeline}, {"early", m_early}, {"outstanding", m_outstanding}, {"last_verdict", m_lastVerdict},
                  {"log", m_logPath}, {"stop_reason", m_stopReason}, {"last", m_last}};
    if (n > 0) { s["avg_capture_ms"] = m_sumCaptureMs / n; s["avg_analysis_ms"] = m_sumAnalysisMs / n; s["avg_download_ms"] = m_sumDownloadMs / n; }
    if (m_cycles > 0) { s["avg_cycle_ms"] = m_sumCycleMs / m_cycles; s["duty_cycle"] = m_exposure * 1000.0 / (m_sumCycleMs / m_cycles); }
    return s;
}
