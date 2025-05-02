#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QDebug>
#include <QQueue>

class TelescopeDiscovery : public QMainWindow {
    Q_OBJECT

public:
    TelescopeDiscovery(QWidget *parent = nullptr) : QMainWindow(parent) {
        // Setup UI
        QWidget *centralWidget = new  QWidget(this);
        setCentralWidget(centralWidget);
        
        QVBoxLayout *layout = new QVBoxLayout(centralWidget);
        
        statusLabel = new QLabel("Ready to discover Celestron Origin telescopes", this);
        layout->addWidget(statusLabel);
        
        QPushButton *discoverButton = new QPushButton("Discover Telescopes", this);
        connect(discoverButton, &QPushButton::clicked, this, &TelescopeDiscovery::startDiscovery);
        layout->addWidget(discoverButton);
        
        resultsListWidget = new QListWidget(this);
        layout->addWidget(resultsListWidget);
        
        QPushButton *connectButton = new QPushButton("Connect to Selected Telescope", this);
        connect(connectButton, &QPushButton::clicked, this, &TelescopeDiscovery::connectToTelescope);
        layout->addWidget(connectButton);
        
        // WebSocket for telescope connection
        webSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
        connect(webSocket, &QWebSocket::connected, this, &TelescopeDiscovery::onWebSocketConnected);
        connect(webSocket, &QWebSocket::disconnected, this, &TelescopeDiscovery::onWebSocketDisconnected);
        connect(webSocket, &QWebSocket::textMessageReceived, this, &TelescopeDiscovery::onTextMessageReceived);

    scanTimer = new QTimer(this);
    connect(scanTimer, &QTimer::timeout, this, &TelescopeDiscovery::processNextPendingScan);
	
        // Set window size and title
        resize(600, 400);
        setWindowTitle("Celestron Origin Telescope Discovery");
    }

