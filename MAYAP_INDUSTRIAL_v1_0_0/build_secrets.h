#pragma once

// No fleet realtime credential. Unique device keys are persisted in NVS.

// Optional Arduino LAN OTA password. Empty disables the LAN upload service.
#ifndef MAYAP_OTA_PASSWORD
#define MAYAP_OTA_PASSWORD ""
#endif

// Optional ignored local overrides for the isolated bench host/LAN OTA password.
#if __has_include("build_secrets.local.h")
#include "build_secrets.local.h"
#endif
