#!/usr/bin/env python3
"""Probe OpenFileGDB spatial-index lifecycle in an isolated temporary GDB."""

import json
import tempfile
from pathlib import Path

from osgeo import gdal, ogr, osr


def execute_sql(dataset, sql):
    gdal.ErrorReset()
    result = dataset.ExecuteSQL(sql)
    if result is not None:
        dataset.ReleaseResultSet(result)
    return {
        "error_type": gdal.GetLastErrorType(),
        "error": gdal.GetLastErrorMsg(),
    }


def main():
    gdal.DontUseExceptions()
    report = {"gdal": gdal.VersionInfo("--version")}
    driver = ogr.GetDriverByName("OpenFileGDB")
    if driver is None:
        raise RuntimeError("OpenFileGDB driver is unavailable")

    with tempfile.TemporaryDirectory(prefix="fast-gdb-sidx-") as temp_dir:
        gdb_path = Path(temp_dir) / "probe.gdb"
        dataset = driver.CreateDataSource(str(gdb_path))
        if dataset is None:
            raise RuntimeError("OpenFileGDB could not create a temporary GDB")

        srs = osr.SpatialReference()
        srs.ImportFromEPSG(4326)
        layer = dataset.CreateLayer("points", srs, ogr.wkbPoint)
        if layer is None:
            raise RuntimeError("OpenFileGDB could not create the point layer")
        if layer.CreateField(ogr.FieldDefn("value", ogr.OFTInteger)) != ogr.OGRERR_NONE:
            raise RuntimeError("OpenFileGDB could not create the value field")

        for value in range(10):
            feature = ogr.Feature(layer.GetLayerDefn())
            feature.SetField("value", value)
            point = ogr.Geometry(ogr.wkbPoint)
            point.AddPoint(value, value)
            feature.SetGeometry(point)
            if layer.CreateFeature(feature) != ogr.OGRERR_NONE:
                raise RuntimeError(f"CreateFeature failed at value {value}")
        feature = None
        layer = None
        dataset = None

        spx_files = list(gdb_path.glob("*.spx"))
        report["created_spx_files"] = [path.name for path in spx_files]
        if len(spx_files) != 1:
            raise RuntimeError(f"expected one generated SPX file, got {spx_files}")
        spx_path = spx_files[0]

        dataset = ogr.Open(str(gdb_path), 1)
        if dataset is None:
            raise RuntimeError("could not reopen temporary GDB for update")
        for label, sql in (
            ("create_spatial_index_sql", "CREATE SPATIAL INDEX ON points"),
            ("drop_spatial_index_sql", "DROP SPATIAL INDEX ON points"),
        ):
            result = execute_sql(dataset, sql)
            result["spx_exists_after"] = spx_path.exists()
            report[label] = result
        dataset = None

        spx_path.unlink()
        dataset = ogr.Open(str(gdb_path), 1)
        result = execute_sql(dataset, "REPACK points")
        dataset = None
        report["repack"] = result
        report["spx_exists_after_repack"] = spx_path.exists()

        dataset = ogr.Open(str(gdb_path), 1)
        layer = dataset.GetLayerByName("points")
        feature = layer.GetFeature(1)
        if feature is None:
            raise RuntimeError("could not read FID 1 before SetFeature")
        feature.SetField("value", 99)
        report["setfeature_status"] = layer.SetFeature(feature)
        feature = None
        layer = None
        dataset = None
        report["spx_exists_after_setfeature"] = spx_path.exists()

        dataset = ogr.Open(str(gdb_path), 0)
        layer = dataset.GetLayerByName("points")
        feature = layer.GetFeature(1)
        report["updated_value"] = feature.GetFieldAsInteger("value")
        report["feature_count"] = layer.GetFeatureCount()
        feature = None
        layer = None
        dataset = None

    print(json.dumps(report, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    main()
