#pragma once
#include <Arduino.h>

struct EpSettings {
  String ssid;
  String wifiPassword;
  String priceZone;
  bool includeVat = false;
  bool mqttEnabled = false;
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPassword;
  uint32_t revision = 0;
};

const EpSettings &ep_settings();
bool ep_config_is_valid();
bool ep_load_settings();
bool ep_save_settings(const EpSettings &settings);
bool ep_load_or_seed_legacy(const char *ssid,
                            const char *wifiPassword,
                            const char *priceZone,
                            bool includeVat,
                            bool mqttEnabled,
                            const char *mqttHost,
                            uint16_t mqttPort,
                            const char *mqttUser,
                            const char *mqttPassword);
void ep_print_settings_summary();
