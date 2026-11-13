# Elprisdisplay 2.0

En fristående e-paper-display för svenska elpriser, byggd med **ESP32-S3** med integrerad **3,97-tums 800×480 e-paper-panel**.

Displayen visar verkliga och prognostiserade elpriser över 72 timmar, uppdateras automatiskt varje kvart och är konstruerad för låg strömförbrukning genom deep sleep mellan uppdateringarna.

Ingen Home Assistant-installation eller lokal server krävs för normal drift. Displayen behöver bara ström och Wi-Fi.

**Konfigurera displayen:**  
https://hgpuke.github.io/elprisdisplay/

## Funktioner

- 72-timmars graf med:
  - dagens verkliga priser
  - morgondagens verkliga priser när de har publicerats
  - prognostiserade priser för kommande timmar
- Kvartspriser med aktuell prisindikering
- Dynamisk Y-skala, inklusive negativa priser och höga pristoppar
- Faktiska priser visas som fyllda staplar
- Prognoser visas som konturerade staplar
- Historiska staplar markeras med raster
- Min-, max- och aktuellpris
- Datum och veckodagar
- Batterinivå på displayen
- Automatisk svensk sommar-/vintertid
- Kvartssynkad deep sleep
- Wi-Fi aktiveras endast när ny data behöver hämtas
- Valbart elområde **SE1–SE4**
- Valbart pris med eller utan **25 % moms**
- Valfri MQTT-telemetri till exempelvis Home Assistant

## Datakällor

### Verkliga priser

Dagens och morgondagens kvartspriser hämtas från:

`https://www.elprisetjustnu.se/`

API:

```text
https://www.elprisetjustnu.se/api/v1/prices/YYYY/MM-DD_SE3.json
```

Elområdet i URL:en ändras automatiskt mellan SE1–SE4 enligt konfigurationen.

### Prisprognos

Prognoser hämtas från:

`https://wolta.se/`

API:

```text
https://wolta.se/api/v1/prices/forecast?zone=SE3
```

Wolta används för framtida priser tills verkliga priser finns tillgängliga. Verkliga priser har alltid företräde framför prognosen.

Både verkliga priser och prognoser behandlas utan moms från datakällan. Om användaren valt att visa priser inklusive moms lägger displayen på 25 % på båda på samma sätt.

## Uppdateringsstrategi

Displayen vaknar strax efter varje kvart:

```text
xx:00:10
xx:15:10
xx:30:10
xx:45:10
```

Den aktuella prisinformationen uppdateras och ESP32 går därefter tillbaka till deep sleep.

Wi-Fi startas inte vid varje uppvaknande.

Prognosen från Wolta hämtas normalt två gånger per dygn:

- omkring 00:00
- omkring 13:15

Efter 13:15 försöker displayen också hämta morgondagens verkliga priser. Om de ännu inte är publicerade görs ett nytt försök varje kvart tills de finns tillgängliga.

## Hårdvara

Projektet är utvecklat och testat på ett integrerat Waveshare-kort med:

| Del | Specifikation |
| --- | --- |
| MCU | ESP32-S3 N16R8 |
| Flash | 16 MB |
| PSRAM | 8 MB OPI |
| Display | 3,97" e-paper |
| Upplösning | 800 × 480 |
| E-paper controller | SSD1677 |
| RTC | PCF85063 |
| PMIC | AXP2101 |
| Temperatur/fukt | SHTC3 |
| Batteri | LiPo |
| Användarknapp | GPIO5 |

### E-paper-pinnar

| Signal | GPIO |
| --- | ---: |
| BUSY | 3 |
| DC | 9 |
| CS | 10 |
| SCLK | 11 |
| MOSI | 12 |
| RST | 46 |

### I²C

| Signal | GPIO |
| --- | ---: |
| SDA | 41 |
| SCL | 42 |

## Konfiguration

Elprisdisplayen använder **NVS** i ESP32 för permanent lagring av inställningar.

Följande kan konfigureras:

- Wi-Fi SSID
- Wi-Fi-lösenord
- Elområde SE1–SE4
- Priser inklusive eller exklusive moms
- MQTT på/av
- MQTT-server
- MQTT-port
- MQTT-användarnamn
- MQTT-lösenord

### Första installationen

Om displayen saknar giltig konfiguration går den automatiskt in i konfigurationsläge.

E-paper-displayen visar:

- en QR-kod
- texten som förklarar att koden ska skannas
- den fullständiga adressen:

```text
https://hgpuke.github.io/elprisdisplay/
```

Det gör att konfigurationen kan öppnas även från en dator utan kamera.

Konfigurationssidan använder Bluetooth LE för kommunikationen mellan webbläsaren och ESP32.

### Ändra en befintlig konfiguration

Tryck in användarknappen på displayen.

Displayen vaknar då direkt till **CONFIG MODE** och visar QR-koden igen.

Befintliga inställningar läses från NVS och visas på konfigurationssidan. En eller flera parametrar kan ändras.

Konfigurationsläget använder **inte deep sleep**. ESP32 ligger kvar vaken tills konfigurationen är avslutad, vilket också gör USB/Serial tillgängligt för felsökning.

