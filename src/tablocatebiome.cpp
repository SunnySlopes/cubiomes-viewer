#include "tablocatebiome.h"
#include "ui_tablocatebiome.h"

#include "message.h"
#include "util.h"
#include "world.h"
#include "config.h"

#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpressionValidator>
#include <QTextStream>
#include <QTimer>
#include <QApplication>
#include <QIntValidator>
#include <QDoubleValidator>
#include <QSettings>

#include "cubiomes/finders.h"
#include "cubiomes/generator.h"

#include <map>
#include <cmath>

void AnalysisLocateBiome::run()
{
    stop.store(false);

    Generator g;
    setupGenerator(&g, wi.mc, wi.large);

    for (idx = 0; idx < (long)seeds.size(); idx++)
    {
        if (stop.load()) break;
        wi.seed = seeds[idx];
        if (dat.locate >= 0)
            runLocateFarm(&g);
    }
}

static int locateBiomeFarm(Generator *g, Pos *pos, int *siz, double *percent, int nmax,
    int x1, int z1, int x2, int z2, int match, int minsiz, int y,
    volatile char *stop)
{
    const int outerRadius = 128; // 外圆半径128（方块单位）
    const int innerRadius = 24;  // 内圆半径24（方块单位）
    const int outerRadius2 = outerRadius * outerRadius;
    const int innerRadius2 = innerRadius * innerRadius;
    const double ringArea = 3.141592653589793 * (outerRadius * outerRadius - innerRadius * innerRadius);
    const int gridSize = 128; // 每128*128范围内只输出面积最大的一个
    
    // x1, z1, x2, z2 是scale=4坐标系下的坐标，需要转换为block坐标
    int blockX1 = x1 * 4;
    int blockZ1 = z1 * 4;
    int blockX2 = x2 * 4;
    int blockZ2 = z2 * 4;
    
    // 预计算整个搜索区域的1:4群系（scale=4）
    // 扩展区域以包含所有可能的128半径范围
    int precalcX1 = x1 - (outerRadius / 4 + 1);
    int precalcZ1 = z1 - (outerRadius / 4 + 1);
    int precalcX2 = x2 + (outerRadius / 4 + 1);
    int precalcZ2 = z2 + (outerRadius / 4 + 1);
    int precalcW = precalcX2 - precalcX1 + 1;
    int precalcH = precalcZ2 - precalcZ1 + 1;
    
    Range precalcRange = {4, precalcX1, precalcZ1, precalcW, precalcH, y, 1};
    int *biomeCache = allocCache(g, precalcRange);
    if (!biomeCache)
        return 0;
    
    if (genBiomes(g, biomeCache, precalcRange))
    {
        free(biomeCache);
        return 0;
    }
    
    // 找到第一个能被gridSize整除的坐标
    int startX = ((blockX1 + gridSize - 1) / gridSize) * gridSize;
    int startZ = ((blockZ1 + gridSize - 1) / gridSize) * gridSize;
    
    // 使用map存储每个128*128区域的最大面积结果
    struct Result {
        Pos pos;
        int siz;
        double percent;
    };
    std::map<std::pair<int, int>, Result> gridResults;
    
    // 遍历所有能被gridSize整除的坐标点（作为128*128区域的起始点）
    for (int gridX = startX; gridX <= blockX2 && (!stop || !*stop); gridX += gridSize)
    {
        for (int gridZ = startZ; gridZ <= blockZ2 && (!stop || !*stop); gridZ += gridSize)
        {
            if (stop && *stop)
                goto cleanup;
            
            // 在当前128*128区域内，遍历所有能被16整除的坐标点（chunk边界）
            int gridEndX = gridX + gridSize;
            int gridEndZ = gridZ + gridSize;
            int regionStartX = (gridX > blockX1) ? gridX : blockX1;
            int regionStartZ = (gridZ > blockZ1) ? gridZ : blockZ1;
            int regionEndX = (gridEndX < blockX2) ? gridEndX : blockX2;
            int regionEndZ = (gridEndZ < blockZ2) ? gridEndZ : blockZ2;
            
            // 找到第一个能被16整除的坐标
            int chunkStartX = ((regionStartX + 15) / 16) * 16;
            int chunkStartZ = ((regionStartZ + 15) / 16) * 16;
            
            Result bestResult;
            bestResult.siz = -1;
            
            // 遍历当前128*128区域内的所有chunk中心点
            for (int centerX = chunkStartX; centerX < regionEndX && (!stop || !*stop); centerX += 16)
            {
                for (int centerZ = chunkStartZ; centerZ < regionEndZ && (!stop || !*stop); centerZ += 16)
                {
                    if (stop && *stop)
                        break;
                    
                    // 快速预检查：检查中心点周围128格范围内是否有目标群系
                    // 转换为scale=4坐标
                    int centerX4 = centerX / 4;
                    int centerZ4 = centerZ / 4;
                    int checkRadius4 = (outerRadius / 4) + 1;
                    int checkRadius24 = checkRadius4 * checkRadius4; // 半径的平方（scale=4坐标系）
                    int checkMinX4 = centerX4 - checkRadius4;
                    int checkMaxX4 = centerX4 + checkRadius4;
                    int checkMinZ4 = centerZ4 - checkRadius4;
                    int checkMaxZ4 = centerZ4 + checkRadius4;
                    
                    // 限制在预计算区域内
                    if (checkMinX4 < precalcX1) checkMinX4 = precalcX1;
                    if (checkMaxX4 > precalcX2) checkMaxX4 = precalcX2;
                    if (checkMinZ4 < precalcZ1) checkMinZ4 = precalcZ1;
                    if (checkMaxZ4 > precalcZ2) checkMaxZ4 = precalcZ2;
                    
                    // 快速检查：是否有目标群系（在外圆范围内）
                    bool hasTargetBiome = false;
                    for (int pz4 = checkMinZ4; pz4 <= checkMaxZ4 && !hasTargetBiome; pz4++)
                    {
                        for (int px4 = checkMinX4; px4 <= checkMaxX4 && !hasTargetBiome; px4++)
                        {
                            // 计算到中心的距离（scale=4坐标系）
                            int dx4 = px4 - centerX4;
                            int dz4 = pz4 - centerZ4;
                            int dist24 = dx4 * dx4 + dz4 * dz4;
                            
                            // 只检查在外圆范围内的点
                            if (dist24 <= checkRadius24)
                            {
                                int idx = (pz4 - precalcZ1) * precalcW + (px4 - precalcX1);
                                if (idx >= 0 && idx < precalcW * precalcH)
                                {
                                    int id = biomeCache[idx] & 0xff;
                                    if (id == match)
                                    {
                                        hasTargetBiome = true;
                                    }
                                }
                            }
                        }
                    }
                    
                    // 如果没有目标群系，跳过详细计算
                    if (!hasTargetBiome)
                        continue;
                    
                    // 计算以centerX, centerZ为中心，半径24-128的环形区域内目标群系的总面积
                    uint64_t totalArea = 0;
                    
                    // 遍历外圆内的所有4x4块（scale=4）
                    int minX = (centerX - outerRadius) / 4;
                    int maxX = (centerX + outerRadius) / 4;
                    int minZ = (centerZ - outerRadius) / 4;
                    int maxZ = (centerZ + outerRadius) / 4;
                    
                    // 限制在搜索区域内
                    if (minX < x1) minX = x1;
                    if (maxX > x2) maxX = x2;
                    if (minZ < z1) minZ = z1;
                    if (maxZ > z2) maxZ = z2;
                    
                    for (int pz = minZ; pz <= maxZ; pz++)
                    {
                        for (int px = minX; px <= maxX; px++)
                        {
                            if (stop && *stop)
                                break;
                            
                            // 计算该4x4块的中心坐标（block坐标）
                            int blockX = px * 4 + 2;
                            int blockZ = pz * 4 + 2;
                            
                            // 计算到圆心的距离
                            int dx = blockX - centerX;
                            int dz = blockZ - centerZ;
                            int dist2 = dx * dx + dz * dz;
                            
                            // 如果点在环形区域内（在外圆内但不在内圆内）
                            if (dist2 <= outerRadius2 && dist2 > innerRadius2)
                            {
                                // 从缓存中读取群系
                                int cacheIdx = (pz - precalcZ1) * precalcW + (px - precalcX1);
                                if (cacheIdx >= 0 && cacheIdx < precalcW * precalcH)
                                {
                                    int id = biomeCache[cacheIdx] & 0xff;
                                    if (id == match)
                                    {
                                        totalArea += 16; // 每个4x4块代表16个方块
                                    }
                                }
                            }
                        }
                    }
                    
                    // 如果总面积超过阈值，且是当前128*128区域内最大的，更新最佳结果
                    if (totalArea >= (uint64_t)minsiz && (int)totalArea > bestResult.siz)
                    {
                        bestResult.pos.x = centerX;
                        bestResult.pos.z = centerZ;
                        bestResult.siz = (int)totalArea;
                        bestResult.percent = (totalArea / ringArea) * 100.0;
                    }
                }
            }
            
            // 如果找到了有效结果，存储到map中
            if (bestResult.siz >= minsiz)
            {
                std::pair<int, int> gridKey(gridX / gridSize, gridZ / gridSize);
                gridResults[gridKey] = bestResult;
            }
        }
    }
    
cleanup:
    // 释放缓存
    free(biomeCache);
    
    // 将map中的结果转换为数组
    int n = 0;
    for (const auto& pair : gridResults)
    {
        if (n >= nmax)
            break;
        pos[n] = pair.second.pos;
        if (siz) siz[n] = pair.second.siz;
        if (percent) percent[n] = pair.second.percent;
        n++;
    }
    
    return n;
}

