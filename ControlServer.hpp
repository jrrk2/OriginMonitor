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
 * Requests are handled one at a time; a command that waits for the telescope blocks later ones.
 */
class ControlServer : public QObject
{
    Q_OBJECT
public:
    ControlServer(OriginBackend* backend, bool allowRaw, QObject* parent = nullptr);
    bool listen(quint16 port);

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    QJsonObject handle(const QJsonObject& req);
    QJsonObject waitResponse(int sequenceId, int timeoutMs);   // matching "Type":"Response" message
    QJsonObject queryFocuser(int timeoutMs = 5000);            // GetStatus on Focuser
    QJsonObject doSnapshot(const QJsonObject& req);
    QJsonObject doFocuserMove(const QJsonObject& req);
    QJsonObject queryMount(int timeoutMs = 5000);              // GetStatus on Mount
    QJsonObject doGoto(const QJsonObject& req);
    QJsonObject doInitialize(const QJsonObject& req);
    QJsonObject queryTaskController(int timeoutMs = 3000);
    static QJsonObject fail(const QString& message);

    OriginBackend* m_backend;
    Sequencer* m_seq;
    bool m_allowRaw;
    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QSet<QTcpSocket*> m_busy;                       // sockets with a command in progress (handle() runs nested event loops)
    QHash<QString, QJsonObject> m_last;         // "Source/Command" -> last message seen
    QHash<int, QJsonObject> m_responses;        // SequenceID -> response
    int m_focuserLower = 1975;                  // travel limits, updated from telescope status
    int m_focuserUpper = 37527;
};
