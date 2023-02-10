#include "searchthread.h"
#include "formsearchcontrol.h"
#include "cutil.h"
#include "seedtables.h"

#include <QMessageBox>
#include <QEventLoop>
#include <QApplication>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QMutex>
#include <QVector>


SearchMaster::SearchMaster(FormSearchControl *parent)
    : QThread(parent)
    , parent(parent)
    , mutex()
    , abort()
    , timer()
    , count()
    , searchtype()
    , mc()
    , large()
    , condtree()
    , itemsize()
    , threadcnt()
    , gen48()
    , slist()
    , idx()
    , scnt()
    , prog()
    , seed()
    , smin()
    , smax()
    , isdone()
{
}

SearchMaster::~SearchMaster()
{
    stop();
}


bool SearchMaster::set(
    WorldInfo wi,
    const SearchConfig& sc,
    const Gen48Settings& gen48,
    const Config& config,
    std::vector<uint64_t>& slist,
    const QVector<Condition>& cv
    )
{
    (void) config;
    char refbuf[100] = {};
    char disabled[100] = {};

    for (const Condition& c : cv)
        if (c.meta & Condition::DISABLED)
            disabled[c.save] = 1;

    for (const Condition& c : cv)
    {
        char cid[8];
        snprintf(cid, sizeof(cid), "[%02d]", c.save);
        if (c.save < 1 || c.save > 99)
        {
            QMessageBox::warning(parent, tr("警告"),
                tr("条件包含无效ID %1 ！").arg(cid));
            return false;
        }
        if (c.type < 0 || c.type >= FILTER_MAX)
        {
            QMessageBox::warning(parent, tr("错误"),
                    tr("条件 %2 包含无效筛选条件类型 %1 ！")
                    .arg(c.type).arg(cid));
            return false;
        }
        if (disabled[c.save])
            continue;

        const FilterInfo& finfo = g_filterinfo.list[c.type];

        if (c.relative && refbuf[c.relative] == 0)
        {
            QMessageBox::warning(parent, "警告",
                    tr("条件 %1 包含无效条件引用:\n"
                    "条件缺失或超出范围！").arg(cid));
            return false;
        }
        if (++refbuf[c.save] > 1)
        {
            QMessageBox::warning(parent, tr("警告"),
                    tr("条件 %1 包含超过1条条件！").arg(cid));
            return false;
        }
        if (c.relative && disabled[c.relative])
        {
            int button = QMessageBox::information(NULL, tr("警告"),
                    tr("条件 %1 因其参照条件禁用而被连带禁用！")
                    .arg(cid), QMessageBox::Abort|QMessageBox::Ignore);
            if (button == QMessageBox::Abort)
                return false;
        }
        if (wi.mc < finfo.mcmin)
        {
            const char *mcs = mc2str(finfo.mcmin);
            QMessageBox::warning(parent, tr("警告"),
                    tr("条件 %1 需要MC版本 >= %2 ！")
                    .arg(cid, mcs));
            return false;
        }
        if (wi.mc > finfo.mcmax)
        {
            const char *mcs = mc2str(finfo.mcmax);
            QMessageBox::warning(parent, tr("警告"),
                    tr("条件 %1 需要MC版本 <= %2 ！")
                    .arg(cid, mcs));
            return false;
        }
        if (finfo.cat == CAT_BIOMES &&
            c.type != F_BIOME_CENTER &&
            c.type != F_BIOME_CENTER_256 &&
            c.type != F_TEMPS &&
            c.type != F_CLIMATE_NOISE)
        {
            uint64_t b = c.biomeToFind;
            uint64_t m = c.biomeToFindM;
            if ((c.biomeToExcl & b) || (c.biomeToExclM & m))
            {
                QMessageBox::warning(parent, tr("警告"),
                        tr("群系条件 %1 同时被禁用和启用！")
                        .arg(cid));
                return false;
            }
            if ((b | m | c.biomeToExcl | c.biomeToExclM) == 0)
            {
                int button = QMessageBox::information(parent, tr("提示"),
                        tr("群系条件 %1 未添加任何具体群系！")
                        .arg(cid), QMessageBox::Abort|QMessageBox::Ignore);
                if (button == QMessageBox::Abort)
                    return false;
            }

            int layerId = finfo.layer;
            if (layerId == 0 && wi.mc <= MC_1_17)
            {
                Generator tmp;
                setupGenerator(&tmp, wi.mc, 0);
                const Layer *l = getLayerForScale(&tmp, finfo.step);
                if (l)
                    layerId = l - tmp.ls.layers;
            }
            uint64_t ab, am;
            uint32_t flags = 0;
            getAvailableBiomes(&ab, &am, layerId, wi.mc, flags);
            b ^= (ab & b);
            m ^= (am & m);
            if (b || m)
            {
                int cnt = __builtin_popcountll(b) + __builtin_popcountll(m);
                QString msg = tr("群系条件 %1 包含了MC %2 不会生成的群系", "", cnt)
                        .arg(cid, mc2str(wi.mc));
                QMessageBox::warning(parent, tr("警告"), msg);
                return false;
            }
        }
        if (c.type == F_TEMPS)
        {
            int w = c.x2 - c.x1 + 1;
            int h = c.z2 - c.z1 + 1;
            if (c.count > w * h)
            {
                QMessageBox::warning(parent, tr("警告"),
                        tr("温度类别条件 %1 在(%3 x %4)的区域中条件太多(%2)！")
                        .arg(cid).arg(c.count).arg(w).arg(h));
                return false;
            }
        }
        if (finfo.cat == CAT_STRUCT)
        {
            if (c.count >= 128)
            {
                QMessageBox::warning(parent, tr("警告"),
                        tr("结构条件 %1 查询结构过多 (>= 128)！")
                        .arg(cid));
                return false;
            }
        }
        if (c.skipref && c.rmax == 0 && c.x1 == 0 && c.x2 == 0 && c.z1 == 0 && c.z2 == 0)
        {
            QMessageBox::warning(parent, tr("警告"),
                    tr("条件 %1 忽略了它唯一的检查位置！")
                    .arg(cid));
            return false;
        }
    }

    this->searchtype = sc.searchtype;
    this->mc = wi.mc;
    this->large = wi.large;
    this->condtree.set(cv, wi);
    this->itemsize = 1; //config.seedsPerItem;
    this->threadcnt = sc.threads;
    this->slist = slist;
    this->gen48 = gen48;
    this->idx = 0;
    this->scnt = ~(uint64_t)0;
    this->prog = 0;
    this->seed = sc.startseed;
    this->smin = sc.smin;
    this->smax = sc.smax;
    this->isdone = false;
    this->abort = false;
    return true;
}


