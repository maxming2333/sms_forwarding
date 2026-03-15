#pragma once
#include "config/AppConfig.h"

// Dispatch SMS to all enabled push channels
void pushAll(const char* sender, const char* message, const char* timestamp);

// Dispatch SMS to a single channel; returns HTTP code or -1
int  pushOne(const PushChannel& ch, const char* sender, const char* message,
             const char* timestamp, const char* devicePhone);

// Dispatch incoming-call notification to all enabled push channels
void pushCallAll(const char* caller, const char* timestamp);

// Dispatch incoming-call notification to a single channel; returns HTTP code or -1
int  pushCallOne(const PushChannel& ch, const char* caller,
                 const char* timestamp, const char* devicePhone);

