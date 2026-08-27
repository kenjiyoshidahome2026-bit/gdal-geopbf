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
                                         OGRwkbGeometryType eGType, double dfScale, bool bScaleFromOption)
    : m_dfScale(dfScale), m_bScaleFromOption(bScaleFromOption)
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
            default: {
                // 元が BUFS への参照だったフィールドは、その型のまま書き戻す
                const char* pszKind = GetMetadataItem(fd->GetNameRef(), "GEOPBF");
                const int nType = (pszKind && EQUAL(pszKind, "BLOB"))  ? DT_BLOB
                                : (pszKind && EQUAL(pszKind, "IMAGE")) ? DT_IMAGE
                                : DT_STRING;
                v.stringField(nType, poFeature->GetFieldAsString(i));
                break;
            }
        }
        index.push_back((uint64_t)i);
        values.bytesField(TAG_VALUE, v.buf);
    }
    // ★順序が仕様の一部: VALUE を先、INDEX を後に書く。参照実装の読み手は VALUE を
    // 出現順に溜めながら進み、INDEX に出会った時点で対応付ける＝INDEX が先にあると
    // 値が空のまま結び付き、属性が全部失われる（自前の読み手は両方溜めてから突合する
    // ので気付けない。JS 実装との突合で発覚）。
    if (!index.empty()) {
        feat.raw(values.buf.data(), values.buf.size());
        feat.packedVarintField(TAG_INDEX, index);
    }

    m_features.push_back(std::move(feat.buf));
    poFeature->SetFID((GIntBig)m_features.size() - 1);
    return OGRERR_NONE;
}

// ── Dataset ───────────────────────────────────────────────────────────────────

OGRGeoPBFWriteDataset::OGRGeoPBFWriteDataset(const char* pszFilename, double dfScale, bool bGzip,
                                             bool bScaleFromOption)
    : m_osFilename(pszFilename), m_dfScale(dfScale), m_bGzip(bGzip),
      m_bScaleFromOption(bScaleFromOption)
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

    // 元レイヤが座標解像度を宣言していれば、それに合わせて量子化幅を決める（GDAL 標準の経路）。
    // -dsco PRECISION= の明示があればそちらが勝つ。0.5 桁以上ずれる解像度は近い桁へ丸める。
    double dfScale = m_dfScale;
    if (!m_bScaleFromOption && poGeomFieldDefn) {
        const double dfRes = poGeomFieldDefn->GetCoordinatePrecision().dfXYResolution;
        CPLDebug("GeoPBF", "inheriting XY resolution %.3g from the source layer", dfRes);
        if (dfRes > 0) {
            const int n = (int)std::lround(-std::log10(dfRes));
            if (n >= 0 && n <= 9) dfScale = std::pow(10.0, n);
        }
    }
    m_poLayer = new OGRGeoPBFWriteLayer(pszName, poSRS, eGType, dfScale, m_bScaleFromOption);
    return m_poLayer;
}

// ヘッダ（NAME / KEYS / PRECISION）→ FARRAY（FEATURE の並び）の順に一括で書く
OGRErr OGRGeoPBFWriteDataset::WriteFile() {
    PbfWriter out;
    const char* pszName = m_poLayer ? m_poLayer->GetLayerDefn()->GetName() : "layer";
    out.stringField(TAG_NAME, pszName);
    if (m_poLayer)
        for (const std::string& k : m_poLayer->Keys()) out.stringField(TAG_KEYS, k);
    // 実際に符号化に使われた縮尺はレイヤ側が持つ（メタデータで引き継いだ場合はそちらが正）
    const double dfScale = m_poLayer ? m_poLayer->Scale() : m_dfScale;
    out.varintField(TAG_PRECISION, (uint64_t)std::lround(std::log10(dfScale)));

    // BUFS（バイナリのプール）＝読み手が BUFS メタデータ領域に置いたものをそのまま戻す。
    // 地物の値には参照（"name:mime:id"）だけが入っているので、ここが欠けると中身が消える。
    CSLConstList papszBufs = GetMetadata("BUFS");   // 3.13 は const 付きで返る
    for (int i = 0; papszBufs && papszBufs[i]; i++) {
        char* pszKey = nullptr;
        const char* pszB64 = CPLParseNameValue(papszBufs[i], &pszKey);
        if (pszB64 && pszKey) {
            std::vector<uint8_t> buf(pszB64, pszB64 + strlen(pszB64));
            buf.push_back(0);
            const int nLen = CPLBase64DecodeInPlace(buf.data());
            buf.resize(nLen > 0 ? nLen : 0);
            out.bytesField(TAG_BUFS, buf);
        }
        CPLFree(pszKey);
    }
    if (papszBufs && CSLCount(papszBufs))
        CPLDebug("GeoPBF", "%d binary buffers carried over", CSLCount(papszBufs));

    PbfWriter farray;
    if (m_poLayer)
        for (const std::vector<uint8_t>& f : m_poLayer->Features()) farray.bytesField(TAG_FEATURE, f);
    out.bytesField(TAG_FARRAY, farray.buf);

    // COMPRESS=GZIP＝GDAL の /vsigzip/ へ書く（自前 zlib を持たない＝依存ゼロを維持）。
    // 読み手は gzip 印を見て透過的に開くので、拡張子は .geopbf のままでよい。
    const std::string osOut = m_bGzip ? "/vsigzip/" + m_osFilename : m_osFilename;
    VSILFILE* fp = VSIFOpenL(osOut.c_str(), "wb");
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
    // 既定は GZIP＝GeoPBF は gzip 済みで配布されるのが形式の作法（形式作者の定め）。
    // 無圧縮が要るのは、伸長できない相手へ渡す時だけ。
    const char* pszCompress = CSLFetchNameValueDef(papszOptions, "COMPRESS", "GZIP");
    if (!EQUAL(pszCompress, "NONE") && !EQUAL(pszCompress, "GZIP")) {
        CPLError(CE_Failure, CPLE_IllegalArg, "GeoPBF: COMPRESS must be NONE or GZIP.");
        return nullptr;
    }
    const bool bPrecisionGiven = CSLFetchNameValue(papszOptions, "PRECISION") != nullptr;
    return new OGRGeoPBFWriteDataset(pszName, std::pow(10.0, nPrecision),
                                     EQUAL(pszCompress, "GZIP"), bPrecisionGiven);
}