void AnalysisLocateBiome::runLocateFarm(Generator *g)
{
    applySeed(g, DIM_OVERWORLD, wi.seed);
    enum { MAX_LOCATE = 4096 };
    Pos allPos[MAX_LOCATE];
    int allSiz[MAX_LOCATE];
    double allPercent[MAX_LOCATE];
    int totalN = 0;
    
    // 计算区域大小（scale=4坐标系）
    int64_t sx = dat.x2 - dat.x1 + 1;
    int64_t sz = dat.z2 - dat.z1 + 1;
    
    // INT_MAX限制，需要分块处理
    const int MAX_CHUNK_SIZE = INT_MAX / 2; // 使用一半INT_MAX作为块大小，避免溢出
    
    // 创建一个volatile char变量来传递stop标志
    volatile char stopFlag = 0;
    
    // 分块处理大区域
    int stepX = (sx > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : (int)sx;
    int stepZ = (sz > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : (int)sz;
    
    for (int64_t z = dat.z1; z <= dat.z2 && totalN < MAX_LOCATE && !stop.load(); z += stepZ)
    {
        for (int64_t x = dat.x1; x <= dat.x2 && totalN < MAX_LOCATE && !stop.load(); x += stepX)
        {
            // 更新stopFlag
            stopFlag = stop.load() ? 1 : 0;
            
            int64_t endX = x + stepX - 1;
            int64_t endZ = z + stepZ - 1;
            if (endX > dat.x2) endX = dat.x2;
            if (endZ > dat.z2) endZ = dat.z2;
            
            Pos chunkPos[MAX_LOCATE];
            int chunkSiz[MAX_LOCATE];
            double chunkPercent[MAX_LOCATE];
            int chunkN = locateBiomeFarm(
                g, chunkPos, chunkSiz, chunkPercent, MAX_LOCATE - totalN,
                (int)x, (int)z, (int)endX, (int)endZ,
                dat.locate, minsize, wi.y>>2,
                &stopFlag
            );
            
            // 合并结果
            for (int i = 0; i < chunkN && totalN < MAX_LOCATE; i++)
            {
                allPos[totalN] = chunkPos[i];
                allSiz[totalN] = chunkSiz[i];
                allPercent[totalN] = chunkPercent[i];
                totalN++;
            }
        }
    }
    
    if (totalN && !stop.load())
    {
        QTreeWidgetItem *seeditem = new QTreeWidgetItem();
        seeditem->setData(0, Qt::DisplayRole, QVariant::fromValue((qlonglong)wi.seed));
        seeditem->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
        seeditem->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_OVERWORLD));
        for (int i = 0; i < totalN; i++)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem(seeditem);
            item->setText(0, "-");
            item->setData(1, Qt::DisplayRole, QVariant::fromValue(allSiz[i]));
            item->setData(2, Qt::DisplayRole, QVariant::fromValue(allPos[i].x));
            item->setData(3, Qt::DisplayRole, QVariant::fromValue(allPos[i].z));
            item->setData(4, Qt::DisplayRole, QVariant::fromValue(QString::number(allPercent[i], 'f', 2)));
            item->setData(0, Qt::UserRole+0, QVariant::fromValue(wi.seed));
            item->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_OVERWORLD));
            item->setData(0, Qt::UserRole+2, QVariant::fromValue(allPos[i]));
        }
        emit seedFarmItem(seeditem);
    }
}

