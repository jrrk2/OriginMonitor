#include "TelescopeGUI.hpp"
#include "CommandInterface.hpp"
#include "OriginBackend.hpp"
#include "MessierCatalog.hpp"
#include "AutopilotController.hpp"
#include "AutopilotTab.hpp"
#include "AlignmentController.hpp"
#include "AlignmentTab.hpp"
#include "JsonMonitorTab.hpp"
#include <cmath>

TelescopeGUI::TelescopeGUI(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Celestron Origin Monitor");
    resize(900, 700);

    //
    // IMPORTANT: Create the backend first
    //
    originBackend = new OriginBackend(this);
    connect(originBackend, &OriginBackend::connected, this, &TelescopeGUI::onWebSocketConnected);
    connect(originBackend, &OriginBackend::disconnected, this, &TelescopeGUI::onWebSocketDisconnected);

    //
    // Get the SAME data processor that OriginBackend feeds
    //
    dataProcessor = originBackend->getDataProcessor();

    //
    // Connect GUI update slots to this processor
    //
    connect(dataProcessor, &TelescopeDataProcessor::mountStatusUpdated,
            this, &TelescopeGUI::updateMountDisplay);

    connect(dataProcessor, &TelescopeDataProcessor::cameraStatusUpdated,
            this, &TelescopeGUI::updateCameraDisplay);

    connect(dataProcessor, &TelescopeDataProcessor::focuserStatusUpdated,
            this, &TelescopeGUI::updateFocuserDisplay);

    connect(dataProcessor, &TelescopeDataProcessor::environmentStatusUpdated,
            this, &TelescopeGUI::updateEnvironmentDisplay);

    connect(dataProcessor, &TelescopeDataProcessor::diskStatusUpdated,
            this, &TelescopeGUI::updateDiskDisplay);

    connect(dataProcessor, &TelescopeDataProcessor::dewHeaterStatusUpdated,
            this, &TelescopeGUI::updateDewHeaterDisplay);
    /*
    connect(dataProcessor, &TelescopeDataProcessor::orientationStatusUpdated,
            this, &TelescopeGUI::updateOrientationDisplay);
    */
    connect(dataProcessor, &TelescopeDataProcessor::taskControllerStatusUpdated,
            this, &TelescopeGUI::updateTaskControllerDisplay);

    connect(dataProcessor, &TelescopeDataProcessor::newImageAvailable,
            this, &TelescopeGUI::updateImageDisplay);

    //
    // Autopilot controller
    //
    m_autopilotController = new AutopilotController(originBackend, this);

    //
    // Alignment controller (plate solving, mount model)
    //
    m_alignmentController = new AlignmentController(originBackend, this);

    // Wire mount model into backend for automatic GoTo corrections
    originBackend->setMountModel(m_alignmentController->mountModel());

    //
    // Now build GUI
    //
    setupUI();
    m_logReplayDialog = nullptr;

    //
    // Device discovery
    //
    setupDiscovery();
    
    // Update time display every second
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TelescopeGUI::updateTimeDisplay);
    timer->start(1000);

    // cancel slews after 100ms
    slewTimer = new QTimer(this);
    slewTimer->setSingleShot(true);
    slewTimer->setInterval(1000);
    connect(slewTimer, &QTimer::timeout, this, &TelescopeGUI::onSlewCancel);

    // ===== ADD TO TelescopeGUI CONSTRUCTOR =====

    // Connect live JPEG stream to image preview
    connect(originBackend, &OriginBackend::liveImageDownloaded,
	    this, [this](const QByteArray& imageData, double ra, double dec, double exposure) {

		// Load image from data
		QImage image;
		if (!image.loadFromData(imageData)) {
		    qWarning() << "Failed to load live image from data";
		    return;
		}

		// Update image preview label
		if (imagePreviewLabel) {
		    QPixmap pixmap = QPixmap::fromImage(image);
		    QPixmap scaledPixmap = pixmap.scaled(imagePreviewLabel->size(), 
							Qt::KeepAspectRatio, 
							Qt::SmoothTransformation);
		    imagePreviewLabel->setPixmap(scaledPixmap);
		}

		// Update metadata labels
		if (imageRaLabel) {
		    imageRaLabel->setText(QString("%1 hrs").arg(
			originBackend->radiansToHours(ra), 0, 'f', 6));
		}
		if (imageDecLabel) {
		    imageDecLabel->setText(QString("%1°").arg(
			originBackend->radiansToDegrees(dec), 0, 'f', 6));
		}
		if (imageTypeLabel) {
		    imageTypeLabel->setText("Live Stream (JPEG)");
		}

		// Update last update timestamp
		if (imageLastUpdateLabel) {
		    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
		    imageLastUpdateLabel->setText(timestamp);
		}

		// Optionally log
		// qDebug() << "Live image displayed - RA:" << ra << "Dec:" << dec;
	    });

    // In TelescopeGUI constructor (after originBackend creation):
    connect(originBackend, &OriginBackend::tiffImageDownloaded,
	    this, [this](const QString& filePath, const QByteArray& imageData,
			 double ra, double dec, double exposure) {

		qDebug() << "=== TIFF Snapshot Downloaded ===";
		qDebug() << "Remote path:" << filePath;
		qDebug() << "Size:" << imageData.size() << "bytes";
		qDebug() << "RA:" << originBackend->radiansToHours(ra) << "hrs";
		qDebug() << "Dec:" << originBackend->radiansToDegrees(dec) << "deg";
		qDebug() << "Exposure:" << exposure << "sec";

		// Create save directory
		QString baseDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) 
				+ "/CelestronOrigin";
		QDir().mkpath(baseDir);

		// Generate filename with timestamp
		QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
		QString localPath = QString("%1/snapshot_%2.tiff").arg(baseDir, timestamp);

		// Save the TIFF data
		QFile file(localPath);
		if (file.open(QIODevice::WriteOnly)) {
		    qint64 written = file.write(imageData);
		    file.close();

		    if (written == imageData.size()) {
			qDebug() << "Successfully saved to:" << localPath;

			// Update UI
			onSnapshotDownloaded(localPath);

			// Update metadata labels
			if (imageRaLabel) {
			    imageRaLabel->setText(QString("%1 hrs").arg(
				originBackend->radiansToHours(ra), 0, 'f', 6));
			}
			if (imageDecLabel) {
			    imageDecLabel->setText(QString("%1°").arg(
				originBackend->radiansToDegrees(dec), 0, 'f', 6));
			}
			if (imageTypeLabel) {
			    imageTypeLabel->setText("Snapshot (TIFF)");
			}
		    } else {
			qWarning() << "Write error: wrote" << written << "of" << imageData.size();
		    }
		} else {
		    qWarning() << "Failed to create file:" << localPath;
		    qWarning() << "Error:" << file.errorString();
		}
	    });    
// ============================================================================
// Add to TelescopeGUI.cpp constructor - Connect the signal
// ============================================================================

// In TelescopeGUI constructor, after other signal connections:
connect(dataProcessor, &TelescopeDataProcessor::taskControllerStatusUpdated, 
        this, &TelescopeGUI::updateTaskControllerDisplay);
}

