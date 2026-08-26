.. _vector.geopbf:

GeoPBF -- Compact Protocol Buffers vector format
================================================

.. shortname:: GeoPBF

.. build_dependencies:: (none)

This driver implements read and write support for GeoPBF, a compact binary
vector format built on Protocol Buffers. Coordinates are stored as
delta-encoded integers scaled by a fixed power of ten, and attribute names are
held once in a file-level dictionary, which makes a GeoPBF file roughly one
tenth the size of the equivalent GeoJSON.

The format is used by the `Ortho Earth <https://www.ortho-earth.com/>`__ engine
to serve vector data from static object storage without a tile server. The
`format specification
<https://github.com/kenjiyoshidahome2026-bit/geopbf/blob/main/pbf%20spec.md>`__
and a `JavaScript reference implementation
<https://www.npmjs.com/package/geopbf>`__ are published under the MIT licence.

The driver carries its own minimal Protocol Buffers reader and writer, so it
adds no external dependency.

Driver capabilities
-------------------

.. supports_create::

.. supports_georeferencing::

.. supports_virtualio::

Dataset and layer model
-----------------------

A GeoPBF file holds exactly one layer. The layer name is taken from the
``NAME`` field of the file header, and falls back to ``layer`` when that field
is absent. Attempting to create a second layer in the same dataset fails.

Coordinates are always WGS 84 (EPSG:4326) longitude/latitude. On creation, a
source CRS that differs from WGS 84 raises a warning and coordinates are
written unchanged; use ``-t_srs EPSG:4326`` to reproject.

The format is two-dimensional. Z values are dropped on write, with a warning
issued once per layer.

Geometry types
--------------

All Simple Features geometry types are supported: Point, MultiPoint,
LineString, MultiLineString, Polygon, MultiPolygon and GeometryCollection.
Curve geometries are linearised on write.

Attributes
----------

Attribute names form a dictionary in the file header; each feature stores the
indices of the fields it sets, so sparsely populated attributes cost nothing.

On write, values are stored with their type: ``Integer`` and ``Integer64`` as
zig-zag varints, ``Real`` as a double, ``Integer`` of subtype ``Boolean`` as a
boolean, ``Date`` and ``DateTime`` as a Unix timestamp, and list fields as
JSON. All other types are written as strings.

On read, every attribute is currently exposed as a field of type ``String``.

Compression
-----------

A GeoPBF file may be gzip-compressed as a whole; such files carry the same
``.geopbf`` extension and are decompressed transparently on read.

Opening
-------

Files are recognised by their content, so the ``.geopbf`` extension is not
required. Files compressed with gzip are recognised by extension, since their
header cannot be inspected without decompressing them.

Performance
-----------

The format carries no spatial index — that is the price of its small size. The
driver compensates in memory: the first time a layer is asked for its feature
count, its extent, a feature by identifier, or a spatially filtered iteration,
it scans the file once and records each feature's byte range and bounding box,
then buckets those into a uniform grid. Subsequent window queries touch only the
candidate features and never build geometries that fall outside the filter.

The index is built lazily, so a plain full-table conversion never pays for it.

Measured on 500,000 points (a 32 MB file; the same data is 60 MB as GeoPackage
and 78 MB as FlatGeobuf):

==================================  ==========
Operation                           Time
==================================  ==========
Open                                    23 ms
First call that builds the index        60 ms
``GetExtent`` (cached afterwards)      < 1 ms
Window query returning 460 features    1.2 ms
Full scan of all features            1 010 ms
==================================  ==========

The layer advertises :cpp:enumerator:`OLCFastFeatureCount`,
:cpp:enumerator:`OLCFastGetExtent`, :cpp:enumerator:`OLCFastSpatialFilter` and
:cpp:enumerator:`OLCRandomRead`. Feature identifiers are the zero-based position
of the feature in the file.

Dataset creation options
------------------------

|about-dataset-creation-options|

The following dataset creation options are available:

-  .. dsco:: COMPRESS
      :choices: NONE, GZIP
      :default: NONE

      Whether to gzip the whole file on creation. Files distributed by the
      reference tooling are usually gzipped, and the driver reads either form
      transparently, so ``GZIP`` produces output symmetric with those files
      (549 730 parcels: 147 MB uncompressed, 29 MB gzipped).

-  .. dsco:: PRECISION
      :choices: 0-9
      :default: 6

      Number of decimal digits of longitude and latitude preserved in the
      file. Coordinates are stored as ``round(degrees * 10^PRECISION)``, so the
      default of 6 keeps about 0.1 m. Lowering it produces smaller files at a
      coarser resolution.

Examples
--------

-  Inspect a file:

   ::

      ogrinfo -al sample.geopbf

-  Convert to GeoJSON:

   ::

      ogr2ogr -f GeoJSON out.geojson sample.geopbf

-  Convert a GeoPackage to GeoPBF, keeping 7 decimal digits:

   ::

      ogr2ogr -f GeoPBF -dsco PRECISION=7 out.geopbf in.gpkg

-  Write a gzipped file, as the reference tooling distributes them:

   ::

      ogr2ogr -f GeoPBF -dsco COMPRESS=GZIP out.geopbf in.gpkg

-  Read a gzip-compressed file over HTTP:

   ::

      ogrinfo -al /vsicurl/https://example.com/data/sample.geopbf

Limitations
-----------

-  One layer per file.
-  Two dimensions only; Z and M values are not stored.
-  WGS 84 only.
-  No update mode: existing files can be read or overwritten, not edited.
-  The whole file is read into memory when opened — for a gzip-compressed file
   that means the *decompressed* size, which can be several times the size on
   disk. Written features are
   held in memory until the dataset is closed, because the attribute-name
   dictionary is only complete once every feature has been seen. The optional
   in-memory index adds about 32 bytes per feature on top of that.

See Also
--------

-  `GeoPBF format specification
   <https://github.com/kenjiyoshidahome2026-bit/geopbf/blob/main/pbf%20spec.md>`__
-  `geopbf JavaScript implementation <https://www.npmjs.com/package/geopbf>`__
