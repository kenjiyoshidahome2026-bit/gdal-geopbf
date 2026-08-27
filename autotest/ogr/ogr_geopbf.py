#!/usr/bin/env pytest
###############################################################################
# Project:  GDAL/OGR
# Purpose:  Test the GeoPBF driver
# Author:   Kenji Yoshida
#
###############################################################################
# SPDX-License-Identifier: MIT
###############################################################################
#
# Layout follows GDAL's autotest tree so the file can be moved to
# autotest/ogr/ogr_geopbf.py unchanged. It deliberately depends only on
# osgeo + pytest, so it also runs standalone against an out-of-tree plugin:
#
#     GDAL_DRIVER_PATH=../../build pytest ogr_geopbf.py
#
# When adopted in-tree, the two helpers at the bottom of this header can be
# replaced by gdaltest / ogrtest equivalents.

import gzip
import json
import math
import os

import pytest

from osgeo import gdal, ogr, osr

pytestmark = pytest.mark.require_driver("GeoPBF")

DATA = os.path.join(os.path.dirname(__file__), "data", "geopbf")


def _geom_almost_equal(g1, g2, tol=1e-6):
    """Geometry comparison that tolerates the format's coordinate quantization."""
    if g1 is None or g2 is None:
        return g1 is None and g2 is None
    if g1.GetGeometryType() != g2.GetGeometryType():
        return False
    if g1.GetGeometryCount() != g2.GetGeometryCount():
        return False
    if g1.GetGeometryCount() > 0:
        return all(
            _geom_almost_equal(g1.GetGeometryRef(i), g2.GetGeometryRef(i), tol)
            for i in range(g1.GetGeometryCount())
        )
    if g1.GetPointCount() != g2.GetPointCount():
        return False
    return all(
        math.isclose(g1.GetX(i), g2.GetX(i), abs_tol=tol)
        and math.isclose(g1.GetY(i), g2.GetY(i), abs_tol=tol)
        for i in range(g1.GetPointCount())
    )


SAMPLE_WKT = [
    "POINT (139.766084 35.681382)",
    "LINESTRING (139.7 35.6,139.8 35.7,140.0 35.75)",
    "POLYGON ((139 35,139.5 35,139.5 35.5,139 35.5,139 35),"
    "(139.1 35.1,139.2 35.1,139.2 35.2,139.1 35.2,139.1 35.1))",
    "MULTIPOINT ((130 33),(131 34))",
    "MULTILINESTRING ((135 34,135.1 34.1),(136 35,136.2 35.2,136.3 35.1))",
    "MULTIPOLYGON (((130 30,130.5 30,130.5 30.5,130 30)),"
    "((131 31,131.5 31,131.5 31.5,131 31)))",
    "GEOMETRYCOLLECTION (POINT (141 43),LINESTRING (141.1 43.1,141.2 43.2))",
]


###############################################################################
# Open a file produced by the reference (JavaScript) implementation


def test_ogr_geopbf_open():

    ds = ogr.Open(os.path.join(DATA, "point.geopbf"))
    assert ds is not None
    assert ds.GetLayerCount() == 1

    lyr = ds.GetLayer(0)
    assert lyr.GetName() == "point"
    assert lyr.GetFeatureCount() == 3

    srs = lyr.GetSpatialRef()
    assert srs is not None
    assert srs.GetAuthorityCode("GEOGCS") == "4326"

    f = lyr.GetNextFeature()
    assert _geom_almost_equal(f.GetGeometryRef(), ogr.CreateGeometryFromWkt("POINT (139.5 35.5)"))
    assert f["name"] == "alpha"
    assert f["num"] == "1"  # every value is exposed as a string by the reader


###############################################################################
# Whole-file gzip is accepted (that is how the web tooling exports .geopbf)


