#pragma once

#include <Arduino.h>

#include "chargescreen_stats.h"

// Reads the 4G signal from a TP-Link Deco (X1500-4G) on the local network:
// owner-password login, one read of admin/network?form=internet, logout.
// Protocol from the tplinkrouterc6u Deco client; see the parking design spec.
// Netlink task only (uses large static buffers). Returns false on any error.
bool decoReadSignal(const char *host, const char *password, cs::DecoSignal &sig);
