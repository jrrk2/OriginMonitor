#include "OriginBackend.hpp"
#include "DataDir.hpp"
#include <QDebug>
#include <QDateTime>
#include <QUrl>
#include <QBuffer>
#include <cmath>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>

OriginBackend::OriginBackend(QObject *parent)
    : QObject(parent)
    , m_nextSequenceId(2000)
    , m_webSocket(nullptr)
    , m_dataProcessor(nullptr)
    , m_networkManager(nullptr)
    , m_statusTimer(nullptr)
    , m_pingTimer(nullptr)
    , m_parkMonitorTimer(nullptr)
    , m_connectedPort(80)
    , m_isExposing(false)
    , m_imageReady(false)
    , m_logFile(nullptr)
    , m_logStream(nullptr)
    , m_cameraManualMode(false)
    , m_currentExposure(0.1)
    , m_currentISO(200)
    , m_currentBinning(1)
    , m_snapshotInProgress(false)
    , m_lastImageRa(0)
    , m_lastImageDec(0)
    , m_lastImageExposure(0)
    , m_statusRotation(0)
    , m_parkingInProgress(false)
    , m_unparkingInProgress(false)
    , m_targetAltitude(0.0)
{
    m_webSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
    m_dataProcessor = new TelescopeDataProcessor(this);
    m_networkManager = new QNetworkAccessManager(this);
    m_statusTimer = new QTimer(this);
    m_pingTimer = new QTimer(this);
    m_parkMonitorTimer = new QTimer(this);

    // Initialize logging
    initializeLogging();

    // Connect WebSocket signals
    connect(m_webSocket, &QWebSocket::connected, this, &OriginBackend::onWebSocketConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &OriginBackend::onWebSocketDisconnected);
    connect(m_webSocket, &QWebSocket::textMessageReceived, this, &OriginBackend::onTextMessageReceived);

    // ADD THESE - WebSocket ping/pong handling:
    connect(m_webSocket, &QWebSocket::pong, this, [this](quint64 elapsedTime, const QByteArray &) {
        qDebug() << "Pong received, RTT:" << elapsedTime << "ms";
        logWebSocketMessage("PONG", QString("RTT: %1ms").arg(elapsedTime));
    });
    
    connect(m_webSocket, &QWebSocket::errorOccurred,
            this, [this](QAbstractSocket::SocketError error) {
        qWarning() << "WebSocket error:" << error << m_webSocket->errorString();
        logWebSocketMessage("ERROR", QString("Code: %1, Message: %2")
                           .arg(error).arg(m_webSocket->errorString()));
        recordEvent("connection_error", m_webSocket->errorString(), QJsonObject{{"code", int(error)}});
    });

    // Connect data processor signals
    connect(m_dataProcessor, &TelescopeDataProcessor::mountStatusUpdated, 
            this, &OriginBackend::updateStatus);
    connect(m_dataProcessor, &TelescopeDataProcessor::newImageAvailable, 
            this, &OriginBackend::imageReady);

    // Setup status update timer
    m_statusTimer->setInterval(5000); // Update every 5 seconds
    connect(m_statusTimer, &QTimer::timeout, this, &OriginBackend::updateStatus);

    // ADD THIS - Setup ping timer (keep-alive)
    m_pingTimer->setInterval(15000); // Ping every 10 seconds
    connect(m_pingTimer, &QTimer::timeout, this, [this]() {
        if (m_webSocket && m_webSocket->isValid() && 
            m_webSocket->state() == QAbstractSocket::ConnectedState) {
            qDebug() << "Sending WebSocket ping...";
            m_webSocket->ping();
            logWebSocketMessage("PING", "Keep-alive ping sent");
        }
    });

    // Setup park monitoring timer
    m_parkMonitorTimer->setInterval(500);  // Check every 0.5 seconds
    connect(m_parkMonitorTimer, &QTimer::timeout, this, &OriginBackend::monitorParkProgress);

}

OriginBackend::~OriginBackend()
{
    disconnectFromTelescope();
    cleanupLogging();  // ADD THIS LINE
}