def test_ogr_geopbf_gzip(tmp_path):

    src = os.path.join(DATA, "point.geopbf")
    dst = str(tmp_path / "gzipped.geopbf")
    with open(src, "rb") as fsrc, gzip.open(dst, "wb") as fdst:
        fdst.write(fsrc.read())

    ds = ogr.Open(dst)
    assert ds is not None, "gzipped .geopbf should be read transparently"
    assert ds.GetLayer(0).GetFeatureCount() == 3

    # An explicit /vsigzip/ prefix must not be applied twice
    ds = ogr.Open("/vsigzip/" + dst)
    assert ds is not None
    assert ds.GetLayer(0).GetFeatureCount() == 3


###############################################################################
# Identify() works on content, not only on the file extension


def test_ogr_geopbf_identify_without_extension(tmp_path):

    dst = str(tmp_path / "no_extension")
    with open(os.path.join(DATA, "point.geopbf"), "rb") as fsrc:
        open(dst, "wb").write(fsrc.read())

    ds = ogr.Open(dst)
    assert ds is not None
    assert ds.GetDriver().GetName() == "GeoPBF"


@pytest.mark.parametrize(
    "content,expected_driver",
    [
        (b"id,name\n1,test\n", "CSV"),
        (b'{"type":"FeatureCollection","features":[]}', "GeoJSON"),
    ],
)
def test_ogr_geopbf_does_not_hijack(tmp_path, content, expected_driver):
    """Identify() must not claim files belonging to other drivers."""

    dst = str(tmp_path / "other")
    open(dst, "wb").write(content)
    ds = ogr.Open(dst)
    if ds is not None:
        assert ds.GetDriver().GetName() == expected_driver


def test_ogr_geopbf_reject_garbage(tmp_path):

    dst = str(tmp_path / "garbage.geopbf")
    open(dst, "wb").write(b"\xde\xad\xbe\xef" * 8)
    with gdal.quiet_errors():
        assert ogr.Open(dst) is None


###############################################################################
# Write support: every geometry type survives a round trip


