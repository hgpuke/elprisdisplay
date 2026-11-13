#include "EpSettings.h"
#include <Preferences.h>

namespace {
EpSettings active;
bool ready = false;
static const char *slotNames[] = {"epslot0", "epslot1"};

bool validZone(const String &zone) {
  return zone == "SE1" || zone == "SE2" || zone == "SE3" || zone == "SE4";
}

bool valid(const EpSettings &s) {
  if (s.ssid.isEmpty() || s.ssid.length() > 32) return false;
  if (s.wifiPassword.length() > 64) return false;
  if (!validZone(s.priceZone)) return false;
  if (s.mqttHost.length() > 127) return false;
  if (s.mqttUser.length() > 63) return false;
  if (s.mqttPassword.length() > 127) return false;
  if (s.mqttPort == 0) return false;
  if (s.mqttEnabled && s.mqttHost.isEmpty()) return false;
  return true;
}

bool readSlot(unsigned slot, EpSettings &out) {
  Preferences p;
  if (!p.begin(slotNames[slot], true)) return false;
  if (!p.getBool("valid", false)) {
    p.end();
    return false;
  }

  EpSettings s;
  s.ssid = p.getString("ssid", "");
  s.wifiPassword = p.getString("wpass", "");
  s.priceZone = p.getString("zone", "");
  s.includeVat = p.getBool("vat", false);
  s.mqttEnabled = p.getBool("mqen", false);
  s.mqttHost = p.getString("mqhost", "");
  s.mqttPort = p.getUShort("mqport", 1883);
  s.mqttUser = p.getString("mquser", "");
  s.mqttPassword = p.getString("mqpass", "");
  s.revision = p.getUInt("rev", 0);
  p.end();

  if (!valid(s) || s.revision == 0) return false;
  out = s;
  return true;
}

bool samePayload(const EpSettings &a, const EpSettings &b) {
  return a.ssid == b.ssid &&
         a.wifiPassword == b.wifiPassword &&
         a.priceZone == b.priceZone &&
         a.includeVat == b.includeVat &&
         a.mqttEnabled == b.mqttEnabled &&
         a.mqttHost == b.mqttHost &&
         a.mqttPort == b.mqttPort &&
         a.mqttUser == b.mqttUser &&
         a.mqttPassword == b.mqttPassword;
}
} // namespace

const EpSettings &ep_settings() {
  return active;
}

bool ep_config_is_valid() {
  return ready;
}

bool ep_load_settings() {
  EpSettings a, b;
  const bool va = readSlot(0, a);
  const bool vb = readSlot(1, b);
  ready = va || vb;

  if (!ready) {
    active = EpSettings{};
    return false;
  }

  active = !va ? b :
           !vb ? a :
           ((int32_t)(a.revision - b.revision) > 0 ? a : b);
  return true;
}

bool ep_save_settings(const EpSettings &settings) {
  EpSettings candidate = settings;
  candidate.revision = ready ? active.revision + 1 : 1;
  if (!valid(candidate)) return false;

  EpSettings s0, s1;
  const bool has0 = readSlot(0, s0);
  const bool has1 = readSlot(1, s1);
  const unsigned target = !has0 ? 0 :
                          !has1 ? 1 :
                          ((int32_t)(s0.revision - s1.revision) > 0 ? 1 : 0);

  Preferences p;
  if (!p.begin(slotNames[target], false)) return false;

  // Markören skrivs sist. Ett strömavbrott mitt i skrivningen gör inte
  // den halvskrivna slotten giltig och den äldre slotten finns kvar.
  bool ok = p.putBool("valid", false) > 0;
  ok = p.putString("ssid", candidate.ssid) > 0 && ok;
  ok = p.putString("wpass", candidate.wifiPassword) > 0 && ok;
  ok = p.putString("zone", candidate.priceZone) > 0 && ok;
  ok = p.putBool("vat", candidate.includeVat) > 0 && ok;
  ok = p.putBool("mqen", candidate.mqttEnabled) > 0 && ok;
  ok = p.putString("mqhost", candidate.mqttHost) > 0 && ok;
  ok = p.putUShort("mqport", candidate.mqttPort) > 0 && ok;
  ok = p.putString("mquser", candidate.mqttUser) > 0 && ok;
  ok = p.putString("mqpass", candidate.mqttPassword) > 0 && ok;
  ok = p.putUInt("rev", candidate.revision) > 0 && ok;
  if (ok) ok = p.putBool("valid", true) > 0;
  p.end();
  if (!ok) return false;

  EpSettings verify;
  if (!readSlot(target, verify) || !samePayload(candidate, verify) ||
      verify.revision != candidate.revision) {
    return false;
  }

  active = candidate;
  ready = true;
  return true;
}

bool ep_load_or_seed_legacy(const char *ssid,
                            const char *wifiPassword,
                            const char *priceZone,
                            bool includeVat,
                            bool mqttEnabled,
                            const char *mqttHost,
                            uint16_t mqttPort,
                            const char *mqttUser,
                            const char *mqttPassword) {
  if (ep_load_settings()) return true;

  Serial.println("CONFIG: ingen giltig NVS-konfiguration; migrerar nuvarande secrets.h");

  EpSettings legacy;
  legacy.ssid = ssid ? ssid : "";
  legacy.wifiPassword = wifiPassword ? wifiPassword : "";
  legacy.priceZone = priceZone ? priceZone : "";
  legacy.includeVat = includeVat;
  legacy.mqttEnabled = mqttEnabled;
  legacy.mqttHost = mqttHost ? mqttHost : "";
  legacy.mqttPort = mqttPort;
  legacy.mqttUser = mqttUser ? mqttUser : "";
  legacy.mqttPassword = mqttPassword ? mqttPassword : "";

  if (!ep_save_settings(legacy)) {
    Serial.println("CONFIG: migrering till NVS MISSLYCKADES");
    return false;
  }

  Serial.println("CONFIG: nuvarande secrets.h har migrerats till NVS");
  return true;
}

void ep_print_settings_summary() {
  if (!ready) {
    Serial.println("CONFIG: ingen giltig konfiguration");
    return;
  }

  Serial.printf("CONFIG: rev=%lu WiFi=\"%s\" zone=%s moms=%s MQTT=%s",
                (unsigned long)active.revision,
                active.ssid.c_str(),
                active.priceZone.c_str(),
                active.includeVat ? "pa" : "av",
                active.mqttEnabled ? "pa" : "av");

  if (active.mqttEnabled) {
    Serial.printf(" server=%s:%u password=%s",
                  active.mqttHost.c_str(),
                  (unsigned)active.mqttPort,
                  active.mqttPassword.isEmpty() ? "saknas" : "sparat");
  }
  Serial.println();
}