void TelescopeGUI::startDiscovery() {
    statusLabel->setText("Discovering telescopes...");
    telescopeListWidget->clear();
    telescopeAddresses.clear();
    
    // Close existing socket if it's open
    if (udpSocket->state() != QAbstractSocket::UnconnectedState) {
        udpSocket->close();
    }
    
    // Bind to port 55555 on all interfaces
    bool success = udpSocket->bind(QHostAddress::AnyIPv4, 55555, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    
    if (success) {
        // Enable broadcast reception
        udpSocket->setSocketOption(QAbstractSocket::SocketOption(5), 1); // BroadcastOption
        
        // Connect to the readyRead signal
        connect(udpSocket, &QUdpSocket::readyRead, this, &TelescopeGUI::processPendingDatagrams);
        
        statusLabel->setText("Listening for telescope broadcasts...");
        
        // Auto-stop discovery after 30 seconds if nothing found
        QTimer::singleShot(30000, this, [this]() {
            if (telescopeAddresses.isEmpty()) {
                stopDiscovery();
                statusLabel->setText("No telescopes found. Discovery stopped.");
            }
        });
    } else {
        statusLabel->setText(QString("Failed to bind to port 55555: %1").arg(udpSocket->errorString()));
    }
}

void TelescopeGUI::stopDiscovery() {
    // Disconnect signals and close socket
    if (udpSocket->state() != QAbstractSocket::UnconnectedState) {
        disconnect(udpSocket, &QUdpSocket::readyRead, this, &TelescopeGUI::processPendingDatagrams);
        udpSocket->close();
    }
    
    statusLabel->setText("Discovery stopped");
}

void TelescopeGUI::processPendingDatagrams() {
    // Read all available datagrams
    while (udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(udpSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;
        
        // Read the datagram
        udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        
        QString datagramStr = QString::fromUtf8(datagram);
	//        qDebug() << "Received UDP broadcast from" << sender.toString() << ":" << datagramStr;
        
        // Check if this looks like a telescope broadcast
        if (datagramStr.contains("Origin", Qt::CaseInsensitive) && 
            datagramStr.contains("IP Address", Qt::CaseInsensitive)) {
            
            // Extract the telescope model
            QString telescopeModel;
            if (datagramStr.contains("Identity:")) {
                int identityStart = datagramStr.indexOf("Identity:");
                int identityEnd = datagramStr.indexOf(" ", identityStart + 9);
                if (identityEnd > identityStart) {
                    telescopeModel = datagramStr.mid(identityStart + 9, identityEnd - identityStart - 9);
                }
            }
            
            // Extract the IP address
            QString telescopeIP;
            QRegularExpression ipRegex("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b");
            QRegularExpressionMatch match = ipRegex.match(datagramStr);
            
            if (match.hasMatch()) {
                telescopeIP = match.captured(0);
                
                // Add to our list if not already there
                if (!telescopeAddresses.contains(telescopeIP)) {
                    telescopeAddresses.append(telescopeIP);
                    
                    // Add to the UI list
                    QString displayText;
                    if (!telescopeModel.isEmpty()) {
                        displayText = QString("%1 - %2").arg(telescopeIP, telescopeModel);
                    } else {
                        displayText = QString("%1 - Celestron Origin Telescope").arg(telescopeIP);
                    }
                    
                    telescopeListWidget->addItem(displayText);
                    if (telescopeIpLineEdit && telescopeIpLineEdit->text().trimmed().isEmpty()) {
                        telescopeIpLineEdit->setText(telescopeIP);
                    }
                    
                    statusLabel->setText(QString("Found Celestron Origin telescope at %1").arg(telescopeIP));
                }
            }
        }
    }
}

void TelescopeGUI::connectToSelectedTelescope() {    
    if (originBackend->isConnected()) {
        originBackend->disconnectFromTelescope();
        return;
    }

    QListWidgetItem *selectedItem = telescopeListWidget->currentItem();
    QString ipAddress = telescopeIpLineEdit->text().trimmed();

    if (ipAddress.isEmpty() && selectedItem) {
        // Extract the IP address from the selected item
        QRegularExpression ipRegex("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b");
        QRegularExpressionMatch match = ipRegex.match(selectedItem->text());
        if (match.hasMatch()) {
            ipAddress = match.captured(0);
        }
    }

    if (ipAddress.isEmpty()) {
        statusLabel->setText("Enter a telescope IP address or select a discovered telescope");
        return;
    }

    telescopeIpLineEdit->setText(ipAddress);
    QSettings settings;
    settings.setValue("connection/telescopeIp", ipAddress);

    statusLabel->setText(QString("Connecting to telescope at %1...").arg(ipAddress));
    
    if (originBackend->connectToTelescope(ipAddress, 80)) {
        // Store the currently connected IP
        connectedIpAddress = ipAddress;
    } else {
        connectedIpAddress.clear();
        statusLabel->setText(QString("Failed to connect to telescope at %1").arg(ipAddress));
    }
}

void TelescopeGUI::onWebSocketConnected() {
    statusLabel->setText("Connected to telescope!");
    connectButton->setText("Disconnect");
}

void TelescopeGUI::onWebSocketDisconnected() {
    statusLabel->setText("Disconnected from telescope");
    connectButton->setText("Connect");
    connectedIpAddress = "";
}

void TelescopeGUI::onTextMessageReceived(const QString &message) {
    // Log the received message
    logJsonPacket(message, true);
    
    // Process the received message
    dataProcessor->processJsonPacket(message.toUtf8());
}

void TelescopeGUI::updateMountDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    mountBatteryLevelLabel->setText(data.mount.batteryLevel);
    mountBatteryVoltageLabel->setText(QString::number(data.mount.batteryVoltage, 'f', 2) + " V");
    mountChargerStatusLabel->setText(data.mount.chargerStatus);
    mountBatteryCurrentLabel->setText(QString::number(data.mount.batteryCurrent));
    mountTimeLabel->setText(data.mount.time);
    mountDateLabel->setText(data.mount.date);
    mountTimeZoneLabel->setText(data.mount.timeZone);
    
    mountLatitudeLabel->setText(QString::number(data.mount.latitude * 180.0 / M_PI, 'f', 1) + "° +/- 0.05");
    mountLongitudeLabel->setText(QString::number(data.mount.longitude * 180.0 / M_PI, 'f', 1) + "° +/- 0.05");
    
    mountIsAlignedLabel->setText(data.mount.isAligned ? "Yes" : "No");
    mountIsTrackingLabel->setText(data.mount.isTracking ? "Yes" : "No");
    mountIsGotoOverLabel->setText(data.mount.isGotoOver ? "Yes" : "No");
    mountNumAlignRefsLabel->setText(QString::number(data.mount.numAlignRefs));
    mountAzimuthLabel->setText(QString::number(data.mount.azimuth));
    mountAzimuthErrorLabel->setText(QString::number(data.mount.azimuthError));
    mountAltitudeLabel->setText(QString::number(data.mount.altitude));
    mountAltitudeErrorLabel->setText(QString::number(data.orientation.altitude));
}

void TelescopeGUI::updateCameraDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    cameraBinningLabel->setText(QString::number(data.camera.binning));
    cameraBitDepthLabel->setText(QString::number(data.camera.bitDepth));
    cameraExposureLabel->setText(QString::number(data.camera.exposure, 'f', 2) + " s");
    cameraISOLabel->setText(QString::number(data.camera.iso));
    
    // Update RGB balance display
    cameraRedBalanceLabel->setText(QString::number(data.camera.colorRBalance, 'f', 1));
    cameraGreenBalanceLabel->setText(QString::number(data.camera.colorGBalance, 'f', 1));
    cameraBlueBalanceLabel->setText(QString::number(data.camera.colorBBalance, 'f', 1));
}

void TelescopeGUI::updateFocuserDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    focuserPositionLabel->setText(QString::number(data.focuser.position));
    focuserBacklashLabel->setText(QString::number(data.focuser.backlash));
    focuserLowerLimitLabel->setText(QString::number(data.focuser.calibrationLowerLimit));
    focuserUpperLimitLabel->setText(QString::number(data.focuser.calibrationUpperLimit));
    focuserIsCalibrationCompleteLabel->setText(data.focuser.isCalibrationComplete ? "Yes" : "No");
    
    // Update calibration progress bar
    focuserCalibrationProgressBar->setValue(data.focuser.percentageCalibrationComplete);
}

void TelescopeGUI::updateEnvironmentDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    envAmbientTempLabel->setText(QString::number(data.environment.ambientTemperature, 'f', 1) + " °C");
    envCameraTempLabel->setText(QString::number(data.environment.cameraTemperature, 'f', 1) + " °C");
    envCpuTempLabel->setText(QString::number(data.environment.cpuTemperature, 'f', 1) + " °C");
    envFrontCellTempLabel->setText(QString::number(data.environment.frontCellTemperature, 'f', 1) + " °C");
    envHumidityLabel->setText(QString::number(data.environment.humidity, 'f', 0) + " %");
    envDewPointLabel->setText(QString::number(data.environment.dewPoint, 'f', 1) + " °C");
    envCpuFanLabel->setText(data.environment.cpuFanOn ? "On" : "Off");
    envOtaFanLabel->setText(data.environment.otaFanOn ? "On" : "Off");
}

void TelescopeGUI::updateImageDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    imageFileLabel->setText(data.lastImage.fileLocation);
    imageTypeLabel->setText(data.lastImage.imageType);
    imageDecLabel->setText(QString::number(data.lastImage.dec * 180.0 / M_PI, 'f', 6) + "°");
    imageRaLabel->setText(QString::number(data.lastImage.ra * 180.0 / M_PI, 'f', 6) + "°");
    imageFovXLabel->setText(QString::number(data.lastImage.fovX * 180.0 / M_PI, 'f', 4) + "°");
    imageFovYLabel->setText(QString::number(data.lastImage.fovY * 180.0 / M_PI, 'f', 4) + "°");
    
    // Request image from the telescope if connected
    if (originBackend->isConnected() && !data.lastImage.fileLocation.isEmpty()) {
        requestImage(data.lastImage.fileLocation);
    }
}

void TelescopeGUI::updateDiskDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    
    // Calculate values in GB
    double totalGB = data.disk.capacity / (1024.0 * 1024.0 * 1024.0);
    double freeGB = data.disk.freeBytes / (1024.0 * 1024.0 * 1024.0);
    double usedGB = totalGB - freeGB;
    
    diskCapacityLabel->setText(QString::number(totalGB, 'f', 2) + " GB");
    diskFreeLabel->setText(QString::number(freeGB, 'f', 2) + " GB");
    diskUsedLabel->setText(QString::number(usedGB, 'f', 2) + " GB");
    diskLevelLabel->setText(data.disk.level);
    
    // Update progress bar
    int usagePercent = (int)((usedGB / totalGB) * 100.0);
    diskUsageBar->setValue(usagePercent);
}

void TelescopeGUI::updateDewHeaterDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    dewHeaterModeLabel->setText(data.dewHeater.mode);
    dewHeaterAggressionLabel->setText(QString::number(data.dewHeater.aggression));
    dewHeaterLevelLabel->setText(QString::number(data.dewHeater.heaterLevel * 100.0, 'f', 0) + " %");
    dewHeaterManualPowerLabel->setText(QString::number(data.dewHeater.manualPowerLevel * 100.0, 'f', 0) + " %");
    
    // Update progress bar
    int heaterLevel = (int)(data.dewHeater.heaterLevel * 100.0);
    dewHeaterLevelBar->setValue(heaterLevel);
}

void TelescopeGUI::updateTimeDisplay() {
    QDateTime now = QDateTime::currentDateTime();
    
    // Update connection status time
    if (originBackend->isConnected()) {
        const TelescopeData &data = dataProcessor->getData();
        
        // Calculate time since last update for each component
        updateLastUpdateLabel(mountLastUpdateLabel, data.mountLastUpdate);
        updateLastUpdateLabel(cameraLastUpdateLabel, data.cameraLastUpdate);
        updateLastUpdateLabel(focuserLastUpdateLabel, data.focuserLastUpdate);
        updateLastUpdateLabel(environmentLastUpdateLabel, data.environmentLastUpdate);
        updateLastUpdateLabel(imageLastUpdateLabel, data.imageLastUpdate);
        updateLastUpdateLabel(diskLastUpdateLabel, data.diskLastUpdate);
        updateLastUpdateLabel(dewHeaterLastUpdateLabel, data.dewHeaterLastUpdate);
    }
}