    // Method to lookup all .local hostnames on the network using DNS-SD
    void startDiscovery() {
        statusLabel->setText("Discovering telescopes...");
        resultsListWidget->clear();
        telescopeAddresses.clear();
        
        // Use dns-sd command line tool to browse for services (works well on macOS)
        QProcess *process = new QProcess(this);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
            process->deleteLater();
            
            if (exitCode != 0) {
                statusLabel->setText("Error running mDNS discovery");
                return;
            }
            
            // Move on to resolving found hostnames
            if (telescopeAddresses.isEmpty()) {
                // If we didn't find any telescopes via service browsing,
                // look for any .local hostnames that might be telescopes
                findLocalHostnames();
            }
        });
        
        // First, look for any HTTP services on the network
        process->start("dns-sd", QStringList() << "-B" << "_http._tcp" << "local");
        
        // Also handle process output as it arrives
        connect(process, &QProcess::readyReadStandardOutput, [this, process]() {
            QByteArray output = process->readAllStandardOutput();
            parseServiceDiscoveryOutput(output);
        });
        
        // Also set a timeout to ensure we don't wait forever
        QTimer::singleShot(5000, [this, process]() {
            if (process->state() == QProcess::Running) {
                process->terminate();
                
                // Move on to the next discovery method
                findLocalHostnames();
            }
        });
    }
    
    void parseServiceDiscoveryOutput(const QByteArray &output) {
        // Parse the output of the dns-sd command
        QString outputStr = QString::fromUtf8(output);
        QStringList lines = outputStr.split('\n', Qt::SkipEmptyParts);
        
        for (const QString &line : lines) {
            // Look for lines that might contain service instance names
            if (line.contains("_http._tcp") && !line.startsWith("Browsing")) {
                // Extract the service name
                QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                if (parts.size() >= 4) {
                    QString serviceName = parts[3];
                    
                    // If this looks like it might be a Celestron device
                    if (serviceName.contains("Celestron", Qt::CaseInsensitive) || 
                        serviceName.contains("Origin", Qt::CaseInsensitive) ||
                        serviceName.contains("Telescope", Qt::CaseInsensitive)) {
                        
                        // Resolve this service to get its IP
                        resolveService(serviceName, "_http._tcp");
                    }
                }
            }
        }
    }
    
    void resolveService(const QString &serviceName, const QString &serviceType) {
        QProcess *resolveProcess = new QProcess(this);
        connect(resolveProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                [this, resolveProcess](int exitCode, QProcess::ExitStatus exitStatus) {
            resolveProcess->deleteLater();
        });
        
        // Handle process output as it arrives
        connect(resolveProcess, &QProcess::readyReadStandardOutput, [this, resolveProcess, serviceName]() {
            QByteArray output = resolveProcess->readAllStandardOutput();
            QString outputStr = QString::fromUtf8(output);
            QStringList lines = outputStr.split('\n', Qt::SkipEmptyParts);
            
            for (const QString &line : lines) {
                // Look for IP address in the resolved output
                if (line.contains(".") && !line.startsWith("Lookup")) {
                    QRegularExpression ipRegex("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b");
                    QRegularExpressionMatch match = ipRegex.match(line);
                    if (match.hasMatch()) {
                        QString ipAddress = match.captured(0);
                        
                        // Verify this is a telescope by trying to connect
                        verifyTelescopeWithWebSocket(QHostAddress(ipAddress), serviceName);
                    }
                }
            }
        });
        
        // Start the resolution process
        resolveProcess->start("dns-sd", QStringList() << "-L" << serviceName << serviceType << "local");
        
        // Set a timeout
        QTimer::singleShot(3000, [resolveProcess]() {
            if (resolveProcess->state() == QProcess::Running) {
                resolveProcess->terminate();
            }
        });
    }
    
    void findLocalHostnames() {
        // Look for device hostnames directly
        QProcess *process = new QProcess(this);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
            process->deleteLater();
            
            // We're now done with discovery
            if (telescopeAddresses.isEmpty()) {
                // If we still haven't found anything, try the last resort: UDP broadcast
                sendUdpBroadcast();
            } else {
                statusLabel->setText(QString("Found %1 Celestron Origin telescope(s)").arg(telescopeAddresses.size()));
            }
        });
        
        // Use avahi-browse or similar to find all mDNS hostnames
        // This is the macOS specific approach using dns-sd
        process->start("dns-sd", QStringList() << "-B" << "_services._dns-sd._udp" << "local");
        
        // Also handle process output as it arrives
        connect(process, &QProcess::readyReadStandardOutput, [this, process]() {
            QByteArray output = process->readAllStandardOutput();
            parseLocalHostnamesOutput(output);
        });
        
        // Set a timeout
        QTimer::singleShot(5000, [this, process]() {
            if (process->state() == QProcess::Running) {
                process->terminate();
                
                // Final check
                if (telescopeAddresses.isEmpty()) {
                    sendUdpBroadcast();
                } else {
                    statusLabel->setText(QString("Found %1 Celestron Origin telescope(s)").arg(telescopeAddresses.size()));
                }
            }
        });
    }
    
    void parseLocalHostnamesOutput(const QByteArray &output) {
        // Parse the output looking for potential service types
        QString outputStr = QString::fromUtf8(output);
        QStringList lines = outputStr.split('\n', Qt::SkipEmptyParts);
        
        for (const QString &line : lines) {
            if (line.contains("_tcp") && !line.startsWith("Browsing")) {
                QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                if (parts.size() >= 4) {
                    QString serviceType = parts[3];
                    
                    // Check for any service types that might be used by the telescope
                    if (serviceType != "_http._tcp" && 
                        (serviceType.contains("_device") || 
                         serviceType.contains("_control") || 
                         serviceType.contains("_telescope"))) {
                        
                        // Try to browse for this service type
                        browseServiceType(serviceType);
                    }
                }
            }
        }
    }
    
    void browseServiceType(const QString &serviceType) {
        QProcess *browseProcess = new QProcess(this);
        connect(browseProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                [browseProcess](int exitCode, QProcess::ExitStatus exitStatus) {
            browseProcess->deleteLater();
        });
        
        // Handle process output
        connect(browseProcess, &QProcess::readyReadStandardOutput, [this, browseProcess, serviceType]() {
            QByteArray output = browseProcess->readAllStandardOutput();
            QString outputStr = QString::fromUtf8(output);
            QStringList lines = outputStr.split('\n', Qt::SkipEmptyParts);
            
            for (const QString &line : lines) {
                if (!line.startsWith("Browsing")) {
                    QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                    if (parts.size() >= 4) {
                        QString serviceName = parts[3];
                        
                        // Resolve this service
                        resolveService(serviceName, serviceType);
                    }
                }
            }
        });
        
        // Start the browse process
        browseProcess->start("dns-sd", QStringList() << "-B" << serviceType << "local");
        
        // Set a timeout
        QTimer::singleShot(3000, [browseProcess]() {
            if (browseProcess->state() == QProcess::Running) {
                browseProcess->terminate();
            }
        });
    }


