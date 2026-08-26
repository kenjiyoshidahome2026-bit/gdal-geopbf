#include "ogrgeopbf.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <algorithm>

// ── Dataset ───────────────────────────────────────────────────────────────────

OGRGeoPBFDataset::OGRGeoPBFDataset() {}
OGRGeoPBFDataset::~OGRGeoPBFDataset() { delete m_poLayer; }

// ファイル全体をメモリへ。サイズを先に問い合わせず逐次読みにするのは、/vsigzip/ のように
// 「開く前に伸長後サイズが判らない」ハンドルでも同じ経路で読めるようにするため。
// ファイル全体をメモリへ。nHint＝展開後の大きさの見込み（0＝不明）。
// 見込みが当たれば確保も読みも一回で済む。外れても正しく読めるよう、足りなければ伸ばす。
// ※ここを刻んで伸ばすと再確保の山で実メモリが数倍に膨らむ（gzip 35MB／展開159MB のファイルで
//   ピーク 700MB を踏んだ）。大きさを先に知ることが唯一効く対策。
static bool GeoPBFIngest(VSILFILE* fp, std::vector<uint8_t>& out, vsi_l_offset nHint) {
    constexpr size_t CHUNK = 4 << 20;
    size_t n = 0;
    // 見込み +1 バイト。ぴったり確保すると「読み切ったのに EOF が確定しない」ため、
    // 確かめるためだけにもう一段伸ばす＝再確保とコピーで実メモリが 2.5 倍になる。
    if (nHint > 0 && nHint < (vsi_l_offset)(1ULL << 40)) out.resize((size_t)nHint + 1);
    for (;;) {
        if (out.size() == n) out.resize(std::max(n + CHUNK, out.size() + out.size() / 2));
        const size_t want = out.size() - n;
        const size_t got = VSIFReadL(out.data() + n, 1, want, fp);
        n += got;
        if (got < want) break;            // 要求より少ない＝末尾に達した
    }
    if (out.size() != n) out.resize(n);
    return n > 0;
}

int OGRGeoPBFDataset::Open(const char* pszFilename) {
    // gzip された .geopbf を透過的に読む。ウェブ側の書き出し（encoder）は gzip して .geopbf を吐き、
    // JS の読み手は gzip 印を見て自動で伸長する＝同じ物がドライバでも開けないと片翼になる。
    // 実装は GDAL の /vsigzip/ に委譲（自前 zlib を持たない＝依存ゼロの掟を守る）。
    std::string osOpen(pszFilename);
    vsi_l_offset nHint = 0;
    {
        VSILFILE* fpProbe = VSIFOpenL(pszFilename, "rb");
        if (!fpProbe) return FALSE;
        unsigned char magic[2] = {0, 0};
        const size_t got = VSIFReadL(magic, 1, 2, fpProbe);
        const bool bGzip = (got == 2 && magic[0] == 0x1F && magic[1] == 0x8B);
        if (bGzip) {
            // gzip の末尾4バイト(ISIZE)＝展開後の大きさ（2^32 で巡回）。展開せずに判る唯一の手がかり。
            unsigned char isize[4] = {0, 0, 0, 0};
            if (VSIFSeekL(fpProbe, 0, SEEK_END) == 0) {
                const vsi_l_offset nComp = VSIFTellL(fpProbe);
                if (nComp >= 4 && VSIFSeekL(fpProbe, nComp - 4, SEEK_SET) == 0 &&
                    VSIFReadL(isize, 1, 4, fpProbe) == 4) {
                    const uint32_t u = (uint32_t)isize[0] | ((uint32_t)isize[1] << 8) |
                                       ((uint32_t)isize[2] << 16) | ((uint32_t)isize[3] << 24);
                    if (u >= nComp) nHint = u;   // 4GB 超は巡回して当てにならない＝見込み無しで進む
                }
            }
        } else {
            VSIStatBufL st;
            if (VSIStatL(pszFilename, &st) == 0 && st.st_size > 0) nHint = (vsi_l_offset)st.st_size;
        }
        VSIFCloseL(fpProbe);
        const bool bAlreadyVsi = osOpen.compare(0, 9, "/vsigzip/") == 0;
        if (bGzip && !bAlreadyVsi) osOpen = "/vsigzip/" + osOpen;
    }
    VSILFILE* fp = VSIFOpenL(osOpen.c_str(), "rb");
    if (!fp) return FALSE;
    const bool bIngested = GeoPBFIngest(fp, m_data, nHint);
    VSIFCloseL(fp);
    if (!bIngested) return FALSE;

    PbfReader r(m_data.data(), 0, m_data.size());
    while (!r.atEnd()) {
        int wt; int tag = r.readTag(wt);
        if (tag == TAG_NAME && wt == 2) {
            size_t len = r.readVarint(); m_name = r.readString(len);
        } else if (tag == TAG_KEYS && wt == 2) {
            size_t len = r.readVarint(); m_keys.push_back(r.readString(len));
        } else if (tag == TAG_PRECISION && wt == 0) {
            m_scale = std::pow(10.0, (double)r.readVarint());
        } else if (tag == TAG_BUFS && wt == 2) {
            size_t len = r.readVarint();
            m_bufs.push_back({m_data.data() + r.pos, m_data.data() + r.pos + len});
            r.pos += len;
        } else if (tag == TAG_FARRAY && wt == 2) {
            size_t len = r.readVarint();
            m_farrayPos = r.pos;
            m_farrayEnd = r.pos + len;
            r.pos += len;
        } else {
            r.skipField(wt);
        }
    }
    CPLDebug("GeoPBF", "%.1f MB in memory, %zu keys%s", m_data.size() / 1e6, m_keys.size(),
             osOpen != pszFilename ? " (gzip)" : "");
    if (!m_farrayPos) return FALSE;
    SetDescription(pszFilename);
    m_poLayer = new OGRGeoPBFLayer(this);
    return TRUE;
}

