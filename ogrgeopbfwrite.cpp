// GeoPBF 書き出し（ogr2ogr -f GeoPBF）。読み手（ogrgeopbf.cpp）と厳密に対称であること。
//
// 座標の規約（pbf-base.js と ogrgeopbf.cpp の DecodeGeometry に一致）:
//   ・整数化: round(度 × 10^PRECISION)。既定 PRECISION=6（≒0.1m）
//   ・差分: リング／ラインごとに累算器を [0,0] へ戻す（＝各リングの先頭は絶対値）
//   ・Point は絶対値（[0,0] からの差分＝絶対値なので同じ経路で書ける）
//   ・ポリゴンのリングは OGR が閉じた形で持つ＝閉じたまま書く（読み手の closeRings() は no-op になる）
//   ・量子化で重なった連続点は落とす（ゼロ長セグメントを残さない）。それ以外の形状加工はしない
#include "ogrgeopbf.h"
#include "cpl_time.h"
#include <cmath>
#include <algorithm>

// ── 座標ヘルパ ────────────────────────────────────────────────────────────────

namespace {

// 経度を [-180,180] へ（pbf-base.js の fix() と同じ）
inline double WrapLon(double x) {
    while (x < -180) x += 360;
    while (x >  180) x -= 360;
    return x;
}

struct IPt { int64_t x, y; };

// 折れ線／リングを整数化し、連続重複を落とす
std::vector<IPt> Quantize(const OGRSimpleCurve* poCurve, double dfScale) {
    const int n = poCurve->getNumPoints();
    std::vector<IPt> out;
    out.reserve(n);
    for (int i = 0; i < n; i++) {
        const IPt p{ (int64_t)std::llround(WrapLon(poCurve->getX(i)) * dfScale),
                     (int64_t)std::llround(poCurve->getY(i) * dfScale) };
        if (!out.empty() && out.back().x == p.x && out.back().y == p.y) continue;
        out.push_back(p);
    }
    return out;
}

// 1本ぶんの差分を coords へ流す（累算器はこの呼び出し内で完結＝リングごとにリセット）
void AppendDeltas(const std::vector<IPt>& pts, std::vector<int64_t>& coords) {
    int64_t cx = 0, cy = 0;
    for (const IPt& p : pts) {
        coords.push_back(p.x - cx);
        coords.push_back(p.y - cy);
        cx = p.x; cy = p.y;
    }
}

// リングは閉じた状態で書く（量子化で閉じが壊れた場合だけ閉じ直す）
std::vector<IPt> QuantizeRing(const OGRLinearRing* poRing, double dfScale) {
    std::vector<IPt> pts = Quantize(poRing, dfScale);
    if (pts.size() >= 3 && (pts.front().x != pts.back().x || pts.front().y != pts.back().y))
        pts.push_back(pts.front());
    return pts;
}

}  // namespace

// ── Layer ─────────────────────────────────────────────────────────────────────

OGRGeoPBFWriteLayer::OGRGeoPBFWriteLayer(const char* pszName, const OGRSpatialReference* poSRS,
                                         OGRwkbGeometryType eGType, double dfScale)
    : m_dfScale(dfScale)
{
    m_poFeatureDefn = new OGRFeatureDefn(pszName);
    m_poFeatureDefn->Reference();
    m_poFeatureDefn->SetGeomType(eGType);

    // 形式は WGS84 固定。別の系で渡されたら黙って歪めず警告する（変換は -t_srs で行うのが GDAL の作法）
    if (poSRS) {
        OGRSpatialReference oWGS84;
        oWGS84.SetWellKnownGeogCS("WGS84");
        // 軸順の違い（OGC:CRS84 と EPSG:4326）は同一とみなす＝GeoJSON 入力で誤警告しない
        const char* const apszSameOpts[] = { "IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", nullptr };
        if (!poSRS->IsSame(&oWGS84, apszSameOpts))
            CPLError(CE_Warning, CPLE_AppDefined,
                     "GeoPBF stores WGS84 (EPSG:4326) coordinates only; "
                     "the source CRS is different and coordinates are written unchanged. "
                     "Use -t_srs EPSG:4326 to reproject.");
        OGRSpatialReference* poSRSClone = poSRS->Clone();
        poSRSClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        m_poFeatureDefn->GetGeomFieldDefn(0)->SetSpatialRef(poSRSClone);
        poSRSClone->Release();
    }
}