static int check(uint64_t s48, void *data)
{
    (void) data;
    const StructureConfig sconf = {0,0,0,0,0};
    return isQuadBaseFeature24(sconf, s48, 7+1, 7+1, 9+1) != 0;
}

static void genQHBases(QObject *qtobj, int qual, uint64_t salt, std::vector<uint64_t>& list48)
{
    const char *lbstr = NULL;
    const uint64_t *lbset = NULL;
    uint64_t lbcnt = 0;
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (path.isEmpty())
        path = "protobases";

    switch (qual)
    {
    case IDEAL_SALTED:
    case IDEAL:
        lbstr = "ideal";
        lbset = low20QuadIdeal;
        lbcnt = sizeof(low20QuadIdeal) / sizeof(uint64_t);
        break;
    case CLASSIC:
        lbstr = "cassic";
        lbset = low20QuadClassic;
        lbcnt = sizeof(low20QuadClassic) / sizeof(uint64_t);
        break;
    case NORMAL:
        lbstr = "normal";
        lbset = low20QuadHutNormal;
        lbcnt = sizeof(low20QuadHutNormal) / sizeof(uint64_t);
        break;
    case BARELY:
        lbstr = "barely";
        lbset = low20QuadHutBarely;
        lbcnt = sizeof(low20QuadHutBarely) / sizeof(uint64_t);
        break;
    default:
        return;
    }

    path += QString("/quad_") + lbstr + ".txt";
    QByteArray fnam = path.toLocal8Bit();
    uint64_t *qb = NULL;
    uint64_t qn = 0;

    if ((qb = loadSavedSeeds(fnam.data(), &qn)) == NULL)
    {
        printf("将四联种子集保存到: %s\n", fnam.data());
        fflush(stdout);

        QMetaObject::invokeMethod(qtobj, "打开种子集消息", Qt::QueuedConnection, Q_ARG(QString, path));

        int threads = QThread::idealThreadCount();
        int err = searchAll48(&qb, &qn, fnam.data(), threads, lbset, lbcnt, 20, check, NULL);

        if (err)
        {
            QMetaObject::invokeMethod(
                    qtobj, "警告", Qt::BlockingQueuedConnection,
                    Q_ARG(QString, SearchMaster::tr("未能成功创建种子集")));
            return;
        }
        else
        {
            QMetaObject::invokeMethod(qtobj, "关闭种子集消息", Qt::BlockingQueuedConnection);
        }
    }
    else
    {
        //printf("Loaded quad-protobases from: %s\n", fnam.data());
        //fflush(stdout);
    }

    if (qb)
    {
        // convert protobases to proper bases by subtracting the salt
        list48.resize(qn);
        for (uint64_t i = 0; i < qn; i++)
            list48[i] = qb[i] - salt;
        free(qb);
    }
}