TabLocateBiome::TabLocateBiome(MainWindow *parent)
    : QWidget(parent)
    , ui(new Ui::TabLocateBiome)
    , parent(parent)
    , thread()
    , elapsed()
    , updt(20)
    , nextupdate()
{
    ui->setupUi(this);

    QIntValidator *intval = new QIntValidator(-60e6, 60e6, this);
    ui->lineX1->setValidator(intval);
    ui->lineZ1->setValidator(intval);
    ui->lineX2->setValidator(intval);
    ui->lineZ2->setValidator(intval);

    // 计算环形区域总面积
    const double ringArea = 3.141592653589793 * (128.0 * 128.0 - 24.0 * 24.0);
    const int maxArea = (int)ringArea;
    
    ui->lineFarmSize->setValidator(new QIntValidator(0, maxArea, this));
    ui->lineFarmPercent->setValidator(new QDoubleValidator(0.0, 100.0, 2, this));
    
    // 连接方块和百分比的同步信号
    connect(ui->lineFarmSize, &QLineEdit::editingFinished, this, &TabLocateBiome::on_lineFarmSize_editingFinished);
    connect(ui->lineFarmPercent, &QLineEdit::editingFinished, this, &TabLocateBiome::on_lineFarmPercent_editingFinished);

    ui->treeLocateFarm->setSortingEnabled(true);
    ui->treeLocateFarm->sortByColumn(-1, Qt::DescendingOrder);

    connect(&thread, &AnalysisLocateBiome::seedFarmItem, this, &TabLocateBiome::onAnalysisSeedFarmItem, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisLocateBiome::finished, this, &TabLocateBiome::onAnalysisFinished);

    // 连接信号槽，实现双向同步
    connect(ui->lineX1, &QLineEdit::editingFinished, this, &TabLocateBiome::on_lineX1_editingFinished);
    connect(ui->lineZ1, &QLineEdit::editingFinished, this, &TabLocateBiome::on_lineZ1_editingFinished);
    connect(ui->lineX2, &QLineEdit::editingFinished, this, &TabLocateBiome::on_lineX2_editingFinished);
    connect(ui->lineZ2, &QLineEdit::editingFinished, this, &TabLocateBiome::on_lineZ2_editingFinished);
    connect(ui->comboSeedSource, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TabLocateBiome::on_comboSeedSource_currentIndexChanged);

    for (int id = 0; id < 256; id++)
    {
        QString s;
        if (!(s = getBiomeDisplay(MC_1_17, id)).isEmpty())
            str2biome[s] = id;
        if (!(s = getBiomeDisplay(MC_NEWEST, id)).isEmpty())
            str2biome[s] = id;
    }

    const QStringList bnames = str2biome.keys();
    QRegularExpressionValidator *revalFarm = new QRegularExpressionValidator(
        QRegularExpression("(" + bnames.join("|") + ")"), this
    );
    ui->comboFarmBiome->lineEdit()->setValidator(revalFarm);

    refreshBiomes();
}

