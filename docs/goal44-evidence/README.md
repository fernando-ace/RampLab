# Goal 44 KAUO topology evidence

This bundle contains source geometry, normalized pavement polygons, generated Goal 44 control and disruption scenarios, route diagnostics, simulator traces and metrics, and the report. `SHA256SUMS.txt` verifies each payload file. `bundle-archive-sha256.txt` records the ZIP hash.

The route interiors are deterministic paths through ALDOT pavement inventory polygons. The inventory does not define approved taxiway centerlines or runway-crossing permissions. Of 54 assumed or sourced graph nodes, 46 are snapped farther than the 2 m raster resolution; those locations are explicitly unverified approximations. Static connectivity is not proof of operational route authorization.

The latest regenerated control and disruption runs have final metrics, but both are negative: each completed 0/3 turnarounds and recorded fleet collisions and unresolved deadlocks. Three control replays are byte-identical. These artifacts do not demonstrate successful or collision-free operations.