// Replace your sendUdpBroadcast method with this one
void sendUdpBroadcast() {
    // Clear any existing scan queue
    pendingScans.clear();
    
    // Reset the status
    statusLabel->setText("Scanning network for telescopes...");
    
    // For each local network interface
    QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        if (interface.flags() & QNetworkInterface::IsRunning && 
            !(interface.flags() & QNetworkInterface::IsLoopBack)) {
            
            QList<QNetworkAddressEntry> entries = interface.addressEntries();
            for (const QNetworkAddressEntry &entry : entries) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    // Get the subnet info
                    QHostAddress ip = entry.ip();
                    QHostAddress subnet = entry.netmask();
                    
                    // Calculate network parameters
                    quint32 ipInt = ip.toIPv4Address();
                    quint32 subnetInt = subnet.toIPv4Address();
                    quint32 networkInt = ipInt & subnetInt;
                    int hostCount = ~subnetInt + 1;
                    
                    // Limit the scan range to avoid overloading
                    int maxHosts = qMin(hostCount - 2, 254);  // Limit to 254 hosts
                    
                    for (int i = 1; i <= maxHosts; i++) {
                        QHostAddress targetIp(networkInt + i);
                        
                        // Skip our own IP
                        if (targetIp != ip) {
                            // Add to the queue instead of immediately connecting
                            pendingScans.enqueue(targetIp);
                        }
                    }
                }
            }
        }
    }
    
    // Shuffle the queue to get a better distribution of scanning
    // (prevents hitting the same subnet range all at once)
    QList<QHostAddress> tempList = pendingScans.toList();

    // Use Qt's own random generator instead of std::random_shuffle
    QRandomGenerator generator(QDateTime::currentMSecsSinceEpoch());
    for (int i = tempList.size() - 1; i > 0; --i) {
        int j = generator.bounded(i + 1);
        if (i != j) {
            tempList.swapItemsAt(i, j);
        }
    }
    
    pendingScans.clear();
    for (const QHostAddress &addr : tempList) {
        pendingScans.enqueue(addr);
    }
    
    // Start processing the queue by kicking off maxConcurrentConnections requests
    for (int i = 0; i < maxConcurrentConnections && !pendingScans.isEmpty(); i++) {
        tryWebSocketConnection(pendingScans.dequeue());
    }
    
    // Set a timer to update the status when we're done
    QTimer::singleShot(30000, [this]() {
        // Stop the scan timer if it's still running
        if (scanTimer->isActive()) {
            scanTimer->stop();
        }
        
        // Abort any remaining connections
        for (QWebSocket *socket : activeTestSockets) {
            socket->abort();
            socket->deleteLater();
        }
        activeTestSockets.clear();
        pendingScans.clear();
        
        // Update status
        if (telescopeAddresses.isEmpty()) {
            statusLabel->setText("No Celestron Origin telescopes found");
        } else {
            statusLabel->setText(QString("Found %1 Celestron Origin telescope(s)").arg(telescopeAddresses.size()));
        }
    });
}

  // Replace your tryWebSocketConnection method with this one
