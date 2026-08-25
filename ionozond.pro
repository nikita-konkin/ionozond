#-------------------------------------------------
# ionozond - the ionosonde viewer
#
# Original build: Qt 5.9.1 + Qwt 6 + FFTW3, GCC 5.4 / Ubuntu 16.04.
# This tree targets Qt 5.15 + Qwt 6.1 on Linux; see NOTES.md.
#-------------------------------------------------

QT       += core gui widgets

TARGET   = ionozond
TEMPLATE = app
CONFIG  += c++11

# Qwt: Debian/Ubuntu install headers in /usr/include/qwt and name the
# library libqwt-qt5. Override on the qmake command line if yours differs.
QWT_INCLUDE = $$(QWT_INCLUDE)
isEmpty(QWT_INCLUDE): QWT_INCLUDE = /usr/include/qwt
INCLUDEPATH += $$QWT_INCLUDE

# uic writes #include "common.h" for the promoted DigitalClock widget
INCLUDEPATH += $$PWD/src

LIBS += -lqwt-qt5 -lfftw3 -lm

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    src/main.cpp \
    src/common.cpp \
    src/igmath.cpp \
    src/lfs_header.cpp \
    src/configwriter.cpp \
    src/schedule.cpp \
    src/qigcolormap.cpp \
    src/qigcolormap_tables.cpp \
    src/rasterdata.cpp \
    src/lfpfile.cpp \
    src/qrxionogram.cpp \
    src/iganalytics.cpp \
    src/datetimescaledraw.cpp \
    src/snrvariationswidget.cpp \
    src/pdpvariationswidget.cpp \
    src/qigframe.cpp \
    src/qcpuusagewidget.cpp \
    src/qdrivepiechart.cpp \
    src/parametersdialog.cpp \
    src/scheduledialog.cpp \
    src/frmmain.cpp

HEADERS += \
    src/common.h \
    src/igmath.h \
    src/lfs_header.h \
    src/configwriter.h \
    src/schedule.h \
    src/qigcolormap.h \
    src/rasterdata.h \
    src/lfpfile.h \
    src/qrxionogram.h \
    src/iganalytics.h \
    src/datetimescaledraw.h \
    src/snrvariationswidget.h \
    src/pdpvariationswidget.h \
    src/qigframe.h \
    src/qcpuusagewidget.h \
    src/qdrivepiechart.h \
    src/parametersdialog.h \
    src/scheduledialog.h \
    src/frmmain.h

FORMS += \
    src/frmmain.ui \
    src/parametersdialog.ui \
    src/scheduledialog.ui

RESOURCES += ionozond.qrc
