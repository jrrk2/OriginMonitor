#ifndef ALPACA_SERVER_HPP
#define ALPACA_SERVER_HPP

#include <QObject>
#include <QTcpServer>
#include <QHttpServer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QMap>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QHostInfo>
#include <QRandomGenerator>

// Change from OpenStellinaBackend to OriginBackend
#include "OriginBackend.hpp"

/**
 * @class AlpacaServer
 * @brief Implements an ASCOM Alpaca server for the Celestron Origin telescope
 * 
 * This class provides a RESTful HTTP API that follows the ASCOM Alpaca
 * specification for astronomy equipment, allowing any Alpaca-compatible
 * software to control the Celestron Origin telescope.
 */
class AlpacaServer : public QObject
{
    Q_OBJECT

public:
    struct ClientTransaction {
        int clientID = 0;
        int clientTransactionID = 0;
        QString transactionID;
    };
  
    explicit AlpacaServer(QObject *parent = nullptr);
    ~AlpacaServer();

    /**
     * @brief Start the Alpaca server
     * @param port The port to listen on
     * @return True if server started successfully
     */
    bool start(int port = 11111);

    /**
     * @brief Stop the Alpaca server
     */
    void stop();

    /**
     * @brief Check if the server is running
     * @return True if server is running
     */
    bool isRunning() const;

    /**
     * @brief Set the telescope backend to control
     * @param backend Pointer to the telescope backend
     */
    void setTelescopeBackend(OriginBackend* backend);

signals:
    /**
     * @brief Signal emitted when server starts
     */
    void serverStarted();

    /**
     * @brief Signal emitted when server stops
     */
    void serverStopped();

    /**
     * @brief Signal emitted when a request is received
     * @param method HTTP method
     * @param path Request path
     */
    void requestReceived(const QString& method, const QString& path);

    /**
     * @brief Signal emitted when a command is sent to the telescope
     * @param command Command name
     * @param parameters Command parameters
     */
    void commandSent(const QString& command, const QJsonObject& parameters);

private:
    QHttpServer m_server;
    QTcpServer m_tcpserver;
    bool m_running;
    OriginBackend* m_telescopeBackend;  // Changed from OpenStellinaBackend
    QMap<QString, int> m_clientIDs;
    int m_transactionCounter;
    QTimer m_discoveryTimer;

    // Server configuration
    QString m_serverName;
    QString m_manufacturer;
    QString m_manufacturerVersion;
    QString m_location;
    int m_port;
    int m_instanceNumber;

    // Utility methods
    QJsonObject createErrorResponse(int errorNumber, const QString& errorMessage);
    QJsonObject createSuccessResponse(const QJsonValue& value, const ClientTransaction& transaction);
    ClientTransaction parseClientTransaction(const QHttpServerRequest& request);
    bool parseRequestBody(const QHttpServerRequest& request, QMap<QString, QVariant>& result);
    
    // Request handlers
    void setupEndpoints();
    
    // Management endpoints
    QJsonObject handleManagementVersions(const QHttpServerRequest& request);
    QJsonObject handleManagementDescription(const QHttpServerRequest& request);
    QJsonObject handleManagementConfiguredDevices(const QHttpServerRequest& request);
    
