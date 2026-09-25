#include "tabfixedseed.h"
#include "ui_tabfixedseed.h"

#include "message.h"
#include "mapview.h"
#include "util.h"

#include "monument_search.h"
#include "monument_search_core.h"
#include "Thread.h"
#include "slime_pipeline.h"
#include "river_search.h"
#include "cave_search.h"
#include "swamp_hut_search.h"
#include "fortress_search.h"
#include "slimerander.h"

#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMetaType>
#include <QTextStream>
#include <QThread>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <thread>

Q_DECLARE_METATYPE(FixedSeedHit)

static void loadLine(QSettings *s, QLineEdit *edit, const char *key)
{
    if (s->contains(key))
        edit->setText(QString::number(s->value(key).toInt()));
}

static int phaseCountForMode(int mode)
{
    return (mode == FS_SLIME || mode == FS_RIVER || mode == FS_CAVE) ? 2 : 1;
}

static int originDistance(int x, int z)
{
    return (int)std::llround(std::hypot((double)x, (double)z));
}

static void wrapOneProgressBar(QProgressBar *bar, QLabel **elapsedOut, QLabel **remainOut)
{
    if (!bar || !bar->parentWidget() || !bar->parentWidget()->layout())
        return;

    QWidget *host = bar->parentWidget();
    QLayout *lay = host->layout();

    auto *elapsed = new QLabel(QStringLiteral("0:00"), host);
    auto *remain = new QLabel(QStringLiteral("--:--"), host);
    QFont mono(QStringLiteral("Monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    elapsed->setFont(mono);
    remain->setFont(mono);
    elapsed->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    remain->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto *timeRow = new QHBoxLayout;
    timeRow->setContentsMargins(0, 0, 0, 0);
    timeRow->setSpacing(8);
    timeRow->addWidget(elapsed);
    timeRow->addStretch(1);
    timeRow->addWidget(remain);

    auto *col = new QVBoxLayout;
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);
    col->addLayout(timeRow);
    col->addWidget(bar);

    if (auto *vbox = qobject_cast<QVBoxLayout *>(lay)) {
        const int idx = vbox->indexOf(bar);
        vbox->removeWidget(bar);
        vbox->insertLayout(idx >= 0 ? idx : vbox->count(), col);
    } else if (auto *grid = qobject_cast<QGridLayout *>(lay)) {
        const int idx = grid->indexOf(bar);
        int r = 0, c = 0, rs = 1, cs = 1;
        if (idx >= 0)
            grid->getItemPosition(idx, &r, &c, &rs, &cs);
        grid->removeWidget(bar);
        grid->addLayout(col, r, c, rs, cs);
    } else {
        delete col;
        delete elapsed;
        delete remain;
        return;
    }

    *elapsedOut = elapsed;
    *remainOut = remain;
}

void FixedSeedWorker::run()
{
    stop = false;
    pause = false;
    bool completed = true;
    const int phaseCount = phaseCountForMode((int)mode);

    auto syncPauseStop = [&](std::atomic_bool *try_pause, std::atomic_bool *try_stop) {
        if (!try_pause || !try_stop)
            return;
        try_pause->store(pause.load());
        try_stop->store(stop.load());
    };
#define FS_SYNC_PROG(prog) syncPauseStop(&(prog).try_pause, &(prog).try_stop)

    auto joinWatcher = [&](std::thread &watcher) {
        bool userStop = stop.exchange(true);
        watcher.join();
        stop.store(userStop);
        if (userStop)
            completed = false;
    };

    for (uint64_t seed : seeds) {
        if (stop.load()) {
            completed = false;
            break;
        }

        emit progress((int)mode, 0, 1, 1, phaseCount,
                      tr("seed %1").arg((qint64)seed));

        int minX = std::min(x1, x2), maxX = std::max(x1, x2);
        int minZ = std::min(z1, z2), maxZ = std::max(z1, z2);
        int sx = maxX - minX + 1;
        int sz = maxZ - minZ + 1;

        if (mode == FS_MONUMENT) {
            MonumentSearchConfig cfg;
            cfg.seed = seed;
            cfg.mc = wi.mc;
            cfg.minX = minX >> 9;
            cfg.maxX = (maxX >> 9) + 1;
            cfg.minZ = minZ >> 9;
            cfg.maxZ = (maxZ >> 9) + 1;
            Progress prog;
            ThreadSafeResults<PairHit> out;
            std::thread watcher([&]() {
                while (!stop.load()) {
                    FS_SYNC_PROG(prog);
                    if (prog.total.load() > 0)
                        emit progress(FS_MONUMENT, prog.current.load(), prog.total.load(),
                                      1, phaseCount, tr("Scanning regions"));
                    QThread::msleep(100);
                    if (prog.status.load() >= 2)
                        break;
                }
                FS_SYNC_PROG(prog);
            });
            runMonumentSearch(cfg, &prog, out, threads);
            joinWatcher(watcher);
            for (const PairHit &h : out.getAllResults()) {
                FixedSeedHit hit;
                hit.seed = seed;
                hit.x = h.hangX;
                hit.z = h.hangZ;
                hit.a = h.dx;
                hit.b = h.dz;
                hit.c = h.lowEfficiency;
                if (monumentBiomeCheck) {
                    int ot = monument_classify_ocean_type(wi.mc, seed, h.hangX, h.hangZ);
                    if (ot == 2)
                        hit.extra = tr("frozen");
                    else if (ot == 1)
                        hit.extra = tr("cold");
                    else
                        hit.extra = tr("others");
                }
                emit hitFound(hit);
            }
        } else if (mode == FS_SLIME) {
            SlimePipelineConfig cfg;
            cfg.seed = (int64_t)seed;
            cfg.mc = wi.mc;
            cfg.blockX0 = minX;
            cfg.blockZ0 = minZ;
            cfg.blockX1 = maxX;
            cfg.blockZ1 = maxZ;
            cfg.threshold = slimeThreshold;
            cfg.minArea = slimeMinArea;
            cfg.biomeConv = slimeBiomeConv;
            cfg.threads = threads;
            SlimePipelineProgress prog;
            std::thread watcher([&]() {
                while (prog.phase.load() >= 0) {
                    prog.try_pause = pause.load();
                    prog.try_stop = stop.load();
                    if (pause.load())
                        sr_control_pause();
                    else
                        sr_control_resume();
                    if (stop.load()) {
                        sr_control_stop();
                        break;
                    }
                    int ph = prog.phase.load();
                    if (ph < 1)
                        ph = 1;
                    QString status = ph == 2 ? tr("Optimizing AFK") : tr("Radar scan");
                    emit progress(FS_SLIME, prog.current.load(), std::max(1, prog.total.load()),
                                  ph, phaseCount, status);
                    QThread::msleep(100);
                    if (prog.phase.load() == 2 && prog.total.load() > 0
                        && prog.current.load() >= prog.total.load())
                        break;
                }
            });
            std::vector<SlimePipelineHit> hits;
            runSlimePipeline(cfg, &prog, hits);
            joinWatcher(watcher);
            for (const auto &h : hits) {
                FixedSeedHit hit;
                hit.seed = seed;
                hit.x = (int)h.afkX;
                hit.z = (int)h.afkZ;
                hit.a = (int)h.rawArea;
                hit.b = (int)h.area;
                hit.c = h.chunks;
                emit hitFound(hit);
            }
        } else if (mode == FS_RIVER) {
            RiverSearchConfig cfg;
            cfg.seed = seed;
            cfg.mc = wi.mc;
            cfg.startX = minX;
            cfg.startZ = minZ;
            cfg.sx = sx;
            cfg.sz = sz;
            cfg.y = sampleY;
            cfg.minArea = minArea;
            cfg.threads = threads;
            RiverProgress prog;
            std::thread watcher([&]() {
                while (!stop.load()) {
                    FS_SYNC_PROG(prog);
                    int ph = prog.phase.load();
                    if (ph < 1)
                        ph = 1;
                    QString status = ph >= 2 ? tr("Refine") : tr("Coarse scan");
                    emit progress(FS_RIVER, prog.current.load(), std::max(1, prog.total.load()),
                                  ph, phaseCount, status);
                    QThread::msleep(100);
                    if (ph >= 2 && prog.total.load() > 0 && prog.current.load() >= prog.total.load())
                        break;
                }
                FS_SYNC_PROG(prog);
            });
            std::vector<RiverHit> hits;
            runRiverSearch(cfg, &prog, hits);
            joinWatcher(watcher);
            for (const auto &h : hits) {
                FixedSeedHit hit;
                hit.seed = seed;
                hit.x = h.x;
                hit.z = h.z;
                hit.a = h.area;
                emit hitFound(hit);
            }
        } else if (mode == FS_CAVE) {
            CaveSearchConfig cfg;
            cfg.seed = seed;
            cfg.mc = wi.mc;
            cfg.startX = minX;
            cfg.startZ = minZ;
            cfg.sx = sx;
            cfg.sz = sz;
            cfg.y = -56;
            cfg.minArea = minArea;
            cfg.threads = threads;
            cfg.fastSearch = caveFastSearch;
            cfg.riverFactor = caveRiverFactor;
            CaveProgress prog;
            std::thread watcher([&]() {
                while (!stop.load()) {
                    FS_SYNC_PROG(prog);
                    int ph = prog.phase1.load();
                    if (ph < 0)
                        break;
                    if (ph < 1)
                        ph = 1;
                    QString status = ph >= 2 ? tr("Refine") : tr("Coarse scan");
                    emit progress(FS_CAVE, prog.current.load(), std::max(1, prog.total.load()),
                                  ph, phaseCount, status);
                    QThread::msleep(100);
                    if (ph >= 2 && prog.total.load() > 0 && prog.current.load() >= prog.total.load())
                        break;
                }
                FS_SYNC_PROG(prog);
            });
            std::vector<CaveHit> hits;
            runCaveSearch(cfg, &prog, hits);
            joinWatcher(watcher);
            for (const auto &h : hits) {
                FixedSeedHit hit;
                hit.seed = seed;
                hit.x = h.x;
                hit.z = h.z;
                hit.a = h.area;
                hit.b = h.caveArea;
                hit.c = h.riverArea;
                emit hitFound(hit);
            }
        } else if (mode == FS_WITCH_HUT) {
            WitchHutSearchConfig cfg;
            cfg.seed = seed;
            cfg.mc = wi.mc;
            cfg.minX = minX;
            cfg.maxX = maxX;
            cfg.minZ = minZ;
            cfg.maxZ = maxZ;
            cfg.maxY = witchMaxY;
            cfg.threads = threads;
            WitchHutProgress prog;
            std::thread watcher([&]() {
                while (!stop.load()) {
                    FS_SYNC_PROG(prog);
                    emit progress(FS_WITCH_HUT, prog.current.load(), std::max(1, prog.total.load()),
                                  1, phaseCount, tr("Witch hut scan"));
                    QThread::msleep(100);
                    if (prog.total.load() > 0 && prog.current.load() >= prog.total.load())
                        break;
                }
                FS_SYNC_PROG(prog);
            });
            std::vector<WitchHutHit> hits;
            std::string hutErr;
            if (!runWitchHutSearch(cfg, &prog, hits, &hutErr)) {
                joinWatcher(watcher);
                emit searchError(tr("Witch hut session init failed: %1")
                                     .arg(QString::fromStdString(hutErr)));
                completed = false;
                break;
            }
            joinWatcher(watcher);
            for (const auto &h : hits) {
                FixedSeedHit hit;
                hit.seed = seed;
                hit.x = h.x;
                hit.y = h.y;
                hit.z = h.z;
                emit hitFound(hit);
            }
        } else if (mode == FS_FORT_CROSS || mode == FS_FORT_SPAN) {
            FortressSearchConfig cfg;
            cfg.mode = fortressMode;
            cfg.mc = wi.mc;
            cfg.seed = seed;
            cfg.centerX = (minX + maxX) / 2;
            cfg.centerZ = (minZ + maxZ) / 2;
            cfg.r = std::max(std::abs(maxX - minX), std::abs(maxZ - minZ)) / 2;
            cfg.crossFilter = crossFilter;
            cfg.minLong = minLong;
            cfg.minShort = minShort;
            cfg.minHeight = minHeight;
            FortressProgress prog;
            fortressControlReset();
            ThreadSafeResults<FortressHit> out;
            std::thread watcher([&]() {
                while (!stop.load()) {
                    prog.try_pause = pause.load();
                    prog.try_stop = stop.load();
                    if (pause.load())
                        fortressControlPause();
                    else
                        fortressControlResume();
                    if (stop.load())
                        fortressControlStop();
                    emit progress((int)mode, prog.current.load(), std::max(1, prog.total.load()),
                                  1, phaseCount, tr("Fortress search"));
                    QThread::msleep(100);
                    if (prog.total.load() > 0 && prog.current.load() >= prog.total.load()
                        && prog.phase1.load() < 0)
                        break;
                }
                if (stop.load())
                    fortressControlStop();
            });
            runFortressSearch(cfg, &prog, out, threads);
            joinWatcher(watcher);
            for (const FortressHit &h : out.getAllResults()) {
                FixedSeedHit hit;
                hit.seed = seed;
                hit.x = h.outX;
                hit.y = h.outY;
                hit.z = h.outZ;
                hit.a = h.shapeCode;
                hit.b = h.longEdge;
                hit.c = h.shortEdge;
                hit.d = h.heightEdge;
                if (h.mode == 0) {
                    if (h.shapeCode == 1)
                        hit.extra = tr("double");
                    else if (h.shapeCode == 2)
                        hit.extra = tr("triple");
                    else if (h.shapeCode == 3)
                        hit.extra = tr("quad");
                } else if (h.mode == 1) {
                    hit.extra = QString("%1x%2x%3").arg(h.longEdge).arg(h.heightEdge).arg(h.shortEdge);
                }
                emit hitFound(hit);
            }
        }

        if (stop.load()) {
            completed = false;
            break;
        }
    }

    emit finishedOk(completed && !stop.load());
#undef FS_SYNC_PROG
}

TabFixedSeed::TabFixedSeed(MainWindow *parent)
    : QWidget(parent)
    , ui(new Ui::TabFixedSeed)
    , parent(parent)
{
    ui->setupUi(this);
    qRegisterMetaType<FixedSeedHit>("FixedSeedHit");

    reorderToolTabs();
    updateWitchMaxYOptions();

    wrapProgressBarsWithTime();
    connect(&timeTimer, &QTimer::timeout, this, &TabFixedSeed::onTimeTick);

    connect(&worker, &FixedSeedWorker::progress, this, &TabFixedSeed::onWorkerProgress);
    connect(&worker, &FixedSeedWorker::hitFound, this, &TabFixedSeed::onWorkerHit);
    connect(&worker, &FixedSeedWorker::searchError, this, &TabFixedSeed::onWorkerSearchError);
    connect(&worker, &FixedSeedWorker::finishedOk, this, &TabFixedSeed::onWorkerFinished);

    auto connectTree = [&](QTreeWidget *t, int mode) {
        t->setProperty("fsMode", mode);
        t->header()->setSectionResizeMode(QHeaderView::Interactive);
        t->header()->setStretchLastSection(false);
        t->setUniformRowHeights(true);
        // sortingEnabled makes the header clickable and draws the arrow (same as Structures).
        t->setSortingEnabled(true);
        t->header()->setSectionsClickable(true);
        t->header()->setSortIndicatorShown(true);
        t->sortByColumn(-1, Qt::AscendingOrder);
        connect(t, &QTreeWidget::itemClicked, this, &TabFixedSeed::onTreeItemClicked);
        connect(t->header(), &QHeaderView::sectionClicked, this, [this, t, mode](int section) {
            onHeaderClicked(t, mode, section);
        });
    };
    connectTree(ui->treeMonument, FS_MONUMENT);
    connectTree(ui->treeSlime, FS_SLIME);
    connectTree(ui->treeRiver, FS_RIVER);
    connectTree(ui->treeCave, FS_CAVE);
    connectTree(ui->treeWitchHut, FS_WITCH_HUT);
    connectTree(ui->treeFortCross, FS_FORT_CROSS);
    connectTree(ui->treeFortSpan, FS_FORT_SPAN);

    auto upd = [this](const QString &) { updateThresholdUi(); };
    connect(ui->lineX1, &QLineEdit::textChanged, this, upd);
    connect(ui->lineZ1, &QLineEdit::textChanged, this, upd);
    connect(ui->lineX2, &QLineEdit::textChanged, this, upd);
    connect(ui->lineZ2, &QLineEdit::textChanged, this, upd);

    squareCenterX = (ui->lineX1->text().toInt() + ui->lineX2->text().toInt()) / 2;
    squareCenterZ = (ui->lineZ1->text().toInt() + ui->lineZ2->text().toInt()) / 2;
    updateSquareUi();
    updateThresholdUi();
    applyTreeColumnWidths();
}

TabFixedSeed::~TabFixedSeed()
{
    timeTimer.stop();
    worker.stop = true;
    worker.pause = false;
    sr_control_stop();
    fortressControlStop();
    worker.wait(5000);
    delete ui;
}

bool TabFixedSeed::isSearchRunning() const
{
    return worker.isRunning();
}

void TabFixedSeed::setExternalSearchRunning(bool running)
{
    if (!worker.isRunning())
        ui->pushStart->setEnabled(!running);
}

void TabFixedSeed::requestStop()
{
    on_pushStop_clicked();
}

void TabFixedSeed::save(QSettings &settings)
{
    settings.setValue("fixedseed/x1", ui->lineX1->text().toInt());
    settings.setValue("fixedseed/z1", ui->lineZ1->text().toInt());
    settings.setValue("fixedseed/x2", ui->lineX2->text().toInt());
    settings.setValue("fixedseed/z2", ui->lineZ2->text().toInt());
    settings.setValue("fixedseed/threads", ui->spinThreads->value());
    settings.setValue("fixedseed/seedsrc", ui->comboSeedSource->currentIndex());
    settings.setValue("fixedseed/tool", ui->tabTools->currentIndex());
    settings.setValue("fixedseed/square", ui->checkSquare->isChecked());
    settings.setValue("fixedseed/squareSide", ui->spinSquareSide->value());
    settings.setValue("fixedseed/squareCX", squareCenterX);
    settings.setValue("fixedseed/squareCZ", squareCenterZ);
    settings.setValue("fixedseed/monumentBiome", ui->checkMonumentBiome->isChecked());
    settings.setValue("fixedseed/slimeThr", ui->spinSlimeThreshold->value());
    settings.setValue("fixedseed/slimeMin", ui->spinSlimeMinArea->value());
    settings.setValue("fixedseed/slimeBiome", ui->checkSlimeBiomeConv->isChecked());
    settings.setValue("fixedseed/riverMin", ui->spinRiverMinArea->value());
    settings.setValue("fixedseed/caveMin", ui->spinCaveMinArea->value());
    settings.setValue("fixedseed/caveFast", ui->checkCaveFastSearch->isChecked());
    settings.setValue("fixedseed/caveRiverFactor", ui->doubleCaveRiverFactor->value());
    settings.setValue("fixedseed/witchMaxY", selectedWitchMaxY());
    settings.setValue("fixedseed/crossFilter", ui->comboCrossFilter->currentIndex());
    settings.setValue("fixedseed/minLong", ui->spinMinLong->value());
    settings.setValue("fixedseed/minShort", ui->spinMinShort->value());
    settings.setValue("fixedseed/minHeight", ui->spinMinHeight->value());
}

void TabFixedSeed::load(QSettings &settings)
{
    loadLine(&settings, ui->lineX1, "fixedseed/x1");
    loadLine(&settings, ui->lineZ1, "fixedseed/z1");
    loadLine(&settings, ui->lineX2, "fixedseed/x2");
    loadLine(&settings, ui->lineZ2, "fixedseed/z2");
    if (settings.contains("fixedseed/threads"))
        ui->spinThreads->setValue(settings.value("fixedseed/threads").toInt());
    ui->comboSeedSource->setCurrentIndex(settings.value("fixedseed/seedsrc", 0).toInt());
    ui->tabTools->setCurrentIndex(settings.value("fixedseed/tool", 0).toInt());
    ui->spinSquareSide->setValue(settings.value("fixedseed/squareSide", 20000).toInt());
    squareCenterX = settings.value("fixedseed/squareCX",
        (ui->lineX1->text().toInt() + ui->lineX2->text().toInt()) / 2).toInt();
    squareCenterZ = settings.value("fixedseed/squareCZ",
        (ui->lineZ1->text().toInt() + ui->lineZ2->text().toInt()) / 2).toInt();
    ui->checkMonumentBiome->setChecked(settings.value("fixedseed/monumentBiome", true).toBool());
    ui->spinSlimeThreshold->setValue(settings.value("fixedseed/slimeThr", 40).toInt());
    ui->spinSlimeMinArea->setValue(settings.value("fixedseed/slimeMin", 10000).toInt());
    ui->checkSlimeBiomeConv->setChecked(settings.value("fixedseed/slimeBiome", false).toBool());
    ui->spinRiverMinArea->setValue(settings.value("fixedseed/riverMin", 30000).toInt());
    ui->spinCaveMinArea->setValue(settings.value("fixedseed/caveMin", 40000).toInt());
    ui->checkCaveFastSearch->setChecked(settings.value("fixedseed/caveFast", true).toBool());
    ui->doubleCaveRiverFactor->setValue(settings.value("fixedseed/caveRiverFactor", 0.75).toDouble());
    {
        const int maxY = settings.value("fixedseed/witchMaxY", -40).toInt();
        updateWitchMaxYOptions();
        int idx = ui->comboWitchMaxY->findData(maxY);
        if (idx < 0)
            idx = ui->comboWitchMaxY->findData(-40);
        if (idx >= 0)
            ui->comboWitchMaxY->setCurrentIndex(idx);
    }
    ui->comboCrossFilter->setCurrentIndex(settings.value("fixedseed/crossFilter", 0).toInt());
    ui->spinMinLong->setValue(settings.value("fixedseed/minLong", 0).toInt());
    ui->spinMinShort->setValue(settings.value("fixedseed/minShort", 240).toInt());
    ui->spinMinHeight->setValue(settings.value("fixedseed/minHeight", 0).toInt());
    ui->checkSquare->blockSignals(true);
    ui->checkSquare->setChecked(settings.value("fixedseed/square", false).toBool());
    ui->checkSquare->blockSignals(false);
    if (ui->checkSquare->isChecked())
        applySquareToCoords();
    updateSquareUi();
    updateThresholdUi();
}

void TabFixedSeed::on_buttonFromVisible_clicked()
{
    int x1, z1, x2, z2;
    parent->getMapView()->getVisible(&x1, &z1, &x2, &z2);
    ui->lineX1->setText(QString::number(x1));
    ui->lineZ1->setText(QString::number(z1));
    ui->lineX2->setText(QString::number(x2));
    ui->lineZ2->setText(QString::number(z2));
    squareCenterX = (x1 + x2) / 2;
    squareCenterZ = (z1 + z2) / 2;
    updateThresholdUi();
}

void TabFixedSeed::on_checkSquare_toggled(bool checked)
{
    if (checked) {
        // LowY*: square side is always centered on world origin (0,0).
        squareCenterX = 0;
        squareCenterZ = 0;
        applySquareToCoords();
    }
    updateSquareUi();
    updateThresholdUi();
}

void TabFixedSeed::on_spinSquareSide_valueChanged(int)
{
    if (updatingSquare)
        return;
    if (ui->checkSquare->isChecked())
        applySquareToCoords();
    updateThresholdUi();
}

void TabFixedSeed::on_comboSeedSource_currentIndexChanged(int)
{
    updateThresholdUi();
    updateWitchMaxYOptions();
}

void TabFixedSeed::on_tabTools_currentChanged(int index)
{
    if (QTreeWidget *tree = treeForMode(index)) {
        if (tree->viewport()->width() > 80 && tree->columnWidth(0) < 40)
            applyTreeColumnWidths();
    }
}

void TabFixedSeed::applySquareToCoords()
{
    int side = ui->spinSquareSide->value();
    if (side < 1)
        side = 1;
    int half = side / 2;
    // Always origin-centered while square mode is active (matches LowYSwampHut / LowYDripstone).
    squareCenterX = 0;
    squareCenterZ = 0;
    updatingSquare = true;
    ui->lineX1->setText(QString::number(-half));
    ui->lineZ1->setText(QString::number(-half));
    ui->lineX2->setText(QString::number(-half + side - 1));
    ui->lineZ2->setText(QString::number(-half + side - 1));
    updatingSquare = false;
}

void TabFixedSeed::updateSquareUi()
{
    bool locked = ui->pushStop->isEnabled();
    bool sq = ui->checkSquare->isChecked();
    bool editArea = !locked && !sq;
    ui->lineX1->setEnabled(editArea);
    ui->lineZ1->setEnabled(editArea);
    ui->lineX2->setEnabled(editArea);
    ui->lineZ2->setEnabled(editArea);
    ui->buttonFromVisible->setEnabled(editArea);
    ui->spinSquareSide->setEnabled(!locked && sq);
    ui->labelSquareSide->setEnabled(!locked && sq);
    ui->checkSquare->setEnabled(!locked);
}

double TabFixedSeed::equivalentHalfSide() const
{
    int x1 = ui->lineX1->text().toInt();
    int z1 = ui->lineZ1->text().toInt();
    int x2 = ui->lineX2->text().toInt();
    int z2 = ui->lineZ2->text().toInt();
    double w = std::abs(x2 - x1) + 1.0;
    double h = std::abs(z2 - z1) + 1.0;
    return std::sqrt(w * h) / 2.0;
}

int TabFixedSeed::autoSlimeThreshold() const
{
    double R = equivalentHalfSide();
    if (R <= 10000) return 40;
    if (R <= 100000) return 45;
    if (R <= 10000000) return 50;
    return 55;
}

void TabFixedSeed::updateThresholdUi()
{
    // Threshold is user-adjustable in [40, 60] for both single- and multi-seed.
    const int v = ui->spinSlimeThreshold->value();
    ui->spinSlimeThreshold->setMinimum(40);
    ui->spinSlimeThreshold->setMaximum(60);
    ui->spinSlimeThreshold->setReadOnly(false);
    ui->spinSlimeThreshold->setEnabled(true);
    if (v < 40)
        ui->spinSlimeThreshold->setValue(40);
    else if (v > 60)
        ui->spinSlimeThreshold->setValue(60);
}

void TabFixedSeed::gatherSeeds(std::vector<uint64_t> *out)
{
    out->clear();
    WorldInfo wi;
    parent->getSeed(&wi);
    if (ui->comboSeedSource->currentIndex() == 0) {
        out->push_back(wi.seed);
    } else {
        *out = parent->formControl->getResults();
        if (out->empty())
            out->push_back(wi.seed);
    }
}

QProgressBar *TabFixedSeed::progressBarForMode(int mode) const
{
    switch (mode) {
    case FS_MONUMENT: return ui->progressMonument;
    case FS_SLIME: return ui->progressSlime;
    case FS_RIVER: return ui->progressRiver;
    case FS_CAVE: return ui->progressCave;
    case FS_WITCH_HUT: return ui->progressWitchHut;
    case FS_FORT_CROSS: return ui->progressFortCross;
    case FS_FORT_SPAN: return ui->progressFortSpan;
    default: return nullptr;
    }
}

QTreeWidget *TabFixedSeed::treeForMode(int mode) const
{
    switch (mode) {
    case FS_MONUMENT: return ui->treeMonument;
    case FS_SLIME: return ui->treeSlime;
    case FS_RIVER: return ui->treeRiver;
    case FS_CAVE: return ui->treeCave;
    case FS_WITCH_HUT: return ui->treeWitchHut;
    case FS_FORT_CROSS: return ui->treeFortCross;
    case FS_FORT_SPAN: return ui->treeFortSpan;
    default: return nullptr;
    }
}

void TabFixedSeed::wrapProgressBarsWithTime()
{
    // Must match FixedSeedMode enum order (not UI file order).
    QProgressBar *bars[FS_MODE_COUNT] = {
        ui->progressWitchHut,   // FS_WITCH_HUT = 0
        ui->progressSlime,      // FS_SLIME = 1
        ui->progressCave,       // FS_CAVE = 2
        ui->progressRiver,      // FS_RIVER = 3
        ui->progressMonument,   // FS_MONUMENT = 4
        ui->progressFortCross,  // FS_FORT_CROSS = 5
        ui->progressFortSpan,   // FS_FORT_SPAN = 6
    };
    for (int i = 0; i < FS_MODE_COUNT; i++) {
        wrapOneProgressBar(bars[i], &labelElapsed[i], &labelRemain[i]);
        if (labelElapsed[i])
            labelElapsed[i]->setToolTip(tr("Elapsed (pause excluded)"));
        if (labelRemain[i])
            labelRemain[i]->setToolTip(tr("Estimated time remaining"));
        if (bars[i]) {
            bars[i]->setValue(0);
            bars[i]->setFormat(tr("Ready"));
        }
        resetTimeLabels(i);
    }
}

QString TabFixedSeed::formatDuration(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 s = ms / 1000;
    const int sec = (int)(s % 60);
    const int min = (int)((s / 60) % 60);
    const qint64 hrs = s / 3600;
    if (hrs > 0) {
        // H:MM:SS once elapsed reaches one hour
        return QStringLiteral("%1:%2:%3")
            .arg(hrs)
            .arg(min, 2, 10, QLatin1Char('0'))
            .arg(sec, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(min)
        .arg(sec, 2, 10, QLatin1Char('0'));
}

qint64 TabFixedSeed::activeElapsedMs() const
{
    if (!searchClock.isValid())
        return 0;
    qint64 ms = searchClock.elapsed() - pausedMs;
    if (pauseClock.isValid())
        ms -= pauseClock.elapsed();
    return ms > 0 ? ms : 0;
}

void TabFixedSeed::resetTimeLabels(int mode)
{
    if (mode < 0 || mode >= FS_MODE_COUNT)
        return;
    if (labelElapsed[mode])
        labelElapsed[mode]->setText(tr("Elapsed %1").arg(QStringLiteral("0:00")));
    if (labelRemain[mode])
        labelRemain[mode]->setText(tr("ETA %1").arg(QStringLiteral("--:--")));
}

void TabFixedSeed::updateTimeLabels()
{
    if (progMode < 0 || progMode >= FS_MODE_COUNT)
        return;
    QLabel *el = labelElapsed[progMode];
    QLabel *rm = labelRemain[progMode];
    if (!el || !rm)
        return;

    const qint64 elapsedMs = activeElapsedMs();
    el->setText(tr("Elapsed %1").arg(formatDuration(elapsedMs)));

    QString eta = QStringLiteral("--:--");
    const int cur = std::max(0, progCurrent);
    const int tot = std::max(1, progTotal);
    const int phaseCount = phaseCountForMode(progMode);
    const int phase = std::max(1, std::min(lastPhase > 0 ? lastPhase : 1, phaseCount));
    // Overall fraction across phases so ETA does not reset / hit 0 at phase boundaries.
    const double frac = phaseCount > 1
        ? ((double)(phase - 1) + (double)cur / (double)tot) / (double)phaseCount
        : (double)cur / (double)tot;
    if (frac >= 0.999) {
        eta = formatDuration(0);
    } else if (frac > 0.001 && elapsedMs >= 500) {
        const qint64 remainMs = (qint64)(elapsedMs * (1.0 - frac) / frac);
        eta = formatDuration(remainMs);
    }
    rm->setText(tr("ETA %1").arg(eta));
}

void TabFixedSeed::onTimeTick()
{
    updateTimeLabels();
}

void TabFixedSeed::setProgressFormat(QProgressBar *bar, int current, int total, int phase,
                                     int phaseCount, const QString &status, bool finished)
{
    if (!bar)
        return;
    bar->setTextVisible(true);
    // Use fullwidth ％ so QProgressBar never treats it as %p/%v/%m/%% escapes.
    static const QChar kPct(0xFF05);
    if (finished) {
        bar->setValue(bar->maximum() > 0 ? bar->maximum() : 1);
        if (phaseCount > 1 && !status.isEmpty())
            bar->setFormat(tr("Phase %1: %2 done").arg(phase).arg(status));
        else if (phaseCount > 1)
            bar->setFormat(tr("Phase %1: done").arg(phase));
        else
            bar->setFormat(tr("Done"));
        return;
    }
    total = std::max(1, total);
    current = std::max(0, current);
    bar->setMaximum(total);
    bar->setValue(std::min(current, total));
    const int pct = (100 * current) / total;
    if (phaseCount > 1) {
        bar->setFormat(tr("Phase %1: %2 %3/%4=%5%6")
                           .arg(phase)
                           .arg(status)
                           .arg(current)
                           .arg(total)
                           .arg(pct)
                           .arg(kPct));
    } else if (!status.isEmpty()) {
        bar->setFormat(tr("%1 %2/%3=%4%5")
                           .arg(status)
                           .arg(current)
                           .arg(total)
                           .arg(pct)
                           .arg(kPct));
    } else {
        bar->setFormat(QStringLiteral("%1/%2=%3%4")
                           .arg(current)
                           .arg(total)
                           .arg(pct)
                           .arg(kPct));
    }
}

void TabFixedSeed::clearResultsForMode(int mode)
{
    if (QTreeWidget *tree = treeForMode(mode))
        tree->clear();
    if (QProgressBar *bar = progressBarForMode(mode)) {
        bar->setValue(0);
        bar->setFormat(tr("Ready"));
    }
    resetTimeLabels(mode);
}

void TabFixedSeed::setControlsLocked(bool running)
{
    ui->pushStart->setEnabled(!running);
    ui->pushPause->setEnabled(running);
    ui->pushStop->setEnabled(running);
    ui->spinThreads->setEnabled(!running);
    ui->tabTools->setEnabled(!running);
    ui->comboSeedSource->setEnabled(!running);
    updateSquareUi();
}

void TabFixedSeed::on_pushStart_clicked()
{
    if (worker.isRunning())
        return;

    if (ui->checkSquare->isChecked())
        applySquareToCoords();

    updateThresholdUi();
    gatherSeeds(&worker.seeds);
    if (worker.seeds.empty()) {
        warn(this, tr("No seeds to search."));
        return;
    }

    parent->getSeed(&worker.wi);
    worker.x1 = ui->lineX1->text().toInt();
    worker.z1 = ui->lineZ1->text().toInt();
    worker.x2 = ui->lineX2->text().toInt();
    worker.z2 = ui->lineZ2->text().toInt();
    worker.threads = ui->spinThreads->value();
    worker.mode = (FixedSeedMode)ui->tabTools->currentIndex();

    {
        const qint64 minX = std::min(worker.x1, worker.x2);
        const qint64 maxX = std::max(worker.x1, worker.x2);
        const qint64 minZ = std::min(worker.z1, worker.z2);
        const qint64 maxZ = std::max(worker.z1, worker.z2);
        const qint64 sx = maxX - minX + 1;
        const qint64 sz = maxZ - minZ + 1;
        // Guard against overflow on huge ranges.
        const bool areaOverflow = sx > 0 && sz > 0 && sx > (std::numeric_limits<qint64>::max() / sz);
        const qint64 area = areaOverflow ? std::numeric_limits<qint64>::max() : sx * sz;
        const qint64 monumentLimit = (qint64)1048576 * (qint64)1048576 * 4;
        const qint64 fortCrossLimit = (qint64)100001 * (qint64)100001;
        if ((worker.mode == FS_MONUMENT && area > monumentLimit)
            || (worker.mode == FS_FORT_SPAN && area > monumentLimit)
            || (worker.mode == FS_FORT_CROSS && area > fortCrossLimit)) {
            warn(this, tr("Search area is too large and would produce too many results; please shrink the search area."));
            return;
        }
    }

    worker.slimeThreshold = ui->spinSlimeThreshold->value();
    worker.slimeMinArea = ui->spinSlimeMinArea->value();
    worker.slimeBiomeConv = ui->checkSlimeBiomeConv->isChecked();
    worker.monumentBiomeCheck = ui->checkMonumentBiome->isChecked();
    worker.sampleY = worker.wi.y;
    if (worker.mode == FS_CAVE) {
        worker.minArea = ui->spinCaveMinArea->value();
        worker.caveFastSearch = ui->checkCaveFastSearch->isChecked();
        worker.caveRiverFactor = (float)ui->doubleCaveRiverFactor->value();
    } else if (worker.mode == FS_WITCH_HUT) {
        worker.witchMaxY = selectedWitchMaxY();
    } else
        worker.minArea = ui->spinRiverMinArea->value();
    worker.fortressMode = (worker.mode == FS_FORT_SPAN) ? 1 : 0;
    worker.crossFilter = ui->comboCrossFilter->currentIndex() == 0 ? 2
                        : (ui->comboCrossFilter->currentIndex() == 1 ? 3 : 4);
    worker.minLong = ui->spinMinLong->value();
    worker.minShort = ui->spinMinShort->value();
    worker.minHeight = ui->spinMinHeight->value();
    worker.stop = false;
    worker.pause = false;

    clearResultsForMode((int)worker.mode);

    progMode = (int)worker.mode;
    progCurrent = 0;
    progTotal = 1;
    pausedMs = 0;
    pauseClock.invalidate();
    searchClock.start();
    timeTimer.start(250);
    updateTimeLabels();

    setControlsLocked(true);
    ui->labelStatus->setText(tr("Searching..."));
    ui->pushPause->setText(tr("Pause"));
    emit fixedSearchStatusChanged(true);
    worker.start();
}

void TabFixedSeed::on_pushPause_clicked()
{
    if (!worker.isRunning())
        return;
    worker.pause = !worker.pause.load();
    if (worker.pause.load()) {
        if (!pauseClock.isValid())
            pauseClock.start();
        ui->pushPause->setText(tr("Resume"));
        ui->labelStatus->setText(tr("Paused"));
    } else {
        if (pauseClock.isValid()) {
            pausedMs += pauseClock.elapsed();
            pauseClock.invalidate();
        }
        ui->pushPause->setText(tr("Pause"));
        ui->labelStatus->setText(tr("Searching..."));
    }
    updateTimeLabels();
}

void TabFixedSeed::on_pushStop_clicked()
{
    worker.stop = true;
    worker.pause = false;
    sr_control_stop();
    fortressControlStop();
    ui->labelStatus->setText(tr("Stopping..."));
}

void TabFixedSeed::on_pushClear_clicked()
{
    clearResultsForMode(ui->tabTools->currentIndex());
}

void TabFixedSeed::on_pushExport_clicked()
{
    QTreeWidget *tree = treeForMode(ui->tabTools->currentIndex());
    if (!tree || tree->topLevelItemCount() == 0)
        return;

    QString fnam = QFileDialog::getSaveFileName(this, tr("Export results"), parent->prevdir,
                                                tr("Text files (*.txt *.csv);;Any files (*)"));
    if (fnam.isEmpty())
        return;
    QFile file(fnam);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        warn(this, tr("Failed to open file for export."));
        return;
    }
    parent->prevdir = QFileInfo(fnam).absolutePath();
    QTextStream stream(&file);
    const int cols = tree->columnCount();
    QStringList headers;
    for (int c = 0; c < cols; c++)
        headers << tree->headerItem()->text(c);
    stream << headers.join('\t') << '\n';

    std::function<void(QTreeWidgetItem *)> writeItem = [&](QTreeWidgetItem *item) {
        if (!item)
            return;
        // Export leaf result rows only (children under a seed parent).
        if (item->childCount() == 0 && item->parent()) {
            QStringList colsText;
            for (int c = 0; c < cols; c++)
                colsText << item->text(c);
            // Prefer parent seed in column 0 when child shows "-".
            if (colsText.value(0) == QLatin1String("-") && item->parent())
                colsText[0] = item->parent()->text(0);
            stream << colsText.join('\t') << '\n';
        }
        for (int i = 0; i < item->childCount(); i++)
            writeItem(item->child(i));
    };
    for (int i = 0; i < tree->topLevelItemCount(); i++)
        writeItem(tree->topLevelItem(i));
}

class FixedSeedItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        QTreeWidget *tw = treeWidget();
        const int col = tw && tw->sortColumn() >= 0 ? tw->sortColumn() : 0;
        const int mode = tw ? tw->property("fsMode").toInt() : -1;
        const bool desc = tw && tw->header()->sortIndicatorOrder() == Qt::DescendingOrder;

        auto num = [](const QTreeWidgetItem *it, int c, bool *ok) -> qint64 {
            const QVariant v = it->data(c, Qt::UserRole);
            *ok = v.isValid();
            return v.toLongLong();
        };

        // Low-efficiency monument hits stay last in both directions.
        // Qt inverts operator< by swapping arguments when descending, so flip that key back.
        if (mode == FS_MONUMENT) {
            const int la = data(6, Qt::UserRole).toInt();
            const int lb = other.data(6, Qt::UserRole).toInt();
            if (la != lb)
                return desc ? lb < la : la < lb;
        }

        if (mode == FS_FORT_CROSS && col == 5) {
            bool oa = false, ob = false;
            const qint64 ta = num(this, 5, &oa);
            const qint64 tb = num(&other, 5, &ob);
            if (oa && ob && ta != tb)
                return ta < tb;
            bool da = false, db = false;
            const qint64 xa = num(this, 4, &da);
            const qint64 xb = num(&other, 4, &db);
            if (da && db && xa != xb)
                return desc ? xa > xb : xa < xb;
        }

        if (col == 0) {
            return data(0, Qt::UserRole).toULongLong() < other.data(0, Qt::UserRole).toULongLong();
        }

        bool oa = false, ob = false;
        const qint64 va = num(this, col, &oa);
        const qint64 vb = num(&other, col, &ob);
        if (oa && ob && va != vb)
            return va < vb;
        if (oa != ob)
            return oa;
        return text(col) < other.text(col);
    }
};

