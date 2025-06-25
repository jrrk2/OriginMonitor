#include "TelescopeGUI.hpp"
#include "CommandInterface.hpp"
#include "AlpacaServer.hpp"
#include "OriginBackend.hpp"
#include <cmath>

TelescopeGUI::TelescopeGUI(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Celestron Origin Monitor");
    resize(900, 700);
    
    dataProcessor = new TelescopeDataProcessor(this);
    
    // Connect signals from data processor
    connect(dataProcessor, &TelescopeDataProcessor::mountStatusUpdated, this, &TelescopeGUI::updateMountDisplay);
    connect(dataProcessor, &TelescopeDataProcessor::cameraStatusUpdated, this, &TelescopeGUI::updateCameraDisplay);
    connect(dataProcessor, &TelescopeDataProcessor::focuserStatusUpdated, this, &TelescopeGUI::updateFocuserDisplay);
    connect(dataProcessor, &TelescopeDataProcessor::environmentStatusUpdated, this, &TelescopeGUI::updateEnvironmentDisplay);
    connect(dataProcessor, &TelescopeDataProcessor::newImageAvailable, this, &TelescopeGUI::updateImageDisplay);
    connect(dataProcessor, &TelescopeDataProcessor::diskStatusUpdated, this, &TelescopeGUI::updateDiskDisplay);
    connect(dataProcessor, &TelescopeDataProcessor::dewHeaterStatusUpdated, this, &TelescopeGUI::updateDewHeaterDisplay);
    connect(dataProcessor, &TelescopeDataProcessor::orientationStatusUpdated, this, &TelescopeGUI::updateOrientationDisplay);

    // NEW: Initialize Alpaca components
    originBackend = new OriginBackend(this);
    alpacaServer = new AlpacaServer(this);
    alpacaServer->setTelescopeBackend(originBackend);
    
    // Connect Alpaca server signals
    connect(alpacaServer, &AlpacaServer::serverStarted, this, &TelescopeGUI::onAlpacaServerStarted);
    connect(alpacaServer, &AlpacaServer::serverStopped, this, &TelescopeGUI::onAlpacaServerStopped);
    connect(alpacaServer, &AlpacaServer::requestReceived, this, &TelescopeGUI::onAlpacaRequestReceived);
    
    // Connect Origin backend signals
    connect(originBackend, &OriginBackend::connected, this, [this]() {
        alpacaLogTextEdit->append(QString("[%1] Origin telescope connected")
                                 .arg(QTime::currentTime().toString()));
    });
    
    connect(originBackend, &OriginBackend::disconnected, this, [this]() {
        alpacaLogTextEdit->append(QString("[%1] Origin telescope disconnected")
                                 .arg(QTime::currentTime().toString()));
    });
    
    setupUI();
    setupWebSocket();
    setupDiscovery();
    
    // Update time display every second
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TelescopeGUI::updateTimeDisplay);
    timer->start(1000);
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
        qDebug() << "Received UDP broadcast from" << sender.toString() << ":" << datagramStr;
        
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
                    
                    statusLabel->setText(QString("Found Celestron Origin telescope at %1").arg(telescopeIP));
                }
            }
        }
    }
}

void TelescopeGUI::connectToSelectedTelescope() {
    if (isConnected) {
        // Disconnect if already connected
        webSocket->close();
        return;
    }
    
    QListWidgetItem *selectedItem = telescopeListWidget->currentItem();
    if (!selectedItem) {
        statusLabel->setText("Please select a telescope from the list");
        return;
    }
    
    // Extract the IP address from the selected item
    QString text = selectedItem->text();
    QRegularExpression ipRegex("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b");
    QRegularExpressionMatch match = ipRegex.match(text);
    
    if (!match.hasMatch()) {
        statusLabel->setText("Could not find IP address in selected item");
        return;
    }
    
    QString ipAddress = match.captured(0);
    statusLabel->setText(QString("Connecting to telescope at %1...").arg(ipAddress));
    
    // Connect to the telescope via WebSocket using the correct endpoint
    QString url = QString("ws://%1:80/SmartScope-1.0/mountControlEndpoint").arg(ipAddress);
    webSocket->open(QUrl(url));
    
    // Store the currently connected IP
    connectedIpAddress = ipAddress;
}

void TelescopeGUI::onWebSocketConnected() {
    statusLabel->setText("Connected to telescope!");
    connectButton->setText("Disconnect");
    isConnected = true;

    // Send a status request to get basic info
    QJsonObject command;
    command["Command"] = "GetStatus";
    command["Destination"] = "System";
    command["SequenceID"] = 1;
    command["Source"] = "QtApp";
    command["Type"] = "Command";
    
    sendJsonMessage(command);
}

