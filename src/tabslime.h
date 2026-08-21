#ifndef TABSLIME_H
#define TABSLIME_H

#include <QWidget>
#include <QThread>
#include <QTreeWidgetItem>

#include "mainwindow.h"
#include "util.h"

namespace Ui {
class TabSlime;
}

struct SlimeResult
{
    int centerX, centerZ;  // 挂机点坐标（16的倍数）
    int area;               // 有效史莱姆区块刷怪面积
};

class AnalysisSlime : public QThread
{
    Q_OBJECT
public:
    explicit AnalysisSlime(QObject *parent = nullptr)
        : QThread(parent), idx() {}

    virtual void run() override;

signals:
    void seedDone(uint64_t seed, QList<SlimeResult> results);

public:
    std::vector<uint64_t> seeds;
    WorldInfo wi;
    std::atomic_bool stop;
    std::atomic_long idx;
    int x1, z1, x2, z2;     // 挂机位置坐标范围
    int minArea;            // 最小面积阈值
    bool includeBiome;      // 是否包含群系计算
};

class TabSlime : public QWidget, public ISaveTab
{
    Q_OBJECT

public:
    explicit TabSlime(MainWindow *parent = nullptr);
    ~TabSlime();

    virtual bool event(QEvent *e) override;

    virtual void save(QSettings& settings) override;
    virtual void load(QSettings& settings) override;
    virtual void refresh() override;

private slots:
    void onAnalysisSeedDone(uint64_t seed, QList<SlimeResult> results);
    void onAnalysisFinished();
    void onBufferTimeout();

    void on_pushStart_clicked();
    void on_pushExport_clicked();
    void on_buttonFromVisible_clicked();
    void on_treeResults_itemClicked(QTreeWidgetItem *item, int column);
    
    void on_lineX1_editingFinished();
    void on_lineZ1_editingFinished();
    void on_lineX2_editingFinished();
    void on_lineZ2_editingFinished();
    void on_comboSeedSource_currentIndexChanged(int index);

private:
    void exportResults(QTextStream& stream);

private:
    Ui::TabSlime *ui;
    MainWindow *parent;
    AnalysisSlime thread;
    
    QElapsedTimer elapsed;
    uint64_t updt;
    uint64_t nextupdate;
    QList<QTreeWidgetItem*> qbufl;
};

#endif // TABSLIME_H

