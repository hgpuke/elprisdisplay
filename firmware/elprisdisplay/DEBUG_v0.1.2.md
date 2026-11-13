# v0.1.2 DEBUG

Temporary integration-diagnostic build.

Changes:
- cold boot/reset stays awake after first render (`INTEGRATION_DEBUG_HOLD = true`)
- failed price fetch never goes directly to deep sleep
- 8 second cold-start delay so Serial Monitor can attach
- detailed HTTP status/content-length/heap logging
- logs accepted/valid quarter-point counts for actual and forecast sources
- logs actual/forecast/missing counts for all 72 graph hours
- displays `INGA PRISDATA` in the graph if no hourly bars were built

This version is intentionally not the final power-saving build. Once price
fetching is verified, set `INTEGRATION_DEBUG_HOLD = false` and restore normal
cold-start deep sleep.
