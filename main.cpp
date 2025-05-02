#include <QApplication>
#include "TelescopeGUI.hpp"

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
    QApplication app(argc, argv);
    
    // Create and show the main window
    TelescopeGUI *gui = new TelescopeGUI();
    gui->show();
    
    // Start the application event loop
    return app.exec();
}
