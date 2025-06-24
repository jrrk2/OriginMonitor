#include "AlpacaServer.hpp"
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#define ALPACA_API_VERSION   "1"
#define ALPACA_DISCOVERY_PORT 32227

AlpacaServer::AlpacaServer(QObject *parent)
    : QObject(parent)
    , m_running(false)
    , m_telescopeBackend(nullptr)
    , m_transactionCounter(0)
    , m_serverName("Celestron Origin Alpaca Server")
    , m_manufacturer("Celestron Origin Project")
    , m_manufacturerVersion("1.0.0")
    , m_location("")
    , m_port(11111)
    , m_instanceNumber(0)
{
    // Connect discovery timer
    connect(&m_discoveryTimer, &QTimer::timeout, this, &AlpacaServer::sendDiscoveryBroadcast);
    
    // Generate a unique server name with hostname
    QString hostname = QHostInfo::localHostName();
    m_serverName = QString("Celestron Origin Alpaca Server on %1").arg(hostname);
    
    // Set default location (empty means server root)
    m_location = "";
}

AlpacaServer::~AlpacaServer()
{
    stop();
}

bool AlpacaServer::start(int port)
{
    if (m_running) {
        qDebug() << "Alpaca server already running";
        return true;
    }
    
    if (!m_telescopeBackend) {
        qWarning() << "Cannot start Alpaca server - no telescope backend";
        return false;
    }
    
    m_port = port;
    
    // Setup API endpoints
    setupEndpoints();
    
    bool rslt = m_tcpserver.listen(QHostAddress::Any, m_port);
    if (rslt == false) {
        qWarning() << "Failed to start listening Alpaca server on port" << m_port;
        return false;
    }

    bool bind = m_server.bind(&m_tcpserver);
    if (bind == false) {
        qWarning() << "Failed to bind Alpaca server on port" << m_port;
        return false;
    }
    
    m_running = true;
    qDebug() << "Alpaca server started on port" << m_port;
    
    // Start discovery broadcast
    startDiscoveryBroadcast();
    
    emit serverStarted();
    
    return true;
}

void AlpacaServer::stop()
{
    if (!m_running) {
        return;
    }
    
    // Stop discovery broadcast
    stopDiscoveryBroadcast();
    
    m_running = false;
    qDebug() << "Alpaca server stopped";
    
    emit serverStopped();
}

bool AlpacaServer::isRunning() const
{
    return m_running;
}

void AlpacaServer::setTelescopeBackend(OriginBackend* backend)
{
    m_telescopeBackend = backend;
}

// Management API Endpoints

QJsonObject AlpacaServer::handleManagementVersions(const QHttpServerRequest& request)
{
    QJsonArray versions;
    versions.append(1);  // We support Alpaca API v1
    
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(versions, transaction);
}

QJsonObject AlpacaServer::handleManagementDescription(const QHttpServerRequest& request)
{
    QJsonObject description;
    description["ServerName"] = m_serverName;
    description["Manufacturer"] = m_manufacturer;
    description["ManufacturerVersion"] = m_manufacturerVersion;
    description["Location"] = m_location;
    
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(description, transaction);
}

QJsonObject AlpacaServer::handleManagementConfiguredDevices(const QHttpServerRequest& request)
{
    QJsonArray devices;
    
    // Add the telescope device
    QJsonObject telescope;
    telescope["DeviceName"] = "Celestron Origin Telescope";
    telescope["DeviceType"] = "Telescope";
    telescope["DeviceNumber"] = 0;
    telescope["UniqueID"] = "CelestronOrigin_Telescope_0";
    devices.append(telescope);
    
    // Add the camera device
    QJsonObject camera;
    camera["DeviceName"] = "Celestron Origin Camera";
    camera["DeviceType"] = "Camera";
    camera["DeviceNumber"] = 0;
    camera["UniqueID"] = "CelestronOrigin_Camera_0";
    devices.append(camera);
    
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(devices, transaction);
}

// Common Device API Endpoints

QJsonObject AlpacaServer::handleDeviceConnected(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (command) {
        // PUT - Set the connected state
        if (request.method() != QHttpServerRequest::Method::Put) {
            return createErrorResponse(1, "Method not allowed");
        }
        
        // Parse request body
        QMap<QString, QVariant> params;
        if (!parseRequestBody(request, params)) {
            return createErrorResponse(1002, "Invalid parameters");
        }
        
        // Look for 'Connected' parameter
        QVariant connectedVar;
        if (params.contains("Connected")) {
            connectedVar = params["Connected"];
        } else if (params.contains("connected")) {
            connectedVar = params["connected"];
        } else {
            return createErrorResponse(1002, "Missing 'Connected' parameter");
        }
        
        // Convert to bool
        bool connected = false;
        if (connectedVar.type() == QVariant::Bool) {
            connected = connectedVar.toBool();
        } else {
            QString connectedStr = connectedVar.toString().toLower();
            connected = (connectedStr == "true" || connectedStr == "1");
        }
        
        qDebug() << "Handling connection request, connected=" << connected;
        
        if (connected && !m_telescopeBackend->isConnected()) {
            // Connect to the telescope - use default Origin settings
            QString host = "192.168.1.100"; // Default Origin IP or discover
            int port = 80; // Origin uses port 80
            
            bool success = m_telescopeBackend->connectToTelescope(host, port);
            if (!success) {
                return createErrorResponse(1, "Failed to connect to telescope");
            }
        }
        else if (!connected && m_telescopeBackend->isConnected()) {
            // Disconnect from the telescope
            m_telescopeBackend->disconnectFromTelescope();
        }
        
        return createSuccessResponse(m_telescopeBackend->isConnected(), transaction);
    }
    else {
        // GET - Return the connected state
        return createSuccessResponse(m_telescopeBackend->isConnected(), transaction);
    }
}

