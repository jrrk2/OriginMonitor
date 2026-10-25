#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class OriginBackend;

/**
 * Unattended exposure loop inside the app (control command "sequence"), so a supervising session does not have to drive every frame.
 *
 * SHOOTING: take frames back to back. After `bad_limit` consecutive frames whose verdict is not clear/hazy it drops to
 * PROBING: one frame every `probe_s` seconds until a probe is clear/hazy, then SHOOTING again. Frames are saved to the data
 * directory and one JSON line per frame (verdict, stars, HFR, sky rate, transparency, timings) is appended to sequence_<date>.jsonl.
 * With `pipeline` the next exposure is requested at the telescope's "image ready" notification, so download and analysis overlap it.
 * With `early` the next exposure is requested as soon as the previous exposure has finished (the telescope's LIVE preview notification),
 * while it is still writing the FITS (about 6.6 s on this telescope). If the telescope rejects that, `early` switches itself off.
 * Focus watchdog: the median HFR of the last `focus_window` (8) good frames with at least 30 stars is compared with a baseline (`focus_baseline`, full-resolution px,
 * else the first full window). When it stays above baseline x `focus_drift` (1.15) for 5 frames a `sequence_focus_drift` event is recorded and, with
 * `focus_pause`, the sequence stops so that the supervisor can run focus-sweep and restart.
 * Auto exposure (`auto_exposure`): after each good frame the exposure is re-chosen as the shortest of (1) the field-rotation limit at the frame's alt/az
 * (2 px at the frame corner, x0.85), (2) the sky filling half of the 16-bit range above black, (3) the length at which sky noise is `target_efficiency` (0.94)
 * of the total noise variance, beyond which longer subs gain little over read noise (`read_noise_adu` 580, `sky_var_per_adu` 310, measured at ISO 800 on 2026-10-25;
 * both scale with ISO so the length does not), and (4) `max_exposure`; never below `min_exposure`. Shorter values apply at once, longer ones only when at least
 * 15% longer and 3 minutes after the last change. Each change is a `sequence_exposure` event.
 * Stops on: stop command, `until_local`/`for_minutes`, `max_frames`, disconnect, or a frame that never arrives (events are recorded).
 */
class Sequencer : public QObject
{
    Q_OBJECT
public:
    explicit Sequencer(OriginBackend* backend, QObject* parent = nullptr);
    bool active() const { return m_active; }
    QJsonObject start(const QJsonObject& req);   // exposure, iso, bad_limit, probe_s, pipeline, max_frames, for_minutes | until_local "HH:MM"
    QJsonObject stop(const QString& reason);
    QJsonObject status() const;

signals:
    void stopped();                                // the sequence has finished (after any in-flight frame has been taken in)

private slots:
    void onImage(const QString& remotePath, const QByteArray& data, double ra, double dec, double exposure);
    void onMessage(const QJsonObject& msg);        // ImageServer NewImageReady: the exposure is done, the file is waiting on the telescope
    void onFocus(const QJsonObject& analysis);
    void onWatchdog();
    void requestFrame();

private:
    enum class State { Shooting, Probing };
    void finish(const QString& reason);
    // Pipelined captures leave the next exposure in flight while the previous frame is downloaded and analysed. A stop that is not a failure waits for them (the frame is taken in and scored),
    // so that a goto or a refocus afterwards does not ruin an exposure and the last frame is not lost.
    void beginDrain(const QString& reason);
    void checkDrained();
    bool m_draining = false; QString m_drainReason;

    OriginBackend* m_b;
    bool m_active = false;
    State m_state = State::Shooting;
    double m_exposure = 6;
    int m_iso = 800, m_binning = 1, m_badLimit = 3, m_probeMs = 30000, m_maxFrames = 0;
    bool m_pipeline = true, m_early = false, m_secondSent = false;
    QList<qint64> m_reqTimes;                       // epoch ms of each request still awaiting its "image ready"
    qint64 m_untilMs = 0;                       // epoch ms, 0 = none
    int m_frames = 0, m_good = 0, m_bad = 0, m_badRun = 0, m_probes = 0;
    int m_outstanding = 0, m_awaitDl = 0;           // exposures requested but not announced / announced but not downloaded
    bool m_expectAnalysis = false, m_haveLastReady = false;
    QElapsedTimer m_reqT, m_anaT, m_readyT, m_dlT;
    double m_sumDownloadMs = 0, m_curDownloadMs = 0;
    QList<double> m_reqExp;                         // the exposure of each of those requests
    double m_curExp = 0;                            // exposure of the frame being downloaded/analysed
    double m_sumCaptureMs = 0, m_sumAnalysisMs = 0, m_sumCycleMs = 0, m_sumCycleExpS = 0; int m_cycles = 0;
    QMetaObject::Connection m_cImg, m_cFoc, m_cMsg;
    QTimer m_watchdog, m_probeTimer;
    QString m_logPath, m_curPath, m_lastVerdict, m_stopReason;
    double m_curCaptureMs = 0, m_curCycleMs = 0;
    QJsonObject m_last;
    // focus watchdog: rolling median of the HFR of good frames against a baseline (given, or the first full window)
    QList<double> m_hfrWin;
    double m_hfrBase = 0, m_hfrMedian = 0, m_driftRatio = 1.15;
    int m_focusWin = 8, m_driftRun = 0, m_drifts = 0;
    bool m_focusPause = false;
    // auto exposure (see the class comment)
    void adjustExposure(const QJsonObject& analysis);
    bool m_auto = false;
    double m_minExp = 2, m_maxExp = 60, m_latDeg = 52.2054, m_targetEff = 0.94, m_readNoise = 580, m_skyVarPerAdu = 310, m_blackAdu = 4985;
    qint64 m_lastExpChangeMs = 0;
    QJsonObject m_autoInfo;                         // the last choice and its limits, for status
    QString m_lastCause;                            // cause of the previous bad frame (obstruction | cloud | unknown), empty after a good one
};