static void setSortNum(QTreeWidgetItem *item, int col, qint64 value)
{
    item->setText(col, QString::number(value));
    item->setData(col, Qt::UserRole, QVariant::fromValue(value));
}

QTreeWidgetItem *TabFixedSeed::seedParentItem(QTreeWidget *tree, uint64_t seed)
{
    for (int i = 0; i < tree->topLevelItemCount(); i++) {
        QTreeWidgetItem *it = tree->topLevelItem(i);
        if (it->data(0, Qt::UserRole).toULongLong() == seed)
            return it;
    }
    auto *parentItem = new FixedSeedItem(tree);
    parentItem->setText(0, QString::number((qint64)seed));
    parentItem->setData(0, Qt::UserRole, QVariant::fromValue(seed));
    parentItem->setExpanded(true);
    return parentItem;
}

void TabFixedSeed::applyTreeColumnWidths()
{
    struct Spec { int n; bool chars; };
    auto apply = [this](QTreeWidget *tree, int mode, const Spec *spec, int n) {
        if (!tree || colReady[mode] || n <= 0 || tree->columnCount() != n)
            return;
        if (tree->viewport()->width() < 40)
            return;
        const QFontMetrics fm(tree->font());
        const int pad = 12;
        QVector<int> base(n);
        int sum = 0;
        for (int i = 0; i < n; i++) {
            const QChar ch = spec[i].chars ? QLatin1Char('M') : QLatin1Char('0');
            base[i] = txtWidth(fm, QString(spec[i].n, ch)) + pad;
            sum += base[i];
        }
        const int avail = tree->viewport()->width();
        if (avail > sum && sum > 0) {
            int used = 0;
            for (int i = 0; i < n; i++) {
                const int w = (i == n - 1) ? (avail - used) : (base[i] * avail / sum);
                tree->setColumnWidth(i, qMax(base[i], w));
                used += tree->columnWidth(i);
            }
        } else {
            for (int i = 0; i < n; i++)
                tree->setColumnWidth(i, base[i]);
        }
        colReady[mode] = true;
    };

    const Spec monument[] = {{26, false}, {10, false}, {10, false}, {12, false}, {4, false}, {4, false}, {8, false}, {8, false}};
    const Spec slime[] = {{26, false}, {10, false}, {10, false}, {12, false}, {7, false}, {5, false}, {7, false}};
    const Spec xzArea[] = {{26, false}, {10, false}, {10, false}, {12, false}, {6, false}};
    const Spec cave[] = {{26, false}, {10, false}, {10, false}, {12, false}, {7, false}, {7, false}, {7, false}};
    const Spec witchHut[] = {{26, false}, {10, false}, {6, false}, {10, false}, {12, false}};
    const Spec fortCross[] = {{26, false}, {10, false}, {4, false}, {10, false}, {12, false}, {10, true}};
    const Spec fortSpan[] = {{26, false}, {10, false}, {10, false}, {12, false},
                             {5, false}, {5, false}, {5, false}, {5, false}};

    apply(ui->treeMonument, FS_MONUMENT, monument, 8);
    apply(ui->treeSlime, FS_SLIME, slime, 7);
    apply(ui->treeRiver, FS_RIVER, xzArea, 5);
    apply(ui->treeCave, FS_CAVE, cave, 7);
    apply(ui->treeWitchHut, FS_WITCH_HUT, witchHut, 5);
    apply(ui->treeFortCross, FS_FORT_CROSS, fortCross, 6);
    apply(ui->treeFortSpan, FS_FORT_SPAN, fortSpan, 8);

    // Match seed column width to monument tab (reference width).
    const int seedW = ui->treeMonument->columnWidth(0);
    if (seedW > 0) {
        for (QTreeWidget *t : {ui->treeSlime, ui->treeRiver, ui->treeCave,
                               ui->treeWitchHut, ui->treeFortCross, ui->treeFortSpan}) {
            if (t && t->columnWidth(0) > 0)
                t->setColumnWidth(0, seedW);
        }
    }
}