void TelescopeGUI::setupUI() {
    QMenuBar* menuBar = new QMenuBar(this);
    setMenuBar(menuBar);
    
    // Tools menu
    QMenu* toolsMenu = menuBar->addMenu("Tools");
    
    QAction* replayLogsAction = new QAction("Replay WebSocket Logs...", this);
    replayLogsAction->setShortcut(QKeySequence("Ctrl+R"));
    connect(replayLogsAction, &QAction::triggered, this, &TelescopeGUI::showLogReplay);
    toolsMenu->addAction(replayLogsAction);
  
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    
    // Discovery and connection panel
    QGroupBox *discoveryBox = new QGroupBox("Telescope Discovery and Connection", centralWidget);
    QVBoxLayout *discoveryLayout = new QVBoxLayout(discoveryBox);
    
    // Status and controls
    QHBoxLayout *controlLayout = new QHBoxLayout();

    QHBoxLayout *addressLayout = new QHBoxLayout();
    addressLayout->addWidget(new QLabel("Telescope IP:", discoveryBox));
    telescopeIpLineEdit = new QLineEdit(discoveryBox);
    telescopeIpLineEdit->setPlaceholderText("Enter IP address, or select a discovered telescope below");
    QSettings settings;
    telescopeIpLineEdit->setText(settings.value("connection/telescopeIp").toString());
    connect(telescopeIpLineEdit, &QLineEdit::editingFinished, this, [this]() {
        const QString ipAddress = telescopeIpLineEdit->text().trimmed();
        telescopeIpLineEdit->setText(ipAddress);
        QSettings settings;
        settings.setValue("connection/telescopeIp", ipAddress);
    });
    addressLayout->addWidget(telescopeIpLineEdit, 1);
    discoveryLayout->addLayout(addressLayout);
    
    QPushButton *discoverButton = new QPushButton("Discover Telescopes", discoveryBox);
    connect(discoverButton, &QPushButton::clicked, this, &TelescopeGUI::startDiscovery);
    controlLayout->addWidget(discoverButton);
    
    connectButton = new QPushButton("Connect", discoveryBox);
    connect(connectButton, &QPushButton::clicked, this, &TelescopeGUI::connectToSelectedTelescope);
    controlLayout->addWidget(connectButton);
    
    statusLabel = new QLabel("Ready to discover telescopes", discoveryBox);
    controlLayout->addWidget(statusLabel, 1); // Give it a stretch factor
    
    discoveryLayout->addLayout(controlLayout);
    
    // Telescope list
    telescopeListWidget = new QListWidget(discoveryBox);
    connect(telescopeListWidget, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        QRegularExpression ipRegex("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b");
        QRegularExpressionMatch match = ipRegex.match(item->text());
        if (match.hasMatch()) {
            const QString ipAddress = match.captured(0);
            telescopeIpLineEdit->setText(ipAddress);
            QSettings settings;
            settings.setValue("connection/telescopeIp", ipAddress);
        }
    });
    discoveryLayout->addWidget(telescopeListWidget);
    
    mainLayout->addWidget(discoveryBox);
    
    // Tab widget for different categories
    QTabWidget *tabWidget = new QTabWidget(centralWidget);
    
    // Create tabs
    tabWidget->addTab(createMountTab(), "Mount");
    tabWidget->addTab(createCameraTab(), "Camera");
    tabWidget->addTab(createFocuserTab(), "Focuser");
    tabWidget->addTab(createEnvironmentTab(), "Environment");
    tabWidget->addTab(createImageTab(), "Show Image");
    tabWidget->addTab(createDiskTab(), "Disk");
    tabWidget->addTab(createDewHeaterTab(), "Dew Heater");
    tabWidget->addTab(createSlewAndImageTab(), "Slew & Image");   
    tabWidget->addTab(createTaskControllerTab(), "Task Controller");   
    tabWidget->addTab(createCommandTab(), "Commands");
    tabWidget->addTab(new AutopilotTab(m_autopilotController, this), "Autopilot");
    tabWidget->addTab(new AlignmentTab(m_alignmentController, this), "Alignment");
    tabWidget->addTab(new JsonMonitorTab(originBackend, this), "JSON Monitor");

    mainLayout->addWidget(tabWidget);
}

QWidget* TelescopeGUI::createCommandTab() {
    CommandInterface *commandInterface = new CommandInterface(this, this);
    return commandInterface;
}

// ============================================================================
// Add to TelescopeGUI.cpp - Create the tab
// ============================================================================

QWidget* TelescopeGUI::createTaskControllerTab() {
    QWidget *tab = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);
    
    // Initialize section
    QGroupBox *initstatusGroup = new QGroupBox("Telescope Initialization", tab);
    QVBoxLayout *initvboxLayout = new QVBoxLayout(initstatusGroup);
    
    // Status display
    QHBoxLayout *initstatusLayout = new QHBoxLayout();
    initstatusLayout->addWidget(new QLabel("Alignment Status:"));
    alignmentStatusLabel = new QLabel("Unknown", initstatusGroup);
    initstatusLayout->addWidget(alignmentStatusLabel);
    
    initstatusLayout->addWidget(new QLabel("Mount Status:"));
    mountStatusLabel = new QLabel("Unknown", initstatusGroup);
    initstatusLayout->addWidget(mountStatusLabel);
    
    initvboxLayout->addLayout(initstatusLayout);
    
    // Initialization button
    QHBoxLayout *initButtonLayout = new QHBoxLayout();
    initializeButton = new QPushButton("Initialize Telescope", initstatusGroup);
    connect(initializeButton, &QPushButton::clicked, this, &TelescopeGUI::initializeTelescope);
    initButtonLayout->addWidget(initializeButton);
    initvboxLayout->addLayout(initButtonLayout);
    
    mainLayout->addWidget(initstatusGroup);
    
    // Overall Status Group
    QGroupBox *statusGroup = new QGroupBox("Overall Status", tab);
    QGridLayout *statusLayout = new QGridLayout(statusGroup);
    
    int row = 0;
    
    statusLayout->addWidget(new QLabel("State:"), row, 0);
    taskStateLabel = new QLabel("-", statusGroup);
    taskStateLabel->setFont(QFont("Arial", 12, QFont::Bold));
    statusLayout->addWidget(taskStateLabel, row++, 1);
    
    statusLayout->addWidget(new QLabel("Stage:"), row, 0);
    taskStageLabel = new QLabel("-", statusGroup);
    statusLayout->addWidget(taskStageLabel, row++, 1);
    
    statusLayout->addWidget(new QLabel("Ready:"), row, 0);
    taskReadyLabel = new QLabel("-", statusGroup);
    taskReadyLabel->setFont(QFont("Arial", 10, QFont::Bold));
    statusLayout->addWidget(taskReadyLabel, row++, 1);
    
    mainLayout->addWidget(statusGroup);
    
    // Initialization Group
    QGroupBox *initGroup = new QGroupBox("Initialization Progress", tab);
    QGridLayout *initLayout = new QGridLayout(initGroup);
    
    row = 0;
    
    initLayout->addWidget(new QLabel("Current Step:"), row, 0);
    taskCurrentStepLabel = new QLabel("-", initGroup);
    taskCurrentStepLabel->setFont(QFont("Arial", 11, QFont::Bold));
    initLayout->addWidget(taskCurrentStepLabel, row++, 1);
    
    initLayout->addWidget(new QLabel("Alignment Points:"), row, 0);
    taskNumPointsLabel = new QLabel("-", initGroup);
    initLayout->addWidget(taskNumPointsLabel, row++, 1);
    
    initLayout->addWidget(new QLabel("Progress:"), row, 0);
    taskProgressBar = new QProgressBar(initGroup);
    taskProgressBar->setRange(0, 100);
    taskProgressBar->setValue(0);
    taskProgressBar->setTextVisible(true);
    taskProgressBar->setFormat("%p% complete");
    initLayout->addWidget(taskProgressBar, row++, 1);
    
    mainLayout->addWidget(initGroup);
    
    // Focus Group
    QGroupBox *focusGroup = new QGroupBox("Focus Progress", tab);
    QGridLayout *focusLayout = new QGridLayout(focusGroup);
    
    row = 0;
    
    focusLayout->addWidget(new QLabel("Focuser Position:"), row, 0);
    taskFocusPositionLabel = new QLabel("-", focusGroup);
    focusLayout->addWidget(taskFocusPositionLabel, row++, 1);
    
    focusLayout->addWidget(new QLabel("Focus Progress:"), row, 0);
    taskFocusProgressBar = new QProgressBar(focusGroup);
    taskFocusProgressBar->setRange(0, 100);
    taskFocusProgressBar->setValue(0);
    taskFocusProgressBar->setTextVisible(true);
    focusLayout->addWidget(taskFocusProgressBar, row++, 1);
    
    mainLayout->addWidget(focusGroup);
    
    // Imaging Group
    QGroupBox *imagingGroup = new QGroupBox("Imaging Session", tab);
    QGridLayout *imagingLayout = new QGridLayout(imagingGroup);
    
    row = 0;
    
    imagingLayout->addWidget(new QLabel("Session Name:"), row, 0);
    taskImagingNameLabel = new QLabel("-", imagingGroup);
    imagingLayout->addWidget(taskImagingNameLabel, row++, 1);
    
    mainLayout->addWidget(imagingGroup);
    
    // Last update
    taskLastUpdateLabel = new QLabel("Last Update: Never", tab);
    mainLayout->addWidget(taskLastUpdateLabel);
    
    // Add stretch to push everything up
    mainLayout->addStretch(1);
    
    return tab;
}

// ============================================================================
// Add to TelescopeGUI.cpp - Update display handler
// ============================================================================

