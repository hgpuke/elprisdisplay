#include "EpConfigBle.h"
#include "EpSettings.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <string>
#include <vector>

#if !__has_include(<NimBLEDevice.h>)
#error "NimBLE-Arduino saknas. Installera 'NimBLE-Arduino' av h2zero i Arduino Library Manager (2.x)."
#endif
#include <NimBLEDevice.h>

namespace {
constexpr const char *SVC_UUID    = "00467768-6228-2272-4663-277478268000";
constexpr const char *STATE_UUID  = "00467768-6228-2272-4663-277478268001";
constexpr const char *ERROR_UUID  = "00467768-6228-2272-4663-277478268002";
constexpr const char *RPC_UUID    = "00467768-6228-2272-4663-277478268003";
constexpr const char *RESULT_UUID = "00467768-6228-2272-4663-277478268004";
constexpr const char *CAP_UUID    = "00467768-6228-2272-4663-277478268005";
constexpr const char *NET_UUID    = "c0161234-4455-4f66-8777-123456789001";
constexpr const char *CFG_UUID    = "c0161234-4455-4f66-8777-123456789002";

struct NetworkInfo {
  String ssid;
  int rssi;
  bool secured;
};

std::vector<NetworkInfo> networks;
std::string cfgBuffer;

NimBLECharacteristic *stateChar = nullptr;
NimBLECharacteristic *errorChar = nullptr;
NimBLECharacteristic *rpcChar = nullptr;
NimBLECharacteristic *resultChar = nullptr;
NimBLECharacteristic *netChar = nullptr;
NimBLECharacteristic *cfgChar = nullptr;

SemaphoreHandle_t stateMutex = nullptr;

EpSettings pendingSettings;
bool pendingSettingsReady = false;
bool configSaved = false;
bool restartRequested = false;

String pendingWifiSsid;
String pendingWifiPass;
bool wifiRequestReady = false;

void setState(uint8_t value) {
  stateChar->setValue(&value, 1);
  stateChar->notify();
}

void setError(uint8_t value) {
  errorChar->setValue(&value, 1);
  errorChar->notify();
}

uint8_t checksum(const std::string &v, size_t n) {
  uint8_t sum = 0;
  for (size_t i = 0; i < n; ++i)
    sum = static_cast<uint8_t>(sum + static_cast<uint8_t>(v[i]));
  return sum;
}

bool validZone(const String &zone) {
  return zone == "SE1" || zone == "SE2" || zone == "SE3" || zone == "SE4";
}

String publicConfigJson() {
  if (!ep_config_is_valid()) return "";

  const EpSettings &s = ep_settings();
  JsonDocument doc;
  doc["ssid"] = s.ssid;
  doc["zone"] = s.priceZone;
  doc["includeVat"] = s.includeVat;

  JsonObject mqtt = doc["mqtt"].to<JsonObject>();
  mqtt["enabled"] = s.mqttEnabled;
  mqtt["host"] = s.mqttHost;
  mqtt["port"] = s.mqttPort;
  mqtt["user"] = s.mqttUser;
  mqtt["passwordSet"] = !s.mqttPassword.isEmpty();

  String out;
  serializeJson(doc, out);
  return out;
}

uint8_t parsePendingConfig() {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, cfgBuffer);
  if (err) {
    Serial.printf("CONFIG BLE: JSON-fel: %s\n", err.c_str());
    return 3;
  }

  const char *zoneRaw = doc["zone"].as<const char*>();
  if (!zoneRaw) return 4;
  String zone(zoneRaw);
  if (!validZone(zone)) return 4;

  if (!doc["includeVat"].is<bool>()) return 4;
  if (!doc["mqtt"].is<JsonObject>()) return 4;

  JsonObject mqtt = doc["mqtt"].as<JsonObject>();
  if (!mqtt["enabled"].is<bool>()) return 4;

  const char *hostRaw = mqtt["host"].as<const char*>();
  const char *userRaw = mqtt["user"].as<const char*>();
  if (!hostRaw || !userRaw) return 4;

  uint32_t port = mqtt["port"].as<uint32_t>();
  if (port == 0 || port > 65535) return 4;

  EpSettings candidate;
  if (ep_config_is_valid()) candidate = ep_settings();