static bool applyTranspose(std::vector<uint64_t>& slist,
        const Gen48Settings& gen48, uint64_t bufmax)
{
    std::vector<uint64_t> list48;

    int x = gen48.x1;
    int z = gen48.z1;
    int w = gen48.x2 - x + 1;
    int h = gen48.z2 - z + 1;

    // does the set of candidates for this condition fit in memory?
    if ((uint64_t)slist.size() * sizeof(int64_t) * w*h >= bufmax)
    {
        slist.clear();
        return false;
    }

    try {
        list48.resize(slist.size() * w*h);
    } catch (...) {
        slist.clear();
        return false;
    }

    uint64_t *p = list48.data();
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            for (uint64_t b : slist)
                *p++ = moveStructure(b, x+i, z+j);

    std::sort(list48.begin(), list48.end());
    auto last = std::unique(list48.begin(), list48.end());
    list48.erase(last, list48.end());
    slist.swap(list48);
    return !slist.empty();
}

void SearchMaster::presearch()
{
    uint64_t sstart = seed;

    if (searchtype != SEARCH_LIST)
    {
        if (gen48.mode == GEN48_QH)
        {
            uint64_t salt = 0;
            if (gen48.qual == IDEAL_SALTED)
                salt = gen48.salt;
            else
            {
                StructureConfig sconf;
                getStructureConfig_override(Swamp_Hut, mc, &sconf);
                salt = sconf.salt;
            }
            slist.clear();
            genQHBases(parent, gen48.qual, salt, slist);
        }
        else if (gen48.mode == GEN48_QM)
        {
            StructureConfig sconf;
            getStructureConfig_override(Monument, mc, &sconf);
            const uint64_t *qb = g_qm_90;
            uint64_t qn = sizeof(g_qm_90) / sizeof(uint64_t);
            slist.clear();
            slist.reserve(qn);
            for (uint64_t i = 0; i < qn; i++)
                if (qmonumentQual(qb[i]) >= gen48.qmarea)
                    slist.push_back((qb[i] - sconf.salt) & MASK48);
        }
        else if (gen48.mode == GEN48_LIST)
        {
            if (gen48.listsalt)
            {
                for (uint64_t& rs : slist)
                    rs += gen48.listsalt;
            }
        }

        if (!slist.empty())
            applyTranspose(slist, gen48, PRECOMPUTE48_BUFSIZ);
    }

    if (searchtype == SEARCH_LIST)
    {
        if (!slist.empty())
        {   // 64-bit seed list
            scnt = slist.size();
            for (idx = 0; idx < scnt; idx++)
                if (slist[idx] == sstart)
                    break;
            if (idx == scnt)
                idx = 0;
            seed = slist[idx];
            smax = slist.back();
            prog = idx;
        }
        else
        {   // slist should not be empty for a meaningful list search
            scnt = smax = ~(uint64_t)0;
            prog = seed = sstart;
            idx = 0;
        }
    }

    if (searchtype == SEARCH_INC)
    {   // smin & smax are given by user
        if (!slist.empty())
        {   // incremental search with a 48-bit list (incl. quad-searches)
            seed = sstart;
            if (seed < smin)
                seed = smin;
            scnt = 0x10000 * slist.size();
            uint64_t high = (seed >> 48) & 0xffff;
            for (idx = 0; idx < slist.size(); idx++)
                if (slist[idx] >= (seed & MASK48))
                    break;
            if (idx == slist.size())
            {
                if (high++ >= (smax >> 48))
                    isdone = true;
                idx = 0;
            }
            seed = (high << 48) | slist[idx];
            // trim the search space to the range [smin, smax]
            uint64_t idxmin, idxmax;
            for (idxmin = 0; idxmin < slist.size(); idxmin++)
                if (slist[idxmin] >= (smin & MASK48))
                    break;
            for (idxmax = 0; idxmax < slist.size(); idxmax++)
                if (slist[idxmax] >= (smax & MASK48))
                    break;
            high = (high - (smin >> 48)) & 0xffff;
            prog = high * slist.size() + idx - idxmin;
            high = ((smax >> 48) - (smin >> 48)) & 0xffff;
            scnt = high * slist.size() + idxmax - idxmin;
        }
        else
        {   // simple incremental search
            seed = sstart;
            if (seed < smin)
                seed = smin;
            prog = seed - smin;
            scnt = smax - smin;
        }
        if (seed > smax)
            isdone = true;
    }

    if (searchtype == SEARCH_BLOCKS)
    {
        if (!slist.empty())
        {
            scnt = 0x10000 * slist.size();
            for (idx = 0; idx < slist.size(); idx++)
                if (slist[idx] >= (sstart & MASK48))
                    break;
            if (idx == slist.size())
                isdone = true;
            else
            {
                seed = (sstart & ~MASK48) | slist[idx];
                prog = 0x10000 * idx + (seed >> 48);
            }
            smax = slist.back() | (0xffffULL << 48);
        }
        else
        {
            scnt = smax = ~(uint64_t)0;
            seed = sstart;
            prog = (seed << 16) | (seed >> 48);
        }
    }
}

