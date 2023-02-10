#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "presetdialog.h"
#include "aboutdialog.h"
#include "conditiondialog.h"
#include "extgendialog.h"
#include "biomecolordialog.h"
#include "structuredialog.h"
#include "exportdialog.h"
#include "tabtriggers.h"
#include "tabbiomes.h"
#include "tabstructures.h"

#if WITH_UPDATER
#include "updater.h"
#endif

#include "world.h"
#include "cutil.h"

#include <QIntValidator>
#include <QMetaType>
#include <QtDebug>
#include <QDataStream>
#include <QMenu>
#include <QFileDialog>
#include <QTextStream>
#include <QSettings>
#include <QTreeWidget>
#include <QDateTime>
#include <QStandardPaths>
#include <QDebug>
#include <QFile>


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , dock(new QDockWidget(tr("地图"), this))
    , mapView(new MapView(this))
    , formCond()
    , formGen48()
    , formControl()
    , config()
    , prevdir(".")
    , autosaveTimer()
    , prevtab(-1)
    , dimactions{}
    , dimgroup()
{
    ui->setupUi(this);

    dock->setWidget(mapView);
    dock->setFeatures(QDockWidget::DockWidgetFloatable);
    QMainWindow *submain = new QMainWindow(this);
    ui->frameMap->layout()->addWidget(submain);
    submain->addDockWidget(Qt::LeftDockWidgetArea, dock);
    mapView->setFocusPolicy(Qt::StrongFocus);

    formCond = new FormConditions(this);
    formGen48 = new FormGen48(this);
    formControl = new FormSearchControl(this);

    ui->tabContainer->addTab(new TabTriggers(this), tr("触发器"));
    ui->tabContainer->addTab(new TabBiomes(this), tr("群系"));
    ui->tabContainer->addTab(new TabStructures(this), tr("结构"));

    QList<QAction*> layeracts = ui->menuBiome_layer->actions();
    int lopt = LOPT_DEFAULT_1;
    for (int i = 0; i < layeracts.size(); i++)
    {
        if (layeracts[i]->isSeparator())
            continue;
        connect(layeracts[i], &QAction::toggled,
            [=](bool state) {
                this->onActionBiomeLayerSelect(state, layeracts[i], lopt);
            });
        lopt++;
    }

    QAction *toorigin = new QAction(QIcon(":/icons/origin.png"), "前往坐标原点并重置缩放比例", this);
    connect(toorigin, &QAction::triggered, [=](){ this->mapGoto(0,0,16); });
    ui->toolBar->addAction(toorigin);
    ui->toolBar->addSeparator();

    dimactions[0] = addMapAction(-1, "overworld", tr("主世界"));
    dimactions[1] = addMapAction(-1, "nether", tr("下界"));
    dimactions[2] = addMapAction(-1, "the_end", tr("末地"));
    dimgroup = new QActionGroup(this);

    for (int i = 0; i < 3; i++)
    {
        connect(dimactions[i], &QAction::triggered, [=](){ this->updateMapSeed(); });
        ui->toolBar->addAction(dimactions[i]);
        dimgroup->addAction(dimactions[i]);
    }
    dimactions[0]->setChecked(true);
    ui->toolBar->addSeparator();

    saction.resize(STRUCT_NUM);
    addMapAction(D_GRID, "grid", tr("展示网格"));
    addMapAction(D_SLIME, "slime", tr("展示史莱姆区块"));
    addMapAction(D_SPAWN, "spawn", tr("展示世界出生点"));
    addMapAction(D_STRONGHOLD, "stronghold", tr("展示要塞"));
    addMapAction(D_VILLAGE, "village", tr("展示村庄"));
    addMapAction(D_MINESHAFT, "mineshaft", tr("展示废弃矿井"));
    addMapAction(D_DESERT, "desert", tr("展示沙漠神殿"));
    addMapAction(D_JUNGLE, "jungle", tr("展示丛林神庙"));
    addMapAction(D_HUT, "hut", tr("展示沼泽小屋"));
    addMapAction(D_MONUMENT, "monument", tr("展示海底神殿"));
    addMapAction(D_IGLOO, "igloo", tr("展示冰屋"));
    addMapAction(D_MANSION, "mansion", tr("展示林地府邸"));
    addMapAction(D_RUINS, "ruins", tr("展示海底遗迹"));
    addMapAction(D_SHIPWRECK, "shipwreck", tr("展示沉船"));
    addMapAction(D_TREASURE, "treasure", tr("展示宝藏"));
    addMapAction(D_OUTPOST, "outpost", tr("展示前哨站"));
    addMapAction(D_PORTAL, "portal", tr("展示废弃传送门"));
    addMapAction(D_ANCIENTCITY, "ancient_city", tr("展示远古城市"));
    ui->toolBar->addSeparator();
    addMapAction(D_FORTESS, "fortress", tr("展示下界要塞"));
    addMapAction(D_BASTION, "bastion", tr("展示堡垒遗迹"));
    ui->toolBar->addSeparator();
    addMapAction(D_ENDCITY, "endcity", tr("展示末地城"));
    addMapAction(D_GATEWAY, "gateway", tr("展示末地折跃门"));

    saction[D_GRID]->setChecked(true);

    ui->splitterMap->setSizes(QList<int>({6000, 10000}));
    ui->splitterSearch->setSizes(QList<int>({1000, 1000, 2000}));

    qRegisterMetaType< int64_t >("int64_t");
    qRegisterMetaType< uint64_t >("uint64_t");
    qRegisterMetaType< QVector<uint64_t> >("QVector<uint64_t>");
    qRegisterMetaType< Config >("Config");

    ui->comboY->lineEdit()->setValidator(new QIntValidator(-64, 320, this));

    formCond->updateSensitivity();

    connect(&autosaveTimer, &QTimer::timeout, this, &MainWindow::onAutosaveTimeout);

    loadSettings();

    ui->collapseConstraints->init(tr("条件"), formCond, false);
    connect(formCond, &FormConditions::changed, this, &MainWindow::onConditionsChanged);
    ui->collapseConstraints->setInfo(
        tr("Help: Conditions"),
        tr(
        "<html><head/><body><p>"
        "The search conditions define the properties by which potential seeds "
        "are filtered."
        "</p><p>"
        "Conditions can reference each other to produce relative positionial "
        "dependencies (indicated with the ID in square brackets [XY]). These "
        "will usually be checked at the geometric <b>average position</b> of the "
        "parent trigger. When multiple trigger positions are encountered in the "
        "same seed <b>and the required instance count is exactly one</b>, the "
        "instances are checked individually instead."
        "</p><p>"
        "Biome conditions <b>do not have trigger instances</b> and always yield "
        "the center point of the testing area. You can use reference point "
        "helpers to construct relative biome dependencies."
        "</p></body></html>"
    ));

    // 48-bit generator settings are not all that interesting unless we are
    // using them, so start as collapsed if they are on the "Auto" setting.
    Gen48Settings gen48 = formGen48->getSettings(false);
    ui->collapseGen48->init(tr("种子生成器(低48二进制位)"), formGen48, gen48.mode == GEN48_AUTO);
    connect(formGen48, &FormGen48::changed, this, &MainWindow::onGen48Changed);
    ui->collapseGen48->setInfo(
        tr("Help: Seed generator"),
        tr(
        "<html><head/><body><p>"
        "For some searches, the 48-bit structure seed candidates can be "
        "generated without searching, which can vastly reduce the search space "
        "that has to be checked."
        "</p><p>"
        "The generator mode <b>Auto</b> is recommended for general use, which "
        "automatically selects suitable options based on the conditions list."
        "</p><p>"
        "The <b>Quad-feature</b> mode produces candidates for "
        "quad&#8209;structures that have a uniform distribution of "
        "region&#8209;size=32 and chunk&#8209;gap=8, such as swamp huts."
        "</p><p>"
        "A perfect <b>Quad-monument</b> structure constellation does not "
        "actually exist, but some extremely rare structure seed bases get close, "
        "with over 90&#37; of the area within 128 blocks. The generator uses a "
        "precomputed list of these seed bases."
        "</p><p>"
        "Using a <b>Seed list</b> you can provide a custom set of 48-bit "
        "candidates. Optionally, a salt value can be added and the seeds can "
        "be region transposed."
        "</p></body></html>"
    ));

    ui->collapseControl->init("符合条件的种子", formControl, false);
    connect(formControl, &FormSearchControl::selectedSeedChanged, this, &MainWindow::onSelectedSeedChanged);
    connect(formControl, &FormSearchControl::searchStatusChanged, this, &MainWindow::onSearchStatusChanged);
    ui->collapseControl->setInfo(
        tr("Help: Matching seeds"),
        tr(
        "<html><head/><body><p>"
        "The list of seeds acts as a buffer onto which suitable seeds are added "
        "when they are found. You can also copy the seed list, or paste seeds "
        "into the list. Selecting a seed will open it in the map view."
        "</p></body></html>"
    ));

    onConditionsChanged();
    update();

#if WITH_UPDATER
    QAction *updateaction = new QAction("检查更新", this);
    connect(updateaction, &QAction::triggered, [=]() { searchForUpdates(false); });
    ui->menuHelp->insertAction(ui->actionAbout, updateaction);
    ui->menuHelp->insertSeparator(ui->actionAbout);

    if (config.checkForUpdates)
        searchForUpdates(true);
#endif
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    formControl->stopSearch();
    QThreadPool::globalInstance()->clear();
    saveSettings();
    QMainWindow::closeEvent(event);
}

