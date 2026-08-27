# gdal-geopbf

GDAL/OGR driver for the [GeoPBF](https://github.com/kenjiyoshidahome2026-bit/geopbf) vector format.

GeoPBF is a compact binary vector format based on Protocol Buffers with delta-encoded, integer-scaled coordinates.

## Features

- Read **and write** `.geopbf` via any GDAL-based tool (`ogrinfo`, `ogr2ogr`, QGIS, PostGIS, ...)
- **Gzipped `.geopbf` is read transparently** (memory use follows the decompressed size) — files exported from web tooling are gzip-compressed; the driver detects the gzip signature and reads them through GDAL's `/vsigzip/` (no zlib dependency added)
- **Content-based `Identify()`** — files are recognised by their header signature, not only by the `.geopbf` extension
- Zero external dependencies — self-contained Protobuf reader and writer
- Typed attributes: integers, reals, booleans, dates, JSON and bounding boxes come
  back as their own OGR types rather than as text
- Binary payloads (icons, blobs) survive a round trip: the shared pool travels as
  dataset metadata, features keep their references into it
- Coordinate precision is carried through conversions (the file's quantisation is
  published as the layer's coordinate resolution), so a round trip never silently
  coarsens coordinates
- In-memory spatial index built on demand: window queries stay fast on large
  layers (500 k points: 1.2 ms for a 460-feature window, versus 276 ms without it)
- Supports all GeoPBF geometry types: Point, MultiPoint, LineString, MultiLineString, Polygon, MultiPolygon, GeometryCollection
- WGS84 (EPSG:4326) spatial reference

## Requirements

- GDAL ≥ 3.12 (tested against 3.12.4 and 3.13.3)
- CMake ≥ 3.16
- C++17 compiler

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Install

```bash
cmake --install build
```

Installs to the GDAL plugin directory (`gdal-config --plugindir`); override it with
`-DGDAL_PLUGIN_DIR=/path/to/gdalplugins`. After install, GDAL loads the driver
automatically — no environment variables needed. The built module is `ogr_GeoPBF`,
following GDAL's naming convention for vector plugins.

To try it without installing:

```bash
GDAL_DRIVER_PATH=$PWD/build ogrinfo --formats | grep GeoPBF
```

## Usage

```bash
ogrinfo sample.geopbf
ogr2ogr -f GeoJSON out.geojson sample.geopbf
ogr2ogr -f GPKG out.gpkg sample.geopbf

# write
ogr2ogr -f GeoPBF out.geopbf input.gpkg
ogr2ogr -f GeoPBF -dsco PRECISION=7 out.geopbf input.geojson
ogr2ogr -f GeoPBF -dsco COMPRESS=NONE out.geopbf input.gpkg    # 既定は gzip 済み。無圧縮にしたい時だけ
```

## Without installing (development)

```bash
GDAL_DRIVER_PATH=./build ogrinfo sample.geopbf
```

## License

MIT

## Writing

`PRECISION` (dataset creation option, default `6`) sets how many decimal digits of
longitude/latitude survive: coordinates are stored as `round(degrees × 10^PRECISION)`.
6 digits is about 0.1 m.

Notes:

- One layer per file — GeoPBF has no container for multiple layers.
- 2D only; Z values are dropped with a warning.
- WGS84 (EPSG:4326) only; reproject with `-t_srs EPSG:4326` if the source differs.
- Curve geometries are linearised. Values are written typed (integer, real, bool, date,
  JSON for list fields); the reader currently exposes every attribute as a string.

## Tests

`autotest/ogr/ogr_geopbf.py` follows GDAL's autotest layout and runs standalone
against the built plugin:

```bash
GDAL_DRIVER_PATH=$PWD/build pytest autotest/ogr/ogr_geopbf.py
```

`autotest/ogr/data/geopbf/point.geopbf` is written by the reference JavaScript
implementation, so the suite also covers cross-implementation compatibility.

## Packaging

`conda/` holds a conda-forge recipe in the v1 (`recipe.yaml`) format, ready to be
copied into `conda-forge/staged-recipes` as `recipes/libgdal-geopbf/`. It needs a
tagged release and its sha256 filled in first; the header of the recipe lists the
steps. Upstreaming into GDAL itself is the preferred route — the recipe exists as
a fallback so the driver can reach users either way.

## Using it with QGIS

QGIS ships its own copy of GDAL, so the plugin has to be built against *that* GDAL,
not the one on your `PATH`. On macOS there are two separate obstacles worth knowing
about, because neither produces an obvious error message.

**1. ABI.** `QGIS.app` bundles its own `libgdal` (QGIS 4.2 → GDAL 3.12). A plugin
linked against a different GDAL will not load, or will pull a second copy of GDAL
into the process. Build against the matching headers and let the symbols resolve
at load time instead of linking:

```bash
# headers for the GDAL version QGIS bundles — check with
#   /Applications/QGIS*.app/Contents/MacOS/ogrinfo --version
curl -sLO https://github.com/OSGeo/gdal/releases/download/v3.12.4/gdal-3.12.4.tar.gz
tar xzf gdal-3.12.4.tar.gz
sed -e 's/@GDAL_VERSION_MAJOR@/3/' -e 's/@GDAL_VERSION_MINOR@/12/' \
    -e 's/@GDAL_VERSION_REV@/4/'   -e 's/@GDAL_VERSION_BUILD@/0/' \
    -e 's/@GDAL_RELEASE_DATE@/20260422/' -e 's/@GDAL_RELEASE_NAME@/3.12.4/' \
    gdal-3.12.4/gcore/gdal_version.h.in > gen/gdal_version.h
cp $(dirname $(command -v gdalinfo))/../include/cpl_config.h gen/   # platform defines

clang++ -std=c++17 -O2 -dynamiclib -undefined dynamic_lookup \
  -Igen -Igdal-3.12.4/port -Igdal-3.12.4/gcore -Igdal-3.12.4/ogr \
  -Igdal-3.12.4/ogr/ogrsf_frmts -Igdal-3.12.4/alg \
  -o ~/gdalplugins/ogr_GeoPBF.so ogrgeopbf.cpp ogrgeopbfwrite.cpp
```

**2. Code signing.** `QGIS.app` carries the
`com.apple.security.cs.disable-library-validation` entitlement, so it *will* load a
third-party plugin. The command line tools bundled beside it (`ogrinfo`,
`ogr2ogr`, `qgis_process`) do **not** carry that entitlement, and refuse with
`different Team IDs`. Test through the GUI, or through a re-signed copy of the tool:

```bash
cp /Applications/QGIS*.app/Contents/MacOS/ogrinfo /tmp/ogrinfo
codesign --force -s - --entitlements ent.plist /tmp/ogrinfo   # both cs.* entitlements
DYLD_LIBRARY_PATH=/Applications/QGIS*.app/Contents/Frameworks \
GDAL_DRIVER_PATH=~/gdalplugins /tmp/ogrinfo --formats | grep GeoPBF
```

**Pointing QGIS at the plugin.** Settings → Options → System → Environment: add
`GDAL_DRIVER_PATH` = the directory holding `ogr_GeoPBF.so`, then restart QGIS.
`.geopbf` files can then be opened by drag and drop like any other vector format.