void SearchMaster::run()
{
    presearch();
    stop();

    for (int i = 0; i < threadcnt; i++)
    {
        SearchWorker *worker = new SearchWorker(this);
        QObject::connect(
            worker, &SearchWorker::result,
            parent, &FormSearchControl::onSearchResult,
            Qt::BlockingQueuedConnection);
        QObject::connect(
            worker, &SearchWorker::finished,
            this, &SearchMaster::onWorkerFinished,
            Qt::QueuedConnection);

        workers.push_back(worker);
    }

    QMutexLocker locker(&mutex);

    abort = false;
    timer.start();
    count = 0;

    for (SearchWorker *worker: workers)
    {
        worker->start();
    }
}


void SearchMaster::stop()
{
    abort = true;
    if (workers.empty())
        return;

    // clear event loop of currently running signals (such as results triggers)
    QApplication::processEvents();

    for (long stop_ms = 300; ; stop_ms *= 5)
    {
        QElapsedTimer timer;
        timer.start();

        int running = 0;
        for (SearchWorker *worker: workers)
        {
            long ms = stop_ms - timer.elapsed();
            if (mc < 1) ms = 1;
            running += !worker->wait(ms);
        }
        if (!running)
            break;
        int button = 0;
        Qt::ConnectionType connectiontype = Qt::BlockingQueuedConnection;
        if (QThread::currentThread() == QApplication::instance()->thread())
        {   // main thread would deadlock with a blocking connection
            connectiontype = Qt::DirectConnection;
        }
        QMetaObject::invokeMethod(
                parent, "警告", connectiontype,
                Q_RETURN_ARG(int, button),
                Q_ARG(QString, tr("未能停止 %n 个工作线程\n"
                "是否继续等待线程停止？", "", running)),
                Q_ARG(QMessageBox::StandardButtons, QMessageBox::No|QMessageBox::Yes));
        if (button != QMessageBox::Yes)
            break;
    }

    for (SearchWorker *worker: workers)
    {
        if (worker->isRunning())
        {
            worker->disconnect(this);
            connect(worker, &SearchWorker::finished, worker, &QObject::deleteLater);
        }
        else
        {
            delete worker;
        }
    }
    workers.clear();
    emit searchFinish(false);
}