OGRGeoPBFWriteLayer::~OGRGeoPBFWriteLayer() { m_poFeatureDefn->Release(); }

int OGRGeoPBFWriteLayer::TestCapability(const char* pszCap) const {
    if (EQUAL(pszCap, OLCSequentialWrite)) return TRUE;
    if (EQUAL(pszCap, OLCCreateField))     return TRUE;
    if (EQUAL(pszCap, OLCStringsAsUTF8))   return TRUE;
    if (EQUAL(pszCap, OLCZGeometries))     return FALSE;   // 形式は 2D のみ
    return FALSE;
}

OGRErr OGRGeoPBFWriteLayer::CreateField(const OGRFieldDefn* poField, int /*bApproxOK*/) {
    if (!m_features.empty()) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GeoPBF: cannot add a field after features have been written.");
        return OGRERR_FAILURE;
    }
    m_poFeatureDefn->AddFieldDefn(poField);
    m_keys.push_back(poField->GetNameRef());
    return OGRERR_NONE;
}

// GEOMETRY メッセージの中身（GTYPE / LENGTH / COORDS / GARRAY）を w へ
void OGRGeoPBFWriteLayer::EncodeGeometry(const OGRGeometry* poGeom, PbfWriter& w) {
    const OGRwkbGeometryType eType = wkbFlatten(poGeom->getGeometryType());
    if (!m_bWarnedZ && poGeom->Is3D()) {
        m_bWarnedZ = true;
        CPLError(CE_Warning, CPLE_AppDefined, "GeoPBF is a 2D format; Z values are dropped.");
    }

    std::vector<int64_t>  coords;
    std::vector<uint64_t> lengths;

    switch (eType) {
        case wkbPoint: {
            const auto* p = poGeom->toPoint();
            w.varintField(TAG_GTYPE, 0);
            coords.push_back((int64_t)std::llround(WrapLon(p->getX()) * m_dfScale));
            coords.push_back((int64_t)std::llround(p->getY() * m_dfScale));
            break;
        }
        case wkbMultiPoint: {   // 全点で1本の差分列（LENGTH なし）
            w.varintField(TAG_GTYPE, 1);
            const auto* mp = poGeom->toMultiPoint();
            std::vector<IPt> pts;
            for (const auto* p : *mp) {
                const IPt q{ (int64_t)std::llround(WrapLon(p->getX()) * m_dfScale),
                             (int64_t)std::llround(p->getY() * m_dfScale) };
                pts.push_back(q);   // 点群は重複も意味を持つので落とさない
            }
            AppendDeltas(pts, coords);
            break;
        }
        case wkbLineString: {
            w.varintField(TAG_GTYPE, 2);
            AppendDeltas(Quantize(poGeom->toLineString(), m_dfScale), coords);
            break;
        }
        case wkbMultiLineString: {
            w.varintField(TAG_GTYPE, 3);
            for (const auto* ls : *poGeom->toMultiLineString()) {
                const std::vector<IPt> pts = Quantize(ls, m_dfScale);
                if (pts.empty()) continue;
                lengths.push_back(pts.size());
                AppendDeltas(pts, coords);
            }
            break;
        }
        case wkbPolygon: {
            w.varintField(TAG_GTYPE, 4);
            const auto* poly = poGeom->toPolygon();
            for (const auto* ring : *poly) {
                const std::vector<IPt> pts = QuantizeRing(ring, m_dfScale);
                if (pts.empty()) continue;
                lengths.push_back(pts.size());
                AppendDeltas(pts, coords);
            }
            break;
        }
        case wkbMultiPolygon: {   // LENGTH = [ポリゴン数, リング数, リング点数…]
            w.varintField(TAG_GTYPE, 5);
            const auto* mpoly = poGeom->toMultiPolygon();
            lengths.push_back((uint64_t)mpoly->getNumGeometries());
            for (const auto* poly : *mpoly) {
                lengths.push_back((uint64_t)poly->getNumInteriorRings() + 1);
                for (const auto* ring : *poly) {
                    const std::vector<IPt> pts = QuantizeRing(ring, m_dfScale);
                    lengths.push_back(pts.size());
                    AppendDeltas(pts, coords);
                }
            }
            break;
        }
        case wkbGeometryCollection: {
            w.varintField(TAG_GTYPE, 6);
            PbfWriter garray;
            for (const auto* sub : *poGeom->toGeometryCollection()) {
                PbfWriter sw;
                EncodeGeometry(sub, sw);
                garray.bytesField(TAG_GEOMETRY, sw.buf);
            }
            w.bytesField(TAG_GARRAY, garray.buf);
            return;   // GARRAY 経路は COORDS/LENGTH を持たない
        }
        default:
            CPLError(CE_Warning, CPLE_AppDefined,
                     "GeoPBF: unsupported geometry type %s; feature written without geometry.",
                     OGRGeometryTypeToName(eType));
            return;
    }
    if (!lengths.empty()) w.packedVarintField(TAG_LENGTH, lengths);
    if (!coords.empty())  w.packedSVarintField(TAG_COORDS, coords);
}

