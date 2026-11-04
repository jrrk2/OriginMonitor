# Batch plate solver with StellarSolver (tools/SolveFrames.cpp), used by tools/canvas_stack.py
#   mkdir -p build/solveframes && cd build/solveframes && qmake6 ../../tools/SolveFrames.pro && make
TEMPLATE = app
TARGET = SolveFrames
CONFIG += c++17 console
CONFIG -= app_bundle
QT = core
INCLUDEPATH += /usr/include/libstellarsolver
LIBS += -lstellarsolver6 -lcfitsio
SOURCES += SolveFrames.cpp
