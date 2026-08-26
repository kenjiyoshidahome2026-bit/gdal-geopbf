#include "ogrgeopbf.h"
#include <cmath>
#include <cstdio>
#include <ctime>

// ── Dataset ───────────────────────────────────────────────────────────────────

OGRGeoPBFDataset::OGRGeoPBFDataset() {}
OGRGeoPBFDataset::~OGRGeoPBFDataset() { delete m_poLayer; }

// ファイル全体をメモリへ。サイズを先に問い合わせず逐次読みにするのは、/vsigzip/ のように
// 「開く前に伸長後サイズが判らない」ハンドルでも同じ経路で読めるようにするため。
static bool GeoPBFIngest(VSILFILE* fp, std::vector<uint8_t>& out) {
    constexpr size_t CHUNK = 1 << 20;
    size_t n = 0;
    for (;;) {
        out.resize(n + CHUNK);
        const size_t got = VSIFReadL(out.data() + n, 1, CHUNK, fp);
        n += got;
        if (got < CHUNK) break;
    }
    out.resize(n);
    return n > 0;
}

int OGRGeoPBFDataset::Open(const char* pszFilename) {
    // gzip された .geopbf を透過的に読む。ウェブ側の書き出し（encoder）は gzip して .geopbf を吐き、
    // JS の読み手は gzip 印を見て自動で伸長する＝同じ物がドライバでも開けないと片翼になる。
    // 実装は GDAL の /vsigzip/ に委譲（自前 zlib を持たない＝依存ゼロの掟を守る）。
    std::string osOpen(pszFilename);
    {
        VSILFILE* fpProbe = VSIFOpenL(pszFilename, "rb");
        if (!fpProbe) return FALSE;
        unsigned char magic[2] = {0, 0};
        const size_t got = VSIFReadL(magic, 1, 2, fpProbe);
        VSIFCloseL(fpProbe);
        const bool bAlreadyVsi = osOpen.compare(0, 9, "/vsigzip/") == 0;
        if (got == 2 && magic[0] == 0x1F && magic[1] == 0x8B && !bAlreadyVsi)
            osOpen = "/vsigzip/" + osOpen;
    }
    VSILFILE* fp = VSIFOpenL(osOpen.c_str(), "rb");
    if (!fp) return FALSE;
    const bool bIngested = GeoPBFIngest(fp, m_data);
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

OGRFeature* OGRGeoPBFLayer::GetNextFeature() {
    const uint8_t* data = m_poDS->m_data.data();
    const size_t   fend = m_poDS->m_farrayEnd;

    while (m_pos < fend) {
        PbfReader r(data, m_pos, fend);
        int wt; int tag = r.readTag(wt);
        if (tag != TAG_FEATURE || wt != 2) { r.skipField(wt); m_pos = r.pos; continue; }

        auto feat = r.enterMessage();
        m_pos = r.pos;

        OGRFeature* poFeature = new OGRFeature(m_poFeatureDefn);
        poFeature->SetFID(m_nFID++);

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
            int fi = (int)index[i];
            if (fi < m_poFeatureDefn->GetFieldCount() && !values[i].empty())
                poFeature->SetField(fi, values[i].c_str());
        }
        if (geom) poFeature->SetGeometryDirectly(geom);

        if ((m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)) &&
            (m_poFilterGeom == nullptr || FilterGeometry(geom)))
            return poFeature;
        delete poFeature;
    }
    return nullptr;
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

CPL_C_START
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
        "</CreationOptionList>");
    d->pfnIdentify = OGRGeoPBFDriverIdentify;   // Identify は Open より先に呼ばれる＝他形式のファイルを掴まない
    d->pfnOpen = OGRGeoPBFDriverOpen;
    d->pfnCreate = OGRGeoPBFDriverCreate;
    GetGDALDriverManager()->RegisterDriver(d);
}
CPL_C_END
