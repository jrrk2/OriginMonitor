// Complete Example: Camera Control + Comet Tracking Integration
// This shows how to use all camera features with your comet tracking app

#include "OriginCameraController.hpp"
#include "CometTrackingAdjustment.hpp"
#include "TelescopeGUI.hpp"
#include <QGroupBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QProgressBar>

// Add to TelescopeGUI.hpp:
/*
private:
    OriginCameraController* m_cameraController;
    QString m_snapshotSavePath;
    QProgressBar* m_downloadProgress;
*/

// ============================================================================
// INITIALIZATION
// ============================================================================

void TelescopeGUI::setupCameraController()
{
    // Get telescope IP from backend
    QString telescopeIP = m_backend->getTelescopeIP(); // e.g., "192.168.1.195"
    
    m_cameraController = new OriginCameraController(m_backend, 
                                                     telescopeIP, 
                                                     this);
    
    // Set default snapshot save location
    m_snapshotSavePath = QDir::homePath() + "/CometSnapshots";
    QDir().mkpath(m_snapshotSavePath);
    
    // Connect signals
    connect(m_cameraController, &OriginCameraController::modeChanged,
            this, &TelescopeGUI::onCameraModeChanged);
    
    connect(m_cameraController, &OriginCameraController::snapshotReady,
            this, &TelescopeGUI::onSnapshotReady);
    
    connect(m_cameraController, &OriginCameraController::snapshotDownloaded,
            this, &TelescopeGUI::onSnapshotDownloaded);
    
    connect(m_cameraController, &OriginCameraController::downloadProgress,
            this, &TelescopeGUI::onDownloadProgress);
    
    connect(m_cameraController, &OriginCameraController::errorOccurred,
            this, [](const QString& error) {
                qWarning() << "Camera error:" << error;
            });
}

// ============================================================================
// UI CREATION
// ============================================================================

void TelescopeGUI::createCameraControlUI()
{
    QGroupBox* cameraGroup = new QGroupBox("Camera Control", this);
    QVBoxLayout* cameraLayout = new QVBoxLayout(cameraGroup);
    
    // Mode Control
    QHBoxLayout* modeLayout = new QHBoxLayout();
    QPushButton* manualBtn = new QPushButton("Manual Mode", this);
    QPushButton* autoBtn = new QPushButton("Auto Mode", this);
    
    connect(manualBtn, &QPushButton::clicked, 
            m_cameraController, &OriginCameraController::setManualMode);
    connect(autoBtn, &QPushButton::clicked,
            m_cameraController, &OriginCameraController::setAutoMode);
    
    modeLayout->addWidget(manualBtn);
    modeLayout->addWidget(autoBtn);
    cameraLayout->addLayout(modeLayout);
    
    // Exposure Control
    QHBoxLayout* exposureLayout = new QHBoxLayout();
    QLabel* exposureLabel = new QLabel("Exposure (sec):", this);
    QDoubleSpinBox* exposureSpin = new QDoubleSpinBox(this);
    exposureSpin->setRange(0.01, 300.0);
    exposureSpin->setValue(0.1);
    exposureSpin->setDecimals(3);
    exposureSpin->setSingleStep(0.1);
    
    connect(exposureSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
                m_cameraController->setExposure(value);
            });
    
    exposureLayout->addWidget(exposureLabel);
    exposureLayout->addWidget(exposureSpin);
    cameraLayout->addLayout(exposureLayout);
    
    // ISO Control
    QHBoxLayout* isoLayout = new QHBoxLayout();
    QLabel* isoLabel = new QLabel("ISO:", this);
    QSpinBox* isoSpin = new QSpinBox(this);
    isoSpin->setRange(100, 6400);
    isoSpin->setValue(200);
    isoSpin->setSingleStep(100);
    
    connect(isoSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int value) {
                m_cameraController->setISO(value);
            });
    
    isoLayout->addWidget(isoLabel);
    isoLayout->addWidget(isoSpin);
    cameraLayout->addLayout(isoLayout);
    
    // Quick Presets
    QHBoxLayout* presetsLayout = new QHBoxLayout();
    QPushButton* fastBtn = new QPushButton("Fast (0.02s, 200)", this);
    QPushButton* mediumBtn = new QPushButton("Medium (0.1s, 200)", this);
    QPushButton* longBtn = new QPushButton("Long (1s, 400)", this);
    
    connect(fastBtn, &QPushButton::clicked, this, [this, exposureSpin, isoSpin]() {
        exposureSpin->setValue(0.02);
        isoSpin->setValue(200);
        m_cameraController->setCaptureParameters(0.02, 200);
    });
    
    connect(mediumBtn, &QPushButton::clicked, this, [this, exposureSpin, isoSpin]() {
        exposureSpin->setValue(0.1);
        isoSpin->setValue(200);
        m_cameraController->setCaptureParameters(0.1, 200);
    });
    
    connect(longBtn, &QPushButton::clicked, this, [this, exposureSpin, isoSpin]() {
        exposureSpin->setValue(1.0);
        isoSpin->setValue(400);
        m_cameraController->setCaptureParameters(1.0, 400);
    });
    
    presetsLayout->addWidget(fastBtn);
    presetsLayout->addWidget(mediumBtn);
    presetsLayout->addWidget(longBtn);
    cameraLayout->addLayout(presetsLayout);
    
    // Snapshot Controls
    QPushButton* snapshotBtn = new QPushButton("Take Snapshot", this);
    snapshotBtn->setStyleSheet("font-weight: bold; padding: 10px;");
    connect(snapshotBtn, &QPushButton::clicked,
            m_cameraController, &OriginCameraController::takeSingleSnapshot);
    cameraLayout->addWidget(snapshotBtn);
    
    // Download Progress
    m_downloadProgress = new QProgressBar(this);
    m_downloadProgress->setVisible(false);
    cameraLayout->addWidget(m_downloadProgress);
    
    // Add to main layout
    // (wherever you want the camera controls in your GUI)
}