När inställningarna sparas startar displayen om och återgår till normal drift.

### Lösenord

Wi-Fi- och MQTT-lösenord läses aldrig tillbaka från ESP32 till webbsidan.

Vid omkonfiguration betyder ett tomt lösenordsfält:

> Behåll det redan lagrade lösenordet.

Om MQTT stängs av ligger tidigare MQTT-inställningar kvar i NVS men används inte. De finns därför kvar om MQTT senare aktiveras igen.

## Webbläsare och Bluetooth

Konfigurationssidan använder **Web Bluetooth** och måste öppnas över HTTPS.

Den är testad med:

- Chrome på Android
- Chrome på dator
- Edge på dator

Webbläsaren måste ha stöd för Web Bluetooth.

BLE-enheten annonseras som:

```text
Elpris
```

med GATT-namnet:

```text
Elprisdisplay
```

## MQTT / Home Assistant

MQTT är helt valfritt.

När MQTT är aktiverat kan displayen publicera bland annat:

- batteriprocent
- batterispänning
- temperatur
- luftfuktighet

Displayen väcker aldrig Wi-Fi enbart för MQTT-telemetri. Telemetri skickas endast när Wi-Fi ändå behöver användas för att hämta prisdata eller prognoser.

Detta minskar energiförbrukningen.

## Arduino IDE

Projektet är utvecklat med Arduino IDE och ESP32 Arduino Core.

### Kortinställningar

Använd:

```text
Board: ESP32S3 Dev Module
Flash Size: 16MB
PSRAM: OPI PSRAM
USB CDC On Boot: Enabled
Partition Scheme: 16M Flash (3MB APP/9MB FATFS)
```

Om menyn **USB Mode** finns kan:

```text
Hardware CDC and JTAG
```

användas.

`USB CDC On Boot = Enabled` krävs för att `Serial` ska fungera korrekt via ESP32-S3:ans inbyggda USB-port.

## Bibliotek

Firmware använder bland annat:

- GxEPD2
- U8g2_for_Adafruit_GFX
- ArduinoJson
- PubSubClient
- NimBLE-Arduino

Dessutom används bibliotek som ingår i ESP32 Arduino Core, bland annat:

- WiFi
- Wire
- SPI
- FFat
- Preferences

## Bygga och flasha

Öppna:

```text
firmware/elprisdisplay/elprisdisplay.ino
```

i Arduino IDE.

Kontrollera kortinställningarna ovan och kompilera/skicka sedan programmet till ESP32-S3.

Efter flashning kan Serial Monitor användas med:

```text
115200 baud
```

Vid normal drift försvinner USB-serieporten när ESP32 går till deep sleep och återkommer när enheten vaknar.

## Projektstruktur

```text
elprisdisplay/
├── docs/
│   └── index.html
├── firmware/
│   └── elprisdisplay/
│       ├── elprisdisplay.ino
│       ├── EpConfigBle.cpp
│       ├── EpConfigBle.h
│       ├── EpSettings.cpp
│       ├── EpSettings.h
│       ├── EpQrData.h
│       ├── PriceSources.cpp
│       ├── PriceSources.h
│       └── ...
├── .gitignore
└── README.md
```

`docs/` publiceras via GitHub Pages och innehåller konfigurationsgränssnittet.

`firmware/` innehåller ESP32-firmwaren.

## Strömsparande

ESP32 använder deep sleep mellan kvartsuppdateringarna.

E-paper-kontrollern sätts i hibernate men panelens 3,3 V-matning via AXP2101 ALDO3 lämnas på.

Detta är avsiktligt.

Tester visade att avstängning av ALDO3 minskade viloströmmen, men samtidigt förlorades e-paper-kontrollerns RAM-innehåll. Nästa partiella uppdatering gav då korrupt bild.

Den valda lösningen prioriterar därför stabila partiella uppdateringar framför den sista möjliga minskningen av viloström.

## Säkerhet och privata uppgifter

Lägg aldrig in lokala lösenord eller andra autentiseringsuppgifter i repositoryt.

Följande ska inte versionshanteras:

```text
secrets.h
```

Den normala firmwareversionen lagrar användarens konfiguration i ESP32:ans NVS.

GitHub Pages-sidan innehåller inga lagrade användaruppgifter. Wi-Fi- och MQTT-lösenord överförs lokalt från webbläsaren till displayen via Bluetooth.

## Version

Nuvarande stabila version:

**v2.0**

Denna version omfattar:

- den färdiga 72-timmars prisdisplayen
- verkliga och prognostiserade kvartspriser
- deep-sleep-scheduler
- batteri- och sensortelemetri
- NVS-baserad konfiguration
- QR-baserad första installation
- BLE/Web Bluetooth-konfiguration
- omkonfiguration via användarknappen

Git-taggen `v0.2.2` markerar den verifierade hårdvaruversion som färdigställde konfigurationsdelen av Elprisdisplay 2.0.
(Det fanns en version 1.0 tidigare som aldrig publicerades på GitHub)
