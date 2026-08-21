#include "tabslime.h"
#include "ui_tabslime.h"

#include "config.h"
#include "message.h"
#include "util.h"
#include "world.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QIntValidator>
#include <QTimer>
#include <QTextStream>

#include "cubiomes/biomes.h"
#include "cubiomes/finders.h"
#include "cubiomes/generator.h"

// 计算以centerX, centerZ为中心的挂机点，24格之外128格之内的史莱姆区块覆盖面积
static int calculateSlimeArea(uint64_t seed, int centerX, int centerZ, bool includeBiome, Generator *g, int mc)
{
    const int MIN_DIST = 24;  // 24格之外
    const int MAX_DIST = 128; // 128格之内
    const int MIN_DIST2 = MIN_DIST * MIN_DIST;
    const int MAX_DIST2 = MAX_DIST * MAX_DIST;
    
    int totalArea = 0;
    
    // 计算需要检查的区块范围
    int minChunkX = (centerX - MAX_DIST) >> 4;
    int maxChunkX = (centerX + MAX_DIST) >> 4;
    int minChunkZ = (centerZ - MAX_DIST) >> 4;
    int maxChunkZ = (centerZ + MAX_DIST) >> 4;
    
    for (int chunkZ = minChunkZ; chunkZ <= maxChunkZ; chunkZ++)
    {
        for (int chunkX = minChunkX; chunkX <= maxChunkX; chunkX++)
        {
            // 检查是否是史莱姆区块
            if (!isSlimeChunk(seed, chunkX, chunkZ))
                continue;
            
            // 计算区块中心坐标
            int chunkCenterX = (chunkX << 4) + 8;
            int chunkCenterZ = (chunkZ << 4) + 8;
            
            // 计算到挂机点的距离（使用区块中心）
            int dx = chunkCenterX - centerX;
            int dz = chunkCenterZ - centerZ;
            int dist2 = dx * dx + dz * dz;
            
            // 检查是否在24格之外128格之内
            if (dist2 < MIN_DIST2 || dist2 > MAX_DIST2)
                continue;
            
            // 计算该区块的有效面积
            int chunkArea = 256; // 一个区块是16x16=256方块
            
            if (includeBiome && g)
            {
                int biomeId = none;
                
                if (mc >= MC_1_18)
                {
                    // MC 1.18+：在y=-64取样群系，使用scale=4
                    int y = -64 >> 2; // -16 for scale=4
                    biomeId = getBiomeAt(g, 4, chunkCenterX >> 2, y, chunkCenterZ >> 2);
                }
                else
                {
                    // MC 1.17及以前：在y=0取样群系
                    biomeId = getBiomeAt(g, 4, chunkCenterX >> 2, 0, chunkCenterZ >> 2);
                }
                
                // 根据版本决定哪些群系需要处理
                // 1.17及以前：仅计算河流和蘑菇岛的减成
                // 1.18：增加溶洞的减成
                // 1.19：增加深暗的减成
                
                // 蘑菇岛不算（所有版本）
                if (biomeId == mushroom_fields)
                {
                    chunkArea = 0;
                }
                // 深暗之域不算（1.19+）
                else if (mc >= MC_1_19 && biomeId == deep_dark)
                {
                    chunkArea = 0;
                }
                else
                {
                    // 河流的面积乘以5/6（所有版本）
                    if (biomeId == river)
                    {
                        chunkArea = (chunkArea * 5) / 6;
                    }
                    // 溶洞的面积乘以5/6（1.18+）
                    else if (mc >= MC_1_18 && biomeId == dripstone_caves)
                    {
                        chunkArea = (chunkArea * 5) / 6;
                    }
                    // Old Growth Pine Taiga的面积乘以0.95（所有版本）
                    else if (biomeId == old_growth_pine_taiga)
                    {
                        chunkArea = (chunkArea * 95) / 100;
                    }
                    // 其他群系面积不变
                }
            }
            
            totalArea += chunkArea;
        }
    }
    
    return totalArea;
}