const OGRLayer* OGRGeoPBFDataset::GetLayer(int i) const {
    return (i == 0) ? m_poLayer : nullptr;
}

// ── Layer ─────────────────────────────────────────────────────────────────────

OGRGeoPBFLayer::OGRGeoPBFLayer(OGRGeoPBFDataset* poDS)
    : m_poDS(poDS), m_pos(poDS->m_farrayPos)
{
    const char* name = poDS->m_name.empty() ? "layer" : poDS->m_name.c_str();
    m_poFeatureDefn = new OGRFeatureDefn(name);
    m_poFeatureDefn->Reference();

    OGRSpatialReference* poSRS = new OGRSpatialReference();
    poSRS->SetWellKnownGeogCS("WGS84");
    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    m_poFeatureDefn->GetGeomFieldDefn(0)->SetSpatialRef(poSRS);
    poSRS->Release();

    for (const auto& key : poDS->m_keys) {
        OGRFieldDefn f(key.c_str(), OFTString);
        m_poFeatureDefn->AddFieldDefn(&f);
    }
}

OGRGeoPBFLayer::~OGRGeoPBFLayer() { m_poFeatureDefn->Release(); }

void OGRGeoPBFLayer::ResetReading() {
    m_pos  = m_poDS->m_farrayPos;
    m_nFID = 0;
    m_iNextIdx = 0;
    m_bUseCandidates = false;   // フィルタが差し替わっていれば次回作り直す
}

std::string OGRGeoPBFLayer::DecodeValue(PbfReader& msg) {
    if (msg.atEnd()) return "";
    int wt; int dtype = msg.readTag(wt);
    char buf[64];
    switch (dtype) {
        case DT_STRING: case DT_JSON: case DT_FUNC: {
            size_t len = msg.readVarint(); return msg.readString(len);
        }
        case DT_FLOAT: {
            double d = msg.readDouble();
            snprintf(buf, sizeof(buf), "%.15g", d); return buf;
        }
        case DT_INTEGER: {
            int64_t n = msg.readSVarint();
            snprintf(buf, sizeof(buf), "%" PRId64 "", n); return buf;
        }
        case DT_BOOL:
            return msg.readVarint() ? "true" : "false";
        case DT_DATE: {
            time_t t = (time_t)msg.readSVarint();
            struct tm* tm = gmtime(&t);
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm); return buf;
        }
        case DT_COLOR: {
            size_t len = msg.readVarint();
            if (len >= 4) {
                uint8_t r=msg.data[msg.pos], g=msg.data[msg.pos+1],
                        b=msg.data[msg.pos+2], a=msg.data[msg.pos+3];
                msg.pos += len;
                snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", r,g,b,a/255.0f);
            } else if (len == 3) {
                uint8_t r=msg.data[msg.pos], g=msg.data[msg.pos+1], b=msg.data[msg.pos+2];
                msg.pos += 3;
                snprintf(buf, sizeof(buf), "rgb(%d,%d,%d)", r,g,b);
            } else {
                msg.pos += len; return "";
            }
            return buf;
        }
        default:
            msg.skipField(wt); return "";
    }
}