void TelescopeGUI::onWebSocketDisconnected() {
    statusLabel->setText("Disconnected from telescope");
    connectButton->setText("Connect");
    isConnected = false;
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
    mountTimeLabel->setText(data.mount.time);
    mountDateLabel->setText(data.mount.date);
    mountTimeZoneLabel->setText(data.mount.timeZone);
    
    mountLatitudeLabel->setText(QString::number(data.mount.latitude * 180.0 / M_PI, 'f', 1) + "° +/- 0.05");
    mountLongitudeLabel->setText(QString::number(data.mount.longitude * 180.0 / M_PI, 'f', 1) + "° +/- 0.05");
    
    mountIsAlignedLabel->setText(data.mount.isAligned ? "Yes" : "No");
    mountIsTrackingLabel->setText(data.mount.isTracking ? "Yes" : "No");
    mountIsGotoOverLabel->setText(data.mount.isGotoOver ? "Yes" : "No");
    mountNumAlignRefsLabel->setText(QString::number(data.mount.numAlignRefs));
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
    imageOrientationLabel->setText(QString::number(data.lastImage.orientation * 180.0 / M_PI, 'f', 2) + "°");
    imageFovXLabel->setText(QString::number(data.lastImage.fovX * 180.0 / M_PI, 'f', 4) + "°");
    imageFovYLabel->setText(QString::number(data.lastImage.fovY * 180.0 / M_PI, 'f', 4) + "°");
    
    // Request image from the telescope if connected
    if (isConnected && !data.lastImage.fileLocation.isEmpty()) {
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

void TelescopeGUI::updateOrientationDisplay() {
    const TelescopeData &data = dataProcessor->getData();
    orientationAltitudeLabel->setText(QString::number(data.orientation.altitude) + "°");
}

void TelescopeGUI::updateTimeDisplay() {
    QDateTime now = QDateTime::currentDateTime();
    
    // Update connection status time
    if (isConnected) {
        const TelescopeData &data = dataProcessor->getData();
        
        // Calculate time since last update for each component
        updateLastUpdateLabel(mountLastUpdateLabel, data.mountLastUpdate);
        updateLastUpdateLabel(cameraLastUpdateLabel, data.cameraLastUpdate);
        updateLastUpdateLabel(focuserLastUpdateLabel, data.focuserLastUpdate);
        updateLastUpdateLabel(environmentLastUpdateLabel, data.environmentLastUpdate);
        updateLastUpdateLabel(imageLastUpdateLabel, data.imageLastUpdate);
        updateLastUpdateLabel(diskLastUpdateLabel, data.diskLastUpdate);
        updateLastUpdateLabel(dewHeaterLastUpdateLabel, data.dewHeaterLastUpdate);
        updateLastUpdateLabel(orientationLastUpdateLabel, data.orientationLastUpdate);
    }
}

void TelescopeGUI::setupUI() {
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    
    // Discovery and connection panel
    QGroupBox *discoveryBox = new QGroupBox("Telescope Discovery and Connection", centralWidget);
    QVBoxLayout *discoveryLayout = new QVBoxLayout(discoveryBox);
    
    // Status and controls
    QHBoxLayout *controlLayout = new QHBoxLayout();
    
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
    discoveryLayout->addWidget(telescopeListWidget);
    
    mainLayout->addWidget(discoveryBox);
    
    // Tab widget for different categories
    QTabWidget *tabWidget = new QTabWidget(centralWidget);
    
    // Create tabs
    tabWidget->addTab(createMountTab(), "Mount");
    tabWidget->addTab(createCameraTab(), "Camera");
    tabWidget->addTab(createFocuserTab(), "Focuser");
    tabWidget->addTab(createEnvironmentTab(), "Environment");
    tabWidget->addTab(createImageTab(), "Image");
    tabWidget->addTab(createDiskTab(), "Disk");
    tabWidget->addTab(createDewHeaterTab(), "Dew Heater");
    tabWidget->addTab(createOrientationTab(), "Orientation");
    tabWidget->addTab(createCommandTab(), "Commands");
    tabWidget->addTab(createSlewAndImageTab(), "Slew && Image");   
    tabWidget->addTab(createDownloadTab(), "Auto Download");
    tabWidget->addTab(createAlpacaTab(), "Alpaca Server");  // NEW TAB
    
    mainLayout->addWidget(tabWidget);
}

QWidget* TelescopeGUI::createMountTab() {
    QWidget *tab = new QWidget();
    QGridLayout *layout = new QGridLayout(tab);
    
    int row = 0;
    
    // Add labels for mount data
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
    
    layout->addWidget(new QLabel("Last Update:"), row, 0);
    mountLastUpdateLabel = new QLabel("-", tab);
    layout->addWidget(mountLastUpdateLabel, row++, 1);
    
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

QWidget* TelescopeGUI::createImageTab() {
    QWidget *tab = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);
    
    // Create two panels - left for info, right for image
    QSplitter *splitter = new QSplitter(Qt::Horizontal, tab);
    mainLayout->addWidget(splitter);
    
    // Left panel - info
    QWidget *infoPanel = new QWidget(splitter);
    QGridLayout *infoLayout = new QGridLayout(infoPanel);
    
    int row = 0;
    
    infoLayout->addWidget(new QLabel("File Location:"), row, 0);
    imageFileLabel = new QLabel("-", infoPanel);
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
    
    infoLayout->addWidget(new QLabel("Orientation:"), row, 0);
    imageOrientationLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageOrientationLabel, row++, 1);
    
    infoLayout->addWidget(new QLabel("Field of View X:"), row, 0);
    imageFovXLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageFovXLabel, row++, 1);
    
    infoLayout->addWidget(new QLabel("Field of View Y:"), row, 0);
    imageFovYLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageFovYLabel, row++, 1);
    
    infoLayout->addWidget(new QLabel("Last Update:"), row, 0);
    imageLastUpdateLabel = new QLabel("-", infoPanel);
    infoLayout->addWidget(imageLastUpdateLabel, row++, 1);
    
    // Add vertical space at the bottom
    infoLayout->setRowStretch(row, 1);
    
    // Right panel - image preview
    QWidget *imagePanel = new QWidget(splitter);
    QVBoxLayout *imageLayout = new QVBoxLayout(imagePanel);
    
    imagePreviewLabel = new QLabel(imagePanel);
    imagePreviewLabel->setMinimumSize(400, 300);
    imagePreviewLabel->setAlignment(Qt::AlignCenter);
    imagePreviewLabel->setScaledContents(false);
    imagePreviewLabel->setText("No image available");
    
    imageLayout->addWidget(imagePreviewLabel);
    
    // Set stretch factors for the splitter
    splitter->setStretchFactor(0, 1);  // Info panel
    splitter->setStretchFactor(1, 3);  // Image panel
    
    return tab;
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

