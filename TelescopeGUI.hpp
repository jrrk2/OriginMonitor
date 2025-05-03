#pragma once

#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QTabWidget>
#include <QSplitter>
#include <QProgressBar>
#include <QTimer>
#include <QUdpSocket>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
// #include <QFile>
#include <QTextStream>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QPixmap>

#include "TelescopeDataProcessor.hpp"
#include "CommandInterface.hpp"

/**
 * @brief Main application window for the telescope monitor
 */
class TelescopeGUI : public QMainWindow {
    Q_OBJECT
    
public:
    /**
     * @brief Constructor
     * @param parent The parent widget
     */
    TelescopeGUI(QWidget *parent = nullptr);
    
    /**
     * @brief Send a JSON message to the telescope
     * @param obj The JSON object to send
     */
    void sendJsonMessage(const QJsonObject &obj);
    
private slots:
    /**
     * @brief Start discovery of telescopes
     */
    void startDiscovery();
    
    /**
     * @brief Stop discovery of telescopes
     */
    void stopDiscovery();
    
    /**
     * @brief Process UDP broadcast datagrams
     */
    void processPendingDatagrams();
    
    /**
     * @brief Connect to the selected telescope
     */
    void connectToSelectedTelescope();
    
    /**
     * @brief Slot called when the WebSocket connects
     */
    void onWebSocketConnected();
    
    /**
     * @brief Slot called when the WebSocket disconnects
     */
    void onWebSocketDisconnected();
    
    /**
     * @brief Slot called when a message is received from the WebSocket
     * @param message The received message
     */
    void onTextMessageReceived(const QString &message);
    
    /**
     * @brief Update the mount display
     */
    void updateMountDisplay();
    
    /**
     * @brief Update the camera display
     */
    void updateCameraDisplay();
    
    /**
     * @brief Update the focuser display
     */
    void updateFocuserDisplay();
    
    /**
     * @brief Update the environment display
     */
    void updateEnvironmentDisplay();
    
    /**
     * @brief Update the image display
     */
    void updateImageDisplay();
    
    /**
     * @brief Update the disk display
     */
    void updateDiskDisplay();
    
    /**
     * @brief Update the dew heater display
     */
    void updateDewHeaterDisplay();
    
    /**
     * @brief Update the orientation display
     */
    void updateOrientationDisplay();
    
    /**
     * @brief Update the time display
     */
    void updateTimeDisplay();
    
private:
    /**
     * @brief Set up the UI elements
     */
    void setupUI();
    
    /**
     * @brief Set up the WebSocket
     */
    void setupWebSocket();
    
    /**
     * @brief Set up the UDP discovery
     */
    void setupDiscovery();
    
    /**
     * @brief Log a JSON packet to a file
     * @param message The JSON message
     * @param incoming Whether the message is incoming or outgoing
     */
    void logJsonPacket(const QString &message, bool incoming);
    
    /**
     * @brief Request an image from the telescope
     * @param filePath The path to the image file
     */
    void requestImage(const QString &filePath);
    
    /**
     * @brief Update a "last update" label
     * @param label The label to update
     * @param lastUpdate The timestamp of the last update
     */
    void updateLastUpdateLabel(QLabel *label, const QDateTime &lastUpdate);

    // for future focus functionality
    void analyzeImageForFocus(const QByteArray &imageData);
    // Optionally add a variable to store focus scores
    QList<double> focusScores;
  
    // Tab creation methods
    QWidget* createMountTab();
    QWidget* createCameraTab();
    QWidget* createFocuserTab();
    QWidget* createEnvironmentTab();
    QWidget* createImageTab();
    QWidget* createDiskTab();
    QWidget* createDewHeaterTab();
    QWidget* createOrientationTab();
    QWidget* createCommandTab();
    
    // Class members
    TelescopeDataProcessor *dataProcessor;
    QWebSocket *webSocket;
    QUdpSocket *udpSocket;
    
    // UI elements
    QListWidget *telescopeListWidget;
    QPushButton *connectButton;
    QLabel *statusLabel;
    
    // State variables
    QStringList telescopeAddresses;
    QString connectedIpAddress;
    bool isConnected = false;
    
    // Mount tab widgets
    QLabel *mountBatteryLevelLabel;
    QLabel *mountBatteryVoltageLabel;
    QLabel *mountChargerStatusLabel;
    QLabel *mountTimeLabel;
    QLabel *mountDateLabel;
    QLabel *mountTimeZoneLabel;
    QLabel *mountLatitudeLabel;
    QLabel *mountLongitudeLabel;
    QLabel *mountIsAlignedLabel;
    QLabel *mountIsTrackingLabel;
    QLabel *mountIsGotoOverLabel;
    QLabel *mountNumAlignRefsLabel;
    QLabel *mountLastUpdateLabel;
    
    // Camera tab widgets
    QLabel *cameraBinningLabel;
    QLabel *cameraBitDepthLabel;
    QLabel *cameraExposureLabel;
    QLabel *cameraISOLabel;
    QLabel *cameraRedBalanceLabel;
    QLabel *cameraGreenBalanceLabel;
    QLabel *cameraBlueBalanceLabel;
    QLabel *cameraLastUpdateLabel;
    
    // Focuser tab widgets
    QLabel *focuserPositionLabel;
    QLabel *focuserBacklashLabel;
    QLabel *focuserLowerLimitLabel;
    QLabel *focuserUpperLimitLabel;
    QLabel *focuserIsCalibrationCompleteLabel;
    QProgressBar *focuserCalibrationProgressBar;
    QLabel *focuserLastUpdateLabel;
    
    // Environment tab widgets
    QLabel *envAmbientTempLabel;
    QLabel *envCameraTempLabel;
    QLabel *envCpuTempLabel;
    QLabel *envFrontCellTempLabel;
    QLabel *envHumidityLabel;
    QLabel *envDewPointLabel;
    QLabel *envCpuFanLabel;
    QLabel *envOtaFanLabel;
    QLabel *environmentLastUpdateLabel;
    
    // Image tab widgets
    QLabel *imageFileLabel;
    QLabel *imageTypeLabel;
    QLabel *imageDecLabel;
    QLabel *imageRaLabel;
    QLabel *imageOrientationLabel;
    QLabel *imageFovXLabel;
    QLabel *imageFovYLabel;
    QLabel *imageLastUpdateLabel;
    QLabel *imagePreviewLabel;
    
    // Disk tab widgets
    QLabel *diskCapacityLabel;
    QLabel *diskFreeLabel;
    QLabel *diskUsedLabel;
    QLabel *diskLevelLabel;
    QProgressBar *diskUsageBar;
    QLabel *diskLastUpdateLabel;
    
    // Dew Heater tab widgets
    QLabel *dewHeaterModeLabel;
    QLabel *dewHeaterAggressionLabel;
    QLabel *dewHeaterLevelLabel;
    QLabel *dewHeaterManualPowerLabel;
    QProgressBar *dewHeaterLevelBar;
    QLabel *dewHeaterLastUpdateLabel;
    
    // Orientation tab widgets
    QLabel *orientationAltitudeLabel;
    QLabel *orientationLastUpdateLabel;

    CommandInterface *commandInterface;
    bool debug = false;
};