// ============================================================================
// SIGNAL HANDLERS
// ============================================================================

void TelescopeGUI::onCameraModeChanged(bool isManual)
{
    qDebug() << "Camera mode changed:" << (isManual ? "Manual" : "Auto");
    // Update UI to reflect mode
}

void TelescopeGUI::onSnapshotReady(const QString& fileLocation, 
                                   double ra, double dec)
{
    qDebug() << "Snapshot ready at" << fileLocation;
    qDebug() << "Position: RA =" << ra << "Dec =" << dec;
    
    // Automatically download the snapshot
    QString filename = QFileInfo(fileLocation).fileName();
    QString savePath = m_snapshotSavePath + "/" + filename;
    
    // Add timestamp to filename
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString extension = QFileInfo(filename).suffix();
    QString basename = QFileInfo(filename).baseName();
    savePath = m_snapshotSavePath + "/" + basename + "_" + timestamp + "." + extension;
    
    m_cameraController->downloadSnapshot(fileLocation, savePath);
    
    // Show progress bar
    m_downloadProgress->setVisible(true);
    m_downloadProgress->setValue(0);
}

void TelescopeGUI::onSnapshotDownloaded(const QString& localPath)
{
    qDebug() << "Snapshot downloaded to:" << localPath;
    
    // Hide progress bar
    m_downloadProgress->setVisible(false);
    
    // Show notification or display image
    QMessageBox::information(this, "Snapshot Saved", 
                            "Snapshot saved to:\n" + localPath);
    
    // Optional: Display the image in your GUI
    // displayImage(localPath);
}

void TelescopeGUI::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    if (bytesTotal > 0) {
        int percent = (bytesReceived * 100) / bytesTotal;
        m_downloadProgress->setValue(percent);
    }
}

// ============================================================================
// INTEGRATED COMET TRACKING WITH SNAPSHOTS
// ============================================================================

