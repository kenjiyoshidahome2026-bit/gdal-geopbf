#pragma once
#include "ogrsf_frmts.h"
#include "gdal_version.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cinttypes>

// ── GDAL 版差の吸収 ───────────────────────────────────────────────────────────
// 3.13 で二つ変わった: GDALDataset::Close() が進捗引数を取るようになり、
// ドライバの Create コールバックが char** から CSLConstList になった。
// QGIS 4.2 が同梱するのは 3.12 系＝ここを吸収しないと最も客の多い環境で建たない。
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
#  define GEOPBF_CREATE_OPTIONS CSLConstList
#  define GEOPBF_CLOSE_DECL \
      CPLErr Close(GDALProgressFunc pfnProgress = nullptr, void* pProgressData = nullptr) override
#  define GEOPBF_CLOSE_DEFN(cls) CPLErr cls::Close(GDALProgressFunc, void*)
#else
#  define GEOPBF_CREATE_OPTIONS char**
#  define GEOPBF_CLOSE_DECL      CPLErr Close() override
#  define GEOPBF_CLOSE_DEFN(cls) CPLErr cls::Close()
#endif

// ── Minimal Protobuf reader ───────────────────────────────────────────────────

struct PbfReader {
    const uint8_t* data;
    size_t pos, end;

    PbfReader(const uint8_t* d, size_t p, size_t e) : data(d), pos(p), end(e) {}
    bool atEnd() const { return pos >= end; }

    uint64_t readVarint() {
        uint64_t v = 0; int s = 0;
        while (pos < end) {
            uint8_t b = data[pos++];
            v |= (uint64_t)(b & 0x7F) << s;
            if (!(b & 0x80)) break;
            s += 7;
        }
        return v;
    }
    int64_t readSVarint() {
        uint64_t n = readVarint();
        return (int64_t)((n >> 1) ^ -(int64_t)(n & 1));
    }
    double readDouble() {
        double v; memcpy(&v, data + pos, 8); pos += 8; return v;
    }
    std::string readString(size_t len) {
        std::string s((const char*)data + pos, len); pos += len; return s;
    }

    // Returns field number; sets wire_type
    int readTag(int& wire_type) {
        uint64_t t = readVarint(); wire_type = t & 7; return (int)(t >> 3);
    }
    void skipField(int wire_type) {
        switch (wire_type) {
            case 0: readVarint(); break;
            case 1: pos += 8; break;
            case 2: { size_t n = readVarint(); pos += n; break; }
            case 5: pos += 4; break;
        }
    }
    // Enter a LEN-type sub-message; advances parent past it
    PbfReader enterMessage() {
        size_t len = readVarint();
        PbfReader sub(data, pos, pos + len);
        pos += len;
        return sub;
    }
    std::vector<int64_t> readPackedSVarint() {
        size_t len = readVarint(), ep = pos + len;
        std::vector<int64_t> v;
        while (pos < ep) v.push_back(readSVarint());
        return v;
    }
    std::vector<uint64_t> readPackedVarint() {
        size_t len = readVarint(), ep = pos + len;
        std::vector<uint64_t> v;
        while (pos < ep) v.push_back(readVarint());
        return v;
    }
};

// ── Minimal Protobuf writer ───────────────────────────────────────────────────
// PbfReader と対称。依存ゼロを守るため protobuf ライブラリは使わない（形式が小さいので手書きで足りる）。

struct PbfWriter {
    std::vector<uint8_t> buf;

