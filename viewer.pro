#-------------------------------------------------
# dsChirp_viewer - standalone .lfs capture viewer
#
# A cut-down harness: just the ionogram panel, no scheduler and no sounder
# process. Build alongside the main app:
#     qmake viewer.pro && make
#-------------------------------------------------

QT       += core gui widgets

TARGET   = dsChirp_viewer
TEMPLATE = app
CONFIG  += c++11

QWT_INCLUDE = $$(QWT_INCLUDE)
isEmpty(QWT_INCLUDE): QWT_INCLUDE = /usr/include/qwt
INCLUDEPATH += $$QWT_INCLUDE $$PWD/src

LIBS += -lqwt-qt5 -lfftw3 -lm

SOURCES += \
    tools/viewer_main.cpp \
    src/common.cpp \
    src/igmath.cpp \
    src/iganalytics.cpp \
    src/lfs_header.cpp \
    src/qigcolormap.cpp \
    src/qigcolormap_tables.cpp \
    src/rasterdata.cpp \
    src/lfpfile.cpp \
    src/qrxionogram.cpp

HEADERS += \
    src/common.h \
    src/igmath.h \
    src/iganalytics.h \
    src/lfs_header.h \
    src/qigcolormap.h \
    src/rasterdata.h \
    src/lfpfile.h \
    src/qrxionogram.h