  candidate.priceZone = zone;
  candidate.includeVat = doc["includeVat"].as<bool>();
  candidate.mqttEnabled = mqtt["enabled"].as<bool>();
  candidate.mqttHost = hostRaw;
  candidate.mqttPort = static_cast<uint16_t>(port);
  candidate.mqttUser = userRaw;

  if (candidate.mqttHost.length() > 127 || candidate.mqttUser.length() > 63) return 4;
  if (candidate.mqttEnabled && candidate.mqttHost.isEmpty()) return 4;

  if (mqtt["password"].is<const char*>()) {
    const char *pw = mqtt["password"].as<const char*>();
    if (!pw) return 4;
    candidate.mqttPassword = pw;
  }
  if (candidate.mqttPassword.length() > 127) return 4;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pendingSettings = candidate;
  pendingSettingsReady = true;
  configSaved = false;
  xSemaphoreGive(stateMutex);

  Serial.printf("CONFIG BLE: mottaget zone=%s moms=%s MQTT=%s\n",
                candidate.priceZone.c_str(),
                candidate.includeVat ? "pa" : "av",
                candidate.mqttEnabled ? "pa" : "av");
  return 0;
}

bool takePendingSettings(EpSettings &out) {
  bool ok;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  ok = pendingSettingsReady;
  if (ok) out = pendingSettings;
  xSemaphoreGive(stateMutex);
  return ok;
}

bool commitWithWifi(const String &ssid, const String &password) {
  EpSettings candidate;
  if (!takePendingSettings(candidate)) {
    if (!ep_config_is_valid()) return false;
    candidate = ep_settings();
  }

  candidate.ssid = ssid;
  candidate.wifiPassword = password;

  if (!ep_save_settings(candidate)) {
    Serial.println("CONFIG BLE: NVS-sparande misslyckades");
    return false;
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  configSaved = true;
  pendingSettingsReady = false;
  xSemaphoreGive(stateMutex);

  ep_print_settings_summary();
  return true;
}

bool finishWithoutWifiChange() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool saved = configSaved;
  bool pending = pendingSettingsReady;
  EpSettings candidate = pendingSettings;
  xSemaphoreGive(stateMutex);

  if (saved) return true;
  if (!pending || !ep_config_is_valid()) return false;

  candidate.ssid = ep_settings().ssid;
  candidate.wifiPassword = ep_settings().wifiPassword;

  if (!ep_save_settings(candidate)) return false;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  configSaved = true;
  pendingSettingsReady = false;
  xSemaphoreGive(stateMutex);

  ep_print_settings_summary();
  return true;
}

void queueWifi(const String &ssid, const String &pass) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pendingWifiSsid = ssid;
  pendingWifiPass = pass;
  wifiRequestReady = true;
  xSemaphoreGive(stateMutex);
}

bool takeWifi(String &ssid, String &pass) {
  bool ready;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  ready = wifiRequestReady;
  if (ready) {
    ssid = pendingWifiSsid;
    pass = pendingWifiPass;
    wifiRequestReady = false;
  }
  xSemaphoreGive(stateMutex);
  return ready;
}

void handleNetworkCharacteristic(NimBLECharacteristic *c, const std::string &v) {
  uint8_t ix = v.size() ? static_cast<uint8_t>(v[0]) : 0;
  uint8_t part = v.size() > 1 ? static_cast<uint8_t>(v[1]) : 0;

  uint8_t response[20] = { static_cast<uint8_t>(networks.size()), 0, 0, 0 };
  size_t bytes = 4;

  if (ix >= 1 && ix <= networks.size()) {
    const auto &n = networks[ix - 1];
    response[1] = static_cast<uint8_t>(static_cast<int8_t>(std::max(-127, std::min(0, n.rssi))));
    response[2] = n.secured ? 1 : 0;

    size_t length = std::min<size_t>(32, n.ssid.length());
    response[3] = static_cast<uint8_t>(length);

    size_t offset = static_cast<size_t>(part) * 16;
    if (offset < length) {
      size_t take = std::min<size_t>(16, length - offset);
      memcpy(response + 4, n.ssid.c_str() + offset, take);
      bytes += take;
    }
  }

  c->setValue(response, bytes);
}

