#include <QApplication>
#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include "TelescopeGUI.hpp"
#include "OriginBackend.hpp"
#include "ControlServer.hpp"

// Headless mode: no GUI, just the backend plus the local JSON control server.
//   CelestronOriginMonitor --headless [--control-port 8765] [--connect HOST] [--control-raw]
static int runHeadless(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    quint16 port = 8765; QString host; bool raw = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--control-port" && i + 1 < args.size()) port = args[++i].toUShort();
        else if (args[i] == "--connect" && i + 1 < args.size()) host = args[++i];
        else if (args[i] == "--control-raw") raw = true;
    }
    OriginBackend backend;
    ControlServer server(&backend, raw);
    if (!server.listen(port)) { qCritical() << "cannot listen on 127.0.0.1:" << port; return 1; }
    qInfo() << "control server on 127.0.0.1:" << port << (raw ? "(raw commands enabled)" : "");
    if (!host.isEmpty())
        QTimer::singleShot(0, &app, [&]() { qInfo() << "connect" << host << backend.connectToTelescope(host); });
    return app.exec();
}

/**
 * @brief Main function for the Celestron Origin Monitor application
 * 
 * This application provides a graphical user interface for monitoring
 * and controlling Celestron Origin telescopes. It automatically discovers
 * telescopes on the network and displays their status information.
 * 
 * @param argc Command line argument count
 * @param argv Command line arguments
 * @return Application exit code
 */
int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i)
        if (QString(argv[i]) == "--headless") return runHeadless(argc, argv);
    QApplication app(argc, argv);
    QString kernelDir = "/Users/jonathan/spice_kernels/";
    QString outputDir = kernelDir; // Write output to same directory
    QString horizonsFile = kernelDir + "horizons_reference.txt";
    QString cometName = "1004054";

    // Create and show the main window
    TelescopeGUI *gui = new TelescopeGUI();
    gui->show();
    
    // Start the application event loop
    return app.exec();
}