bool TabFixedSeed::event(QEvent *e)
{
    if (e->type() == QEvent::LayoutRequest || e->type() == QEvent::Show || e->type() == QEvent::Resize)
        applyTreeColumnWidths();
    return QWidget::event(e);
}

void TabFixedSeed::onWorkerProgress(int mode, int current, int total, int phase,
                                    int phaseCount, QString status)
{
    lastPhase = phase;
    lastStatus = status;
    progMode = mode;
    progCurrent = current;
    progTotal = std::max(1, total);
    setProgressFormat(progressBarForMode(mode), current, total, phase, phaseCount, status, false);
    updateTimeLabels();
    if (!worker.pause.load())
        ui->labelStatus->setText(status);
}

void TabFixedSeed::onHeaderClicked(QTreeWidget *tree, int mode, int section)
{
    if (!tree || mode < 0 || mode >= FS_MODE_COUNT || section < 0)
        return;
    // Qt already sorted this click. Third click on the same column clears the indicator.
    if (lastSortCol[mode] == section
        && tree->header()->sortIndicatorOrder() == Qt::AscendingOrder) {
        tree->sortByColumn(-1, Qt::DescendingOrder);
        lastSortCol[mode] = -1;
        return;
    }
    lastSortCol[mode] = tree->header()->sortIndicatorSection();
}

