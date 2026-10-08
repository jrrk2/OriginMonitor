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
#include <QEventLoop>
#include <QWebSocket>
#include "TelescopeDataProcessor.hpp"

class MountModel;

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
    void setMountModel(MountModel *model);

    // Relative slew correction: nudge mount by dAlt/dAz arcseconds
    // Uses timed Slew command at a low rate
    void correctionSlew(double dAltArcsec, double dAzArcsec);
    bool abortMotion();
    bool parkMount();           // Park using Slew rate and altitude monitoring
    bool unparkMount();         // Unpark using Slew rate and altitude monitoring
    bool initializeTelescope();
    bool moveDirection(int direction, int speed);
    void speed(int altRate);
    // Tracking
    bool setTracking(bool enabled);
    bool isTracking() const;

    // New commands discovered from Origin.app v1.1.4
    // NOTE: unverified against OLV OriginLiveView; not confirmed working against real hardware.
    bool gotoAltAzm(double altRadians, double azmRadians);
    bool gotoEncoder(double enc0, double enc1);
    bool stopAll();
    bool reboot();
    bool powerDown();
    bool setFans(bool cpuFan, bool otaFan);
    bool getFans();
    bool getEnvironmentSensors();

    // Filter selection (Origin dual-band OSC filter)
    bool setFilter(const QString& filter);
    bool getFilter();

    // Dew heater control (ported from OriginLiveView DewHeater.cs)
    bool setDewHeaterAuto(int aggression);
    bool setDewHeaterManual(double powerLevel);

    // Focuser position control
    // CONFIRMED VALID: manual focus jog/drag in the real app is implemented entirely as repeated
    // absolute MoveTo calls (e.g. Value: 17945 -> 17904 -> 17625 -> 16625 ...), each immediately
    // ACKed with ErrorCode 0 - reconfirmed live 2026-09-17 capturing the official app in terrestrial
    // mode (14 consecutive MoveTo calls, zero RunManualFocus/RunTerrestrialAutoFocus calls).
    bool getFocuserPosition();
    bool moveFocuserTo(int position);  // CONFIRMED VALID: genuine app sends {"Command":"MoveTo","Destination":"Focuser","Value":<int position>}

    // CONFIRMED VALID (genuine capture): real response fields are AutoFocusAfterGotoIsEnabled,
    // TemperatureShiftAutoFocusIsEnabled, ThresholdTemperatureC - note the real field is
    // "ThresholdTemperatureC", NOT "TemperatureThresholdC" as OLV's SET command uses below;
    // the SET side was never captured, so setAutoFocusOnTemperatureChange()'s field name is unconfirmed
    // and may need to be corrected to ThresholdTemperatureC if it turns out not to take effect.
    bool getFocuserAdvancedSettings();
    bool setAutoFocusAfterGoto(bool enabled);
    bool setAutoFocusOnTemperatureChange(bool enabled, double thresholdC);

    // Mount configuration (EQ/wedge/backlash/custom rate/PEC)
    bool setMountConfig(bool isEquatorial, bool onWedge, double altBacklash, double azBacklash,
                         int customRate9Speed, bool enableCustomRate9, bool enablePecEnabled);

    // EL flat panel connection lifecycle
    bool connectElPanel();
    bool disconnectElPanel();

    bool runAddReference();
    bool runCenterTarget(double raRadians, double decRadians);  // CONFIRMED VALID: seen in genuine Origin app capture (RunCenterTarget/TaskController)
    bool runObservingList(const QString& listUuid, bool powerDownOnCompletion = false);

    // Ad-hoc single-target imaging schedule (ported from OLV ScheduleImage.cs),
    // sends the target inline instead of referencing a stored observing list Uuid.
    bool scheduleImaging(double raRadians, double decRadians, const QString& name,
                          double exposureTime, int iso, int totalDurationSeconds,
                          double objectMagnitude, double objectSize, bool saveRawImage,
                          bool autoFocusAfterSlew, bool powerDownOnCompletion,
                          bool continueImagingIfStackingFails);
    // MYSTERY SOLVED: across two full genuine app sessions (including one spanning a complete
    // RunInitialize cycle whose InitializationInfo.CurrentStep passed through "FOCUSING" for 10+
    // MYSTERY SOLVED (now with 3 independent genuine sessions, including a live capture of the
    // official app run in terrestrial mode with manual focus actively exercised on 2026-09-17):
    // the real Origin app NEVER ONCE sent RunAutoFocus, RunInfinityAutoFocus, RunManualFocus,
    // RunTerrestrialAutoFocus, or CancelAutoFocus - not even while manually focusing in terrestrial
    // mode, which only ever used MoveTo/Focuser (see above). Focusing during initialization is
    // fully autonomous firmware behavior driven by RunInitialize/TaskController and the
    // AutoFocusAfterGoto/AutoFocusOnTemperatureChange config flags above, not something explicitly
    // triggered by the client. Treat the methods below as CONFIRMED UNUSED by the real app;
    // TelescopeGUI already handles the "FOCUSING" TaskController step correctly without calling these.
    bool runAutoFocus();
    bool runInfinityAutoFocus();
    bool runManualFocus(int range, int startPosition, int stepSize);
    bool runPolarAlign();
    bool cancelAutoFocus();
    bool runGenerateNewDarks(int maxDarks, int numDarks);  // UNVERIFIED param set (see runGenerateNewDarksWithParams for the OLV-verified one)
    bool runGenerateNewFlat();
    bool imageNextTarget();                 // UNVERIFIED
    bool getObservingListInfo(const QString& listUuid);  // CONFIRMED VALID: seen in genuine Origin app capture (GetObservingListInfo/TaskController)
    bool getLogs();
    bool getAutomaticUpdates();               // CONFIRMED VALID: seen in genuine Origin app capture (GetAutomaticUpdates/System)
    bool setAutomaticUpdates(bool enabled);  // CONFIRMED VALID: seen in genuine Origin app capture (SetAutomaticUpdates/System)
    bool getVisibleWiFi();                   // UNVERIFIED
    bool getWiFiNetworks();                  // CONFIRMED VALID: seen in genuine Origin app capture (GetWiFiNetworks/Network)
    bool addWiFiNetwork(const QString& ssid, const QString& password);       // UNVERIFIED
    bool deleteWiFiNetwork(const QString& ssid);                             // UNVERIFIED
    bool connectToSpecificNetwork(const QString& ssid, const QString& password);  // UNVERIFIED
    bool setLedBrightness(int level);        // UNVERIFIED (only the Get side was seen in captures)
    bool getLedBrightness();                 // CONFIRMED VALID: seen in genuine Origin app capture (GetBrightnessLevel/LedRing, also used with Destination: LampController)
    bool turnOnLight();
    bool turnOffLight();
    bool setLightLevel(int level);
    bool getAccelerometer();                 // CONFIRMED VALID: returned real data when probed live against 192.168.1.195
    bool zeroCalibrate();                    // UNVERIFIED
    bool deleteAllAlignRefs();               // UNVERIFIED
    bool enablePec(bool enabled);            // UNVERIFIED as standalone command; OLV only ever sends EnablePec bundled inside SetMountConfig

    // Mosaic capture - CONFIRMED VALID against a live capture of the current official app
    // (2026-09-17, real "RunCaptureMosaic"/TaskController command with an actual Andromeda Galaxy
    // mosaic in progress). This superseded both our old fabricated OffsetX/OffsetY/NbrSubs guess
    // AND the OLV-ported "CaptureMosaic" command name - OLV's mosaic forms are older than the
    // telescope's current mosaic implementation and use a stale command name. MosaicWidth/Height
    // are angular size in radians (not tile counts), matching MosaicOrientation/ObjectRa/ObjectDec.
    bool runCaptureMosaic(double mosaicWidth, double mosaicHeight, double mosaicOrientation,
                          double raRadians, double decRadians, const QString& name,
                          double exposureTime, int iso, int totalDurationSeconds,
                          double objectMagnitude, double objectSize, bool saveRawImage,
                          const QString& minimumStartTime, bool autoFocusAfterSlew,
                          bool powerDownOnCompletion, int ditherDistance, int ditherInterval);

    // Compose the final mosaic from its captured tiles. Per user confirmation this can only be
    // called once all tiles are captured (poll getAvailableMosaics()/getMosaicTiles() and wait for
    // TilesCompleted == TilesTotal first) - our capture window ended while still IN_PROGRESS/0
    // tiles complete, so calling this too early was never exercised and RunComposeMosaic itself is
    // still UNVERIFIED against a genuine capture, not confirmed autonomous/unnecessary.
    bool runComposeMosaic();

    // Mosaic status/browsing - all CONFIRMED VALID against the same live capture.
    bool getAvailableMosaics();                       // GetAvailableMosaics/Mosaic
    bool getMosaicTiles(const QString& mosaicUuid);   // GetMosaicTiles/Mosaic
    bool getMaximumTiles();                           // GetMaximumTiles/Mosaic

    // Manual sample capture used for Bias/Flat frames (ported from OLVTakeBias.cs / OLVTakeFlat.cs)
    bool runSampleCapture(double exposureTime, int iso, int bitDepth, int binning,
                          bool calibrate, bool debayer);

    // Alternate darks parameterization used by OLV TakeDark/RetakeDarks forms
    bool runGenerateNewDarksWithParams(int bitDepth, double exposureTime, int iso);

    bool getAvailableImagingLists();          // UNVERIFIED
    bool getListOfAvailableDirectories();      // UNVERIFIED
    bool copyDirectoryToFlashDrive(const QString& dirName, const QString& sourcePath, const QString& targetPath);  // UNVERIFIED

    // Status access
    TelescopeStatus status() const;
    double temperature() const;
    int m_nextSequenceId;

    // Camera operations
    bool isCameraLogicallyConnected() const { return m_status.isCameraLogicallyConnected; }
    void setCameraConnected(bool connected);
    bool isExposing() const;
    bool isImageReady() const;
    QImage getLastImage() const;
    void setLastImage(const QImage& image);
    void setImageReady(bool ready);
    bool abortExposure();
    QImage singleShot(int gain, int binning, int exposureTimeMicroseconds);
    void sendCommand(const QString& command, const QString& destination, 
                    const QJsonObject& params = QJsonObject());
    bool takeSnapshot(double exposure, int iso, int binning);
    bool setManualMode();
    bool setAutoMode();
    bool getCameraMode();
    bool getCaptureParameters();
    bool getCameraInfo();
    // Camera mode control
    bool setCameraManualMode();
    bool setCameraAutoMode();
     
    // Camera exposure/ISO control
    bool setCameraExposure(double seconds);
    bool setCameraBinning(int binning);
    bool setCameraISO(int iso);
    
    // Snapshot control
    bool takeSingleSnapshot();
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
    void rawJsonTraffic(const QString &direction, const QJsonObject &json);

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
    QImage m_lastImage;
    
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
    void requestImage(const QString& filePath);
    double hoursToRadians(double hours);
    double degreesToRadians(double degrees);
    void stopParkMotion();  // Stop altitude motion during park/unpark
    
    // Camera state tracking
    bool m_cameraManualMode;
    double m_currentExposure;
    int m_currentISO;
    int m_currentBinning;
    bool m_snapshotInProgress;
    
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
    MountModel *m_mountModel = nullptr;  // Optional mount model for pointing corrections
    QTimer *m_correctionTimer = nullptr;  // Timer to stop correction slew
};