OGRErr OGRGeoPBFWriteLayer::ICreateFeature(OGRFeature* poFeature) {
    PbfWriter feat;

    const OGRGeometry* poGeom = poFeature->GetGeometryRef();
    OGRGeometry* poLinear = nullptr;   // 曲線は線形化してから書く（形式に曲線は無い）
    if (poGeom && OGR_GT_IsCurve(wkbFlatten(poGeom->getGeometryType())) == FALSE &&
        poGeom->hasCurveGeometry()) {
        poLinear = poGeom->getLinearGeometry();
        if (poLinear) poGeom = poLinear;
    }
    if (poGeom && !poGeom->IsEmpty()) {
        PbfWriter g;
        EncodeGeometry(poGeom, g);
        if (!g.buf.empty()) feat.bytesField(TAG_GEOMETRY, g.buf);
    }
    delete poLinear;

    // 属性＝設定済みフィールドだけを INDEX（KEYS の添字）と VALUE の同順で並べる
    std::vector<uint64_t> index;
    PbfWriter values;
    const int nFields = m_poFeatureDefn->GetFieldCount();
    for (int i = 0; i < nFields; i++) {
        if (!poFeature->IsFieldSetAndNotNull(i)) continue;
        const OGRFieldDefn* fd = m_poFeatureDefn->GetFieldDefn(i);
        PbfWriter v;
        switch (fd->GetType()) {
            case OFTInteger:
                if (fd->GetSubType() == OFSTBoolean)
                    v.varintField(DT_BOOL, poFeature->GetFieldAsInteger(i) ? 1 : 0);
                else
                    v.svarintField(DT_INTEGER, poFeature->GetFieldAsInteger(i));
                break;
            case OFTInteger64:
                v.svarintField(DT_INTEGER, poFeature->GetFieldAsInteger64(i));
                break;
            case OFTReal:
                v.doubleField(DT_FLOAT, poFeature->GetFieldAsDouble(i));
                break;
            case OFTDate:
            case OFTDateTime: {
                int y, mo, d, h, mi, tz; float s;
                if (poFeature->GetFieldAsDateTime(i, &y, &mo, &d, &h, &mi, &s, &tz)) {
                    struct tm brokendown;
                    memset(&brokendown, 0, sizeof(brokendown));
                    brokendown.tm_year = y - 1900; brokendown.tm_mon = mo - 1;
                    brokendown.tm_mday = d; brokendown.tm_hour = h;
                    brokendown.tm_min = mi;  brokendown.tm_sec = (int)(s + 0.5f);
                    v.svarintField(DT_DATE, (int64_t)CPLYMDHMSToUnixTime(&brokendown));
                } else {
                    v.stringField(DT_STRING, poFeature->GetFieldAsString(i));
                }
                break;
            }
            case OFTIntegerList: case OFTInteger64List:
            case OFTRealList:    case OFTStringList: {
                char* pszJson = poFeature->GetFieldAsSerializedJSon(i);
                v.stringField(DT_JSON, pszJson ? pszJson : "");
                CPLFree(pszJson);
                break;
            }
            default:
                v.stringField(DT_STRING, poFeature->GetFieldAsString(i));
                break;
        }
        index.push_back((uint64_t)i);
        values.bytesField(TAG_VALUE, v.buf);
    }
    if (!index.empty()) {
        feat.packedVarintField(TAG_INDEX, index);
        feat.raw(values.buf.data(), values.buf.size());
    }

    m_features.push_back(std::move(feat.buf));
    poFeature->SetFID((GIntBig)m_features.size() - 1);
    return OGRERR_NONE;
}

