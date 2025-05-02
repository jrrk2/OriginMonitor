#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTextEdit>
#include <QUdpSocket>
#include <QNetworkInterface>
#include <QTimer>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QStatusBar>

class RawUDPListener : public QMainWindow {
    Q_OBJECT

public:
    RawUDPListener(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("Raw UDP Broadcast Listener");
        resize(800, 600);
        
        QWidget *centralWidget = new QWidget(this);
        setCentralWidget(centralWidget);
        
        QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
        
        // Interface selection
        QHBoxLayout *interfaceLayout = new QHBoxLayout();
        interfaceLayout->addWidget(new QLabel("Network Interface:"));
        interfaceCombo = new QComboBox();
        interfaceLayout->addWidget(interfaceCombo);
        mainLayout->addLayout(interfaceLayout);
        
        // Options
        QHBoxLayout *optionsLayout = new QHBoxLayout();
        allowBroadcastCheck = new QCheckBox("Allow Broadcast");
        allowBroadcastCheck->setChecked(true);
        optionsLayout->addWidget(allowBroadcastCheck);
        
        reuseAddressCheck = new QCheckBox("Reuse Address");
        reuseAddressCheck->setChecked(true);
        optionsLayout->addWidget(reuseAddressCheck);
        
        shareAddressCheck = new QCheckBox("Share Address");
        shareAddressCheck->setChecked(true);
        optionsLayout->addWidget(shareAddressCheck);
        
        mainLayout->addLayout(optionsLayout);
        
        // Port selection
        QHBoxLayout *portLayout = new QHBoxLayout();
        portLayout->addWidget(new QLabel("Port:"));
        portEdit = new QLineEdit("55555");
        portLayout->addWidget(portEdit);
        mainLayout->addLayout(portLayout);
        
        // Control buttons
        QHBoxLayout *controlLayout = new QHBoxLayout();
        startButton = new QPushButton("Start Listening");
        connect(startButton, &QPushButton::clicked, this, &RawUDPListener::startListening);
        controlLayout->addWidget(startButton);
        
        stopButton = new QPushButton("Stop Listening");
        connect(stopButton, &QPushButton::clicked, this, &RawUDPListener::stopListening);
        stopButton->setEnabled(false);
        controlLayout->addWidget(stopButton);
        
        clearButton = new QPushButton("Clear Log");
        connect(clearButton, &QPushButton::clicked, this, &RawUDPListener::clearLog);
        controlLayout->addWidget(clearButton);
        
        mainLayout->addLayout(controlLayout);
        
        // Log area
        logText = new QTextEdit();
        logText->setReadOnly(true);
        mainLayout->addWidget(logText);
        
        // Status bar for packet counts
        statusBar = new QStatusBar(this);
        setStatusBar(statusBar);
        packetCountLabel = new QLabel("Packets: 0");
        statusBar->addWidget(packetCountLabel);
        
        // Setup UDP socket
        udpSocket = new QUdpSocket(this);
        
        // Populate interface list
        populateInterfaceList();
        
        // Update packet count periodically
        QTimer *updateTimer = new QTimer(this);
        connect(updateTimer, &QTimer::timeout, this, &RawUDPListener::updateStatus);
        updateTimer->start(1000);  // Update every second
    }
    