bool OriginBackend::connectToTelescope(const QString& host, int port)
{
    if (isConnected()) {
        qDebug() << "Already connected to telescope";
        return true;
    }

    m_connectedHost = host;
    m_connectedPort = port;

    // Construct WebSocket URL for Origin telescope
    QString url = QString("ws://%1:%2/SmartScope-1.0/mountControlEndpoint").arg(host).arg(port);
    
    qDebug() << "Connecting to Origin telescope at:" << url;
    
    m_webSocket->open(QUrl(url));
    
    // Wait for connection with timeout
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(10000); // 10 second timeout
    
    connect(m_webSocket, &QWebSocket::connected, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeoutTimer.start();
    loop.exec();
    
    return isConnected();
}

void OriginBackend::disconnectFromTelescope()
{
    if (m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState) {
        m_webSocket->close();
    }
    
    if (m_statusTimer->isActive()) {
        m_statusTimer->stop();
    }
    
    // ADD THIS
    if (m_pingTimer && m_pingTimer->isActive()) {
        m_pingTimer->stop();
    }

    m_status.isConnected = false;
    m_status.isLogicallyConnected = false;
    m_status.isCameraLogicallyConnected = false;
    qDebug() << "disconnected";
    
    emit disconnected();
}

bool OriginBackend::isConnected() const
{
    return m_status.isConnected;
}

bool OriginBackend::isLogicallyConnected() const
{
    return m_status.isConnected && m_status.isLogicallyConnected;
}

bool OriginBackend::gotoPosition(double ra, double dec)
{
    if (!isConnected()) {
        qWarning() << "Cannot goto position - not connected";
        return false;
    }

    // Convert RA/Dec to radians for Origin telescope
    double raRadians = hoursToRadians(ra);
    double decRadians = degreesToRadians(dec);

    QJsonObject params;
    params["Ra"] = raRadians;
    params["Dec"] = decRadians;

    sendCommand("GotoRaDec", "Mount", params);
    
    m_status.isSlewing = true;
    m_status.currentOperation = "Slewing";
    
    return true;
}

bool OriginBackend::syncPosition(double ra, double dec)
{
    if (!isConnected()) {
        qWarning() << "Cannot sync position - not connected";
        return false;
    }

    // Convert RA/Dec to radians for Origin telescope
    double raRadians = hoursToRadians(ra);
    double decRadians = degreesToRadians(dec);

    QJsonObject params;
    params["Ra"] = raRadians;
    params["Dec"] = decRadians;

    sendCommand("SyncToRaDec", "Mount", params);
    
    return true;
}

// Snapshot Control
bool OriginBackend::takeSingleSnapshot()
{
  return takeSnapshot(m_currentExposure, m_currentISO, m_currentBinning);
}

bool OriginBackend::startContinuousSnapshots()
{
  if (m_continuousSnapshots) return true;
  m_continuousSnapshots = true;
  if (!takeSingleSnapshot()) {
    m_continuousSnapshots = false;
    return false;
  }
  emit continuousSnapshotsChanged(true);
  return true;
}

void OriginBackend::stopContinuousSnapshots()
{
  if (!m_continuousSnapshots) return;
  // Any snapshot already in flight will still complete and be saved
  m_continuousSnapshots = false;
  emit continuousSnapshotsChanged(false);
}

bool OriginBackend::takeSnapshot(double exposure, int iso, int binning)
{
    qDebug() << "Taking snapshot: Exposure =" << exposure << "ISO =" << iso;
    if (!isConnected()) {
      qDebug() << "ignored due to closed socket";
        return false;
    }

    m_snapshotInProgress = true;
    // RunSampleCapture command triggers a single snapshot
    QJsonObject params;
    params["ExposureTime"] = exposure;
    params["ISO"] = iso;
    params["Binning"] = binning;
    // Snapshots are always the raw 16-bit Bayer frame (FITS). Ask for that explicitly: older firmware defaulted to a
    // debayered TIFF when "Debayer" was absent (logs Oct 2025 - Jan 2026), current firmware (Origin 1.5.6240) defaults
    // to raw FITS, so relying on the default would change the file type after a firmware update.
    if (m_requestedBitDepth != 16)
        setCameraBitDepth(16);
    params["Debayer"] = false;
    
    sendCommand("RunSampleCapture", "TaskController", params);
    return true;
}

// ============================================================================
// MODE CONTROL
// ============================================================================

bool OriginBackend::setCameraManualMode()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;    
    sendCommand("SetEnableManual", "LiveStream", cmd);
    qDebug() << "Setting manual mode";
    return true;
}

bool OriginBackend::setCameraAutoMode()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;
    m_requestedExposure = 0.0;
    m_requestedISO = 0;
    sendCommand("SetEnableAuto", "LiveStream", cmd);
    qDebug() << "Setting auto mode";
    return true;
}

bool OriginBackend::getCameraMode()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;
    sendCommand("GetEnableManual", "LiveStream", cmd);
    return true;
}

bool OriginBackend::getCaptureParameters()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;    
    sendCommand("GetCaptureParameters", "Camera", cmd);
    return true;
}

// ============================================================================
// CAMERA INFO
// ============================================================================

bool OriginBackend::getCameraInfo()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;
    sendCommand("GetCameraInfo", "Camera", cmd);
    return true;
}

bool OriginBackend::abortMotion()
{
    if (!isConnected()) {
        return false;
    }

    sendCommand("AbortAxisMovement", "Mount");
    
    m_status.isSlewing = false;
    m_status.currentOperation = "Idle";
    
    return true;
}

// ============================================================================
// PARK AND UNPARK IMPLEMENTATION using Slew command
// ============================================================================

void  OriginBackend::speed(
		  int altRate)  // Negative to move down, using same rate as GUI
{
    QJsonObject slewCommand;
    slewCommand["AltRate"] = altRate;
    slewCommand["AzmRate"] = 0;  // No azimuth motion
    
    sendCommand("Slew", "Mount", slewCommand);
}