void AnalysisSlime::run()
{
    stop = false;
    
    Generator g;
    setupGenerator(&g, wi.mc, wi.large);
    
    if (includeBiome)
    {
        applySeed(&g, DIM_OVERWORLD, wi.seed);
    }
    
    for (idx = 0; idx < (long)seeds.size(); idx++)
    {
        if (stop) break;
        
        uint64_t seed = seeds[idx];
        wi.seed = seed;
        
        if (includeBiome)
        {
            applySeed(&g, DIM_OVERWORLD, seed);
        }
        
        QList<SlimeResult> results;
        
        // 遍历所有可能的挂机点（xz都是16的倍数）
        // 确保坐标是16的倍数
        // startX: >= x1 的最小的16的倍数
        // endX: <= x2 的最大的16的倍数
        int startX = (x1 % 16 == 0) ? x1 : ((x1 < 0) ? ((x1 / 16) - 1) * 16 : ((x1 / 16) + 1) * 16);
        int startZ = (z1 % 16 == 0) ? z1 : ((z1 < 0) ? ((z1 / 16) - 1) * 16 : ((z1 / 16) + 1) * 16);
        int endX = (x2 / 16) * 16;
        int endZ = (z2 / 16) * 16;
        
        for (int centerZ = startZ; centerZ <= endZ && !stop; centerZ += 16)
        {
            for (int centerX = startX; centerX <= endX && !stop; centerX += 16)
            {
                int area = calculateSlimeArea(seed, centerX, centerZ, includeBiome, includeBiome ? &g : nullptr, wi.mc);
                
                if (area >= minArea)
                {
                    SlimeResult result;
                    result.centerX = centerX;
                    result.centerZ = centerZ;
                    result.area = area;
                    results.append(result);
                }
            }
        }
        
        if (!results.empty() && !stop)
        {
            emit seedDone(seed, results);
        }
    }
}

TabSlime::TabSlime(MainWindow *parent)
    : QWidget(parent)
    , ui(new Ui::TabSlime)
    , parent(parent)
    , thread()
{
    ui->setupUi(this);
    
    // 设置翻译文本
    ui->labelDescription->setText(tr("Calculate optimal AFK positions for slime chunk coverage (24-128 blocks radius)."));
    ui->label->setText(tr("Seed(s):"));
    ui->comboSeedSource->setItemText(0, tr("Current seed"));
    ui->comboSeedSource->setItemText(1, tr("From matching seeds list"));
    ui->labelX1->setText(QString("<html><head/><body><p>X<span style=\" vertical-align:sub;\">1</span>:</p></body></html>"));
    ui->labelZ1->setText(QString("<html><head/><body><p>Z<span style=\" vertical-align:sub;\">1</span>:</p></body></html>"));
    ui->labelX2->setText(QString("<html><head/><body><p>X<span style=\" vertical-align:sub;\">2</span>:</p></body></html>"));
    ui->labelZ2->setText(QString("<html><head/><body><p>Z<span style=\" vertical-align:sub;\">2</span>:</p></body></html>"));
    ui->buttonFromVisible->setText(tr("From visible"));
    ui->labelMinArea->setText(tr("Minimum area:"));
    ui->checkIncludeBiome->setText(tr("Include biome parameters"));
    ui->checkIncludeBiome->setToolTip(tr("<html><head/><head/><body><p>Sample biomes at y=-64 (for MC 1.18+). Exclude mushroom fields and deep dark. Apply multipliers for dripstone caves/rivers (5/6) and old growth pine taiga (0.95).</p></body></html>"));
    if (ui->treeResults->headerItem())
    {
        ui->treeResults->headerItem()->setText(0, tr("seed"));
        ui->treeResults->headerItem()->setText(1, tr("area"));
        ui->treeResults->headerItem()->setText(2, tr("x"));
        ui->treeResults->headerItem()->setText(3, tr("z"));
    }
    ui->pushExport->setText(tr("Export..."));
    ui->pushStart->setText(tr("Analyze"));
    
    updt = 20;
    nextupdate = 0;
    
    QIntValidator *intval = new QIntValidator(-60e6, 60e6, this);
    ui->lineX1->setValidator(intval);
    ui->lineZ1->setValidator(intval);
    ui->lineX2->setValidator(intval);
    ui->lineZ2->setValidator(intval);
    
    ui->lineMinArea->setValidator(new QIntValidator(1, INT_MAX, this));
    ui->lineMinArea->setText("10000");
    
    ui->treeResults->setSortingEnabled(true);
    ui->treeResults->sortByColumn(1, Qt::DescendingOrder); // 按面积降序排列
    
    connect(&thread, &AnalysisSlime::seedDone, this, &TabSlime::onAnalysisSeedDone, Qt::BlockingQueuedConnection);
    connect(&thread, &AnalysisSlime::finished, this, &TabSlime::onAnalysisFinished);
    
    // 连接信号槽，实现双向同步
    connect(ui->lineX1, &QLineEdit::editingFinished, this, &TabSlime::on_lineX1_editingFinished);
    connect(ui->lineZ1, &QLineEdit::editingFinished, this, &TabSlime::on_lineZ1_editingFinished);
    connect(ui->lineX2, &QLineEdit::editingFinished, this, &TabSlime::on_lineX2_editingFinished);
    connect(ui->lineZ2, &QLineEdit::editingFinished, this, &TabSlime::on_lineZ2_editingFinished);
    connect(ui->comboSeedSource, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TabSlime::on_comboSeedSource_currentIndexChanged);
}

