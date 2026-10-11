######################################################################
# OriginWatch: read-only GUI that watches an OriginHeadless daemon (status, log, live stack preview, finished stacks)
#   mkdir -p build/watch && cd build/watch && qmake6 ../../OriginWatch.pro && make
# It sends the daemon only read-only commands (a whitelist in WatchMain.cpp) and reads the data directory.
######################################################################
TEMPLATE = app
TARGET = OriginWatch
CONFIG += c++17
QT += core gui widgets network
INCLUDEPATH += .
DESTDIR = build
OBJECTS_DIR = build/obj
MOC_DIR = build/moc
SOURCES += WatchMain.cpp
HEADERS += DataDir.hpp