// ── Dataset ───────────────────────────────────────────────────────────────────

OGRGeoPBFWriteDataset::OGRGeoPBFWriteDataset(const char* pszFilename, double dfScale)
    : m_osFilename(pszFilename), m_dfScale(dfScale)
{
    SetDescription(pszFilename);
}

OGRGeoPBFWriteDataset::~OGRGeoPBFWriteDataset() {
    OGRGeoPBFWriteDataset::Close();
    delete m_poLayer;
}

const OGRLayer* OGRGeoPBFWriteDataset::GetLayer(int i) const {
    return (i == 0) ? m_poLayer : nullptr;
}

int OGRGeoPBFWriteDataset::TestCapability(const char* pszCap) const {
    if (EQUAL(pszCap, ODsCCreateLayer)) return m_poLayer == nullptr;   // 単層フォーマット
    if (EQUAL(pszCap, ODsCZGeometries)) return FALSE;
    return FALSE;
}

OGRLayer* OGRGeoPBFWriteDataset::ICreateLayer(const char* pszName,
                                             const OGRGeomFieldDefn* poGeomFieldDefn,
                                             CSLConstList /*papszOptions*/) {
    if (m_poLayer) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GeoPBF holds a single layer per file; layer '%s' cannot be added.", pszName);
        return nullptr;
    }
    const OGRSpatialReference* poSRS = poGeomFieldDefn ? poGeomFieldDefn->GetSpatialRef() : nullptr;
    const OGRwkbGeometryType eGType = poGeomFieldDefn ? poGeomFieldDefn->GetType() : wkbUnknown;
    m_poLayer = new OGRGeoPBFWriteLayer(pszName, poSRS, eGType, m_dfScale);
    return m_poLayer;
}

// ヘッダ（NAME / KEYS / PRECISION）→ FARRAY（FEATURE の並び）の順に一括で書く
OGRErr OGRGeoPBFWriteDataset::WriteFile() {
    PbfWriter out;
    const char* pszName = m_poLayer ? m_poLayer->GetLayerDefn()->GetName() : "layer";
    out.stringField(TAG_NAME, pszName);
    if (m_poLayer)
        for (const std::string& k : m_poLayer->Keys()) out.stringField(TAG_KEYS, k);
    out.varintField(TAG_PRECISION, (uint64_t)std::lround(std::log10(m_dfScale)));

    PbfWriter farray;
    if (m_poLayer)
        for (const std::vector<uint8_t>& f : m_poLayer->Features()) farray.bytesField(TAG_FEATURE, f);
    out.bytesField(TAG_FARRAY, farray.buf);

    VSILFILE* fp = VSIFOpenL(m_osFilename.c_str(), "wb");
    if (!fp) {
        CPLError(CE_Failure, CPLE_OpenFailed, "GeoPBF: cannot create %s", m_osFilename.c_str());
        return OGRERR_FAILURE;
    }
    const size_t nWritten = VSIFWriteL(out.buf.data(), 1, out.buf.size(), fp);
    const int nCloseErr = VSIFCloseL(fp);
    if (nWritten != out.buf.size() || nCloseErr != 0) {
        CPLError(CE_Failure, CPLE_FileIO, "GeoPBF: short write on %s", m_osFilename.c_str());
        return OGRERR_FAILURE;
    }
    return OGRERR_NONE;
}

GEOPBF_CLOSE_DEFN(OGRGeoPBFWriteDataset) {
    CPLErr eErr = CE_None;
    if (!m_bWritten) {
        m_bWritten = true;
        if (WriteFile() != OGRERR_NONE) eErr = CE_Failure;
    }
    return eErr;
}

// ── Driver entry point ────────────────────────────────────────────────────────

GDALDataset* OGRGeoPBFDriverCreate(const char* pszName, int, int, int, GDALDataType,
                                   GEOPBF_CREATE_OPTIONS papszOptions) {
    const int nPrecision = atoi(CSLFetchNameValueDef(papszOptions, "PRECISION", "6"));
    if (nPrecision < 0 || nPrecision > 9) {
        CPLError(CE_Failure, CPLE_IllegalArg, "GeoPBF: PRECISION must be between 0 and 9.");
        return nullptr;
    }
    return new OGRGeoPBFWriteDataset(pszName, std::pow(10.0, nPrecision));
}