void TelescopeGUI::updateTaskControllerDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    const TaskControllerStatus &tc = data.taskController;
    
    // Update state with color coding
    taskStateLabel->setText(tc.state);
    if (tc.state == "INITIALIZING") {
        taskStateLabel->setStyleSheet("color: orange;");
    } else if (tc.state == "IDLE") {
        taskStateLabel->setStyleSheet("color: green;");
	initializeButton->setEnabled(true);
    } else if (tc.state == "IMAGING") {
        taskStateLabel->setStyleSheet("color: blue;");
    } else {
        taskStateLabel->setStyleSheet("color: black;");
    }
    
    // Update stage
    taskStageLabel->setText(tc.stage);
    
    // Update ready status with color
    taskReadyLabel->setText(tc.isReady ? "YES" : "NO");
    taskReadyLabel->setStyleSheet(tc.isReady ? "color: green;" : "color: red;");
    
    // Update initialization info
    if (!tc.currentStep.isEmpty()) {
        taskCurrentStepLabel->setText(tc.currentStep);
        
        // Color code the current step
        if (tc.currentStep == "NONE") {
            taskCurrentStepLabel->setStyleSheet("color: gray;");
        } else if (tc.currentStep == "MOVING_MOUNT") {
            taskCurrentStepLabel->setStyleSheet("color: blue;");
        } else if (tc.currentStep == "ALIGNING") {
            taskCurrentStepLabel->setStyleSheet("color: orange;");
        } else if (tc.currentStep == "FOCUSING") {
            taskCurrentStepLabel->setStyleSheet("color: purple;");
        } else {
            taskCurrentStepLabel->setStyleSheet("color: black;");
        }
    } else {
        taskCurrentStepLabel->setText("-");
        taskCurrentStepLabel->setStyleSheet("");
    }
    
    taskNumPointsLabel->setText(QString::number(tc.numPoints));
    taskProgressBar->setValue(tc.percentageComplete);
    
    // Update focus info
    if (tc.focusPosition > 0) {
        taskFocusPositionLabel->setText(QString::number(tc.focusPosition));
    } else {
        taskFocusPositionLabel->setText("-");
    }
    taskFocusProgressBar->setValue(tc.focusPercentageComplete);
    
    // Update imaging info
    if (!tc.imagingName.isEmpty()) {
        taskImagingNameLabel->setText(tc.imagingName);
    } else {
        taskImagingNameLabel->setText("-");
    }
    
    // Update last update time
    updateLastUpdateLabel(taskLastUpdateLabel, data.taskControllerLastUpdate);
    
    // Also update the main status label at top of GUI
    if (statusLabel) {
        QString statusText = QString("Status: %1").arg(tc.state);
        if (tc.state == "INITIALIZING") {
            statusText += QString(" - %1 (%2%)").arg(tc.currentStep).arg(tc.percentageComplete);
        }
        statusLabel->setText(statusText);
    }
}

QWidget* TelescopeGUI::createMountTab() {
    QWidget *tab = new QWidget();
    QGridLayout *layout = new QGridLayout(tab);
    
    int row = 0;
    
    // Add labels for mount data
    layout->addWidget(new QLabel("Battery Current:"), row, 0);
    mountBatteryCurrentLabel = new QLabel("-", tab);
    layout->addWidget(mountBatteryCurrentLabel, row++, 1);
    
    layout->addWidget(new QLabel("Battery Level:"), row, 0);
    mountBatteryLevelLabel = new QLabel("-", tab);
    layout->addWidget(mountBatteryLevelLabel, row++, 1);
    
    layout->addWidget(new QLabel("Battery Voltage:"), row, 0);
    mountBatteryVoltageLabel = new QLabel("-", tab);
    layout->addWidget(mountBatteryVoltageLabel, row++, 1);
    
    layout->addWidget(new QLabel("Charger Status:"), row, 0);
    mountChargerStatusLabel = new QLabel("-", tab);
    layout->addWidget(mountChargerStatusLabel, row++, 1);
    
    layout->addWidget(new QLabel("Time:"), row, 0);
    mountTimeLabel = new QLabel("-", tab);
    layout->addWidget(mountTimeLabel, row++, 1);
    
    layout->addWidget(new QLabel("Date:"), row, 0);
    mountDateLabel = new QLabel("-", tab);
    layout->addWidget(mountDateLabel, row++, 1);
    
    layout->addWidget(new QLabel("Time Zone:"), row, 0);
    mountTimeZoneLabel = new QLabel("-", tab);
    layout->addWidget(mountTimeZoneLabel, row++, 1);
    
    layout->addWidget(new QLabel("Latitude:"), row, 0);
    mountLatitudeLabel = new QLabel("-", tab);
    layout->addWidget(mountLatitudeLabel, row++, 1);
    
    layout->addWidget(new QLabel("Longitude:"), row, 0);
    mountLongitudeLabel = new QLabel("-", tab);
    layout->addWidget(mountLongitudeLabel, row++, 1);
    
    layout->addWidget(new QLabel("Is Aligned:"), row, 0);
    mountIsAlignedLabel = new QLabel("-", tab);
    layout->addWidget(mountIsAlignedLabel, row++, 1);
    
    layout->addWidget(new QLabel("Is Tracking:"), row, 0);
    mountIsTrackingLabel = new QLabel("-", tab);
    layout->addWidget(mountIsTrackingLabel, row++, 1);
    
    layout->addWidget(new QLabel("Is Goto Over:"), row, 0);
    mountIsGotoOverLabel = new QLabel("-", tab);
    layout->addWidget(mountIsGotoOverLabel, row++, 1);
    
    layout->addWidget(new QLabel("Num Align Refs:"), row, 0);
    mountNumAlignRefsLabel = new QLabel("-", tab);
    layout->addWidget(mountNumAlignRefsLabel, row++, 1);

    layout->addWidget(new QLabel("Azimuth:"), row, 0);
    mountAzimuthLabel = new QLabel("-", tab);
    layout->addWidget(mountAzimuthLabel, row++, 1);

    layout->addWidget(new QLabel("Azimuth Error:"), row, 0);
    mountAzimuthErrorLabel = new QLabel("-", tab);
    layout->addWidget(mountAzimuthErrorLabel, row++, 1);

    layout->addWidget(new QLabel("Altitude:"), row, 0);
    mountAltitudeLabel = new QLabel("-", tab);
    layout->addWidget(mountAltitudeLabel, row++, 1);

    layout->addWidget(new QLabel("Altitude Error:"), row, 0);
    mountAltitudeErrorLabel = new QLabel("-", tab);
    layout->addWidget(mountAltitudeErrorLabel, row++, 1);
        
    layout->addWidget(new QLabel("Last Update:"), row, 0);
    mountLastUpdateLabel = new QLabel("-", tab);
    layout->addWidget(mountLastUpdateLabel, row++, 1);
    
    // Add separator
    QFrame* line = new QFrame(tab);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line, row++, 0, 1, 2);
    
    // Add Park/Unpark controls
    QGroupBox *parkGroup = new QGroupBox("Park/Unpark Controls", tab);
    QVBoxLayout *parkLayout = new QVBoxLayout(parkGroup);
    
    QLabel *parkInfoLabel = new QLabel("Park: Move to Alt -10° (safe storage position)\n"
                                       "Unpark: Move to Alt +60° and initialize", parkGroup);
    parkInfoLabel->setWordWrap(true);
    parkInfoLabel->setStyleSheet("QLabel { color: gray; font-size: 10px; }");
    parkLayout->addWidget(parkInfoLabel);
    
    QHBoxLayout *parkButtonLayout = new QHBoxLayout();
    
    QPushButton *parkButton = new QPushButton("Park Telescope", parkGroup);
    parkButton->setToolTip("Move telescope to parked position (Alt: -10°)");
    connect(parkButton, &QPushButton::clicked, this, [this]() {
        if (originBackend->isConnected()) {
            qDebug() << "Park button clicked";
            originBackend->parkMount();
        } else {
            QMessageBox::warning(this, "Not Connected", 
                               "Please connect to telescope first");
        }
    });
    parkButtonLayout->addWidget(parkButton);
    
    QPushButton *unparkButton = new QPushButton("Unpark Telescope", parkGroup);
    unparkButton->setToolTip("Move telescope to operational position (Alt: 60°) and initialize");
    connect(unparkButton, &QPushButton::clicked, this, [this]() {
        if (originBackend->isConnected()) {
            qDebug() << "Unpark button clicked";
            originBackend->unparkMount();
        } else {
            QMessageBox::warning(this, "Not Connected", 
                               "Please connect to telescope first");
        }
    });
    parkButtonLayout->addWidget(unparkButton);
    
    parkLayout->addLayout(parkButtonLayout);
    layout->addWidget(parkGroup, row++, 0, 1, 2);
    
    layout->setRowStretch(row, 1);
    return tab;
}

QWidget* TelescopeGUI::createCameraTab() {
    QWidget *tab = new QWidget();
    QGridLayout *layout = new QGridLayout(tab);
    
    int row = 0;
    
    layout->addWidget(new QLabel("Binning:"), row, 0);
    cameraBinningLabel = new QLabel("-", tab);
    layout->addWidget(cameraBinningLabel, row++, 1);
    
    layout->addWidget(new QLabel("Bit Depth:"), row, 0);
    cameraBitDepthLabel = new QLabel("-", tab);
    layout->addWidget(cameraBitDepthLabel, row++, 1);
    
    layout->addWidget(new QLabel("Exposure:"), row, 0);
    cameraExposureLabel = new QLabel("-", tab);
    layout->addWidget(cameraExposureLabel, row++, 1);
    
    layout->addWidget(new QLabel("ISO:"), row, 0);
    cameraISOLabel = new QLabel("-", tab);
    layout->addWidget(cameraISOLabel, row++, 1);
    
    // Color balance group
    QGroupBox *colorBalanceGroup = new QGroupBox("Color Balance", tab);
    QGridLayout *colorLayout = new QGridLayout(colorBalanceGroup);
    
    colorLayout->addWidget(new QLabel("Red:"), 0, 0);
    cameraRedBalanceLabel = new QLabel("-", colorBalanceGroup);
    colorLayout->addWidget(cameraRedBalanceLabel, 0, 1);
    
    colorLayout->addWidget(new QLabel("Green:"), 1, 0);
    cameraGreenBalanceLabel = new QLabel("-", colorBalanceGroup);
    colorLayout->addWidget(cameraGreenBalanceLabel, 1, 1);
    
    colorLayout->addWidget(new QLabel("Blue:"), 2, 0);
    cameraBlueBalanceLabel = new QLabel("-", colorBalanceGroup);
    colorLayout->addWidget(cameraBlueBalanceLabel, 2, 1);
    
    layout->addWidget(colorBalanceGroup, row++, 0, 1, 2);
    
    layout->addWidget(new QLabel("Last Update:"), row, 0);
    cameraLastUpdateLabel = new QLabel("-", tab);
    layout->addWidget(cameraLastUpdateLabel, row++, 1);
    
    layout->setRowStretch(row, 1);
    return tab;
}