// ── 空間索引（メモリ内）────────────────────────────────────────────────────────
// ファイル形式は索引を持たない（小ささと引き換え）。開いた時点で中身は全部メモリに
// あるので、必要になった時だけ一度走査して「地物のバイト範囲と bbox」を作る。
// これで画面範囲の問い合わせがジオメトリを組み立てずに済む。
// 遅延構築＝素の全件変換（ogr2ogr）には一切コストを乗せない。

namespace {

// GEOMETRY メッセージから bbox だけを拾う（OGRGeometry を作らない）。
// 差分の規約は DecodeGeometry と同一＝リング/ラインごとに累算器を戻す。
bool ScanGeomBBox(PbfReader r, double scale,
                  double& minx, double& miny, double& maxx, double& maxy) {
    int type = -1;
    std::vector<int64_t>  coords;
    std::vector<uint64_t> lengths;
    bool bAny = false;

    while (!r.atEnd()) {
        int wt; int tag = r.readTag(wt);
        if      (tag == TAG_GTYPE)              type    = (int)r.readVarint();
        else if (tag == TAG_COORDS && wt == 2)  coords  = r.readPackedSVarint();
        else if (tag == TAG_LENGTH && wt == 2)  lengths = r.readPackedVarint();
        else if (tag == TAG_GARRAY && wt == 2) {
            auto gar = r.enterMessage();
            while (!gar.atEnd()) {
                int gw; int gt = gar.readTag(gw);
                if (gt == TAG_GEOMETRY && gw == 2) {
                    auto gs = gar.enterMessage();
                    bAny |= ScanGeomBBox(gs, scale, minx, miny, maxx, maxy);
                } else gar.skipField(gw);
            }
        } else r.skipField(wt);
    }
    if (coords.empty()) return bAny;

    auto take = [&](int64_t ix, int64_t iy) {
        const double x = ix / scale, y = iy / scale;
        if (x < minx) minx = x;
        if (y < miny) miny = y;
        if (x > maxx) maxx = x;
        if (y > maxy) maxy = y;
        bAny = true;
    };
    auto run = [&](size_t& ci, size_t nPts) {          // 1本＝累算器はここで完結
        int64_t cx = 0, cy = 0;
        for (size_t j = 0; j < nPts && ci + 1 < coords.size(); j++, ci += 2) {
            cx += coords[ci]; cy += coords[ci + 1];
            take(cx, cy);
        }
    };

    size_t ci = 0;
    if (type == 0) {                                   // Point＝絶対値
        take(coords[0], coords.size() > 1 ? coords[1] : 0);
    } else if (type == 1 || type == 2) {               // 連続した1本の差分列
        run(ci, coords.size() / 2);
    } else if (type == 3 || type == 4) {               // LENGTH＝本/リングごとの点数
        for (uint64_t len : lengths) run(ci, (size_t)len);
    } else if (type == 5) {                            // [ポリゴン数, リング数, 点数…]
        size_t li = 0;
        if (li < lengths.size()) {
            const size_t nPoly = (size_t)lengths[li++];
            for (size_t pi = 0; pi < nPoly && li < lengths.size(); pi++) {
                const size_t nRing = (size_t)lengths[li++];
                for (size_t ri = 0; ri < nRing && li < lengths.size(); ri++)
                    run(ci, (size_t)lengths[li++]);
            }
        }
    }
    return bAny;
}

constexpr int32_t BBOX_EMPTY_MIN = 2147483647;         // 幾何なし＝空 bbox の印
constexpr int32_t BBOX_EMPTY_MAX = -2147483647 - 1;
inline int32_t Q6(double v) {                          // 1e-6 度の整数（索引の選別用）
    const double q = v * 1e6;
    return (int32_t)(q > 2.1e9 ? 2.1e9 : q < -2.1e9 ? -2.1e9 : q);
}

}  // namespace

