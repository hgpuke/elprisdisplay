# Elprisdisplay 2.0 v0.1

Detta är den första flashbara 2.0-basen. Den bygger på den senast verifierade
`elpriser_wifi_optimerad_telemetri.ino`, men byter ut Home Assistant-prisdata
och den gamla 38-timmars/väderlayouten.

## Behållna funktioner från v0.9

- PCF85063 RTC lagrar UTC.
- Systemklockan laddas från RTC vid varje wake.
- Deep sleep till nästa kvart + 2 sekunder.
- Funktionsknappen väcker och aktiverar stay-awake-läge.
- Wi-Fi återanvänder senast kända kanal/BSSID.
- NTP korrigerar RTC när nya morgondagspriser faktiskt har kommit.
- MQTT-telemetri startar aldrig Wi-Fi på egen hand.
- Partial refresh vid timer-wakes.
- AXP2101-batteriprocent visas på displayen.

## Ny prisarkitektur

- Publicerade priser: Elpriset just nu.
- Prognos: Wolta.
- Intern cache behåller kvartsvärden.
- Grafen visar 72 timmedelvärden.
- Publicerat pris har alltid företräde framför prognos för samma timme.
- Från 13:15 provas morgondagens publicerade priser varje kvart tills hela
  morgondagen finns.
- Vid dygnsskifte flyttas morgondagens publicerade cache till idag.
- Wolta-cache flyttas ett dygn vid midnatt och uppdateras över Wi-Fi.
- Kallstart hämtar idag, försöker imorgon och hämtar prognos.

## Refresh

- Varje kvart: toppraden/statistik/NU-rutan partial refresh.
- Varje hel timme: grafpanelen partial refresh för NU-markören.
- Nya pris/prognosdata eller dygnsskifte: grafpanelen partial refresh.
- Kallstart/reset: full refresh.

## Barlow

Fontfiler följer inte med projektet. Generera de bitmapfonter som sketchen
använder från den öppna Google Fonts-källan:

```bash
cd elprisdisplay_2_0_v0_1
python3 tools/prepare_barlow_fonts.py
```

Det skapar `BarlowFonts.h`.

Om Pillow saknas:

```bash
python3 -m pip install --user pillow
```

Användning:
- Barlow SemiBold/Bold för header, statistik, dagar och Y-axel.
- Barlow Condensed SemiBold endast för den täta timaxeln.

## Konfiguration

```bash
cp secrets.h.example secrets.h
```

Redigera `secrets.h`.

`MQTT_ENABLED = false` gör displayen helt oberoende av MQTT/Home Assistant.

## Bibliotek

- GxEPD2
- ArduinoJson
- PubSubClient (endast för valfri telemetri)
- ESP32 Arduino core

## Känd begränsning

Precis som v0.9 använder kvartscachen 96 lokala kvartpositioner per dygn.
Sommar-/vintertidsdygn med 23/25 timmar är därför ännu inte fullständigt
representerade i grafen.