    void raw(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }
    void varint(uint64_t v) {
        while (v >= 0x80) { buf.push_back((uint8_t)(v | 0x80)); v >>= 7; }
        buf.push_back((uint8_t)v);
    }
    static uint64_t zigzag(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
    void svarint(int64_t v) { varint(zigzag(v)); }
    void tag(int field, int wire_type) { varint(((uint64_t)field << 3) | (uint64_t)wire_type); }

    void varintField(int f, uint64_t v)  { tag(f, 0); varint(v); }
    void svarintField(int f, int64_t v)  { tag(f, 0); svarint(v); }
    void doubleField(int f, double d) {
        tag(f, 1);
        double t = d; CPL_LSBPTR64(&t);            // protobuf は常にリトルエンディアン
        uint8_t b[8]; memcpy(b, &t, 8); raw(b, 8);
    }
    void stringField(int f, const std::string& s) {
        tag(f, 2); varint(s.size()); raw((const uint8_t*)s.data(), s.size());
    }
    void bytesField(int f, const std::vector<uint8_t>& b) {
        tag(f, 2); varint(b.size()); raw(b.data(), b.size());
    }
    void packedVarintField(int f, const std::vector<uint64_t>& v) {
        PbfWriter t; for (uint64_t x : v) t.varint(x); bytesField(f, t.buf);
    }
    void packedSVarintField(int f, const std::vector<int64_t>& v) {
        PbfWriter t; for (int64_t x : v) t.svarint(x); bytesField(f, t.buf);
    }
};

// ── geopbf format constants ───────────────────────────────────────────────────

enum { TAG_NAME=1, TAG_KEYS=2, TAG_PRECISION=3, TAG_BUFS=4,
       TAG_FARRAY=5, TAG_FEATURE=6, TAG_GEOMETRY=7, TAG_GTYPE=8,
       TAG_LENGTH=9, TAG_COORDS=10, TAG_VALUE=11, TAG_INDEX=12,
       TAG_GARRAY=13, TAG_DESCRIPTION=14, TAG_LICENSE=15,
       TAG_ATTRIBUTION=16, TAG_MIN_ZOOM=17, TAG_MAX_ZOOM=18 };

enum { DT_NULL=0, DT_BOOL=1, DT_INTEGER=2, DT_FLOAT=3,
       DT_STRING=4, DT_DATE=5, DT_COLOR=6, DT_FUNC=7,
       DT_JSON=8, DT_BBOX=9, DT_BLOB=10, DT_IMAGE=11 };

// ── OGR classes ───────────────────────────────────────────────────────────────

class OGRGeoPBFLayer;

class OGRGeoPBFDataset : public GDALDataset {
public:
    OGRGeoPBFDataset();
    ~OGRGeoPBFDataset();
    int Open(const char* pszFilename);
    int GetLayerCount() const override { return 1; }
    const OGRLayer* GetLayer(int i) const override;

    std::vector<uint8_t>              m_data;
    std::vector<std::string>          m_keys;
    std::vector<std::vector<uint8_t>> m_bufs;
    double  m_scale    = 1e6;
    std::string m_name;
    size_t  m_farrayPos = 0;
    size_t  m_farrayEnd = 0;

private:
    OGRGeoPBFLayer* m_poLayer = nullptr;
};

class OGRGeoPBFLayer : public OGRLayer {
public:
    OGRGeoPBFLayer(OGRGeoPBFDataset* poDS);
    ~OGRGeoPBFLayer();
    const OGRFeatureDefn* GetLayerDefn() const override { return m_poFeatureDefn; }
    OGRFeature*     GetNextFeature() override;
    OGRFeature*     GetFeature(GIntBig nFID) override;
    void            ResetReading() override;
    GIntBig         GetFeatureCount(int bForce = TRUE) override;
    OGRErr          IGetExtent(int iGeomField, OGREnvelope* psExtent, bool bForce) override;
    int             TestCapability(const char*) const override;

private:
    OGRGeoPBFDataset* m_poDS;
    OGRFeatureDefn*   m_poFeatureDefn;
    size_t   m_pos;
    GIntBig  m_nFID = 0;