QWidget* TelescopeGUI::createOrientationTab() {
    QWidget *tab = new QWidget();
    QGridLayout *layout = new QGridLayout(tab);
    
    int row = 0;
    
    layout->addWidget(new QLabel("Altitude:"), row, 0);
    orientationAltitudeLabel = new QLabel("-", tab);
    layout->addWidget(orientationAltitudeLabel, row++, 1);
    
    layout->addWidget(new QLabel("Last Update:"), row, 0);
    orientationLastUpdateLabel = new QLabel("-", tab);
    layout->addWidget(orientationLastUpdateLabel, row++, 1);
    
    layout->setRowStretch(row, 1);
    return tab;
}

QWidget* TelescopeGUI::createCommandTab() {
    CommandInterface *commandInterface = new CommandInterface(this, this);
    return commandInterface;
}

void TelescopeGUI::setupWebSocket() {
    webSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
    
    connect(webSocket, &QWebSocket::connected, this, &TelescopeGUI::onWebSocketConnected);
    connect(webSocket, &QWebSocket::disconnected, this, &TelescopeGUI::onWebSocketDisconnected);
    connect(webSocket, &QWebSocket::textMessageReceived, this, &TelescopeGUI::onTextMessageReceived);
}

void TelescopeGUI::setupDiscovery() {
    // UDP socket for discovery
    udpSocket = new QUdpSocket(this);
    
    // Start discovery automatically
    QTimer::singleShot(500, this, &TelescopeGUI::startDiscovery);
}

void TelescopeGUI::sendJsonMessage(const QJsonObject &obj) {
    QJsonDocument doc(obj);
    QString message = doc.toJson();
    
    // Log the outgoing message
    logJsonPacket(message, false);
    
    // Send via WebSocket
    if (webSocket->isValid() && webSocket->state() == QAbstractSocket::ConnectedState) {
        webSocket->sendTextMessage(message);
    }
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
    
    qDebug() << "Requesting image from:" << fullPath;
    
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QNetworkReply *reply = manager->get(request);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager, filePath]() {
        if (reply->error() == QNetworkReply::NoError) {
            // Read the image data
            QByteArray imageData = reply->readAll();
            
            qDebug() << "Received image data, size:" << imageData.size() << "bytes";
            
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
    qDebug() << "Focus quality score (contrast):" << contrastScore;
    
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


// Add the implementation of createDownloadTab()
QWidget* TelescopeGUI::createDownloadTab() {
    QWidget *tab = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);
    
    // Download path selection
    QGroupBox *pathGroup = new QGroupBox("Download Path", tab);
    QHBoxLayout *pathLayout = new QHBoxLayout(pathGroup);
    
    downloadPathEdit = new QLineEdit(QDir::homePath() + "/CelestronOriginDownloads", pathGroup);
    pathLayout->addWidget(downloadPathEdit);
    
    browseButton = new QPushButton("Browse", pathGroup);
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, "Select Download Directory",
                                                      downloadPathEdit->text(),
                                                      QFileDialog::ShowDirsOnly |
                                                      QFileDialog::DontResolveSymlinks);
        if (!dir.isEmpty()) {
            downloadPathEdit->setText(dir);
        }
    });
    pathLayout->addWidget(browseButton);
    
    mainLayout->addWidget(pathGroup);
    
    // Control buttons
    QHBoxLayout *controlLayout = new QHBoxLayout();
    
    startDownloadButton = new QPushButton("Start Automatic Download", tab);
    connect(startDownloadButton, &QPushButton::clicked, this, &TelescopeGUI::startAutomaticDownload);
    controlLayout->addWidget(startDownloadButton);
    
    stopDownloadButton = new QPushButton("Stop Download", tab);
    stopDownloadButton->setEnabled(false);
    connect(stopDownloadButton, &QPushButton::clicked, this, &TelescopeGUI::stopAutomaticDownload);
    controlLayout->addWidget(stopDownloadButton);
    
    mainLayout->addLayout(controlLayout);
    
    // Progress display
    QGroupBox *progressGroup = new QGroupBox("Download Progress", tab);
    QVBoxLayout *progressLayout = new QVBoxLayout(progressGroup);
    
    QLabel *overallLabel = new QLabel("Overall Progress:", progressGroup);
    progressLayout->addWidget(overallLabel);
    
    overallProgressBar = new QProgressBar(progressGroup);
    overallProgressBar->setRange(0, 100);
    overallProgressBar->setValue(0);
    progressLayout->addWidget(overallProgressBar);
    
    currentFileLabel = new QLabel("Current File:", progressGroup);
    progressLayout->addWidget(currentFileLabel);
    
    currentFileProgressBar = new QProgressBar(progressGroup);
    currentFileProgressBar->setRange(0, 100);
    currentFileProgressBar->setValue(0);
    progressLayout->addWidget(currentFileProgressBar);
    
    mainLayout->addWidget(progressGroup);
    
    // Download log
    QGroupBox *logGroup = new QGroupBox("Download Log", tab);
    QVBoxLayout *logLayout = new QVBoxLayout(logGroup);
    
    downloadLogList = new QListWidget(logGroup);
    logLayout->addWidget(downloadLogList);
    
    mainLayout->addWidget(logGroup);
    
    return tab;
}

