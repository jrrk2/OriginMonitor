#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#ifndef ORIGIN_NO_GUI
#include <QImage>
#endif
#include <QUuid>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QStringConverter>
#include <QWebSocket>
#include "TelescopeDataProcessor.hpp"
#include "FocusAnalyzer.hpp"
#include "SkyMonitor.hpp"

/**
 * @brief Backend adapter to connect Alpaca server to Celestron Origin telescope
 * 
 * This class implements the OpenStellinaBackend interface but communicates
 * with a Celestron Origin telescope using its WebSocket JSON protocol.
 */
class OriginBackend : public QObject
{
    Q_OBJECT

public:
    struct TelescopeStatus {
        double altPosition = 0.0;     // Altitude in degrees
        double azPosition = 0.0;      // Azimuth in degrees
        double raPosition = 0.0;      // RA in hours
        double decPosition = 0.0;     // Dec in degrees
        bool isConnected = false;
        bool isLogicallyConnected = false;
        bool isCameraLogicallyConnected = false;
        bool isSlewing = false;
        bool isTracking = false;
        bool isParked = false;
        bool isAligned = false;
        QString currentOperation = "Idle";
        double temperature = 20.0;
    };

    explicit OriginBackend(QObject *parent = nullptr);
    ~OriginBackend();

    // Give GUI access to the processor that receives WebSocket packets
    TelescopeDataProcessor* getDataProcessor() const { return m_dataProcessor; }
  
    // Connection management
    bool connectToTelescope(const QString& host, int port = 80);
    void disconnectFromTelescope();
    bool isConnected() const;
    bool isLogicallyConnected() const;
    QString getConnectedHost();

    // Set the logical connection state (fast, no network activity)
    void setConnected(bool connected) {
        if (connected && !m_status.isConnected) {
            qWarning() << "Cannot set connected - no physical connection to Origin";
            return;
        }
        m_status.isLogicallyConnected = connected;
        qDebug() << "Logical connection state:" << m_status.isLogicallyConnected;
    }
  
    // Mount operations
    bool gotoPosition(double ra, double dec);
    bool syncPosition(double ra, double dec);
    bool abortMotion();
    bool parkMount();           // Park using MoveAxis and altitude monitoring
    bool unparkMount();         // Unpark using MoveAxis and altitude monitoring
    bool initializeTelescope();
    bool moveDirection(int direction, int speed);
    void speed(int altRate);
    // Tracking
    bool setTracking(bool enabled);
    bool isTracking() const;

    // Status access
    TelescopeStatus status() const;
    double temperature() const;
    int m_nextSequenceId;

    // Camera operations
    bool isCameraLogicallyConnected() const { return m_status.isCameraLogicallyConnected; }
    void setCameraConnected(bool connected);
    bool isExposing() const;
    bool isImageReady() const;
#ifndef ORIGIN_NO_GUI
    QImage getLastImage() const;
    void setLastImage(const QImage& image);
#endif
    void setImageReady(bool ready);
    bool abortExposure();
#ifndef ORIGIN_NO_GUI
    QImage singleShot(int gain, int binning, int exposureTimeMicroseconds);
#endif
    void sendCommand(const QString& command, const QString& destination, 
                    const QJsonObject& params = QJsonObject());
    bool takeSnapshot(double exposure, int iso, int binning, bool forceBitDepth = false);   // forceBitDepth: send BitDepth 16 even if it was sent before (the telescope drops it after a slew and the frame comes back as a PNG)
    bool setManualMode();
    bool setAutoMode();
    bool getCameraMode();
    bool getCaptureParameters();
    bool getCameraInfo();
    // Camera mode control
    bool setCameraManualMode();
    bool setCameraAutoMode();
    // The live stream is never wanted: with it on, a sample capture can wait for the live frame in progress. Sent on every connect and at every sequence start (the telescope forgets it when
    // the stream is closed; verified 2026-10-14 with GetDisableLiveStream: the reply has IsDisabled, the setter needs the parameter "Disable").
    bool setLiveStreamDisabled(bool disable);
    double m_lastReqExposure = 0; int m_lastReqIso = 0;      // what the latest RunSampleCapture asked for (not the live-stream settings): a frame whose header differs was taken with older settings
     
    // Camera exposure/ISO control
    bool setCameraExposure(double seconds);
    bool setCameraBinning(int binning);
    bool setCameraISO(int iso);
    bool getFocuserAdvancedSettings();   // AutoFocusAfterGoto / temperature-shift autofocus flags
    bool setCameraBitDepth(int bitDepth);  // internal: the app always uses 16 (raw 16-bit Bayer FITS snapshots)
    