TabSlime::~TabSlime()
{
    thread.stop = true;
    thread.wait(500);
    delete ui;
}

bool TabSlime::event(QEvent *e)
{
    if (e->type() == QEvent::LayoutRequest)
    {
        QFontMetrics fm = QFontMetrics(ui->treeResults->font());
        ui->treeResults->setColumnWidth(0, txtWidth(fm) * 24);
        ui->treeResults->setColumnWidth(1, txtWidth(fm) * 16);
        ui->treeResults->setColumnWidth(2, txtWidth(fm) * 9);
        ui->treeResults->setColumnWidth(3, txtWidth(fm) * 9);
    }
    return QWidget::event(e);
}

void TabSlime::save(QSettings& settings)
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
    settings.setValue("slime/x1", x1);
    settings.setValue("slime/z1", z1);
    settings.setValue("slime/x2", x2);
    settings.setValue("slime/z2", z2);
    settings.setValue("slime/seedsrc", seedsrc);
    settings.setValue("slime/minarea", ui->lineMinArea->text().toInt());
    settings.setValue("slime/includebiome", ui->checkIncludeBiome->isChecked());
}

static void loadCheck(QSettings *s, QCheckBox *cb, const char *key)
{
    cb->setChecked(s->value(key, cb->isChecked()).toBool());
}
static void loadCombo(QSettings *s, QComboBox *combo, const char *key)
{
    combo->setCurrentIndex(s->value(key, combo->currentIndex()).toInt());
}
static void loadLine(QSettings *s, QLineEdit *line, const char *key)
{
    qlonglong x = line->text().toLongLong();
    line->setText(QString::number(s->value(key, x).toLongLong()));
}

void TabSlime::load(QSettings& settings)
{
    // 优先从其他tab（如tabbiomes）读取坐标和seeds选项
    QSettings s(APP_STRING, APP_STRING);
    int x1 = s.value("analysis/x1", ui->lineX1->text().toInt()).toInt();
    int z1 = s.value("analysis/z1", ui->lineZ1->text().toInt()).toInt();
    int x2 = s.value("analysis/x2", ui->lineX2->text().toInt()).toInt();
    int z2 = s.value("analysis/z2", ui->lineZ2->text().toInt()).toInt();
    int seedsrc = s.value("analysis/seedsrc", ui->comboSeedSource->currentIndex()).toInt();
    
    // 如果其他tab有值，使用其他tab的值，否则使用自己的保存值
    if (s.contains("analysis/x1"))
    {
        ui->lineX1->setText(QString::number(x1));
        ui->lineZ1->setText(QString::number(z1));
        ui->lineX2->setText(QString::number(x2));
        ui->lineZ2->setText(QString::number(z2));
        ui->comboSeedSource->setCurrentIndex(seedsrc);
    }
    else
    {
        loadLine(&settings, ui->lineX1, "slime/x1");
        loadLine(&settings, ui->lineZ1, "slime/z1");
        loadLine(&settings, ui->lineX2, "slime/x2");
        loadLine(&settings, ui->lineZ2, "slime/z2");
        loadCombo(&settings, ui->comboSeedSource, "slime/seedsrc");
    }
    
    loadLine(&settings, ui->lineMinArea, "slime/minarea");
    loadCheck(&settings, ui->checkIncludeBiome, "slime/includebiome");
}

