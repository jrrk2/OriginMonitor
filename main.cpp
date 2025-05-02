#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTextEdit>
#include <QUdpSocket>
#include <QTimer>
#include <QNetworkInterface>
#include <QDebug>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>

class TelescopeDiscoveryUDP : public QMainWindow {
    Q_OBJECT

public:
    TelescopeDiscoveryUDP(QWidget *parent = nullptr) : QMainWindow(parent) {
        // Setup UI
        QWidget *centralWidget = new QWidget(this);
        setCentralWidget(centralWidget);
        
        QVBoxLayout *layout = new QVBoxLayout(centralWidget);
        
        statusText = new QTextEdit(this);
        statusText->setReadOnly(true);
        layout->addWidget(statusText);
        
        QPushButton *listenButton = new QPushButton("Listen for UDP Broadcasts", this);
        connect(listenButton, &QPushButton::clicked, this, &TelescopeDiscoveryUDP::startUDPListener);
        layout->addWidget(listenButton);
        
        QPushButton *connectButton = new QPushButton("Connect to First Found Telescope", this);
        connect(connectButton, &QPushButton::clicked, this, &TelescopeDiscoveryUDP::connectToTelescope);
        layout->addWidget(connectButton);
        
        // Initialize the UDP socket
        udpSocket = new QUdpSocket(this);
        
        // Initialize WebSocket
        webSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
        connect(webSocket, &QWebSocket::connected, this, &TelescopeDiscoveryUDP::onWebSocketConnected);
        connect(webSocket, &QWebSocket::disconnected, this, &TelescopeDiscoveryUDP::onWebSocketDisconnected);
        connect(webSocket, &QWebSocket::textMessageReceived, this, &TelescopeDiscoveryUDP::onTextMessageReceived);
        connect(webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                [this](QAbstractSocket::SocketError error) {
            addStatus(QString("WebSocket error: %1").arg(webSocket->errorString()));
        });
        
        // Set window properties
        resize(600, 500);
        setWindowTitle("Celestron Origin UDP Discovery Tool");
    }
    
    void startUDPListener() {
        addStatus("Starting UDP listener for telescope broadcasts...");
        
        // Bind to the UDP broadcast port (55555)
        if (!udpSocket->bind(55555, QUdpSocket::ShareAddress)) {
            addStatus(QString("Failed to bind to UDP port 55555: %1").arg(udpSocket->errorString()));
            return;
        }
        
        // Connect the readyRead signal to our slot
        connect(udpSocket, &QUdpSocket::readyRead, this, &TelescopeDiscoveryUDP::processUDPDatagram);
        
        addStatus("UDP listener started on port 55555. Waiting for telescope broadcasts...");
        
        // Auto-stop listening after 30 seconds if nothing is found
        QTimer::singleShot(30000, [this]() {
            if (foundTelescopeAddresses.isEmpty()) {
                udpSocket->close();
                disconnect(udpSocket, &QUdpSocket::readyRead, this, &TelescopeDiscoveryUDP::processUDPDatagram);
                addStatus("No telescopes found after 30 seconds. Listener stopped.");
            }
        });
    }
    
    void processUDPDatagram() {
        // Read all available datagrams
        while (udpSocket->hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(udpSocket->pendingDatagramSize());
            QHostAddress sender;
            quint16 senderPort;
            
            // Read the datagram
            udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
            
            // Log the raw datagram for debugging
            addStatus(QString("Received UDP broadcast from %1:%2").arg(sender.toString()).arg(senderPort));
            addStatus(QString("Raw datagram: %1").arg(QString::fromUtf8(datagram)));
            
            // Check if this looks like a telescope broadcast
            QString datagramStr = QString::fromUtf8(datagram);
            if (datagramStr.contains("Origin", Qt::CaseInsensitive) && 
                datagramStr.contains("IP Address", Qt::CaseInsensitive)) {
                
                addStatus("*** FOUND CELESTRON ORIGIN TELESCOPE ***");
                
                // Try to extract the IP address from the datagram
                QRegularExpression ipRegex("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b");
                QRegularExpressionMatch match = ipRegex.match(datagramStr);
                
                QString telescopeIP;
                if (match.hasMatch()) {
                    telescopeIP = match.captured(0);
                    addStatus(QString("Extracted IP address from broadcast: %1").arg(telescopeIP));
                } else {
                    // Fall back to using the sender's address
                    telescopeIP = sender.toString();
                    addStatus(QString("Using sender address: %1").arg(telescopeIP));
                }
                
                // Parse model/identity info if available
                if (datagramStr.contains("Identity:")) {
                    int identityStart = datagramStr.indexOf("Identity:");
                    int identityEnd = datagramStr.indexOf(" ", identityStart + 9);
                    if (identityEnd > identityStart) {
                        QString identity = datagramStr.mid(identityStart + 9, identityEnd - identityStart - 9);
                        addStatus(QString("Telescope identity: %1").arg(identity));
                    }
                }
                
                // Add to our list if not already there
                if (!foundTelescopeAddresses.contains(telescopeIP)) {
                    foundTelescopeAddresses.append(telescopeIP);
                }
            }
        }
    }
    
    void connectToTelescope() {
        if (foundTelescopeAddresses.isEmpty()) {
            addStatus("No telescopes found yet. Please run the UDP listener first.");
            return;
        }
        
        // Use the first found telescope
        QString ipAddress = foundTelescopeAddresses.first();
        addStatus(QString("Connecting to telescope at %1...").arg(ipAddress));
        
        // Connect via WebSocket
        QUrl url(QString("ws://%1:80").arg(ipAddress));
        webSocket->open(url);
    }
    
    void onWebSocketConnected() {
        addStatus("WebSocket connected! Sending GetStatus command...");
        
        // Send a status request
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
        addStatus("WebSocket disconnected");
    }
    
    void onTextMessageReceived(const QString &message) {
        addStatus("Received WebSocket message:");
        addStatus(message);
        
        QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            
            // Display the response details
            QString responseType = obj["Type"].toString();
            QString command = obj["Command"].toString();
            
            addStatus(QString("Response details - Type: %1, Command: %2").arg(responseType, command));
            
            // If there's a Status object, display that too
            if (obj.contains("Status") && obj["Status"].isObject()) {
                QJsonObject status = obj["Status"].toObject();
                addStatus("Status information:");
                for (auto it = status.begin(); it != status.end(); ++it) {
                    addStatus(QString("  %1: %2").arg(it.key(), it.value().toVariant().toString()));
                }
            }
        }
    }
    
    void addStatus(const QString &message) {
        statusText->append(message);
        qDebug() << message;
    }

private:
    QTextEdit *statusText;
    QUdpSocket *udpSocket;
    QWebSocket *webSocket;
    QStringList foundTelescopeAddresses;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    TelescopeDiscoveryUDP discovery;
    discovery.show();
    
    return app.exec();
}

#include "main.moc"