void OriginBackend::monitorParkProgress()
{
    const TelescopeData& data = m_dataProcessor->getData();
    double currentAlt = data.orientation.altitude;
    
    qDebug() << "Park monitoring - Target:" << m_targetAltitude 
             << "Current:" << currentAlt;
    
    if (m_parkingInProgress) {
        // Check if we've reached park altitude (-10°)
        if (currentAlt < m_targetAltitude) {
            qDebug() << "Park position reached!";
            
            // Stop any motion
            stopParkMotion();
            
            // Mark as parked
            m_status.isParked = true;
            m_parkingInProgress = false;
            m_status.currentOperation = "Parked";
            m_parkMonitorTimer->stop();
            
            emit parkCompleted();
        }
	else
	  {
	    //	    speed(fmax(-9.0, m_targetAltitude - currentAlt));
	  }
    }
    else if (m_unparkingInProgress) {
        // Check if we've reached unpark altitude (+60°)
        // Allow 2° tolerance
        if (currentAlt > m_targetAltitude) {
            qDebug() << "Unpark position reached!";
            
            // Stop any motion
            stopParkMotion();
            
            // Now initialize the telescope
            m_status.currentOperation = "Initializing";
            qDebug() << "Starting telescope initialization...";
            
            // Initialize
            initializeTelescope();
            
            // Wait a moment for initialization to start
            QTimer::singleShot(2000, this, [this]() {
                m_status.isParked = false;
                m_unparkingInProgress = false;
                m_status.currentOperation = "Idle";
                m_parkMonitorTimer->stop();
                
                qDebug() << "Unpark completed";
                emit unparkCompleted();
            });
        }
	else
	  {
	    //	    speed(fmin(9.0, m_targetAltitude - currentAlt));
	  }
    }
}

void OriginBackend::stopParkMotion()
{
    qDebug() << "Stopping altitude motion";
    
    // Send Slew command with zero rates to stop
    QJsonObject slewCommand;
    slewCommand["AltRate"] = 0;
    slewCommand["AzmRate"] = 0;
    
    sendCommand("Slew", "Mount", slewCommand);
}

bool OriginBackend::parkMount()
{
    if (!isConnected()) {
        qWarning() << "Cannot park - not connected";
        return false;
    }

    if (m_status.isParked) {
        qDebug() << "Mount already parked";
        return true;
    }

    const TelescopeData& data = m_dataProcessor->getData();
    double currentAlt = data.orientation.altitude;
    
    qDebug() << "Parking telescope";
    qDebug() << "Current altitude:" << currentAlt;
    qDebug() << "Target altitude: -10°";
    
    // Stop tracking first
    setTracking(false);
    
    // Set target altitude
    m_targetAltitude = -10.0;
    m_parkingInProgress = true;
    m_status.isParked = false;
    m_status.currentOperation = "Parking";
    
    emit parkStarted();
    
    // Start slewing down (negative altitude rate)
    // Rate is in degrees per second or similar unit
    int altRate = -9;  // Negative to move down, using same rate as GUI
    
    QJsonObject slewCommand;
    slewCommand["AltRate"] = altRate;
    slewCommand["AzmRate"] = 0;  // No azimuth motion
    
    sendCommand("Slew", "Mount", slewCommand);
    
    // Start monitoring altitude
    m_parkMonitorTimer->start();
    
    // Safety timeout - stop after 60 seconds regardless
    QTimer::singleShot(60000, this, [this]() {
        if (m_parkingInProgress) {
            qWarning() << "Park timeout - stopping motion";
            stopParkMotion();
            m_parkingInProgress = false;
            m_parkMonitorTimer->stop();
            emit trackingError("Park operation timed out");
        }
    });
    
    return true;
}

bool OriginBackend::unparkMount()
{
    if (!isConnected()) {
        qWarning() << "Cannot unpark - not connected";
        return false;
    }

    if (!m_status.isParked) {
        qDebug() << "Mount not parked, unparking anyway";
    }

    const TelescopeData& data = m_dataProcessor->getData();
    double currentAlt = data.orientation.altitude;
    
    qDebug() << "Unparking telescope";
    qDebug() << "Current altitude:" << currentAlt;
    qDebug() << "Target altitude: +60°";
    
    // Set target altitude
    m_targetAltitude = 60.0;
    m_unparkingInProgress = true;
    m_status.isParked = false;
    m_status.currentOperation = "Unparking";
    
    emit unparkStarted();
    
    // Start slewing up (positive altitude rate)
    int altRate = 9;  // Positive to move up, using same rate as GUI
    
    QJsonObject slewCommand;
    slewCommand["AltRate"] = altRate;
    slewCommand["AzmRate"] = 0;  // No azimuth motion
    
    sendCommand("Slew", "Mount", slewCommand);
    
    // Start monitoring altitude
    m_parkMonitorTimer->start();
    
    // Safety timeout - stop after 90 seconds regardless
    QTimer::singleShot(90000, this, [this]() {
        if (m_unparkingInProgress) {
            qWarning() << "Unpark timeout - stopping motion";
            stopParkMotion();
            m_unparkingInProgress = false;
            m_parkMonitorTimer->stop();
            emit trackingError("Unpark operation timed out");
        }
    });
    
    return true;
}

