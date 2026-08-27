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
Names containing a dot express nesting in the reference implementation
(``owner.name``); they are passed through unchanged as ordinary field names.

Every value carries its own type on the wire, so field types are recovered on
read rather than guessed from text:

===================  ==========================  ======================================
GeoPBF value         OGR field                   Notes
===================  ==========================  ======================================
BOOL                 Integer, ``Boolean``
INTEGER              Integer64                   zig-zag varint
FLOAT                Real                        IEEE double
STRING               String
DATE                 DateTime                    Unix timestamp, exposed as UTC
JSON                 String, ``JSON``
BBOX                 RealList (4 values)
COLOR                String                      ``rgb(r,g,b)`` / ``rgba(r,g,b,a)``
FUNC                 String                      **never evaluated** — see below
BLOB, IMAGE          String                      reference into the pool — see below
===================  ==========================  ======================================

Types are determined by inspecting features until every field has been seen, or
until :oo:`TYPE_SCAN_FEATURES` features have been read. A field whose values
disagree between features falls back to ``String``.

.. warning::

    A ``FUNC`` value holds JavaScript source. The reference implementation turns
    it back into a callable; this driver never does, and exposes it as text.
    Treat it as untrusted input.

Binary payloads
---------------

Binary content (``BLOB`` for files, ``IMAGE`` for raw RGBA pixels) is not stored
in the feature. The bytes live in a single dataset-level pool and each feature
holds only a reference into it: ``name:mime:id`` for a blob, ``width:height:id``
for an image. One icon shared by a million points is therefore stored once.

That sharing has no equivalent in the OGR data model — a binary field would copy
the payload into every feature — so the driver keeps the two apart:

- the reference stays in the feature, as a ``String`` field;
- the pool is exposed as the dataset metadata domain ``BUFS``, one base64 item
  per buffer, keyed by the id the references use;
- the layer metadata domain ``GEOPBF`` records which fields are references, and
  whether each is a ``BLOB`` or an ``IMAGE``.

Because ``ogr2ogr`` copies dataset and layer metadata by default, a GeoPBF to
GeoPBF conversion keeps the payloads, the references and the sharing intact. Use
``-nomd`` and the binary content is dropped.

Reaching the bytes from a feature therefore takes one indirection: read the id
out of the reference, then decode the matching item of the ``BUFS`` domain.
Exposing them directly as ``Binary`` fields would duplicate shared payloads once
per feature, and an ``IMAGE`` is raw RGBA rather than an encoded picture, so no
tool would display it without re-encoding; that is left out deliberately.

Field order
-----------

Within a feature, the ``VALUE`` messages are written before the ``INDEX`` that
binds them to field names. The reference implementation collects values as it
walks the feature and binds them when it reaches the index, so an index written
first yields empty attributes there — even though a reader that gathers both
before pairing them, including this driver, sees nothing wrong.

Compression
-----------

A GeoPBF file is gzip-compressed as a whole by convention, and files written by
this driver are gzipped unless :dsco:`COMPRESS=NONE` is given. Compressed and
uncompressed files share the ``.geopbf`` extension and are told apart by their
content, so both are read transparently.

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

The driver advertises ``DCAP_HONOR_GEOM_COORDINATE_PRECISION``, and the layer
advertises :cpp:enumerator:`OLCFastFeatureCount`,
:cpp:enumerator:`OLCFastGetExtent`, :cpp:enumerator:`OLCFastSpatialFilter` and
:cpp:enumerator:`OLCRandomRead`. Feature identifiers are the zero-based position
of the feature in the file.

Open options
------------

|about-open-options|

-  .. oo:: TYPE_SCAN_FEATURES
      :choices: <int>
      :default: 1000

      How many features to inspect when determining field types. The scan stops
      as soon as every field has been seen at least once, so this only matters
      for files where some attribute appears late.

Dataset creation options
------------------------

|about-dataset-creation-options|

The following dataset creation options are available:

-  .. dsco:: COMPRESS
      :choices: GZIP, NONE
      :default: GZIP

      Whether to gzip the whole file on creation. **A GeoPBF file is gzipped by
      convention** — that is the form the format is published and distributed
      in — so this is the default. Both forms carry the ``.geopbf`` extension
      and are read transparently. Use ``NONE`` only for a consumer that cannot
      inflate (549 730 parcels: 29 MB gzipped, 147 MB uncompressed).

-  .. dsco:: PRECISION
      :choices: 0-9
      :default: 6

      Number of decimal digits of longitude and latitude preserved in the
      file. Coordinates are stored as ``round(degrees * 10^PRECISION)``: 6 digits
      is about 11 cm, 7 is about 1.1 cm. Lowering it produces smaller files at a
      coarser resolution.

      When not given, the value is taken from the coordinate resolution declared
      by the source layer, through the standard OGR mechanism
      (:cpp:class:`OGRGeomCoordinatePrecision`). This driver publishes the file's
      quantisation as that resolution on read and honours it on write, so a
      conversion never quietly coarsens coordinates — including through an
      intermediate format that carries the resolution, such as GeoPackage.
      ``ogr2ogr -xyRes`` overrides the inherited value, and an explicit
      :dsco:`PRECISION` creation option overrides everything.

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
