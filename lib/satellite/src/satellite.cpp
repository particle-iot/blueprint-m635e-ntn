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

#include "satellite.h"
#include "modem_manager.h"

#include "logging.h"
LOG_SOURCE_CATEGORY("ncp.client");

#include "check.h"
#include "scope_guard.h"
#include "stream_util.h"
#include "hex_to_bytes.h"

#include <str_util.h>

#include <memory>
#include <cstdint>
#include <pb_encode.h>
#include <cloud/cloud_new.pb.h>

#define USE_NON_IP 0
#define UDP_CONNECT_ID 0

static const size_t kUdpRxBufferSize = 320;

// Canonical on-wire datagram cap for outbound frames on both transports: the
// modem's AT-command body limit (256 raw bytes = 512 hex chars on the QISENDEX
// line).
static const size_t kMaxWireDatagramBytes = 256;

namespace particle {

using namespace constrained;

namespace {

#define SATELLITE_NCP_RX_DATA_READ_TIMEOUT_MS (3000)
#define SATELLITE_NCP_REGISTRATION_UPDATE_SLOW_MS (60000)
#define SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS (5000)
#define SATELLITE_NCP_RECEIVE_UPDATE_MS (10000)

#define SATELLITE_NCP_SERVINGCELL_UPDATE_MS (5000)

#define SATELLITE_NCP_NO_REGISTRATION_MS (300000)

#define SATELLITE_NCP_COMM_ERRORS_MAX (3)
#define SATELLITE_NCP_SOCKET_REBUILD_MS (30000)
// AT+QISTATE <socket_state>: 0 Initial, 1 Opening, 2 Connected, 3 Listening,
// 4 Closing. Only Connected is usable.
#define SATELLITE_NCP_SOCKET_CONNECTED (2)
#define SATELLITE_NCP_SOCKET_NONE (-1)
#define SATELLITE_NCP_SOCKET_UNKNOWN (-2)
#define SATELLITE_NCP_SOCKET_CONFIRM_TRIES (4)
#define SATELLITE_NCP_SOCKET_CONFIRM_MS (500)

#define SATELLITE_NCP_COPS_TIMEOUT_MS (180000)

} // namespace annonymous

Satellite::Satellite() : 
    begun_(false), 
    nwConnectionDesired_(NW_STATE_IDLE),
    registrationUpdateMs_(SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS) 
{

}

Satellite::~Satellite() {
}

int Satellite::cbCFUN(int type, const char* buf, int len, int* cfun)
{
    if ((type == TYPE_PLUS) && cfun) {
        if (sscanf(buf, "\r\n+CFUN: %d", cfun) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int Satellite::cbCOPS(int type, const char* buf, int len, char* network)
{
    if ((type == TYPE_PLUS) && network) {
        if (sscanf(buf, "\r\n+COPS: %*d,%*d,\"%31[^\"]\r\n", network) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int Satellite::cbQCFGEXTquery(int type, const char* buf, int len, int* rxlen)
{
    if ((type == TYPE_PLUS) && rxlen) {
        if (sscanf(buf, "\r\n+QCFGEXT: \"nipdr\",%*d,%*d,%d\r\n", rxlen) == 1)
            /*nothing*/;
    }
    return WAIT;
}

// +QIACT: <contextID>,<context_state>,<context_type>,<IP_address>
//   +QIACT: 1,1,1,"10.64.1.23"
//
// <context_state>: 0 deactivated, 1 activated. A modem with no active PDP
// context answers AT+QIACT? with a bare OK and no +QIACT: line
int Satellite::cbQIACT(int type, const char* buf, int len, int* state)
{
    if ((type == TYPE_PLUS) && state) {
        int id = -1;
        int st = -1;
        if (sscanf(buf, "\r\n+QIACT: %d,%d", &id, &st) == 2) {
            if (id == 1) { // the one context this app activates
                *state = st;
            }
        }
    }
    return WAIT;
}

// +QISTATE: <connectID>,<service_type>,<IP_address>,<remote_port>,<local_port>,
//           <socket_state>,<context_ID>,<serverID>,<access_mode>,<AT_port>
//   +QISTATE: 0,"UDP","3.231.157.58",40000,0,2,1,0,0,"cmux1"
//
// <socket_state>: 0 Initial, 1 Opening, 2 Connected, 3 Listening, 4 Closing.
// Only 2 counts as usable. A modem that has been power-cycled answers
// AT+QISTATE? with a bare OK and no +QISTATE: line at all
int Satellite::cbQISTATE(int type, const char* buf, int len, int* state)
{
    if ((type == TYPE_PLUS) && state) {
        int id = -1;
        int st = -1;
        if (sscanf(buf, "\r\n+QISTATE: %d,\"%*[^\"]\",\"%*[^\"]\",%*d,%*d,%d",
                    &id, &st) == 2) {
            if (id == UDP_CONNECT_ID) {
                *state = st;
            }
        }
    }
    return WAIT;
}

int Satellite::cbQIRDquery(int type, const char* buf, int len, int* rxlen)
{
    //+QIRD: <total_receive_length>,<have_read_length>,<unread_length>
    if (rxlen) {
        sscanf(buf, "\r\n+QIRD: %*d,%*d,%d\r\n", rxlen);
    }
    return WAIT;
}

int Satellite::cbQIRD(int type, const char* buf, int len, char* outBuf) {
  static int incomingPacketLength = 0;
  if (incomingPacketLength == 0) {
    sscanf(buf, "\r\n+QIRD: %d\r\n", &incomingPacketLength);
    if (incomingPacketLength > (int)kUdpRxBufferSize) {
      // Never let a large read (e.g. several queued downlinks) overrun the
      // caller's buffer; receiveData() caps its requests the same way.
      incomingPacketLength = kUdpRxBufferSize;
    }
  } else if(outBuf) {
    // Receive format is hex (QICFG "dataformat",0,1): the data line after the
    // leading "\r\n" is hex text, two chars per datagram byte. Copy and
    // NUL-terminate for hexToBytes() in receiveData().
    memcpy(outBuf, &buf[2], incomingPacketLength * 2);
    outBuf[incomingPacketLength * 2] = '\0';
    incomingPacketLength = 0;
  }

  return WAIT;
}

int Satellite::cbQISENDEX(int type, const char* buf, int len, int* param)
{
    if (strstr(buf, "SEND OK")) {
        return RESP_OK;
    }
    // QISENDEX reports a failed send as "SEND FAIL"; the modem may also emit a
    // bare "ERROR". Return RESP_ERROR so the command fails fast and the caller
    // can retry instead of waiting out the full timeout.
    if (strstr(buf, "SEND FAIL") || (type & TYPE_ERROR)) {
        return RESP_ERROR;
    }
    return WAIT;
}

int Satellite::cbQCFGEXTread(int type, const char* buf, int len, char* rxdata)
{
    if ((type == TYPE_PLUS) && rxdata) {
        if (sscanf(buf, "\r\n+QCFGEXT: \"nipdr\",%*d,%s\r\n", rxdata) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int Satellite::cbQGPSLOC(int type, const char* buf, int len, GnssPositioningInfo* info)
{
    if ((type == TYPE_PLUS) && info) {
        if (sscanf(buf, "\r\n+QGPSLOC: %02d%02d%02d.%*03d,%lf,%lf,%f,%f,%d,%f,%f,%f,%02d%02d%02d,%d\r\n",
                        &info->utcTime.tm_hour, &info->utcTime.tm_min, &info->utcTime.tm_sec,
                        &info->latitude, &info->longitude, &info->accuracy, &info->altitude,
                        &info->posMode, &info->cog, &info->speedKmph, &info->speedKnots,
                        &info->utcTime.tm_mday, &info->utcTime.tm_mon, &info->utcTime.tm_year,
                        &info->satsInView) == 15) {
            info->valid = 1;
        }
    }
    return WAIT;
}

int Satellite::cbQENG(int type, const char* buf, int len, NtnServingCellInfo* info)
{
    if ((type != TYPE_PLUS) || !info) {
        return WAIT;
    }
    NtnServingCellInfo p;
    // state, rat, duplex are quoted; cellId and tac are hex; the rest decimal.
    // Example responses:
    // +QENG: "servingcell","SEARCH"
    // +QENG: "servingcell","LIMSRV","NTN NBIoT","FDD",901,98,DC379,9,7685,23,0,0,7D9,-123,-14,-108,85,17
    // +QENG: "servingcell","CONNECT","NTN NBIoT","FDD",901,98,DC379,9,7685,23,0,0,7D9,-126,-18,-107,75,
    // +QENG: "servingcell","NOCONN","NTN NBIoT","FDD",901,98,2C480D,29,7689,23,0,0,7ED,-117,-13,-103,102,23
    int n = sscanf(buf,
            "\r\n+QENG: \"servingcell\",\"%11[^\"]\",\"%15[^\"]\",\"%7[^\"]\",%d,%d,%x,%d,%d,%d,%d,%d,%x,%d,%d,%d,%d,%d",
            p.state, p.rat, p.duplex, &p.mcc, &p.mnc, &p.cellId, &p.pcid, &p.earfcn,
            &p.band, &p.ulBandwidth, &p.dlBandwidth, &p.tac, &p.rsrp, &p.rsrq,
            &p.rssi, &p.sinr, &p.srxlev);
    if (n >= 1) {
        // Full signal metrics require everything up to SINR (16 fields);
        // srxlev (the 17th) is often empty. A bare state (e.g. SEARCH) gives n==1.
        p.valid = (n >= 16);
        *info = p;
    }
    return WAIT;
}

// 0000307909 [ncp.at] TRACE: > AT+QNWCFG="ntn_locfix"
// 0000307925 [ncp.at] TRACE: < +QNWCFG: "ntn_locfix",1,38.073146,-122.165430,111
// 0000307946 [ncp.at] TRACE: < OK
int Satellite::cbQNWCFGNTNLOCFIX(int type, const char* buf, int len, GnssPositioningInfo* info)
{
    if ((type == TYPE_PLUS) && info) {
        if (sscanf(buf, "\r\n+QNWCFG: \"ntn_locfix\",%d,%lf,%lf,%f\r\n",
                        &info->posMode,
                        &info->latitude, &info->longitude, &info->altitude) == 4) {
            info->valid = 1;
            Log.info("Current NTN location fix: mode=%d, lat=%f, lon=%f, alt=%f",
                info->posMode, info->latitude, info->longitude, info->altitude);
        }
    }
    return WAIT;
}

// True if the modem's current ntn_locfix already matches our desired fixed fix within reason.
bool Satellite::locFixMatches(const GnssPositioningInfo& cur) const {
    constexpr double kCoordEps = 1e-4;
    return cur.valid && cur.posMode == 1
        && fabs(cur.latitude  - locLat_) < kCoordEps
        && fabs(cur.longitude - locLon_) < kCoordEps
        && lround(cur.altitude) == lround(locAlt_);
}

// Terrestrial (LTE) registration, for reporting only - it does NOT gate the
// NTN connect path. NTN registration comes from AT+QENG="servingcell"; 
// the two genuinely disagree, and each is authoritative
// for its own radio.
//
// AT+CEREG? cannot be read back by the application: Device OS consumes the
// +CEREG: response for its own registration tracking, so an application
// callback never sees it and <stat> stays unset. It is still issued here so
// each poll lands in the AT trace, which is what a field log is read for.
//
// AT+COPS? does reach the application intact. The modem populates <oper> only
// once it has registered:
//   +COPS: 0                 - not registered
//   +COPS: 0,0,"901 98",14   - registered to 901 98
int Satellite::queryCellularRegistration() {
    cellularOperator_[0] = '\0';

    Cellular.command(2000, "AT+CEREG?"); // trace only, see above
    if ((RESP_OK == Cellular.command(cbCOPS, cellularOperator_, 10000, "AT+COPS?"))
            && (cellularOperator_[0] != '\0')) {
        return 1;
    }
    cellularOperator_[0] = '\0'; // partial parse on a failed command
    return 0;
}

// AT+QENG="servingcell" <state> values:
//   SEARCH  - no cell found yet, not on the network
//   LIMSRV  - camped on a cell, limited service
//   NOCONN  - camped and registered, idle mode (no active bearer)
//   CONNECT - camped and registered, active call/data in progress
//
// SEARCH is the only state that counts as unregistered: it is the one state
// where the modem holds no cell at all. LIMSRV means a cell has been found and
// acquisition is progressing, so it is treated as registered.
static bool ntnRegistered(const char* state) {
    return state[0] && strcmp(state, "SEARCH") != 0;
}

// Query and parse the serving-cell report into servingCell_.
// This is the source of truth for NTN registration (via ntnRegistered() on
// <state>) and also carries the signal metrics the status line prints
int Satellite::queryServingCell() {
    lastServingCellCheck_ = millis();
    servingCell_ = NtnServingCellInfo{};
    Cellular.command(cbQENG, &servingCell_, 2000, "AT+QENG=\"servingcell\"");

    return servingCell_.state[0] ? 0 : -1;
}

// Poll the modem with bare AT until it answers. Returns SYSTEM_ERROR_NONE once
// it does, SYSTEM_ERROR_TIMEOUT after `tries` silent attempts, or the AT layer's
// error for a hard failure.
int Satellite::waitAtResponse(unsigned int tries, unsigned int timeout) {
    unsigned int attempt = 0;
    for (;;) {
        const int r = Cellular.command(timeout, "AT");
        if (r == RESP_OK) {
            return SYSTEM_ERROR_NONE;
        }
        if (r != WAIT) {
            // A real answer that is not OK (RESP_ERROR and friends) - retrying
            // will not change it.
            return (r < 0) ? r : SYSTEM_ERROR_UNKNOWN;
        }
        if (++attempt >= tries) {
            break;
        }
    }
    return SYSTEM_ERROR_TIMEOUT;
}

int Satellite::begin() {
    begun_ = true;
    errorCount_ = 0;

    // assume we need to reconnect
    ntnInit_ = 0;
    ntnConnected_ = 0;
    socketSuspect_ = false;
    lastSocketRebuild_ = 0; // let the first rebuild attempt run immediately

    if (!Cellular.isOn() || Cellular.isOff()) {
        // Turn on the modem
        Cellular.on();
        if (!waitFor(Cellular.isOn, 60000)) {
            return SYSTEM_ERROR_TIMEOUT;
        }
    }

    // Ensure cellular is set to disconnected, otherwise when we issue AT+CFUN=0
    // or other +C*REG: URCs pop up, Device OS may try to start up a PPP connection
    Cellular.disconnect();
    if (!waitForNot(Cellular.ready, 60000)) {
        Log.info("Timeout, Cellular is still connected for over 60s!");
        return SYSTEM_ERROR_TIMEOUT;
    }

    waitAtResponse(10); // Check if the module is alive

    Cellular.command(2000, "AT+QGMR");

    ModemManager::sendTerminalCapability();

    // Check if ntn_locfix needs to be set or unset
    auto resetModem = false;
    GnssPositioningInfo locFixSetting = {};
    Cellular.command(cbQNWCFGNTNLOCFIX, &locFixSetting, 2000, "AT+QNWCFG=\"ntn_locfix\"");

    if (locForceFixed_) {
        if (!locFixMatches(locFixSetting)) {
            Log.info("Programming NTN location fix: %f,%f,%d", locLat_, locLon_,  (int)lround(locAlt_));
            Cellular.command(2000, "AT+QNWCFG=\"ntn_locfix\",1,%f,%f,%d", locLat_, locLon_,  (int)lround(locAlt_));
            resetModem = true;
        }
    } else {
        // If ntn is set and we need to unset it, do that and reset
        if (locFixSetting.valid && (locFixSetting.posMode == 1)) {
            Log.warn("Clearing NTN location fix");
            Cellular.command(2000, "AT+QNWCFG=\"ntn_locfix\",0");
            resetModem = true;
        } else if (!locFixSetting.valid) {
            Log.warn("No NTN location fix programmed; NTN registration may fail");
        }
    }

    if (resetModem) {
        Cellular.off();
        Cellular.on();
        if (!waitFor(Cellular.isOn, 60000)) {
            return SYSTEM_ERROR_TIMEOUT;
        }

        waitAtResponse(10);
        ModemManager::sendTerminalCapability();
        // Read back settings after reset
        Cellular.command(2000, "AT+QNWCFG=\"ntn_locfix\"");
    }

    Cellular.command(2000, "AT+QCFG=\"band\"");
    Cellular.command(2000, "AT+CEREG=2");
    Cellular.command(2000, "AT+CEREG?");
    Cellular.command(2000, "AT+COPS=3,0");

    queryCellularRegistration();
    queryServingCell();
    if (ntnRegistered(servingCell_.state)) {
        registered_ = 1;
        Log.info("SKIPPING THE FOLLOWING COMMANDS:\n"
            "\"AT+CFUN=0\"\n"
            "\"AT+CGDCONT=1,\"IP\",\"360Connect\"\n"
            "\"AT+QCFG=\"nwscanmode\",3,1\n"
            "\"AT+QCFG=\"iotopmode\",3,1\n"
            "\"AT+CFUN=1\n");
    } else {
        Cellular.command(180000, "AT+CFUN=0");
        Cellular.command(2000, "AT+CGDCONT=1,\"IP\",\"360Connect\"");
        Cellular.command(2000, "AT+QCFG=\"nwscanmode\",3,1"); // LTE (includes NTN)
        Cellular.command(2000, "AT+QCFG=\"iotopmode\",3,1");  // NTN only
        Cellular.command(180000, "AT+CFUN=1");
        ModemManager::sendTerminalCapability();
    }

    return initProtocolStack();
}

// Protocol stack + secure-UDP session init shared by both transports (NTN
// begin() and beginCellularTransport()). Safe to call repeatedly:
// CloudProtocol::init() is a no-op once initialized, and the secure session is
// only (re)derived when not ready - each re-derivation burns up to a counter
// stride, so it must not run again on every radio switch.
int Satellite::initProtocolStack() {
    Log.trace("Initializing protocol handler");
    CloudProtocolConfig protoConf;
    protoConf.onSend([this](auto data, auto port, auto /* onAck */) {
        return tx((const uint8_t*)data.data(), data.size(), port);
    });

    // The configured cap is the ON-WIRE datagram limit. Clamp out-of-range
    // values to the transport maximum rather than admitting frames the modem
    // cannot carry (too high) or that no frame can ever fit (too low).
    size_t minWireCap = 1;
#if SECURE_UDP_ENABLED
    static_assert(secure_udp::kUplinkOverheadBytes < kMaxWireDatagramBytes,
            "secure overhead must leave room for a payload");
    minWireCap = secure_udp::kUplinkOverheadBytes + 1;
#endif
    if (maxPayloadSize_ < minWireCap || maxPayloadSize_ > kMaxWireDatagramBytes) {
        Log.warn("Max payload size %u out of range; clamping to %u",
            (unsigned)maxPayloadSize_, (unsigned)kMaxWireDatagramBytes);
        maxPayloadSize_ = kMaxWireDatagramBytes;
    }
    size_t maxProtoFrame = maxPayloadSize_;
#if SECURE_UDP_ENABLED
    // The secure frame wraps the protocol frame in kUplinkOverheadBytes of
    // KeyId/CounterLow/Tag, so the protocol layer only gets the remainder —
    // otherwise a cap-sized frame would leave tx() as cap + overhead bytes.
    maxProtoFrame -= secure_udp::kUplinkOverheadBytes;
#endif
    protoConf.maxPayloadSize(maxProtoFrame);
    int r = proto_.init(protoConf);
    if (r < 0) {
        Log.error("CloudProtocol::init() failed: %d", r);
        return r;
    }

#if SECURE_UDP_ENABLED
    // Derive the per-device keys (DCT private key + pinned cloud key) and load
    // counter watermarks. On failure (e.g. Device Protection on, §7.1) there is
    // no fallback to unauthenticated frames — fail here so begin() /
    // beginCellularTransport() report the fault instead of coming up "online"
    // with an uplink that can never send.
    if (!secureUdp_.ready() && !secureUdp_.init(secureUdpStore_)) {
        Log.error("Secure UDP init failed (device key unreadable? Device Protection on?)");
        return SYSTEM_ERROR_INVALID_STATE;
    }
    Log.info("Secure UDP session ready");
#endif

    return 0;
}

void Satellite::setRawMode(bool enabled, RawRxHandler onRx) {
    rawMode_ = enabled;
    rawRxHandler_ = std::move(onRx);
    if (enabled) {
        Log.warn("NTN RAW PASSTHROUGH enabled: datagrams bypass the constrained "
                 "protocol and secure UDP in both directions");
    }
}

void Satellite::setEndpoint(const IPAddress& ip, uint16_t port) {
    endpointIp_ = ip;
    endpointPort_ = port;
    Log.info("UDP endpoint set to %u.%u.%u.%u:%u",
        (unsigned)endpointIp_[0], (unsigned)endpointIp_[1],
        (unsigned)endpointIp_[2], (unsigned)endpointIp_[3],
        (unsigned)endpointPort_);
}

int Satellite::beginCellularTransport() {
    if (cellularTransportActive()) {
        return 0;
    }

    // No modem AT work here: on the cellular/WiFi profile Device OS owns the
    // modem, so the datagram transport is a Device OS UDP socket riding the
    // active network interface. The caller must have the network up
    // (Particle.connected()) before starting.
    // initProtocolStack() fails when the secure session cannot initialize —
    // there is no fallback to unauthenticated frames or Particle.publish; the
    // publisher stats surface the dropped publishes.
    int r = initProtocolStack();
    if (r < 0) {
        return r;
    }

    udp_.setBuffer(kUdpRxBufferSize);
    if (!udp_.begin(endpointPort_)) {
        Log.error("UDP begin on port %u failed", (unsigned)endpointPort_);
        return SYSTEM_ERROR_NETWORK;
    }
    transportMode_ = TransportMode::DEVICEOS_UDP;
    udpStarted_ = true;
    lastUdpReceiveCheck_ = 0;
    proto_.connect();
    Log.info("Constrained protocol over Device OS UDP started (dst %u.%u.%u.%u:%u, local port %u)",
        (unsigned)endpointIp_[0], (unsigned)endpointIp_[1],
        (unsigned)endpointIp_[2], (unsigned)endpointIp_[3],
        (unsigned)endpointPort_, (unsigned)endpointPort_);
    return 0;
}

void Satellite::endCellularTransport() {
    if (!udpStarted_) {
        return;
    }
    udp_.stop();
    udpStarted_ = false;
    transportMode_ = TransportMode::NTN_AT_SOCKET;
    // Reset the channel so pending out-requests whose ACKs can no longer be
    // routed don't linger; the NTN path re-connects the protocol later in
    // connectImpl().
    proto_.disconnect();
    Log.info("Constrained protocol over Device OS UDP stopped");
}

int Satellite::processCellularTransport() {
    if (!cellularTransportActive()) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    // Same receive poll cadence as the NTN path - timing parity is the point
    // of running the constrained protocol over this transport.
    if (millis() - lastUdpReceiveCheck_ >= SATELLITE_NCP_RECEIVE_UPDATE_MS) {
        lastUdpReceiveCheck_ = millis();
        // Drain everything waiting: the cloud can release several queued
        // downlinks between polls (one per verified uplink).
        int n = 0;
        while ((n = udp_.parsePacket()) > 0) {
            char rxData[320] = "";
            int len = udp_.read((unsigned char*)rxData, sizeof(rxData));
            if (len > 0) {
                Log.info("Bytes Read %d", len);
                handleInboundDatagram(rxData, (size_t)len);
            }
        }
    }
    proto_.run();
    return 0;
}

int Satellite::connect() {
    nwConnectionDesired_ = NW_STATE_CONNECT;
    nwConnected_ = NW_CONNECTED_INIT;
    return 0;
}

// Returns the raw <socket_state>, SATELLITE_NCP_SOCKET_NONE when the modem
// answers but reports no socket for UDP_CONNECT_ID (a bare OK, which is what a
// power cycle leaves behind), or SATELLITE_NCP_SOCKET_UNKNOWN when the query
// itself failed or timed out.
int Satellite::querySocketState() {
    int state = SATELLITE_NCP_SOCKET_NONE;
    if (RESP_OK != Cellular.command(cbQISTATE, &state, 2000, "AT+QISTATE?")) {
        return SATELLITE_NCP_SOCKET_UNKNOWN;
    }
    return state;
}

void Satellite::noteSocketLost(const char* why) {
    socketSuspect_ = false;
    if (!ntnInit_ && !ntnConnected_ && nwConnected_ != NW_CONNECTED_SUCCESS) {
        return; // already torn down, nothing to announce
    }
    Log.warn("NTN socket lost (%s); rebuilding data session", why);
    proto_.disconnect();
    ntnInit_ = 0;
    ntnConnected_ = 0;
    nwConnected_ = NW_CONNECTED_INIT;
}

bool Satellite::socketRebuildAllowed() {
    return !lastSocketRebuild_ ||
            (millis() - lastSocketRebuild_ >= SATELLITE_NCP_SOCKET_REBUILD_MS);
}

// Build (or rebuild) the modem-side data session: PDP context, the volatile hex
// receive mode, and the UDP socket. Sets ntnInit_ on success.
int Satellite::openDataSession() {
    lastSocketRebuild_ = millis();
    socketSuspect_ = false;

    // Device OS may be mid power-cycle. Confirm the modem answers at all 
    // before committing to QIACT and QIOPEN
    if (waitAtResponse(2, 1000) != SYSTEM_ERROR_NONE) {
        Log.warn("Modem not responding; deferring NTN data-session rebuild");
        ntnInit_ = 0;
        return -1;
    }

    int r = 0;
#if USE_NON_IP
    r = Cellular.command(2000, "AT+QCFGEXT=\"nipdcfg\",0,\"particle.io\"");
    if (r == RESP_OK) {
        r = Cellular.command(2000, "AT+QCFGEXT=\"nipdcfg\"");
    }
    if (r == RESP_OK) {
        r = Cellular.command(2000, "AT+QCFGEXT=\"nipd\",1,30");
        ntnInit_ = 1;
    } else {
        ntnInit_ = 0;
    }
#else
    // A socket left in a non-usable state (Opening/Listening/Closing) holds the
    // id and would make QIOPEN fail; close it first.
    const int sockState = querySocketState();
    if (sockState >= 0 && sockState != SATELLITE_NCP_SOCKET_CONNECTED) {
        Log.info("Closing stale NTN socket (state %d)", sockState);
        Cellular.command(2000, "AT+QICLOSE=%d", UDP_CONNECT_ID);
    }

    Cellular.command(2000, "AT+QICSGP=1");
    Cellular.command(2000, "AT+QIACT?");

    Cellular.command(2000, "AT+QICSGP=1,1,\"360Connect\"");
    r = Cellular.command(150 * 1000, "AT+QIACT=1");

    int actState = -1;
    Cellular.command(cbQIACT, &actState, 2000, "AT+QIACT?");
    if (r != RESP_OK || actState != 1) {
        Log.warn("PDP context not active (QIACT=%d, state=%d); deferring NTN socket open",
                r, actState);
        ntnInit_ = 0;
        return -1;
    }

    Cellular.command(2000, "AT+QICFG=\"dataformat\",0,1");

    Log.info("Opening NTN UDP socket to %u.%u.%u.%u:%u%s",
            (unsigned)endpointIp_[0], (unsigned)endpointIp_[1],
            (unsigned)endpointIp_[2], (unsigned)endpointIp_[3],
            (unsigned)endpointPort_, rawMode_ ? " (RAW passthrough)" : "");
    r = Cellular.command(150 * 1000, "AT+QIOPEN=1,%d,\"UDP\",\"%u.%u.%u.%u\",%u", UDP_CONNECT_ID,
            (unsigned)endpointIp_[0], (unsigned)endpointIp_[1],
            (unsigned)endpointIp_[2], (unsigned)endpointIp_[3],
            (unsigned)endpointPort_);

    if (r != RESP_OK) {
        Log.warn("QIOPEN rejected: %d", r);
        querySocketState();
        ntnInit_ = 0;
        return -1;
    }

    // QIOPEN's OK only means the request was accepted. The outcome comes back
    // asynchronously as "+QIOPEN: <id>,<err>", which is too late to gate on and
    // would otherwise be parsed as part of whatever command runs next. Ask the
    // modem what the socket actually is instead: QISTATE reporting Connected is
    // the only proof it is usable.
    unsigned int tries = SATELLITE_NCP_SOCKET_CONFIRM_TRIES;
    bool usable = false;
    for (;;) {
        if (querySocketState() == SATELLITE_NCP_SOCKET_CONNECTED) {
            usable = true;
            break;
        }
        if (--tries == 0) {
            break;
        }
        delay(SATELLITE_NCP_SOCKET_CONFIRM_MS);
    }

    ntnInit_ = usable ? 1 : 0;
    if (!ntnInit_) {
        Log.warn("QIOPEN did not yield a usable socket");
    }
#endif
    return (ntnInit_ ? 0 : -1);
}

int Satellite::connectImpl() {
    if (nwConnectionDesired_ != NW_STATE_CONNECT || connected()) {
        return 0;
    }
    if (!registered_) {
        return 0;
    }

    static uint32_t lastConnectAttempt;
    if (millis() - lastConnectAttempt <= 5000) {
        return 0;
    }
    lastConnectAttempt = millis();

    if (!ntnInit_) {
        if (!socketRebuildAllowed()) {
            return 0;
        }
        openDataSession();
    }

    if (ntnInit_) {
        // registered_ was checked on entry and is owned by updateRegistration()
        // (serving-cell <state>); the data session is up once ntnInit_ succeeded.
        ntnConnected_ = 1;
    }

    if (ntnConnected_) {
        int r = proto_.connect();
        if (r < 0) {
            Log.error("CloudProtocol::connect() failed: %d", r);
            Log.warn("Ensure Satellite::begin() is called before Satellite::connect()");
            nwConnected_ = NW_CONNECTED_FAILED;
            errorCount_++;
            return r;
        }
        Log.info("Connected to the Satellite");
        nwConnected_ = NW_CONNECTED_SUCCESS;
    }

    return 0;
}

int Satellite::disconnect() {
    proto_.disconnect();
    nwConnectionDesired_ = NW_STATE_DISCONNECT;
    nwConnected_ = NW_CONNECTED_INIT;
    ntnConnected_ = 0;
    ntnInit_ = 0;
    socketSuspect_ = false;
    registrationUpdateMs_ = SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS;
    registered_ = 0;

#if !USE_NON_IP
    Cellular.command(2000, "AT+QICLOSE=%d", UDP_CONNECT_ID);
    Cellular.command(2000, "AT+QIDEACT=1");
#endif

    Cellular.command(180000, "AT+CFUN=0"); // required to properly end NTN data session

    return 0;
}

bool Satellite::connected(void) {
    return (nwConnected_ == NW_CONNECTED_SUCCESS) && (nwConnectionDesired_ == NW_STATE_CONNECT);
}

// Sole owner of registration state. Polls registration on one timer, maintains
// registered_, handles reattach/detach, and recovers from prolonged loss of
// registration. connectImpl() consumes registered_ but never polls itself.
void Satellite::updateRegistration(bool force) {
    if (!force && millis() - lastRegistrationCheck_ < registrationUpdateMs_) {
        return;
    }
    lastRegistrationCheck_ = millis();

    queryServingCell();
    queryCellularRegistration();

    int r = servingCell_.state[0] ? ntnRegistered(servingCell_.state) : registered_;

    if (r) {
        noRegistrationTimer_ = 0;
        if (!registered_) {
            nwConnected_ = NW_CONNECTED_INIT;
            ntnConnected_ = 0;
        }
    } else {
        // Registration lost: tear the cloud session down so it is rebuilt on
        // reattach. nwConnectionDesired_ is left alone - the connection should
        // come back on its own once the serving cell comes back.
        if (nwConnected_ == NW_CONNECTED_SUCCESS) {
            proto_.disconnect();
        }
        nwConnected_ = NW_CONNECTED_INIT;
        ntnInit_ = 0; // in case we de-registered, make sure NTN is re-initialized
        ntnConnected_ = 0;
        socketSuspect_ = false;
        if (!noRegistrationTimer_) {
            noRegistrationTimer_ = millis();
        } else if (millis() - noRegistrationTimer_ > SATELLITE_NCP_NO_REGISTRATION_MS) {
            // Prolonged no-registration: kick the radio.
            Log.info("No registration for %d minutes, toggling CFUN.", SATELLITE_NCP_NO_REGISTRATION_MS / 60000);
            Cellular.command(180000, "AT+CFUN=0");
            Cellular.command(180000, "AT+CFUN=1");
            ModemManager::sendTerminalCapability();
            noRegistrationTimer_ = millis();
        }
    }

    registered_ = r;
    // Poll fast until connected, then back off.
    registrationUpdateMs_ = connected() ? SATELLITE_NCP_REGISTRATION_UPDATE_SLOW_MS
                                         : SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS;
}

void Satellite::receiveData(void) {
    // check for incoming data and update cloud protocol
    if (registered_ && connected() && millis() - lastReceivedCheck_ >= SATELLITE_NCP_RECEIVE_UPDATE_MS) {
        lastReceivedCheck_ = millis();
        int recv = 0;
        char rxData[kUdpRxBufferSize] = ""; // decoded datagram bytes
        int atResponse = 0;

#if USE_NON_IP
        atResponse = Cellular.command(cbQCFGEXTquery, &recv, 10000, "AT+QCFGEXT=\"nipdr\",0");
        if ((RESP_OK == atResponse) && (recv > 0)) {
            atResponse = Cellular.command(cbQCFGEXTread, rxData, 10000, "AT+QCFGEXT=\"nipdr\",%d,1", recv);
            if ((RESP_OK == atResponse) && recv) {
                Log.info("Bytes Read %d", recv);
                handleInboundDatagram(rxData, (size_t)recv);
            } else {
                Log.error("Error reading data!");
            }
        }
#else
        if (querySocketState() != SATELLITE_NCP_SOCKET_CONNECTED) {
            socketSuspect_ = true;
            return; // QIRD on a socket the modem does not have just ERRORs
        }

        atResponse = Cellular.command(cbQIRDquery, &recv, 60 * 1000, "AT+QIRD=%d,0", UDP_CONNECT_ID);
        if (RESP_OK != atResponse) {
            // The socket was there a moment ago; let process() settle it.
            socketSuspect_ = true;
        }
        if ((RESP_OK == atResponse) && (recv > 0)) {
            if (recv > (int)kUdpRxBufferSize) {
                // More buffered than one read carries (e.g. several queued
                // downlinks); read what fits, the rest is picked up on the
                // next poll. cbQIRD clamps the same way as a backstop.
                recv = kUdpRxBufferSize;
            }
            // Receive format is hex (QICFG "dataformat",0,1): the response
            // carries 2 chars per datagram byte; decode before dispatch.
            char hexData[kUdpRxBufferSize * 2 + 1] = "";
            atResponse = Cellular.command(cbQIRD, hexData, 10000, "AT+QIRD=%d,%d", UDP_CONNECT_ID, recv);
            if (RESP_OK == atResponse) {
                const size_t decoded = hexToBytes(hexData, rxData, sizeof(rxData));
                Log.info("Bytes Read %d", recv);
                if (decoded > 0) {
                    handleInboundDatagram(rxData, decoded);
                } else {
                    Log.error("Error decoding RX hex data!");
                }
            } else {
                Log.error("Error reading data!");
            }
        }
#endif
    }
}

// Verify + dispatch one inbound datagram; shared by the NTN AT read path and
// the Device OS UDP poll.
void Satellite::handleInboundDatagram(char* data, size_t len) {
    if (rawMode_) {
        char hexBuf[kUdpRxBufferSize * 2 + 1] = {};
        const size_t dumpLen = (len < kUdpRxBufferSize) ? len : kUdpRxBufferSize;
        toHex(data, dumpLen, hexBuf, sizeof(hexBuf));

        constexpr size_t kTextLogMax = 160;
        char text[kTextLogMax + 1] = {};
        const size_t textLen = (dumpLen < kTextLogMax) ? dumpLen : kTextLogMax;
        for (size_t i = 0; i < textLen; ++i) {
            const unsigned char c = (unsigned char)data[i];
            text[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        text[textLen] = '\0';

        Log.info("RAW RX: %u bytes: %s", (unsigned)len, text);
        Log.trace("%s", hexBuf);

        if (rawRxHandler_) {
            rawRxHandler_((const uint8_t*)data, len);
        }
        return;
    }
#if SECURE_UDP_ENABLED
    // Log the raw encrypted frame in the same hex format as tx(), so inbound
    // datagrams (echoes, dupes) can be matched to uplinks by frame counter.
    {
        char hexBuf[kUdpRxBufferSize * 2 + 1] = {};
        const size_t dumpLen = (len < kUdpRxBufferSize) ? len : kUdpRxBufferSize;
        auto hexLength = toHex(data, dumpLen, hexBuf, sizeof(hexBuf));
        (void) hexLength;
        Log.info("RX: %u bytes", (unsigned)len);
        Log.trace("%s", hexBuf);
        // FULL LOGGING
        //=============
        // LOG_DUMP(TRACE, hexBuf, dumpLen);
        // LOG_PRINTF(TRACE, "\r\n");
    }
    // Verify + strip the secure frame before handing the inner payload to the
    // protocol layer. Every non-Ok status drops the datagram (spec §6.2, §7.2),
    // but the modes are logged and counted separately: a storage fault after a
    // valid tag needs different remediation than an attack or replay noise.
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    const auto status = secureUdp_.verifyDownlink((const uint8_t*)data, len, payload, payloadLen);
    switch (status) {
        case secure_udp::Status::Ok: {
            auto dataBuf = util::Buffer((char*)payload, payloadLen);
            proto_.receive(dataBuf, 223);
            break;
        }
        case secure_udp::Status::Malformed:
            ++secureRxStats_.malformed;
            Log.warn("Secure UDP downlink malformed; dropping %u bytes", (unsigned)len);
            break;
        case secure_udp::Status::Replay:
            ++secureRxStats_.replay;
            Log.warn("Secure UDP downlink replayed/stale; dropping %u bytes", (unsigned)len);
            break;
        case secure_udp::Status::PersistFailed:
            ++secureRxStats_.persistFailed;
            Log.error("Secure UDP downlink authenticated but replay floor persist failed; dropping %u bytes",
                (unsigned)len);
            break;
        case secure_udp::Status::NotReady:
            ++secureRxStats_.notReady;
            Log.warn("Secure UDP downlink before session ready; dropping %u bytes", (unsigned)len);
            break;
        case secure_udp::Status::BadTag:
        case secure_udp::Status::CounterExhausted:
        default:
            ++secureRxStats_.badTag;
            Log.warn("Secure UDP downlink bad tag; dropping %u bytes", (unsigned)len);
            break;
    }
#else
    auto dataBuf = util::Buffer(data, len);
    LOG_DUMP(TRACE, dataBuf.data(), len);
    LOG_PRINTF(TRACE, "\r\n");
    proto_.receive(dataBuf, 223);
#endif
}

int Satellite::tx(const uint8_t* buf, size_t len, int port) {
    if (transportMode_ == TransportMode::DEVICEOS_UDP) {
        if (!udpStarted_ || !Particle.connected()) {
            return SYSTEM_ERROR_INVALID_STATE;
        }
    } else if (!registered_ || !connected()) {
        return SYSTEM_ERROR_INVALID_STATE;
    }

#if SECURE_UDP_ENABLED
    // Wrap the protocol payload as an authenticated uplink frame before the
    // existing hex-encode + QISENDEX. The secure layer is transparent to the
    // CloudProtocol caller (spec §5–§7). The scratch buffer is the wire cap:
    // anything the protocol layer admits (cap − overhead) fits after wrapping.
    uint8_t secureFrame[kMaxWireDatagramBytes];
    size_t frameLen = 0;
    const auto status = secureUdp_.protectUplink(buf, len, secureFrame, sizeof(secureFrame), frameLen);
    switch (status) {
        case secure_udp::Status::Ok:
            break;
        case secure_udp::Status::NotReady:
            Log.error("Secure UDP not ready; dropping %u-byte uplink", (unsigned)len);
            return SYSTEM_ERROR_INVALID_STATE;
        case secure_udp::Status::CounterExhausted:
            Log.error("Secure UDP uplink counter exhausted; dropping %u-byte uplink", (unsigned)len);
            return SYSTEM_ERROR_OUT_OF_RANGE;
        case secure_udp::Status::PersistFailed:
            Log.error("Secure UDP counter persistence failed; dropping %u-byte uplink", (unsigned)len);
            return SYSTEM_ERROR_FLASH_IO;
        case secure_udp::Status::TooLarge:
        default:
            Log.error("Secure UDP payload too large (payload=%u, max=%u; wire cap %u minus %u overhead)",
                (unsigned)len,
                (unsigned)(sizeof(secureFrame) - secure_udp::kUplinkOverheadBytes),
                (unsigned)sizeof(secureFrame),
                (unsigned)secure_udp::kUplinkOverheadBytes);
            return SYSTEM_ERROR_TOO_LARGE;
    }
    // Final wire-size gate: the protocol layer was sized to cap − overhead, so
    // this only catches drift between the two limits.
    if (frameLen > maxPayloadSize_) {
        Log.error("Secure frame %u bytes exceeds on-wire cap %u",
            (unsigned)frameLen, (unsigned)maxPayloadSize_);
        return SYSTEM_ERROR_TOO_LARGE;
    }
    buf = secureFrame;
    len = frameLen;
#endif

    return txBytes(buf, len);
}

int Satellite::txRaw(const uint8_t* buf, size_t len) {
    if (transportMode_ == TransportMode::DEVICEOS_UDP) {
        if (!udpStarted_ || !Particle.connected()) {
            return SYSTEM_ERROR_INVALID_STATE;
        }
    } else if (!registered_ || !connected()) {
        return SYSTEM_ERROR_INVALID_STATE;
    }

    if (maxPayloadSize_ && len > maxPayloadSize_) {
        Log.error("Raw payload %u bytes exceeds on-wire cap %u",
            (unsigned)len, (unsigned)maxPayloadSize_);
        return SYSTEM_ERROR_TOO_LARGE;
    }

    return txBytes(buf, len);
}

int Satellite::txBytes(const uint8_t* buf, size_t len) {
    if (transportMode_ == TransportMode::DEVICEOS_UDP) {
        // Raw datagram over the Device OS socket; no hex/AT framing. A send
        // failure here must NOT feed errorCount_ - that counter drives the
        // NTN modem (CFUN) recovery in processErrors(), and a UDP failure on
        // the normal connection must not queue a modem reset for the next
        // NTN session.
        int sent = udp_.sendPacket(buf, len, endpointIp_, endpointPort_);
        if (sent < (int)len) {
            Log.error("UDP sendPacket failed: %d (%u bytes)", sent, (unsigned)len);
            return -1;
        }
        Log.info("Bytes Sent %u", (unsigned)len);
        return 0;
    }

    auto hexBufSize = len * 2 + 1;
    std::unique_ptr<char[]> hexBuf(new(std::nothrow) char[hexBufSize]);
    if (!hexBuf) {
        return SYSTEM_ERROR_NO_MEMORY;
    }
    memset(hexBuf.get(), 0, hexBufSize);
    auto hexLength = toHex(buf, len, hexBuf.get(), hexBufSize);
    (void) hexLength;
    Log.info("TX: %d bytes", len);
    Log.trace("%s", (char*)hexBuf.get());
    // FULL LOGGING
    //=============
    // LOG_DUMP(TRACE, (char*)hexBuf.get(), len);
    // LOG_PRINTF(TRACE, "\r\n");

    constexpr int kMaxSendAttempts = 3;
#if USE_NON_IP
    auto r = Cellular.command(2000, "AT+QCFGEXT=\"nipds\",1,\"%s\",%d", hexBuf.get(), len);
#else
    int dummy;
    int r = RESP_ERROR;
    // At most one data-session rebuild per tx(): openDataSession() can spend
    // minutes in QIACT/QIOPEN, and the rebuild throttle does not bound a second
    // one because the first already outlasts SATELLITE_NCP_SOCKET_REBUILD_MS.
    bool rebuildTried = false;
    for (int attempt = 1; attempt <= kMaxSendAttempts; ++attempt) {
        r = Cellular.command(cbQISENDEX, &dummy, 2000, "AT+QISENDEX=%d,\"%s\",0", UDP_CONNECT_ID, hexBuf.get());
        if (r == RESP_OK) {
            break;
        }
        Log.warn("QISENDEX attempt %d/%d failed: %d", attempt, kMaxSendAttempts, r);
        if (attempt == kMaxSendAttempts) {
            break;
        }

        const int sockState = querySocketState();
        if (sockState == SATELLITE_NCP_SOCKET_UNKNOWN) {
            Log.warn("Socket state unknown after send failure; backing off");
            socketSuspect_ = true;
        } else if (sockState != SATELLITE_NCP_SOCKET_CONNECTED) {
            if (!rebuildTried && registered_ && socketRebuildAllowed()) {
                rebuildTried = true;
                if (openDataSession() == 0) {
                    Log.info("Rebuilt NTN socket mid-send; retrying");
                    continue; // straight to the retry, no backoff delay
                }
            }
            Log.warn("QISENDEX failed with no usable socket and no rebuild");
            socketSuspect_ = true;
            break;
        }

        delay(10000);
    }
#endif
    // Send hex data
    if (RESP_OK == r) {
        Log.info("Bytes Sent %d", len);
    } else {
        Log.error("Error sending after %d attempts: %d bytes: %d", kMaxSendAttempts, len, r);
        errorCount_++;
        return -1;
    }

    return 0;
}

int Satellite::getGNSSLocation(unsigned int maxFixWaitTimeMs) {
    // No GNSS antenna mode: return the coordinates supplied via setLocationFix()
    // without ever touching the GNSS engine.
    if (locForceFixed_ && locFixValid_) {
        (void)maxFixWaitTimeMs;
        GnssPositioningInfo info = {};
        info.latitude  = locLat_;
        info.longitude = locLon_;
        info.altitude  = locAlt_;
        info.valid = 1;
        lastPositionInfo_ = info;
        Log.info("Using fixed location: %.5lf, %.5lf, ALT:%.1f", info.latitude, info.longitude, info.altitude);
        return 0;
    }

    GnssPositioningInfo info = {};
    auto s = millis();
    Cellular.command(2000, "AT+QGPS=1");
    delay(5000);

    do {
        Cellular.command(cbQGPSLOC, &info, 2000, "AT+QGPSLOC=2");

        if (info.valid) {
            Log.info("GPS TIME: %02d/%02d/%02d %02d:%02d:%02d", info.utcTime.tm_year, info.utcTime.tm_mon,
                    info.utcTime.tm_mday, info.utcTime.tm_hour, info.utcTime.tm_min, info.utcTime.tm_sec);
            Log.info("LOCATION: %.5lf, %.5lf, ALT:%.1f SATS:%d\r\n", info.latitude, info.longitude,
                    info.altitude, info.satsInView);
        } else {
            delay(5000);
        }
    } while (!info.valid && millis() - s < maxFixWaitTimeMs);

    Cellular.command(2000, "AT+QGPSEND");

    if (info.valid) {
        lastPositionInfo_ = info;
    }
    return info.valid == 1 ? 0 : -1;
}

int Satellite::setLocationFix(double lat, double lon, double alt, bool forceFixed) {
    locLat_ = lat;
    locLon_ = lon;
    locAlt_ = alt;
    locFixValid_ = true;
    locForceFixed_ = forceFixed;
    Log.info("NTN location fix set to %.5lf, %.5lf, ALT:%.1lf (forceFixed=%s)",
        lat, lon, alt, forceFixed ? "true" : "false");
    return 0;
}

int Satellite::publishLocation() {
    if (!lastPositionInfo_.valid) {
        return -1;
    }

    memset(publishBuffer, 0, sizeof(publishBuffer));
    SpecialJSONWriter writer(publishBuffer, sizeof(publishBuffer));
    auto now = (unsigned int)Time.now();
    writer.beginObject();
        writer.name("cmd").value("loc");
        writer.name("time").value(now);
        writer.name("loc").beginObject();
            writer.name("lck").value(1);
            writer.name("time").value(now);
            writer.name("lat").value(lastPositionInfo_.latitude);
            writer.name("lon").value(lastPositionInfo_.longitude);
            writer.name("alt").value(lastPositionInfo_.altitude);
        writer.endObject();
    writer.endObject();

    return 0;
}

int Satellite::processErrors() {
    if (errorCount_ >= SATELLITE_NCP_COMM_ERRORS_MAX) {
        Log.error("%d errors, resetting modem!", SATELLITE_NCP_COMM_ERRORS_MAX);
        // reset modem and re-init
        Cellular.command(180000, "AT+CFUN=0");
        Cellular.command(180000, "AT+CFUN=1");
        ModemManager::sendTerminalCapability();
        errorCount_ = 0;
        registrationUpdateMs_ = SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS;
        registered_ = 0;
        nwConnected_ = NW_CONNECTED_INIT;
        ntnInit_ = 0;
        ntnConnected_ = 0;
        socketSuspect_ = false;
        lastSocketRebuild_ = 0; // rebuild immediately once the modem is back
    }
    // TODO: Check for uncommanded band change
    // 0000001817 [ncp.at] TRACE: > AT+QCFG="band"
    // 0000001831 [ncp.at] TRACE: < +QCFG: "band",0xf,0x100002000000000f0e189f,0x10004200000000090e189f,0x7
    // 0000001859 [ncp.at] TRACE: < OK
    return 0;
}

int Satellite::process(bool force) {
    updateRegistration(force);

    // Settle socket health at a safe point.
    if (socketSuspect_ && connected()) {
        const int sockState = querySocketState();
        if (sockState == SATELLITE_NCP_SOCKET_UNKNOWN) {
            Log.warn("Socket state unknown; deferring socket-health decision");
        } else {
            socketSuspect_ = false;
            if (sockState != SATELLITE_NCP_SOCKET_CONNECTED) {
                noteSocketLost("QISTATE reports no usable socket");
            }
        }
    }

    connectImpl();
    receiveData();
    processErrors();
    // Refresh the serving-cell signal report for status/diagnostics.
    if (force || millis() - lastServingCellCheck_ >= SATELLITE_NCP_SERVINGCELL_UPDATE_MS) {
        queryServingCell();
    }
    proto_.run();

    return 0;
}

} // namespace particle
