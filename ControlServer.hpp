#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QHash>

class OriginBackend;
class Sequencer;

/**
 * Local scripting interface: newline-delimited JSON over TCP, bound to 127.0.0.1 only.
 *
 * Request:  {"cmd": "<name>", ...args}\n
 * Reply:    {"ok": true, ...}\n   or   {"ok": false, "error": "..."}\n
 *
 * Commands: ping, connect, status, manual, auto, set, snapshot, mount_status, goto,
 *           focus_status, analysis, events, target, stack, sky_map, monitor, focuser_status, focuser_move, focuser_settings, send (only with allowRaw), quit.
 * Many clients (AI sessions, scripts) may be connected at once. Read-only queries are answered from a short-lived cache (status 2 s, focus/analysis 1 s,
 * sky map 30 s ...) so polling clients do not each hit the telescope; the reply then carries "cached":true and "age_ms". Any other command (goto, set,
 * snapshot, ...) clears the cache. Add "fresh":true to a request to bypass it. {"cmd":"cache"} reports hit statistics, {"cmd":"cache","clear":true} flushes.
 * Requests are handled one at a time; a command that waits for the telescope blocks later ones.
 */
class ControlServer : public QObject
{
    Q_OBJECT
public:
    ControlServer(OriginBackend* backend, bool allowRaw, QObject* parent = nullptr);
    bool listen(quint16 port);
    void setCacheEnabled(bool on) { m_cacheOn = on; }
    void shutdown();                                  // stop any sequence, save the live stack, quit the application (also what "quit" does)

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    QJsonObject handle(const QJsonObject& req);                // answers read-only queries from the cache when fresh enough, otherwise handleUncached()
    QJsonObject handleUncached(const QJsonObject& req);
    static int cacheTtlMs(const QString& cmd);                  // 0 = not cacheable
    QJsonObject waitResponse(int sequenceId, int timeoutMs);   // matching "Type":"Response" message
    QJsonObject queryFocuser(int timeoutMs = 5000);            // GetStatus on Focuser
    QJsonObject doSnapshot(const QJsonObject& req);
    QJsonObject doFocuserMove(const QJsonObject& req);
    QJsonObject queryMount(int timeoutMs = 5000);              // GetStatus on Mount
    QJsonObject doGoto(const QJsonObject& req);
    QJsonObject doInitialize(const QJsonObject& req);
    QJsonObject doUnpark(const QJsonObject& req);
    QJsonObject doSolve(const QJsonObject& req);
    QJsonObject doFrameStats(const QJsonObject& req);
    static QJsonObject frameStats(const QString& path, double exposureS, int bins, double maxExposureS, double targetAdu = 6000.0);
    double tiltAltitude(double* ageSec) const;                  // OrientationSensor altitude (deg, whole degrees) and the age of that reading
    QJsonObject queryTaskController(int timeoutMs = 3000);
    static QJsonObject fail(const QString& message);

    struct CacheEntry { QJsonObject reply; qint64 atMs = 0; };
    QHash<QByteArray, CacheEntry> m_cache;                      // canonical request JSON -> reply
    bool m_cacheOn = true;
    qint64 m_hits = 0, m_misses = 0;

    OriginBackend* m_backend;
    Sequencer* m_seq;
    bool m_allowRaw;
    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QSet<QTcpSocket*> m_busy;                       // sockets with a command in progress (handle() runs nested event loops)
    QHash<QString, QJsonObject> m_last;         // "Source/Command" -> last message seen
    QHash<int, QJsonObject> m_responses;        // SequenceID -> response
    QString m_tcError; qint64 m_tcErrorMs = 0;  // newest TaskController error message (it can be replaced by the next status before a poll sees it)
    QJsonObject m_lastSolve;                    // summary of the latest plate-solve attempt (shown by the analysis command and OriginWatch)
    void recordSolve(const QJsonObject& r);
    int m_focuserLower = 1975;                  // travel limits, updated from telescope status
    int m_focuserUpper = 37527;
};