void TabSlime::refresh()
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
}

void TabSlime::on_lineX1_editingFinished()
{
    QSettings settings(APP_STRING, APP_STRING);
    int x1 = ui->lineX1->text().toInt();
    settings.setValue("analysis/x1", x1);
    settings.setValue("slime/x1", x1);
}

void TabSlime::on_lineZ1_editingFinished()
{
    QSettings settings(APP_STRING, APP_STRING);
    int z1 = ui->lineZ1->text().toInt();
    settings.setValue("analysis/z1", z1);
    settings.setValue("slime/z1", z1);
}

void TabSlime::on_lineX2_editingFinished()
{
    QSettings settings(APP_STRING, APP_STRING);
    int x2 = ui->lineX2->text().toInt();
    settings.setValue("analysis/x2", x2);
    settings.setValue("slime/x2", x2);
}

void TabSlime::on_lineZ2_editingFinished()
{
    QSettings settings(APP_STRING, APP_STRING);
    int z2 = ui->lineZ2->text().toInt();
    settings.setValue("analysis/z2", z2);
    settings.setValue("slime/z2", z2);
}

void TabSlime::on_comboSeedSource_currentIndexChanged(int index)
{
    QSettings settings(APP_STRING, APP_STRING);
    settings.setValue("analysis/seedsrc", index);
    settings.setValue("slime/seedsrc", index);
}

void TabSlime::onAnalysisSeedDone(uint64_t seed, QList<SlimeResult> results)
{
    if (results.empty())
        return;
    
    // 创建种子父节点
    QTreeWidgetItem *seeditem = new QTreeWidgetItem();
    seeditem->setData(0, Qt::DisplayRole, QVariant::fromValue((qlonglong)seed));
    seeditem->setData(0, Qt::UserRole+0, QVariant::fromValue(seed));
    seeditem->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_OVERWORLD));
    
    // 为每个结果创建子节点
    for (const SlimeResult& result : results)
    {
        QTreeWidgetItem *item = new QTreeWidgetItem(seeditem);
        item->setText(0, "-");
        item->setData(1, Qt::DisplayRole, QVariant::fromValue(result.area));
        item->setData(2, Qt::DisplayRole, QVariant::fromValue(result.centerX));
        item->setData(3, Qt::DisplayRole, QVariant::fromValue(result.centerZ));
        item->setData(0, Qt::UserRole+0, QVariant::fromValue(seed));
        item->setData(0, Qt::UserRole+1, QVariant::fromValue((int)DIM_OVERWORLD));
        item->setData(0, Qt::UserRole+2, QVariant::fromValue(Pos{result.centerX, result.centerZ}));
    }
    
    qbufl.push_back(seeditem);
    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &TabSlime::onBufferTimeout);
    }
}

void TabSlime::onAnalysisFinished()
{
    onBufferTimeout();
    ui->pushStart->setChecked(false);
    ui->pushStart->setText(tr("Analyze"));
}

void TabSlime::onBufferTimeout()
{
    if (qbufl.empty())
        return;
    
    uint64_t t = -elapsed.elapsed();
    
    ui->treeResults->setSortingEnabled(false);
    ui->treeResults->setUpdatesEnabled(false);
    ui->treeResults->addTopLevelItems(qbufl);
    ui->treeResults->setUpdatesEnabled(true);
    ui->treeResults->setSortingEnabled(true);
    qbufl.clear();
    
    QString progress = QString::asprintf(" (%ld/%zu)", thread.idx.load(), thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);
    
    QApplication::processEvents(); // force processing of events so we can time correctly
    
    t += elapsed.elapsed();
    if (8*t > updt)
        updt = 4*t;
    nextupdate = elapsed.nsecsElapsed() + 1e6 * updt;
}