TabLocateBiome::~TabLocateBiome()
{
    thread.stop = true;
    thread.wait(500);
    delete ui;
}

bool TabLocateBiome::event(QEvent *e)
{
    if (e->type() == QEvent::LayoutRequest)
    {
        QFontMetrics fm = QFontMetrics(ui->treeLocateFarm->font());
        ui->treeLocateFarm->setColumnWidth(0, txtWidth(fm) * 24);
        ui->treeLocateFarm->setColumnWidth(1, txtWidth(fm) * 16);
        ui->treeLocateFarm->setColumnWidth(2, txtWidth(fm) * 9);
        ui->treeLocateFarm->setColumnWidth(3, txtWidth(fm) * 9);
        ui->treeLocateFarm->setColumnWidth(4, txtWidth(fm) * 12);
    }
    return QWidget::event(e);
}

void TabLocateBiome::save(QSettings& settings)
{
    int x1 = ui->lineX1->text().toInt();
    int z1 = ui->lineZ1->text().toInt();
    int x2 = ui->lineX2->text().toInt();
    int z2 = ui->lineZ2->text().toInt();
    int seedsrc = ui->comboSeedSource->currentIndex();
    
    // 保存到共享的键，与其他tab同步
    settings.setValue("analysis/x1", x1);
    settings.setValue("analysis/z1", z1);
    settings.setValue("analysis/x2", x2);
    settings.setValue("analysis/z2", z2);
    settings.setValue("analysis/seedsrc", seedsrc);
    
    // 也保存到自己的键
    settings.setValue("locatebiome/x1", x1);
    settings.setValue("locatebiome/z1", z1);
    settings.setValue("locatebiome/x2", x2);
    settings.setValue("locatebiome/z2", z2);
    settings.setValue("locatebiome/seedsrc", seedsrc);
    settings.setValue("locatebiome/biomeid", str2biome[ui->comboFarmBiome->currentText()]);
    settings.setValue("locatebiome/farmsize", ui->lineFarmSize->text().toInt());
}

