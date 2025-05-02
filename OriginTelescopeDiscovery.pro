QT += core gui network websockets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = OriginTelescopeDiscovery
TEMPLATE = app

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp

# macOS specific configurations
macx {
    QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.14
    
    # Include macOS specific frameworks if needed
    # LIBS += -framework CoreFoundation
    
    # Set the icon for the application
    # ICON = icon.icns
    
    # Bundle identifier
    QMAKE_TARGET_BUNDLE_PREFIX = com.yourcompany
    QMAKE_BUNDLE = OriginTelescopeDiscovery
    
    # Info.plist settings
    QMAKE_INFO_PLIST = Info.plist
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
