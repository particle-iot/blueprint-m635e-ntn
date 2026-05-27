/*
 * Copyright (c) 2026 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

// =============================================================================
// M635E NTN Blueprint Demo - Application Configuration
// =============================================================================
// Central configuration for the always-on NTN satellite demo. Every value that
// controls runtime behaviour for the demo lives here so it can be tuned in one
// place. These are compile-time #defines: change a value, recompile, reflash.
//
// Conventions:
//   *_MS values are milliseconds.
//   FEATURE_* flags are intended for use with #if so unused code can be
//   compiled out entirely.
// =============================================================================


// -----------------------------------------------------------------------------
// Feature toggles
// -----------------------------------------------------------------------------
// Enable / disable each connectivity stack. Use with #if in application code so
// the unused stack can be excluded from the build.
//   1 = enabled, 0 = disabled.
#define FEATURE_LTE_ENABLED                 1
#define FEATURE_NTN_ENABLED                 1

#if !FEATURE_LTE_ENABLED && !FEATURE_NTN_ENABLED
#error "At least one of FEATURE_LTE_ENABLED / FEATURE_NTN_ENABLED must be set"
#endif


// -----------------------------------------------------------------------------
// Startup
// -----------------------------------------------------------------------------
// Which radio the device boots on.
//   1 = boot on Cellular (LTE-M), 0 = boot on Satellite (NTN).
// NOTE: For a real deployment you would normally start on Cellular 
// and only fall back to Satellite. Starting on Satellite is
// useful for NTN-first demos.
#define START_ON_CELLULAR                   0


// -----------------------------------------------------------------------------
// Publish timing
// -----------------------------------------------------------------------------
// How often to publish in each mode. Do NOT set the satellite interval below
// 10 seconds.
#define LTE_PUBLISH_INTERVAL_MS             (60UL * 1000)
#define NTN_PUBLISH_INTERVAL_MS             (5UL * 60 * 1000)


// -----------------------------------------------------------------------------
// Radio switching timeouts
// -----------------------------------------------------------------------------
// It is NOT recommended to set these below 10 minutes.
//   CELLULAR_DISCONNECTED_TIMEOUT_MS : time disconnected on LTE before
//                                      switching to Satellite. (There is no
//                                      cellular "connected" timeout - if LTE is
//                                      connected there is no reason to switch.)
//   SATELLITE_CONNECTED_TIMEOUT_MS   : time connected on Satellite before
//                                      switching back to test Cellular again.
//   SATELLITE_DISCONNECTED_TIMEOUT_MS: time disconnected on Satellite before
//                                      switching back to Cellular. Satellite
//                                      can take a while to connect - don't set
//                                      this too low.
#define CELLULAR_DISCONNECTED_TIMEOUT_MS    (10UL * 60 * 1000)
#define SATELLITE_CONNECTED_TIMEOUT_MS      (10UL * 60 * 1000)
#define SATELLITE_DISCONNECTED_TIMEOUT_MS   (20UL * 60 * 1000)


// -----------------------------------------------------------------------------
// Forced switching (testing only)
// -----------------------------------------------------------------------------
// Set both FORCE_* flags to 0 for normal operation. Set to 1 to force a switch
// between radios purely on the timeout below (ignores connection state) so the
// switching logic can be exercised on the bench.
//   e.g. FORCE_CELLULAR_TO_SATELLITE_SWITCH=1 with a 10-minute timeout switches
//   from Cellular to Satellite after 10 minutes regardless of LTE signal.
#define FORCE_CELLULAR_TO_SATELLITE_SWITCH          0
#define FORCE_SATELLITE_TO_CELLULAR_SWITCH          0
#define FORCE_C2S_SWITCH_TIMEOUT_MS                 (10UL * 60 * 1000)
#define FORCE_S2C_SWITCH_TIMEOUT_MS                 (10UL * 60 * 1000)


// -----------------------------------------------------------------------------
// Location (for NTN locfix)
// -----------------------------------------------------------------------------
// NTN attach requires a location. The device sets it on the modem via
// AT+QNWCFG="ntn_locfix",... before registration.
//
// LOC_SOURCE selects where that location comes from:
//   LOC_SOURCE_FIXED              : always use the fixed coordinates below.
//   LOC_SOURCE_GPS_FIXED_FALLBACK : try a GPS fix first (up to
//                                   LOC_GPS_FIX_TIMEOUT_MS); if none, fall back
//                                   to the fixed coordinates below.
#define LOC_SOURCE_FIXED                    0
#define LOC_SOURCE_GPS_FIXED_FALLBACK       1
#define LOC_SOURCE                          LOC_SOURCE_GPS_FIXED_FALLBACK

// Abandon the GPS fix attempt after this long and use the fixed fallback.
#define LOC_GPS_FIX_TIMEOUT_MS              (60UL * 1000)

// Fixed fallback coordinates (decimal degrees / metres).
#define LOC_FIXED_LATITUDE                  (38.07315)
#define LOC_FIXED_LONGITUDE                 (-122.16545)
#define LOC_FIXED_ALTITUDE                  (111.8)