bool OriginBackend::initializeTelescope()
{
    if (!isConnected()) {
        return false;
    }

    // Use current date/time and location for initialization
    QDateTime now = QDateTime::currentDateTime();
    
    QJsonObject params;
    params["Date"] = now.toString("dd MM yyyy");
    params["Time"] = now.toString("HH:mm:ss");
    params["TimeZone"] = "UTC"; // Or get system timezone
    params["Latitude"] = degreesToRadians(52.2); // Default latitude
    params["Longitude"] = degreesToRadians(0.0); // Default longitude
    params["FakeInitialize"] = false;

    sendCommand("RunInitialize", "TaskController", params);
    
    m_status.currentOperation = "Initializing";
    
    return true;
}

bool OriginBackend::moveDirection(int direction, int speed)
{
    if (!isConnected()) {
        return false;
    }

    // Map direction codes to Origin telescope directions
    // 0 = North (Dec+), 1 = South (Dec-), 2 = East (RA+), 3 = West (RA-)
    
    QString axisName;
    QString directionName;
    
    switch(direction) {
        case 0: // North
            axisName = "Dec";
            directionName = "Positive";
            break;
        case 1: // South
            axisName = "Dec";
            directionName = "Negative";
            break;
        case 2: // East
            axisName = "Ra";
            directionName = "Positive";
            break;
        case 3: // West
            axisName = "Ra";
            directionName = "Negative";
            break;
        default:
            return false;
    }

    QJsonObject params;
    params["Axis"] = axisName;
    params["Direction"] = directionName;
    params["Speed"] = speed; // 0-100

    sendCommand("MoveAxis", "Mount", params);
    
    return true;
}

bool OriginBackend::setTracking(bool enabled)
{
    if (!isConnected()) {
        return false;
    }

    QString command = enabled ? "StartTracking" : "StopTracking";
    sendCommand(command, "Mount");
    
    m_status.isTracking = enabled;
    
    return true;
}

bool OriginBackend::isTracking() const
{
    return m_status.isTracking;
}

OriginBackend::TelescopeStatus OriginBackend::status() const
{
    return m_status;
}

double OriginBackend::temperature() const
{
    return m_status.temperature;
}

bool OriginBackend::isExposing() const
{
    return m_isExposing;
}

bool OriginBackend::isImageReady() const
{
    return m_imageReady;
}

#ifndef ORIGIN_NO_GUI
QImage OriginBackend::getLastImage() const
{
    return m_lastImage;
}

void OriginBackend::setLastImage(const QImage& image)
{
    m_lastImage = image;
}

#endif

void OriginBackend::setImageReady(bool ready)
{
    m_imageReady = ready;
}

bool OriginBackend::abortExposure()
{
    if (!isConnected()) {
        return false;
    }

    sendCommand("CancelImaging", "TaskController");
    
    m_isExposing = false;
    
    return true;
}

// Exposure and ISO Control
bool OriginBackend::setCameraExposure(double seconds)
{
    QJsonObject cmd;
    if (!isConnected()) return false;
    cmd["Exposure"] = seconds;
    m_requestedExposure = seconds;
    sendCommand("SetCaptureParameters", "Camera", cmd);
    return true;
}

bool OriginBackend::setCameraBinning(int binning)
{
    QJsonObject cmd;
    if (!isConnected()) return false;
    cmd["Binning"] = binning;
    sendCommand("SetCaptureParameters", "Camera", cmd);
    return true;
}

bool OriginBackend::getFocuserAdvancedSettings()
{
    if (!isConnected()) return false;
    sendCommand("GetFocuserAdvancedSettings", "Focuser");
    return true;
}

bool OriginBackend::setCameraBitDepth(int bitDepth)
{
    QJsonObject cmd;
    if (!isConnected()) return false;
    cmd["BitDepth"] = bitDepth;
    m_requestedBitDepth = bitDepth;
    sendCommand("SetCaptureParameters", "Camera", cmd);
    return true;
}

bool OriginBackend::setCameraISO(int iso)
{
    QJsonObject cmd;
    if (!isConnected()) return false;
    cmd["ISO"] = iso;
    m_requestedISO = iso;
    sendCommand("SetCaptureParameters", "Camera", cmd);
    return true;
}

