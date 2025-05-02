#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QUdpSocket>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
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
        
        // UDP socket for discovery
        udpSocket = new QUdpSocket(this);
        
        // Set window size and title
        resize(600, 400);
        setWindowTitle("Celestron Origin Telescope Discovery");
    }

    void startDiscovery() {
        statusLabel->setText("Discovering telescopes...");
        resultsListWidget->clear();
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
            connect(udpSocket, &QUdpSocket::readyRead, this, &TelescopeDiscovery::processPendingDatagrams);
            
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
    
    void stopDiscovery() {
        // Disconnect signals and close socket
        if (udpSocket->state() != QAbstractSocket::UnconnectedState) {
            disconnect(udpSocket, &QUdpSocket::readyRead, this, &TelescopeDiscovery::processPendingDatagrams);
            udpSocket->close();
        }
        
        statusLabel->setText("Discovery stopped");
    }
    
    void processPendingDatagrams() {
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
                        
                        resultsListWidget->addItem(displayText);
                        
                        statusLabel->setText(QString("Found Celestron Origin telescope at %1").arg(telescopeIP));
                    }
                }
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
        
        // Connect to the telescope via WebSocket
        QString url = QString("ws://%1:80").arg(ipAddress);
        webSocket->open(QUrl(url));
    }
    
    void onWebSocketConnected() {
        statusLabel->setText("Connected to telescope!");
        
        // Send a status request to get basic info
        QJsonObject command;
        command["Command"] = "GetStatus";
        command["Destination"] = "System";
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
    QUdpSocket *udpSocket;
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
