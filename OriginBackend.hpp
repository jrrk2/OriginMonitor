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
#include <QImage>
#include <QUuid>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QStringConverter>
#include <QWebSocket>
#include "TelescopeDataProcessor.hpp"

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
        bool isSlewing = false;
        bool isTracking = false;
        bool isParked = false;
        bool isAligned = false;
        QString currentOperation = "Idle";
        double temperature = 20.0;
    };

    explicit OriginBackend(QObject *parent = nullptr);
    ~OriginBackend();

    // Connection management
    bool connectToTelescope(const QString& host, int port = 80);
    void disconnectFromTelescope();
    bool isConnected() const;

    // Mount operations
    bool gotoPosition(double ra, double dec);
    bool syncPosition(double ra, double dec);
    bool abortMotion();
    bool parkMount();
    bool unparkMount();
    bool initializeTelescope();
    bool moveDirection(int direction, int speed);

    // Tracking
    bool setTracking(bool enabled);
    bool isTracking() const;

    // Status access
    TelescopeStatus status() const;
    double temperature() const;

    // Camera operations
    bool isExposing() const;
    bool isImageReady() const;
    QImage getLastImage() const;
    void setLastImage(const QImage& image);
    void setImageReady(bool ready);
    bool abortExposure();
    QImage singleShot(int gain, int binning, int exposureTimeMicroseconds);

signals:
    void connected();
    void disconnected();
    void statusUpdated();
    void imageReady();

private slots:
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onTextMessageReceived(const QString &message);
    void onImageDownloaded();
    void updateStatus();

private:
    QWebSocket *m_webSocket;
    TelescopeDataProcessor *m_dataProcessor;
    QNetworkAccessManager *m_networkManager;
    QTimer *m_statusTimer;
    
    // State variables
    QString m_connectedHost;
    int m_connectedPort;
    bool m_isConnected;
    bool m_isExposing;
    bool m_imageReady;
    QImage m_lastImage;
    int m_nextSequenceId;
    
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
    void sendCommand(const QString& command, const QString& destination, 
                    const QJsonObject& params = QJsonObject());
    QJsonObject createCommand(const QString& command, const QString& destination, 
                             const QJsonObject& params = QJsonObject());
    void updateStatusFromProcessor();
    void requestImage(const QString& filePath);
    double radiansToHours(double radians);
    double radiansToDegrees(double radians);
    double hoursToRadians(double hours);
    double degreesToRadians(double degrees);
};