void TabLocateBiome::load(QSettings& settings)
{
    // 优先从共享键加载，如果没有则从自己的键加载
    QSettings s(APP_STRING, APP_STRING);
    int x1 = s.value("analysis/x1", ui->lineX1->text().toInt()).toInt();
    int z1 = s.value("analysis/z1", ui->lineZ1->text().toInt()).toInt();
    int x2 = s.value("analysis/x2", ui->lineX2->text().toInt()).toInt();
    int z2 = s.value("analysis/z2", ui->lineZ2->text().toInt()).toInt();
    int seedsrc = s.value("analysis/seedsrc", ui->comboSeedSource->currentIndex()).toInt();
    
    // 临时断开信号，避免触发保存
    ui->lineX1->blockSignals(true);
    ui->lineZ1->blockSignals(true);
    ui->lineX2->blockSignals(true);
    ui->lineZ2->blockSignals(true);
    ui->comboSeedSource->blockSignals(true);
    
    ui->lineX1->setText(QString::number(x1));
    ui->lineZ1->setText(QString::number(z1));
    ui->lineX2->setText(QString::number(x2));
    ui->lineZ2->setText(QString::number(z2));
    ui->comboSeedSource->setCurrentIndex(seedsrc);
    
    ui->lineX1->blockSignals(false);
    ui->lineZ1->blockSignals(false);
    ui->lineX2->blockSignals(false);
    ui->lineZ2->blockSignals(false);
    ui->comboSeedSource->blockSignals(false);
    
    // 加载自己的特定设置
    refreshBiomes(settings.value("locatebiome/biomeid", -1).toInt());
    ui->lineFarmSize->setText(QString::number(settings.value("locatebiome/farmsize", 0).toLongLong()));
}

void TabLocateBiome::refreshBiomes(int activeid)
{
    WorldInfo wi;
    parent->getSeed(&wi);
    if (activeid == -1)
    {
        QString s = ui->comboFarmBiome->currentText();
        if (str2biome.count(s))
            activeid = str2biome[s];
    }
    std::vector<int> ids;
    for (int i = 0; i < 256; i++)
        if (isOverworld(wi.mc, i) || i == activeid)
            ids.push_back(i);
    IdCmp cmp(IdCmp::SORT_LEX, wi.mc, DIM_UNDEF);
    std::sort(ids.begin(), ids.end(), cmp);
    ui->comboFarmBiome->clear();
    for (int i : ids)
        ui->comboFarmBiome->addItem(getBiomeIcon(i), getBiomeDisplay(wi.mc, i), QVariant::fromValue(i));
    if (activeid >= 0)
    {
        int idx = ui->comboFarmBiome->findText(getBiomeDisplay(wi.mc, activeid));
        ui->comboFarmBiome->setCurrentIndex(idx);
    }
}