void handleConfigCharacteristic(NimBLECharacteristic *c, const std::string &v) {
  if (v.empty()) {
    uint8_t status = 1;
    c->setValue(&status, 1);
    return;
  }

  const uint8_t cmd = static_cast<uint8_t>(v[0]);

  if (cmd == 0x10) {
    const uint8_t part = v.size() > 1 ? static_cast<uint8_t>(v[1]) : 0;
    const String json = publicConfigJson();
    const size_t total = json.length();
    const size_t offset = static_cast<size_t>(part) * 16;
    const size_t take = offset < total ? std::min<size_t>(16, total - offset) : 0;

    uint8_t response[20] = {
      0,
      static_cast<uint8_t>(total & 0xFF),
      static_cast<uint8_t>((total >> 8) & 0xFF),
      static_cast<uint8_t>(take)
    };
    if (take) memcpy(response + 4, json.c_str() + offset, take);
    c->setValue(response, 4 + take);
    return;
  }

  uint8_t status = 0;

  if (cmd == 0x01) {
    cfgBuffer.clear();
  } else if (cmd == 0x02) {
    if (cfgBuffer.size() + v.size() - 1 > 1024) status = 2;
    else cfgBuffer.append(v.data() + 1, v.size() - 1);
  } else if (cmd == 0x03) {
    status = parsePendingConfig();
  } else if (cmd == 0x20) {
    if (!finishWithoutWifiChange()) status = 5;
    if (status == 0) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      restartRequested = true;
      xSemaphoreGive(stateMutex);
    }
  } else {
    status = 1;
  }

  c->setValue(&status, 1);
}

void handleRpcCharacteristic(NimBLECharacteristic *c, const std::string &v) {
  (void)c;
  setError(0);

  if (v.size() < 3) {
    setError(1);
    return;
  }

  const uint8_t cmd = static_cast<uint8_t>(v[0]);
  const uint8_t dataLen = static_cast<uint8_t>(v[1]);

  if (v.size() != static_cast<size_t>(dataLen) + 3 ||
      checksum(v, v.size() - 1) != static_cast<uint8_t>(v.back())) {
    setError(1);
    return;
  }

  if (cmd != 0x01) {
    setError(2);
    return;
  }

  size_t p = 2;
  if (p >= v.size() - 1) { setError(1); return; }

  uint8_t ssidLen = static_cast<uint8_t>(v[p++]);
  if (ssidLen == 0 || ssidLen > 32 || p + ssidLen >= v.size()) {
    setError(1);
    return;
  }

  String ssid;
  for (uint8_t i = 0; i < ssidLen; ++i) ssid += v[p++];

  if (p >= v.size() - 1) { setError(1); return; }

  uint8_t passLen = static_cast<uint8_t>(v[p++]);
  if (passLen > 64 || p + passLen != v.size() - 1) {
    setError(1);
    return;
  }

  String pass;
  for (uint8_t i = 0; i < passLen; ++i) pass += v[p++];

  Serial.printf("CONFIG BLE: WiFi-uppgifter mottagna for \"%s\"\n", ssid.c_str());
  queueWifi(ssid, pass);
  setState(3);
}

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    (void)info;
    const std::string v = c->getValue();

    if (c == netChar) handleNetworkCharacteristic(c, v);
    else if (c == cfgChar) handleConfigCharacteristic(c, v);
    else if (c == rpcChar) handleRpcCharacteristic(c, v);
  }
};

CharacteristicCallbacks callbacks;

std::vector<NetworkInfo> scanWifi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);

  int n = WiFi.scanNetworks(false, true);
  std::vector<NetworkInfo> found;

  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;

    int rssi = WiFi.RSSI(i);
    bool secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

    bool duplicate = false;
    for (auto &e : found) {
      if (e.ssid == ssid) {
        if (rssi > e.rssi) {
          e.rssi = rssi;
          e.secured = secured;
        }
        duplicate = true;
        break;
      }
    }

    if (!duplicate) found.push_back({ssid, rssi, secured});
  }

  WiFi.scanDelete();

  std::sort(found.begin(), found.end(),
            [](const NetworkInfo &a, const NetworkInfo &b) {
              return a.rssi > b.rssi;
            });

  if (found.size() > 50) found.resize(50);
  return found;
}

