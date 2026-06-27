#pragma once
#include "ogrsf_frmts.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cinttypes>

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
    void            ResetReading() override;
    int             TestCapability(const char*) const override { return FALSE; }

private:
    OGRGeoPBFDataset* m_poDS;
    OGRFeatureDefn*   m_poFeatureDefn;
    size_t   m_pos;
    GIntBig  m_nFID = 0;

    OGRGeometry* DecodeGeometry(PbfReader& r);
    std::string  DecodeValue(PbfReader& r);
};
