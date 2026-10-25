######################################################################
# OriginWatch: GUI that watches an OriginHeadless daemon (status, log, live stack preview, finished stacks) and, in its Tasks tab, runs the common tasks
#   mkdir -p build/watch && cd build/watch && qmake6 ../../OriginWatch.pro && make
# Its socket client sends the daemon only read-only commands (a whitelist in WatchMain.cpp); the Tasks tab (WatchTasks.hpp) launches OriginHeadless instead.
# With --web-port it also serves the same information to a phone over HTTP(S) (WatchWeb.hpp); see the README.
######################################################################
TEMPLATE = app
TARGET = OriginWatch
CONFIG += c++17
QT += core gui widgets network
INCLUDEPATH += .
LIBS += -ljpeg
DESTDIR = build
OBJECTS_DIR = build/obj
MOC_DIR = build/moc
SOURCES += WatchMain.cpp Weather.cpp Astro.cpp Ephemeris.cpp
HEADERS += DataDir.hpp Weather.hpp WatchWeb.hpp WatchTasks.hpp Astro.hpp Ephemeris.hpp AllSkyStars.hpp

# Sun altitude for the forecast tab (daylight rows, dark hours per night). As in OriginHeadless.pro: JPL DE440 through CSPICE when a CSPICE tree is found,
# else Astro.cpp's low-precision series (a few arcminutes for the Sun, plenty here).
CSPICE_DIR = $$(ORIGIN_CSPICE)
isEmpty(CSPICE_DIR): exists($$PWD/build/cspice/cspice/lib/cspice.a): CSPICE_DIR = $$PWD/build/cspice/cspice
!isEmpty(CSPICE_DIR) {
    DEFINES += ORIGIN_SPICE
    INCLUDEPATH += $$CSPICE_DIR/include
    LIBS += $$CSPICE_DIR/lib/cspice.a -lm
}