QAction *MainWindow::addMapAction(int sopt, const char *iconpath, QString tip)
{
    QIcon icon;
    QString inam = QString(":icons/") + iconpath;
    icon.addPixmap(QPixmap(inam + ".png"), QIcon::Normal, QIcon::On);
    icon.addPixmap(QPixmap(inam + "_d.png"), QIcon::Normal, QIcon::Off);
    QAction *action = new QAction(icon, tip, this);
    action->setCheckable(true);
    ui->toolBar->addAction(action);
    if (sopt >= 0)
    {
        action->connect(action, &QAction::toggled, [=](bool state){ this->onActionMapToggled(sopt, state); });
        saction[sopt] = action;
    }
    return action;
}


MapView* MainWindow::getMapView()
{
    return mapView;
}

bool MainWindow::getSeed(WorldInfo *wi, bool applyrand)
{
    bool ok = true;
    const std::string& mcs = ui->comboBoxMC->currentText().toStdString();
    wi->mc = str2mc(mcs.c_str());
    if (wi->mc < 0)
    {
        wi->mc = MC_NEWEST;
        qDebug() << "未知MC版本: " << wi->mc;
        ok = false;
    }

    int v = str2seed(ui->seedEdit->text(), &wi->seed);
    if (applyrand && v == S_RANDOM)
    {
        ui->seedEdit->setText(QString::asprintf("%" PRId64, (int64_t)wi->seed));
    }

    wi->large = ui->checkLarge->isChecked();
    wi->y = ui->comboY->currentText().section(' ', 0, 0).toInt();

    return ok;
}

