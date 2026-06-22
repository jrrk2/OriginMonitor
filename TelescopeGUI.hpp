#pragma once

class OriginBackend;
class AutopilotController;
class AlignmentController;

#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
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
#include <QFileDialog>
#include <QTextStream>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QPixmap>
#include <QSpinBox>
#include <QTextEdit>
#include <QCheckBox>
#include <QScrollBar>
#include <QMenuBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QFrame>
#include <QSettings>

#include "TelescopeDataProcessor.hpp"
#include "CommandInterface.hpp"
#include "LogReplayDialog.hpp"

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
    OriginBackend* originBackend;
    
private slots:
    void showLogReplay();
    void updateTaskControllerDisplay();
    void startUpButton();
    void startDownButton();
    void startLeftButton();
    void startRightButton();
    void slew(int ,int);
    void cancelSlew();
    void onTrackingError(const QString& error);
    void onSlewCancel(); // cancel slew after delay
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
     * @brief Update the time display
     */
    void updateTimeDisplay();

    void startSlewAndImage();
    void cancelSlewAndImage();
    void slewAndImageTimerTimeout();
    void updateSlewAndImageStatus();
    void initializeTelescope();
    void startTelescopeAlignment();
    void checkMountStatus();

    // snaphot camera slots
    void onCameraModeChanged(bool isManual);
    void onCaptureParametersChanged(double exposure, int iso);
    void onSnapshotReady(const QString& fileLocation, double ra, double dec);
    void onSnapshotDownloaded(const QString& localPath);
    void onSnapshotDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);

private:
    // Task Controller tab widgets
    QLabel *taskStateLabel;
    QLabel *taskStageLabel;
    QLabel *taskReadyLabel;
    QLabel *taskCurrentStepLabel;
    QLabel *taskNumPointsLabel;
    QProgressBar *taskProgressBar;
    QLabel *taskFocusPositionLabel;
    QProgressBar *taskFocusProgressBar;
    QLabel *taskImagingNameLabel;
    QLabel *taskLastUpdateLabel;

    // UI elements
    QLineEdit* cometNameEdit;
    QPushButton* startTrackingButton;
    QPushButton* stopTrackingButton;
    QSpinBox* updateIntervalSpinBox;
    QLabel* cometRaLabel;
    QLabel* cometDecLabel;
    QLabel* cometAltLabel;
    QLabel* cometAzLabel;
    QLabel* cometObservableLabel;
  
    /**
     * @brief Set up the UI elements
     */
    void setupUI();
    
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
    QWidget* createCommandTab();
    QWidget* createSlewAndImageTab();
    QWidget* createTaskControllerTab();
  
    // Class members
    TelescopeDataProcessor *dataProcessor;
    QUdpSocket *udpSocket;
    
    // UI elements
    QListWidget *telescopeListWidget;
    QLineEdit *telescopeIpLineEdit;
    QPushButton *connectButton;
    QLabel *statusLabel;
    
    // State variables
    QStringList telescopeAddresses;
    QString connectedIpAddress;
    
    // Mount tab widgets
    QLabel *mountBatteryCurrentLabel;
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
    QLabel *mountAltitudeLabel;
    QLabel *mountAzimuthLabel;
    QLabel *mountAltitudeErrorLabel;
    QLabel *mountAzimuthErrorLabel;
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
    QLabel *imageFovXLabel;
    QLabel *imageFovYLabel;
    QLabel *imageLastUpdateLabel;
    QLabel *imagePreviewLabel;
    QString m_snapshotSavePath;
    
    // Camera control UI widgets
    QDoubleSpinBox* exposureSpinBox;
    QSpinBox* binningSpinBox;
    QSpinBox* isoSpinBox;
    QPushButton* snapshotButton;
    QProgressBar* snapshotProgressBar;
    
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
    
    // Add more private members for the new tab
    QComboBox *targetComboBox;
    QLineEdit *customRaEdit;
    QLineEdit *customDecEdit;
    QLineEdit *customNameEdit;
    QSpinBox *durationSpinBox;
    QPushButton *startSlewButton;
    QPushButton *cancelSlewButton;
    QProgressBar *slewProgressBar;
    QLabel *slewStatusLabel;
    QTimer *slewAndImageTimer;
    QTimer *statusUpdateTimer;
    QTimer *slewTimer;
    bool isSlewingAndImaging = false;
    int imagingTimeRemaining = 0;
    QString currentImagingTargetUuid;

    QLabel *alignmentStatusLabel;
    QLabel *mountStatusLabel;
    QPushButton *initializeButton;

    bool debug = false;
    LogReplayDialog* m_logReplayDialog;
    AutopilotController* m_autopilotController;
    AlignmentController* m_alignmentController;

  
};