void TelescopeGUI::startAutomatedCometImaging()
{
    // This combines comet tracking with automatic snapshot capture
    
    // 1. Start comet tracking
    if (m_trackingAdjustment) {
        m_trackingAdjustment->startTracking();
    }
    
    // 2. Set camera to manual mode with specific settings
    m_cameraController->setManualMode();
    m_cameraController->setCaptureParameters(2.0, 800); // 2 sec, ISO 800
    
    // 3. Set up timer to take snapshots periodically
    QTimer* snapshotTimer = new QTimer(this);
    connect(snapshotTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "Taking periodic comet snapshot...";
        m_cameraController->takeSingleSnapshot();
    });
    
    // Take a snapshot every 30 seconds while tracking
    snapshotTimer->start(30000);
    
    qDebug() << "Automated comet imaging started";
    qDebug() << "  Tracking update interval: 5s";
    qDebug() << "  Snapshot interval: 30s";
    qDebug() << "  Exposure: 2.0s, ISO: 800";
}

// ============================================================================
// COMMAND SUMMARY
// ============================================================================

/*
CAMERA CONTROL COMMANDS (from your snapshot data):

1. Enable/Disable Manual Mode:
   {'Command': 'SetEnableManual', 'Destination': 'LiveStream', 
    'SequenceID': X, 'Source': 'YourApp', 'Type': 'Command'}
   
   {'Command': 'SetEnableAuto', 'Destination': 'LiveStream',
    'SequenceID': X, 'Source': 'YourApp', 'Type': 'Command'}

2. Get Manual Mode Status:
   {'Command': 'GetEnableManual', 'Destination': 'LiveStream',
    'SequenceID': X, 'Source': 'YourApp', 'Type': 'Command'}
   
   Response: {'IsManual': true/false, ...}

3. Set Exposure and ISO:
   {'Command': 'SetCaptureParameters', 'Destination': 'Camera',
    'SequenceID': X, 'Source': 'YourApp', 'Type': 'Command',
    'Exposure': <seconds>, 'ISO': <value>}

4. Get Current Camera Settings:
   {'Command': 'GetCaptureParameters', 'Destination': 'Camera',
    'SequenceID': X, 'Source': 'YourApp', 'Type': 'Command'}
   
   Response: {'Exposure': X, 'ISO': Y, 'Binning': 1, 'BitDepth': 24, ...}

5. Take a Single Snapshot:
   {'Command': 'RunSampleCapture', 'Destination': 'TaskController',
    'SequenceID': X, 'Source': 'YourApp', 'Type': 'Command',
    'ExposureTime': <seconds>, 'ISO': <value>}

6. Get Camera Info:
   {'Command': 'GetCameraInfo', 'Destination': 'Camera',
    'SequenceID': X, 'Source': 'YourApp', 'Type': 'Command'}
   
   Response: {'CameraID': 'Origin178-...', 'CameraModel': 'Origin178',
              'DefaultIso': 200, ...}

7. Image Ready Notification (received automatically):
   {'Command': 'NewImageReady', 'FileLocation': 'Images/Temp/1.jpg',
    'Ra': <radians>, 'Dec': <radians>, 'ExposureTime': <seconds>, ...}

8. Download Image:
   HTTP GET: http://<telescope_ip>/<FileLocation>
   Example: http://192.168.1.195/Images/Temp/1.jpg

IMPORTANT NOTES:
- The telescope serves images immediately via HTTP
- Images are stored temporarily in Images/Temp/ on the telescope
- Download images immediately after capture
- FileLocation can be .jpg or .tiff depending on BitDepth setting
- Manual mode is required for custom exposure/ISO settings
- Auto mode uses automatic exposure adjustment

IMAGE FORMATS:
- BitDepth 24: JPEG format (Images/Temp/X.jpg)
- BitDepth 16: TIFF format (Images/Temp/sampleX.tiff)

TYPICAL WORKFLOW:
1. SetEnableManual - Switch to manual mode
2. SetCaptureParameters - Set exposure and ISO
3. RunSampleCapture - Take snapshot
4. Wait for NewImageReady notification
5. Download image via HTTP GET
6. Optionally switch back to auto mode

*/