void OGRGeoPBFLayer::BuildIndex() {
    if (m_bIndexBuilt) return;
    m_bIndexBuilt = true;

    const uint8_t* data = m_poDS->m_data.data();
    const size_t   fend = m_poDS->m_farrayEnd;
    const double   scale = m_poDS->m_scale;
    m_sExtent = OGREnvelope();

    size_t pos = m_poDS->m_farrayPos;
    while (pos < fend) {
        PbfReader r(data, pos, fend);
        int wt; int tag = r.readTag(wt);
        if (tag != TAG_FEATURE || wt != 2) { r.skipField(wt); pos = r.pos; continue; }
        const size_t len = r.readVarint();
        const size_t start = r.pos, end = r.pos + len;
        pos = end;

        FeatRec rec{ start, end, BBOX_EMPTY_MIN, BBOX_EMPTY_MIN, BBOX_EMPTY_MAX, BBOX_EMPTY_MAX };
        double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
        PbfReader feat(data, start, end);
        while (!feat.atEnd()) {
            int fwt; int ftag = feat.readTag(fwt);
            if (ftag == TAG_GEOMETRY && fwt == 2) {
                auto gr = feat.enterMessage();
                if (ScanGeomBBox(gr, scale, minx, miny, maxx, maxy)) {
                    rec.minx = Q6(minx); rec.miny = Q6(miny);
                    rec.maxx = Q6(maxx); rec.maxy = Q6(maxy);
                    OGREnvelope e; e.MinX = minx; e.MinY = miny; e.MaxX = maxx; e.MaxY = maxy;
                    m_sExtent.Merge(e);
                }
            } else feat.skipField(fwt);
        }
        m_index.push_back(rec);
    }

    // 一様格子（上限 256×256）。点なら1セル、面なら跨いだセル全部に添字を置く。
    if (m_index.empty() || !m_sExtent.IsInit()) return;
    const size_t n = m_index.size();
    int g = (int)std::sqrt((double)n / 8.0);
    if (g < 1) g = 1;
    if (g > 256) g = 256;
    m_nGridW = m_nGridH = g;
    m_dfGridMinX = m_sExtent.MinX; m_dfGridMinY = m_sExtent.MinY;
    m_dfGridStepX = std::max(1e-12, (m_sExtent.MaxX - m_sExtent.MinX) / g);
    m_dfGridStepY = std::max(1e-12, (m_sExtent.MaxY - m_sExtent.MinY) / g);
    m_grid.assign((size_t)g * g, {});
    for (uint32_t i = 0; i < (uint32_t)n; i++) {
        const FeatRec& rc = m_index[i];
        if (rc.minx > rc.maxx) continue;               // 幾何なし
        const int x0 = std::max(0, std::min(g - 1, (int)((rc.minx / 1e6 - m_dfGridMinX) / m_dfGridStepX)));
        const int x1 = std::max(0, std::min(g - 1, (int)((rc.maxx / 1e6 - m_dfGridMinX) / m_dfGridStepX)));
        const int y0 = std::max(0, std::min(g - 1, (int)((rc.miny / 1e6 - m_dfGridMinY) / m_dfGridStepY)));
        const int y1 = std::max(0, std::min(g - 1, (int)((rc.maxy / 1e6 - m_dfGridMinY) / m_dfGridStepY)));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                m_grid[(size_t)y * g + x].push_back(i);
    }
    CPLDebug("GeoPBF", "index built: %zu features, %dx%d grid", n, g, g);
}