bool SearchMaster::getProgress(uint64_t *prog, uint64_t *end, uint64_t *seed)
{
    if (!mutex.tryLock(10))
    {
        if (searchtype == SEARCH_BLOCKS && slist.empty())
        {   // a block search with no list looks for candidates in the search
            // master and can therefore make progress outside of workers
            *prog = this->prog;
            *end  = this->scnt;
            *seed = this->seed;
            return true;
        }
        return false;
    }
    *prog = this->prog;
    *end  = this->scnt;
    *seed = this->seed;

    bool valid = false;
    for (SearchWorker *worker: workers)
    {
        if (worker->prog < *prog)
        {
            *prog = worker->prog;
            *seed = worker->seed;
            valid = true;
        }
    }
    mutex.unlock();
    return valid;
}

bool SearchMaster::requestItem(SearchWorker *item)
{
    if (isdone)
        return false;

    QMutexLocker locker(&mutex);

    // check if we should adjust the item size
    uint64_t nsec = timer.nsecsElapsed();
    count++;
    if (nsec > 0.1e9)
    {
        if (count < 1e2 && itemsize > 1)
            itemsize /= 2;
        if (count > 1e3 && itemsize < 0x10000)
            itemsize *= 2;
        timer.start();
        count = 0;
    }

    item->prog      = prog;
    item->idx       = idx;
    item->sstart    = seed;
    item->scnt      = itemsize;
    item->seed      = seed;

    prog += itemsize;

    if (searchtype == SEARCH_LIST)
    {
        if (idx + itemsize > scnt)
            item->scnt = scnt - idx;
        idx += itemsize;
        if (idx >= scnt)
            isdone = true;
    }

    if (searchtype == SEARCH_INC)
    {
        if (!slist.empty())
        {
            uint64_t high = (seed >> 48) & 0xffff;
            idx += itemsize;
            high += idx / slist.size();
            idx %= slist.size();
            seed = (high << 48) | slist[idx];
            if (high > (smax >> 48))
                isdone = true;
        }
        else
        {
            // seed += itemsize; with overflow detection
            uint64_t s = seed + itemsize;
            if (s < seed)
                isdone = true; // overflow
            seed = s;
        }
        if (seed > smax)
            isdone = true;
    }

    if (searchtype == SEARCH_BLOCKS)
    {
        if (!slist.empty())
        {
            uint64_t high = (seed >> 48) & 0xffff;
            high += itemsize;
            if (high >= 0x10000)
            {
                high = 0;
                idx++;
            }
            if (idx >= slist.size())
                isdone = true;
            else
                seed = (high << 48) | slist[idx];
        }
        else
        {
            WorldGen gen;
            Pos origin = {0,0};
            gen.init(mc, large);

            uint64_t high = (seed >> 48) & 0xffff;
            uint64_t low = seed & MASK48;
            high += itemsize;
            if (high >= 0x10000)
            {
                item->scnt -= 0x10000 - high;
                high = 0;
                low++;

                for (; low <= MASK48 && !abort; low++)
                {
                    gen.setSeed(low);
                    if (testTreeAt(origin, &condtree, PASS_FAST_48, &gen, &abort)
                        != COND_FAILED)
                    {
                        break;
                    }
                    // update progress for skipped block
                    seed = low;
                    prog += 0x10000;
                }
                if (low > MASK48)
                    isdone = true;
            }
            seed = (high << 48) | low;
        }
    }

    return true;
}


