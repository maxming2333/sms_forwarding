#pragma once
// All push-channel implementations share this header.
// Each channel function returns:
//   >0  : HTTP status code received
//    0  : Success (non-HTTP push, e.g. SMS forwarding)
//   -1  : Skip (disabled, incomplete config, etc.)
//   <-1 : Network error code

#include "config/AppConfig.h"

// ── Per-type SMS send functions ───────────────────────────────────────────────
int pushPostJson    (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushBark        (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushGet         (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushDingtalk    (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushPushplus    (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushServerchan  (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushCustom      (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushFeishu      (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushGotify      (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushTelegram    (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushWorkWeixin  (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);
int pushSmsForward  (const PushChannel& ch, const char* sender, const char* msg, const char* ts, const char* dev);

// ── Unified incoming-call notification ───────────────────────────────────────
// caller  : raw caller number (e.g. "+8613812345678")
// ts      : human-readable timestamp string (e.g. "2026-01-22 20:48:18")
// dev     : own SIM phone number (may be "未知号码")
// Uses ch.customCallBody if set; otherwise falls back to per-type built-in format.
int pushCall(const PushChannel& ch, const char* caller, const char* ts, const char* dev);