// 現在の空間フィルタ範囲に触れる格子セルから候補の地物添字を集める
void OGRGeoPBFLayer::PrepareCandidates() {
    m_anCandidates.clear();
    m_iNextIdx = 0;
    m_bUseCandidates = true;
    m_sCandEnv = m_sFilterEnvelope;

    if (m_grid.empty()) {                              // 格子が無い（空/幾何なし）＝全件を候補に
        m_anCandidates.reserve(m_index.size());
        for (uint32_t i = 0; i < (uint32_t)m_index.size(); i++) m_anCandidates.push_back(i);
        return;
    }
    const int g = m_nGridW;
    const int x0 = std::max(0, std::min(g - 1, (int)((m_sFilterEnvelope.MinX - m_dfGridMinX) / m_dfGridStepX)));
    const int x1 = std::max(0, std::min(g - 1, (int)((m_sFilterEnvelope.MaxX - m_dfGridMinX) / m_dfGridStepX)));
    const int y0 = std::max(0, std::min(g - 1, (int)((m_sFilterEnvelope.MinY - m_dfGridMinY) / m_dfGridStepY)));
    const int y1 = std::max(0, std::min(g - 1, (int)((m_sFilterEnvelope.MaxY - m_dfGridMinY) / m_dfGridStepY)));
    const int32_t fx0 = Q6(m_sFilterEnvelope.MinX), fy0 = Q6(m_sFilterEnvelope.MinY);
    const int32_t fx1 = Q6(m_sFilterEnvelope.MaxX), fy1 = Q6(m_sFilterEnvelope.MaxY);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            for (uint32_t i : m_grid[(size_t)y * g + x]) {
                const FeatRec& rc = m_index[i];        // セル内でも bbox で最終選別
                if (rc.maxx < fx0 || rc.minx > fx1 || rc.maxy < fy0 || rc.miny > fy1) continue;
                m_anCandidates.push_back(i);
            }
        }
    }
    std::sort(m_anCandidates.begin(), m_anCandidates.end());   // FID 順を保つ（面が複数セルに跨る分の重複も消す）
    m_anCandidates.erase(std::unique(m_anCandidates.begin(), m_anCandidates.end()), m_anCandidates.end());
}

// FEATURE メッセージ本体（start..end）から OGRFeature を組み立てる
OGRFeature* OGRGeoPBFLayer::FeatureFromRecord(const FeatRec& rec, GIntBig nFID) {
    PbfReader feat(m_poDS->m_data.data(), (size_t)rec.start, (size_t)rec.end);

    OGRFeature* poFeature = new OGRFeature(m_poFeatureDefn);
    poFeature->SetFID(nFID);

    std::vector<std::string> values;
    std::vector<uint64_t>    index;
    OGRGeometry* geom = nullptr;

    while (!feat.atEnd()) {
        int fwt; int ftag = feat.readTag(fwt);
        if (ftag == TAG_GEOMETRY && fwt == 2) {
            auto gr = feat.enterMessage();
            geom = DecodeGeometry(gr);
        } else if (ftag == TAG_VALUE && fwt == 2) {
            auto vr = feat.enterMessage();
            values.push_back(DecodeValue(vr));
        } else if (ftag == TAG_INDEX && fwt == 2) {
            index = feat.readPackedVarint();
        } else {
            feat.skipField(fwt);
        }
    }
    for (size_t i = 0; i < index.size() && i < values.size(); i++) {
        const int fi = (int)index[i];
        if (fi < m_poFeatureDefn->GetFieldCount() && !values[i].empty())
            poFeature->SetField(fi, values[i].c_str());
    }
    if (geom) {
        geom->assignSpatialReference(m_poFeatureDefn->GetGeomFieldDefn(0)->GetSpatialRef());
        poFeature->SetGeometryDirectly(geom);
    }
    return poFeature;
}

