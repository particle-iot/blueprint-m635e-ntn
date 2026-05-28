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

#include "Particle.h"
#include "satellite.h"
#include "modem_manager.h"
#include "app_config.h"

SYSTEM_MODE(SEMI_AUTOMATIC);

SerialLogHandler logHandler(LOG_LEVEL_ALL);

Satellite satellite;
ModemManager modem;

// -----------------------------------------------------------------------------
// Application state machine
// -----------------------------------------------------------------------------
enum class AppState {
    Boot,              // init: enable the start radio, reset timers
    CellularConnect,   // Particle.connect() issued; waiting for the cloud
    CellularOnline,    // connected on LTE; publishing on the LTE interval
    AcquireLocation,   // get a GPS fix (or fixed fallback) and set ntn_locfix
    SatelliteConnect,  // satellite.begin()/connect(); waiting for NTN registration
    SatelliteOnline,   // connected on NTN; publishing on the NTN interval
    SwitchToSatellite, // tear down cloud + cellular, enable the Satellite radio
    SwitchToCellular,  // tear down satellite, enable the Cellular radio
    Fault              // error / recovery (modem-reset escalation hook)
};

static AppState appState = AppState::Boot;
static bool stateEntry = true;   // true on the first loop() after a transition

uint32_t lastPublish = 0;
uint32_t connectedStartTime = 0;
uint32_t disconnectedStartTime = 0;
uint32_t connectedDurationAccum = 0;
uint32_t disconnectedDurationAccum = 0;
uint32_t radioTime = 0;

int publishCount = 1;
int satPublishSuccess = 0;
int satPublishFailures = 0;

// Location used for outgoing publishes (decimal degrees / metres). Populated
// from a GPS fix or the fixed fallback; reused until refreshed.
double pubLat = 0;
double pubLon = 0;
double pubAlt = 0;
bool havePubLoc = false;

const char* stateName(AppState s) {
    switch (s) {
        case AppState::Boot:              return "Boot";
        case AppState::CellularConnect:   return "CellularConnect";
        case AppState::CellularOnline:    return "CellularOnline";
        case AppState::AcquireLocation:   return "AcquireLocation";
        case AppState::SatelliteConnect:  return "SatelliteConnect";
        case AppState::SatelliteOnline:   return "SatelliteOnline";
        case AppState::SwitchToSatellite: return "SwitchToSatellite";
        case AppState::SwitchToCellular:  return "SwitchToCellular";
        case AppState::Fault:             return "Fault";
        default:                          return "Unknown";
    }
}

void transitionTo(AppState next) {
    Log.info("STATE: %s -> %s", stateName(appState), stateName(next));
    appState = next;
    stateEntry = true;
}

// True only on the first loop() iteration after entering the current state.
// Used to run one-shot entry actions (e.g. begin()/connect()).
bool onEntry() {
    bool e = stateEntry;
    stateEntry = false;
    return e;
}

// Should we leave Cellular for Satellite? Driven by the disconnect timer (or the
// force flag for bench testing). Never true if NTN is disabled in config.
bool cellularShouldSwitchToSatellite() {
    if (!g_cfg.ntnEnabled) {
        return false;
    }
    if (g_cfg.forceCellularToSatelliteSwitch) {
        return radioTime && (millis() - radioTime > g_cfg.forceC2sSwitchTimeoutS * 1000UL);
    }
    return disconnectedStartTime && (millis() - disconnectedStartTime > g_cfg.cellularDisconnectedTimeoutS * 1000UL);
}

// Should we leave Satellite for Cellular? We don't camp on Satellite if Cellular
// might be available - go test it again after the connected/disconnected
// timeout. Never true if LTE is disabled in config.
bool satelliteShouldSwitchToCellular() {
    if (!g_cfg.lteEnabled) {
        return false;
    }
    if (g_cfg.forceSatelliteToCellularSwitch) {
        return radioTime && (millis() - radioTime > g_cfg.forceS2cSwitchTimeoutS * 1000UL);
    }
    return (disconnectedStartTime && (millis() - disconnectedStartTime > g_cfg.satelliteDisconnectedTimeoutS * 1000UL)) ||
           (connectedStartTime && (millis() - connectedStartTime > g_cfg.satelliteConnectedTimeoutS * 1000UL));
}

// Acquire the location to program into the modem's NTN location fix, then hand
// it to the satellite library. In Fixed mode we never query GNSS - we pass the
// configured coords through and tell the library to short-circuit any later
// getGNSSLocation() call (forceFixed=true). In Dynamic mode we try the GNSS
// engine for up to locGpsFixTimeoutS and fall back to the fixed coords.
void acquireAndSetLocationFix() {
    double lat = g_cfg.locFixedLatitude;
    double lon = g_cfg.locFixedLongitude;
    double alt = g_cfg.locFixedAltitude;
    const bool forceFixed = (g_cfg.locSource == LocSource::Fixed);

    if (!forceFixed) {
        if (satellite.getGNSSLocation(g_cfg.locGpsFixTimeoutS * 1000UL) == 0) {
            auto p = satellite.lastPositionInfo();
            lat = p.latitude;
            lon = p.longitude;
            alt = p.altitude;
        } else {
            Log.warn("No GNSS fix; using fixed fallback location");
        }
    }

    pubLat = lat;
    pubLon = lon;
    pubAlt = alt;
    havePubLoc = true;
    satellite.setLocationFix(lat, lon, alt, forceFixed);
}