QWidget* TelescopeGUI::createFocuserTab() {
    QWidget *tab = new QWidget();
    QGridLayout *layout = new QGridLayout(tab);
    
    int row = 0;
    
    layout->addWidget(new QLabel("Position:"), row, 0);
    focuserPositionLabel = new QLabel("-", tab);
    layout->addWidget(focuserPositionLabel, row++, 1);
    
    layout->addWidget(new QLabel("Backlash:"), row, 0);
    focuserBacklashLabel = new QLabel("-", tab);
    layout->addWidget(focuserBacklashLabel, row++, 1);
    
    layout->addWidget(new QLabel("Lower Limit:"), row, 0);
    focuserLowerLimitLabel = new QLabel("-", tab);
    layout->addWidget(focuserLowerLimitLabel, row++, 1);
    
    layout->addWidget(new QLabel("Upper Limit:"), row, 0);
    focuserUpperLimitLabel = new QLabel("-", tab);
    layout->addWidget(focuserUpperLimitLabel, row++, 1);
    
    layout->addWidget(new QLabel("Is Calibration Complete:"), row, 0);
    focuserIsCalibrationCompleteLabel = new QLabel("-", tab);
    layout->addWidget(focuserIsCalibrationCompleteLabel, row++, 1);
    
    layout->addWidget(new QLabel("Calibration Progress:"), row, 0);
    focuserCalibrationProgressBar = new QProgressBar(tab);
    focuserCalibrationProgressBar->setRange(0, 100);
    focuserCalibrationProgressBar->setValue(0);
    layout->addWidget(focuserCalibrationProgressBar, row++, 1);
    
    layout->addWidget(new QLabel("Last Update:"), row, 0);
    focuserLastUpdateLabel = new QLabel("-", tab);
    layout->addWidget(focuserLastUpdateLabel, row++, 1);
    
    layout->setRowStretch(row, 1);
    return tab;
}

QWidget* TelescopeGUI::createEnvironmentTab() {
    QWidget *tab = new QWidget();
    QGridLayout *layout = new QGridLayout(tab);
    
    int row = 0;
    
    layout->addWidget(new QLabel("Ambient Temperature:"), row, 0);
    envAmbientTempLabel = new QLabel("-", tab);
    layout->addWidget(envAmbientTempLabel, row++, 1);
    
    layout->addWidget(new QLabel("Camera Temperature:"), row, 0);
    envCameraTempLabel = new QLabel("-", tab);
    layout->addWidget(envCameraTempLabel, row++, 1);
    
    layout->addWidget(new QLabel("CPU Temperature:"), row, 0);
    envCpuTempLabel = new QLabel("-", tab);
    layout->addWidget(envCpuTempLabel, row++, 1);
    
    layout->addWidget(new QLabel("Front Cell Temperature:"), row, 0);
    envFrontCellTempLabel = new QLabel("-", tab);
    layout->addWidget(envFrontCellTempLabel, row++, 1);
    
    layout->addWidget(new QLabel("Humidity:"), row, 0);
    envHumidityLabel = new QLabel("-", tab);
    layout->addWidget(envHumidityLabel, row++, 1);
    
    layout->addWidget(new QLabel("Dew Point:"), row, 0);
    envDewPointLabel = new QLabel("-", tab);
    layout->addWidget(envDewPointLabel, row++, 1);
    
    layout->addWidget(new QLabel("CPU Fan:"), row, 0);
    envCpuFanLabel = new QLabel("-", tab);
    layout->addWidget(envCpuFanLabel, row++, 1);
    
    layout->addWidget(new QLabel("OTA Fan:"), row, 0);
    envOtaFanLabel = new QLabel("-", tab);
    layout->addWidget(envOtaFanLabel, row++, 1);
    
    layout->addWidget(new QLabel("Last Update:"), row, 0);
    environmentLastUpdateLabel = new QLabel("-", tab);
    layout->addWidget(environmentLastUpdateLabel, row++, 1);
    
    layout->setRowStretch(row, 1);
    return tab;
}
// ============================================================================
// ENHANCED SHOW IMAGE TAB - Modified createImageTab() for TelescopeGUI.cpp
// This integrates camera snapshot/exposure/ISO controls into your existing tab
// ============================================================================

QWidget* TelescopeGUI::createImageTab() {
    QWidget *tab = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);
    
    // Camera Control Panel
    QGroupBox *cameraControlGroup = new QGroupBox("Camera Control", tab);
    QGridLayout *cameraControlLayout = new QGridLayout(cameraControlGroup);
    
    int ctrlRow = 0;
    
    // Mode Control
    cameraControlLayout->addWidget(new QLabel("Mode:"), ctrlRow, 0);
    QHBoxLayout *modeLayout = new QHBoxLayout();
    QPushButton *manualModeBtn = new QPushButton("Manual", tab);
    QPushButton *autoModeBtn = new QPushButton("Auto", tab);
    
    // Connect DIRECTLY to backend (no camera controller!)
    connect(manualModeBtn, &QPushButton::clicked, 
            originBackend, &OriginBackend::setCameraManualMode);
    connect(autoModeBtn, &QPushButton::clicked,
            originBackend, &OriginBackend::setCameraAutoMode);
    
    modeLayout->addWidget(manualModeBtn);
    modeLayout->addWidget(autoModeBtn);
    modeLayout->addStretch();
    cameraControlLayout->addLayout(modeLayout, ctrlRow++, 1, 1, 2);
    
    // Exposure Control
    cameraControlLayout->addWidget(new QLabel("Exposure (sec):"), ctrlRow, 0);
    exposureSpinBox = new QDoubleSpinBox(tab);
    exposureSpinBox->setRange(0.001, 300.0);
    exposureSpinBox->setValue(0.1);
    exposureSpinBox->setDecimals(6);
    
    connect(exposureSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            originBackend, &OriginBackend::setCameraExposure);
    
    cameraControlLayout->addWidget(exposureSpinBox, ctrlRow++, 1);
    
    // Binning Control
    cameraControlLayout->addWidget(new QLabel("Binning:"), ctrlRow, 0);
    binningSpinBox = new QSpinBox(tab);
    binningSpinBox->setRange(1, 2);
    binningSpinBox->setValue(1);
    
    connect(binningSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            originBackend, &OriginBackend::setCameraBinning);
    
    cameraControlLayout->addWidget(binningSpinBox, ctrlRow++, 1);
    
    // ISO Control
    cameraControlLayout->addWidget(new QLabel("ISO:"), ctrlRow, 0);
    isoSpinBox = new QSpinBox(tab);
    isoSpinBox->setRange(100, 6400);
    isoSpinBox->setValue(200);
    
    connect(isoSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            originBackend, &OriginBackend::setCameraISO);
    
    cameraControlLayout->addWidget(isoSpinBox, ctrlRow++, 1);
    
    // Snapshot Button - connects to backend!
    snapshotButton = new QPushButton("Take Snapshot", tab);
    snapshotButton->setStyleSheet("font-weight: bold; padding: 8px; background-color: #4CAF50; color: white;");
    
    connect(snapshotButton, &QPushButton::clicked, this, [this]() {
        bool success = originBackend->takeSingleSnapshot();
        snapshotButton->setEnabled(!success);
        snapshotButton->setText("Capturing...");
    });
    
    cameraControlLayout->addWidget(snapshotButton, ctrlRow++, 1, 1, 2);
    
    // Progress bar
    snapshotProgressBar = new QProgressBar(tab);
    snapshotProgressBar->setVisible(false);
    cameraControlLayout->addWidget(snapshotProgressBar, ctrlRow++, 1, 1, 2);
    
    mainLayout->addWidget(cameraControlGroup);
    
    // ========================================================================
    // MIDDLE SECTION: Original Two-Panel Layout
    // ========================================================================
    QSplitter *splitter = new QSplitter(Qt::Horizontal, tab);
    mainLayout->addWidget(splitter, 1);  // Give it stretch priority
    
    // Left panel - Image info
    QWidget *infoPanel = new QWidget(splitter);
    QGridLayout *infoLayout = new QGridLayout(infoPanel);
    
    int row = 0;
    
    infoLayout->addWidget(new QLabel("File Location:"), row, 0);
    imageFileLabel = new QLabel("-", infoPanel);
    imageFileLabel->setWordWrap(true);
    infoLayout->addWidget(imageFileLabel, row++, 1);
    
    infoLayout->addWidget(new QLabel("Image Type:"), row, 0);
    imageTypeLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageTypeLabel, row++, 1);
    
    infoLayout->addWidget(new QLabel("Declination:"), row, 0);
    imageDecLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageDecLabel, row++, 1);
    
    infoLayout->addWidget(new QLabel("Right Ascension:"), row, 0);
    imageRaLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageRaLabel, row++, 1);
    
    infoLayout->addWidget(new QLabel("Field of View X:"), row, 0);
    imageFovXLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageFovXLabel, row++, 1);
    
    infoLayout->addWidget(new QLabel("Field of View Y:"), row, 0);
    imageFovYLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageFovYLabel, row++, 1);
    
    // Hand control buttons
    auto *upButton = new QPushButton("Up");
    auto *downButton = new QPushButton("Down");
    auto *leftButton = new QPushButton("Left");
    auto *rightButton = new QPushButton("Right");
    connect(upButton, &QPushButton::clicked, this, &TelescopeGUI::startUpButton);
    connect(downButton, &QPushButton::clicked, this, &TelescopeGUI::startDownButton);
    connect(leftButton, &QPushButton::clicked, this, &TelescopeGUI::startLeftButton);
    connect(rightButton, &QPushButton::clicked, this, &TelescopeGUI::startRightButton);
    
    infoLayout->addWidget(upButton, row++, 1);
    infoLayout->addWidget(leftButton, row, 0);
    infoLayout->addWidget(rightButton, row++, 2);
    infoLayout->addWidget(downButton, row++, 1);
    
    infoLayout->addWidget(new QLabel("Last Update:"), row, 0);
    imageLastUpdateLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageLastUpdateLabel, row++, 1);
    
    // Add vertical space at the bottom
    infoLayout->setRowStretch(row, 1);
    
    // Right panel - Image preview
    QWidget *imagePanel = new QWidget(splitter);
    QVBoxLayout *imageLayout = new QVBoxLayout(imagePanel);
    
    imagePreviewLabel = new QLabel(imagePanel);
    imagePreviewLabel->setMinimumSize(400, 300);
    imagePreviewLabel->setAlignment(Qt::AlignCenter);
    imagePreviewLabel->setScaledContents(false);
    imagePreviewLabel->setText("No image available");
    imagePreviewLabel->setStyleSheet("QLabel { background-color: #2b2b2b; border: 2px solid #555; }");
    
    imageLayout->addWidget(imagePreviewLabel);
    
    // Set stretch factors for the splitter
    splitter->setStretchFactor(0, 1);  // Info panel
    splitter->setStretchFactor(1, 3);  // Image panel
    // Add these connections at the end of createImageTab(), before returning tab:

    // Connect backend camera signals to update UI
    connect(originBackend, &OriginBackend::cameraModeChanged,
	    this, &TelescopeGUI::onCameraModeChanged);

    connect(originBackend, &OriginBackend::captureParametersChanged,
	    this, [this](double exposure, int iso, int binning) {
		// Update spinboxes without triggering signals
		exposureSpinBox->blockSignals(true);
		binningSpinBox->blockSignals(true);
		isoSpinBox->blockSignals(true);

		exposureSpinBox->setValue(exposure);
		binningSpinBox->setValue(binning);
		isoSpinBox->setValue(iso);

		exposureSpinBox->blockSignals(false);
		binningSpinBox->blockSignals(false);
		isoSpinBox->blockSignals(false);

		// qDebug() << "Capture parameters updated:" << exposure << iso;
	    });

    // Re-enable snapshot button when image is ready
    connect(originBackend, &OriginBackend::imageReady,
	    this, [this]() {
		if (snapshotButton) {
		    snapshotButton->setEnabled(true);
		    snapshotButton->setText("Take Snapshot");
		}
		if (snapshotProgressBar) {
		    snapshotProgressBar->setVisible(false);
		}
	    });
 
    return tab;
}

