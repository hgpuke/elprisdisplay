# v0.1.1 compile fixes

- Replaced unavailable `timegm()` with a timezone-independent UTC epoch converter.
- Moved `YScale` to the global type section so Arduino auto-prototype generation can see it.
- No scheduler, deep-sleep, refresh, price-source, or rendering behavior changed.