QJsonObject AlpacaServer::handleDeviceDescription(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse("Celestron Origin Telescope", transaction);
}

QJsonObject AlpacaServer::handleDeviceDriverInfo(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse("Celestron Origin Alpaca Driver v1.0", transaction);
}

QJsonObject AlpacaServer::handleDeviceDriverVersion(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse("1.0", transaction);
}

QJsonObject AlpacaServer::handleDeviceInterfaceVersion(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(1, transaction);
}

QJsonObject AlpacaServer::handleDeviceName(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse("Celestron Origin", transaction);
}

QJsonObject AlpacaServer::handleDeviceSupportedActions(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    QJsonArray actions;
    // Add any custom actions your telescope supports
    
    return createSuccessResponse(actions, transaction);
}

// Telescope-specific API Endpoints

QJsonObject AlpacaServer::handleTelescopeAlignmentMode(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    // 0 = altAz, 1 = polar, 2 = german
    return createSuccessResponse(0, transaction); // Origin is Alt-Az mount
}

QJsonObject AlpacaServer::handleTelescopeAltitude(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    OriginBackend::TelescopeStatus status = m_telescopeBackend->status();
    return createSuccessResponse(status.altPosition, transaction);
}

QJsonObject AlpacaServer::handleTelescopeAzimuth(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    OriginBackend::TelescopeStatus status = m_telescopeBackend->status();
    return createSuccessResponse(status.azPosition, transaction);
}

QJsonObject AlpacaServer::handleTelescopeDeclination(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    OriginBackend::TelescopeStatus status = m_telescopeBackend->status();
    return createSuccessResponse(status.decPosition, transaction);
}

QJsonObject AlpacaServer::handleTelescopeRightAscension(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    OriginBackend::TelescopeStatus status = m_telescopeBackend->status();
    return createSuccessResponse(status.raPosition, transaction);
}

QJsonObject AlpacaServer::handleTelescopeApertureArea(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    // Area in square meters (Origin has a 150mm aperture)
    double area = 3.1415926 * 0.075 * 0.075; // π * (d/2)²
    return createSuccessResponse(area, transaction);
}

QJsonObject AlpacaServer::handleTelescopeApertureDiameter(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    // Diameter in meters (Origin has a 150mm aperture)
    return createSuccessResponse(0.150, transaction);
}

QJsonObject AlpacaServer::handleTelescopeAtHome(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    // Determine if telescope is at home position
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleTelescopeAtPark(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    OriginBackend::TelescopeStatus status = m_telescopeBackend->status();
    return createSuccessResponse(status.isParked, transaction);
}

QJsonObject AlpacaServer::handleTelescopeSlewing(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    OriginBackend::TelescopeStatus status = m_telescopeBackend->status();
    return createSuccessResponse(status.isSlewing, transaction);
}

QJsonObject AlpacaServer::handleTelescopeTracking(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    if (command) {
        // PUT - Set tracking state
        if (request.method() != QHttpServerRequest::Method::Put) {
            return createErrorResponse(1, "Method not allowed");
        }
        
        QMap<QString, QVariant> params;
        if (!parseRequestBody(request, params)) {
            return createErrorResponse(1002, "Invalid parameters");
        }
        
        if (!params.contains("Tracking")) {
            return createErrorResponse(1002, "Missing Tracking parameter");
        }
        
        bool tracking = params["Tracking"].toBool();
        bool success = m_telescopeBackend->setTracking(tracking);
        
        if (!success) {
            return createErrorResponse(1, "Failed to set tracking");
        }
        
        return createSuccessResponse(true, transaction);
    }
    else {
        // GET - Return tracking state
        OriginBackend::TelescopeStatus status = m_telescopeBackend->status();
        return createSuccessResponse(status.isTracking, transaction);
    }
}

// Telescope capability methods
QJsonObject AlpacaServer::handleTelescopeCanFindHome(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction); // Origin supports initialization
}

QJsonObject AlpacaServer::handleTelescopeCanPark(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction); // Origin supports parking
}

QJsonObject AlpacaServer::handleTelescopeCanPulseGuide(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction); // Origin doesn't support pulse guiding
}

