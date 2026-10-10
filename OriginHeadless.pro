######################################################################
# OriginHeadless: control server + CLI with no Qt GUI/Widgets at all
#   mkdir -p build/headless && cd build/headless && qmake6 ../../OriginHeadless.pro && make
######################################################################
TEMPLATE = app
TARGET = OriginHeadless
CONFIG += c++17 console
CONFIG -= app_bundle
QT = core network websockets          # QT = (not +=) drops the default gui module
DEFINES += ORIGIN_NO_GUI
INCLUDEPATH += .
QMAKE_CXXFLAGS += -g
DESTDIR = build
OBJECTS_DIR = build/obj
MOC_DIR = build/moc

SOURCES += \
    HeadlessMain.cpp \
    Planner.cpp \
    CoordinateUtils.cpp \
    TelescopeDataProcessor.cpp \
    OriginBackend.cpp \
    ControlServer.cpp \
    Sequencer.cpp \
    FocusAnalyzer.cpp \
    SkyFrame.cpp \
    Registration.cpp \
    LiveStacker.cpp \
    Image16.cpp \
    SkyMonitor.cpp \
    Astro.cpp

HEADERS += \
    Planner.hpp \
    ControlServer.hpp \
    Sequencer.hpp \
    FocusAnalyzer.hpp \
    SkyFrame.hpp \
    Registration.hpp \
    LiveStacker.hpp \
    Image16.hpp \
    SkyMonitor.hpp \
    Astro.hpp \
    CoordinateUtils.hpp \
    MessierCatalog.hpp \
    DataDir.hpp \
    TelescopeData.hpp \
    TelescopeDataProcessor.hpp \
    OriginBackend.hpp

target.path = /opt/OriginHeadless/bin
INSTALLS += target