bool MainWindow::setSeed(WorldInfo wi, int dim, int layeropt)
{
    const char *mcstr = mc2str(wi.mc);
    if (!mcstr)
    {
        qDebug() << "未知MC版本: " << wi.mc;
        return false;
    }

    if (dim == DIM_OVERWORLD)
        dimactions[0]->setChecked(true);
    if (dim == DIM_NETHER)
        dimactions[1]->setChecked(true);
    else if (dim == DIM_END)
        dimactions[2]->setChecked(true);
    else
        dim = getDim();

    ui->checkLarge->setChecked(wi.large);
    int i, n = ui->comboY->count();
    for (i = 0; i < n; i++)
        if (ui->comboY->itemText(i).section(' ', 0, 0).toInt() == wi.y)
            break;
    if (i >= n)
        ui->comboY->addItem(QString::number(wi.y));
    ui->comboY->setCurrentIndex(i);

    ui->comboBoxMC->setCurrentText(mcstr);
    ui->seedEdit->setText(QString::asprintf("%" PRId64, (int64_t)wi.seed));
    getMapView()->setSeed(wi, dim, layeropt);

    ui->checkLarge->setEnabled(wi.mc >= MC_1_3);
    ui->comboY->setEnabled(wi.mc >= MC_1_16);

    ISaveTab *tab = dynamic_cast<ISaveTab*>(ui->tabContainer->currentWidget());
    if (tab)
        tab->refresh();
    return true;
}

int MainWindow::getDim()
{
    if (!dimgroup)
        return 0;
    QAction *active = dimgroup->checkedAction();
    if (active == dimactions[1])
        return -1; // nether
    if (active == dimactions[2])
        return +1; // end
    return 0;
}

void MainWindow::saveSettings()
{
    QSettings settings("cubiomes-viewer", "cubiomes-viewer");

    settings.setValue("mainwindow/size", size());
    settings.setValue("mainwindow/pos", pos());
    settings.setValue("mainwindow/prevdir", prevdir);

    settings.setValue("config/dockable", config.dockable);
    settings.setValue("config/smoothMotion", config.smoothMotion);
    settings.setValue("config/showBBoxes", config.showBBoxes);
    settings.setValue("config/restoreSession", config.restoreSession);
    settings.setValue("config/checkForUpdates", config.checkForUpdates);
    settings.setValue("config/autosaveCycle", config.autosaveCycle);
    settings.setValue("config/uistyle", config.uistyle);
    settings.setValue("config/maxMatching", config.maxMatching);
    settings.setValue("config/gridSpacing", config.gridSpacing);
    settings.setValue("config/mapCacheSize", config.mapCacheSize);
    settings.setValue("config/biomeColorPath", config.biomeColorPath);
    settings.setValue("config/separator", config.separator);
    settings.setValue("config/quote", config.quote);

    settings.setValue("world/estimateTerrain", g_extgen.estimateTerrain);
    settings.setValue("world/saltOverride", g_extgen.saltOverride);
    for (int st = 0; st < FEATURE_NUM; st++)
    {
        uint64_t salt = g_extgen.salts[st];
        if (salt <= MASK48)
            settings.setValue(QString("world/salt_") + struct2str(st), (qulonglong)salt);
    }

    on_tabContainer_currentChanged(-1);

    WorldInfo wi;
    getSeed(&wi, false);
    settings.setValue("map/mc", wi.mc);
    settings.setValue("map/large", wi.large);
    settings.setValue("map/seed", (qlonglong)wi.seed);
    settings.setValue("map/dim", getDim());
    settings.setValue("map/x", getMapView()->getX());
    settings.setValue("map/y", wi.y);
    settings.setValue("map/z", getMapView()->getZ());
    settings.setValue("map/scale", getMapView()->getScale());
    for (int stype = 0; stype < STRUCT_NUM; stype++)
    {
        QString s = QString("map/show_") + mapopt2str(stype);
        settings.setValue(s, getMapView()->getShow(stype));
    }

    if (config.restoreSession)
    {
        QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir dir(path);
        if (!dir.exists())
            dir.mkpath(".");
        saveProgress(path + "/session.save", true);
    }
}


