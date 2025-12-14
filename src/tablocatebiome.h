#ifndef TABLOCATEBIOME_H
#define TABLOCATEBIOME_H

#include <QWidget>
#include <QThread>
#include <QTreeWidgetItem>

#include "mainwindow.h"
#include "util.h"

namespace Ui {
class TabLocateBiome;
}

class AnalysisLocateBiome : public QThread
{
    Q_OBJECT
public:
    explicit AnalysisLocateBiome(QObject *parent = nullptr)
        : QThread(parent), idx() {}

    virtual void run() override;
    void runLocateFarm(Generator *g);

signals:
    void seedFarmItem(QTreeWidgetItem *item);

public:
    std::vector<uint64_t> seeds;
    WorldInfo wi;
    std::atomic_bool stop;
    std::atomic_long idx;
    struct Dat {
        int x1, z1, x2, z2;
        int scale;
        int locate;
    } dat;
    int minsize;
};

class TabLocateBiome : public QWidget, public ISaveTab
{
    Q_OBJECT

public:
    explicit TabLocateBiome(MainWindow *parent = nullptr);
    ~TabLocateBiome();

    virtual bool event(QEvent *e) override;

    virtual void save(QSettings& settings) override;
    virtual void load(QSettings& settings) override;
    virtual void refresh() override;

    void refreshBiomes(int activeid = -1);

private slots:
    void onAnalysisSeedFarmItem(QTreeWidgetItem *item);
    void onAnalysisFinished();
    void onBufferTimeout();

    void on_pushStart_clicked();
    void on_pushExport_clicked();
    void on_buttonFromVisible_clicked();
    void on_lineFarmSize_textChanged(const QString &arg1);
    void on_lineFarmSize_editingFinished();
    void on_lineFarmPercent_editingFinished();
    void on_treeLocateFarm_itemClicked(QTreeWidgetItem *item, int column);
    
    void on_lineX1_editingFinished();
    void on_lineZ1_editingFinished();
    void on_lineX2_editingFinished();
    void on_lineZ2_editingFinished();
    void on_comboSeedSource_currentIndexChanged(int index);

private:
    void exportResults(QTextStream& stream);

private:
    Ui::TabLocateBiome *ui;
    MainWindow *parent;
    AnalysisLocateBiome thread;
    QMap<QString, int> str2biome;
    AnalysisLocateBiome::Dat datl;

    QElapsedTimer elapsed;
    uint64_t updt;
    uint64_t nextupdate;
    QList<QTreeWidgetItem*> qbuflf;
    std::vector<Shape> currentShapes;
};

#endif // TABLOCATEBIOME_H