void TabFixedSeed::onWorkerHit(FixedSeedHit hit)
{
    QTreeWidget *tree = treeForMode((int)worker.mode);
    if (!tree)
        return;

    QTreeWidgetItem *parentItem = seedParentItem(tree, hit.seed);
    auto *item = new FixedSeedItem(parentItem);
    item->setText(0, QStringLiteral("-"));
    item->setData(0, Qt::UserRole, QVariant::fromValue(hit.seed));
    item->setData(0, Qt::UserRole + 1, hit.x);
    item->setData(0, Qt::UserRole + 2, hit.z);
    item->setData(0, Qt::UserRole + 3, hit.y);
    item->setData(0, Qt::UserRole + 4, true); // navigable result row

    const int dist = originDistance(hit.x, hit.z);
    switch (worker.mode) {
    case FS_MONUMENT:
        setSortNum(item, 1, hit.x);
        setSortNum(item, 2, hit.z);
        setSortNum(item, 3, dist);
        setSortNum(item, 4, hit.a);
        setSortNum(item, 5, hit.b);
        item->setText(6, hit.c ? tr("yes") : tr("no"));
        item->setData(6, Qt::UserRole, hit.c ? 1 : 0);
        item->setText(7, hit.extra.isEmpty() ? QStringLiteral("-") : hit.extra);
        break;
    case FS_SLIME:
        setSortNum(item, 1, hit.x);
        setSortNum(item, 2, hit.z);
        setSortNum(item, 3, dist);
        setSortNum(item, 4, hit.a); // raw area
        setSortNum(item, 5, hit.c); // chunks
        if (worker.slimeBiomeConv)
            setSortNum(item, 6, hit.b);
        else
            item->setText(6, QStringLiteral("-"));
        break;
    case FS_RIVER:
        setSortNum(item, 1, hit.x);
        setSortNum(item, 2, hit.z);
        setSortNum(item, 3, dist);
        setSortNum(item, 4, hit.a);
        break;
    case FS_CAVE:
        setSortNum(item, 1, hit.x);
        setSortNum(item, 2, hit.z);
        setSortNum(item, 3, dist);
        setSortNum(item, 4, hit.a); // total (converted)
        setSortNum(item, 5, hit.b); // cave biome
        setSortNum(item, 6, hit.c); // river biome
        break;
    case FS_WITCH_HUT:
        setSortNum(item, 1, hit.x);
        setSortNum(item, 2, hit.y);
        setSortNum(item, 3, hit.z);
        setSortNum(item, 4, dist);
        break;
    case FS_FORT_CROSS:
        setSortNum(item, 1, hit.x);
        setSortNum(item, 2, hit.y);
        setSortNum(item, 3, hit.z);
        setSortNum(item, 4, dist);
        item->setText(5, hit.extra);
        item->setData(5, Qt::UserRole, hit.a); // 1 double, 2 triple, 3 quad
        break;
    case FS_FORT_SPAN:
        setSortNum(item, 1, hit.x);
        setSortNum(item, 2, hit.z);
        setSortNum(item, 3, dist);
        setSortNum(item, 4, hit.b); // span X
        setSortNum(item, 5, hit.c); // span Z
        setSortNum(item, 6, hit.d); // height (span Y)
        setSortNum(item, 7, hit.b * hit.c); // area = spanX * spanZ
        break;
    default:
        delete item;
        return;
    }
    if (tree->header()->sortIndicatorSection() >= 0)
        tree->sortByColumn(tree->header()->sortIndicatorSection(), tree->header()->sortIndicatorOrder());
}