    // ── 空間索引（メモリ内・ファイル形式は不変） ──────────────────────────────
    // ファイル自体は索引を持たない（小ささと引き換え）。開いた時点で中身は全部
    // メモリにあるので、最初に必要になった時だけ一度走査して「地物の位置と bbox」を作る。
    // これで画面範囲の問い合わせがジオメトリを組み立てずに済み、QGIS のパン/ズームが
    // 全件走査でなくなる（500k点で 276ms → 数ms）。索引は座標を 1e-7 度の int32 で持つ
    // （経度 ±180e7 は int32 に収まらないため 1e-6 で保持＝地物選別には十分）。
    struct FeatRec {
        uint64_t start, end;          // FEATURE メッセージ本体のバイト範囲
        int32_t  minx, miny, maxx, maxy;   // bbox（1e-6 度の整数）
    };
    std::vector<FeatRec>               m_index;
    std::vector<std::vector<uint32_t>> m_grid;   // 一様格子 → 地物添字
    double m_dfGridMinX = 0, m_dfGridMinY = 0, m_dfGridStepX = 1, m_dfGridStepY = 1;
    int    m_nGridW = 0, m_nGridH = 0;
    OGREnvelope m_sExtent;
    bool   m_bIndexBuilt = false;
    size_t m_iNextIdx = 0;                       // 索引利用時の走査位置
    std::vector<uint32_t> m_anCandidates;        // 空間フィルタの候補（格子から集めた）
    bool   m_bUseCandidates = false;
    OGREnvelope m_sCandEnv;                      // 候補列を作った時のフィルタ範囲（変わったら作り直す）

    void         BuildIndex();
    void         PrepareCandidates();            // 現在の空間フィルタから候補列を作る
    OGRFeature*  FeatureFromRecord(const FeatRec& rec, GIntBig nFID);
    OGRGeometry* DecodeGeometry(PbfReader& r);
    std::string  DecodeValue(PbfReader& r);
};

// ── Write side ────────────────────────────────────────────────────────────────
// 単層フォーマット＝1データセット1レイヤ。地物は逐次エンコードしてメモリに積み、
// Close()/デストラクタで「ヘッダ → FARRAY」を一括で書き出す（KEYS 辞書が全地物を見終わるまで確定しないため）。

GDALDataset* OGRGeoPBFDriverCreate(const char* pszName, int, int, int, GDALDataType, GEOPBF_CREATE_OPTIONS);

class OGRGeoPBFWriteLayer final : public OGRLayer {
public:
    OGRGeoPBFWriteLayer(const char* pszName, const OGRSpatialReference* poSRS,
                        OGRwkbGeometryType eGType, double dfScale);
    ~OGRGeoPBFWriteLayer();

    const OGRFeatureDefn* GetLayerDefn() const override { return m_poFeatureDefn; }
    void        ResetReading() override {}
    OGRFeature* GetNextFeature() override { return nullptr; }
    int         TestCapability(const char* pszCap) const override;
    OGRErr      CreateField(const OGRFieldDefn* poField, int bApproxOK = TRUE) override;
    OGRErr      ICreateFeature(OGRFeature* poFeature) override;

    const std::vector<std::string>&          Keys()     const { return m_keys; }
    const std::vector<std::vector<uint8_t>>& Features() const { return m_features; }

private:
    OGRFeatureDefn*                   m_poFeatureDefn;
    std::vector<std::string>          m_keys;        // = ヘッダの KEYS（フィールド名辞書）
    std::vector<std::vector<uint8_t>> m_features;    // 各要素＝FEATURE メッセージの中身
    double                            m_dfScale;
    bool                              m_bWarnedZ = false;   // Z は形式に無い＝一度だけ警告

    void EncodeGeometry(const OGRGeometry* poGeom, PbfWriter& w);   // GEOMETRY メッセージの中身を書く
};

class OGRGeoPBFWriteDataset final : public GDALDataset {
public:
    OGRGeoPBFWriteDataset(const char* pszFilename, double dfScale);
    ~OGRGeoPBFWriteDataset() override;

    int              GetLayerCount() const override { return m_poLayer ? 1 : 0; }
    const OGRLayer*  GetLayer(int i) const override;
    int              TestCapability(const char* pszCap) const override;
    GEOPBF_CLOSE_DECL;

protected:
    OGRLayer* ICreateLayer(const char* pszName, const OGRGeomFieldDefn* poGeomFieldDefn,
                           CSLConstList papszOptions) override;

private:
    std::string           m_osFilename;
    double                m_dfScale;
    OGRGeoPBFWriteLayer*  m_poLayer = nullptr;
    bool                  m_bWritten = false;

    OGRErr WriteFile();
};