// Build the location payload and route it to the active publish stack.
void publishLocationData() {
    auto now = (unsigned int)Time.now();

    particle::Variant locEvent;
    locEvent.set("cmd", "loc");
    locEvent.set("time", now);
    particle::Variant locationObject;
        locationObject.set("lck", havePubLoc ? 1 : 0);
        locationObject.set("time", now);
        locationObject.set("lat", pubLat);
        locationObject.set("lon", pubLon);
        locationObject.set("alt", pubAlt);
    locEvent.set("loc", locationObject);

    Log.info("publishing location %s", locEvent.toJSON().c_str());

    if (satellite.connected()) {
        Log.info("SATELLITE PUBLISH: {\"count\":%d} ------------------", publishCount);
        auto satPublishResult = satellite.publish(1 /* code */, locEvent);
        satPublishResult < 0 ? satPublishFailures++ : satPublishSuccess++;
        Log.info("Satellite publish successes/total %d/%d ", satPublishSuccess, satPublishSuccess + satPublishFailures);
        publishCount++;
    } else if (Particle.connected()) {
        Log.info("CELLULAR PUBLISH: {\"count\":%d} ------------------", publishCount);
        auto cloudPublishResult = Particle.publish("loc", locEvent);
        Log.info("Cellular publish result: %d", cloudPublishResult);
        publishCount++;
    }
}

void updateConnectionTimers(bool force=false) {
    int connected = 0;
    static int lastConnected = -1;
    static radio_type_t lastRadio = RADIO_UNKNOWN;
    radio_type_t radio = modem.radioEnabled();

    if (radio == RADIO_CELLULAR) {
        if (Particle.connected()) {
            connected = 1;
        }
    } else if (radio == RADIO_SATELLITE) {
        if (satellite.connected()) {
            connected = 1;
        }
    }

    // reset timers on radio change only
    if (lastRadio != radio) {
        connectedStartTime = 0;
        disconnectedStartTime = 0;
        connectedDurationAccum = 0;
        disconnectedDurationAccum = 0;
        lastConnected = -1;
        radioTime = millis();
        lastRadio = radio;
    }

    // make sure these are equal on first run
    if (lastConnected == -1) {
        lastConnected = connected;
    }

    // accumulate connected/disconnected time only
    if (connected) {
        if (lastConnected != connected) {
            // save disconnectedDurationAccum if we just switched from disconnected to connected
            if (disconnectedStartTime) {
                disconnectedDurationAccum = millis() - disconnectedStartTime;
            }
            // add any connected time accumulated
            if (connectedDurationAccum) {
                connectedStartTime = millis() - connectedDurationAccum;
            }
            lastConnected = connected;
        }
        if (!connectedStartTime) {
            connectedStartTime = millis();
        }
    } else {
        if (lastConnected != connected) {
            if (connectedStartTime) {
                // save connectedDurationAccum if we just switched from connected to disconnected
                connectedDurationAccum = millis() - connectedStartTime;
            }
            if (disconnectedDurationAccum) {
                // add any disconnected time accumulated
                disconnectedStartTime = millis() - disconnectedDurationAccum;
            }
            lastConnected = connected;
        }
        if (!disconnectedStartTime) {
            disconnectedStartTime = millis();
        }
    }

    static uint32_t lastCheck = millis();
    if (force || millis() - lastCheck > 5000) {
        lastCheck = millis();
        Log.info("[%s] Con: %lu, Dis: %lu ConAccum: %lu, DisAccum: %lu", satellite.connected() ? "CONNECTED" : "DISCONNECTED", millis() - connectedStartTime, millis() - disconnectedStartTime, connectedDurationAccum, disconnectedDurationAccum);
    }
}

void setup()
{
    waitFor(Serial.isConnected, 10000);
    WiFi.clearCredentials(); // force testing on Cellular/Satellite

    pinMode(D7, OUTPUT);
    digitalWrite(D7, LOW);

    // Load runtime config from the bundled app_config.json asset before
    // anything else reads g_cfg. Missing/invalid asset falls back to the
    // compiled defaults in app_config.cpp.
    loadAppConfig();

    modem.begin();

    // Hardware is initialised; the Boot state selects and enables the start radio.
    appState = AppState::Boot;
    stateEntry = true;
}