void TabFixedSeed::onWorkerFinished(bool completed)
{
    timeTimer.stop();
    if (pauseClock.isValid()) {
        pausedMs += pauseClock.elapsed();
        pauseClock.invalidate();
    }
    progCurrent = progTotal;
    updateTimeLabels();

    int mode = (int)worker.mode;
    int phaseCount = phaseCountForMode(mode);
    setProgressFormat(progressBarForMode(mode), 0, 1, lastPhase > 0 ? lastPhase : phaseCount,
                      phaseCount, lastStatus, true);
    setControlsLocked(false);
    ui->labelStatus->setText(completed ? tr("Done") : tr("Stopped"));
    ui->pushPause->setText(tr("Pause"));
    emit fixedSearchStatusChanged(false);
    updateThresholdUi();
}

void TabFixedSeed::onWorkerSearchError(const QString &message)
{
    warn(this, tr("Error"), message);
    ui->labelStatus->setText(tr("Error"));
}

void TabFixedSeed::onTreeItemClicked(QTreeWidgetItem *item, int)
{
    if (!item)
        return;

    uint64_t seed = item->data(0, Qt::UserRole).toULongLong();
    if (!seed && item->parent())
        seed = item->parent()->data(0, Qt::UserRole).toULongLong();

    WorldInfo wi;
    parent->getSeed(&wi);
    wi.seed = seed;
    int dim = (ui->tabTools->currentIndex() == FS_FORT_CROSS
               || ui->tabTools->currentIndex() == FS_FORT_SPAN)
                  ? DIM_NETHER
                  : DIM_OVERWORLD;
    parent->setSeed(wi, dim);

    const bool navigable = item->data(0, Qt::UserRole + 4).toBool();
    if (!navigable)
        return;

    int x = item->data(0, Qt::UserRole + 1).toInt();
    int z = item->data(0, Qt::UserRole + 2).toInt();
    parent->mapGoto(x + 0.5, z + 0.5, 0);
    parent->getMapView()->setAfkRange(x, z, true);
}

