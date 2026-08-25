#-------------------------------------------------
# lfp_build - generate .lfp sidecars for an archive
#-------------------------------------------------
QT       += core gui widgets
TARGET   = lfp_build
TEMPLATE = app
CONFIG  += c++11 console

QWT_INCLUDE = $$(QWT_INCLUDE)
isEmpty(QWT_INCLUDE): QWT_INCLUDE = /usr/include/qwt
INCLUDEPATH += $$QWT_INCLUDE $$PWD/src

LIBS += -lqwt-qt5 -lfftw3 -lm

SOURCES += \
    tools/lfp_build.cpp \
    src/common.cpp \
    src/igmath.cpp \
    src/iganalytics.cpp \
    src/lfs_header.cpp \
    src/lfpfile.cpp \
    src/qigcolormap.cpp \
    src/qigcolormap_tables.cpp \
    src/rasterdata.cpp \
    src/qrxionogram.cpp

HEADERS += \
    src/common.h \
    src/igmath.h \
    src/iganalytics.h \
    src/lfs_header.h \
    src/lfpfile.h \
    src/qigcolormap.h \
    src/rasterdata.h \
    src/qrxionogram.h
