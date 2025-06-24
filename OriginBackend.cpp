#include "OriginBackend.hpp"
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
    , m_webSocket(nullptr)
    , m_dataProcessor(nullptr)
    , m_networkManager(nullptr)
    , m_statusTimer(nullptr)
    , m_connectedPort(80)
    , m_isConnected(false)
    , m_isExposing(false)
    , m_imageReady(false)
    , m_nextSequenceId(2000)
    , m_logFile(nullptr)  // ADD THIS
    , m_logStream(nullptr)  // ADD THIS
{
    m_webSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
    m_dataProcessor = new TelescopeDataProcessor(this);
    m_networkManager = new QNetworkAccessManager(this);
    m_statusTimer = new QTimer(this);

    // Initialize logging - ADD THIS
    initializeLogging();

    // Connect WebSocket signals
    connect(m_webSocket, &QWebSocket::connected, this, &OriginBackend::onWebSocketConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &OriginBackend::onWebSocketDisconnected);
    connect(m_webSocket, &QWebSocket::textMessageReceived, this, &OriginBackend::onTextMessageReceived);

    // Connect data processor signals
    connect(m_dataProcessor, &TelescopeDataProcessor::mountStatusUpdated, 
            this, &OriginBackend::updateStatus);
    connect(m_dataProcessor, &TelescopeDataProcessor::newImageAvailable, 
            this, &OriginBackend::imageReady);

    // Setup status update timer
    m_statusTimer->setInterval(2000); // Update every 2 seconds
    connect(m_statusTimer, &QTimer::timeout, this, &OriginBackend::updateStatus);
}

OriginBackend::~OriginBackend()
{
    disconnectFromTelescope();
    cleanupLogging();  // ADD THIS LINE
}