// Add the implementation of startAutomaticDownload()
void TelescopeGUI::startAutomaticDownload() {
    if (!isConnected) {
        QMessageBox::warning(this, "Not Connected", "Please connect to a telescope first");
        return;
    }
    
    if (isDownloading) {
        return;
    }
    
    QString downloadPath = downloadPathEdit->text();
    
    // Create directory if it doesn't exist
    QDir dir(downloadPath);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            QMessageBox::warning(this, "Error", "Failed to create download directory");
            return;
        }
    }
    
    // Create auto downloader if needed
    if (!autoDownloader) {
        autoDownloader = new AutoDownloader(webSocket, connectedIpAddress, downloadPath, this);
        
        // Connect signals
        connect(autoDownloader, &AutoDownloader::directoryDownloadStarted, 
                this, &TelescopeGUI::onDirectoryDownloadStarted);
        connect(autoDownloader, &AutoDownloader::fileDownloadStarted, 
                this, &TelescopeGUI::onFileDownloadStarted);
        connect(autoDownloader, &AutoDownloader::fileDownloaded, 
                this, &TelescopeGUI::onFileDownloaded);
        connect(autoDownloader, &AutoDownloader::directoryDownloaded, 
                this, &TelescopeGUI::onDirectoryDownloaded);
        connect(autoDownloader, &AutoDownloader::allDownloadsComplete, 
                this, &TelescopeGUI::onAllDownloadsComplete);
        connect(autoDownloader, &AutoDownloader::downloadProgress, 
                this, &TelescopeGUI::updateDownloadProgress);
    } else {
        // Update download path
        autoDownloader->setDownloadPath(downloadPath);
    }
    
    // Start download
    isDownloading = true;
    startDownloadButton->setEnabled(false);
    stopDownloadButton->setEnabled(true);
    browseButton->setEnabled(false);
    downloadPathEdit->setEnabled(false);
    
    // Reset progress indicators
    overallProgressBar->setValue(0);
    currentFileProgressBar->setValue(0);
    currentFileLabel->setText("Current File: Initializing...");
    
    // Clear log
    downloadLogList->clear();
    
    // Add start entry to log
    QListWidgetItem *item = new QListWidgetItem(QString("Starting automatic download to %1").arg(downloadPath));
    downloadLogList->addItem(item);
    downloadLogList->scrollToBottom();
    
    // Start the download
    autoDownloader->startDownload();
}

// Add the implementation of stopAutomaticDownload()
void TelescopeGUI::stopAutomaticDownload() {
    if (!isDownloading || !autoDownloader) {
        return;
    }
    
    // Stop the download
    autoDownloader->stopDownload();
    
    // Update UI
    isDownloading = false;
    startDownloadButton->setEnabled(true);
    stopDownloadButton->setEnabled(false);
    browseButton->setEnabled(true);
    downloadPathEdit->setEnabled(true);
    
    // Add stop entry to log
    QListWidgetItem *item = new QListWidgetItem("Download stopped by user");
    downloadLogList->addItem(item);
    downloadLogList->scrollToBottom();
}

// Add the implementation of updateDownloadProgress()
void TelescopeGUI::updateDownloadProgress(const QString &currentFile, int filesCompleted, 
                                        int totalFiles, qint64 bytesReceived, qint64 bytesTotal) {
    // Update current file progress
    if (bytesTotal > 0) {
        int percent = (int)((bytesReceived * 100) / bytesTotal);
        currentFileProgressBar->setValue(percent);
    }
    
    // Update current file label
    currentFileLabel->setText(QString("Current File: %1").arg(currentFile));
    
    // Update overall progress
    if (totalFiles > 0) {
        int percent = (int)((filesCompleted * 100) / totalFiles);
        overallProgressBar->setValue(percent);
    }
}