#ifndef ORIGIN_NO_GUI
QImage OriginBackend::singleShot(int gain, int binning, int exposureTimeMicroseconds)
{
    if (!isConnected()) {
        qWarning() << "Cannot take image - not connected";
        return QImage();
    }

    // Generate a unique session UUID
    QUuid uuid = QUuid::createUuid();
    m_currentImagingSession = uuid.toString(QUuid::WithoutBraces);

    // Set camera parameters first
    QJsonObject cameraParams;
    cameraParams["ISO"] = gain;
    cameraParams["Binning"] = binning;
    cameraParams["Exposure"] = exposureTimeMicroseconds / 1000000.0; // Convert to seconds

    sendCommand("SetCaptureParameters", "Camera", cameraParams);

    // Wait a moment for parameters to be set
    QEventLoop loop500;
    QTimer::singleShot(500, &loop500, &QEventLoop::quit);
    loop500.exec();
 
    // Start imaging
    QJsonObject imagingParams;
    imagingParams["Name"] = QString("AlpacaCapture_%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    imagingParams["Uuid"] = m_currentImagingSession;
    imagingParams["SaveRawImage"] = true;

    sendCommand("RunImaging", "TaskController", imagingParams);
    
    m_isExposing = true;
    m_imageReady = false;

    // Wait for exposure to complete with timeout
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval((exposureTimeMicroseconds / 1000) + 30000); // Exposure time + 30 seconds

    connect(this, &OriginBackend::imageReady, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeoutTimer.start();
    loop.exec();

    m_isExposing = false;

    if (m_imageReady) {
        return m_lastImage;
    } else {
        qWarning() << "Image capture timed out or failed";
        return QImage();
    }
}
#endif

// Also log connection events by modifying these methods:
void OriginBackend::onWebSocketConnected()
{
    qDebug() << "Connected to Origin telescope";
    
    // LOG CONNECTION EVENT - ADD THIS
    logWebSocketMessage("SYSTEM", QString("Connected to %1:%2").arg(m_connectedHost).arg(m_connectedPort));
    
    m_status.isConnected = true;
    qDebug() << "connected";
    
    // Start status updates
    m_statusTimer->start();

    // ADD THIS - Start keep-alive pings
    m_pingTimer->start();

    // Request initial status
    sendCommand("GetStatus", "Mount");
    setCameraBitDepth(16);     // raw 16-bit Bayer snapshots (re-sent after each capture, the camera reverts it)
    
    emit connected();
}

void OriginBackend::onWebSocketDisconnected()
{
    qDebug() << "Disconnected from Origin telescope";
    
    // LOG DISCONNECTION EVENT - ADD THIS
    logWebSocketMessage("SYSTEM", "Disconnected from telescope");
    
    m_status.isConnected = false;
    m_status.isLogicallyConnected = false;
    qDebug() << "disconnected";
    
    if (m_statusTimer->isActive()) {
        m_statusTimer->stop();
    }
    if (m_pingTimer->isActive()) {
        m_pingTimer->stop();
    }
    
    stopContinuousSnapshots();
    recordEvent("disconnected", "websocket connection to the telescope was lost");
    emit disconnected();
}

void OriginBackend::onTextMessageReceived(const QString &message)
{
    logWebSocketMessage("RECV", message);
    
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    
    QJsonObject obj = doc.object();

    // Forward telescope-reported errors to the event feed
    {
        const QString errMsg = obj["ErrorMessage"].toString();
        const int errCode = obj["ErrorCode"].toInt(0);
        const QString cmd = obj["Command"].toString(), type = obj["Type"].toString(), state = obj["State"].toString();
        const QString lower = (cmd + " " + type + " " + state).toLower();
        if (errCode != 0 || !errMsg.isEmpty())
            recordEvent("telescope_error", QString("%1 %2: %3 (code %4)").arg(obj["Source"].toString(), cmd, errMsg, QString::number(errCode)), obj);
        else if (lower.contains("error") || lower.contains("fault") || lower.contains("alert") || lower.contains("warn") || lower.contains("fail") || lower.contains("alarm"))
            recordEvent("telescope_alert", QString("%1 %2 %3 %4").arg(obj["Source"].toString(), cmd, type, state).trimmed(), obj);
    }

    // Still emit for other listeners
    emit messageReceived(obj);
    
    // Process through data processor
    bool processed = m_dataProcessor->processJsonPacket(message.toUtf8());
    if (processed) {
        updateStatusFromProcessor();
    }
    
    QString command = obj["Command"].toString();
    QString type = obj["Type"].toString();
    QString source = obj["Source"].toString();
    
    // Handle camera-specific responses
    if (type == "Response") {
        int errorCode = obj["ErrorCode"].toInt();
        
        if (errorCode != 0) {
            QString errorMsg = obj["ErrorMessage"].toString();
            qWarning() << "Command error:" << errorCode << errorMsg;
            return;
        }
        
        // Process camera responses
        if (command == "GetCaptureParameters") {
            m_currentExposure = obj["Exposure"].toDouble();
            m_currentBinning = obj["Binning"].toInt();
            m_currentISO = obj["ISO"].toInt();

            // Keep the user's settings (cleared on switch to auto) rather than the telescope's reverted ones
            if (m_requestedExposure > 0.0) m_currentExposure = m_requestedExposure;
            if (m_requestedISO > 0) m_currentISO = m_requestedISO;
            
	    // qDebug() << "Capture parameters: Exposure =" << m_currentExposure << "ISO =" << m_currentISO;
            
            emit captureParametersChanged(m_currentExposure, m_currentISO, m_currentBinning);
        }
        else if (command == "GetEnableManual" || 
                 command == "SetEnableManual" || 
                 command == "SetEnableAuto") {
            if (obj.contains("IsManual")) {
                m_cameraManualMode = obj["IsManual"].toBool();
                qDebug() << "Camera mode:" << (m_cameraManualMode ? "Manual" : "Auto");
                emit cameraModeChanged(m_cameraManualMode);
            }
        }
        else if (command == "GetCameraInfo") {
            QString cameraID = obj["CameraID"].toString();
            QString model = obj["CameraModel"].toString();
            qDebug() << "Camera info: ID =" << cameraID << "Model =" << model;
            emit cameraInfoReceived(cameraID, model);
        }
    }

    // Handle image notifications
    if (source == "ImageServer" && 
        command == "NewImageReady" &&
        type == "Notification") {
        
        QString filePath = obj["FileLocation"].toString();
        
        if (!filePath.isEmpty()) {
            // Store metadata
            m_lastImageRa = obj["Ra"].toDouble();
            m_lastImageDec = obj["Dec"].toDouble();
            m_lastImageExposure = obj["ExposureTime"].toDouble();
            m_lastImageFilePath = filePath;
            
            // Determine type by extension
            // Snapshots arrive as ImageType SAMPLE_CAPTURE (currently a .png), live view as LIVE (.jpg)
            bool isTiff = obj["ImageType"].toString() == "SAMPLE_CAPTURE" ||
                         filePath.endsWith(".tiff", Qt::CaseInsensitive) || 
                         filePath.endsWith(".tif", Qt::CaseInsensitive);
            
	    // qDebug() << "Image ready:" << filePath << (isTiff ? "(TIFF snapshot)" : "(JPEG live stream)");

	    // ADD THIS CHECK - Skip live JPEGs during snapshot capture
	    if (!isTiff && m_snapshotInProgress) {
	      qDebug() << "Skipping live JPEG - snapshot in progress";
	      return;  // Don't download live images during snapshot
	    }

            // Download the image
            requestImage(filePath, isTiff);
        }
    }
}

void OriginBackend::updateStatus()
{
    if (!isConnected()) {
        return;
    }
    switch (m_statusRotation % 3) {
        case 0:
            sendCommand("GetStatus", "Mount");
            break;
        case 1:
            sendCommand("GetStatus", "Environment");
            break;
        case 2:
            sendCommand("GetCaptureParameters", "Camera");
            break;
    }
    
    m_statusRotation++;
}

QJsonObject OriginBackend::createCommand(const QString& command, const QString& destination, const QJsonObject& params)
{
    QJsonObject jsonCommand;
    jsonCommand["Command"] = command;
    jsonCommand["Destination"] = destination;
    jsonCommand["SequenceID"] = m_nextSequenceId++;
    jsonCommand["Source"] = "OriginMobileApp";
    jsonCommand["Type"] = "Command";
    
    // Add any parameters
    for (auto it = params.begin(); it != params.end(); ++it) {
        jsonCommand[it.key()] = it.value();
    }
    
    return jsonCommand;
}

// Modify the sendCommand method in OriginBackend.cpp:
void OriginBackend::sendCommand(const QString& command, const QString& destination, const QJsonObject& params)
{
    QJsonObject jsonCommand = createCommand(command, destination, params);
    
    QJsonDocument doc(jsonCommand);
    QString message = doc.toJson(QJsonDocument::Compact);  // Use Compact for cleaner logs
    
    if (isConnected()) {
        // LOG THE OUTGOING MESSAGE - ADD THIS
        logWebSocketMessage("SEND", message);
        
        m_webSocket->sendTextMessage(message);
        
        // Store the command for potential response tracking
        int sequenceId = jsonCommand["SequenceID"].toInt();
        m_pendingCommands[sequenceId] = command;
        
        // qDebug() << "Sent command:" << command << "to" << destination;
    } else {
        qWarning() << "Cannot send command - WebSocket not connected";
    }
}

void OriginBackend::updateStatusFromProcessor()
{
    const TelescopeData& data = m_dataProcessor->getData();
    
    // Update mount status
    m_status.isTracking = data.mount.isTracking;
    m_status.isSlewing = !data.mount.isGotoOver;
    m_status.isAligned = data.mount.isAligned;
    
    // Convert coordinates from radians
    m_status.raPosition = radiansToHours(data.mount.enc0); // Assuming enc0 is RA
    m_status.decPosition = radiansToDegrees(data.mount.enc1); // Assuming enc1 is Dec
    
    // Calculate Alt/Az from current mount data if available
    // This would require proper coordinate conversion
    m_status.altPosition = 45.0; // Placeholder
    m_status.azPosition = 180.0; // Placeholder
    
    // Update temperature from environment data
    m_status.temperature = data.environment.ambientTemperature;
    
    // Update operation status
    if (m_status.isSlewing) {
        m_status.currentOperation = "Slewing";
    } else if (m_status.isTracking) {
        m_status.currentOperation = "Tracking";
    } else {
        m_status.currentOperation = "Idle";
    }
    
    emit statusUpdated();
}

void OriginBackend::requestImage(const QString &filePath, bool isSnapshot) {
    if (m_connectedHost.isEmpty()) return;
    
    const QString hostPort = m_connectedPort == 80 ? m_connectedHost : QString("%1:%2").arg(m_connectedHost).arg(m_connectedPort);
    QString fullPath = QString("http://%1/SmartScope-1.0/dev2/%2")
                       .arg(hostPort, filePath);
    QUrl url(fullPath);
    QNetworkRequest request(url);
    
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Accept", "*/*");
    
    bool isTiff = isSnapshot;
    
    // qDebug() << "Downloading:" << fullPath;
    
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QNetworkReply *reply = manager->get(request);
    
    // Store metadata in reply
    reply->setProperty("filePath", filePath);
    reply->setProperty("isTiff", isTiff);
    reply->setProperty("ra", m_lastImageRa);
    reply->setProperty("dec", m_lastImageDec);
    reply->setProperty("exposure", m_lastImageExposure);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager]() {
        if (reply->error() == QNetworkReply::NoError) {
            QByteArray imageData = reply->readAll();
            QString filePath = reply->property("filePath").toString();
            bool isTiff = reply->property("isTiff").toBool();
            double ra = reply->property("ra").toDouble();
            double dec = reply->property("dec").toDouble();
            double exposure = reply->property("exposure").toDouble();
            
            // qDebug() << "Downloaded:" << imageData.size() << "bytes" << (isTiff ? "(TIFF)" : "(JPEG)");
            
            if (isTiff) {
                // Snapshot - emit special signal
                m_snapshotInProgress = false;
                qDebug() << "Snapshot complete - resuming live stream";
                emit tiffImageDownloaded(filePath, imageData, ra, dec, exposure);
                evaluateSnapshotFocus(filePath, imageData, ra, dec, exposure);
                // The telescope resets the live-stream exposure/ISO after a sample capture
                if (m_resendCaptureParams) {
                    if (m_requestedExposure > 0.0) setCameraExposure(m_requestedExposure);
                    if (m_requestedISO > 0) setCameraISO(m_requestedISO);
                    if (m_requestedBitDepth > 0) setCameraBitDepth(m_requestedBitDepth);
                }
                if (m_continuousSnapshots && !takeSingleSnapshot()) {
                    stopContinuousSnapshots();
                }
            } else {
                // Live stream - could emit different signal or handle directly
                // For now, your existing code handles this
		// Try to load as an image
#ifndef ORIGIN_NO_GUI
		QImage image;
		if (image.loadFromData(imageData)) {
		    m_lastImage = image;
#else
		if (!imageData.isEmpty()) {                 // headless build: no image decoder, the encoded bytes are passed on as they are
#endif
		    m_imageReady = true;

		    if (false) qDebug() << "Image downloaded successfully, size:" << imageData.size() << "bytes";
		    emit liveImageDownloaded(imageData, ra, dec, exposure);
		} else {
		    qWarning() << "Failed to load image from downloaded data";
		}
            }
        } else {
            qWarning() << "Download error:" << reply->errorString();
            recordEvent("download_error", reply->errorString());
            m_snapshotInProgress = false;
            if (reply->property("isTiff").toBool()) {
                stopContinuousSnapshots();
            }
        }
        
        reply->deleteLater();
        manager->deleteLater();
    });
}

double OriginBackend::radiansToHours(double radians)
{
    return radians * 12.0 / M_PI; // 12 hours = π radians
}

double OriginBackend::radiansToDegrees(double radians)
{
    return radians * 180.0 / M_PI;
}

double OriginBackend::hoursToRadians(double hours)
{
    return hours * M_PI / 12.0;
}

double OriginBackend::degreesToRadians(double degrees)
{
    return degrees * M_PI / 180.0;
}

// Add this method to OriginBackend.cpp:
void OriginBackend::initializeLogging() {
    // Create logs directory in user's Documents
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString logDir = documentsPath + "/CelestronOriginLogs";
    QDir().mkpath(logDir);
    
    // Create log file with timestamp
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
    QString logFileName = QString("%1/websocket_log_%2.txt").arg(logDir, timestamp);
    
    m_logFile = new QFile(logFileName, this);
    if (m_logFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
        m_logStream = new QTextStream(m_logFile);
        m_logStream->setEncoding(QStringConverter::Utf8);
        
        qDebug() << "WebSocket logging initialized:" << logFileName;
        logWebSocketMessage("SYSTEM", "=== WebSocket Logging Started ===");
    } else {
        qWarning() << "Failed to open log file:" << logFileName;
        delete m_logFile;
        m_logFile = nullptr;
    }
}

// Add this method to OriginBackend.cpp:
void OriginBackend::logWebSocketMessage(const QString& direction, const QString& message) {
    if (!m_logStream) return;
    
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString logEntry = QString("[%1] %2: %3").arg(timestamp, direction, message);
    
    *m_logStream << logEntry << Qt::endl;
    m_logStream->flush();
    
    // Also output to console for immediate viewing
    //       qDebug() << "WS" << direction << ":" << message;
}

// Add this method to OriginBackend.cpp:
void OriginBackend::cleanupLogging() {
    if (m_logStream) {
        logWebSocketMessage("SYSTEM", "=== WebSocket Logging Ended ===");
        delete m_logStream;
        m_logStream = nullptr;
    }
    
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
    }
}

QString OriginBackend::getConnectedHost()
{
  return m_connectedHost;
}

// In OriginBackend.cpp:
void OriginBackend::setCameraConnected(bool connected)
{
    qDebug() << "setCameraConnected called with:" << connected;
    qDebug() << "  Physical connection:" << m_status.isConnected;
    qDebug() << "  Camera logical before:" << m_status.isCameraLogicallyConnected;
    
    if (connected && !m_status.isConnected) {
        qWarning() << "Cannot logically connect camera - no physical connection";
        return;
    }
    
    m_status.isCameraLogicallyConnected = connected;
    qDebug() << "  Camera logical after:" << m_status.isCameraLogicallyConnected;
}

// Sky analysis for every snapshot (focus, transparency, tile coverage, sky level, moon/sun geometry, live stack), kept in memory, logged to
// focus_log.csv next to the snapshots and announced by focusEvaluated(). The decision what to do about it is left to the caller.
void OriginBackend::evaluateSnapshotFocus(const QString& filePath, const QByteArray& imageData, double ra, double dec, double exposure)
{
    QJsonObject o = m_sky.process(imageData, exposure, m_currentISO, ra, dec, QDateTime::currentDateTimeUtc());
    o["file"] = filePath; o["ra_rad"] = ra; o["dec_rad"] = dec;
    m_focusHistory.append(o);
    while (m_focusHistory.size() > 500) m_focusHistory.removeFirst();

    if (o["valid"].toBool()) qInfo().nospace() << "Sky: " << o["verdict"].toString() << ", HFR " << o["hfr_px"].toDouble() << " px from " << o["stars"].toInt() << " stars, transparency "
        << o["registration"].toObject()["transparency"].toDouble() << ", stack " << o["stack"].toObject()["frames"].toInt() << " frames";
    else qInfo() << "Sky: not scored -" << o["note"].toString() << o["verdict"].toString();
    if (o["verdict"].toString() == "unreadable") recordEvent("image_unreadable", "snapshot could not be analysed: " + o["note"].toString());

    QDir dir(originDataDir());
    if (dir.mkpath(".")) {
        QFile log(dir.filePath("focus_log.csv"));
        const bool fresh = !log.exists();
        if (log.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream t(&log);
            const QJsonObject reg = o["registration"].toObject();
            if (fresh) t << "time,file,exposure,target,verdict,stars,hfr_px,hfr_arcsec,hfr_spread_px,transparency,matched_fraction,sky_rate,sky_gradient_sigma,alt_deg,az_deg,moon_alt_deg,moon_separation_deg,stack_frames\n";
            t << o["time"].toString() << ',' << QFileInfo(filePath).fileName() << ',' << exposure << ',' << o["target"].toString() << ',' << o["verdict"].toString() << ',' << o["stars"].toInt() << ','
              << o["hfr_px"].toDouble() << ',' << o["hfr_arcsec"].toDouble() << ',' << o["hfr_spread_px"].toDouble() << ',' << reg["transparency"].toDouble() << ',' << reg["matched_fraction"].toDouble() << ','
              << o["sky_rate"].toDouble() << ',' << o["sky_gradient_sigma"].toDouble() << ',' << o["alt_deg"].toDouble() << ',' << o["az_deg"].toDouble() << ',' << o["moon_alt_deg"].toDouble() << ','
              << o["moon_separation_deg"].toDouble() << ',' << o["stack"].toObject()["frames"].toInt() << '\n';
        }
    }
    emit focusEvaluated(o);
}

void OriginBackend::recordEvent(const QString& kind, const QString& text, const QJsonObject& detail)
{
    // identical consecutive events within 5 s are one event (the telescope repeats status errors every tick)
    if (!m_events.isEmpty()) {
        const QJsonObject& last = m_events.last();
        if (last["kind"].toString() == kind && last["text"].toString() == text && QDateTime::fromString(last["time"].toString(), Qt::ISODateWithMs).msecsTo(QDateTime::currentDateTimeUtc()) < 5000) return;
    }
    QJsonObject e{{"seq", double(++m_eventSeq)}, {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"kind", kind}, {"text", text}};
    if (!detail.isEmpty()) e["detail"] = detail;
    m_events.append(e);
    while (m_events.size() > 500) m_events.removeFirst();
    qWarning().noquote() << "EVENT" << kind << text;
    emit eventRecorded(e);
}

QJsonObject OriginBackend::eventsSince(qint64 afterSeq, int limit) const
{
    QJsonArray a;
    for (const QJsonObject& e : m_events) if (qint64(e["seq"].toDouble()) > afterSeq && a.size() < limit) a.append(e);
    return QJsonObject{{"ok", true}, {"events", a}, {"next", double(m_eventSeq)}, {"connected", m_status.isConnected}};
}

QJsonArray OriginBackend::focusHistory(int count) const
{
    QJsonArray a;
    for (int i = std::max(0, int(m_focusHistory.size()) - count); i < m_focusHistory.size(); ++i) a.append(m_focusHistory[i]);
    return a;
}