OGRFeature* OGRGeoPBFLayer::GetNextFeature() {
    // 空間フィルタあり＝索引経路（候補だけを組み立てる）
    if (m_poFilterGeom) {
        BuildIndex();
        if (!m_bUseCandidates || m_sCandEnv.MinX != m_sFilterEnvelope.MinX ||
            m_sCandEnv.MinY != m_sFilterEnvelope.MinY ||
            m_sCandEnv.MaxX != m_sFilterEnvelope.MaxX ||
            m_sCandEnv.MaxY != m_sFilterEnvelope.MaxY)
            PrepareCandidates();

        while (m_iNextIdx < m_anCandidates.size()) {
            const uint32_t i = m_anCandidates[m_iNextIdx++];
            OGRFeature* poFeature = FeatureFromRecord(m_index[i], (GIntBig)i);
            if ((m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)) &&
                FilterGeometry(poFeature->GetGeometryRef()))
                return poFeature;
            delete poFeature;
        }
        return nullptr;
    }

    // フィルタなし＝逐次読み（索引を作らない＝全件変換にコストを乗せない）
    const uint8_t* data = m_poDS->m_data.data();
    const size_t   fend = m_poDS->m_farrayEnd;

    while (m_pos < fend) {
        PbfReader r(data, m_pos, fend);
        int wt; int tag = r.readTag(wt);
        if (tag != TAG_FEATURE || wt != 2) { r.skipField(wt); m_pos = r.pos; continue; }
        const size_t len = r.readVarint();
        const FeatRec rec{ r.pos, r.pos + len, 0, 0, 0, 0 };
        m_pos = r.pos + len;

        OGRFeature* poFeature = FeatureFromRecord(rec, m_nFID++);
        if (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature))
            return poFeature;
        delete poFeature;
    }
    return nullptr;
}

OGRFeature* OGRGeoPBFLayer::GetFeature(GIntBig nFID) {
    BuildIndex();
    if (nFID < 0 || (size_t)nFID >= m_index.size()) return nullptr;
    return FeatureFromRecord(m_index[(size_t)nFID], nFID);
}

GIntBig OGRGeoPBFLayer::GetFeatureCount(int bForce) {
    if (m_poFilterGeom == nullptr && m_poAttrQuery == nullptr) {
        BuildIndex();
        return (GIntBig)m_index.size();
    }
    return OGRLayer::GetFeatureCount(bForce);
}

OGRErr OGRGeoPBFLayer::IGetExtent(int /*iGeomField*/, OGREnvelope* psExtent, bool /*bForce*/) {
    BuildIndex();
    if (!m_sExtent.IsInit()) return OGRERR_FAILURE;
    *psExtent = m_sExtent;
    return OGRERR_NONE;
}

int OGRGeoPBFLayer::TestCapability(const char* pszCap) const {
    if (EQUAL(pszCap, OLCFastFeatureCount))
        return m_poFilterGeom == nullptr && m_poAttrQuery == nullptr;
    if (EQUAL(pszCap, OLCFastGetExtent))    return TRUE;
    if (EQUAL(pszCap, OLCFastSpatialFilter)) return TRUE;   // 索引で候補を絞る（必要時に構築）
    if (EQUAL(pszCap, OLCRandomRead))       return TRUE;    // FID = 出現順＝索引で直接引ける
    if (EQUAL(pszCap, OLCStringsAsUTF8))    return TRUE;
    return FALSE;
}

// ── Geometry decoding ─────────────────────────────────────────────────────────
//
// Coordinate encoding (pbf-base.js):
//   - All coords: delta-encoded scaled integers (÷ m_scale = lon/lat)
//   - Delta accumulator resets to [0,0] per ring/line (write2/write3 call diff() per ring)
//   - Polygon rings: closing vertex is NOT stored; added on read
//   - Point: absolute (delta from [0,0] = absolute value)

