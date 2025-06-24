######################################################################
# Celestron Origin Monitor .pro file for macOS/XCode with Alpaca Server
######################################################################

# Project configuration
VERSION = 1.0.0
TEMPLATE = app
TARGET = CelestronOriginMonitor
DESTDIR = build
OBJECTS_DIR = build/obj
MOC_DIR = build/moc
RCC_DIR = build/rcc
UI_DIR = build/ui

# macOS specific settings
CONFIG += app_bundle
QMAKE_MACOSX_DEPLOYMENT_TARGET = 13.0  # Updated to match your existing setting
QMAKE_INFO_PLIST = Info.plist

# Include path
INCLUDEPATH += .

# Enable modern C++ features
CONFIG += c++17

# Qt modules - ADDED httpserver for Alpaca support
QT += core widgets network websockets httpserver

# Original source files
SOURCES += \
    main.cpp \
    TelescopeDataProcessor.cpp \
    CommandInterface.cpp \
    TelescopeGUI.cpp

# NEW: Add Alpaca server sources
SOURCES += \
    OriginBackend.cpp \
    AlpacaServer.cpp

# Original header files
HEADERS += \
    TelescopeData.hpp \
    TelescopeDataProcessor.hpp \
    CommandInterface.hpp \
    TelescopeGUI.hpp

# NEW: Add Alpaca server headers  
HEADERS += \
    OriginBackend.hpp \
    AlpacaServer.hpp

# Optional: Add AutoDownloader if you want download functionality in Alpaca
# SOURCES += AutoDownloader.cpp
# HEADERS += AutoDownloader.hpp

# Default rules
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# macOS icon (comment out if app.icns doesn't exist)
# ICON = app.icns

# Compiler and linker flags for macOS
macx {
    QMAKE_CXXFLAGS += -Werror=return-type
    QMAKE_LFLAGS += -Wl,-rpath,@executable_path/../Frameworks
    
    # For XCode build
    CONFIG += debug_and_release build_all relative_qt_rpath
    
    # Bundle identifier
    QMAKE_TARGET_BUNDLE_PREFIX = com.yourdomain
    QMAKE_BUNDLE = CelestronOriginMonitor
    
    # Deployment target
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 13.0
    QMAKE_LFLAGS += -Wl,-rpath,@executable_path/../Frameworks
}

# Post-build deployment
macx {
    # Use QMAKE_BUNDLE_DATA instead of QMAKE_POST_LINK for Info.plist modifications
    QMAKE_INFO_PLIST = Info.plist
    
    # This creates a script that will run after the app bundle is created
    deploy.commands = /usr/libexec/PlistBuddy -c \"Set :CFBundleShortVersionString $$VERSION\" $$DESTDIR/$${TARGET}.app/Contents/Info.plist
    
    # Make sure this runs after the target is built
    deploy.depends = $(TARGET)
    
    # Add the deploy step to your build process
    QMAKE_EXTRA_TARGETS += deploy
    POST_TARGETDEPS += deploy
}

# NEW: Optional configuration flags for Alpaca features
# Uncomment these if you want to enable specific features

# Enable comprehensive logging
# DEFINES += ALPACA_DEBUG_LOGGING

# Enable auto-discovery broadcast
# DEFINES += ALPACA_DISCOVERY_ENABLED

# Enable image download support
# DEFINES += ALPACA_IMAGE_SUPPORT

# Set default Alpaca port (can be overridden at runtime)
# DEFINES += ALPACA_DEFAULT_PORT=11111