# Återanvändning från Elprisdisplay v0.9

Utgångspunkt: `elpriser_wifi_optimerad_telemetri.ino`, den senast verifierade
versionen från 20 oktober 2026.

## Direkt återanvänd logik

- `connectWiFi()`:
  - sparad kanal/BSSID i RTC-minne
  - snabb återanslutning först
  - vanlig scanning som fallback

- RTC:
  - PCF85063 på 0x51
  - RTC lagrar UTC
  - systemklockan konverteras därefter till Europe/Stockholm

- `syncClock()`:
  - SNTP callback
  - verklig synkronisering inväntas
  - RTC skrivs först efter lyckad NTP-sync

- `sleepUntilNextQuarter()`:
  - nästa 900-sekundersgräns
  - +2 sekunder
  - Wi-Fi av
  - display hibernate
  - timer wake + GPIO5 wake

- wake-logik:
  - normal timer wake använder `initDisplayHardware(false)`
  - kallstart/reset använder `initDisplayHardware(true)`
  - function button ger emergency stay-awake

- telemetri:
  - AXP2101
  - SHTC3
  - MQTT Discovery
  - telemetri får aldrig starta Wi-Fi själv

## Motsvarighet till gamla 13:15-logiken

Från 13:15:
1. försök hämta morgondagens publicerade priser,
2. om de inte finns: försök nästa kvart,
3. när alla 96 kvartsvärden finns:
   - de sparas i RTC-cache,
   - grafen byggs om,
   - NTP körs medan Wi-Fi ändå är aktivt,
   - RTC korrigeras.

Skillnaden är bara datakällan:
- v0.9: `sensor.nordpool` via Home Assistant
- 2.0: Elpriset just nu direkt över HTTPS

## Motsvarighet till gamla midnattslogiken

Vid första wake efter nytt datum:
- `actualTomorrow` blir `actualToday`,
- ny `actualTomorrow` töms,
- prognoscachen flyttas ett dygn,
- grafens kalenderdatum byggs om,
- Wolta-prognosen uppdateras.

Det nya 72h-fönstret är alltid:
- idag
- imorgon
- dagen efter imorgon

Det gamla 38h-fönstrets `graphStartHour` behövs därför inte längre.

## Partial refresh

- kvart: toppraden + statistik + aktuellt pris
- hel timme: grafpanelen, så att NU-markören flyttar
- nya data / datumbyte: grafpanelen
- kallstart: full refresh

Detta är medvetet större partial-regioner än i v0.9, men fortfarande inte en
full refresh var 15:e minut.
