#include "TelescopeGUI.hpp"
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
    
    mountLatitudeLabel->setText(QString::number(data.mount.latitude * 180.0 / M_PI, 'f', 6) + "°");
    mountLongitudeLabel->setText(QString::number(data.mount.longitude * 180.0 / M_PI, 'f', 6) + "°");
    
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
    CommandInterface *commandInterface = new CommandInterface(webSocket, this);
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
    
    qDebug() << logLine;
}

void TelescopeGUI::requestImage(const QString &filePath) {
    if (connectedIpAddress.isEmpty()) return;
    
    // Create a simple HTTP request to fetch the image
    QUrl url(QString("http://%1/%2").arg(connectedIpAddress, filePath));
    QNetworkRequest request(url);
    
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QNetworkReply *reply = manager->get(request);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager]() {
        if (reply->error() == QNetworkReply::NoError) {
            // Read the image data
            QByteArray imageData = reply->readAll();
            
            // Create a QPixmap from the data
            QPixmap pixmap;
            if (pixmap.loadFromData(imageData)) {
                // Scale to fit the label while preserving aspect ratio
                pixmap = pixmap.scaled(imagePreviewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                
                // Display the image
                imagePreviewLabel->setPixmap(pixmap);
            }
        }
        
        // Clean up
        reply->deleteLater();
        manager->deleteLater();
    });
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