// ============================================================================
// ADVANCED: BATCH SNAPSHOT SERIES
// ============================================================================

class CometSnapshotSeries : public QObject
{
    Q_OBJECT
public:
    CometSnapshotSeries(OriginCameraController* camera,
                       const QString& savePath,
                       QObject* parent = nullptr)
        : QObject(parent)
        , m_camera(camera)
        , m_savePath(savePath)
        , m_currentSnapshot(0)
        , m_totalSnapshots(0)
    {
        connect(m_camera, &OriginCameraController::snapshotReady,
                this, &CometSnapshotSeries::onSnapshotReady);
        connect(m_camera, &OriginCameraController::snapshotDownloaded,
                this, &CometSnapshotSeries::onSnapshotDownloaded);
    }
    
    void startSeries(int count, double exposure, int iso, int intervalMs)
    {
        m_totalSnapshots = count;
        m_currentSnapshot = 0;
        m_exposure = exposure;
        m_iso = iso;
        
        qDebug() << "Starting snapshot series:";
        qDebug() << "  Count:" << count;
        qDebug() << "  Exposure:" << exposure << "s";
        qDebug() << "  ISO:" << iso;
        qDebug() << "  Interval:" << intervalMs << "ms";
        
        // Ensure manual mode
        m_camera->setManualMode();
        QTimer::singleShot(500, this, [this]() {
            takeNextSnapshot();
        });
        
        // Set up timer for subsequent snapshots
        m_seriesTimer.setInterval(intervalMs);
        connect(&m_seriesTimer, &QTimer::timeout,
                this, &CometSnapshotSeries::takeNextSnapshot);
    }
    
    void stop()
    {
        m_seriesTimer.stop();
        qDebug() << "Snapshot series stopped at" << m_currentSnapshot
                 << "of" << m_totalSnapshots;
    }
    
signals:
    void seriesProgress(int current, int total);
    void seriesComplete(int count);

private slots:
    void takeNextSnapshot()
    {
        if (m_currentSnapshot >= m_totalSnapshots) {
            m_seriesTimer.stop();
            emit seriesComplete(m_currentSnapshot);
            qDebug() << "Snapshot series complete:" << m_currentSnapshot << "images";
            return;
        }
        
        m_currentSnapshot++;
        qDebug() << "Taking snapshot" << m_currentSnapshot 
                 << "of" << m_totalSnapshots;
        
        m_camera->takeSnapshot(m_exposure, m_iso);
        emit seriesProgress(m_currentSnapshot, m_totalSnapshots);
    }
    
    void onSnapshotReady(const QString& fileLocation, double ra, double dec)
    {
        // Auto-download with series number in filename
        QString filename = QString("comet_series_%1_%2.jpg")
                          .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"))
                          .arg(m_currentSnapshot, 4, 10, QChar('0'));
        
        QString fullPath = m_savePath + "/" + filename;
        m_camera->downloadSnapshot(fileLocation, fullPath);
        
        qDebug() << "Series image" << m_currentSnapshot << "ready";
        qDebug() << "  Position: RA =" << ra << "Dec =" << dec;
    }
    
    void onSnapshotDownloaded(const QString& localPath)
    {
        qDebug() << "Series image" << m_currentSnapshot << "saved:" << localPath;
    }

private:
    OriginCameraController* m_camera;
    QString m_savePath;
    QTimer m_seriesTimer;
    int m_currentSnapshot;
    int m_totalSnapshots;
    double m_exposure;
    int m_iso;
};

// Usage example:
void TelescopeGUI::captureSnapshotSeries()
{
    CometSnapshotSeries* series = new CometSnapshotSeries(
        m_cameraController,
        m_snapshotSavePath,
        this
    );
    
    // Connect progress signals
    connect(series, &CometSnapshotSeries::seriesProgress,
            this, [this](int current, int total) {
                qDebug() << "Series progress:" << current << "/" << total;
                // Update progress bar
                if (m_downloadProgress) {
                    m_downloadProgress->setMaximum(total);
                    m_downloadProgress->setValue(current);
                }
            });
    
    connect(series, &CometSnapshotSeries::seriesComplete,
            this, [](int count) {
                qDebug() << "Series complete!" << count << "images captured";
            });
    
    // Capture 20 images, 2 second exposure, ISO 800, every 15 seconds
    series->startSeries(20, 2.0, 800, 15000);
}