void MainWindow::loadSettings()
{
    QSettings settings("cubiomes-viewer", "cubiomes-viewer");

    resize(settings.value("mainwindow/size", size()).toSize());
    move(settings.value("mainwindow/pos", pos()).toPoint());
    prevdir = settings.value("mainwindow/prevdir", pos()).toString();

    config.dockable = settings.value("config/dockable", config.dockable).toBool();
    config.smoothMotion = settings.value("config/smoothMotion", config.smoothMotion).toBool();
    config.showBBoxes = settings.value("config/showBBoxes", config.showBBoxes).toBool();
    config.restoreSession = settings.value("config/restoreSession", config.restoreSession).toBool();
    config.checkForUpdates = settings.value("config/checkForUpdates", config.checkForUpdates).toBool();
    config.autosaveCycle = settings.value("config/autosaveCycle", config.autosaveCycle).toInt();
    config.uistyle = settings.value("config/uistyle", config.uistyle).toInt();
    config.maxMatching = settings.value("config/maxMatching", config.maxMatching).toInt();
    config.gridSpacing = settings.value("config/gridSpacing", config.gridSpacing).toInt();
    config.mapCacheSize = settings.value("config/mapCacheSize", config.mapCacheSize).toInt();
    config.biomeColorPath = settings.value("config/biomeColorPath", config.biomeColorPath).toString();
    config.separator = settings.value("config/separator", config.separator).toString();
    config.quote = settings.value("config/quote", config.quote).toString();

    if (!config.biomeColorPath.isEmpty())
        onBiomeColorChange();

    getMapView()->setConfig(config);
    onStyleChanged(config.uistyle);
    setDockable(config.dockable);

    g_extgen.estimateTerrain = settings.value("world/estimateTerrain", g_extgen.estimateTerrain).toBool();
    g_extgen.saltOverride = settings.value("world/saltOverride", g_extgen.saltOverride).toBool();
    for (int st = 0; st < FEATURE_NUM; st++)
    {
        QVariant v = QVariant::fromValue(~(qulonglong)0);
        g_extgen.salts[st] = settings.value(QString("world/salt_") + struct2str(st), v).toULongLong();
    }

    WorldInfo wi;
    getSeed(&wi, true);
    wi.mc = settings.value("map/mc", wi.mc).toInt();
    wi.large = settings.value("map/large", wi.large).toBool();
    wi.seed = (uint64_t) settings.value("map/seed", QVariant::fromValue((qlonglong)wi.seed)).toLongLong();
    wi.y = settings.value("map/y", 256).toInt();
    int dim = settings.value("map/dim", getDim()).toInt();
    setSeed(wi, dim);

    qreal x = getMapView()->getX();
    qreal z = getMapView()->getZ();
    qreal scale = getMapView()->getScale();

    x = settings.value("map/x", x).toDouble();
    z = settings.value("map/z", z).toDouble();
    scale = settings.value("map/scale", scale).toDouble();

    for (int sopt = 0; sopt < STRUCT_NUM; sopt++)
    {
        bool show = getMapView()->getShow(sopt);
        QString soptstr = QString("map/show_") + mapopt2str(sopt);
        show = settings.value(soptstr, show).toBool();
        if (saction[sopt])
            saction[sopt]->setChecked(show);
        getMapView()->setShow(sopt, show);
    }
    mapGoto(x, z, scale);

    if (config.restoreSession)
    {
        QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        path += "/session.save";
        if (QFile::exists(path))
        {
            loadProgress(path, false, false);
        }
    }

    if (config.autosaveCycle > 0)
    {
        autosaveTimer.setInterval(config.autosaveCycle * 60000);
        autosaveTimer.start();
    }
    else
    {
        autosaveTimer.stop();
    }
}


