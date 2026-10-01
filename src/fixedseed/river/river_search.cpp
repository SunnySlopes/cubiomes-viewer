#include "river_search.h"
#include "Thread.h"
#include "../cave/ExactBiomeColumnCache.h"
#include "cubiomes/generator.h"
#include "cubiomes/biomes.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {
bool checkProgress(RiverProgress *p) {
    if (!p) return true;
    while (p->try_pause.load()) {
        if (p->try_stop.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !p->try_stop.load();
}
bool betterHit(const RiverHit &a, const RiverHit &b) {
    if (a.area != b.area) return a.area > b.area;
    if (a.x != b.x) return a.x < b.x;
    return a.z < b.z;
}
uint64_t key(int x, int z) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(z); }
void dedupeExact(std::vector<RiverHit> &hits) {
    std::sort(hits.begin(), hits.end(), betterHit);
    std::unordered_set<uint64_t> seen;
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const RiverHit &h) {
        return !seen.insert(key(h.x, h.z)).second;
    }), hits.end());
}
void dedupe64MaxArea(std::vector<RiverHit> &hits) {
    std::sort(hits.begin(), hits.end(), betterHit);
    std::unordered_set<uint64_t> seen;
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const RiverHit &h) {
        return !seen.insert(key(h.x >> 6, h.z >> 6)).second;
    }), hits.end());
}
struct RingRow { int dz, outer, inner; };
template<int scale> const std::vector<RingRow> &ringRows() {
    static const std::vector<RingRow> rows = [] {
        std::vector<RingRow> v;
        const int outer = 128 / scale, inner = 24 / scale;
        for (int dz = -outer; dz <= outer; ++dz)
            v.push_back({dz, int(std::floor(std::sqrt(double(outer*outer-dz*dz)))),
                std::abs(dz) <= inner ? int(std::floor(std::sqrt(double(inner*inner-dz*dz)))) : -1});
        return v;
    }();
    return rows;
}
// A horizontal prefix sum answers each single-row annulus segment with two loads.
// Outer circle is inclusive; inner circle is excluded, as in the original finder.
template<int scale> std::vector<RiverHit> scoreRiverGrid(const std::vector<int> &raw,
    int W, int H, int startX, int startZ, int minArea, double fraction, RiverProgress *p,
    std::unordered_map<uint64_t, int> *areas = nullptr) {
    std::vector<RiverHit> hits;
    const int radius = 128 / scale, stride = W + 1;
    if (W <= 2*radius || H <= 2*radius) return hits;
    std::vector<int> prefix(size_t(stride)*H, 0);
    for (int z = 0; z < H; ++z) {
        int *row = prefix.data() + size_t(z)*stride;
        for (int x = 0; x < W; ++x) row[x+1] = row[x] + raw[size_t(z)*W+x];
    }
    int maxArea = 0;
    for (int cz = radius; cz < H-radius; ++cz) {
        if (!checkProgress(p)) return {};
        for (int cx = radius; cx < W-radius; ++cx) {
            int area = 0;
            const uint64_t k = key(startX+cx*scale, startZ+cz*scale);
            const auto cached = areas ? areas->find(k) : std::unordered_map<uint64_t,int>::iterator{};
            if (areas && cached != areas->end()) area = cached->second;
            else {
                for (const auto &m : ringRows<scale>()) {
                    const int *row = prefix.data() + size_t(cz+m.dz)*stride;
                    area += row[cx+m.outer+1] - row[cx-m.outer];
                    if (m.inner >= 0) area -= row[cx+m.inner+1] - row[cx-m.inner];
                }
                area *= scale*scale;
                if (areas) areas->emplace(k,area);
            }
            if (area >= minArea && area >= int(maxArea*fraction)) {
                hits.push_back({startX+cx*scale, startZ+cz*scale, area});
                maxArea = std::max(maxArea, area);
            }
        }
    }
    hits.erase(std::remove_if(hits.begin(), hits.end(), [=](const RiverHit &h) {
        return h.area < int(maxArea*fraction);
    }), hits.end());
    std::sort(hits.begin(), hits.end(), betterHit);
    return hits;
}
template<int scale> std::vector<RiverHit> findBiggestRiver(Generator *g, ExactBiomeColumnCache &cache,
    int startX, int startZ, int sx, int sz, int y, int minArea, double f, RiverProgress *p,
    std::unordered_map<uint64_t,int> *areas = nullptr) {
    const int W = sx/scale, H = sz/scale;
    if (W <= 256/scale || H <= 256/scale) return {};
    if (areas) {
        // Bound memory per worker; cached values are only an acceleration, never a filter.
        if (areas->size() > 262144) areas->clear();
        std::vector<RiverHit> cachedHits;
        int best = 0;
        bool complete = true;
        const int radius = 128/scale;
        for (int z=radius; z<H-radius && complete; ++z) {
            if (!checkProgress(p)) return {};
            for (int x=radius; x<W-radius; ++x) {
                const int wx=startX+x*scale,wz=startZ+z*scale;
                auto it=areas->find(key(wx,wz));
                if (it==areas->end()) { complete=false; break; }
                best=std::max(best,it->second);
                if(it->second>=minArea) cachedHits.push_back({wx,wz,it->second});
            }
        }
        if (complete) {
            cachedHits.erase(std::remove_if(cachedHits.begin(),cachedHits.end(),[=](const RiverHit &h) {
                return h.area<int(best*f);
            }),cachedHits.end());
            std::sort(cachedHits.begin(),cachedHits.end(),betterHit);
            return cachedHits;
        }
    }
    std::vector<int> raw(size_t(W)*H);
    if (g->mc < MC_1_18 && scale == 1) {
        Range r = {1, startX, startZ, W, H, y, 1};
        std::unique_ptr<int, decltype(&std::free)> buf(allocCache(g,r), &std::free);
        if (!buf || genBiomes(g,buf.get(),r)) return {};
        for (size_t i=0; i<raw.size(); ++i) raw[i] = buf.get()[i] == river;
    } else for (int z=0; z<H; ++z) {
        if (!checkProgress(p)) return {};
        for (int x=0; x<W; ++x) {
            const int wx = startX+x*scale+scale/2, wz = startZ+z*scale+scale/2;
            int id;
            if constexpr (scale == 1) id = cache.sampleBlock(wx,y,wz);
            else if (g->mc >= MC_1_18)
                // Preserve the original coarse grid's truncating negative division.
                id = cache.sampleQuart(wx/4,y/4+1,wz/4);
            else id = getBiomeAt(g,1,wx,y,wz);
            raw[size_t(z)*W+x] = id == river;
        }
    }
    return scoreRiverGrid<scale>(raw,W,H,startX,startZ,minArea,f,p,areas);
}
void finish(RiverProgress *p) {
    if (p) { p->phase=2; p->total=std::max(1,p->total.load()); p->current=p->total.load(); }
}
}
void runRiverSearch(const RiverSearchConfig &cfg, RiverProgress *p, std::vector<RiverHit> &out) {
    out.clear();
    if (cfg.sx<=256 || cfg.sz<=256) { finish(p); return; }
    constexpr int tileSize=8192, overlap=256;
    std::vector<std::pair<int,int>> tiles;
    for (int x=0; x<cfg.sx; x+=tileSize-overlap)
        for (int z=0; z<cfg.sz; z+=tileSize-overlap) tiles.emplace_back(x,z);
    const int requested=cfg.threads>0 ? cfg.threads : int(std::max(1u,std::thread::hardware_concurrency()));
    const int threads=std::min(requested,int(tiles.size()));
    std::vector<std::vector<RiverHit>> tileHits(tiles.size());
    std::atomic_int next{0},done{0};
    if (p) { p->phase=1; p->current=0; p->total=int(tiles.size()); }
    {
        ThreadPool pool{size_t(threads)};
        for (int t=0; t<threads; ++t) pool.enqueue([&] {
            auto g=std::make_unique<Generator>();
            setupGenerator(g.get(),cfg.mc,FORCE_OCEAN_VARIANTS);
            applySeed(g.get(),DIM_OVERWORLD,cfg.seed);
            ExactBiomeColumnCache cache(g.get(),SAMPLE_NO_SHIFT);
            std::unordered_map<uint64_t,int> fineAreas;
            while (checkProgress(p)) {
                const int i=next.fetch_add(1); if (i>=int(tiles.size())) break;
                const int ox=tiles[i].first,oz=tiles[i].second;
                const int sx=std::min(tileSize,cfg.sx-ox),sz=std::min(tileSize,cfg.sz-oz);
                auto coarse=findBiggestRiver<16>(g.get(),cache,cfg.startX+ox,cfg.startZ+oz,sx,sz,cfg.y,cfg.minArea,0.8,p);
                for (const auto &c:coarse) {
                    if (!checkProgress(p)) break;
                    auto fine=findBiggestRiver<4>(g.get(),cache,c.x-256,c.z-256,512,512,cfg.y,cfg.minArea,1.0,p,&fineAreas);
                    // Coarse areas are not upper bounds: do not break on a poor fine result.
                    if (fine.empty()) continue;
                    const auto &h=fine.front();
                    const int rx=h.x-(cfg.startX+ox),rz=h.z-(cfg.startZ+oz);
                    if (rx>overlap/2 && rx<sx-overlap/2 && rz>overlap/2 && rz<sz-overlap/2)
                        tileHits[i].push_back(h);
                }
                dedupeExact(tileHits[i]);
                const int d=done.fetch_add(1)+1; if(p) p->current=d;
            }
        });
    }
    if(p && p->try_stop.load()) { finish(p); return; }
    std::vector<RiverHit> candidates;
    for (auto &v:tileHits) candidates.insert(candidates.end(),v.begin(),v.end());
    // Overlapping fine windows often converge to the identical center.
    dedupeExact(candidates);
    if(candidates.empty()) { finish(p); return; }
    if(p) { p->total=int(candidates.size()); p->current=0; p->phase=2; }
    next=0; done=0;
    std::vector<RiverHit> refined(candidates.size());
    std::vector<unsigned char> valid(candidates.size(),0);
    {
        const int n=std::min(requested,int(candidates.size()));
        ThreadPool pool{size_t(n)};
        for(int t=0;t<n;++t) pool.enqueue([&] {
            auto g=std::make_unique<Generator>();
            setupGenerator(g.get(),cfg.mc,FORCE_OCEAN_VARIANTS);
            applySeed(g.get(),DIM_OVERWORLD,cfg.seed);
            ExactBiomeColumnCache cache(g.get());
            std::unordered_map<uint64_t,int> exactAreas;
            while(checkProgress(p)) {
                const int i=next.fetch_add(1); if(i>=int(candidates.size())) break;
                const auto &c=candidates[i];
                auto v=findBiggestRiver<1>(g.get(),cache,c.x-160,c.z-160,320,320,cfg.y,cfg.minArea,1.0,p,&exactAreas);
                if(!v.empty()) { refined[i]=v.front(); valid[i]=1; }
                const int d=done.fetch_add(1)+1; if(p) p->current=d;
            }
        });
    }
    if(p && p->try_stop.load()) { finish(p); return; }
    for(size_t i=0;i<refined.size();++i) if(valid[i]) out.push_back(refined[i]);
    std::sort(out.begin(),out.end(),betterHit);
    // The 90% threshold is calculated in exact area units after all workers finish.
    if(!out.empty()) {
        const int threshold=std::max(cfg.minArea,int(out.front().area*0.90));
        out.erase(std::remove_if(out.begin(),out.end(),[=](const RiverHit &h) {
            return h.area<threshold;
        }),out.end());
    }
    dedupe64MaxArea(out);
    finish(p);
}