// Add the implementation of event handlers
void TelescopeGUI::onDirectoryDownloadStarted(const QString &directory) {
    QListWidgetItem *item = new QListWidgetItem(QString("Starting download of directory: %1").arg(directory));
    downloadLogList->addItem(item);
    downloadLogList->scrollToBottom();
}

void TelescopeGUI::onFileDownloadStarted(const QString &fileName) {
    QListWidgetItem *item = new QListWidgetItem(QString("Downloading: %1").arg(fileName));
    downloadLogList->addItem(item);
    downloadLogList->scrollToBottom();
}

void TelescopeGUI::onFileDownloaded(const QString &fileName, bool success) {
    QString status = success ? "Success" : "Failed";
    QListWidgetItem *item = new QListWidgetItem(QString("Download %1: %2").arg(status, fileName));
    downloadLogList->addItem(item);
    downloadLogList->scrollToBottom();
}

void TelescopeGUI::onDirectoryDownloaded(const QString &directory) {
    QListWidgetItem *item = new QListWidgetItem(QString("Completed download of directory: %1").arg(directory));
    downloadLogList->addItem(item);
    downloadLogList->scrollToBottom();
}

void TelescopeGUI::onAllDownloadsComplete() {
    QListWidgetItem *item = new QListWidgetItem("All downloads complete!");
    downloadLogList->addItem(item);
    downloadLogList->scrollToBottom();
    
    // Update UI
    isDownloading = false;
    startDownloadButton->setEnabled(true);
    stopDownloadButton->setEnabled(false);
    browseButton->setEnabled(true);
    downloadPathEdit->setEnabled(true);
}

QWidget* TelescopeGUI::createSlewAndImageTab() {
    QWidget *tab = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);
    
    // Initialize section
    QGroupBox *initGroup = new QGroupBox("Telescope Initialization", tab);
    QVBoxLayout *initLayout = new QVBoxLayout(initGroup);
    
    // Status display
    QHBoxLayout *statusLayout = new QHBoxLayout();
    statusLayout->addWidget(new QLabel("Alignment Status:"));
    alignmentStatusLabel = new QLabel("Unknown", initGroup);
    statusLayout->addWidget(alignmentStatusLabel);
    
    statusLayout->addWidget(new QLabel("Mount Status:"));
    mountStatusLabel = new QLabel("Unknown", initGroup);
    statusLayout->addWidget(mountStatusLabel);
    
    initLayout->addLayout(statusLayout);
    
    // Initialization button
    QHBoxLayout *initButtonLayout = new QHBoxLayout();
    initializeButton = new QPushButton("Initialize Telescope", initGroup);
    connect(initializeButton, &QPushButton::clicked, this, &TelescopeGUI::initializeTelescope);
    initButtonLayout->addWidget(initializeButton);
    
    // Auto align button (if needed later)
    autoAlignButton = new QPushButton("Start Alignment (if needed)", initGroup);
    connect(autoAlignButton, &QPushButton::clicked, this, &TelescopeGUI::startTelescopeAlignment);
    autoAlignButton->setEnabled(false); // Disabled initially
    initButtonLayout->addWidget(autoAlignButton);
    
    initLayout->addLayout(initButtonLayout);
    
    mainLayout->addWidget(initGroup);
    
    // Target selection section (existing code)
    QGroupBox *targetGroup = new QGroupBox("Target Selection", tab);
    QGridLayout *targetLayout = new QGridLayout(targetGroup);
    
    // Built-in targets
    targetLayout->addWidget(new QLabel("Select Target:"), 0, 0);
    targetComboBox = new QComboBox(targetGroup);
    
    // Add some common targets with their J2000 coordinates
    targetComboBox->addItem("Custom Coordinates");
    targetComboBox->addItem("Cor Caroli - α CVn", "12h56m01.67s +38°19'06.2\"");
    targetComboBox->addItem("Mizar - ζ UMa", "13h23m55.5s +54°55'31\"");
    targetComboBox->addItem("Vega - α Lyr", "18h36m56.3s +38°47'01\"");
    targetComboBox->addItem("Deneb - α Cyg", "20h41m25.9s +45°16'49\"");
    targetComboBox->addItem("Altair - α Aql", "19h50m47.0s +08°52'06\"");
    targetComboBox->addItem("Polaris - α UMi", "02h31m49.1s +89°15'51\"");
    targetComboBox->addItem("M31 - Andromeda Galaxy", "00h42m44.3s +41°16'09\"");
    targetComboBox->addItem("M42 - Orion Nebula", "05h35m17.3s -05°23'28\"");
    targetComboBox->addItem("M45 - Pleiades", "03h47m24.0s +24°07'00\"");
    targetComboBox->addItem("M51 - Whirlpool Galaxy", "13h29m52.7s +47°11'43\"");
    targetComboBox->addItem("Virgo - Supercluster", "12h24m36.0s +8°0'00\"");
    targetComboBox->addItem("Virgo - Galaxy1", "12h24m12.0s +7°57'07\"");
    
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