    void populateInterfaceList() {
        interfaceCombo->clear();
        
        // Add "Any" option
        interfaceCombo->addItem("Any (0.0.0.0)", QVariant::fromValue(QHostAddress(QHostAddress::AnyIPv4)));
        
        // Add all IPv4 interfaces
        QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &interface : interfaces) {
            if (interface.flags() & QNetworkInterface::IsRunning &&
                !(interface.flags() & QNetworkInterface::IsLoopBack)) {
                
                QList<QNetworkAddressEntry> entries = interface.addressEntries();
                for (const QNetworkAddressEntry &entry : entries) {
                    if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                        QString displayName = QString("%1 - %2 (%3)")
                                            .arg(interface.name())
                                            .arg(interface.humanReadableName())
                                            .arg(entry.ip().toString());
                        
                        interfaceCombo->addItem(displayName, QVariant::fromValue(entry.ip()));
                    }
                }
            }
        }
        
        // Select "Any" as the default option since this is best for catching broadcasts
        interfaceCombo->setCurrentIndex(0);
        
        // Add diagnostics info about interfaces to the log
        logMessage("Available Network Interfaces:");
        for (const QNetworkInterface &interface : interfaces) {
            if (interface.flags() & QNetworkInterface::IsRunning) {
                logMessage(QString("- %1 (%2)")
                          .arg(interface.name())
                          .arg(interface.humanReadableName()));
                
                QList<QNetworkAddressEntry> entries = interface.addressEntries();
                for (const QNetworkAddressEntry &entry : entries) {
                    if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                        logMessage(QString("  IP: %1").arg(entry.ip().toString()));
                        logMessage(QString("  Netmask: %1").arg(entry.netmask().toString()));
                        logMessage(QString("  Broadcast: %1").arg(entry.broadcast().toString()));
                    }
                }
            }
        }
    }
    
    void startListening() {
        // Get the selected interface
        QHostAddress bindAddress = interfaceCombo->currentData().value<QHostAddress>();
        
        // Get the port
        bool ok;
        int port = portEdit->text().toInt(&ok);
        if (!ok || port <= 0 || port > 65535) {
            logMessage("ERROR: Invalid port number!");
            return;
        }
        
        // Close existing socket if needed
        if (udpSocket->state() != QAbstractSocket::UnconnectedState) {
            udpSocket->close();
        }
        
        // Set socket options
        QUdpSocket::BindMode bindMode = QUdpSocket::DefaultForPlatform;
        if (reuseAddressCheck->isChecked()) {
            bindMode |= QUdpSocket::ReuseAddressHint;
        }
        if (shareAddressCheck->isChecked()) {
            bindMode |= QUdpSocket::ShareAddress;
        }
        
        // Bind the socket
        bool success = udpSocket->bind(bindAddress, port, bindMode);
        
        if (success) {
            if (allowBroadcastCheck->isChecked()) {
                // Use the proper enum value for broadcast option (QAbstractSocket::BroadcastOption = 5)
                udpSocket->setSocketOption(QAbstractSocket::SocketOption(5), 1);
            }
            
            connect(udpSocket, &QUdpSocket::readyRead, this, &RawUDPListener::processPendingDatagrams);
            
            logMessage(QString("Started listening on %1:%2")
                      .arg(bindAddress.toString())
                      .arg(port));
            
            startButton->setEnabled(false);
            stopButton->setEnabled(true);
            interfaceCombo->setEnabled(false);
            portEdit->setEnabled(false);
            allowBroadcastCheck->setEnabled(false);
            reuseAddressCheck->setEnabled(false);
            shareAddressCheck->setEnabled(false);
            
            packetCount = 0;
            updateStatus();
        } else {
            logMessage(QString("ERROR: Failed to bind to %1:%2 - %3")
                      .arg(bindAddress.toString())
                      .arg(port)
                      .arg(udpSocket->errorString()));
        }
    }
    
    void stopListening() {
        disconnect(udpSocket, &QUdpSocket::readyRead, this, &RawUDPListener::processPendingDatagrams);
        udpSocket->close();
        
        logMessage("Stopped listening");
        
        startButton->setEnabled(true);
        stopButton->setEnabled(false);
        interfaceCombo->setEnabled(true);
        portEdit->setEnabled(true);
        allowBroadcastCheck->setEnabled(true);
        reuseAddressCheck->setEnabled(true);
        shareAddressCheck->setEnabled(true);
    }
    
    void processPendingDatagrams() {
        while (udpSocket->hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(udpSocket->pendingDatagramSize());
            QHostAddress sender;
            quint16 senderPort;
            
            udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
            
            packetCount++;
            
            // Log packet info
            logMessage(QString("Received UDP packet from %1:%2 (%3 bytes)")
                      .arg(sender.toString())
                      .arg(senderPort)
                      .arg(datagram.size()));
            
            // Display packet data in various formats
            logMessage("Raw data (ASCII): " + QString::fromUtf8(datagram));
            logMessage("Raw data (Hex): " + datagram.toHex(' '));
            
            // Check if this looks like a telescope broadcast
            QString datagramStr = QString::fromUtf8(datagram);
            if (datagramStr.contains("Origin", Qt::CaseInsensitive)) {
                logMessage("*** FOUND CELESTRON ORIGIN TELESCOPE BROADCAST ***");
            }
            
            logMessage(""); // Empty line for separation
        }
    }
    
    void clearLog() {
        logText->clear();
    }
    
    void updateStatus() {
        packetCountLabel->setText(QString("Packets received: %1").arg(packetCount));
    }
    
    void logMessage(const QString &message) {
        logText->append(message);
        // Auto-scroll to bottom
        QTextCursor cursor = logText->textCursor();
        cursor.movePosition(QTextCursor::End);
        logText->setTextCursor(cursor);
    }
    
private:
    QComboBox *interfaceCombo;
    QCheckBox *allowBroadcastCheck;
    QCheckBox *reuseAddressCheck;
    QCheckBox *shareAddressCheck;
    QLineEdit *portEdit;
    QPushButton *startButton;
    QPushButton *stopButton;
    QPushButton *clearButton;
    QTextEdit *logText;
    QUdpSocket *udpSocket;
    QStatusBar *statusBar;
    QLabel *packetCountLabel;
    int packetCount = 0;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    
    RawUDPListener listener;
    listener.show();
    
    return app.exec();
}

#include "main.moc"
