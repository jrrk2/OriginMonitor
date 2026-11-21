# Gaia colour calibration of saved live stacks (tools/StackColour.cpp), used by tools/stack_colour.py
#   mkdir -p build/stackcolour && cd build/stackcolour && qmake6 ../../tools/StackColour.pro && make
TEMPLATE = app
TARGET = StackColour
CONFIG += c++17 console
CONFIG -= app_bundle
QT = core
DEFINES += ORIGIN_NO_GUI
INCLUDEPATH += $$PWD/..
SOURCES += StackColour.cpp ../LiveStacker.cpp ../Registration.cpp ../Image16.cpp ../SkyFrame.cpp
HEADERS += ../LiveStacker.hpp ../GaiaCatalog.hpp
