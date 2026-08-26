# gdal-geopbf

GDAL/OGR driver for the [GeoPBF](https://github.com/kenjiyoshidahome2026-bit/geopbf) vector format.

GeoPBF is a compact binary vector format based on Protocol Buffers with delta-encoded, integer-scaled coordinates.

## Features

- Read **and write** `.geopbf` via any GDAL-based tool (`ogrinfo`, `ogr2ogr`, QGIS, PostGIS, ...)
- **Gzipped `.geopbf` is read transparently** — files exported from web tooling are gzip-compressed; the driver detects the gzip signature and reads them through GDAL's `/vsigzip/` (no zlib dependency added)
- **Content-based `Identify()`** — files are recognised by their header signature, not only by the `.geopbf` extension
- Zero external dependencies — self-contained Protobuf reader included
- Supports all GeoPBF geometry types: Point, MultiPoint, LineString, MultiLineString, Polygon, MultiPolygon, GeometryCollection
- WGS84 (EPSG:4326) spatial reference

## Requirements

- GDAL ≥ 3.x
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

Installs to the GDAL plugin directory (`gdal-config --plugindir`). After install, GDAL loads the driver automatically — no environment variables needed.

## Usage

```bash
ogrinfo sample.geopbf
ogr2ogr -f GeoJSON out.geojson sample.geopbf
ogr2ogr -f GPKG out.gpkg sample.geopbf

# write
ogr2ogr -f GeoPBF out.geopbf input.gpkg
ogr2ogr -f GeoPBF -dsco PRECISION=7 out.geopbf input.geojson
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