    // Snapshot control
    bool takeSingleSnapshot();
    bool startContinuousSnapshots();
    void stopContinuousSnapshots();
    bool isContinuousSnapshots() const { return m_continuousSnapshots; }
    bool snapshotInProgress() const { return m_snapshotInProgress; }
    // After a sample capture the backend re-sends exposure/ISO/bit depth (the telescope resets the live stream). A running Sequencer turns that off:
    // every RunSampleCapture carries its own settings and the re-sends arrive mid-exposure.
    void setResendCaptureParams(bool on) { m_resendCaptureParams = on; }
    // Focus quality of every snapshot (see FocusAnalyzer.hpp); history is capped, newest last
    QJsonObject lastFocus() const { return m_focusHistory.isEmpty() ? QJsonObject() : m_focusHistory.last(); }
    QJsonArray focusHistory(int count) const;
    SkyMonitor& sky() { return m_sky; }                  // per-frame sky analysis and live stack (see SkyMonitor.hpp)
    // Event feed for an external supervisor: telescope errors and alerts, connection loss, download failures.
    QJsonObject eventsSince(qint64 afterSeq, int limit = 200) const;
    void recordEvent(const QString& kind, const QString& text, const QJsonObject& detail = QJsonObject());
    double currentExposure() const { return m_currentExposure; }
    int currentISO() const { return m_currentISO; }
    int currentBinning() const { return m_currentBinning; }
    QString connectedHost() const { return m_connectedHost; }
    int lastSequenceId() const { return m_nextSequenceId - 1; }   // ID of the most recently sent command
    double radiansToHours(double radians);
    double radiansToDegrees(double radians);

signals:
    void connected();
    void disconnected();
    void statusUpdated();
    void imageReady();
    // Camera-related signals
    void cameraModeChanged(bool isManual);
    void captureParametersChanged(double exposure, int iso, int binning);
    void cameraInfoReceived(const QString& cameraID, const QString& model);
    void snapshotRequested();
    void messageReceived(const QJsonObject& message);   // every JSON message from the telescope
    void continuousSnapshotsChanged(bool running);
    void eventRecorded(const QJsonObject& event);
    void focusEvaluated(const QJsonObject& focus);       // after every snapshot: FocusResult::toJson() plus time, exposure, ra, dec, file
    void tiffImageDownloaded(const QString& filePath, const QByteArray& imageData,
                            double ra, double dec, double exposure);
    void liveImageDownloaded(const QByteArray& imageData, 
                            double ra, double dec, double exposure);
    // Park/Unpark signals
    void parkStarted();
    void unparkStarted();
    void parkCompleted();
    void unparkCompleted();
    void trackingError(const QString& error);

private slots:
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onTextMessageReceived(const QString &message);
    void updateStatus();
    void monitorParkProgress();  // Monitor altitude during park/unpark

private:
    QWebSocket *m_webSocket;
    TelescopeDataProcessor *m_dataProcessor;
    QNetworkAccessManager *m_networkManager;
    QTimer *m_statusTimer;
    QTimer *m_pingTimer;
    QTimer *m_parkMonitorTimer;  // Timer for monitoring park/unpark progress
  
    // State variables
    QString m_connectedHost;
    int m_connectedPort;
    bool m_isExposing;
    bool m_imageReady;
#ifndef ORIGIN_NO_GUI
    QImage m_lastImage;
#endif
    
    // Current telescope status
    TelescopeStatus m_status;
    
    // Pending operations
    QMap<int, QString> m_pendingCommands;
    QString m_currentImagingSession;
    QFile* m_logFile;
    QTextStream* m_logStream;
    
    void initializeLogging();
    void logWebSocketMessage(const QString& direction, const QString& message);
    void cleanupLogging();

    // Helper methods
    QJsonObject createCommand(const QString& command, const QString& destination, 
                             const QJsonObject& params = QJsonObject());
    void updateStatusFromProcessor();
    void requestImage(const QString& filePath, bool isSnapshot = false);
    double hoursToRadians(double hours);
    double degreesToRadians(double degrees);
    void stopParkMotion();  // Stop altitude motion during park/unpark
    
    // Camera state tracking
    bool m_cameraManualMode;
    double m_currentExposure;
    int m_currentISO;
    int m_currentBinning;
    bool m_snapshotInProgress;
    bool m_continuousSnapshots = false;
    bool m_resendCaptureParams = true;
    QList<QJsonObject> m_focusHistory;
    SkyMonitor m_sky;
    QList<QJsonObject> m_events;
    qint64 m_eventSeq = 0;
    void evaluateSnapshotFocus(const QString& filePath, const QByteArray& imageData, double ra, double dec, double exposure);
    // User's manual settings; the telescope reverts live-stream exposure/ISO after each sample capture
    double m_requestedExposure = 0.0;
    int m_requestedISO = 0;
    int m_requestedBitDepth = 0;
    
    // Image metadata from last NewImageReady
    double m_lastImageRa;
    double m_lastImageDec;
    double m_lastImageExposure;
    QString m_lastImageFilePath;
    int m_statusRotation;
    
    // Park/Unpark state
    bool m_parkingInProgress;
    bool m_unparkingInProgress;
    double m_targetAltitude;  // Target altitude for park/unpark operations
};