def test_ogr_geopbf_write_roundtrip(tmp_path):

    filename = str(tmp_path / "out.geopbf")

    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer("roundtrip", srs=srs, geom_type=ogr.wkbUnknown)
    lyr.CreateField(ogr.FieldDefn("name", ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn("num", ogr.OFTInteger))
    lyr.CreateField(ogr.FieldDefn("val", ogr.OFTReal))

    for i, wkt in enumerate(SAMPLE_WKT):
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
        f["name"] = f"feat{i}"
        f["num"] = i
        f["val"] = i + 0.5
        lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr.GetName() == "roundtrip"
    assert lyr.GetFeatureCount() == len(SAMPLE_WKT)

    for i, wkt in enumerate(SAMPLE_WKT):
        f = lyr.GetNextFeature()
        expected = ogr.CreateGeometryFromWkt(wkt)
        assert _geom_almost_equal(f.GetGeometryRef(), expected), (
            f"geometry {i} ({expected.GetGeometryName()}) did not round trip: "
            f"{f.GetGeometryRef().ExportToWkt()}"
        )
        assert f["name"] == f"feat{i}"
        assert f["num"] == str(i)
        assert float(f["val"]) == pytest.approx(i + 0.5)


###############################################################################
# Attribute types


def test_ogr_geopbf_write_field_types(tmp_path):

    filename = str(tmp_path / "fields.geopbf")

    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(filename)
    lyr = ds.CreateLayer("fields", geom_type=ogr.wkbPoint)
    for name, ftype in [
        ("s", ogr.OFTString),
        ("i", ogr.OFTInteger),
        ("i64", ogr.OFTInteger64),
        ("r", ogr.OFTReal),
        ("d", ogr.OFTDate),
        ("dt", ogr.OFTDateTime),
        ("il", ogr.OFTIntegerList),
    ]:
        lyr.CreateField(ogr.FieldDefn(name, ftype))
    bool_fld = ogr.FieldDefn("b", ogr.OFTInteger)
    bool_fld.SetSubType(ogr.OFSTBoolean)
    lyr.CreateField(bool_fld)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (1 2)"))
    f["s"] = "text"
    f["i"] = -7
    f["i64"] = 12345678901
    f["r"] = 1.25
    f["d"] = "2026/09/04"
    f["dt"] = "2026/09/04 09:30:00"
    f.SetFieldIntegerList(lyr.GetLayerDefn().GetFieldIndex("il"), [1, 2, 3])
    f["b"] = 1
    lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(filename)
    f = ds.GetLayer(0).GetNextFeature()
    assert f["s"] == "text"
    assert f["i"] == "-7"
    assert f["i64"] == "12345678901"
    assert float(f["r"]) == pytest.approx(1.25)
    assert f["d"].startswith("2026-09-04")
    assert f["dt"].startswith("2026-09-04T09:30")
    assert json.loads(f["il"]) == [1, 2, 3]
    assert f["b"] == "true"


###############################################################################
# PRECISION creation option


@pytest.mark.parametrize("precision,tol", [(3, 1e-3), (6, 1e-6)])
def test_ogr_geopbf_write_precision(tmp_path, precision, tol):

    filename = str(tmp_path / f"prec{precision}.geopbf")

    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(
        filename, options=[f"PRECISION={precision}"]
    )
    lyr = ds.CreateLayer("prec", geom_type=ogr.wkbPoint)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (139.7660841 35.6813821)"))
    lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(filename)   # データセットは掴んだままにする（先に解放されるとレイヤが無効になる）
    f = ds.GetLayer(0).GetNextFeature()
    g = f.GetGeometryRef()
    assert g.GetX(0) == pytest.approx(139.7660841, abs=tol)
    assert g.GetY(0) == pytest.approx(35.6813821, abs=tol)


def test_ogr_geopbf_write_precision_invalid(tmp_path):

    with gdal.quiet_errors():
        ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(
            str(tmp_path / "bad.geopbf"), options=["PRECISION=42"]
        )
    assert ds is None


###############################################################################
# GeoPBF holds a single layer per file


def test_ogr_geopbf_write_single_layer(tmp_path):

    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(str(tmp_path / "one.geopbf"))
    assert ds.CreateLayer("first") is not None
    with gdal.quiet_errors():
        assert ds.CreateLayer("second") is None
    ds = None


###############################################################################
# Empty layer still produces a readable file


def test_ogr_geopbf_write_empty(tmp_path):

    filename = str(tmp_path / "empty.geopbf")
    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(filename)
    ds.CreateLayer("empty", geom_type=ogr.wkbPoint)
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    assert ds.GetLayer(0).GetFeatureCount() == 0


###############################################################################
# Update mode is not supported


def test_ogr_geopbf_no_update():

    with gdal.quiet_errors():
        assert ogr.Open(os.path.join(DATA, "point.geopbf"), update=1) is None


###############################################################################
# Spatial index: the layer builds one in memory, so results must match a brute
# force scan exactly — a wrong index is worse than no index.


@pytest.fixture()
def indexed_layer(tmp_path):
    """A layer mixing points, a polygon spanning many grid cells, and a
    geometry-less feature."""

    filename = str(tmp_path / "index.geopbf")
    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(filename)
    lyr = ds.CreateLayer("idx", geom_type=ogr.wkbUnknown)
    lyr.CreateField(ogr.FieldDefn("tag", ogr.OFTString))

    wkts = []
    for i in range(20):
        for j in range(20):
            wkts.append(f"POINT ({130 + i * 0.5} {30 + j * 0.5})")
    # spans a wide area: must be found from any cell it overlaps
    wkts.append("POLYGON ((131 31,139 31,139 39,131 39,131 31))")
    # long diagonal line
    wkts.append("LINESTRING (130 30,139.5 39.5)")

    for i, wkt in enumerate(wkts):
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
        f["tag"] = f"g{i}"
        lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())   # no geometry at all
    f["tag"] = "nogeom"
    lyr.CreateFeature(f)
    ds = None

    return filename, len(wkts) + 1


