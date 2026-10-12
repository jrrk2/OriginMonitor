######################################################################
# Celestron Origin Monitor .pro file for macOS/XCode
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
CONFIG += app_bundle sdk_no_version_check
QMAKE_INFO_PLIST = Info.plist
# QMAKE_LFLAGS += -Wl,--allow-multiple-definition

# Include path
INCLUDEPATH += .
INCLUDEPATH += /opt/homebrew/include

# Enable modern C++ features
CONFIG += c++17
QMAKE_CXXFLAGS += -g

# Qt modules
QT += core widgets network websockets

# Original source files
SOURCES += \
    main.cpp \
    CoordinateUtils.cpp \
    TelescopeDataProcessor.cpp \
    TelescopeGUI.cpp \
    OriginBackend.cpp \
    LogReplayDialog.cpp \
    CommandInterface.cpp \
    ControlServer.cpp \
    Sequencer.cpp \
    FocusAnalyzer.cpp \
    SkyFrame.cpp \
    Registration.cpp \
    LiveStacker.cpp \
    Image16.cpp \
    SkyMonitor.cpp \
    Astro.cpp \
    Ephemeris.cpp \

# Original header files
HEADERS += \
    ControlServer.hpp \
    Sequencer.hpp \
    FocusAnalyzer.hpp \
    SkyFrame.hpp \
    Registration.hpp \
    LiveStacker.hpp \
    Image16.hpp \
    SkyMonitor.hpp \
    Astro.hpp \
    Ephemeris.hpp \
    CoordinateUtils.hpp \
    MessierCatalog.hpp \
    DataDir.hpp \
    TelescopeData.hpp \
    TelescopeDataProcessor.hpp \
    TelescopeGUI.hpp \
    OriginBackend.hpp \
    LogReplayDialog.hpp \
    CommandInterface.hpp \
    
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
    CONFIG += build_all relative_qt_rpath
    
    # Bundle identifier
    QMAKE_TARGET_BUNDLE_PREFIX = com.yourdomain
    QMAKE_BUNDLE = CelestronOriginMonitor
    
    # Deployment target
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 15.0
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
