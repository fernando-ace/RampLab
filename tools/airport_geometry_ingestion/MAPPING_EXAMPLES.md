# Airport geometry mapping examples

Mappings make field selection and coordinate interpretation explicit. They are JSON. Do not map ambiguous columns by name similarity.

## GeoJSON FeatureCollection (WGS 84)

Input properties include `feature_code` and `surface_kind`; geometry coordinates follow RFC 7946 `[longitude, latitude, altitude?]` order.

```json
{
  "dataset_id": "operator-airfield-cycle-2026-10",
  "airport": {"airport_id": "ABCD", "name": "Example only"},
  "source": {
    "source_id": "operator_gis_2026_10",
    "source_type": "operator GIS export",
    "source_url": null,
    "publication_date": "2026-10-01",
    "extraction_method": "direct GIS export",
    "authority": "OPERATOR_PROVIDED",
    "notes": "Replace with source custodian's documented details."
  },
  "source_crs": "EPSG:4326",
  "source_units": "degrees",
  "origin": {
    "latitude_degrees": 32.0,
    "longitude_degrees": -85.0,
    "ellipsoid_height_m": 200.0
  },
  "field_mapping": {
    "id": "feature_code",
    "feature_type": "surface_kind"
  }
}
```

The example origin is illustrative and must be replaced with the source airport reference point. Supported feature-type values are listed in [`SCHEMA.md`](SCHEMA.md). A feature-level authority column can be mapped with `"authority": "authority_class"`; otherwise the source-level authority applies.

When a source uses a documented class vocabulary, `field_mapping.feature_type_map` may explicitly map source values to canonical feature types. Goal47 uses this for ALDOT `USE_` values (`RUNWAY`, `TAXIWAY`, `APRON`, and unclassified surface sections); no routing centerline or access permission is inferred from a polygon class.

## CSV edge rows (local simulator grid)

One row represents one supplied straight edge. All column names are explicit. Local X/Y are in feet; positive X has true bearing 90 degrees, with the origin shifted by the stated ENU offsets.

```json
{
  "airport": {"airport_id": "ABCD"},
  "source": {
    "source_id": "survey_edges_2026",
    "source_type": "surveyed CSV point/edge export",
    "publication_date": "2026-10-01",
    "extraction_method": "survey point-edge export",
    "authority": "SURVEYED"
  },
  "source_crs": "LOCAL_SIMULATOR",
  "source_units": "ft",
  "local_transform": {
    "positive_x_true_bearing_degrees": 90,
    "origin_offset_east_m": 0,
    "origin_offset_north_m": 0
  },
  "field_mapping": {
    "id": "link_id",
    "feature_type": "feature_type",
    "coordinate_fields": {
      "edge": {
        "start_x": "from_x_ft",
        "start_y": "from_y_ft",
        "end_x": "to_x_ft",
        "end_y": "to_y_ft"
      }
    }
  }
}
```

CSV rows should include source attributes such as `directed`, `permissions` (JSON array), `width_m`, and clearance fields when available. Missing permission or direction values block routing; the importer does not assume them.

## CSV with explicit geometry JSON

For multiple coordinate vertices, map a JSON array column and a geometry-type column:

```json
{
  "airport": {"airport_id": "ABCD"},
  "source": {
    "source_id": "cad-conversion-2026",
    "source_type": "documented CAD conversion",
    "extraction_method": "operator supplied conversion",
    "authority": "OPERATOR_PROVIDED"
  },
  "source_crs": "LOCAL_ENU",
  "source_units": "m",
  "field_mapping": {
    "id": "feature_id",
    "feature_type": "feature_type",
    "geometry_type": "geometry_type",
    "coordinates": "coordinates_json"
  }
}
```

## KML/KMZ

KML/KMZ uses longitude/latitude coordinates. Identifier and type are read from explicit Placemark `name` or ExtendedData field mappings. Map, for example, `"id": "name"` and `"feature_type": "kind"`. Altitude is preserved in source metadata but not normalized vertically unless a supported vertical datum workflow is added.
