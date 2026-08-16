# acceptance_metadata.gdb Source Notes

## Generation Environment

| Item | Value |
|------|-------|
| Tool | ArcGIS Pro arcpy |
| ArcGIS Pro Version | 3.5.0.57366 |
| Generation Script | generate_acceptance_metadata.py |
| Generation Time | 2026-07-29T17:12:23.887208 |

## Redistribution

The repository owner has confirmed that this generated acceptance fixture may
be redistributed with the fast-gdb repository. The fixture contains generated
test data only; it does not include ArcGIS Pro binaries, ArcGIS API code, or
the ArcGIS Pro runtime. Paths recorded in the expected-value files and ArcGIS
lineage metadata are repository-relative or normalized placeholders so that
the fixture remains portable across machines.

## Feature Datasets

- TransportFD (EPSG:4326): roads, road_inspections, road_assets
- AdminFD (EPSG:3857): parcels, parcel_parts
- Root: root_points, root_table, all_field_types

## Domains

| Domain | Type | Values |
|--------|------|--------|
| road_status_domain | Coded Value (TEXT) | OPEN=Open, CLOSED=Closed, UNKNOWN=Unknown |
| speed_range_domain | Range (SHORT) | 0-120 |
| road_type_domain | Coded Value (SHORT) | 1=Highway, 2=Local Road, 3=Trail |
| inspection_result_domain | Coded Value (TEXT) | GOOD=Good, FAIR=Fair, POOR=Poor |

## Relationships

| Name | Type | Origin | Destination | Cardinality | Key |
|------|------|--------|-------------|-------------|-----|
| roads_inspections_rel | Simple | roads | road_inspections | 1:M | OBJECTID <-> road_id |
| parcels_parts_rel | Composite | parcels | parcel_parts | 1:M | OBJECTID <-> parcel_id |
| assets_guid_rel | Simple | roads | road_assets | 1:1 | GlobalID <-> asset_guid |

## Advanced Field Types

| Field | Type | Status |
|-------|------|--------|
| bigint_fld | BIGINTEGER | SUPPORTED |
| dateonly_fld | DATEONLY | SUPPORTED |
| timeonly_fld | TIMEONLY | SUPPORTED |
| timestampoffset_fld | TIMESTAMPOFFSET | SUPPORTED |

## Known Limitations

1. Attributed relationship classes: Not created (arcpy 3.5 limitation)
2. Raster: Not generated (not in scope)
3. Multipatch: Not generated (not in scope)

## Delivery Files

- dataset-hierarchy.csv
- domain-expected.csv
- fid-objectid-mapping.csv
- field-inventory.csv
- field-values-expected.csv
- layer-inventory.csv
- manifest.json
- metadata-expected.json
- relationship-expected.csv
- source-notes.md