void TabFixedSeed::reorderToolTabs()
{
    QTabWidget *tw = ui->tabTools;
    struct TabSpec { QWidget *w; };
    const TabSpec order[] = {
        {ui->tabWitchHut},
        {ui->tabSlime},
        {ui->tabCave},
        {ui->tabRiver},
        {ui->tabMonument},
        {ui->tabFortCross},
        {ui->tabFortSpan},
    };
    QString titles[FS_MODE_COUNT];
    for (int i = 0; i < FS_MODE_COUNT; i++) {
        const int idx = tw->indexOf(order[i].w);
        titles[i] = idx >= 0 ? tw->tabText(idx) : QString();
    }
    while (tw->count() > 0)
        tw->removeTab(0);
    for (int i = 0; i < FS_MODE_COUNT; i++)
        tw->addTab(order[i].w, titles[i]);
}

int TabFixedSeed::selectedWitchMaxY() const
{
    const QVariant d = ui->comboWitchMaxY->currentData();
    if (d.isValid())
        return d.toInt();
    bool ok = false;
    const int v = ui->comboWitchMaxY->currentText().toInt(&ok);
    return ok ? v : -40;
}

void TabFixedSeed::updateWitchMaxYOptions()
{
    const bool listMode = ui->comboSeedSource->currentIndex() == 1;
    const int prev = selectedWitchMaxY();
    // LowYSwampHut: -54/-50 only in list search; single seed hides them.
    static const int kList[] = {-54, -50, -40, -30, -20, -10, 0};
    static const int kSingle[] = {-40, -30, -20, -10, 0};
    const int *vals = listMode ? kList : kSingle;
    const int n = listMode ? 7 : 5;

    ui->comboWitchMaxY->blockSignals(true);
    ui->comboWitchMaxY->clear();
    int sel = -1;
    for (int i = 0; i < n; i++) {
        ui->comboWitchMaxY->addItem(QString::number(vals[i]), vals[i]);
        if (vals[i] == prev)
            sel = i;
    }
    if (sel < 0) {
        for (int i = 0; i < n; i++) {
            if (vals[i] == -40) {
                sel = i;
                break;
            }
        }
        if (sel < 0)
            sel = 0;
    }
    ui->comboWitchMaxY->setCurrentIndex(sel);
    ui->comboWitchMaxY->blockSignals(false);
}
