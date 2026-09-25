#ifndef TABFIXEDSEED_H
#define TABFIXEDSEED_H

#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QProgressBar>
#include <QString>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "mainwindow.h"

#include <QThread>
#include <atomic>
#include <vector>

namespace Ui {
class TabFixedSeed;
}

enum FixedSeedMode {
    FS_WITCH_HUT = 0,
    FS_SLIME = 1,
    FS_CAVE = 2,
    FS_RIVER = 3,
    FS_MONUMENT = 4,
    FS_FORT_CROSS = 5,
    FS_FORT_SPAN = 6,
};

enum { FS_MODE_COUNT = 7 };

struct FixedSeedHit {
    uint64_t seed = 0;
    int x = 0, y = 0, z = 0;
    int a = 0, b = 0, c = 0, d = 0;
    QString extra;
};

class FixedSeedWorker : public QThread
{
    Q_OBJECT
public:
    explicit FixedSeedWorker(QObject *parent = nullptr) : QThread(parent) {}

    void run() override;

signals:
    void progress(int mode, int current, int total, int phase, int phaseCount, QString status);
    void hitFound(FixedSeedHit hit);
    void searchError(QString message);
    void finishedOk(bool completed);

public:
    FixedSeedMode mode = FS_WITCH_HUT;
    std::vector<uint64_t> seeds;
    WorldInfo wi;
    int x1 = 0, z1 = 0, x2 = 0, z2 = 0;
    int threads = 1;

    int slimeThreshold = 40;
    int slimeMinArea = 10000;
    bool slimeBiomeConv = false;

    int sampleY = -62;
    int minArea = 30000;

    bool caveFastSearch = true;
    float caveRiverFactor = 0.75f;

    bool monumentBiomeCheck = true;

    int witchMaxY = -40; // phase-2 avg_y threshold; phase-1 uses max(maxY, -50)

    int fortressMode = 0; // 0 cross, 1 span
    int crossFilter = 2;
    int minLong = 0;
    int minShort = 240;
    int minHeight = 0;

    std::atomic_bool stop{false};
    std::atomic_bool pause{false};
};

class TabFixedSeed : public QWidget, public ISaveTab
{
    Q_OBJECT
public:
    explicit TabFixedSeed(MainWindow *parent = nullptr);
    ~TabFixedSeed();

    virtual void save(QSettings& settings) override;
    virtual void load(QSettings& settings) override;
    virtual bool event(QEvent *e) override;

    bool isSearchRunning() const;
    void setExternalSearchRunning(bool running);
    void requestStop();

signals:
    void fixedSearchStatusChanged(bool running);

private slots:
    void on_buttonFromVisible_clicked();
    void on_checkSquare_toggled(bool checked);
    void on_spinSquareSide_valueChanged(int v);
    void on_comboSeedSource_currentIndexChanged(int);
    void on_tabTools_currentChanged(int index);
    void on_pushStart_clicked();
    void on_pushPause_clicked();
    void on_pushStop_clicked();
    void on_pushClear_clicked();
    void on_pushExport_clicked();
    void onWorkerProgress(int mode, int current, int total, int phase, int phaseCount, QString status);
    void onWorkerHit(FixedSeedHit hit);
    void onWorkerSearchError(const QString &message);
    void onWorkerFinished(bool completed);
    void onTreeItemClicked(QTreeWidgetItem *item, int column);
    void onTimeTick();

private:
    void updateThresholdUi();
    void updateSquareUi();
    void applySquareToCoords();
    int autoSlimeThreshold() const;
    double equivalentHalfSide() const;
    void gatherSeeds(std::vector<uint64_t> *out);
    void setControlsLocked(bool running);
    void clearResultsForMode(int mode);
    QProgressBar *progressBarForMode(int mode) const;
    QTreeWidget *treeForMode(int mode) const;
    void setProgressFormat(QProgressBar *bar, int current, int total, int phase, int phaseCount,
                           const QString &status, bool finished);
    void wrapProgressBarsWithTime();
    void resetTimeLabels(int mode);
    void updateTimeLabels();
    qint64 activeElapsedMs() const;
    static QString formatDuration(qint64 ms);
    QTreeWidgetItem *seedParentItem(QTreeWidget *tree, uint64_t seed);
    void applyTreeColumnWidths();
    void onHeaderClicked(QTreeWidget *tree, int mode, int section);
    void reorderToolTabs();
    void updateWitchMaxYOptions();
    int selectedWitchMaxY() const;

    Ui::TabFixedSeed *ui;
    MainWindow *parent;
    FixedSeedWorker worker;
    int squareCenterX = 0;
    int squareCenterZ = 0;
    bool updatingSquare = false;
    int lastPhase = 1;
    QString lastStatus;

    QLabel *labelElapsed[FS_MODE_COUNT] = {};
    QLabel *labelRemain[FS_MODE_COUNT] = {};
    QElapsedTimer searchClock;
    QElapsedTimer pauseClock;
    qint64 pausedMs = 0;
    int progMode = -1;
    int progCurrent = 0;
    int progTotal = 1;
    QTimer timeTimer;
    bool colReady[FS_MODE_COUNT] = {};
    int lastSortCol[FS_MODE_COUNT] = {-1, -1, -1, -1, -1, -1, -1};
};

#endif // TABFIXEDSEED_H
