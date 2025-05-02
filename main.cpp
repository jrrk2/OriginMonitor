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
#include <QRegularExpression>
#include <QDebug>
#include <QQueue>
#include <QRandomGenerator>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

class TelescopeDiscovery : public QMainWindow {
    Q_OBJECT

public:
    TelescopeDiscovery(QWidget *parent = nullptr) : QMainWindow(parent) {
        // Setup UI
        QWidget *centralWidget = new QWidget(this);
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
        
        // Create and configure the scan timer
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
                        tryHTTPConnection(QHostAddress(ipAddress));
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
                // If we still haven't found anything, try the last resort: network scan
                scanLocalNetwork();
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
                    scanLocalNetwork();
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
    
    void scanLocalNetwork() {
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
            tryHTTPConnection(pendingScans.dequeue());
        }
        
        // Set a timer to update the status when we're done
        QTimer::singleShot(30000, [this]() {
            // Stop the scan timer if it's still running
            if (scanTimer->isActive()) {
                scanTimer->stop();
            }
            
            // Abort any remaining connections
            for (QNetworkAccessManager *manager : activeNetworkManagers) {
                manager->deleteLater();
            }
            activeNetworkManagers.clear();
            pendingScans.clear();
            
            // Update status
            if (telescopeAddresses.isEmpty()) {
                statusLabel->setText("No Celestron Origin telescopes found");
            } else {
                statusLabel->setText(QString("Found %1 Celestron Origin telescope(s)").arg(telescopeAddresses.size()));
            }
        });
    }
    
    void tryHTTPConnection(const QHostAddress &ip) {
        // If we already have too many active connections, queue this request
        if (activeNetworkManagers.size() >= maxConcurrentConnections) {
            pendingScans.enqueue(ip);
            
            // Make sure the scan timer is running to process the queue
            if (!scanTimer->isActive()) {
                scanTimer->start(50); // Process queued scans every 50ms
            }
            return;
        }

        // Use HTTP GET instead of WebSocket
        QString ipString = ip.toString();
        qDebug() << "Checking IP" << ipString << "using HTTP GET";
        
        // Keep track of this request
        QNetworkAccessManager *manager = new QNetworkAccessManager(this);
        activeNetworkManagers.append(manager);
        
        // Handle completion of the HTTP request
        connect(manager, &QNetworkAccessManager::finished, 
                [this, manager, ipString](QNetworkReply *reply) {
            
            // Remove from tracking list
            activeNetworkManagers.removeOne(manager);
            
            // Process the response if successful
            if (reply->error() == QNetworkReply::NoError) {
                // Read the response data
                QByteArray data = reply->readAll();
                
                // Print response size for debugging
                qDebug() << "Received HTTP response from" << ipString << "of size" << data.size() << "bytes";
                
                // ANY HTTP 200 response is probably our telescope, but we'll do some basic checks
                if (data.size() > 0) {
                    // Check for HTML content
                    QString htmlContent = QString::fromUtf8(data);
                    
                    // Print the first 100 characters of the HTML for debugging
                    qDebug() << "HTML received from" << ipString << ":" << htmlContent.left(100);
                    
                    // At this point, we've confirmed the device responds on port 80
                    // Mark as a potential telescope
                    qDebug() << "Found potential Origin telescope at" << ipString;
                    
                    // Add to our list of discovered telescopes if not already there
                    if (!telescopeAddresses.contains(ipString)) {
                        telescopeAddresses.append(ipString);
                        
                        // Add to the UI list with a more informative description
                        QString displayText = QString("%1 - Potential Celestron Origin Telescope").arg(ipString);
                        resultsListWidget->addItem(displayText);
                        
                        statusLabel->setText(QString("Found %1 potential Celestron Origin telescope(s)")
                                           .arg(telescopeAddresses.size()));
                    }
                } else {
                    qDebug() << "Device at" << ipString << "returned empty response";
                }
            } else {
                qDebug() << "HTTP error for" << ipString << ":" << reply->errorString();
            }
            
            // Clean up
            reply->deleteLater();
            manager->deleteLater();
            
            // Process next pending scan if any
            processNextPendingScan();
        });
        
        // Set timeout for the request
        QTimer *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, [this, manager, timer, ipString]() {
            qDebug() << "HTTP request timeout for" << ipString;
            if (activeNetworkManagers.contains(manager)) {
                activeNetworkManagers.removeOne(manager);
                manager->deleteLater();
                
                // Process next pending scan
                processNextPendingScan();
            }
            timer->deleteLater();
        });
        timer->start(500);  // 500ms timeout
        
        // Create and send the HTTP GET request
        QUrl url(QString("http://%1").arg(ipString));
        QNetworkRequest request(url);
        request.setTransferTimeout(3000);  // 3 second transfer timeout
        
        // Add a user agent to make the request more like a browser
        request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 TelescopeDiscovery/1.0");
        
        // Use a lower timeout for the timer than the transfer timeout
        QTimer::singleShot(timer->interval(), [this, manager, ipString]() {
            // If the manager is still in our list, it means the request is still pending
            if (activeNetworkManagers.contains(manager)) {
                qDebug() << "Manually aborting HTTP request to" << ipString << "due to timeout";
                // manager->abort();
            }
        });
        
        manager->get(request);
    }
    
    // Helper method to process the pending IP scan queue
    void processNextPendingScan() {
        // If we have pending scans and room for more active connections, process next
        if (!pendingScans.isEmpty() && activeNetworkManagers.size() < maxConcurrentConnections) {
            QHostAddress nextIp = pendingScans.dequeue();
            tryHTTPConnection(nextIp);
            
            // Make sure the timer is running to continue processing
            if (!scanTimer->isActive() && !pendingScans.isEmpty()) {
                scanTimer->start(50);
            }
        } else if (pendingScans.isEmpty() && activeNetworkManagers.isEmpty()) {
            // We're done with all scans
            scanTimer->stop();
            if (telescopeAddresses.isEmpty()) {
                statusLabel->setText("No Celestron Origin telescopes found");
            } else {
                statusLabel->setText(QString("Found %1 Celestron Origin telescope(s)")
                                   .arg(telescopeAddresses.size()));
            }
        }
    }
    
    void connectToTelescope() {
        QListWidgetItem *selectedItem = resultsListWidget->currentItem();
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
    QList<QNetworkAccessManager*> activeNetworkManagers; // Track active HTTP requests
    QQueue<QHostAddress> pendingScans;    // Queue for pending IP scans
    const int maxConcurrentConnections = 10; // Limit concurrent connections (higher for HTTP)
    QTimer *scanTimer;                    // Timer for processing scan queue
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    TelescopeDiscovery discovery;
    discovery.show();
    
    return app.exec();
}

#include "main.moc"