void tryWebSocketConnection(const QHostAddress &ip) {
    // If we already have too many active connections, queue this request
    if (activeTestSockets.size() >= maxConcurrentConnections) {
        pendingScans.enqueue(ip);
        
        // Make sure the scan timer is running to process the queue
        if (!scanTimer->isActive()) {
            scanTimer->start(50); // Process queued scans every 50ms
        }
        return;
    }

    // Create a new socket for testing
    QWebSocket *testSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
    activeTestSockets.append(testSocket);
    
    // Store IP as string for use in lambdas
    QString ipString = ip.toString();
    qDebug() << "Creating WebSocket for" << ipString;
    
    // Connection success handler
    connect(testSocket, &QWebSocket::connected, [this, testSocket, ipString]() {
        qDebug() << "WebSocket connected to" << ipString;
        // Remove from active sockets list - it's handled by verifyTelescopeWithWebSocket now
        activeTestSockets.removeOne(testSocket);
        verifyTelescopeWithWebSocket(QHostAddress(ipString), ipString);
        
        // Process next pending scan if any
        processNextPendingScan();
    });
    
    // Ensure socket is properly cleaned up on disconnect
    connect(testSocket, &QWebSocket::disconnected, [this, testSocket]() {
        activeTestSockets.removeOne(testSocket);
        testSocket->deleteLater();
        
        // Process next pending scan if any
        processNextPendingScan();
    });
    
    // Error handler with proper cleanup
    connect(testSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            [this, testSocket, ipString](QAbstractSocket::SocketError error) {
        qDebug() << "WebSocket error for" << ipString << ":" << error;
        activeTestSockets.removeOne(testSocket);
        testSocket->deleteLater();
        
        // Process next pending scan if any
        processNextPendingScan();
    });
    
    // Connection timeout with proper cleanup
    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, [this, testSocket, timer, ipString]() {
        qDebug() << "Connection timeout for" << ipString;
        if (activeTestSockets.contains(testSocket)) {
            activeTestSockets.removeOne(testSocket);
            testSocket->abort();  // Properly abort the connection
            testSocket->deleteLater();
            
            // Process next pending scan if any
            processNextPendingScan();
        }
        timer->deleteLater();
    });
    timer->start(500); // Longer timeout (500ms instead of 200ms)
    
    // Try to connect
    QString url = QString("ws://%1:80").arg(ipString);
    qDebug() << "Opening WebSocket connection to" << url;
    testSocket->open(QUrl(url));
}

// Add this helper method to process the pending IP scan queue
void processNextPendingScan() {
    // If we have pending scans and room for more active connections, process next
    if (!pendingScans.isEmpty() && activeTestSockets.size() < maxConcurrentConnections) {
        QHostAddress nextIp = pendingScans.dequeue();
        tryWebSocketConnection(nextIp);
    }
}

