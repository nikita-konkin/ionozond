/*
 * dsChirp_viewer -- open one or more .lfs captures in a window.
 *
 * A small harness for looking at archive data without the scheduler, the
 * sounder process, or any of the live machinery. Useful for checking the
 * reconstruction's rendering against the manual, and for browsing an archive.
 *
 *   dsChirp_viewer [file.lfs ...]
 *
 * File > Open, or the << >> buttons to step through the captures found next
 * to the one that was opened.
 */
#include "../src/common.h"
#include "../src/qrxionogram.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

class ViewerWindow : public QMainWindow
{
    Q_OBJECT
public:
    ViewerWindow()
        : m_plot(0), m_index(-1)
    {
        setWindowTitle(QString::fromUtf8("dsChirp — просмотр ионограмм"));
        resize(1100, 700);

        QBaseSoundParams base;
        base.fftCountIndex = 5;         /* 16384 */
        base.sampleRateIndex = 3;       /* 25000 kHz */
        base.igColormapIndex = 1;
        base.colormapGradient = true;
        base.igVerticalScaleIndex = 1;

        m_plot = new QRxIonogram(base, QTxParams(), QRxParams(), this);
        setCentralWidget(m_plot);

        QToolBar *bar = addToolBar(QString::fromUtf8("Файл"));

        QPushButton *open = new QPushButton(QString::fromUtf8("Открыть..."), bar);
        connect(open, SIGNAL(clicked()), this, SLOT(openFile()));
        bar->addWidget(open);

        bar->addSeparator();
        QPushButton *prev = new QPushButton(QLatin1String("<<"), bar);
        connect(prev, SIGNAL(clicked()), this, SLOT(previous()));
        bar->addWidget(prev);

        m_combo = new QComboBox(bar);
        m_combo->setMinimumWidth(340);
        connect(m_combo, SIGNAL(activated(int)), this, SLOT(selectIndex(int)));
        bar->addWidget(m_combo);

        QPushButton *next = new QPushButton(QLatin1String(">>"), bar);
        connect(next, SIGNAL(clicked()), this, SLOT(next()));
        bar->addWidget(next);

        /* Colour map selector, so the recovered maps can be compared. */
        bar->addSeparator();
        bar->addWidget(new QLabel(QString::fromUtf8(" Цвета: "), bar));
        m_cmap = new QComboBox(bar);
        for (int i = 0; i < 9; ++i)   /* IG_COLORMAP_LIST has 9 entries */
            m_cmap->addItem(QString::number(i));
        m_cmap->setCurrentIndex(1);
        connect(m_cmap, SIGNAL(activated(int)), this, SLOT(setColorMap(int)));
        bar->addWidget(m_cmap);

        statusBar()->showMessage(QString::fromUtf8("Откройте .lfs файл"));
    }

    void load(const QStringList &files)
    {
        m_files = files;
        m_combo->clear();
        for (int i = 0; i < m_files.size(); ++i)
            m_combo->addItem(QFileInfo(m_files.at(i)).fileName());
        if (!m_files.isEmpty())
            selectIndex(0);
    }

public slots:
    void openFile()
    {
        const QString f = QFileDialog::getOpenFileName(
            this, QString::fromUtf8("Открыть ионограмму"), QString(),
            QLatin1String("LFS captures (*.lfs);;All files (*)"));
        if (f.isEmpty())
            return;

        /* Pull in every capture sitting alongside it, for stepping. */
        QDir dir = QFileInfo(f).absoluteDir();
        QStringList siblings;
        const QStringList names = dir.entryList(QStringList() << QLatin1String("*.lfs"),
                                                QDir::Files, QDir::Name);
        for (int i = 0; i < names.size(); ++i)
            siblings << dir.filePath(names.at(i));

        load(siblings);
        selectIndex(siblings.indexOf(QFileInfo(f).absoluteFilePath()));
    }

    void selectIndex(int i)
    {
        if (i < 0 || i >= m_files.size())
            return;
        m_index = i;
        m_combo->setCurrentIndex(i);

        const QString path = m_files.at(i);
        statusBar()->showMessage(QString::fromUtf8("Загрузка %1 ...").arg(path));
        QApplication::processEvents();

        if (m_plot->loadLfsData(path)) {
            statusBar()->showMessage(
                QString::fromUtf8("%1    f %2..%3 МГц    t %4..%5 мс")
                    .arg(QFileInfo(path).fileName())
                    .arg(m_plot->horizontalInterval().left(), 0, 'f', 2)
                    .arg(m_plot->horizontalInterval().right(), 0, 'f', 2)
                    .arg(m_plot->verticalInterval().top(), 0, 'f', 2)
                    .arg(m_plot->verticalInterval().bottom(), 0, 'f', 2));
        } else {
            statusBar()->showMessage(
                QString::fromUtf8("Не удалось прочитать %1").arg(path));
        }
    }

    void previous() { if (m_index > 0) selectIndex(m_index - 1); }
    void next()     { if (m_index + 1 < m_files.size()) selectIndex(m_index + 1); }

    void setColorMap(int index)
    {
        m_plot->setColorMapIndex(index);
        if (m_index >= 0)
            selectIndex(m_index);
    }

private:
    QRxIonogram *m_plot;
    QComboBox   *m_combo;
    QComboBox   *m_cmap;
    QStringList  m_files;
    int          m_index;
};

#include "viewer_main.moc"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    ViewerWindow w;
    QStringList files;
    for (int i = 1; i < argc; ++i)
        files << QString::fromLocal8Bit(argv[i]);

    w.show();
    if (!files.isEmpty())
        w.load(files);

    return app.exec();
}
