# Visual assets and data sources

RampLab's operational meshes are original, code-generated combinations of Unreal Engine basic shapes. No purchased Fab/Marketplace art assets are used.

| Asset or data | Source | License / terms | Use |
|---|---|---|---|
| Cesium for Unreal 2.29.1 | [Official CesiumGS release](https://github.com/CesiumGS/cesium-unreal/releases/tag/v2.29.1) | Apache-2.0 | WGS84 georeference and runtime geospatial streaming. The project-local plugin is installed by `InstallCesium.ps1` and ignored by Git. |
| Cesium World Terrain, asset 1 | [Cesium ion Asset Depot](https://cesium.com/platform/cesium-ion/content/) | Cesium ion asset terms and source-specific attribution | Real geographic terrain around KAUO. Runtime internet access and an authorized ion token are required. |
| Bing Maps Aerial, asset 2 | [Cesium ion Asset Depot](https://cesium.com/platform/cesium-ion/content/) | Cesium ion/Bing terms and on-screen attribution | Real-world aerial context draped on Cesium World Terrain. |
| KAUO airport reference point, field elevation, and runway dimensions | [FAA Chart Supplement](https://www.faa.gov/air_traffic/flight_info/aeronav/digital_products/dafd/) and [FAA Digital Terminal Procedures](https://www.faa.gov/air_traffic/flight_info/aeronav/digital_products/dtpp/) | United States Government source | Geographic anchor and the published dimensions/orientation of Runway 18/36. |
| GEOID18 geoid height | [NOAA NGS Geoid Height Service](https://www.ngs.noaa.gov/web_services/geoid.shtml) | United States Government source | Converts the published field elevation to an approximate ellipsoid height appropriate for the Cesium origin. |
| Airport operational overlay | RampLab project | Original project work | Synthetic ramp, taxiway connector, depot, terminal/hangar massing, three stands, and service roads for the fictional scenario. |
| Aircraft, fuel trucks, and baggage tugs/carts | RampLab project | Original project work | Low-poly silhouettes assembled at runtime from Unreal Engine basic-shape meshes. |

Cesium tileset and imagery credit display is explicitly enabled in code. Do not hide or remove it. Dataset attributions shown by Cesium remain authoritative for streamed content.
