#include "searchthread.h"
#include "formsearchcontrol.h"
#include "cutil.h"

#include <QMessageBox>
#include <QEventLoop>
#include <QApplication>


SearchThread::SearchThread(FormSearchControl *parent)
    : QThread()
    , parent(parent)
    , itemgen()
    , pool()
    , activecnt()
    , abort()
    , reqstop()
    , recieved()
    , lastid()
{
    itemgen.abort = &abort;
}


bool SearchThread::set(
    QObject *mainwin, WorldInfo wi,
    const SearchConfig& sc, const Gen48Settings& gen48, const Config& config,
    std::vector<uint64_t>& slist, const QVector<Condition>& cv)
{
    char refbuf[100] = {};

    for (const Condition& c : cv)
    {
        char cid[8];
        snprintf(cid, sizeof(cid), "[%02d]", c.save);
        if (c.save < 1 || c.save > 99)
        {
            QMessageBox::warning(NULL, tr("Warning"),
                tr("Condition with invalid ID %1.").arg(cid));
            return false;
        }
        if (c.type < 0 || c.type >= FILTER_MAX)
        {
            QMessageBox::warning(NULL, tr("Error"),
                    tr("Encountered invalid filter type %1 in condition ID %2.")
                    .arg(c.type).arg(cid));
            return false;
        }

        const FilterInfo& finfo = g_filterinfo.list[c.type];

        if (c.relative && refbuf[c.relative] == 0)
        {
            QMessageBox::warning(NULL, "Warning",
                    tr("Condition with ID %1 has a broken reference position:\n"
                    "condition missing or out of order.").arg(cid));
            return false;
        }
        if (++refbuf[c.save] > 1)
        {
            QMessageBox::warning(NULL, tr("Warning"),
                    tr("More than one condition with ID %1.").arg(cid));
            return false;
        }
        if (wi.mc < finfo.mcmin)
        {
            const char *mcs = mc2str(finfo.mcmin);
            QMessageBox::warning(NULL, tr("Warning"),
                    tr("Condition %1 requires a minimum Minecraft version of %2.")
                    .arg(cid).arg(mcs));
            return false;
        }
        if (wi.mc > finfo.mcmax)
        {
            const char *mcs = mc2str(finfo.mcmax);
            QMessageBox::warning(NULL, tr("Warning"),
                    tr("Condition %1 not available for Minecraft versions above %2.")
                    .arg(cid).arg(mcs));
            return false;
        }
        if (c.type >= F_BIOME && c.type <= F_BIOME_256_OTEMP)
        {
            if ((c.bfilter.biomeToExcl & (c.bfilter.riverToFind | c.bfilter.oceanToFind)) ||
                (c.bfilter.biomeToExclM & c.bfilter.riverToFindM))
            {
                QMessageBox::warning(NULL, tr("Warning"),
                        tr("Biome filter condition with ID %1 has contradicting "
                        "flags for include and exclude.").arg(cid));
                return false;
            }
            // TODO: compare mc version and available biomes
            if (c.count == 0)
            {
                int button = QMessageBox::information(NULL, tr("Info"),
                        tr("Biome filter condition with ID %1 specifies no biomes.")
                        .arg(cid), QMessageBox::Abort, QMessageBox::Ignore);
                if (button == QMessageBox::Abort)
                    return false;
            }
        }
        if (c.type == F_TEMPS)
        {
            int w = c.x2 - c.x1 + 1;
            int h = c.z2 - c.z1 + 1;
            if (c.count > w * h)
            {
                QMessageBox::warning(NULL, tr("Warning"),
                        tr("Temperature category condition with ID %1 has too "
                        "many restrictions (%2) for the area (%3 x %4).")
                        .arg(cid).arg(c.count).arg(w).arg(h));
                return false;
            }
        }
        if (finfo.cat == CAT_STRUCT)
        {
            if (c.count >= 128)
            {
                QMessageBox::warning(NULL, tr("Warning"),
                        tr("Structure condition %1 checks for too many instances (>= 128).")
                        .arg(cid));
                return false;
            }
        }
    }

    itemgen.init(mainwin, wi, sc, gen48, config, slist, cv);

    pool.setMaxThreadCount(sc.threads);
    recieved.resize(config.queueSize);
    lastid = itemgen.itemid;
    reqstop = false;
    abort = false;
    return true;
}


void SearchThread::run()
{
    itemgen.presearch();
    pool.waitForDone();

    uint64_t prog, end;
    itemgen.getProgress(&prog, &end);
    emit progress(prog, end, itemgen.seed);

    for (int64_t idx = 0; idx < recieved.size(); idx++)
    {
        recieved[idx].valid = false;
        startNextItem();
    }

    if (activecnt == 0)
        emit searchFinish();
}


SearchItem *SearchThread::startNextItem()
{
    SearchItem *item = itemgen.requestItem();
    if (!item || item->isdone)
        return NULL;
    // call back here when done
    QObject::connect(item, &SearchItem::itemDone, this, &SearchThread::onItemDone, Qt::BlockingQueuedConnection);
    QObject::connect(item, &SearchItem::canceled, this, &SearchThread::onItemCanceled, Qt::QueuedConnection);
    // redirect results to mainwindow
    QObject::connect(item, &SearchItem::results, parent, &FormSearchControl::searchResultsAdd, Qt::BlockingQueuedConnection);
    ++activecnt;
    pool.start(item);
    return item;
}


void SearchThread::onItemDone(uint64_t itemid, uint64_t seed, bool isdone)
{
    --activecnt;

    itemgen.isdone |= isdone;
    if (!itemgen.isdone && !reqstop && !abort)
    {
        if (itemid == lastid)
        {
            uint64_t len = recieved.size();
            uint64_t idx;
            for (idx = 1; idx < len; idx++)
            {
                if (!recieved[idx].valid)
                    break;
            }

            lastid += idx;

            for (uint64_t i = idx; i < len; i++)
                recieved[i-idx] = recieved[i];
            for (uint64_t i = len-idx; i < len; i++)
                recieved[i].valid = false;

            for (uint64_t i = 0; i < idx; i++)
                startNextItem();

            uint64_t prog, end;
            itemgen.getProgress(&prog, &end);
            emit progress(prog, end, seed);
        }
        else
        {
            int64_t idx = itemid - lastid;
            if (idx < 0 || idx >= recieved.size())
            {
                QMessageBox::critical(parent, tr("Fatal Error"),
                        tr("Encountered invalid state of seed generator. "
                        "This is likely a issue with the search thread logic."),
                        QMessageBox::Abort);
                QApplication::exit();
                ::exit(1);
            }
            recieved[idx].valid = true;
            recieved[idx].seed = seed;
        }
    }

    if (activecnt == 0)
        emit searchFinish();
}

void SearchThread::onItemCanceled(uint64_t itemid)
{
    (void) itemid;
    --activecnt;
    if (activecnt == 0)
        emit searchFinish();
}