bool OriginBackend::connectToTelescope(const QString& host, int port)
{
    if (m_isConnected) {
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
    
    return m_isConnected;
}

void OriginBackend::disconnectFromTelescope()
{
    if (m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState) {
        m_webSocket->close();
    }
    
    if (m_statusTimer->isActive()) {
        m_statusTimer->stop();
    }
    
    m_isConnected = false;
    m_status.isConnected = false;
    
    emit disconnected();
}

bool OriginBackend::isConnected() const
{
    return m_isConnected;
}

bool OriginBackend::gotoPosition(double ra, double dec)
{
    if (!m_isConnected) {
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
    if (!m_isConnected) {
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

bool OriginBackend::abortMotion()
{
    if (!m_isConnected) {
        return false;
    }

    sendCommand("AbortAxisMovement", "Mount");
    
    m_status.isSlewing = false;
    m_status.currentOperation = "Idle";
    
    return true;
}

bool OriginBackend::parkMount()
{
    if (!m_isConnected) {
        return false;
    }

    sendCommand("Park", "Mount");
    
    m_status.isParked = true;
    m_status.currentOperation = "Parking";
    
    return true;
}

bool OriginBackend::unparkMount()
{
    if (!m_isConnected) {
        return false;
    }

    sendCommand("Unpark", "Mount");
    
    m_status.isParked = false;
    m_status.currentOperation = "Unparking";
    
    return true;
}

bool OriginBackend::initializeTelescope()
{
    if (!m_isConnected) {
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
    if (!m_isConnected) {
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
    if (!m_isConnected) {
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

QImage OriginBackend::getLastImage() const
{
    return m_lastImage;
}

void OriginBackend::setLastImage(const QImage& image)
{
    m_lastImage = image;
}

void OriginBackend::setImageReady(bool ready)
{
    m_imageReady = ready;
}

bool OriginBackend::abortExposure()
{
    if (!m_isConnected) {
        return false;
    }

    sendCommand("CancelImaging", "TaskController");
    
    m_isExposing = false;
    
    return true;
}

QImage OriginBackend::singleShot(int gain, int binning, int exposureTimeMicroseconds)
{
    if (!m_isConnected) {
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

// Also log connection events by modifying these methods:
void OriginBackend::onWebSocketConnected()
{
    qDebug() << "Connected to Origin telescope";
    
    // LOG CONNECTION EVENT - ADD THIS
    logWebSocketMessage("SYSTEM", QString("Connected to %1:%2").arg(m_connectedHost).arg(m_connectedPort));
    
    m_isConnected = true;
    m_status.isConnected = true;
    
    // Start status updates
    m_statusTimer->start();
    
    // Request initial status
    sendCommand("GetStatus", "System");
    
    emit connected();
}

void OriginBackend::onWebSocketDisconnected()
{
    qDebug() << "Disconnected from Origin telescope";
    
    // LOG DISCONNECTION EVENT - ADD THIS
    logWebSocketMessage("SYSTEM", "Disconnected from telescope");
    
    m_isConnected = false;
    m_status.isConnected = false;
    
    if (m_statusTimer->isActive()) {
        m_statusTimer->stop();
    }
    
    emit disconnected();
}

// Modify the onTextMessageReceived method in OriginBackend.cpp:
void OriginBackend::onTextMessageReceived(const QString &message)
{
    // LOG THE INCOMING MESSAGE - ADD THIS
    logWebSocketMessage("RECV", message);
    
    // Process the message through the data processor
    bool processed = m_dataProcessor->processJsonPacket(message.toUtf8());
    
    if (processed) {
        updateStatusFromProcessor();
    }

    // Check for image ready notifications
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        
        if (obj["Source"].toString() == "ImageServer" && 
            obj["Command"].toString() == "NewImageReady" &&
            obj["Type"].toString() == "Notification") {
            
            QString filePath = obj["FileLocation"].toString();
            if (!filePath.isEmpty()) {
                requestImage(filePath);
            }
        }
    }
}

void OriginBackend::onImageDownloaded()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray imageData = reply->readAll();
        
        // Try to load as an image
        QImage image;
        if (image.loadFromData(imageData)) {
            m_lastImage = image;
            m_imageReady = true;
            
            qDebug() << "Image downloaded successfully, size:" << imageData.size() << "bytes";
            emit imageReady();
        } else {
            qWarning() << "Failed to load image from downloaded data";
        }
    } else {
        qWarning() << "Error downloading image:" << reply->errorString();
    }

    reply->deleteLater();
}

void OriginBackend::updateStatus()
{
    if (!m_isConnected) {
        return;
    }

    // Request status updates from various components
    sendCommand("GetStatus", "Mount");
    sendCommand("GetStatus", "Environment");
    sendCommand("GetCaptureParameters", "Camera");
}

QJsonObject OriginBackend::createCommand(const QString& command, const QString& destination, const QJsonObject& params)
{
    QJsonObject jsonCommand;
    jsonCommand["Command"] = command;
    jsonCommand["Destination"] = destination;
    jsonCommand["SequenceID"] = m_nextSequenceId++;
    jsonCommand["Source"] = "AlpacaServer";
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
    
    if (m_webSocket->isValid() && m_webSocket->state() == QAbstractSocket::ConnectedState) {
        // LOG THE OUTGOING MESSAGE - ADD THIS
        logWebSocketMessage("SEND", message);
        
        m_webSocket->sendTextMessage(message);
        
        // Store the command for potential response tracking
        int sequenceId = jsonCommand["SequenceID"].toInt();
        m_pendingCommands[sequenceId] = command;
        
        qDebug() << "Sent command:" << command << "to" << destination;
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

void OriginBackend::requestImage(const QString& filePath)
{
    if (m_connectedHost.isEmpty()) {
        return;
    }

    // Construct the URL for image download
    QString fullPath = QString("http://%1/SmartScope-1.0/dev2/%2").arg(m_connectedHost, filePath);
    QUrl url(fullPath);
    QNetworkRequest request(url);
    
    // Set appropriate headers
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Accept", "*/*");
    request.setRawHeader("User-Agent", "OriginAlpacaServer");
    request.setRawHeader("Connection", "keep-alive");
    
    qDebug() << "Requesting image from:" << fullPath;
    
    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, &OriginBackend::onImageDownloaded);
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
    qDebug() << "WS" << direction << ":" << message;
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
