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
CONFIG += app_bundle
QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.14
QMAKE_INFO_PLIST = Info.plist

# Include path
INCLUDEPATH += .

# Enable modern C++ features
CONFIG += c++17

# Qt modules
QT += core widgets network websockets

# Source files
SOURCES += \
    main.cpp \
    TelescopeDataProcessor.cpp \
    CommandInterface.cpp \
    TelescopeGUI.cpp

# Header files
HEADERS += \
    TelescopeData.hpp \
    TelescopeDataProcessor.hpp \
    CommandInterface.hpp \
    TelescopeGUI.hpp

# Default rules
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# macOS icon
ICON = app.icns

# Compiler and linker flags for macOS
macx {
    QMAKE_CXXFLAGS += -Werror=return-type
    QMAKE_LFLAGS += -Wl,-rpath,@executable_path/../Frameworks
    
    # For XCode build
    CONFIG += debug_and_release build_all
    
    # Bundle identifier
    QMAKE_TARGET_BUNDLE_PREFIX = com.yourdomain
    QMAKE_BUNDLE = CelestronOriginMonitor

    # You can add custom Info.plist settings here
    # QMAKE_INFO_PLIST = path/to/Info.plist
}

# And replace your post-link command with this
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