void TabLocateBiome::refresh()
{
    // 从其他tab同步坐标和seeds选项
    QSettings settings(APP_STRING, APP_STRING);
    if (settings.contains("analysis/x1"))
    {
        int x1 = settings.value("analysis/x1").toInt();
        int z1 = settings.value("analysis/z1").toInt();
        int x2 = settings.value("analysis/x2").toInt();
        int z2 = settings.value("analysis/z2").toInt();
        int seedsrc = settings.value("analysis/seedsrc", 0).toInt();
        
        // 临时断开信号，避免触发保存
        ui->lineX1->blockSignals(true);
        ui->lineZ1->blockSignals(true);
        ui->lineX2->blockSignals(true);
        ui->lineZ2->blockSignals(true);
        ui->comboSeedSource->blockSignals(true);
        
        ui->lineX1->setText(QString::number(x1));
        ui->lineZ1->setText(QString::number(z1));
        ui->lineX2->setText(QString::number(x2));
        ui->lineZ2->setText(QString::number(z2));
        ui->comboSeedSource->setCurrentIndex(seedsrc);
        
        ui->lineX1->blockSignals(false);
        ui->lineZ1->blockSignals(false);
        ui->lineX2->blockSignals(false);
        ui->lineZ2->blockSignals(false);
        ui->comboSeedSource->blockSignals(false);
    }
    refreshBiomes();
}

void TabLocateBiome::on_lineX1_editingFinished()
{
    QSettings settings(APP_STRING, APP_STRING);
    int x1 = ui->lineX1->text().toInt();
    settings.setValue("analysis/x1", x1);
    settings.setValue("locatebiome/x1", x1);
}

void TabLocateBiome::on_lineZ1_editingFinished()
{
    QSettings settings(APP_STRING, APP_STRING);
    int z1 = ui->lineZ1->text().toInt();
    settings.setValue("analysis/z1", z1);
    settings.setValue("locatebiome/z1", z1);
}

void TabLocateBiome::on_lineX2_editingFinished()
{
    QSettings settings(APP_STRING, APP_STRING);
    int x2 = ui->lineX2->text().toInt();
    settings.setValue("analysis/x2", x2);
    settings.setValue("locatebiome/x2", x2);
}

void TabLocateBiome::on_lineZ2_editingFinished()
{
    QSettings settings(APP_STRING, APP_STRING);
    int z2 = ui->lineZ2->text().toInt();
    settings.setValue("analysis/z2", z2);
    settings.setValue("locatebiome/z2", z2);
}

void TabLocateBiome::on_comboSeedSource_currentIndexChanged(int index)
{
    QSettings settings(APP_STRING, APP_STRING);
    settings.setValue("analysis/seedsrc", index);
    settings.setValue("locatebiome/seedsrc", index);
}

void TabLocateBiome::onAnalysisSeedFarmItem(QTreeWidgetItem *item)
{
    qbuflf.push_back(item);
    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &TabLocateBiome::onBufferTimeout);
    }
}

void TabLocateBiome::onAnalysisFinished()
{
    onBufferTimeout();
    ui->pushStart->setChecked(false);
    ui->pushStart->setText(tr("Analyze"));
}

void TabLocateBiome::onBufferTimeout()
{
    if (qbuflf.empty())
        return;

    ui->treeLocateFarm->setSortingEnabled(false);
    ui->treeLocateFarm->setUpdatesEnabled(false);
    ui->treeLocateFarm->addTopLevelItems(qbuflf);
    ui->treeLocateFarm->setUpdatesEnabled(true);
    ui->treeLocateFarm->setSortingEnabled(true);
    qbuflf.clear();

    QString progress = QString::asprintf(" (%ld/%zu)", thread.idx.load(), thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);

    QApplication::processEvents();
}