bool connectWifi(const String &ssid, const String &pass) {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);

  if (pass.isEmpty()) WiFi.begin(ssid.c_str());
  else WiFi.begin(ssid.c_str(), pass.c_str());

  const uint32_t start = millis();
  while (millis() - start < 20000) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("CONFIG BLE: WiFi anslutet till \"%s\", IP %s\n",
                    ssid.c_str(), WiFi.localIP().toString().c_str());
      return true;
    }
    delay(100);
  }

  Serial.printf("CONFIG BLE: WiFi-anslutning till \"%s\" misslyckades\n", ssid.c_str());
  return false;
}

void startBle() {
  NimBLEDevice::init("Elprisdisplay");
  NimBLEDevice::setMTU(128);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->advertiseOnDisconnect(true);

  NimBLEService *service = server->createService(SVC_UUID);

  stateChar = service->createCharacteristic(
    STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  errorChar = service->createCharacteristic(
    ERROR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  rpcChar = service->createCharacteristic(
    RPC_UUID, NIMBLE_PROPERTY::WRITE);
  resultChar = service->createCharacteristic(
    RESULT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *capChar = service->createCharacteristic(
    CAP_UUID, NIMBLE_PROPERTY::READ);
  netChar = service->createCharacteristic(
    NET_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  cfgChar = service->createCharacteristic(
    CFG_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);

  rpcChar->setCallbacks(&callbacks);
  netChar->setCallbacks(&callbacks);
  cfgChar->setCallbacks(&callbacks);

  uint8_t state = 2;
  uint8_t error = 0;
  uint8_t caps = 0;
  uint8_t zero = 0;

  stateChar->setValue(&state, 1);
  errorChar->setValue(&error, 1);
  capChar->setValue(&caps, 1);
  resultChar->setValue(&zero, 1);
  netChar->setValue(&zero, 1);
  cfgChar->setValue(&zero, 1);

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  // Legacy BLE advertising has only 31 bytes. A 128-bit service UUID plus
  // the full local name "Elprisdisplay" does not fit in the primary packet.
  // Some Android/Web Bluetooth choosers then show the peripheral as unknown
  // because they do not reliably use the scan-response name.
  //
  // Keep the full GATT device name from NimBLEDevice::init("Elprisdisplay"),
  // but advertise the short local name "Elpris". UUID + name now fit in the
  // same primary advertising packet.
  advertising->addServiceUUID(SVC_UUID);
  advertising->setName("Elpris");
  advertising->enableScanResponse(true);
  advertising->start();

  Serial.println("CONFIG BLE: annonserar lokalt som \"Elpris\" (GATT-namn \"Elprisdisplay\")");
}

bool shouldRestart() {
  bool v;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  v = restartRequested;
  xSemaphoreGive(stateMutex);
  return v;
}
} // namespace

void ep_run_config_mode() {
  stateMutex = xSemaphoreCreateMutex();
  if (!stateMutex) {
    Serial.println("CONFIG BLE: kunde inte skapa mutex");
    for (;;) delay(1000);
  }

  Serial.println("CONFIG MODE: deep sleep disabled");
  Serial.println("CONFIG MODE: normal scheduler disabled");

  networks = scanWifi();
  Serial.printf("CONFIG BLE: %u WiFi-natverk hittades\n", (unsigned)networks.size());

  startBle();

  uint32_t lastStatus = millis();

  for (;;) {
    String ssid, pass;

    if (takeWifi(ssid, pass)) {
      setError(0);

      if (!connectWifi(ssid, pass)) {
        setError(3);
        setState(2);
      } else if (!commitWithWifi(ssid, pass)) {
        setError(0xFF);
        setState(2);
      } else {
        setError(0);
        setState(4);
      }
    }

    if (shouldRestart()) {
      Serial.println("CONFIG MODE: sparat. Startar om till normal drift.");
      Serial.flush();
      delay(400);
      ESP.restart();
    }

    if (millis() - lastStatus >= 30000) {
      lastStatus = millis();
      Serial.printf("CONFIG MODE: vantar pa anvandare, heap=%u\n", ESP.getFreeHeap());
    }

    delay(20);
  }
}