void loop()
{
    updateConnectionTimers();

    switch (appState) {
        // --------------------------------------------------------------------
        case AppState::Boot:
        {
            if (g_cfg.startOnCellular && g_cfg.lteEnabled) {
                // Cellular first
                Log.info("RADIO CELLULAR --------------------");
                modem.radioEnable(RADIO_CELLULAR);
                updateConnectionTimers(true /* forced log */);
                RGB.control(false);
                Particle.connect();
                transitionTo(AppState::CellularConnect);
            } else {
                // NTN-first demo: start on Satellite.
                Log.info("RADIO SATELLITE --------------------");
                modem.radioEnable(RADIO_SATELLITE);
                updateConnectionTimers(true /* forced log */);
                RGB.control(true);
                RGB.color(0,255,0);
                transitionTo(AppState::AcquireLocation);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::CellularConnect:
        {
            if (cellularShouldSwitchToSatellite()) {
                transitionTo(AppState::SwitchToSatellite);
                break;
            }
            if (Particle.connected()) {
                transitionTo(AppState::CellularOnline);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::CellularOnline:
        {
            if (cellularShouldSwitchToSatellite()) {
                transitionTo(AppState::SwitchToSatellite);
                break;
            }
            if (Particle.connected() && (millis() - lastPublish > g_cfg.ltePublishIntervalS * 1000UL)) {
                // Refresh location for the LTE publish where a fix is available.
                if (satellite.getGNSSLocation(g_cfg.locGpsFixTimeoutS * 1000UL) == 0) {
                    auto p = satellite.lastPositionInfo();
                    pubLat = p.latitude;
                    pubLon = p.longitude;
                    pubAlt = p.altitude;
                    havePubLoc = true;
                }
                publishLocationData();
                lastPublish = millis();
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::AcquireLocation:
        {
            // Acquire the location once and program the modem's NTN location fix
            // before attempting registration.
            acquireAndSetLocationFix();
            transitionTo(AppState::SatelliteConnect);
            break;
        }

        // --------------------------------------------------------------------
        case AppState::SatelliteConnect:
        {
            if (onEntry()) {
                Log.info("SATELLITE BEGIN --------------------");
                if (satellite.begin() != SYSTEM_ERROR_NONE) {
                    Log.error("Error initializing Satellite radio");
                    RGB.color(255,0,0);
                    transitionTo(AppState::Fault);
                    break;
                }
                satellite.process();
                Log.info("SATELLITE CONNECT ---------------------");
                satellite.connect();
            }

            satellite.process();

            if (satelliteShouldSwitchToCellular()) {
                transitionTo(AppState::SwitchToCellular);
                break;
            }
            if (satellite.connected()) {
                transitionTo(AppState::SatelliteOnline);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::SatelliteOnline:
        {
            satellite.process();
            if (satellite.connected()) {
                RGB.color(0,255,255);
            }

            if (satelliteShouldSwitchToCellular()) {
                transitionTo(AppState::SwitchToCellular);
                break;
            }
            if (satellite.connected() && (millis() - lastPublish > g_cfg.ntnPublishIntervalS * 1000UL)) {
                // Publish using the location cached on NTN entry.
                publishLocationData();
                lastPublish = millis();
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::SwitchToSatellite:
        {
            Log.info("SWITCH to SATELLITE --------------------");
            // NOTE: Very important to disconnect both Cloud and Cellular before switching to Satellite
            Particle.disconnect();
            waitFor(Particle.disconnected, 60000);
            Cellular.disconnect();

            Log.info("RADIO SATELLITE --------------------");
            if (modem.radioEnable(RADIO_SATELLITE) == SYSTEM_ERROR_NONE) {
                updateConnectionTimers(true /* forced log */);
                RGB.control(true);
                RGB.color(0,255,0);
                transitionTo(AppState::AcquireLocation);
            } else {
                Log.error("Failed to enable Satellite radio");
                transitionTo(AppState::Fault);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::SwitchToCellular:
        {
            Log.info("SWITCH to CELLULAR --------------------");
            // NOTE: Very important to disconnect Satellite before switching to Cellular
            satellite.disconnect();
            satellite.process();
            RGB.control(false);

            Log.info("RADIO CELLULAR --------------------");
            if (modem.radioEnable(RADIO_CELLULAR) == SYSTEM_ERROR_NONE) {
                updateConnectionTimers(true /* forced log */);
                Log.info("CELLULAR CONNECT ---------------------");
                Particle.connect();
                transitionTo(AppState::CellularConnect);
            } else {
                Log.error("Failed to enable Cellular radio");
                transitionTo(AppState::Fault);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::Fault:
        {
            // Placeholder for recovery / modem-reset escalation (future work).
            // For now, surface the fault and re-initialise from Boot.
            Log.error("FAULT state - re-initialising");
            RGB.control(true);
            RGB.color(255,0,0);
            delay(5000);
            transitionTo(AppState::Boot);
            break;
        }

        default:
            transitionTo(AppState::Fault);
            break;
    }
}
