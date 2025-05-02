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
    
    void sendUdpBroadcast() {
        // As a last resort, try scanning each IP that responded to mDNS queries
        // This could be based on the traffic we saw in the packet capture
        // where the telescope was sending mDNS queries
        
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
                        
                        // For each IP in the subnet
                        quint32 ipInt = ip.toIPv4Address();
                        quint32 subnetInt = subnet.toIPv4Address();
                        quint32 networkInt = ipInt & subnetInt;
                        int hostCount = ~subnetInt + 1;
                        
                        for (int i = 1; i < hostCount - 1; i++) {
                            QHostAddress targetIp(networkInt + i);
                            
                            // Skip our own IP
                            if (targetIp != ip) {
                                // Try a WebSocket connection to this IP
                                tryWebSocketConnection(targetIp);
                            }
                        }
                    }
                }
            }
        }
        
        // Set a timer to indicate completion
        QTimer::singleShot(10000, [this]() {
            if (telescopeAddresses.isEmpty()) {
                statusLabel->setText("No Celestron Origin telescopes found");
            } else {
                statusLabel->setText(QString("Found %1 Celestron Origin telescope(s)").arg(telescopeAddresses.size()));
            }
        });
    }
    
    void tryWebSocketConnection(const QHostAddress &ip) {
        // Try a direct WebSocket connection to this IP
        QWebSocket *testSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
        
        connect(testSocket, &QWebSocket::connected, [this, testSocket, ip]() {
            // We got a connection! Send a command to verify it's a telescope
            verifyTelescopeWithWebSocket(ip, ip.toString());
            
            // Close this test socket
            testSocket->close();
        });
        
        connect(testSocket, &QWebSocket::disconnected, testSocket, &QWebSocket::deleteLater);
        
        // Set a timeout for the connection attempt
        QTimer::singleShot(200, [testSocket]() {
            if (testSocket->state() == QAbstractSocket::ConnectingState) {
                testSocket->abort();
                testSocket->deleteLater();
            }
        });
        
        // Try to connect
        QString url = QString("ws://%1:80").arg(ip.toString());
        testSocket->open(QUrl(url));
    }
    
    void verifyTelescopeWithWebSocket(const QHostAddress &ip, const QString &name) {
        // Create a temporary WebSocket to verify if this is really a telescope
        QWebSocket *testSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
        
        connect(testSocket, &QWebSocket::connected, [this, testSocket, ip, name]() {
            // Send a status request command
            QJsonObject command;
            command["Command"] = "GetStatus";
            command["Destination"] = "System";
            command["SequenceID"] = 1;
            command["Source"] = "QtApp";
            command["Type"] = "Command";
            
            QJsonDocument doc(command);
            testSocket->sendTextMessage(doc.toJson());
            
            // Set a timer to close this connection if no response
            QTimer::singleShot(1000, [testSocket]() {
                testSocket->close();
            });
        });
        
        connect(testSocket, &QWebSocket::textMessageReceived, [this, testSocket, ip, name](const QString &message) {
            QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
            if (!doc.isNull() && doc.isObject()) {
                QJsonObject obj = doc.object();
                
                // Check if this looks like a telescope response
                if (obj.contains("Command") && obj.contains("Source")) {
                    // This is very likely a Celestron Origin telescope
                    QString ipString = ip.toString();
                    
                    // Add to our list of discovered telescopes
                    if (!telescopeAddresses.contains(ipString)) {
                        telescopeAddresses.append(ipString);
                        
                        // Add to the UI list
                        QString displayText = QString("%1 (%2)").arg(ipString, name);
                        if (obj.contains("Source") && obj.contains("Command")) {
                            displayText += QString(" - %1 (%2)").arg(obj["Source"].toString(), 
                                                                   obj["Command"].toString());
                        }
                        resultsListWidget->addItem(displayText);
                        
                        statusLabel->setText(QString("Found %1 Celestron Origin telescope(s)").arg(telescopeAddresses.size()));
                    }
                }
            }
            
            testSocket->close();
        });
        
        connect(testSocket, &QWebSocket::disconnected, testSocket, &QWebSocket::deleteLater);
        
        // Connect to the WebSocket endpoint
        QString url = QString("ws://%1:80").arg(ip.toString());
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
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    TelescopeDiscovery discovery;
    discovery.show();
    
    return app.exec();
}

#include "main.moc"
