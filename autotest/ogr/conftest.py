# 単体で走らせるための最小 conftest（in-tree では GDAL の autotest/conftest.py が同じ印を提供する）
import pytest
from osgeo import ogr

# 失敗は戻り値 None で表す流儀に固定（in-tree の GDAL autotest は例外を使う）
ogr.DontUseExceptions()


def pytest_configure(config):
    config.addinivalue_line("markers", "require_driver(name): skip if the OGR driver is absent")


def pytest_runtest_setup(item):
    for mark in item.iter_markers(name="require_driver"):
        if ogr.GetDriverByName(mark.args[0]) is None:
            pytest.skip(f"OGR driver {mark.args[0]} not available")
