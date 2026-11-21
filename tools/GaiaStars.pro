# Gaia stars with predicted camera colours (tools/GaiaStars.cpp), used by tools/montage_area.py
#   mkdir -p build/gaiastars && cd build/gaiastars && qmake6 ../../tools/GaiaStars.pro && make
TEMPLATE = app
TARGET = GaiaStars
CONFIG += c++17 console
CONFIG -= app_bundle
QT = core
INCLUDEPATH += $$PWD/..
SOURCES += GaiaStars.cpp
HEADERS += ../GaiaCatalog.hpp ../StarSpectra.hpp
