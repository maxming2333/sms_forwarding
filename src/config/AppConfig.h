#pragma once
#include <Arduino.h>
#include <Preferences.h>

// ── Hardware pins ────────────────────────────────────────────────────────────
#define TXD           3
#define RXD           4
#define MODEM_EN_PIN  5
#ifndef LED_BUILTIN
#  define LED_BUILTIN 8
#endif

// ── Web management defaults ──────────────────────────────────────────────────
#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

// ── Push channel limits ──────────────────────────────────────────────────────
#define MAX_PUSH_CHANNELS 5

// ── Push type enumeration ────────────────────────────────────────────────────
// Source of truth: src/config/PushTypeMeta.def  (X-macro format)
// The same file is parsed by script/pre_build.py to forward raw enum data
// to the Vue frontend.  UI metadata (labels/hints) lives in
// web/src/components/PushChannelEditor.vue.
enum PushType {
#define PUSH_TYPE_DEF(key, val) key = val,
#include "PushTypeMeta.def"
#undef PUSH_TYPE_DEF
};

// ── Structs ──────────────────────────────────────────────────────────────────
struct PushChannel {
  bool     enabled;
  PushType type;
  String   name;
  String   url;
  String   key1;
  String   key2;
  String   customBody;      // SMS template: {sender} {message} {timestamp} {device}
  String   customCallBody;  // Call template: {caller} {caller_fmt} {timestamp} {receiver}
};

struct Config {
  String      smtpServer;
  int         smtpPort;
  String      smtpUser;
  String      smtpPass;
  String      smtpSendTo;
  String      adminPhone;
  PushChannel pushChannels[MAX_PUSH_CHANNELS];
  String      webUser;
  String      webPass;
  String      wifiSSID;
  String      wifiPass;
  String      numberBlackList;
  // ── Scheduled reboot ───────────────────────────────────────────────────────
  bool        autoRebootEnabled;  // enable daily scheduled reboot
  String      autoRebootTime;     // "HH:MM" in local time (UTC+8)
};

// ── Global instances (defined in AppConfig.cpp) ──────────────────────────────
extern Config config;
extern bool   configValid;

// ── API ──────────────────────────────────────────────────────────────────────
void saveConfig();
void loadConfig();
bool isConfigValid();
bool isPushChannelValid(const PushChannel& ch);
bool isInNumberBlackList(const char* sender);