// Signal Handlers
void TelescopeGUI::onCameraModeChanged(bool isManual) {
    qDebug() << "Camera mode changed:" << (isManual ? "Manual" : "Auto");
    
    // Update UI to reflect mode
    if (exposureSpinBox && isoSpinBox) {
        exposureSpinBox->setEnabled(isManual);
        isoSpinBox->setEnabled(isManual);
    }
}

void TelescopeGUI::onCaptureParametersChanged(double exposure, int iso) {
    qDebug() << "Capture parameters updated: Exposure =" << exposure << "ISO =" << iso;
    
    // Update spin boxes without triggering their signals
    if (exposureSpinBox) {
        exposureSpinBox->blockSignals(true);
        exposureSpinBox->setValue(exposure);
        exposureSpinBox->blockSignals(false);
    }
    
    if (isoSpinBox) {
        isoSpinBox->blockSignals(true);
        isoSpinBox->setValue(iso);
        isoSpinBox->blockSignals(false);
    }
}

void TelescopeGUI::onSnapshotReady(const QString& fileLocation, double ra, double dec) {
    qDebug() << "Snapshot ready at" << fileLocation;
    qDebug() << "Position: RA =" << ra << "Dec =" << dec;
    
    // Update image info labels
    if (imageFileLabel) {
        imageFileLabel->setText(fileLocation);
    }
    if (imageRaLabel) {
        imageRaLabel->setText(QString::number(ra, 'f', 6));
    }
    if (imageDecLabel) {
        imageDecLabel->setText(QString::number(dec, 'f', 6));
    }
    
    // Generate save path with timestamp
    QString filename = QFileInfo(fileLocation).fileName();
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString extension = QFileInfo(filename).suffix();
    QString basename = QFileInfo(filename).baseName();
    QString savePath = m_snapshotSavePath + "/" + basename + "_" + timestamp + "." + extension;
    
    // Show progress bar
    if (snapshotProgressBar) {
        snapshotProgressBar->setVisible(true);
        snapshotProgressBar->setValue(0);
        snapshotProgressBar->setFormat("Downloading snapshot... %p%");
    }
}

void TelescopeGUI::onSnapshotDownloaded(const QString& localPath) {
    qDebug() << "Snapshot downloaded to:" << localPath;
    
    // Hide progress bar
    if (snapshotProgressBar) {
        snapshotProgressBar->setVisible(false);
    }
    
    // Re-enable snapshot button
    if (snapshotButton) {
        snapshotButton->setEnabled(true);
        snapshotButton->setText("Take Snapshot");
    }
    
    // Load and display the downloaded image
    QPixmap pixmap(localPath);
    if (!pixmap.isNull() && imagePreviewLabel) {
        // Scale to fit while preserving aspect ratio
        QPixmap scaledPixmap = pixmap.scaled(imagePreviewLabel->size(), 
                                            Qt::KeepAspectRatio, 
                                            Qt::SmoothTransformation);
        imagePreviewLabel->setPixmap(scaledPixmap);
        
        qDebug() << "Image displayed successfully";
    } else {
        qWarning() << "Failed to load downloaded image";
    }
    
    // Optional: Show notification
    // QMessageBox::information(this, "Snapshot Saved", 
    //                         "Snapshot saved to:\n" + localPath);
}

void TelescopeGUI::onSnapshotDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (snapshotProgressBar && bytesTotal > 0) {
        int percent = (bytesReceived * 100) / bytesTotal;
        snapshotProgressBar->setValue(percent);
    }
}

QWidget* TelescopeGUI::createDiskTab() {
    QWidget *tab = new QWidget();
    QGridLayout *layout = new QGridLayout(tab);
    
    int row = 0;
    
    layout->addWidget(new QLabel("Total Capacity:"), row, 0);
    diskCapacityLabel = new QLabel("-", tab);
    layout->addWidget(diskCapacityLabel, row++, 1);
    
    layout->addWidget(new QLabel("Free Space:"), row, 0);
    diskFreeLabel = new QLabel("-", tab);
    layout->addWidget(diskFreeLabel, row++, 1);
    
    layout->addWidget(new QLabel("Used Space:"), row, 0);
    diskUsedLabel = new QLabel("-", tab);
    layout->addWidget(diskUsedLabel, row++, 1);
    
    layout->addWidget(new QLabel("Level:"), row, 0);
    diskLevelLabel = new QLabel("-", tab);
    layout->addWidget(diskLevelLabel, row++, 1);
    
    layout->addWidget(new QLabel("Disk Usage:"), row, 0);
    diskUsageBar = new QProgressBar(tab);
    diskUsageBar->setRange(0, 100);
    diskUsageBar->setValue(0);
    layout->addWidget(diskUsageBar, row++, 1);
    
    layout->addWidget(new QLabel("Last Update:"), row, 0);
    diskLastUpdateLabel = new QLabel("-", tab);
    layout->addWidget(diskLastUpdateLabel, row++, 1);
    
    layout->setRowStretch(row, 1);
    return tab;
}

QWidget* TelescopeGUI::createDewHeaterTab() {
    QWidget *tab = new QWidget();
    QGridLayout *layout = new QGridLayout(tab);
    
    int row = 0;
    
    layout->addWidget(new QLabel("Mode:"), row, 0);
    dewHeaterModeLabel = new QLabel("-", tab);
    layout->addWidget(dewHeaterModeLabel, row++, 1);
    
    layout->addWidget(new QLabel("Aggression:"), row, 0);
    dewHeaterAggressionLabel = new QLabel("-", tab);
    layout->addWidget(dewHeaterAggressionLabel, row++, 1);
    
    layout->addWidget(new QLabel("Heater Level:"), row, 0);
    dewHeaterLevelLabel = new QLabel("-", tab);
    layout->addWidget(dewHeaterLevelLabel, row++, 1);
    
    layout->addWidget(new QLabel("Manual Power Level:"), row, 0);
    dewHeaterManualPowerLabel = new QLabel("-", tab);
    layout->addWidget(dewHeaterManualPowerLabel, row++, 1);
    
    layout->addWidget(new QLabel("Heater Level:"), row, 0);
    dewHeaterLevelBar = new QProgressBar(tab);
    dewHeaterLevelBar->setRange(0, 100);
    dewHeaterLevelBar->setValue(0);
    layout->addWidget(dewHeaterLevelBar, row++, 1);
    
    layout->addWidget(new QLabel("Last Update:"), row, 0);
    dewHeaterLastUpdateLabel = new QLabel("-", tab);
    layout->addWidget(dewHeaterLastUpdateLabel, row++, 1);
    
    layout->setRowStretch(row, 1);
    return tab;
}

void TelescopeGUI::setupDiscovery() {
    // UDP socket for discovery
    udpSocket = new QUdpSocket(this);
    
    // Start discovery automatically
    QTimer::singleShot(500, this, &TelescopeGUI::startDiscovery);
}

void TelescopeGUI::logJsonPacket(const QString &message, bool incoming) {
    // Create a timestamp
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    
    // Format the message
    QString direction = incoming ? "RECEIVED" : "SENT";
    
    // Create log line
    QString logLine = QString("[%1] %2: %3\n").arg(timestamp, direction, message);
    
    if (debug) qDebug() << logLine;
}

