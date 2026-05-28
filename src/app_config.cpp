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

#include "app_config.h"

#include <vector>

namespace {
Logger cfgLog("app.cfg");

constexpr const char* kAssetName = "app_config.json";

// Apply `value` to `dest` only if the variant carries a value of the right
// kind. Numeric coercion follows JSON's loose semantics (a JSON integer is
// accepted for a double field, but a string is not).
void applyBool(const Variant& v, bool& dest, const char* key) {
    if (v.isBool()) {
        dest = v.toBool();
    } else if (!v.isNull()) {
        cfgLog.warn("config field '%s' is not a bool; using default", key);
    }
}

void applyU32(const Variant& v, uint32_t& dest, const char* key) {
    if (v.isNumber()) {
        // toInt() returns int; for the values we use here (intervals/timeouts
        // up to ~hours of ms) the JSON number is always well under INT32_MAX.
        auto n = v.toInt();
        if (n < 0) {
            cfgLog.warn("config field '%s' is negative; using default", key);
            return;
        }
        dest = static_cast<uint32_t>(n);
    } else if (!v.isNull()) {
        cfgLog.warn("config field '%s' is not a number; using default", key);
    }
}

void applyDouble(const Variant& v, double& dest, const char* key) {
    if (v.isNumber()) {
        dest = v.toDouble();
    } else if (!v.isNull()) {
        cfgLog.warn("config field '%s' is not a number; using default", key);
    }
}

void applyLocSource(const Variant& v, LocSource& dest, const char* key) {
    if (v.isString()) {
        String s = v.toString();
        if (s.equalsIgnoreCase("fixed")) {
            dest = LocSource::Fixed;
        } else if (s.equalsIgnoreCase("dynamic")) {
            dest = LocSource::Dynamic;
        } else {
            cfgLog.warn("config field '%s'='%s' unrecognised; using default", key, s.c_str());
        }
    } else if (!v.isNull()) {
        cfgLog.warn("config field '%s' is not a string; using default", key);
    }
}

const char* locSourceName(LocSource s) {
    switch (s) {
        case LocSource::Fixed:   return "fixed";
        case LocSource::Dynamic: return "dynamic";
    }
    return "?";
}

// Find the bundled asset by name. Returns an empty (invalid) asset if not
// present.
ApplicationAsset findAsset(const char* name) {
    auto assets = System.assetsAvailable();
    for (auto& a : assets) {
        if (a.name() == name) {
            return a;
        }
    }
    return ApplicationAsset();
}

// Read the full asset body into a null-terminated buffer. Returns empty
// String on any error.
String readAssetText(ApplicationAsset& asset) {
    const size_t sz = asset.size();
    if (sz == 0) {
        return String();
    }
    std::vector<char> buf(sz + 1, '\0');
    asset.reset();
    int n = asset.read(buf.data(), sz);
    if (n < 0 || static_cast<size_t>(n) != sz) {
        cfgLog.warn("asset '%s' short read: got %d of %u bytes", asset.name().c_str(), n, (unsigned)sz);
        return String();
    }
    return String(buf.data());
}
} // namespace


// Defaults so the device boots correctly even if the asset is absent or malformed.
AppConfig g_cfg = {
    /* lteEnabled                     */ true,
    /* ntnEnabled                     */ true,
    /* startOnCellular                */ false,
    /* ltePublishIntervalS            */ 60,
    /* ntnPublishIntervalS            */ 5 * 60,
    /* cellularDisconnectedTimeoutS   */ 10 * 60,
    /* satelliteConnectedTimeoutS     */ 10 * 60,
    /* satelliteDisconnectedTimeoutS  */ 60,
    /* forceCellularToSatelliteSwitch */ false,
    /* forceSatelliteToCellularSwitch */ false,
    /* forceC2sSwitchTimeoutS         */ 10 * 60,
    /* forceS2cSwitchTimeoutS         */ 10 * 60,
    /* locSource                      */ LocSource::Fixed,
    /* locGpsFixTimeoutS              */ 60,
    /* locFixedLatitude               */ 38.07315,
    /* locFixedLongitude              */ -122.16545,
    /* locFixedAltitude               */ 111.8,
};