// Add this method to TelescopeGUI.cpp:
QWidget* TelescopeGUI::createAlpacaTab()
{
    QWidget *tab = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);
    
    // Server Control Section
    QGroupBox *controlGroup = new QGroupBox("Alpaca Server Control", tab);
    QGridLayout *controlLayout = new QGridLayout(controlGroup);
    
    // Port configuration
    controlLayout->addWidget(new QLabel("Port:"), 0, 0);
    alpacaPortSpinBox = new QSpinBox(controlGroup);
    alpacaPortSpinBox->setRange(1024, 65535);
    alpacaPortSpinBox->setValue(11111); // Default Alpaca port
    controlLayout->addWidget(alpacaPortSpinBox, 0, 1);
    
    // Server name
    controlLayout->addWidget(new QLabel("Server Name:"), 1, 0);
    alpacaServerNameEdit = new QLineEdit("Celestron Origin Alpaca Server", controlGroup);
    controlLayout->addWidget(alpacaServerNameEdit, 1, 1);
    
    // Auto-start option
    alpacaAutoStartCheckBox = new QCheckBox("Auto-start server on application launch", controlGroup);
    controlLayout->addWidget(alpacaAutoStartCheckBox, 2, 0, 1, 2);
    
    // Discovery broadcast option
    alpacaDiscoveryCheckBox = new QCheckBox("Enable discovery broadcasts", controlGroup);
    alpacaDiscoveryCheckBox->setChecked(true);
    controlLayout->addWidget(alpacaDiscoveryCheckBox, 3, 0, 1, 2);
    
    // Control buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    alpacaStartButton = new QPushButton("Start Server", controlGroup);
    alpacaStopButton = new QPushButton("Stop Server", controlGroup);
    alpacaStopButton->setEnabled(false);
    
    connect(alpacaStartButton, &QPushButton::clicked, this, &TelescopeGUI::startAlpacaServer);
    connect(alpacaStopButton, &QPushButton::clicked, this, &TelescopeGUI::stopAlpacaServer);
    
    buttonLayout->addWidget(alpacaStartButton);
    buttonLayout->addWidget(alpacaStopButton);
    buttonLayout->addStretch();
    
    controlLayout->addLayout(buttonLayout, 4, 0, 1, 2);
    
    mainLayout->addWidget(controlGroup);
    
    // Status Section
    QGroupBox *statusGroup = new QGroupBox("Server Status", tab);
    QGridLayout *statusLayout = new QGridLayout(statusGroup);
    
    statusLayout->addWidget(new QLabel("Status:"), 0, 0);
    alpacaStatusLabel = new QLabel("Stopped", statusGroup);
    alpacaStatusLabel->setStyleSheet("color: red;");
    statusLayout->addWidget(alpacaStatusLabel, 0, 1);
    
    statusLayout->addWidget(new QLabel("Port:"), 1, 0);
    alpacaPortLabel = new QLabel("N/A", statusGroup);
    statusLayout->addWidget(alpacaPortLabel, 1, 1);
    
    statusLayout->addWidget(new QLabel("Requests:"), 2, 0);
    alpacaRequestCountLabel = new QLabel("0", statusGroup);
    statusLayout->addWidget(alpacaRequestCountLabel, 2, 1);
    
    mainLayout->addWidget(statusGroup);
    
    // Connection Info Section
    QGroupBox *connectionGroup = new QGroupBox("Connection Information", tab);
    QVBoxLayout *connectionLayout = new QVBoxLayout(connectionGroup);
    
    QLabel *infoLabel = new QLabel(connectionGroup);
    infoLabel->setText(
        "<b>Alpaca API Endpoints:</b><br>"
        "• Telescope: http://localhost:11111/api/v1/telescope/0/<br>"
        "• Camera: http://localhost:11111/api/v1/camera/0/<br>"
        "• Management: http://localhost:11111/management/v1/<br><br>"
        
        "<b>Compatible Software:</b><br>"
        "• ASCOM Alpaca clients via bridge<br>"
        "• SkySafari mobile app<br>"
        "• Custom scripts using HTTP API<br>"
        "• Web-based control interfaces<br><br>"
        
        "<b>Discovery:</b><br>"
        "• Broadcasts on UDP port 32227<br>"
        "• Compatible clients can auto-discover"
    );
    infoLabel->setWordWrap(true);
    connectionLayout->addWidget(infoLabel);
    
    mainLayout->addWidget(connectionGroup);
    
    // Request Log Section
    QGroupBox *logGroup = new QGroupBox("Request Log", tab);
    QVBoxLayout *logLayout = new QVBoxLayout(logGroup);
    
    alpacaLogTextEdit = new QTextEdit(logGroup);
    alpacaLogTextEdit->setMaximumHeight(200);
    alpacaLogTextEdit->setFont(QFont("Monaco", 10)); // Monospace font
    logLayout->addWidget(alpacaLogTextEdit);
    
    // Log control buttons
    QHBoxLayout *logButtonLayout = new QHBoxLayout();
    QPushButton *clearLogButton = new QPushButton("Clear Log", logGroup);
    QPushButton *saveLogButton = new QPushButton("Save Log", logGroup);
    
    connect(clearLogButton, &QPushButton::clicked, this, &TelescopeGUI::clearAlpacaLog);
    connect(saveLogButton, &QPushButton::clicked, this, &TelescopeGUI::saveAlpacaLog);
    
    logButtonLayout->addWidget(clearLogButton);
    logButtonLayout->addWidget(saveLogButton);
    logButtonLayout->addStretch();
    
    logLayout->addLayout(logButtonLayout);
    
    mainLayout->addWidget(logGroup);
    
    // Add stretch to push everything up
    mainLayout->addStretch();
    
    return tab;
}