void TabLocateBiome::on_pushStart_clicked()
{
    if (thread.isRunning())
    {
        thread.stop.store(true);
        return;
    }

    // 清空之前的结果
    ui->treeLocateFarm->setSortingEnabled(false);
    while (ui->treeLocateFarm->topLevelItemCount() > 0)
        delete ui->treeLocateFarm->takeTopLevelItem(0);
    ui->treeLocateFarm->setSortingEnabled(true);
    qbuflf.clear();

    updt = 20;
    nextupdate = 0;
    elapsed.start();

    parent->getSeed(&thread.wi);
    thread.seeds.clear();
    if (ui->comboSeedSource->currentIndex() == 0)
        thread.seeds.push_back(thread.wi.seed);
    else
        thread.seeds = parent->formControl->getResults();

    int x1 = ui->lineX1->text().toInt();
    int z1 = ui->lineZ1->text().toInt();
    int x2 = ui->lineX2->text().toInt();
    int z2 = ui->lineZ2->text().toInt();
    if (x2 < x1) std::swap(x1, x2);
    if (z2 < z1) std::swap(z1, z2);

    thread.dat.scale = 4;
    thread.dat.x1 = x1 >> 2;
    thread.dat.z1 = z1 >> 2;
    thread.dat.x2 = x2 >> 2;
    thread.dat.z2 = z2 >> 2;
    thread.dat.locate = str2biome[ui->comboFarmBiome->currentText()];
    
    // 开始前同步面积和百分比的值
    const double ringArea = 3.141592653589793 * (128.0 * 128.0 - 24.0 * 24.0);
    QString percentText = ui->lineFarmPercent->text();
    QString sizeText = ui->lineFarmSize->text();
    
    // 如果两者都为空，设置为0
    if (percentText.isEmpty() && sizeText.isEmpty())
    {
        thread.minsize = 0;
    }
    // 如果百分比已填，使用百分比计算面积
    else if (!percentText.isEmpty())
    {
        bool ok;
        double percent = percentText.toDouble(&ok);
        if (ok && percent >= 0.0 && percent <= 100.0)
        {
            thread.minsize = (int)((percent / 100.0) * ringArea);
            // 同步到方块面积（不触发信号）
            ui->lineFarmSize->blockSignals(true);
            ui->lineFarmSize->setText(QString::number(thread.minsize));
            ui->lineFarmSize->blockSignals(false);
        }
        else
        {
            warn(parent, tr("area too small/large"));
            return;
        }
    }
    // 如果方块面积已填，使用方块面积
    else if (!sizeText.isEmpty())
    {
        bool ok;
        int areaBlocks = sizeText.toInt(&ok);
        if (ok && areaBlocks >= 0 && areaBlocks <= (int)ringArea)
        {
            thread.minsize = areaBlocks;
            // 同步到百分比（不触发信号）
            double percent = (areaBlocks / ringArea) * 100.0;
            ui->lineFarmPercent->blockSignals(true);
            ui->lineFarmPercent->setText(QString::number(percent, 'f', 2));
            ui->lineFarmPercent->blockSignals(false);
        }
        else
        {
            warn(parent, tr("area too small/large"));
            return;
        }
    }
    
    if (thread.minsize < 0)
        thread.minsize = 0;

    datl = thread.dat;

    ui->pushExport->setEnabled(false);
    ui->pushStart->setChecked(true);
    QString progress = QString::asprintf(" (0/%zu)", thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);
    thread.start();
}

void TabLocateBiome::on_pushExport_clicked()
{
#if WASM
    QByteArray content;
    QTextStream stream(&content);
    exportResults(stream);
    QFileDialog::saveFileContent(content, "locatebiome.csv");
#else
    QString fnam = QFileDialog::getSaveFileName(
        this, tr("Export biome locate results"), parent->prevdir, tr("Text files (*.txt *csv);;Any files (*)"));
    if (fnam.isEmpty())
        return;

    QFileInfo finfo(fnam);
    QFile file(fnam);
    parent->prevdir = finfo.absolutePath();

    if (!file.open(QIODevice::WriteOnly))
    {
        warn(parent, tr("Failed to open file for export:\n\"%1\"").arg(fnam));
        return;
    }

    QTextStream stream(&file);
    exportResults(stream);
#endif
}

void TabLocateBiome::on_buttonFromVisible_clicked()
{
    MapView *mapview = parent->getMapView();

    int x1, z1, x2, z2;
    mapview->getVisible(&x1, &z1, &x2, &z2);

    ui->lineX1->setText(QString::number(x1));
    ui->lineZ1->setText(QString::number(z1));
    ui->lineX2->setText(QString::number(x2));
    ui->lineZ2->setText(QString::number(z2));
}

void TabLocateBiome::on_lineFarmSize_textChanged(const QString &text)
{
    (void)text;
    // 可以在这里添加面积提示
}

void TabLocateBiome::on_lineFarmSize_editingFinished()
{
    // 计算环形区域总面积
    const double ringArea = 3.141592653589793 * (128.0 * 128.0 - 24.0 * 24.0);
    
    bool ok;
    int areaBlocks = ui->lineFarmSize->text().toInt(&ok);
    if (!ok || areaBlocks < 0)
    {
        warn(parent, tr("area too small/large"));
        return;
    }
    
    if (areaBlocks > (int)ringArea)
    {
        warn(parent, tr("area too small/large"));
        return;
    }
    
    // 同步到百分比
    double percent = (areaBlocks / ringArea) * 100.0;
    ui->lineFarmPercent->blockSignals(true);
    ui->lineFarmPercent->setText(QString::number(percent, 'f', 2));
    ui->lineFarmPercent->blockSignals(false);
}

