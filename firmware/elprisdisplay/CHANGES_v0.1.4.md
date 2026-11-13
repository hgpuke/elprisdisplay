# v0.1.4

- Normal kvartssynkad deep sleep återaktiverad (`INTEGRATION_DEBUG_HOLD=false`).
- Kallstartsdelay åter till 3 s; timer-wake är fortsatt utan onödig delay.
- Headern 6 px lägre i höjd; pris/statistikfält flyttade 6 px upp.
- Batteriikonens nederkant ligger på headertextens baseline.
- Batteriet visar fyra diskreta fyllnadssegment.
- Batteriprocent högerjusteras mot x=798 så `100 %` ryms.
- `SEK/kWh` använder Barlow SemiBold 24 och ligger 7 px högre.
- Timaxeln använder ny Barlow Condensed SemiBold 22.
- Passerade faktiska timmar renderas med 75 % svart dither.
- ArduinoJson-fixarna från fungerande v0.1.3 PriceSources.cpp ingår.

Efter uppdatering måste `python3 tools/prepare_barlow_fonts.py` köras en gång så att `BarlowCondensedSemiBold22` genereras i `BarlowFonts.h`.