bool MainWindow::saveProgress(QString fnam, bool quiet)
{
    QFile file(fnam);

    if (!file.open(QIODevice::WriteOnly))
    {
        if (!quiet)
            warning(tr("无法打开以下文件:\n\"%1\"").arg(fnam));
        return false;
    }

    SearchConfig searchconf = formControl->getSearchConfig();
    Gen48Settings gen48 = formGen48->getSettings(false);
    QVector<Condition> condvec = formCond->getConditions();
    QVector<uint64_t> results = formControl->getResults();

    WorldInfo wi;
    getSeed(&wi);

    QTextStream stream(&file);
    stream << "#Version:  " << VERS_MAJOR << "." << VERS_MINOR << "." << VERS_PATCH << "\n";
    stream << "#Time:     " << QDateTime::currentDateTime().toString() << "\n";
    // MC version of the session should take priority over the one in the settings
    stream << "#MC:       " << mc2str(wi.mc) << "\n";
    stream << "#Large:    " << wi.large << "\n";

    stream << "#Search:   " << searchconf.searchtype << "\n";
    if (!searchconf.slist64path.isEmpty())
        stream << "#List64:   " << searchconf.slist64path.replace("\n", "") << "\n";
    stream << "#Progress: " << searchconf.startseed << "\n";
    stream << "#Threads:  " << searchconf.threads << "\n";
    stream << "#ResStop:  " << (int)searchconf.stoponres << "\n";

    stream << "#Mode48:   " << gen48.mode << "\n";
    if (!gen48.slist48path.isEmpty())
        stream << "#List48:   " << gen48.slist48path.replace("\n", "") << "\n";
    stream << "#HutQual:  " << gen48.qual << "\n";
    stream << "#MonArea:  " << gen48.qmarea << "\n";
    if (gen48.salt != 0)
        stream << "#Salt:     " << gen48.salt << "\n";
    if (gen48.listsalt != 0)
        stream << "#LSalt:    " << gen48.listsalt << "\n";
    if (gen48.manualarea)
    {
        stream << "#Gen48X1:  " << gen48.x1 << "\n";
        stream << "#Gen48Z1:  " << gen48.z1 << "\n";
        stream << "#Gen48X2:  " << gen48.x2 << "\n";
        stream << "#Gen48Z2:  " << gen48.z2 << "\n";
    }
    if (searchconf.smin != 0)
        stream << "#SMin:     " << searchconf.smin << "\n";
    if (searchconf.smax != ~(uint64_t)0)
        stream << "#SMax:     " << searchconf.smax << "\n";

    for (Condition &c : condvec)
        stream << "#Cond: " << c.toHex() << "\n";

    for (uint64_t s : qAsConst(results))
        stream << QString::asprintf("%" PRId64 "\n", (int64_t)s);

    return true;
}

