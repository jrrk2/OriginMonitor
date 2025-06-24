#include <QApplication>
#include <QDebug>
#include <QTimer>
#include "AlpacaServer.hpp"
#include "OriginBackend.hpp"

/**
 * @brief Example main function showing how to integrate the Alpaca server
 *        with the Celestron Origin telescope backend
 */
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // Create the Origin backend
    OriginBackend* originBackend = new OriginBackend();
    
    // Create the Alpaca server
    AlpacaServer* alpacaServer = new AlpacaServer();
    
    // Connect the backend to the server
    alpacaServer->setTelescopeBackend(originBackend);
    
    // Connect to backend signals for logging
    QObject::connect(originBackend, &OriginBackend::connected, []() {
        qDebug() << "Origin telescope connected";
    });
    
    QObject::connect(originBackend, &OriginBackend::disconnected, []() {
        qDebug() << "Origin telescope disconnected";
    });
    
    QObject::connect(originBackend, &OriginBackend::statusUpdated, []() {
        qDebug() << "Origin telescope status updated";
    });
    
    // Connect to server signals for logging
    QObject::connect(alpacaServer, &AlpacaServer::serverStarted, []() {
        qDebug() << "Alpaca server started successfully";
    });
    
    QObject::connect(alpacaServer, &AlpacaServer::requestReceived, 
                    [](const QString& method, const QString& path) {
        qDebug() << "Alpaca request:" << method << path;
    });
    
    // Start the Alpaca server on port 11111
    if (!alpacaServer->start(11111)) {
        qCritical() << "Failed to start Alpaca server";
        return 1;
    }
    
    qDebug() << "Celestron Origin Alpaca Server running on port 11111";
    qDebug() << "Discovery broadcasts will be sent on port 32227";
    qDebug() << "";
    qDebug() << "You can now connect ASCOM/Alpaca clients to:";
    qDebug() << "  Telescope: http://localhost:11111/api/v1/telescope/0/";
    qDebug() << "  Camera:    http://localhost:11111/api/v1/camera/0/";
    qDebug() << "";
    qDebug() << "Example endpoints:";
    qDebug() << "  GET  /api/v1/telescope/0/connected";
    qDebug() << "  PUT  /api/v1/telescope/0/connected (Connected=true)";
    qDebug() << "  GET  /api/v1/telescope/0/altitude";
    qDebug() << "  GET  /api/v1/telescope/0/azimuth";
    qDebug() << "  PUT  /api/v1/telescope/0/slewtocoordinates (RightAscension=12.5, Declination=45.0)";
    qDebug() << "  PUT  /api/v1/camera/0/startexposure (Duration=5.0)";
    qDebug() << "  GET  /api/v1/camera/0/imagearray";
    
    // Optional: Auto-connect to a known Origin telescope
    // Uncomment these lines if you know your telescope's IP address
    /*
    QTimer::singleShot(2000, [originBackend]() {
        qDebug() << "Auto-connecting to Origin telescope...";
        if (originBackend->connectToTelescope("192.168.1.100", 80)) {
            qDebug() << "Successfully connected to Origin telescope";
        } else {
            qDebug() << "Failed to connect to Origin telescope";
        }
    });
    */
    
    return app.exec();
}

/* 
 * To build this integration, you'll need to:
 * 
 * 1. Add these files to your project:
 *    - OriginBackend.hpp
 *    - OriginBackend.cpp
 *    - AlpacaServer.hpp (modified version)
 *    - AlpacaServer.cpp (modified version)
 *    - TelescopeDataProcessor.hpp (from original Origin project)
 *    - TelescopeDataProcessor.cpp (from original Origin project)
 *    - TelescopeData.hpp (from original Origin project)
 * 
 * 2. Update your CMakeLists.txt or .pro file to include:
 *    - Qt6::Core
 *    - Qt6::Network
 *    - Qt6::WebSockets
 *    - Qt6::HttpServer
 * 
 * 3. Example CMakeLists.txt entry:
 *    find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets HttpServer)
 *    target_link_libraries(your_target Qt6::Core Qt6::Network Qt6::WebSockets Qt6::HttpServer)
 * 
 * 4. Example .pro file entry:
 *    QT += core network websockets httpserver
 * 
 * 5. To test the integration:
 *    - Run the application
 *    - Connect your ASCOM/Alpaca client to localhost:11111
 *    - Set the telescope connected property to true
 *    - Try slewing to coordinates or taking exposures
 * 
 * Key Integration Points:
 * 
 * 1. The OriginBackend class translates between Alpaca API calls and 
 *    Origin's WebSocket JSON protocol
 * 
 * 2. Coordinate systems are converted between Alpaca (hours/degrees) 
 *    and Origin (radians)
 * 
 * 3. Image capture is handled through Origin's RunImaging command and
 *    subsequent HTTP download of TIFF files
 * 
 * 4. Status updates flow from Origin -> TelescopeDataProcessor -> OriginBackend -> AlpacaServer
 * 
 * 5. The discovery protocol allows Alpaca clients to automatically find your server
 */