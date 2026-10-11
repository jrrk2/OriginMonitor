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
LIBS += -ljpeg
DESTDIR = build
OBJECTS_DIR = build/obj
MOC_DIR = build/moc

SOURCES += \
    HeadlessMain.cpp \
    Planner.cpp \
    Ephemeris.cpp \
    Weather.cpp \
    Mail.cpp \
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
    FlatAnalyzer.cpp \
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

# High-accuracy Sun and Moon (JPL DE440 through CSPICE). Optional: without it Astro.cpp falls back to a low-precision series.
# Point at a CSPICE tree with ORIGIN_CSPICE=/path/to/cspice (needs include/SpiceUsr.h and lib/cspice.a); build/cspice/cspice is used if present
# (extract NAIF's cspice.tar.Z in build/cspice). The kernels are found at run time, see Ephemeris.hpp.
CSPICE_DIR = $$(ORIGIN_CSPICE)
isEmpty(CSPICE_DIR): exists($$PWD/build/cspice/cspice/lib/cspice.a): CSPICE_DIR = $$PWD/build/cspice/cspice
!isEmpty(CSPICE_DIR) {
    DEFINES += ORIGIN_SPICE
    INCLUDEPATH += $$CSPICE_DIR/include
    LIBS += $$CSPICE_DIR/lib/cspice.a -lm
}