void SearchMaster::onWorkerFinished()
{
    QMutexLocker locker(&mutex);
    for (SearchWorker *worker : workers)
        if (!worker->isFinished())
            return;
    for (SearchWorker *worker: workers)
        delete worker;
    workers.clear();
    emit searchFinish(isdone && !abort);
}


SearchWorker::SearchWorker(SearchMaster *master)
    : QThread(nullptr)
    , master(master)
{
    this->searchtype    = master->searchtype;
    this->mc            = master->mc;
    this->large         = master->large;
    this->pctree        = &master->condtree;
    this->slist         = master->slist.empty() ? NULL : master->slist.data();
    this->len           = master->slist.size();
    this->abort         = &master->abort;

    this->prog          = master->prog;
    this->idx           = master->idx;
    this->sstart        = master->seed;
    this->scnt          = 0;
    this->seed          = master->seed;
}


void SearchWorker::run()
{
    WorldGen gen;
    Pos origin = {0,0};
    gen.init(mc, large);

    switch (searchtype)
    {
    case SEARCH_LIST:
        while (!*abort && master->requestItem(this))
        {   // seed = slist[..]
            uint64_t ie = idx+scnt < len ? idx+scnt : len;
            for (uint64_t i = idx; i < ie; i++)
            {
                seed = slist[i];
                gen.setSeed(seed);
                if (testTreeAt(origin, pctree, PASS_FULL_64, &gen, abort)
                    == COND_OK
                )
                {
                    if (!*abort)
                        emit result(seed);
                }
            }
            //if (ie == len) // done
            //   break;
        }
        break;

    case SEARCH_INC:
        while (!*abort && master->requestItem(this))
        {
            if (slist)
            {   // seed = (high << 48) | slist[..]
                uint64_t high = (sstart >> 48) & 0xffff;
                uint64_t lowidx = idx;

                for (int i = 0; i < scnt; i++)
                {
                    seed = (high << 48) | slist[lowidx];

                    gen.setSeed(seed);
                    if (testTreeAt(origin, pctree, PASS_FULL_64, &gen, abort)
                        == COND_OK
                    )
                    {
                        if (!*abort)
                            emit result(seed);
                    }

                    if (++lowidx >= len)
                    {
                        lowidx = 0;
                        if (++high >= 0x10000)
                        {   // done
                            break;
                        }
                    }
                }
            }
            else
            {   // seed++
                seed = sstart;
                for (int i = 0; i < scnt; i++)
                {
                    gen.setSeed(seed);
                    if (testTreeAt(origin, pctree, PASS_FULL_64, &gen, abort)
                        == COND_OK
                    )
                    {
                        if (!*abort)
                            emit result(seed);
                    }

                    if (seed == ~(uint64_t)0)
                    {   // done
                        break;
                    }
                    seed++;
                }
            }
        }
        break;

    case SEARCH_BLOCKS:
        while (!*abort && master->requestItem(this))
        {   // seed = ([..] << 48) | low
            if (slist && idx >= len)
            {
                continue;
            }

            uint64_t high = (sstart >> 48) & 0xffff;
            uint64_t low;
            if (slist)
                low = slist[idx];
            else
                low = sstart & MASK48;

            gen.setSeed(low);
            if (testTreeAt(origin, pctree, PASS_FULL_48, &gen, abort)
                == COND_FAILED
            )
            {
                continue;
            }

            for (int i = 0; i < scnt; i++)
            {
                seed = (high << 48) | low;

                gen.setSeed(seed);
                if (testTreeAt(origin, pctree, PASS_FULL_64, &gen, abort)
                    == COND_OK
                )
                {
                    if (!*abort)
                        emit result(seed);
                }

                if (++high >= 0x10000)
                    break; // done
            }
        }
        break;
    }
}
