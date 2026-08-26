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