bool MainWindow::loadProgress(QString fnam, bool keepresults, bool quiet)
{
    QFile file(fnam);

    if (!file.open(QIODevice::ReadOnly))
    {
        if (!quiet)
            warning(tr("无法打开以下进度文件:\n\"%1\"").arg(fnam));
        return false;
    }

    int major = 0, minor = 0, patch = 0;
    SearchConfig searchconf = formControl->getSearchConfig();
    Gen48Settings gen48 = formGen48->getSettings(false);
    QVector<Condition> condvec;
    QVector<uint64_t> seeds;

    char buf[4096];
    int tmp;
    WorldInfo wi;
    getSeed(&wi, true);

    QTextStream stream(&file);
    QString line;
    line = stream.readLine();
    int lno = 1;

    if (sscanf(line.toLocal8Bit().data(), "#Version: %d.%d.%d", &major, &minor, &patch) != 3)
    {
        if (quiet)
            return false;
        int button = QMessageBox::warning(this, tr("警告"),
            tr("该文件可能不是进度文件，进度可能不完整或已损坏"
            "\n\n"
            "是否继续？"),
            QMessageBox::Abort|QMessageBox::Yes);
        if (button == QMessageBox::Abort)
            return false;
    }
    else if (cmpVers(major, minor, patch) > 0)
    {
        if (quiet)
            return false;
        int button = QMessageBox::warning(this, tr("警告"),
            tr("进度文件由新版的Cubiomes Viewer生成\n"
            "可能无法导入该版本软件\n\n"
            "是否继续加载？"),
            QMessageBox::Abort|QMessageBox::Yes);
        if (button == QMessageBox::Abort)
            return false;
    }

    while (stream.status() == QTextStream::Ok && !stream.atEnd())
    {
        lno++;
        line = stream.readLine();
        QByteArray ba = line.toLocal8Bit();
        const char *p = ba.data();

        if (line.isEmpty()) continue;
        if (line.startsWith("#Time:")) continue;
        if (line.startsWith("#Title:")) continue;
        if (line.startsWith("#Desc:")) continue;
        else if (sscanf(p, "#MC:       %8[^\n]", buf) == 1)                     { wi.mc = str2mc(buf); if (wi.mc < 0) wi.mc = MC_NEWEST; }
        else if (sscanf(p, "#Large:    %d", &tmp) == 1)                         { wi.large = tmp; }
        // SearchConfig
        else if (sscanf(p, "#Search:   %d", &searchconf.searchtype) == 1)       {}
        else if (sscanf(p, "#Progress: %" PRId64, &searchconf.startseed) == 1)  {}
        else if (sscanf(p, "#Threads:  %d", &searchconf.threads) == 1)          {}
        else if (sscanf(p, "#ResStop:  %d", &tmp) == 1)                         { searchconf.stoponres = tmp; }
        else if (line.startsWith("#List64:   "))                                { searchconf.slist64path = line.mid(11).trimmed(); }
        // Gen48Settings
        else if (sscanf(p, "#Mode48:   %d", &gen48.mode) == 1)                  {}
        else if (sscanf(p, "#HutQual:  %d", &gen48.qual) == 1)                  {}
        else if (sscanf(p, "#MonArea:  %d", &gen48.qmarea) == 1)                {}
        else if (sscanf(p, "#Salt:     %" PRIu64, &gen48.salt) == 1)            {}
        else if (sscanf(p, "#LSalt:    %" PRIu64, &gen48.listsalt) == 1)        {}
        else if (sscanf(p, "#Gen48X1:  %d", &gen48.x1) == 1)                    { gen48.manualarea = true; }
        else if (sscanf(p, "#Gen48Z1:  %d", &gen48.z1) == 1)                    { gen48.manualarea = true; }
        else if (sscanf(p, "#Gen48X2:  %d", &gen48.x2) == 1)                    { gen48.manualarea = true; }
        else if (sscanf(p, "#Gen48Z2:  %d", &gen48.z2) == 1)                    { gen48.manualarea = true; }
        else if (line.startsWith("#List48:   "))                                { gen48.slist48path = line.mid(11).trimmed(); }
        else if (sscanf(p, "#SMin:     %" PRIu64, &searchconf.smin) == 1)       {}
        else if (sscanf(p, "#SMax:     %" PRIu64, &searchconf.smax) == 1)       {}
        else if (line.startsWith("#Cond:"))
        {   // Conditions
            Condition c;
            if (c.readHex(line.mid(6).trimmed()))
            {
                condvec.push_back(c);
            }
            else
            {
                if (quiet)
                    return false;
                int button = QMessageBox::warning(this, tr("警告"),
                    tr("第 %2 行的条件 [%1] 不受支持\n\n"
                    "是否继续？").arg(c.save).arg(lno),
                    QMessageBox::Abort|QMessageBox::Yes);
                if (button == QMessageBox::Abort)
                    return false;
            }
        }
        else
        {   // Seeds
            uint64_t s;
            if (sscanf(line.toLocal8Bit().data(), "%" PRId64, (int64_t*)&s) == 1)
            {
                seeds.push_back(s);
            }
            else
            {
                if (quiet)
                    return false;
                int button = QMessageBox::warning(this, tr("Warning"),
                    tr("无法解析进度文件第 %1 行的条件:\n%2\n\n"
                    "是否继续？").arg(lno).arg(line),
                    QMessageBox::Abort|QMessageBox::Yes);
                if (button == QMessageBox::Abort)
                    return false;
            }
        }
    }

    setSeed(wi);

    formCond->on_buttonRemoveAll_clicked();
    for (Condition &c : condvec)
    {
        QListWidgetItem *item = new QListWidgetItem();
        formCond->addItemCondition(item, c);
    }

    formGen48->setSettings(gen48, quiet);
    formGen48->updateCount();

    if (!keepresults)
        formControl->on_buttonClear_clicked();
    formControl->setSearchConfig(searchconf, quiet);
    formControl->searchResultsAdd(seeds, false);
    formControl->searchProgressReset();

    return true;
}

void MainWindow::updateMapSeed()
{
    WorldInfo wi;
    if (getSeed(&wi))
        setSeed(wi);

    bool state;
    state = (wi.mc >= MC_1_13 && wi.mc <= MC_1_17);
    ui->actionRiver->setEnabled(state);
    ui->actionOceanTemp->setEnabled(state);
    state = (wi.mc >= MC_1_18);
    ui->actionParaTemperature->setEnabled(state);
    ui->actionParaHumidity->setEnabled(state);
    ui->actionParaContinentalness->setEnabled(state);
    ui->actionParaErosion->setEnabled(state);
    ui->actionParaDepth->setEnabled(state);
    ui->actionParaWeirdness->setEnabled(state);

    emit mapUpdated();
}

void MainWindow::setDockable(bool dockable)
{
    if (dockable)
    {   // reset to default
        QWidget *title = dock->titleBarWidget();
        dock->setTitleBarWidget(nullptr);
        delete title;
    }
    else
    {   // add a dummy widget with a layout to hide the title bar
        // and avoid a warning about negative size widget
        QWidget *title = new QWidget(this);
        QHBoxLayout *l = new QHBoxLayout(title);
        l->setMargin(0);
        title->setLayout(l);
        dock->setTitleBarWidget(title);
        dock->setFloating(false);
    }
}

