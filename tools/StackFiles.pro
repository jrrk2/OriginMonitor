# Offline driver for the live pipeline (tools/StackFiles.cpp), used by tools/restack.py
#   mkdir -p build/stackfiles && cd build/stackfiles && qmake6 ../../tools/StackFiles.pro && make
TEMPLATE = app
TARGET = StackFiles
CONFIG += c++17 console
CONFIG -= app_bundle
QT = core
DEFINES += ORIGIN_NO_GUI
INCLUDEPATH += $$PWD/..
LIBS += -ljpeg
SOURCES += StackFiles.cpp ../SkyMonitor.cpp ../FocusAnalyzer.cpp ../SkyFrame.cpp ../Registration.cpp ../LiveStacker.cpp ../Image16.cpp ../Astro.cpp ../Ephemeris.cpp

# Sun and Moon from JPL DE440 through CSPICE when available, as OriginHeadless.pro
CSPICE_DIR = $$(ORIGIN_CSPICE)
isEmpty(CSPICE_DIR): exists($$PWD/../build/cspice/cspice/lib/cspice.a): CSPICE_DIR = $$PWD/../build/cspice/cspice
!isEmpty(CSPICE_DIR) {
    DEFINES += ORIGIN_SPICE
    INCLUDEPATH += $$CSPICE_DIR/include
    LIBS += $$CSPICE_DIR/lib/cspice.a -lm
}