// ============================================================================
// INTEGRATION WITH EXISTING ORIGINBACKEND
// ============================================================================

// You need to add these methods to OriginBackend.cpp:

void OriginBackend::sendCommand(const QJsonObject& command)
{
    if (!webSocket || webSocket->state() != QAbstractSocket::ConnectedState) {
        qWarning() << "WebSocket not connected, cannot send command";
        return;
    }
    
    QJsonDocument doc(command);
    QString jsonStr = doc.toJson(QJsonDocument::Compact);
    
    qDebug() << "Sending command:" << jsonStr;
    webSocket->sendTextMessage(jsonStr);
}

QString OriginBackend::getTelescopeIP() const
{
    // Return the IP address of the connected telescope
    // This should be stored when you connect to the telescope
    return m_telescopeIP; // Member variable set during connection
}

bool OriginBackend::isConnected() const
{
    return webSocket && 
           webSocket->state() == QAbstractSocket::ConnectedState;
}

// Add signal forwarding in OriginBackend for camera controller
void OriginBackend::onTextMessageReceived(const QString& message)
{
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // Forward to camera controller if needed
    emit messageReceived(obj);
    
    // ... existing processing code ...
}

// Add to OriginBackend.hpp:
// signals:
//     void messageReceived(const QJsonObject& message);

// Then in OriginCameraController constructor:
// connect(m_backend, &OriginBackend::messageReceived,
//         this, &OriginCameraController::handleWebSocketMessage);

// ============================================================================
// COMPLETE MAIN SETUP
// ============================================================================

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // Initialize components
    global_observer = new CometObserver();
    
    // Load comet data
    QString kernelPath = "/path/to/spice/kernels";
    global_observer->initializeCspice(kernelPath);
    
    QString cometFile = "/path/to/comet.bsp";
    global_observer->loadCometEphemeris(cometFile, "C/2023 A3");
    
    // Create GUI
    TelescopeGUI gui;
    
    // Wait for telescope connection
    QTimer::singleShot(2000, &gui, [&gui]() {
        // Setup camera controller
        gui.setupCameraController();
        gui.createCameraControlUI();
        
        // Setup comet tracking
        gui.setupCometTracking();
        gui.createTrackingControls();
        
        // Optional: Start automated imaging
        // gui.startAutomatedCometImaging();
    });
    
    gui.show();
    
    return app.exec();
}

// ============================================================================
// TROUBLESHOOTING
// ============================================================================

/*
COMMON ISSUES:

1. "WebSocket not connected"
   - Ensure OriginBackend is connected before using camera controller
   - Check telescope IP address is correct

2. "Download failed"
   - Verify telescope IP is accessible
   - Check that FileLocation path is correct
   - Ensure image exists on telescope

3. "No images captured"
   - Set manual mode before taking snapshots
   - Wait for mode change to complete (500ms delay)
   - Check exposure and ISO are within valid ranges

4. "Images not auto-downloading"
   - Ensure you're connected to NewImageReady notifications
   - Verify HTTP access to telescope is allowed
   - Check save path exists and has write permissions

5. "Tracking not working"
   - Verify CometObserver has valid ephemeris loaded
   - Check mount is responding to commands
   - Ensure telescope is aligned/calibrated

PERFORMANCE TIPS:

1. For comet tracking:
   - Use 5-10 second update intervals
   - Set correction threshold to 5-10 arcsec
   - Use fast exposures (0.02-0.1s) for tracking updates

2. For imaging:
   - Download images immediately after capture
   - Use separate directory for each session
   - Consider TIFF format for better quality (BitDepth 16)

3. Network optimization:
   - Use wired connection if possible
   - Keep computer and telescope on same network
   - Minimize other network traffic during imaging

*/