def test_ogr_geopbf_capabilities(indexed_layer):

    filename, _ = indexed_layer
    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    assert lyr.TestCapability(ogr.OLCFastFeatureCount)
    assert lyr.TestCapability(ogr.OLCFastGetExtent)
    assert lyr.TestCapability(ogr.OLCFastSpatialFilter)
    assert lyr.TestCapability(ogr.OLCRandomRead)


def test_ogr_geopbf_extent_and_count(indexed_layer):

    filename, total = indexed_layer
    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    assert lyr.GetFeatureCount() == total

    minx, maxx, miny, maxy = lyr.GetExtent()
    assert minx == pytest.approx(130, abs=1e-6)
    assert maxx == pytest.approx(139.5, abs=1e-6)
    assert miny == pytest.approx(30, abs=1e-6)
    assert maxy == pytest.approx(39.5, abs=1e-6)


@pytest.mark.parametrize(
    "window",
    [
        (130.0, 30.0, 130.4, 30.4),     # corner, few features
        (134.0, 34.0, 135.0, 35.0),     # middle, crosses the big polygon
        (138.9, 38.9, 140.0, 40.0),     # far corner
        (120.0, 20.0, 150.0, 50.0),     # everything
        (100.0, 10.0, 110.0, 20.0),     # nothing
    ],
)
def test_ogr_geopbf_spatial_filter_matches_bruteforce(indexed_layer, window):

    filename, _ = indexed_layer

    # ground truth: no filter, test every geometry in Python
    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    win = ogr.CreateGeometryFromWkt(
        "POLYGON (({0} {1},{2} {1},{2} {3},{0} {3},{0} {1}))".format(
            window[0], window[1], window[2], window[3]
        )
    )
    expected = set()
    for f in lyr:
        g = f.GetGeometryRef()
        if g is not None and g.Intersects(win):
            expected.add(f["tag"])

    # indexed path
    ds2 = ogr.Open(filename)
    lyr2 = ds2.GetLayer(0)
    lyr2.SetSpatialFilterRect(*window)
    got = {f["tag"] for f in lyr2}

    assert got == expected, f"index disagrees with brute force for {window}"
    assert "nogeom" not in got


def test_ogr_geopbf_spatial_filter_reuse(indexed_layer):
    """Changing the filter without an explicit ResetReading must take effect."""

    filename, total = indexed_layer
    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)

    lyr.SetSpatialFilterRect(130.0, 30.0, 130.4, 30.4)
    first = sum(1 for _ in lyr)

    lyr.SetSpatialFilterRect(120.0, 20.0, 150.0, 50.0)
    second = sum(1 for _ in lyr)
    assert second > first
    assert second == total - 1   # everything except the geometry-less feature

    lyr.SetSpatialFilter(None)
    assert sum(1 for _ in lyr) == total