void MainWindow::applyConfigChanges(const Config old, const Config conf)
{
    this->config = conf;
    getMapView()->setConfig(conf);
    if (old.uistyle != conf.uistyle)
        onStyleChanged(conf.uistyle);

    if (conf.autosaveCycle)
    {
        autosaveTimer.setInterval(conf.autosaveCycle * 60000);
        autosaveTimer.start();
    }
    else
    {
        autosaveTimer.stop();
    }

    if (!conf.biomeColorPath.isEmpty() || !old.biomeColorPath.isEmpty())
        onBiomeColorChange();

    if (conf.dockable != old.dockable)
        setDockable(conf.dockable);
}

int MainWindow::warning(QString text, QMessageBox::StandardButtons buttons)
{
    return QMessageBox::warning(this, tr("警告"), text, buttons);
}

void MainWindow::mapGoto(qreal x, qreal z, qreal scale)
{
    getMapView()->setView(x, z, scale);
}

void MainWindow::setBiomeColorRc(QString rc)
{
    config.biomeColorPath = rc;
    onBiomeColorChange();
}

void MainWindow::on_comboBoxMC_currentIndexChanged(int)
{
    updateMapSeed();
    update();
}
void MainWindow::on_seedEdit_editingFinished()
{
    updateMapSeed();
    update();
}
void MainWindow::on_checkLarge_toggled()
{
    updateMapSeed();
    update();
}
void MainWindow::on_comboY_currentIndexChanged(int)
{
    updateMapSeed();
    update();
}

void MainWindow::on_seedEdit_textChanged(const QString &a)
{
    uint64_t s;
    int v = str2seed(a, &s);
    QString typ = "";
    switch (v)
    {
    case S_TEXT:    typ = tr("文本", "种子导入文件类型"); break;
    case S_NUMERIC: typ = ""; break;
    case S_RANDOM:  typ = tr("任意", "种子导入文件类型"); break;
    }
    ui->labelSeedType->setText(typ);
}

void MainWindow::on_actionSave_triggered()
{
    QString fnam = QFileDialog::getSaveFileName(
        this, tr("保存进度"), prevdir, tr("文本文件(*.txt);;任意文件(*)"));
    if (!fnam.isEmpty())
    {
        QFileInfo finfo(fnam);
        prevdir = finfo.absolutePath();
        saveProgress(fnam);
    }
}

void MainWindow::on_actionLoad_triggered()
{
    QString fnam = QFileDialog::getOpenFileName(
        this, tr("导入进度"), prevdir, tr("文本文件(*.txt);;任意文件(*)"));
    if (!fnam.isEmpty())
    {
        QFileInfo finfo(fnam);
        prevdir = finfo.absolutePath();
        loadProgress(fnam, false, false);
    }
}

void MainWindow::on_actionQuit_triggered()
{
    close();
}

void MainWindow::on_actionPreferences_triggered()
{
    ConfigDialog *dialog = new ConfigDialog(this, &config);
    int status = dialog->exec();
    if (status == QDialog::Accepted)
    {
        applyConfigChanges(config, dialog->getSettings());
    }
    if (dialog->structVisModified)
    {
        getMapView()->deleteWorld();
        updateMapSeed();
        update();
    }
}

void MainWindow::onBiomeColorChange()
{
    QFile file(config.biomeColorPath);
    if (!config.biomeColorPath.isEmpty() && file.open(QIODevice::ReadOnly))
    {
        char buf[32*1024];
        qint64 siz = file.read(buf, sizeof(buf)-1);
        file.close();
        if (siz >= 0)
        {
            buf[siz] = 0;
            initBiomeColors(g_biomeColors);
            parseBiomeColors(g_biomeColors, buf);
        }
    }
    else
    {
        initBiomeColors(g_biomeColors);
    }
    getMapView()->refreshBiomeColors();
    ISaveTab *tab = dynamic_cast<ISaveTab*>(ui->tabContainer->currentWidget());
    if (tab)
        tab->refresh();
}

void MainWindow::on_actionGo_to_triggered()
{
    getMapView()->onGoto();
}

void MainWindow::on_actionOpen_shadow_seed_triggered()
{
    WorldInfo wi;
    if (getSeed(&wi))
    {
        wi.seed = getShadow(wi.seed);
        setSeed(wi);
    }
}

void MainWindow::on_actionStructure_visibility_triggered()
{
    StructureDialog *dialog = new StructureDialog(this);
    if (dialog->exec() != QDialog::Accepted || !dialog->modified)
        return;
    saveStructVis(dialog->structvis);
    getMapView()->deleteWorld();
    updateMapSeed();
    update();
}

void MainWindow::on_actionBiome_colors_triggered()
{
    WorldInfo wi;
    getSeed(&wi);
    BiomeColorDialog *dialog = new BiomeColorDialog(this, config.biomeColorPath, wi.mc, getDim());
    connect(dialog, SIGNAL(yieldBiomeColorRc(QString)), this, SLOT(setBiomeColorRc(QString)));
    dialog->show();
}