// Replace your verifyTelescopeWithWebSocket method with this one
void verifyTelescopeWithWebSocket(const QHostAddress &ip, const QString &name) {
    // Create a temporary WebSocket to verify if this is really a telescope
    QWebSocket *testSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
    QString ipString = ip.toString();
    
    // Debugging
    qDebug() << "Verifying if" << ipString << "is a telescope";
    
    bool responseReceived = false;
    
    connect(testSocket, &QWebSocket::connected, [this, testSocket, ipString, name, &responseReceived]() {
        qDebug() << "Verification WebSocket connected to" << ipString;
        
        // Send a status request command
        QJsonObject command;
        command["Command"] = "GetStatus";
        command["Destination"] = "System";
        command["SequenceID"] = 1;
        command["Source"] = "QtApp";
        command["Type"] = "Command";
        
        QJsonDocument doc(command);
        testSocket->sendTextMessage(doc.toJson());
    });
    
    connect(testSocket, &QWebSocket::textMessageReceived, 
            [this, testSocket, ipString, name, &responseReceived](const QString &message) {
        // Mark that we received a response
        responseReceived = true;
        
        qDebug() << "Received response from" << ipString;
        
        QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            
            // Check if this looks like a telescope response
            if (obj.contains("Command") && obj.contains("Source")) {
                // This is very likely a Celestron Origin telescope
                
                // Add to our list of discovered telescopes if not already there
                if (!telescopeAddresses.contains(ipString)) {
                    telescopeAddresses.append(ipString);
                    
                    // Add to the UI list
                    QString displayText = QString("%1 (%2)").arg(ipString, name);
                    if (obj.contains("Source") && obj.contains("Command")) {
                        displayText += QString(" - %1 (%2)").arg(obj["Source"].toString(), 
                                                               obj["Command"].toString());
                    }
                    resultsListWidget->addItem(displayText);
                    
                    statusLabel->setText(QString("Found %1 Celestron Origin telescope(s)")
                                       .arg(telescopeAddresses.size()));
                }
            }
        }
        
        // Always close the socket after processing the response
        testSocket->close();
    });
    
    // Error handling with proper cleanup
    connect(testSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            [testSocket, ipString](QAbstractSocket::SocketError error) {
        qDebug() << "Verification WebSocket error for" << ipString << ":" << error;
        testSocket->deleteLater();
    });
    
    // Properly clean up on disconnect
    connect(testSocket, &QWebSocket::disconnected, [testSocket, &responseReceived]() {
        testSocket->deleteLater();
    });
    
    // Set a timeout to close this connection if no response
    QTimer *timer = new QTimer(testSocket);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, [testSocket, &responseReceived, ipString]() {
        if (!responseReceived) {
            qDebug() << "Verification timeout for" << ipString;
            testSocket->abort();
        }
    });
    timer->start(1500);  // 1.5 second timeout (longer than before)
    
    // Connect to the WebSocket endpoint
    QString url = QString("ws://%1:80").arg(ipString);
    qDebug() << "Opening verification WebSocket connection to" << url;
    testSocket->open(QUrl(url));
}
  
    void connectToTelescope() {
        QListWidgetItem *selectedItem = resultsListWidget->currentItem();
        if (!selectedItem) {
            statusLabel->setText("Please select a telescope from the list");
            return;
        }
        
        // Extract the IP address from the selected item
        QString text = selectedItem->text();
        QString ipAddress = text.split(" ").first();
        
        statusLabel->setText(QString("Connecting to telescope at %1...").arg(ipAddress));
        
        // Connect to the telescope
        QString url = QString("ws://%1:80").arg(ipAddress);
        webSocket->open(QUrl(url));
    }
    
    void onWebSocketConnected() {
        statusLabel->setText("Connected to telescope!");
        
        // Send a status request to get basic info
        QJsonObject command;
        command["Command"] = "GetStatus";
        command["Destination"] = "Mount";
        command["SequenceID"] = 1;
        command["Source"] = "QtApp";
        command["Type"] = "Command";
        
        QJsonDocument doc(command);
        webSocket->sendTextMessage(doc.toJson());
    }
    
    void onWebSocketDisconnected() {
        statusLabel->setText("Disconnected from telescope");
    }
    
    void onTextMessageReceived(const QString &message) {
        QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            
            // Display the response
            QString responseType = obj["Type"].toString();
            QString command = obj["Command"].toString();
            
            statusLabel->setText(QString("Received %1 for %2").arg(responseType, command));
            
            // You can add more specific handling here
        }
    }

private:
    QLabel *statusLabel;
    QListWidget *resultsListWidget;
    QWebSocket *webSocket;
    QStringList telescopeAddresses;
    QList<QWebSocket*> activeTestSockets; // Track active test sockets
    QQueue<QHostAddress> pendingScans;    // Queue for pending IP scans
    const int maxConcurrentConnections = 5; // Limit concurrent connections
    QTimer *scanTimer;                    // Timer for processing scan queue
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    TelescopeDiscovery discovery;
    discovery.show();
    
    return app.exec();
}

#include "main.moc"