// Add these slot implementations to TelescopeGUI.cpp:

void TelescopeGUI::startAlpacaServer()
{
    int port = alpacaPortSpinBox->value();
    
    alpacaLogTextEdit->append(QString("[%1] Starting Alpaca server on port %2...")
                             .arg(QTime::currentTime().toString())
                             .arg(port));
    
    if (alpacaServer->start(port)) {
        alpacaLogTextEdit->append(QString("[%1] Server started successfully")
                                 .arg(QTime::currentTime().toString()));
    } else {
        alpacaLogTextEdit->append(QString("[%1] Failed to start server")
                                 .arg(QTime::currentTime().toString()));
    }
}

void TelescopeGUI::stopAlpacaServer()
{
    alpacaLogTextEdit->append(QString("[%1] Stopping Alpaca server...")
                             .arg(QTime::currentTime().toString()));
    
    alpacaServer->stop();
    
    alpacaLogTextEdit->append(QString("[%1] Server stopped")
                             .arg(QTime::currentTime().toString()));
}

void TelescopeGUI::onAlpacaServerStarted()
{
    alpacaStatusLabel->setText("Running");
    alpacaStatusLabel->setStyleSheet("color: green;");
    alpacaPortLabel->setText(QString::number(alpacaPortSpinBox->value()));
    
    alpacaStartButton->setEnabled(false);
    alpacaStopButton->setEnabled(true);
    alpacaPortSpinBox->setEnabled(false);
    
    alpacaLogTextEdit->append(QString("[%1] Alpaca server is now accepting connections")
                             .arg(QTime::currentTime().toString()));
    alpacaLogTextEdit->append(QString("[%1] Discovery broadcasts enabled on UDP port 32227")
                             .arg(QTime::currentTime().toString()));
}

void TelescopeGUI::onAlpacaServerStopped()
{
    alpacaStatusLabel->setText("Stopped");
    alpacaStatusLabel->setStyleSheet("color: red;");
    alpacaPortLabel->setText("N/A");
    
    alpacaStartButton->setEnabled(true);
    alpacaStopButton->setEnabled(false);
    alpacaPortSpinBox->setEnabled(true);
}

void TelescopeGUI::onAlpacaRequestReceived(const QString& method, const QString& path)
{
    static int requestCount = 0;
    requestCount++;
    
    alpacaRequestCountLabel->setText(QString::number(requestCount));
    
    // Add to log with timestamp
    alpacaLogTextEdit->append(QString("[%1] %2 %3")
                             .arg(QTime::currentTime().toString())
                             .arg(method)
                             .arg(path));
    
    // Auto-scroll to bottom
    alpacaLogTextEdit->verticalScrollBar()->setValue(
        alpacaLogTextEdit->verticalScrollBar()->maximum());
}

void TelescopeGUI::clearAlpacaLog()
{
    alpacaLogTextEdit->clear();
    alpacaRequestCountLabel->setText("0");
}

void TelescopeGUI::saveAlpacaLog()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        "Save Alpaca Log", 
        QString("alpaca_log_%1.txt").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss")),
        "Text Files (*.txt)");
    
    if (!fileName.isEmpty()) {
        QFile file(fileName);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << alpacaLogTextEdit->toPlainText();
            
            alpacaLogTextEdit->append(QString("[%1] Log saved to %2")
                                     .arg(QTime::currentTime().toString())
                                     .arg(fileName));
        }
    }
}

