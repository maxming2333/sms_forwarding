#pragma once
#include <Arduino.h>

// Long SMS concat buffer limits
#define MAX_CONCAT_PARTS    10
#define CONCAT_TIMEOUT_MS   30000
#define MAX_CONCAT_MESSAGES 5

// Incoming-call dedup window
#define CALL_NOTIFY_INTERVAL_MS 30000

// ── API ───────────────────────────────────────────────────────────────────────
void smsReceiverInit();               // initialise concat buffer
void smsReceiverTick();               // call every loop iteration
void processSmsContent(const char* sender, const char* text, const char* timestamp);
void processIncomingCall(const char* caller);