void TelescopeGUI::requestImage(const QString &filePath) {
    if (connectedIpAddress.isEmpty()) return;
    
    // Construct the proper URL path
    // The telescope is sending just the relative path like "Images/Temp/4.jpg"
    // We need to prepend the proper API path
    QString fullPath = QString("http://%1/SmartScope-1.0/dev2/%2").arg(connectedIpAddress, filePath);
    QUrl url(fullPath);
    QNetworkRequest request(url);
    
    // Set appropriate headers for the request
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Accept", "*/*");
    request.setRawHeader("User-Agent", "CelestronOriginMonitor Qt Application");
    request.setRawHeader("Connection", "keep-alive");
    
    if (false) qDebug() << "Requesting image from:" << fullPath;
    
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QNetworkReply *reply = manager->get(request);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager, filePath]() {
        if (reply->error() == QNetworkReply::NoError) {
            // Read the image data
            QByteArray imageData = reply->readAll();
            
            if (false) qDebug() << "Received image data, size:" << imageData.size() << "bytes";
            
            // Create a QPixmap from the data
            QPixmap pixmap;
            if (pixmap.loadFromData(imageData)) {
                // Scale to fit the label while preserving aspect ratio
                pixmap = pixmap.scaled(imagePreviewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                
                // Display the image
                imagePreviewLabel->setPixmap(pixmap);
                
                // Analyze image for focus quality (optional)
                analyzeImageForFocus(imageData);
            } else {
                qDebug() << "Failed to load image from data";
            }
        } else {
            qDebug() << "Error fetching image:" << reply->errorString();
        }
        
        // Clean up
        reply->deleteLater();
        manager->deleteLater();
    });
}

// Add a new method to analyze focus quality
void TelescopeGUI::analyzeImageForFocus(const QByteArray &imageData) {
    QImage image;
    if (!image.loadFromData(imageData)) {
        return;
    }
    
    // Convert to grayscale
    QImage grayImage = image.convertToFormat(QImage::Format_Grayscale8);
    
    // Calculate contrast as a simple measure of focus quality
    double totalVariance = 0.0;
    double totalPixels = grayImage.width() * grayImage.height();
    
    // Calculate mean pixel value
    double mean = 0.0;
    for (int y = 0; y < grayImage.height(); y++) {
        const uchar* line = grayImage.scanLine(y);
        for (int x = 0; x < grayImage.width(); x++) {
            mean += line[x];
        }
    }
    mean /= totalPixels;
    
    // Calculate variance
    for (int y = 0; y < grayImage.height(); y++) {
        const uchar* line = grayImage.scanLine(y);
        for (int x = 0; x < grayImage.width(); x++) {
            double diff = line[x] - mean;
            totalVariance += diff * diff;
        }
    }
    
    double contrastScore = sqrt(totalVariance / totalPixels);
    
    // Store this score somewhere (member variable or display in UI)
    //    qDebug() << "Focus quality score (contrast):" << contrastScore;
    
    // You could update a label in the UI to show this
    // focusQualityLabel->setText(QString("Focus Quality: %1").arg(contrastScore, 0, 'f', 2));
}

void TelescopeGUI::updateLastUpdateLabel(QLabel *label, const QDateTime &lastUpdate) {
    if (lastUpdate.isValid()) {
        QDateTime now = QDateTime::currentDateTime();
        qint64 secsAgo = lastUpdate.secsTo(now);
        
        if (secsAgo < 60) {
            label->setText(QString("%1 seconds ago").arg(secsAgo));
        } else if (secsAgo < 3600) {
            label->setText(QString("%1 minutes ago").arg(secsAgo / 60));
        } else {
            label->setText(QString("%1 hours ago").arg(secsAgo / 3600));
        }
    } else {
        label->setText("Never");
    }
}

QWidget* TelescopeGUI::createSlewAndImageTab() {
    QWidget *tab = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);
    
    // Target selection section (existing code)
    QGroupBox *targetGroup = new QGroupBox("Target Selection", tab);
    QGridLayout *targetLayout = new QGridLayout(targetGroup);
    
    // Built-in targets
    targetLayout->addWidget(new QLabel("Select Target:"), 0, 0);
    targetComboBox = new QComboBox(targetGroup);
    
    // Add some common targets with their J2000 coordinates
    targetComboBox->addItem("Custom Coordinates");
    QStringList objectNames = MessierCatalog::getObjectNames();
    for (const QString& name : objectNames) {
        targetComboBox->addItem(name);
    }

    targetLayout->addWidget(targetComboBox, 0, 1);
    
    // Custom coordinates for when "Custom Coordinates" is selected
    QGroupBox *customGroup = new QGroupBox("Custom Target", targetGroup);
    QGridLayout *customLayout = new QGridLayout(customGroup);
    
    customLayout->addWidget(new QLabel("Name:"), 0, 0);
    customNameEdit = new QLineEdit(customGroup);
    customNameEdit->setPlaceholderText("Enter target name");
    customLayout->addWidget(customNameEdit, 0, 1);
    
    customLayout->addWidget(new QLabel("RA (decimal hours):"), 1, 0);
    customRaEdit = new QLineEdit(customGroup);
    customRaEdit->setPlaceholderText("e.g. 12.934");
    customLayout->addWidget(customRaEdit, 1, 1);
    
    customLayout->addWidget(new QLabel("Dec (decimal degrees):"), 2, 0);
    customDecEdit = new QLineEdit(customGroup);
    customDecEdit->setPlaceholderText("e.g. 38.318");
    customLayout->addWidget(customDecEdit, 2, 1);
    
    // Connect the combo box selection change to enable/disable custom fields
    connect(targetComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            [this](int index) {
        bool isCustom = (index == 0);
        customNameEdit->setEnabled(isCustom);
        customRaEdit->setEnabled(isCustom);
        customDecEdit->setEnabled(isCustom);
    });
    
    // Initially disable custom fields if not on custom option
    bool isCustom = (targetComboBox->currentIndex() == 0);
    customNameEdit->setEnabled(isCustom);
    customRaEdit->setEnabled(isCustom);
    customDecEdit->setEnabled(isCustom);
    
    targetLayout->addWidget(customGroup, 1, 0, 1, 2);
    
    // Duration section
    QGroupBox *durationGroup = new QGroupBox("Imaging Duration", tab);
    QHBoxLayout *durationLayout = new QHBoxLayout(durationGroup);
    
    durationLayout->addWidget(new QLabel("Image for:"));
    durationSpinBox = new QSpinBox(durationGroup);
    durationSpinBox->setRange(1, 3600);  // 1 second to 1 hour
    durationSpinBox->setValue(300);      // Default 5 minutes
    durationSpinBox->setSuffix(" seconds");
    durationLayout->addWidget(durationSpinBox);
    
    // Control buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    
    startSlewButton = new QPushButton("Start Slew && Imaging", tab);
    connect(startSlewButton, &QPushButton::clicked, this, &TelescopeGUI::startSlewAndImage);
    buttonLayout->addWidget(startSlewButton);
    
    cancelSlewButton = new QPushButton("Cancel", tab);
    cancelSlewButton->setEnabled(false);
    connect(cancelSlewButton, &QPushButton::clicked, this, &TelescopeGUI::cancelSlewAndImage);
    buttonLayout->addWidget(cancelSlewButton);
    
    // Status display
    QGroupBox *statusGroup = new QGroupBox("Operation Status", tab);
    QVBoxLayout *statusGroupLayout = new QVBoxLayout(statusGroup);
    
    slewStatusLabel = new QLabel("Ready", statusGroup);
    statusGroupLayout->addWidget(slewStatusLabel);
    
    slewProgressBar = new QProgressBar(statusGroup);
    slewProgressBar->setRange(0, 100);
    slewProgressBar->setValue(0);
    statusGroupLayout->addWidget(slewProgressBar);
    
    // Add all components to main layout
    mainLayout->addWidget(targetGroup);
    mainLayout->addWidget(durationGroup);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(statusGroup);
    
    // Add a spacer to push everything up
    mainLayout->addStretch(1);
    
    // Create timers for the operation
    slewAndImageTimer = new QTimer(this);
    slewAndImageTimer->setSingleShot(false);
    slewAndImageTimer->setInterval(1000);  // 1 second updates
    connect(slewAndImageTimer, &QTimer::timeout, this, &TelescopeGUI::slewAndImageTimerTimeout);
    
    statusUpdateTimer = new QTimer(this);
    statusUpdateTimer->setSingleShot(false);
    statusUpdateTimer->setInterval(500);  // 0.5 second updates for status
    connect(statusUpdateTimer, &QTimer::timeout, this, &TelescopeGUI::updateSlewAndImageStatus);
    
    // Start periodic status checking
    QTimer *mountCheckTimer = new QTimer(this);
    mountCheckTimer->setSingleShot(false);
    mountCheckTimer->setInterval(2000);  // 2 second checks for mount status
    connect(mountCheckTimer, &QTimer::timeout, this, &TelescopeGUI::checkMountStatus);
    mountCheckTimer->start();
    
    return tab;
}

void TelescopeGUI::startSlewAndImage() {
  if (!originBackend->isConnected()) {
        QMessageBox::warning(this, "Not Connected", "Please connect to a telescope first");
        return;
    }
    
    if (isSlewingAndImaging) {
        return;
    }
    
    // Get the target information
    QString targetName;
    double ra = 0.0;
    double dec = 0.0;
    
    if (targetComboBox->currentIndex() == 0) {
        // Custom coordinates
        targetName = customNameEdit->text().trimmed();
        if (targetName.isEmpty()) {
            QMessageBox::warning(this, "Missing Target Name", "Please enter a target name");
            return;
        }
        
        bool raOk = false;
        bool decOk = false;
        ra = customRaEdit->text().toDouble(&raOk);
        dec = customDecEdit->text().toDouble(&decOk);
        
        if (!raOk || !decOk || ra < 0 || ra >= 24 || dec < -90 || dec > 90) {
            QMessageBox::warning(this, "Invalid Coordinates", 
                                "Please enter valid coordinates:\n- RA between 0 and 24 hours\n- Dec between -90 and +90 degrees");
            return;
        }
    } else {
        // Selected target
        int index = targetComboBox->currentIndex() - 1;
        auto objects = MessierCatalog::getAllObjects();
        auto currentObject = objects[index];
        ra = currentObject.sky_position.ra_deg / 15.0;
        dec = currentObject.sky_position.dec_deg;
        targetName = currentObject.name;
    }
    
    // Convert RA and Dec to radians as required by the telescope
    double raRadians = ra * M_PI / 12.0;  // 12 hours = π radians
    double decRadians = dec * M_PI / 180.0;  // 180 degrees = π radians
    
    qDebug() << "Slewing to target:" << targetName;
    qDebug() << "RA (hours):" << ra << "Dec (degrees):" << dec;
    qDebug() << "RA (radians):" << raRadians << "Dec (radians):" << decRadians;
    
    // Get the imaging duration
    int durationSeconds = durationSpinBox->value();
    imagingTimeRemaining = durationSeconds;
    
    // Update UI
    isSlewingAndImaging = true;
    startSlewButton->setEnabled(false);
    cancelSlewButton->setEnabled(true);
    targetComboBox->setEnabled(false);
    customNameEdit->setEnabled(false);
    customRaEdit->setEnabled(false);
    customDecEdit->setEnabled(false);
    durationSpinBox->setEnabled(false);
    
    // Set initial status
    slewStatusLabel->setText("Slewing to target...");
    slewProgressBar->setValue(0);
    
    // Start the status update timer
    statusUpdateTimer->start();
    
    // Generate a UUID for the imaging session
    QUuid uuid = QUuid::createUuid();
    currentImagingTargetUuid = uuid.toString(QUuid::WithoutBraces);
    
    // Send the GotoRaDec command
    QJsonObject gotoCommand;
    gotoCommand["Ra"] = raRadians;
    gotoCommand["Dec"] = decRadians;
    
    originBackend->sendCommand("GotoRaDec", "Mount", gotoCommand);
}

void TelescopeGUI::updateSlewAndImageStatus() {
    if (!isSlewingAndImaging) {
        return;
    }
    
    const TelescopeData &data = dataProcessor->getData();
    
    // Check if we're still slewing
    if (data.mount.isGotoOver == false) {
        slewStatusLabel->setText("Slewing to target...");
        // We don't know how far along the slew is, so just pulse the progress bar
        int currentValue = slewProgressBar->value();
        slewProgressBar->setValue((currentValue + 5) % 100);
        return;
    }
    
    // If we've completed the slew but haven't started imaging yet
    if (data.mount.isGotoOver && !slewAndImageTimer->isActive()) {
        // Start imaging
        slewStatusLabel->setText("Slew complete. Starting imaging...");
        slewProgressBar->setValue(0);
        
        // Send the RunImaging command
        QJsonObject runImagingCommand;
        runImagingCommand["Name"] = targetComboBox->currentIndex() == 0 ?
                                  customNameEdit->text() : 
                                  targetComboBox->currentText().split(" - ").at(0);
        runImagingCommand["SaveRawImage"] = true;
        runImagingCommand["Uuid"] = currentImagingTargetUuid;
        
        // Send the command
        originBackend->sendCommand("RunImaging", "TaskController", runImagingCommand);
        
        // Wait for a brief moment to ensure the command is processed
        QTimer::singleShot(500, this, [this]() {
            // Start the countdown timer
            slewAndImageTimer->start();
        });
    }
    
    // If the imaging timer is running, update the progress
    if (slewAndImageTimer->isActive()) {
        // Calculate progress as a percentage
        int durationSeconds = durationSpinBox->value();
        int progress = 100 - ((imagingTimeRemaining * 100) / durationSeconds);
        
        slewStatusLabel->setText(QString("Imaging in progress: %1 seconds remaining").arg(imagingTimeRemaining));
        slewProgressBar->setValue(progress);
    }
}

void TelescopeGUI::slewAndImageTimerTimeout() {
    imagingTimeRemaining--;
    
    if (imagingTimeRemaining <= 0) {
        // Time's up, stop imaging
        cancelSlewAndImage();
    }
}

void TelescopeGUI::cancelSlewAndImage() {
    if (!isSlewingAndImaging) {
        return;
    }
    
    // Stop timers
    statusUpdateTimer->stop();
    slewAndImageTimer->stop();
    
    // Cancel any ongoing slew
    originBackend->sendCommand("AbortAxisMovement", "Mount");
    
    // Cancel any imaging
    QJsonObject cancelImagingCommand;
    originBackend->sendCommand("CancelImaging", "MountTaskController");
    
    // Reset UI
    isSlewingAndImaging = false;
    startSlewButton->setEnabled(true);
    cancelSlewButton->setEnabled(false);
    targetComboBox->setEnabled(true);
    
    // Re-enable custom fields if appropriate
    bool isCustom = (targetComboBox->currentIndex() == 0);
    customNameEdit->setEnabled(isCustom);
    customRaEdit->setEnabled(isCustom);
    customDecEdit->setEnabled(isCustom);
    
    durationSpinBox->setEnabled(true);
    
    slewStatusLabel->setText("Imaging cancelled");
    slewProgressBar->setValue(0);
}

void TelescopeGUI::initializeTelescope() {
  if (!originBackend->isConnected()) {
        QMessageBox::warning(this, "Not Connected", "Please connect to a telescope first");
        return;
    }
    
    // Use the exact initialization command from init.txt
    QJsonObject runInitCommand;
    // Get current date and time - you can also use fixed values from init.txt
    QDateTime now = QDateTime::currentDateTime();
    QString dateStr = now.toString("dd MM yyyy");
    QString timeStr = now.toString("HH:mm:ss");
    
    // Use location values from init.txt - Cambridge, UK coordinates
    runInitCommand["Date"] = dateStr; // Or use dateStr for current date
    runInitCommand["FakeInitialize"] = false;
    runInitCommand["Latitude"] = 0.9118493267600084;  // In radians (Cambridge, UK)
    runInitCommand["Longitude"] = 0.0013880067713051129;  // In radians
    runInitCommand["Time"] = timeStr; // Or use timeStr for current time
    runInitCommand["TimeZone"] = "Europe/London";
    
    originBackend->sendCommand("RunInitialize", "TaskController", runInitCommand);
    
    slewStatusLabel->setText("Initializing telescope...");
    initializeButton->setEnabled(false);
}

void TelescopeGUI::startTelescopeAlignment() {
  if (!originBackend->isConnected()) {
        QMessageBox::warning(this, "Not Connected", "Please connect to a telescope first");
        return;
    }
    
    // Send the StartAlignment command
    QJsonObject alignCommand;
    originBackend->sendCommand("StartAlignment", "Mount", alignCommand);
    
    slewStatusLabel->setText("Starting alignment procedure...");
    
    // Note: In a real implementation, we would need to guide the user through
    // the complete alignment procedure, which includes manually centering stars
    // and adding alignment points. This is just a simplified version.
}

void TelescopeGUI::checkMountStatus() {
  if (!originBackend->isConnected()) {
        alignmentStatusLabel->setText("Not connected");
        mountStatusLabel->setText("Not connected");
        return;
    }
    
    // Get the current status from the data processor
    const TelescopeData &data = dataProcessor->getData();
    
    // Update the alignment status
    alignmentStatusLabel->setText(data.mount.isAligned ? "Aligned" : "Not Aligned");
    
    // Update the mount status
    QString mountStatus;
    if (data.mount.isGotoOver && !data.mount.isTracking) {
        mountStatus = "Ready (Idle)";
    } else if (!data.mount.isGotoOver) {
        mountStatus = "Slewing";
    } else if (data.mount.isTracking) {
        mountStatus = "Tracking";
    } else {
        mountStatus = "Unknown";
    }
    
    mountStatusLabel->setText(mountStatus);
    
    // Enable/disable slewing based on status
    bool canSlew = data.mount.isAligned && data.mount.isGotoOver;
    startSlewButton->setEnabled(!isSlewingAndImaging);
}

static const int SLEW_RATE = 9;

void TelescopeGUI::slew(int alt, int az)
{
QJsonObject slewCommand;
    slewCommand["AltRate"] = alt;  // Positive for up
    slewCommand["AzmRate"] = az;
    originBackend->sendCommand("Slew", "Mount", slewCommand);
    qDebug() << "Slew: " << alt << az;
}

void TelescopeGUI::cancelSlew()
{
  slewTimer->stop();
  slewTimer->start();
}

void TelescopeGUI::onSlewCancel()
{
  slew(0, 0);
}

void TelescopeGUI::startUpButton()
{
    qDebug() << "Up Button - Starting upward slew";
    slew(SLEW_RATE, 0);
    cancelSlew();
}

void TelescopeGUI::startDownButton()
{
    qDebug() << "Down Button - Starting downward slew";
    
    slew(-SLEW_RATE, 0);  // Negative for down
    cancelSlew();
}

void TelescopeGUI::startLeftButton()
{
    qDebug() << "Left Button - Starting leftward slew";
    
    slew(0, -SLEW_RATE);  // Negative for left (counterclockwise)
    cancelSlew();

}

void TelescopeGUI::startRightButton()
{
    qDebug() << "Right Button - Starting rightward slew";
    
    slew(0, SLEW_RATE);  // Positive for right (clockwise)
    cancelSlew();
}

void TelescopeGUI::onTrackingError(const QString& error)
{
    QMessageBox::warning(this, "Tracking Error", error);
}

void TelescopeGUI::showLogReplay()
{
    if (!m_logReplayDialog) {
        m_logReplayDialog = new LogReplayDialog(dataProcessor, this);
        
        // Connect signals to update main GUI as replay progresses
        connect(m_logReplayDialog, &LogReplayDialog::messageProcessed,
                this, [this](const LogEntry& entry, bool success) {
            if (success) {
                // Update displays as if receiving real data
                updateMountDisplay();
                updateCameraDisplay();
                updateFocuserDisplay();
                updateEnvironmentDisplay();
                updateImageDisplay();
                updateDiskDisplay();
                updateDewHeaterDisplay();
            }
        });
    }
    
    m_logReplayDialog->show();
    m_logReplayDialog->raise();
    m_logReplayDialog->activateWindow();
}