def test_ogr_geopbf_random_read(indexed_layer):

    filename, total = indexed_layer
    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)

    sequential = [(f.GetFID(), f["tag"], f.GetGeometryRef().ExportToWkt()
                   if f.GetGeometryRef() else None) for f in lyr]
    assert len(sequential) == total

    for fid, tag, wkt in [sequential[0], sequential[len(sequential) // 2], sequential[-1]]:
        f = lyr.GetFeature(fid)
        assert f is not None
        assert f["tag"] == tag
        assert (f.GetGeometryRef().ExportToWkt() if f.GetGeometryRef() else None) == wkt

    assert lyr.GetFeature(total + 100) is None


###############################################################################
# A GeoPBF file is gzipped by convention, so that is what the driver writes


def _is_gzip(path):
    with open(path, "rb") as f:
        return f.read(2) == b"\x1f\x8b"


def test_ogr_geopbf_write_gzip_by_default(tmp_path):

    filename = str(tmp_path / "default.geopbf")
    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(filename)
    lyr = ds.CreateLayer("d", geom_type=ogr.wkbPoint)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (139 35)"))
    lyr.CreateFeature(f)
    ds = None

    assert _is_gzip(filename), "the default output should be gzipped"
    ds = ogr.Open(filename)
    assert ds.GetLayer(0).GetFeatureCount() == 1


def test_ogr_geopbf_write_uncompressed(tmp_path):

    filename = str(tmp_path / "raw.geopbf")
    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(filename, options=["COMPRESS=NONE"])
    lyr = ds.CreateLayer("d", geom_type=ogr.wkbPoint)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (139 35)"))
    lyr.CreateFeature(f)
    ds = None

    assert not _is_gzip(filename)
    ds = ogr.Open(filename)
    assert ds.GetLayer(0).GetFeatureCount() == 1


def test_ogr_geopbf_write_compress_invalid(tmp_path):

    with gdal.quiet_errors():
        ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(
            str(tmp_path / "bad.geopbf"), options=["COMPRESS=LZ4"]
        )
    assert ds is None


###############################################################################
# PRECISION must survive a GeoPBF -> GeoPBF conversion. Silently dropping a
# digit turns 1.1 cm into 11 cm, which matters for cadastral data.


def _file_precision(path):
    import gzip as _gzip

    data = open(path, "rb").read()
    if data[:2] == b"\x1f\x8b":
        data = _gzip.decompress(data)

    def varint(i):
        v = s = 0
        while True:
            b = data[i]
            i += 1
            v |= (b & 0x7F) << s
            if not b & 0x80:
                return v, i
            s += 7

    i = 0
    while i < len(data):
        tag, i = varint(i)
        field, wire = tag >> 3, tag & 7
        if wire == 0:
            value, i = varint(i)
            if field == 3:
                return value
        elif wire == 2:
            ln, i = varint(i)
            if field == 5:      # FARRAY: header is over
                break
            i += ln
        elif wire == 1:
            i += 8
        else:
            break
    return None


def _make(path, precision=None):
    opts = [f"PRECISION={precision}"] if precision is not None else []
    ds = ogr.GetDriverByName("GeoPBF").CreateDataSource(path, options=opts)
    lyr = ds.CreateLayer("p", geom_type=ogr.wkbPoint)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (139.7660841 35.6813821)"))
    lyr.CreateFeature(f)
    ds = None


def test_ogr_geopbf_precision_is_recorded(tmp_path):

    p7 = str(tmp_path / "p7.geopbf")
    _make(p7, 7)
    assert _file_precision(p7) == 7

    ds = ogr.Open(p7)
    assert ds.GetLayer(0).GetMetadataItem("PRECISION") == "7"


def test_ogr_geopbf_precision_survives_conversion(tmp_path):
    """geopbf -> geopbf with no options must not quietly coarsen coordinates."""

    src = str(tmp_path / "src7.geopbf")
    dst = str(tmp_path / "dst.geopbf")
    _make(src, 7)

    gdal.VectorTranslate(dst, src, format="GeoPBF")
    assert _file_precision(dst) == 7, "PRECISION was lost in translation"

    ds = ogr.Open(dst)   # データセットを保持（先に解放されるとレイヤが無効になる）
    f = ds.GetLayer(0).GetNextFeature()
    g = f.GetGeometryRef()
    assert g.GetX(0) == pytest.approx(139.7660841, abs=1e-7)
    assert g.GetY(0) == pytest.approx(35.6813821, abs=1e-7)


def test_ogr_geopbf_precision_option_wins(tmp_path):
    """An explicit creation option must override the inherited value."""

    src = str(tmp_path / "src7.geopbf")
    dst = str(tmp_path / "dst3.geopbf")
    _make(src, 7)

    gdal.VectorTranslate(dst, src, format="GeoPBF", datasetCreationOptions=["PRECISION=3"])
    assert _file_precision(dst) == 3
