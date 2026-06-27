# gdal-geopbf

GDAL/OGR driver for the [GeoPBF](https://github.com/kenjiyoshidahome2026-bit/geopbf) vector format.

GeoPBF is a compact binary vector format based on Protocol Buffers with delta-encoded, integer-scaled coordinates.

## Features

- Read `.geopbf` files via any GDAL-based tool (`ogrinfo`, `ogr2ogr`, QGIS, PostGIS, ...)
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
```

## Without installing (development)

```bash
GDAL_DRIVER_PATH=./build ogrinfo sample.geopbf
```

## License

MIT