void loadAppConfig() {
    auto asset = findAsset(kAssetName);
    if (!asset.isValid()) {
        cfgLog.warn("asset '%s' not present; using compiled defaults", kAssetName);
    } else {
        String json = readAssetText(asset);
        if (json.length() == 0) {
            cfgLog.warn("asset '%s' empty/unreadable; using compiled defaults", kAssetName);
        } else {
            auto v = Variant::fromJSON(json.c_str());
            if (!v.isMap()) {
                cfgLog.warn("asset '%s' is not a JSON object; using compiled defaults", kAssetName);
            } else {
                cfgLog.info("loading config from asset '%s' (%u bytes)", kAssetName, (unsigned)asset.size());
                applyBool  (v.get("feature_lte_enabled"),                g_cfg.lteEnabled,                     "feature_lte_enabled");
                applyBool  (v.get("feature_ntn_enabled"),                g_cfg.ntnEnabled,                     "feature_ntn_enabled");
                applyBool  (v.get("start_on_cellular"),                  g_cfg.startOnCellular,                "start_on_cellular");
                applyU32   (v.get("lte_publish_interval_s"),             g_cfg.ltePublishIntervalS,            "lte_publish_interval_s");
                applyU32   (v.get("ntn_publish_interval_s"),             g_cfg.ntnPublishIntervalS,            "ntn_publish_interval_s");
                applyU32   (v.get("cellular_disconnected_timeout_s"),    g_cfg.cellularDisconnectedTimeoutS,   "cellular_disconnected_timeout_s");
                applyU32   (v.get("satellite_connected_timeout_s"),      g_cfg.satelliteConnectedTimeoutS,     "satellite_connected_timeout_s");
                applyU32   (v.get("satellite_disconnected_timeout_s"),   g_cfg.satelliteDisconnectedTimeoutS,  "satellite_disconnected_timeout_s");
                applyBool  (v.get("force_cellular_to_satellite_switch"), g_cfg.forceCellularToSatelliteSwitch, "force_cellular_to_satellite_switch");
                applyBool  (v.get("force_satellite_to_cellular_switch"), g_cfg.forceSatelliteToCellularSwitch, "force_satellite_to_cellular_switch");
                applyU32   (v.get("force_c2s_switch_timeout_s"),         g_cfg.forceC2sSwitchTimeoutS,         "force_c2s_switch_timeout_s");
                applyU32   (v.get("force_s2c_switch_timeout_s"),         g_cfg.forceS2cSwitchTimeoutS,         "force_s2c_switch_timeout_s");
                applyLocSource(v.get("loc_source"),                      g_cfg.locSource,                      "loc_source");
                applyU32   (v.get("loc_gps_fix_timeout_s"),              g_cfg.locGpsFixTimeoutS,              "loc_gps_fix_timeout_s");
                applyDouble(v.get("loc_fixed_latitude"),                 g_cfg.locFixedLatitude,               "loc_fixed_latitude");
                applyDouble(v.get("loc_fixed_longitude"),                g_cfg.locFixedLongitude,              "loc_fixed_longitude");
                applyDouble(v.get("loc_fixed_altitude"),                 g_cfg.locFixedAltitude,               "loc_fixed_altitude");
            }
        }
    }

    // At least one stack must be enabled.
    if (!g_cfg.lteEnabled && !g_cfg.ntnEnabled) {
        cfgLog.error("both LTE and NTN disabled; re-enabling LTE to keep the device usable");
        g_cfg.lteEnabled = true;
    }

    cfgLog.info("App config:");
    cfgLog.info("  lteEnabled=%s ntnEnabled=%s startOnCellular=%s",
        g_cfg.lteEnabled ? "true" : "false",
        g_cfg.ntnEnabled ? "true" : "false",
        g_cfg.startOnCellular ? "true" : "false");
    cfgLog.info("  publish: lte=%lus ntn=%lus",
        (unsigned long)g_cfg.ltePublishIntervalS,
        (unsigned long)g_cfg.ntnPublishIntervalS);
    cfgLog.info("  switch timeouts: cellDis=%lus satCon=%lus satDis=%lus",
        (unsigned long)g_cfg.cellularDisconnectedTimeoutS,
        (unsigned long)g_cfg.satelliteConnectedTimeoutS,
        (unsigned long)g_cfg.satelliteDisconnectedTimeoutS);
    cfgLog.info("  force: c2s=%s(%lus) s2c=%s(%lus)",
        g_cfg.forceCellularToSatelliteSwitch ? "true" : "false",
        (unsigned long)g_cfg.forceC2sSwitchTimeoutS,
        g_cfg.forceSatelliteToCellularSwitch ? "true" : "false",
        (unsigned long)g_cfg.forceS2cSwitchTimeoutS);
    cfgLog.info("  loc: source=%s timeout=%lus fixed=(%f, %f, %f)",
        locSourceName(g_cfg.locSource),
        (unsigned long)g_cfg.locGpsFixTimeoutS,
        g_cfg.locFixedLatitude, g_cfg.locFixedLongitude, g_cfg.locFixedAltitude);
}