OGRGeometry* OGRGeoPBFLayer::DecodeGeometry(PbfReader& r) {
    int type = -1;
    std::vector<int64_t>  coords;
    std::vector<uint64_t> lengths;
    std::vector<OGRGeometry*> subGeoms;
    const double scale = m_poDS->m_scale;

    while (!r.atEnd()) {
        int wt; int tag = r.readTag(wt);
        if      (tag == TAG_GTYPE)               type    = (int)r.readVarint();
        else if (tag == TAG_COORDS  && wt == 2)  coords  = r.readPackedSVarint();
        else if (tag == TAG_LENGTH  && wt == 2)  lengths = r.readPackedVarint();
        else if (tag == TAG_GARRAY  && wt == 2) {
            auto gar = r.enterMessage();
            while (!gar.atEnd()) {
                int gw; int gt = gar.readTag(gw);
                if (gt == TAG_GEOMETRY && gw == 2) {
                    auto gs = gar.enterMessage();
                    subGeoms.push_back(DecodeGeometry(gs));
                } else gar.skipField(gw);
            }
        } else r.skipField(wt);
    }

    // Helper: decode one ring/line of nPts vertices from coords at offset ci
    // Delta resets per ring (accumulator starts at 0,0 each call)
    auto decodeRing = [&](size_t& ci, size_t nPts, bool close) -> OGRLinearRing* {
        auto* ring = new OGRLinearRing();
        int64_t cx = 0, cy = 0;
        for (size_t j = 0; j < nPts && ci + 1 < coords.size(); j++, ci += 2) {
            cx += coords[ci]; cy += coords[ci + 1];
            ring->addPoint(cx / scale, cy / scale);
        }
        if (close && ring->getNumPoints() > 0) ring->closeRings();
        return ring;
    };
    auto decodeLine = [&](size_t& ci, size_t nPts) -> OGRLineString* {
        auto* ls = new OGRLineString();
        int64_t cx = 0, cy = 0;
        for (size_t j = 0; j < nPts && ci + 1 < coords.size(); j++, ci += 2) {
            cx += coords[ci]; cy += coords[ci + 1];
            ls->addPoint(cx / scale, cy / scale);
        }
        return ls;
    };

    switch (type) {
        case 0: { // Point – absolute coords
            if (coords.size() < 2) return new OGRPoint();
            return new OGRPoint(coords[0] / scale, coords[1] / scale);
        }
        case 1: { // MultiPoint – same delta encoding as LineString (write1)
            auto* mp = new OGRMultiPoint();
            int64_t cx = 0, cy = 0;
            for (size_t i = 0; i + 1 < coords.size(); i += 2) {
                cx += coords[i]; cy += coords[i + 1];
                mp->addGeometryDirectly(new OGRPoint(cx / scale, cy / scale));
            }
            return mp;
        }
        case 2: { // LineString – no LENGTH, one continuous delta stream
            size_t ci = 0;
            auto* ls = decodeLine(ci, coords.size() / 2);
            return ls;
        }
        case 3: { // MultiLineString – LENGTH = [len_per_line…]
            auto* mls = new OGRMultiLineString();
            size_t ci = 0;
            for (uint64_t len : lengths)
                mls->addGeometryDirectly(decodeLine(ci, (size_t)len));
            return mls;
        }
        case 4: { // Polygon – LENGTH = [ring0_pts, ring1_pts…], closing not stored
            auto* poly = new OGRPolygon();
            size_t ci = 0;
            for (uint64_t len : lengths)
                poly->addRingDirectly(decodeRing(ci, (size_t)len, true));
            return poly;
        }
        case 5: { // MultiPolygon – LENGTH = [poly_count, ring_count, ring_pts…]
            auto* mpoly = new OGRMultiPolygon();
            if (lengths.empty()) return mpoly;
            size_t ci = 0, li = 0;
            size_t polyCount = (size_t)lengths[li++];
            for (size_t pi = 0; pi < polyCount && li < lengths.size(); pi++) {
                auto* poly = new OGRPolygon();
                size_t ringCount = (size_t)lengths[li++];
                for (size_t ri = 0; ri < ringCount && li < lengths.size(); ri++)
                    poly->addRingDirectly(decodeRing(ci, (size_t)lengths[li++], true));
                mpoly->addGeometryDirectly(poly);
            }
            return mpoly;
        }
        case 6: { // GeometryCollection – sub-geometries decoded from GARRAY
            auto* gc = new OGRGeometryCollection();
            for (auto* g : subGeoms) if (g) gc->addGeometryDirectly(g);
            return gc;
        }
        default: return nullptr;
    }
}

// ── Driver registration ───────────────────────────────────────────────────────