    // Common device endpoints
    QJsonObject handleDeviceAction(const QHttpServerRequest& request, const QString& action);
    QJsonObject handleDeviceConnected(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleDeviceDescription(const QHttpServerRequest& request);
    QJsonObject handleDeviceDriverInfo(const QHttpServerRequest& request);
    QJsonObject handleDeviceDriverVersion(const QHttpServerRequest& request);
    QJsonObject handleDeviceInterfaceVersion(const QHttpServerRequest& request);
    QJsonObject handleDeviceName(const QHttpServerRequest& request);
    QJsonObject handleDeviceSupportedActions(const QHttpServerRequest& request);
    
    // Telescope-specific endpoints
    QJsonObject handleTelescopeAlignmentMode(const QHttpServerRequest& request);
    QJsonObject handleTelescopeAltitude(const QHttpServerRequest& request);
    QJsonObject handleTelescopeApertureArea(const QHttpServerRequest& request);
    QJsonObject handleTelescopeApertureDiameter(const QHttpServerRequest& request);
    QJsonObject handleTelescopeAtHome(const QHttpServerRequest& request);
    QJsonObject handleTelescopeAtPark(const QHttpServerRequest& request);
    QJsonObject handleTelescopeAzimuth(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanFindHome(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanPark(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanPulseGuide(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSetDeclinationRate(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSetGuideRates(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSetPark(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSetPierSide(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSetRightAscensionRate(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSetTracking(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSlew(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSlewAltAz(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSlewAltAzAsync(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSlewAsync(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSync(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanSyncAltAz(const QHttpServerRequest& request);
    QJsonObject handleTelescopeCanUnpark(const QHttpServerRequest& request);
    QJsonObject handleTelescopeDeclinationRate(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeDoesRefraction(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeGuideRateDeclination(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeGuideRateRightAscension(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeRightAscensionRate(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeSiteElevation(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeSiteLatitude(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeSiteLongitude(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeSlewing(const QHttpServerRequest& request);
    QJsonObject handleTelescopeTargetDeclination(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeTargetRightAscension(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeTracking(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeTrackingRate(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeTrackingRates(const QHttpServerRequest& request);
    QJsonObject handleTelescopeUTCDate(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleTelescopeDeclination(const QHttpServerRequest& request);
    QJsonObject handleTelescopeRightAscension(const QHttpServerRequest& request);
    
    // Telescope actions
    QJsonObject handleTelescopeAbortSlew(const QHttpServerRequest& request);
    QJsonObject handleTelescopeFindHome(const QHttpServerRequest& request);
    QJsonObject handleTelescopeMoveAxis(const QHttpServerRequest& request);
    QJsonObject handleTelescopePark(const QHttpServerRequest& request);
    QJsonObject handleTelescopePulseGuide(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSetPark(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSlewToAltAz(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSlewToAltAzAsync(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSlewToCoordinates(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSlewToCoordinatesAsync(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSlewToTarget(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSlewToTargetAsync(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSyncToAltAz(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSyncToCoordinates(const QHttpServerRequest& request);
    QJsonObject handleTelescopeSyncToTarget(const QHttpServerRequest& request);
    QJsonObject handleTelescopeUnpark(const QHttpServerRequest& request);

    // Camera-specific API Endpoints
    QJsonObject handleCameraBinX(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleCameraBinY(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleCameraCoolerOn(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleCameraCanAbortExposure(const QHttpServerRequest& request);
    QJsonObject handleCameraCameraXSize(const QHttpServerRequest& request);
    QJsonObject handleCameraCameraYSize(const QHttpServerRequest& request);
    QJsonObject handleCameraMaxBinX(const QHttpServerRequest& request);
    QJsonObject handleCameraMaxBinY(const QHttpServerRequest& request);
    QJsonObject handleCameraPixelSizeX(const QHttpServerRequest& request);
    QJsonObject handleCameraPixelSizeY(const QHttpServerRequest& request);
    QJsonObject handleCameraSensorName(const QHttpServerRequest& request);
    QJsonObject handleCameraSensorType(const QHttpServerRequest& request);
    QJsonObject handleCameraCanGetCoolerPower(const QHttpServerRequest& request);
    QJsonObject handleCameraCanSetCCDTemperature(const QHttpServerRequest& request);
    QJsonObject handleCameraCCDTemperature(const QHttpServerRequest& request);
    QJsonObject handleCameraCanFastReadout(const QHttpServerRequest& request);
    QJsonObject handleCameraReadoutModes(const QHttpServerRequest& request);
    QJsonObject handleCameraBayerOffsetX(const QHttpServerRequest& request);
    QJsonObject handleCameraBayerOffsetY(const QHttpServerRequest& request);
    QJsonObject handleCameraMaxADU(const QHttpServerRequest& request);
    QJsonObject handleCameraExposureMax(const QHttpServerRequest& request);
    QJsonObject handleCameraExposureMin(const QHttpServerRequest& request);
    QJsonObject handleCameraExposureResolution(const QHttpServerRequest& request);
    QJsonObject handleCameraGain(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleCameraGainMin(const QHttpServerRequest& request);
    QJsonObject handleCameraGainMax(const QHttpServerRequest& request);
    QJsonObject handleCameraGains(const QHttpServerRequest& request);
    QJsonObject handleCameraStartX(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleCameraStartY(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleCameraNumX(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleCameraNumY(const QHttpServerRequest& request, bool command = false);
    QJsonObject handleCameraState(const QHttpServerRequest& request);
    QJsonObject handleCameraImageReady(const QHttpServerRequest& request);
    QJsonObject handleCameraStartExposure(const QHttpServerRequest& request);
    QJsonObject handleCameraAbortExposure(const QHttpServerRequest& request);
    QJsonObject handleCameraDriverInfo(const QHttpServerRequest& request);
    QJsonObject handleCameraDriverVersion(const QHttpServerRequest& request);
    QJsonObject handleCameraName(const QHttpServerRequest& request);
    QHttpServerResponse handleCameraImageArray(const QHttpServerRequest& request);
    
    // Helper methods for image capture
    QImage fetchFitsFromOrigin(double exposureTime, int gain, int binning);
    QImage loadFitsImage(const QByteArray& fitsData);
  
    // Discovery protocol
    void startDiscoveryBroadcast();
    void stopDiscoveryBroadcast();
    void sendDiscoveryBroadcast();
};

#endif // ALPACA_SERVER_HPP