QJsonObject AlpacaServer::handleTelescopeCanSetTracking(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSlew(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSlewAltAz(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSlewAsync(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSync(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanUnpark(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

// Telescope action methods
QJsonObject AlpacaServer::handleTelescopeAbortSlew(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    bool success = m_telescopeBackend->abortMotion();
    if (!success) {
        return createErrorResponse(1, "Failed to abort slew");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopePark(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    bool success = m_telescopeBackend->parkMount();
    if (!success) {
        return createErrorResponse(1, "Failed to park telescope");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeUnpark(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    bool success = m_telescopeBackend->unparkMount();
    if (!success) {
        return createErrorResponse(1, "Failed to unpark telescope");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeFindHome(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    bool success = m_telescopeBackend->initializeTelescope();
    if (!success) {
        return createErrorResponse(1, "Failed to initialize telescope");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeSlewToCoordinates(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    // Parse request body for coordinates
    QMap<QString, QVariant> params;
    if (!parseRequestBody(request, params)) {
        return createErrorResponse(1002, "Invalid parameters");
    }
    
    if (!params.contains("RightAscension") || !params.contains("Declination")) {
        return createErrorResponse(1002, "Missing coordinates");
    }
    
    double ra = params["RightAscension"].toDouble();
    double dec = params["Declination"].toDouble();
    
    // Validate coordinates
    if (ra < 0 || ra >= 24) {
        return createErrorResponse(1025, "Invalid RightAscension value");
    }
    if (dec < -90 || dec > 90) {
        return createErrorResponse(1025, "Invalid Declination value");
    }
    
    bool success = m_telescopeBackend->gotoPosition(ra, dec);
    if (!success) {
        return createErrorResponse(1, "Failed to slew to coordinates");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeSyncToCoordinates(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    // Parse request body for coordinates
    QMap<QString, QVariant> params;
    if (!parseRequestBody(request, params)) {
        return createErrorResponse(1002, "Invalid parameters");
    }
    
    if (!params.contains("RightAscension") || !params.contains("Declination")) {
        return createErrorResponse(1002, "Missing coordinates");
    }
    
    double ra = params["RightAscension"].toDouble();
    double dec = params["Declination"].toDouble();
    
    // Validate coordinates
    if (ra < 0 || ra >= 24) {
        return createErrorResponse(1025, "Invalid RightAscension value");
    }
    if (dec < -90 || dec > 90) {
        return createErrorResponse(1025, "Invalid Declination value");
    }
    
    bool success = m_telescopeBackend->syncPosition(ra, dec);
    if (!success) {
        return createErrorResponse(1, "Failed to sync to coordinates");
    }
    
    return createSuccessResponse(true, transaction);
}

// Camera endpoints implementation
QJsonObject AlpacaServer::handleCameraState(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    // Camera states: 0=Idle, 1=Waiting, 2=Exposing, 3=Reading, 4=Download, 5=Error
    int cameraState = 0; // Default to idle
    
    if (m_telescopeBackend && m_telescopeBackend->isExposing()) {
        cameraState = 2; // Exposing
    }
    
    return createSuccessResponse(cameraState, transaction);
}

QJsonObject AlpacaServer::handleCameraImageReady(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    bool imageReady = false;
    if (m_telescopeBackend) {
        imageReady = m_telescopeBackend->isImageReady();
    }
    
    return createSuccessResponse(imageReady, transaction);
}

QJsonObject AlpacaServer::handleCameraStartExposure(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    // Parse parameters from the request
    QMap<QString, QVariant> params;
    if (!parseRequestBody(request, params)) {
        return createErrorResponse(1002, "Invalid parameters");
    }
    
    if (!params.contains("Duration")) {
        return createErrorResponse(1002, "Missing Duration parameter");
    }
    
    double duration = params["Duration"].toDouble();
    if (duration <= 0) {
        return createErrorResponse(1025, "Invalid exposure duration");
    }
    
    // Get optional parameters
    int gain = params.value("Gain", 50).toInt();
    int binning = 1; // Default binning
    
    // Start exposure using the backend
    QImage image = m_telescopeBackend->singleShot(gain, binning, (int)(duration * 1000000));
    
    if (!image.isNull()) {
        m_telescopeBackend->setLastImage(image);
        m_telescopeBackend->setImageReady(true);
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleCameraAbortExposure(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    bool success = false;
    if (m_telescopeBackend) {
        success = m_telescopeBackend->abortExposure();
    }
    
    if (!success) {
        return createErrorResponse(1, "Failed to abort exposure");
    }
    
    return createSuccessResponse(true, transaction);
}

// Camera property endpoints with Origin-specific values
QJsonObject AlpacaServer::handleCameraCameraXSize(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(4144, transaction); // Origin camera width
}

QJsonObject AlpacaServer::handleCameraCameraYSize(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(2822, transaction); // Origin camera height
}

QJsonObject AlpacaServer::handleCameraPixelSizeX(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(4.63, transaction); // Origin pixel size in microns
}

QJsonObject AlpacaServer::handleCameraPixelSizeY(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(4.63, transaction); // Origin pixel size in microns
}

QJsonObject AlpacaServer::handleCameraSensorName(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse("Origin Camera Sensor", transaction);
}

QJsonObject AlpacaServer::handleCameraSensorType(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(1, transaction); // 1 = Color sensor
}

QJsonObject AlpacaServer::handleCameraName(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse("Celestron Origin Camera", transaction);
}

QJsonObject AlpacaServer::handleCameraDriverInfo(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse("Celestron Origin Camera Driver v1.0", transaction);
}

QJsonObject AlpacaServer::handleCameraDriverVersion(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse("1.0", transaction);
}

QJsonObject AlpacaServer::handleCameraBinX(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(1, transaction); // Origin supports 1x1 binning
}

QJsonObject AlpacaServer::handleCameraBinY(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(1, transaction); // Origin supports 1x1 binning
}

QJsonObject AlpacaServer::handleCameraMaxBinX(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(1, transaction);
}

QJsonObject AlpacaServer::handleCameraMaxBinY(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(1, transaction);
}

QJsonObject AlpacaServer::handleCameraNumX(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(4144, transaction); // Full frame width
}

QJsonObject AlpacaServer::handleCameraNumY(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(2822, transaction); // Full frame height
}

QJsonObject AlpacaServer::handleCameraStartX(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0, transaction);
}

QJsonObject AlpacaServer::handleCameraStartY(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0, transaction);
}

QJsonObject AlpacaServer::handleCameraGain(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(100, transaction); // Default gain
}

QJsonObject AlpacaServer::handleCameraGainMin(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0, transaction);
}

QJsonObject AlpacaServer::handleCameraGainMax(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(300, transaction);
}

QJsonObject AlpacaServer::handleCameraGains(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    QJsonArray gains;
    for (int i = 0; i <= 300; i += 10) {
        gains.append(i);
    }
    
    return createSuccessResponse(gains, transaction);
}

QJsonObject AlpacaServer::handleCameraExposureMin(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0.001, transaction); // 1ms minimum
}

QJsonObject AlpacaServer::handleCameraExposureMax(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(3600.0, transaction); // 1 hour maximum
}

QJsonObject AlpacaServer::handleCameraExposureResolution(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0.001, transaction);
}

QJsonObject AlpacaServer::handleCameraMaxADU(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(65535, transaction); // 16-bit
}

QJsonObject AlpacaServer::handleCameraCanAbortExposure(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleCameraCoolerOn(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction); // No cooler
}

QJsonObject AlpacaServer::handleCameraCanGetCoolerPower(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleCameraCanSetCCDTemperature(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleCameraCCDTemperature(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    double temp = 20.0; // Default
    if (m_telescopeBackend && m_telescopeBackend->isConnected()) {
        temp = m_telescopeBackend->temperature();
    }
    
    return createSuccessResponse(temp, transaction);
}

QJsonObject AlpacaServer::handleCameraCanFastReadout(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleCameraReadoutModes(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    QJsonArray modes;
    modes.append("Normal");
    
    return createSuccessResponse(modes, transaction);
}

QJsonObject AlpacaServer::handleCameraBayerOffsetX(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0, transaction);
}

QJsonObject AlpacaServer::handleCameraBayerOffsetY(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0, transaction);
}

QHttpServerResponse AlpacaServer::handleCameraImageArray(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    // Check if we're connected and an image is ready
    if (!m_telescopeBackend || !m_telescopeBackend->isConnected()) {
        QJsonObject errorJson = createErrorResponse(1031, "Not connected to camera");
        return QHttpServerResponse(QJsonDocument(errorJson).toJson(), "application/json");
    }
    
    if (!m_telescopeBackend->isImageReady()) {
        QJsonObject errorJson = createErrorResponse(1, "No image is ready");
        return QHttpServerResponse(QJsonDocument(errorJson).toJson(), "application/json");
    }
    
    // Get the image from the backend
    QImage image = m_telescopeBackend->getLastImage();
    if (image.isNull()) {
        QJsonObject errorJson = createErrorResponse(1, "Failed to get image");
        return QHttpServerResponse(QJsonDocument(errorJson).toJson(), "application/json");
    }
    
    // Check the Accept header to determine response format
    bool useImageBytes = false;
    if (request.headers().contains("accept")) {
        QString acceptValue = QString::fromUtf8(request.headers().value("accept")).toLower();
        if (acceptValue.contains("application/imagebytes")) {
            useImageBytes = true;
        }
    }
    
    // If client accepts 'application/imagebytes', return binary format
    if (useImageBytes) {
        int width = image.width();
        int height = image.height();
        
        // Create a buffer for binary data
        QByteArray binaryData;
        
        // Calculate required buffer size
        int headerSize = 44; // Fixed header size for version 1
        int dataSize = width * height * 2; // 2 bytes per pixel for 16-bit grayscale
        binaryData.resize(headerSize + dataSize);
        
        // Get a pointer to the buffer
        char* buffer = binaryData.data();
        
        // Fill the header
        *reinterpret_cast<quint32*>(buffer) = 1; // Metadata version
        *reinterpret_cast<qint32*>(buffer + 4) = 0; // Error number
        *reinterpret_cast<quint32*>(buffer + 8) = transaction.clientTransactionID;
        *reinterpret_cast<quint32*>(buffer + 12) = m_transactionCounter++;
        *reinterpret_cast<quint32*>(buffer + 16) = headerSize; // Data start offset
        *reinterpret_cast<quint32*>(buffer + 20) = 2; // Int16 element type
        *reinterpret_cast<quint32*>(buffer + 24) = 2; // Int16 transmission type
        *reinterpret_cast<quint32*>(buffer + 28) = 2; // Rank (2D)
        *reinterpret_cast<quint32*>(buffer + 32) = width; // Dimension 1
        *reinterpret_cast<quint32*>(buffer + 36) = height; // Dimension 2
        *reinterpret_cast<quint32*>(buffer + 40) = 0; // Dimension 3
        
        // Fill the image data
        char* dataPtr = buffer + headerSize;
        quint16* pixelData = reinterpret_cast<quint16*>(dataPtr);
        
        // Convert image to 16-bit grayscale
        if (image.format() == QImage::Format_Grayscale8) {
            for (int y = 0; y < height; y++) {
                const uchar* scanLine = image.constScanLine(y);
                for (int x = 0; x < width; x++) {
                    int index = y * width + x;
                    pixelData[index] = static_cast<quint16>(scanLine[x] * 257);
                }
            }
        } else {
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int index = y * width + x;
                    QRgb pixel = image.pixel(x, y);
                    pixelData[index] = static_cast<quint16>(qGray(pixel) * 257);
                }
            }
        }
        
        return QHttpServerResponse("application/imagebytes", binaryData);
    }
    // Otherwise, return standard JSON array format
    else {
        int width = image.width();
        int height = image.height();
        
        QJsonArray pixelArray;
        
        // Convert to 16-bit grayscale values
        if (image.format() == QImage::Format_Grayscale8) {
            for (int y = 0; y < height; y++) {
                const uchar* scanLine = image.constScanLine(y);
                for (int x = 0; x < width; x++) {
                    int pixelValue = static_cast<int>(scanLine[x]) * 257;
                    pixelArray.append(pixelValue);
                }
            }
        } else {
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    QRgb pixel = image.pixel(x, y);
                    int gray = qGray(pixel) * 257;
                    pixelArray.append(gray);
                }
            }
        }
        
        QJsonObject jsonResponse = createSuccessResponse(pixelArray, transaction);
        return QHttpServerResponse(QJsonDocument(jsonResponse).toJson(), "application/json");
    }
}

// Utility methods

bool AlpacaServer::parseRequestBody(const QHttpServerRequest& request, QMap<QString, QVariant>& result)
{
    QByteArray body = request.body();
    if (body.isEmpty()) {
        qDebug() << "Request body is empty";
        return false;
    }
    
    // Check Content-Type header
    QString contentType;
    auto headerValue = request.headers().value("content-type");
    if (!headerValue.isEmpty()) {
        contentType = QString::fromUtf8(headerValue.data(), headerValue.size()).toLower();
    }
    
    // Try JSON parsing first
    if (contentType.contains("application/json") || body.trimmed().startsWith('{')) {
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            QJsonObject jsonBody = doc.object();
            for (auto it = jsonBody.constBegin(); it != jsonBody.constEnd(); ++it) {
                result[it.key()] = it.value().toVariant();
            }
            return !result.isEmpty();
        }
    }
    
    // Try form data parsing
    QString bodyStr = QString::fromUtf8(body);
    QUrlQuery query(bodyStr);
    
    QList<QPair<QString, QString>> items = query.queryItems();
    if (!items.isEmpty()) {
        for (const auto& item : items) {
            result[item.first] = item.second;
        }
        return true;
    }
    
    // Direct string splitting as fallback
    QStringList params = bodyStr.split('&');
    for (const QString& param : params) {
        QStringList keyValue = param.split('=');
        if (keyValue.size() == 2) {
            result[keyValue[0]] = QUrl::fromPercentEncoding(keyValue[1].toUtf8());
        }
    }
    
    return !result.isEmpty();
}

QJsonObject AlpacaServer::createErrorResponse(int errorNumber, const QString& errorMessage)
{
    QJsonObject response;
    response["ErrorNumber"] = errorNumber;
    response["ErrorMessage"] = errorMessage;
    response["ClientTransactionID"] = 0;
    response["ServerTransactionID"] = m_transactionCounter++;
    response["Value"] = QJsonValue::Null;
    
    return response;
}

QJsonObject AlpacaServer::createSuccessResponse(const QJsonValue& value, const ClientTransaction& transaction)
{
    QJsonObject response;
    response["ErrorNumber"] = 0;
    response["ErrorMessage"] = "";
    response["ClientTransactionID"] = transaction.clientTransactionID;
    response["ServerTransactionID"] = m_transactionCounter++;
    response["Value"] = value;
    
    return response;
}

AlpacaServer::ClientTransaction AlpacaServer::parseClientTransaction(const QHttpServerRequest& request)
{
    ClientTransaction transaction;
    
    QUrlQuery query(request.url().query());
    
    if (query.hasQueryItem("ClientID")) {
        transaction.clientID = query.queryItemValue("ClientID").toInt();
    }
    
    if (query.hasQueryItem("ClientTransactionID")) {
        transaction.clientTransactionID = query.queryItemValue("ClientTransactionID").toInt();
    }
    
    transaction.transactionID = QUuid::createUuid().toString();
    
    return transaction;
}

// Discovery protocol implementation

void AlpacaServer::startDiscoveryBroadcast()
{
    m_discoveryTimer.start(30000); // Send discovery broadcast every 30 seconds
    sendDiscoveryBroadcast(); // Send one immediately
}

void AlpacaServer::stopDiscoveryBroadcast()
{
    m_discoveryTimer.stop();
}

void AlpacaServer::sendDiscoveryBroadcast()
{
    QUdpSocket socket;
    
    // Prepare discovery message (JSON format)
    QJsonObject message;
    message["AlpacaPort"] = m_port;
    
    QJsonDocument doc(message);
    QByteArray datagram = doc.toJson(QJsonDocument::Compact);
    
    // Send to broadcast address on Alpaca discovery port
    socket.writeDatagram(datagram, QHostAddress::Broadcast, ALPACA_DISCOVERY_PORT);
    
    qDebug() << "Sent Alpaca discovery broadcast on port" << ALPACA_DISCOVERY_PORT;
}

// Setup all API endpoints
void AlpacaServer::setupEndpoints()
{
    // Convert enum to string for signals
    auto methodToString = [](QHttpServerRequest::Method method) -> QString {
        switch (method) {
            case QHttpServerRequest::Method::Get:    return "GET";
            case QHttpServerRequest::Method::Put:    return "PUT";
            case QHttpServerRequest::Method::Post:   return "POST";
            case QHttpServerRequest::Method::Delete: return "DELETE";
            case QHttpServerRequest::Method::Options:return "OPTIONS";
            case QHttpServerRequest::Method::Head:   return "HEAD";
            case QHttpServerRequest::Method::Patch:  return "PATCH";
            default: return "UNKNOWN";
        }
    };

    // Management API endpoints
    m_server.route("/management/apiversions", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleManagementVersions(request);
    });
    
    m_server.route("/api/v1/alpaca/management/apiversions", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleManagementVersions(request);
    });
    
    m_server.route("/management/v1/description", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleManagementDescription(request);
    });
    
    m_server.route("/api/v1/alpaca/management/v1/description", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleManagementDescription(request);
    });
    
    m_server.route("/management/v1/configureddevices", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleManagementConfiguredDevices(request);
    });
    
    m_server.route("/api/v1/alpaca/management/v1/configureddevices", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleManagementConfiguredDevices(request);
    });

    // Setup telescope and camera endpoints
    const char *devicePaths[] = {"/api/v1/telescope/0", "/api/v1/camera/0"};
    
    for (const QString& devicePath : devicePaths) {
        // Common device properties
        m_server.route(devicePath + "/connected", [this, methodToString](const QHttpServerRequest& request) {
            emit requestReceived(methodToString(request.method()), request.url().path());
            if (request.method() == QHttpServerRequest::Method::Put) {
                return this->handleDeviceConnected(request, true);
            } else {
                return this->handleDeviceConnected(request);
            }
        });
        
        m_server.route(devicePath + "/description", [this, methodToString](const QHttpServerRequest& request) {
            emit requestReceived(methodToString(request.method()), request.url().path());
            return this->handleDeviceDescription(request);
        });
        
        m_server.route(devicePath + "/driverinfo", [this, methodToString](const QHttpServerRequest& request) {
            emit requestReceived(methodToString(request.method()), request.url().path());
            return this->handleDeviceDriverInfo(request);
        });
        
        m_server.route(devicePath + "/driverversion", [this, methodToString](const QHttpServerRequest& request) {
            emit requestReceived(methodToString(request.method()), request.url().path());
            return this->handleDeviceDriverVersion(request);
        });
        
        m_server.route(devicePath + "/interfaceversion", [this, methodToString](const QHttpServerRequest& request) {
            emit requestReceived(methodToString(request.method()), request.url().path());
            return this->handleDeviceInterfaceVersion(request);
        });
        
        m_server.route(devicePath + "/name", [this, methodToString](const QHttpServerRequest& request) {
            emit requestReceived(methodToString(request.method()), request.url().path());
            return this->handleDeviceName(request);
        });
    }
    
    // Telescope-specific endpoints
    const QString telescopePath = "/api/v1/telescope/0";
    
    m_server.route(telescopePath + "/altitude", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeAltitude(request);
    });
    
    m_server.route(telescopePath + "/azimuth", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeAzimuth(request);
    });
    
    m_server.route(telescopePath + "/declination", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeDeclination(request);
    });
    
    m_server.route(telescopePath + "/rightascension", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeRightAscension(request);
    });
    
    m_server.route(telescopePath + "/slewing", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeSlewing(request);
    });
    
    m_server.route(telescopePath + "/tracking", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        if (request.method() == QHttpServerRequest::Method::Put) {
            return this->handleTelescopeTracking(request, true);
        } else {
            return this->handleTelescopeTracking(request);
        }
    });
    
    m_server.route(telescopePath + "/canpark", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeCanPark(request);
    });
    
    m_server.route(telescopePath + "/canslew", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeCanSlew(request);
    });
    
    // Telescope actions
    m_server.route(telescopePath + "/abortslew", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeAbortSlew(request);
    });
    
    m_server.route(telescopePath + "/park", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopePark(request);
    });
    
    m_server.route(telescopePath + "/unpark", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeUnpark(request);
    });
    
    m_server.route(telescopePath + "/findhome", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeFindHome(request);
    });
    
    m_server.route(telescopePath + "/slewtocoordinates", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeSlewToCoordinates(request);
    });
    
    m_server.route(telescopePath + "/synctocoordinates", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleTelescopeSyncToCoordinates(request);
    });
    
    // Camera-specific endpoints
    const QString cameraPath = "/api/v1/camera/0";
    
    m_server.route(cameraPath + "/camerastate", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraState(request);
    });
    
    m_server.route(cameraPath + "/imageready", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraImageReady(request);
    });
    
    m_server.route(cameraPath + "/startexposure", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraStartExposure(request);
    });
    
    m_server.route(cameraPath + "/abortexposure", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraAbortExposure(request);
    });
    
    m_server.route(cameraPath + "/imagearray", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraImageArray(request);
    });
    
    m_server.route(cameraPath + "/cameraxsize", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraCameraXSize(request);
    });
    
    m_server.route(cameraPath + "/cameraysize", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraCameraYSize(request);
    });
    
    m_server.route(cameraPath + "/pixelsizex", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraPixelSizeX(request);
    });
    
    m_server.route(cameraPath + "/pixelsizey", [this, methodToString](const QHttpServerRequest& request) {
        emit requestReceived(methodToString(request.method()), request.url().path());
        return this->handleCameraPixelSizeY(request);
    });
}