// 先頭バイトが GeoPBF ヘッダらしいか（tag 1 = NAME・wire type 2 → 0x0A、続いて名前長と名前）。
// 拡張子だけの判定では /vsigzip/ 経由や別名のファイルを取りこぼすため、GDAL の作法どおり署名を見る。
static int GeoPBFLooksLikeHeader(const GByte* p, int n) {
    if (n < 3 || p[0] != 0x0A) return FALSE;
    const int len = p[1];                        // 名前長（1バイト varint ＝ 127 まで）
    if (len < 1 || len > 100 || 2 + len > n) return FALSE;
    for (int i = 0; i < len; i++)
        if (p[2 + i] < 0x20) return FALSE;       // 制御文字は名前に来ない（UTF-8 の日本語は 0x80 以上＝許容）
    return TRUE;
}

static int OGRGeoPBFDriverIdentify(GDALOpenInfo* poOpenInfo) {
    if (poOpenInfo->eAccess == GA_Update) return FALSE;
    const int bExt = EQUAL(CPLGetExtension(poOpenInfo->pszFilename), "geopbf");
    const GByte* h = poOpenInfo->pabyHeader;
    const int n = poOpenInfo->nHeaderBytes;
    if (n >= 2 && h[0] == 0x1F && h[1] == 0x8B) return bExt;   // gzip＝中身を覗けない＝拡張子で裁く
    if (GeoPBFLooksLikeHeader(h, n)) return TRUE;              // 素の PBF＝名前が .geopbf でなくても開く
    return bExt;                                               // 署名が読めない時は従来どおり拡張子（Open が最終判定）
}

static GDALDataset* OGRGeoPBFDriverOpen(GDALOpenInfo* poOpenInfo) {
    if (!OGRGeoPBFDriverIdentify(poOpenInfo)) return nullptr;
    auto* poDS = new OGRGeoPBFDataset();
    if (!poDS->Open(poOpenInfo->pszFilename)) { delete poDS; return nullptr; }
    return poDS;
}

// プラグインの自動読み込みは「ファイル名の接頭辞 → 探すシンボル名」が対になっている:
//   gdal_Xxx.so → GDALRegister_Xxx() ／ ogr_Xxx.so → RegisterOGRXxx()
// ベクタドライバは ogr_* が慣習なので後者を正とし、前者も別名として残す（どちらの名前で置いても読める）。
CPL_C_START
void CPL_DLL GDALRegister_GeoPBF();
void CPL_DLL RegisterOGRGeoPBF() { GDALRegister_GeoPBF(); }

void CPL_DLL GDALRegister_GeoPBF() {
    if (GDALGetDriverByName("GeoPBF") != nullptr) return;
    GDALDriver* d = new GDALDriver();
    d->SetDescription("GeoPBF");
    d->SetMetadataItem(GDAL_DCAP_VECTOR,     "YES");
    d->SetMetadataItem(GDAL_DMD_LONGNAME,    "GeoPBF Vector Format");
    d->SetMetadataItem(GDAL_DMD_EXTENSION,   "geopbf");
    d->SetMetadataItem(GDAL_DCAP_VIRTUALIO,  "YES");
    d->SetMetadataItem(GDAL_DCAP_CREATE_LAYER, "YES");
    d->SetMetadataItem(GDAL_DCAP_CREATE_FIELD, "YES");
    d->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES,
                       "Integer Integer64 Real String Date DateTime "
                       "IntegerList Integer64List RealList StringList");
    d->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATASUBTYPES, "Boolean");
    d->SetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>"
        "  <Option name='PRECISION' type='int' min='0' max='9' default='6' "
        "description='Decimal digits kept in the integer-scaled coordinates (10^n)'/>"
        "  <Option name='COMPRESS' type='string-select' default='NONE' "
        "description='Whole-file compression'>"
        "    <Value>NONE</Value><Value>GZIP</Value>"
        "  </Option>"
        "</CreationOptionList>");
    d->pfnIdentify = OGRGeoPBFDriverIdentify;   // Identify は Open より先に呼ばれる＝他形式のファイルを掴まない
    d->pfnOpen = OGRGeoPBFDriverOpen;
    d->pfnCreate = OGRGeoPBFDriverCreate;
    GetGDALDriverManager()->RegisterDriver(d);
}
CPL_C_END