void MainWindow::on_actionPresetLoad_triggered()
{
    WorldInfo wi;
    getSeed(&wi);
    PresetDialog *dialog = new PresetDialog(this, wi, false);
    dialog->setActiveFilter(formCond->getConditions());
    if (dialog->exec() && !dialog->rc.isEmpty())
        loadProgress(dialog->rc, true, false);
}

void MainWindow::on_actionExamples_triggered()
{
    WorldInfo wi;
    getSeed(&wi);
    PresetDialog *dialog = new PresetDialog(this, wi, true);
    dialog->setActiveFilter(formCond->getConditions());
    if (dialog->exec() && !dialog->rc.isEmpty())
        loadProgress(dialog->rc, true, false);
}

void MainWindow::on_actionAbout_triggered()
{
    AboutDialog *dialog = new AboutDialog(this);
    dialog->show();
}

void MainWindow::on_actionCopy_triggered()
{
    formControl->copyResults();
}

void MainWindow::on_actionPaste_triggered()
{
    formControl->pasteResults();
}

void MainWindow::on_actionAddShadow_triggered()
{
    const QVector<uint64_t> results = formControl->getResults();
    QVector<uint64_t> shadows;
    shadows.reserve(results.size());
    for (uint64_t s : results)
        shadows.push_back( getShadow(s) );
    formControl->searchResultsAdd(shadows, false);
}

void MainWindow::on_actionExtGen_triggered()
{
    ExtGenDialog *dialog = new ExtGenDialog(this, &g_extgen);
    int status = dialog->exec();
    if (status == QDialog::Accepted)
    {
        g_extgen = dialog->getSettings();
        // invalidate the map world, forcing an update
        getMapView()->deleteWorld();
        updateMapSeed();
        update();
    }
}

void MainWindow::on_actionExportImg_triggered()
{
    ExportDialog *dialog = new ExportDialog(this);
    dialog->show();
}

void MainWindow::on_tabContainer_currentChanged(int index)
{
    QSettings settings("cubiomes-viewer", "cubiomes-viewer");
    ISaveTab *tabold = dynamic_cast<ISaveTab*>(ui->tabContainer->widget(prevtab));
    ISaveTab *tabnew = dynamic_cast<ISaveTab*>(ui->tabContainer->widget(index));
    if (tabold) tabold->save(settings);
    if (tabnew) tabnew->load(settings);
    prevtab = index;
}

void MainWindow::on_actionSearch_seed_list_triggered()
{
    formControl->setSearchMode(SEARCH_LIST);
}

void MainWindow::on_actionSearch_full_seed_space_triggered()
{
    formControl->setSearchMode(SEARCH_BLOCKS);
}

void MainWindow::onAutosaveTimeout()
{
    if (config.autosaveCycle)
    {
        QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        saveProgress(path + "/session.save", true);
        //int dispms = 10000;
        //if (saveProgress(path + "/session.save", true))
        //    ui->statusBar->showMessage(tr("已自动保存"), dispms);
        //else
        //    ui->statusBar->showMessage(tr("自动保存失败"), dispms);
    }
}

void MainWindow::onActionMapToggled(int sopt, bool show)
{
    if (sopt == D_PORTAL) // overworld porals should also control nether
        getMapView()->setShow(D_PORTALN, show);
    getMapView()->setShow(sopt, show);
}

void MainWindow::onActionBiomeLayerSelect(bool state, QAction *src, int lopt)
{
    if (state == false)
        return;
    const QList<QAction*> actions = ui->menuBiome_layer->actions();
    for (QAction *act : actions)
        if (act != src)
            act->setChecked(false);
    WorldInfo wi;
    if (getSeed(&wi))
        setSeed(wi, DIM_UNDEF, lopt);
}

void MainWindow::onConditionsChanged()
{
    QVector<Condition> conds = formCond->getConditions();
    formGen48->updateAutoConditions(conds);
}

void MainWindow::onGen48Changed()
{
    formGen48->updateCount();
    formControl->searchProgressReset();
}

void MainWindow::onSelectedSeedChanged(uint64_t seed)
{
    ui->seedEdit->setText(QString::asprintf("%" PRId64, (int64_t)seed));
    on_seedEdit_editingFinished();
}

void MainWindow::onSearchStatusChanged(bool running)
{
    formGen48->setEnabled(!running);
}

void MainWindow::onStyleChanged(int style)
{
    //QCoreApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles, false);
    if (style == STYLE_DARK)
    {
        QFile file(":dark.qss");
        file.open(QFile::ReadOnly);
        QString st = file.readAll();
        qApp->setStyleSheet(st);
    }
    else
    {
        qApp->setStyleSheet("");
        qApp->setStyle("");
    }
}