void TabSlime::on_pushStart_clicked()
{
    if (thread.isRunning())
    {
        thread.stop = true;
        return;
    }
    
    // 保存当前设置到共享键
    QSettings settings(APP_STRING, APP_STRING);
    save(settings);
    
    parent->getSeed(&thread.wi);
    thread.seeds.clear();
    if (ui->comboSeedSource->currentIndex() == 0)
        thread.seeds.push_back(thread.wi.seed);
    else
        thread.seeds = parent->formControl->getResults();
    
    if (thread.seeds.empty())
    {
        warn(parent, tr("No seeds to process."));
        return;
    }
    
    int x1 = ui->lineX1->text().toInt();
    int z1 = ui->lineZ1->text().toInt();
    int x2 = ui->lineX2->text().toInt();
    int z2 = ui->lineZ2->text().toInt();
    if (x2 < x1) std::swap(x1, x2);
    if (z2 < z1) std::swap(z1, z2);
    
    thread.x1 = x1;
    thread.z1 = z1;
    thread.x2 = x2;
    thread.z2 = z2;
    thread.minArea = ui->lineMinArea->text().toInt();
    thread.includeBiome = ui->checkIncludeBiome->isChecked();
    
    if (thread.minArea <= 0)
        thread.minArea = 10000;
    
    ui->treeResults->setSortingEnabled(false);
    while (ui->treeResults->topLevelItemCount() > 0)
        delete ui->treeResults->takeTopLevelItem(0);
    ui->treeResults->setSortingEnabled(true);
    
    updt = 20;
    nextupdate = 0;
    elapsed.start();
    
    ui->pushExport->setEnabled(false);
    ui->pushStart->setChecked(true);
    QString progress = QString::asprintf(" (0/%zu)", thread.seeds.size());
    ui->pushStart->setText(tr("Stop") + progress);
    thread.start();
}

static void csvline(QTextStream& stream, const QString& qte, const QString& sep, QStringList& cols)
{
    if (qte.isEmpty())
    {
        for (QString& s : cols)
            if (s.contains(sep))
                s = "\"" + s + "\"";
    }
    stream << qte << cols.join(sep) << qte << "\n";
}

void TabSlime::exportResults(QTextStream& stream)
{
    QString qte = parent->config.quote;
    QString sep = parent->config.separator;
    
    stream << "Sep=" + sep + "\n";
    sep = qte + sep + qte;
    
    QStringList header = { tr("seed"), tr("area"), tr("x"), tr("z") };
    csvline(stream, qte, sep, header);
    
    QTreeWidgetItemIterator it(ui->treeResults);
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
        csvline(stream, qte, sep, cols);
    }
    stream.flush();
}

void TabSlime::on_pushExport_clicked()
{
#if WASM
    QByteArray content;
    QTextStream stream(&content);
    exportResults(stream);
    QFileDialog::saveFileContent(content, "slime.csv");
#else
    QString fnam = QFileDialog::getSaveFileName(
        this, tr("Export slime results"), parent->prevdir, tr("Text files (*.txt *.csv);;Any files (*)"));
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

void TabSlime::on_buttonFromVisible_clicked()
{
    MapView *mapview = parent->getMapView();
    
    int x1, z1, x2, z2;
    mapview->getVisible(&x1, &z1, &x2, &z2);
    
    ui->lineX1->setText(QString::number(x1));
    ui->lineZ1->setText(QString::number(z1));
    ui->lineX2->setText(QString::number(x2));
    ui->lineZ2->setText(QString::number(z2));
}

void TabSlime::on_treeResults_itemClicked(QTreeWidgetItem *item, int column)
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
        if (wi.seed != seed || (dim != DIM_UNDEF && dim != parent->getDim()))
        {
            wi.seed = seed;
            parent->getMapView()->deleteWorld();
        }
        parent->setSeed(wi, dim);
    }
    
    dat = item->data(0, Qt::UserRole+2);
    if (dat.isValid())
    {
        Pos p = qvariant_cast<Pos>(dat);
        
        // 创建两个圆形：半径为128和24
        std::vector<Shape> shapes;
        
        Shape circle128;
        circle128.type = Shape::CIRCLE;
        circle128.dim = DIM_OVERWORLD;
        circle128.p1 = p;
        circle128.p2 = Pos{0, 0};
        circle128.r = 128;
        shapes.push_back(circle128);
        
        Shape circle24;
        circle24.type = Shape::CIRCLE;
        circle24.dim = DIM_OVERWORLD;
        circle24.p1 = p;
        circle24.p2 = Pos{0, 0};
        circle24.r = 24;
        shapes.push_back(circle24);
        
        // 跳转到坐标并设置形状
        parent->getMapView()->setView(p.x+0.5, p.z+0.5);
        parent->getMapView()->setShapes(shapes);
    }
}

