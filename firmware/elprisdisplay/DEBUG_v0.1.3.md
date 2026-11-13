# v0.1.3 DEBUG

Fixes based on the first complete serial log.

## Elpriset just nu
The endpoint returned HTTP 200 but `deserializeJson(http.getStream())`
failed with `InvalidInput`, while the response used unknown/chunked length.

v0.1.3:
- requests `Accept-Encoding: identity`
- reads the complete HTTP body into a `String`
- strips an optional UTF-8 BOM
- validates the first non-whitespace character
- parses ArduinoJson from the completed payload
- logs payload length and a prefix on parse failure

## Wolta
The API response itself was valid. Its `days[]` objects explicitly carry
the Swedish local forecast date (`date`) and the points use UTC timestamps.

v0.1.3:
- maps each `days[]` object directly by its `date` field
- uses `ts_utc` only to calculate the local quarter index
- logs the first UTC->local conversion
- logs accepted/rejected point counters

## Debug mode
`INTEGRATION_DEBUG_HOLD` remains true. Deep sleep is intentionally disabled
after cold boot/reset until the price path is verified.