// Implement remaining stub methods for completeness

// These methods are stubs that would need implementation based on specific Origin capabilities
QJsonObject AlpacaServer::handleDeviceAction(const QHttpServerRequest& request, const QString& action)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createErrorResponse(1001, "Action not implemented: " + action);
}

// Add any remaining required telescope property handlers as stubs
QJsonObject AlpacaServer::handleTelescopeCanSetDeclinationRate(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSetGuideRates(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSetPark(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSetPierSide(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSetRightAscensionRate(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSlewAltAzAsync(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeCanSyncAltAz(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeDeclinationRate(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0.0, transaction);
}

QJsonObject AlpacaServer::handleTelescopeDoesRefraction(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(false, transaction);
}

QJsonObject AlpacaServer::handleTelescopeGuideRateDeclination(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0.5, transaction);
}

QJsonObject AlpacaServer::handleTelescopeGuideRateRightAscension(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0.5, transaction);
}

QJsonObject AlpacaServer::handleTelescopeRightAscensionRate(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0.0, transaction);
}

QJsonObject AlpacaServer::handleTelescopeSiteElevation(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(10.0, transaction); // Default elevation in meters
}

QJsonObject AlpacaServer::handleTelescopeSiteLatitude(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(52.2, transaction); // Default latitude
}

QJsonObject AlpacaServer::handleTelescopeSiteLongitude(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    return createSuccessResponse(0.0, transaction); // Default longitude
}

QJsonObject AlpacaServer::handleTelescopeTargetDeclination(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (command) {
        // PUT - Set target declination
        QMap<QString, QVariant> params;
        if (!parseRequestBody(request, params)) {
            return createErrorResponse(1002, "Invalid parameters");
        }
        
        if (!params.contains("TargetDeclination")) {
            return createErrorResponse(1002, "Missing TargetDeclination parameter");
        }
        
        double dec = params["TargetDeclination"].toDouble();
        if (dec < -90.0 || dec > 90.0) {
            return createErrorResponse(1025, "Invalid declination value");
        }
        
        return createSuccessResponse(true, transaction);
    }
    else {
        // GET - Return current target declination
        return createSuccessResponse(0.0, transaction);
    }
}

QJsonObject AlpacaServer::handleTelescopeTargetRightAscension(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (command) {
        // PUT - Set target right ascension
        QMap<QString, QVariant> params;
        if (!parseRequestBody(request, params)) {
            return createErrorResponse(1002, "Invalid parameters");
        }
        
        if (!params.contains("TargetRightAscension")) {
            return createErrorResponse(1002, "Missing TargetRightAscension parameter");
        }
        
        double ra = params["TargetRightAscension"].toDouble();
        if (ra < 0.0 || ra >= 24.0) {
            return createErrorResponse(1025, "Invalid right ascension value");
        }
        
        return createSuccessResponse(true, transaction);
    }
    else {
        // GET - Return current target right ascension
        return createSuccessResponse(0.0, transaction);
    }
}

QJsonObject AlpacaServer::handleTelescopeTrackingRate(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (command) {
        // PUT - Set tracking rate
        QMap<QString, QVariant> params;
        if (!parseRequestBody(request, params)) {
            return createErrorResponse(1002, "Invalid parameters");
        }
        
        if (!params.contains("TrackingRate")) {
            return createErrorResponse(1002, "Missing TrackingRate parameter");
        }
        
        int rate = params["TrackingRate"].toInt();
        if (rate < 0 || rate > 3) {
            return createErrorResponse(1025, "Invalid tracking rate value");
        }
        
        return createSuccessResponse(true, transaction);
    }
    else {
        // GET - Return current tracking rate (always sidereal for Origin)
        return createSuccessResponse(0, transaction);
    }
}

QJsonObject AlpacaServer::handleTelescopeTrackingRates(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    QJsonArray rates;
    
    QJsonObject sidereal;
    sidereal["Name"] = "Sidereal";
    sidereal["Value"] = 0;
    rates.append(sidereal);
    
    QJsonObject lunar;
    lunar["Name"] = "Lunar";
    lunar["Value"] = 1;
    rates.append(lunar);
    
    QJsonObject solar;
    solar["Name"] = "Solar";
    solar["Value"] = 2;
    rates.append(solar);
    
    QJsonObject king;
    king["Name"] = "King";
    king["Value"] = 3;
    rates.append(king);
    
    return createSuccessResponse(rates, transaction);
}

QJsonObject AlpacaServer::handleTelescopeUTCDate(const QHttpServerRequest& request, bool command)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (command) {
        // PUT - Set UTC date (Origin doesn't support this)
        return createSuccessResponse(true, transaction);
    }
    else {
        // GET - Return current UTC date
        QDateTime utcNow = QDateTime::currentDateTimeUtc();
        QString isoTime = utcNow.toString(Qt::ISODate);
        return createSuccessResponse(isoTime, transaction);
    }
}

// Additional telescope action stubs
QJsonObject AlpacaServer::handleTelescopeMoveAxis(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    QMap<QString, QVariant> params;
    if (!parseRequestBody(request, params)) {
        return createErrorResponse(1002, "Invalid parameters");
    }
    
    if (!params.contains("Axis") || !params.contains("Rate")) {
        return createErrorResponse(1002, "Missing Axis or Rate parameter");
    }
    
    int axis = params["Axis"].toInt();
    double rate = params["Rate"].toDouble();
    
    // Convert to direction and speed for Origin
    int direction;
    switch(axis) {
        case 0: // Primary axis (RA/AZ)
            direction = (rate >= 0) ? 2 : 3; // East : West
            break;
        case 1: // Secondary axis (DEC/ALT)
            direction = (rate >= 0) ? 0 : 1; // North : South
            break;
        default:
            return createErrorResponse(1025, "Invalid axis value");
    }
    
    int speed = std::min(100, static_cast<int>(std::abs(rate * 100)));
    
    bool success = m_telescopeBackend->moveDirection(direction, speed);
    if (!success) {
        return createErrorResponse(1, "Failed to move axis");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopePulseGuide(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    // Origin doesn't support pulse guiding
    return createErrorResponse(1036, "Pulse guiding not supported");
}

QJsonObject AlpacaServer::handleTelescopeSetPark(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    // Origin doesn't support setting custom park position
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeSlewToAltAz(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    QMap<QString, QVariant> params;
    if (!parseRequestBody(request, params)) {
        return createErrorResponse(1002, "Invalid parameters");
    }
    
    if (!params.contains("Azimuth") || !params.contains("Altitude")) {
        return createErrorResponse(1002, "Missing coordinates");
    }
    
    double az = params["Azimuth"].toDouble();
    double alt = params["Altitude"].toDouble();
    
    // Validate coordinates
    if (az < 0 || az >= 360) {
        return createErrorResponse(1025, "Invalid Azimuth value");
    }
    if (alt < 0 || alt > 90) {
        return createErrorResponse(1025, "Invalid Altitude value");
    }
    
    // Convert Alt/Az to RA/Dec (simplified conversion)
    // In a real implementation, you would need proper coordinate transformation
    double ra = az / 15.0; // Rough approximation
    double dec = alt;
    
    bool success = m_telescopeBackend->gotoPosition(ra, dec);
    if (!success) {
        return createErrorResponse(1, "Failed to slew to coordinates");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeSlewToAltAzAsync(const QHttpServerRequest& request)
{
    // Same as synchronous version for Origin
    return handleTelescopeSlewToAltAz(request);
}

QJsonObject AlpacaServer::handleTelescopeSlewToCoordinatesAsync(const QHttpServerRequest& request)
{
    // Same as synchronous version for Origin
    return handleTelescopeSlewToCoordinates(request);
}

QJsonObject AlpacaServer::handleTelescopeSlewToTarget(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    // Would need to implement target storage to make this work properly
    double ra = 0.0; // Retrieve stored target RA
    double dec = 0.0; // Retrieve stored target DEC
    
    bool success = m_telescopeBackend->gotoPosition(ra, dec);
    if (!success) {
        return createErrorResponse(1, "Failed to slew to target");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeSlewToTargetAsync(const QHttpServerRequest& request)
{
    return handleTelescopeSlewToTarget(request);
}

QJsonObject AlpacaServer::handleTelescopeSyncToAltAz(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    QMap<QString, QVariant> params;
    if (!parseRequestBody(request, params)) {
        return createErrorResponse(1002, "Invalid parameters");
    }
    
    if (!params.contains("Azimuth") || !params.contains("Altitude")) {
        return createErrorResponse(1002, "Missing coordinates");
    }
    
    double az = params["Azimuth"].toDouble();
    double alt = params["Altitude"].toDouble();
    
    // Convert Alt/Az to RA/Dec (simplified)
    double ra = az / 15.0;
    double dec = alt;
    
    bool success = m_telescopeBackend->syncPosition(ra, dec);
    if (!success) {
        return createErrorResponse(1, "Failed to sync to coordinates");
    }
    
    return createSuccessResponse(true, transaction);
}

QJsonObject AlpacaServer::handleTelescopeSyncToTarget(const QHttpServerRequest& request)
{
    ClientTransaction transaction = parseClientTransaction(request);
    
    if (!m_telescopeBackend->isConnected()) {
        return createErrorResponse(1031, "Not connected to telescope");
    }
    
    // Would need to implement target storage
    double ra = 0.0;
    double dec = 0.0;
    
    bool success = m_telescopeBackend->syncPosition(ra, dec);
    if (!success) {
        return createErrorResponse(1, "Failed to sync to target");
    }
    
    return createSuccessResponse(true, transaction);
}

// Helper method for fetching images from Origin
QImage AlpacaServer::fetchFitsFromOrigin(double exposureTime, int gain, int binning)
{
    if (!m_telescopeBackend || !m_telescopeBackend->isConnected()) {
        qWarning() << "Cannot capture image - not connected to telescope";
        return QImage();
    }
    
    return m_telescopeBackend->singleShot(gain, binning, (int)(exposureTime * 1000000));
}

QImage AlpacaServer::loadFitsImage(const QByteArray& fitsData)
{
    // Placeholder implementation - in reality you would use a FITS library
    // to properly parse and convert FITS data to QImage
    QImage image(1280, 960, QImage::Format_Grayscale8);
    image.fill(0);
    return image;
}