void TabLocateBiome::on_lineFarmPercent_editingFinished()
{
    // 计算环形区域总面积
    const double ringArea = 3.141592653589793 * (128.0 * 128.0 - 24.0 * 24.0);
    
    bool ok;
    double percent = ui->lineFarmPercent->text().toDouble(&ok);
    if (!ok || percent < 0.0 || percent > 100.0)
    {
        warn(parent, tr("area too small/large"));
        return;
    }
    
    // 同步到方块面积
    int areaBlocks = (int)((percent / 100.0) * ringArea);
    ui->lineFarmSize->blockSignals(true);
    ui->lineFarmSize->setText(QString::number(areaBlocks));
    ui->lineFarmSize->blockSignals(false);
}

void TabLocateBiome::on_treeLocateFarm_itemClicked(QTreeWidgetItem *item, int column)
{
    (void) column;
    QVariant dat;
    dat = item->data(0, Qt::UserRole);
    if (dat.isValid())
    {
        uint64_t seed = qvariant_cast<uint64_t>(dat);
        int dim = item->data(0, Qt::UserRole+1).toInt();
        WorldInfo wi;
        parent->getSeed(&wi);
        wi.seed = seed;
        parent->setSeed(wi, dim);
    }

    dat = item->data(0, Qt::UserRole+2);
    if (dat.isValid())
    {
        Pos p = qvariant_cast<Pos>(dat);
        parent->getMapView()->setView(p.x+0.5, p.z+0.5);
        
        // 绘制两个同心圆（半径24和128）
        currentShapes.clear();
        
        Shape innerCircle, outerCircle;
        innerCircle.type = Shape::CIRCLE;
        innerCircle.dim = DIM_OVERWORLD;
        innerCircle.p1 = p;
        innerCircle.p2 = Pos{0, 0};
        innerCircle.r = 24; // 内圆半径24
        
        outerCircle.type = Shape::CIRCLE;
        outerCircle.dim = DIM_OVERWORLD;
        outerCircle.p1 = p;
        outerCircle.p2 = Pos{0, 0};
        outerCircle.r = 128; // 外圆半径128
        
        currentShapes.push_back(innerCircle);
        currentShapes.push_back(outerCircle);
        parent->getMapView()->setShapes(currentShapes);
    }
}

void TabLocateBiome::exportResults(QTextStream& stream)
{
    QString qte = parent->config.quote;
    QString sep = parent->config.separator;

    stream << "Sep=" + sep + "\n";
    sep = qte + sep + qte;

    stream << qte << "#X1" << sep << datl.x1 << sep << "(" << (datl.x1*datl.scale) << ")" << qte << "\n";
    stream << qte << "#Z1" << sep << datl.z1 << sep << "(" << (datl.z1*datl.scale) << ")" << qte << "\n";
    stream << qte << "#X2" << sep << datl.x2 << sep << "(" << (datl.x2*datl.scale) << ")" << qte << "\n";
    stream << qte << "#Z2" << sep << datl.z2 << sep << "(" << (datl.z2*datl.scale) << ")" << qte << "\n";
    stream << qte << "#scale" << sep << "1:" << datl.scale << qte << "\n";
    stream << qte << "#biome" << sep << biome2str(MC_NEWEST, datl.locate) << qte << "\n";

    QStringList header = { tr("seed"), tr("area (blocks)"), tr("x"), tr("z"), tr("percentage (%)") };
    stream << qte << header.join(sep) << qte << "\n";

    QTreeWidgetItemIterator it(ui->treeLocateFarm);
    QString seed;
    for (; *it; ++it)
    {
        QTreeWidgetItem *item = *it;
        if (item->text(0) != "-")
        {
            seed = item->text(0);
            continue;
        }
        QStringList cols;
        cols.append(seed);
        cols.append(item->text(1));
        cols.append(item->text(2));
        cols.append(item->text(3));
        cols.append(item->text(4));
        stream << qte << cols.join(sep) << qte << "\n";
    }
    stream.flush();
}