void TelescopeGUI::startSlewAndImage() {
    if (!isConnected) {
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
        targetName = targetComboBox->currentText().split(" - ").at(0);
        
        // Parse the coordinates from the data
        QString coords = targetComboBox->currentData().toString();
        
        // Convert RA from HH:MM:SS.S format to decimal hours
        QRegularExpression raRegex("(\\d+)h(\\d+)m([\\d.]+)s");
        QRegularExpressionMatch raMatch = raRegex.match(coords);
        
        if (raMatch.hasMatch()) {
            int hours = raMatch.captured(1).toInt();
            int minutes = raMatch.captured(2).toInt();
            double seconds = raMatch.captured(3).toDouble();
            
            ra = hours + (minutes / 60.0) + (seconds / 3600.0);
        }
        
        // Convert Dec from DD:MM:SS format to decimal degrees
        QRegularExpression decRegex("([+-]?\\d+)°(\\d+)'([\\d.]+)\"");
        QRegularExpressionMatch decMatch = decRegex.match(coords);
        
        if (decMatch.hasMatch()) {
            int degrees = decMatch.captured(1).toInt();
            int minutes = decMatch.captured(2).toInt();
            double seconds = decMatch.captured(3).toDouble();
            
            dec = abs(degrees) + (minutes / 60.0) + (seconds / 3600.0);
            if (degrees < 0) dec = -dec;
        }
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
    gotoCommand["Command"] = "GotoRaDec";
    gotoCommand["Destination"] = "Mount";
    gotoCommand["SequenceID"] = 1000;
    gotoCommand["Source"] = "QtApp";
    gotoCommand["Type"] = "Command";
    gotoCommand["Ra"] = raRadians;
    gotoCommand["Dec"] = decRadians;
    
    sendJsonMessage(gotoCommand);
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
        runImagingCommand["Command"] = "RunImaging";
        runImagingCommand["Destination"] = "TaskController";
        runImagingCommand["SequenceID"] = 1001;
        runImagingCommand["Source"] = "QtApp";
        runImagingCommand["Type"] = "Command";
        runImagingCommand["Name"] = targetComboBox->currentIndex() == 0 ? 
                                  customNameEdit->text() : 
                                  targetComboBox->currentText().split(" - ").at(0);
        runImagingCommand["SaveRawImage"] = true;
        runImagingCommand["Uuid"] = currentImagingTargetUuid;
        
        // Send the command
        sendJsonMessage(runImagingCommand);
        
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
    QJsonObject cancelCommand;
    cancelCommand["Command"] = "AbortAxisMovement";
    cancelCommand["Destination"] = "Mount";
    cancelCommand["SequenceID"] = 1002;
    cancelCommand["Source"] = "QtApp";
    cancelCommand["Type"] = "Command";
    
    sendJsonMessage(cancelCommand);
    
    // Cancel any imaging
    QJsonObject cancelImagingCommand;
    cancelImagingCommand["Command"] = "CancelImaging";
    cancelImagingCommand["Destination"] = "TaskController";
    cancelImagingCommand["SequenceID"] = 1003;
    cancelImagingCommand["Source"] = "QtApp";
    cancelImagingCommand["Type"] = "Command";
    
    sendJsonMessage(cancelImagingCommand);
    
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
    if (!isConnected) {
        QMessageBox::warning(this, "Not Connected", "Please connect to a telescope first");
        return;
    }
    
    // Use the exact initialization command from init.txt
    QJsonObject runInitCommand;
    runInitCommand["Command"] = "RunInitialize";
    runInitCommand["Destination"] = "TaskController";
    runInitCommand["SequenceID"] = 1001;
    runInitCommand["Source"] = "QtApp";
    runInitCommand["Type"] = "Command";
    
    // Get current date and time - you can also use fixed values from init.txt
    QDateTime now = QDateTime::currentDateTime();
    QString dateStr = now.toString("dd MM yyyy");
    QString timeStr = now.toString("HH:mm:ss");
    
    // Use location values from init.txt - Cambridge, UK coordinates
    runInitCommand["Date"] = "06 05 2025"; // Or use dateStr for current date
    runInitCommand["FakeInitialize"] = false;
    runInitCommand["Latitude"] = 0.9118493267600084;  // In radians (Cambridge, UK)
    runInitCommand["Longitude"] = 0.0013880067713051129;  // In radians
    runInitCommand["Time"] = "20:37:39"; // Or use timeStr for current time
    runInitCommand["TimeZone"] = "Europe/London";
    
    sendJsonMessage(runInitCommand);
    
    slewStatusLabel->setText("Initializing telescope...");
    initializeButton->setEnabled(false);
    
    // Enable the alignment button after initialization
    QTimer::singleShot(5000, this, [this]() {
        autoAlignButton->setEnabled(true);
        initializeButton->setEnabled(true);
        slewStatusLabel->setText("Initialization completed. Check alignment status.");
    });
}

void TelescopeGUI::startTelescopeAlignment() {
    if (!isConnected) {
        QMessageBox::warning(this, "Not Connected", "Please connect to a telescope first");
        return;
    }
    
    // Send the StartAlignment command
    QJsonObject alignCommand;
    alignCommand["Command"] = "StartAlignment";
    alignCommand["Destination"] = "Mount";
    alignCommand["SequenceID"] = 1002;
    alignCommand["Source"] = "QtApp";
    alignCommand["Type"] = "Command";
    
    sendJsonMessage(alignCommand);
    
    slewStatusLabel->setText("Starting alignment procedure...");
    
    // Note: In a real implementation, we would need to guide the user through
    // the complete alignment procedure, which includes manually centering stars
    // and adding alignment points. This is just a simplified version.
}

void TelescopeGUI::checkMountStatus() {
    if (!isConnected) {
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
    startSlewButton->setEnabled(canSlew && !isSlewingAndImaging);
}

