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

#include "Particle.h"

#include "system_error.h"
#include "cloud_protocol.h"

#include <functional>
#include <optional>

// Secure UDP Phase 1 (CDS-UDP-v1) wraps the NTN UDP path. Set to 0 to fall back
// to the legacy unauthenticated datagram + port for bring-up.
#ifndef SECURE_UDP_ENABLED
#define SECURE_UDP_ENABLED 1
#endif

#if SECURE_UDP_ENABLED
#include "secure_udp_session.h"
#endif

const uint8_t NW_CONNECTED_INIT = 0;
const uint8_t NW_CONNECTED_SUCCESS = 1;
const uint8_t NW_CONNECTED_FAILED = 2;

const uint8_t NW_STATE_IDLE = 0;
const uint8_t NW_STATE_CONNECT = 1;
const uint8_t NW_STATE_DISCONNECT = 2;

namespace particle {

struct GnssPositioningInfo {
    uint16_t version;
    uint16_t size;
    double latitude;
    double longitude;
    float accuracy;
    float altitude;
    float cog;
    float speedKmph;
    float speedKnots;
    struct tm utcTime;
    int satsInView;
    bool locked;
    int posMode;
    int valid;
};

// Parsed AT+QENG="servingcell" response. Field order follows the Quectel
// servingcell report:
//   +QENG: "servingcell",<state>,<rat>,<duplex>,<mcc>,<mnc>,<cellId(hex)>,
//          <pcid>,<earfcn>,<band>,<ulBw>,<dlBw>,<tac(hex)>,<rsrp>,<rsrq>,
//          <rssi>,<sinr>,<srxlev>
// When the modem is still searching it reports only the state field, so
// `valid` distinguishes "full signal metrics present" from a bare state.
struct NtnServingCellInfo {
    bool valid = false;          // true when the full signal metrics parsed
    char state[12] = {};         // SEARCH / LIMSRV / CONNECT / NOCONN ...
    char rat[16] = {};           // e.g. "NTN NBIoT"
    char duplex[8] = {};         // FDD / TDD
    int mcc = 0;                 // mobile country code (901 = Skylo NTN shared)
    int mnc = 0;                 // mobile network code
    unsigned int cellId = 0;     // cell identity (hex on the wire)
    int pcid = 0;                // physical cell ID
    int earfcn = 0;              // channel number
    int band = 0;                // frequency band
    int ulBandwidth = 0;
    int dlBandwidth = 0;
    unsigned int tac = 0;        // tracking area code (hex on the wire)
    int rsrp = 0;                // dBm
    int rsrq = 0;                // dB
    int rssi = 0;                // dBm
    int sinr = 0;                // Quectel index 0-250 -> -20..+30 dB
    int srxlev = 0;              // cell-selection RX level
};

class SpecialJSONWriter : public spark::JSONBufferWriter {

  public:
    SpecialJSONWriter(char *buf, size_t size) : spark::JSONBufferWriter(buf, size) {

    }
    using spark::JSONBufferWriter::write;
};

class Satellite {

public:

    Satellite();
    ~Satellite();

    int begin(void);
    int connect(void);
    int disconnect(void);
    bool connected(void);
    int tx(const uint8_t* buf, size_t len, int port);

    // ---- Raw passthrough mode (application testing) -----------------------
    // Sends and receives datagrams with NO constrained protocol and NO secure
    // UDP frame: the bytes handed to txRaw() are exactly the bytes on the wire,
    // and inbound datagrams are handed back verbatim. Everything below the
    // protocol layer is unchanged - registration, ntn_locfix, the PDP/socket
    // lifecycle, the QISENDEX retry + socket-rebuild logic and process() all
    // behave identically. Intended for talking to your own UDP test endpoint;
    // the Particle ingress will not accept unauthenticated datagrams.
    typedef std::function<void(const uint8_t* data, size_t len)> RawRxHandler;

    // Enable/disable raw mode and install the downlink handler. Call before
    // begin() so the first openDataSession() and the first inbound poll both
    // see the mode. Passing nullptr keeps datagrams logged but undelivered.
    void setRawMode(bool enabled, RawRxHandler onRx = nullptr);

    bool rawMode(void) const {
        return rawMode_;
    }

    // Raw uplink. Returns 0 on AT-accepted send, SYSTEM_ERROR_TOO_LARGE when
    // len exceeds the on-wire cap (the full cap is available here - there is no
    // secure-frame overhead to subtract), SYSTEM_ERROR_INVALID_STATE when the
    // transport is not up.
    int txRaw(const uint8_t* buf, size_t len);

    // Override the UDP endpoint both transports send to. Must be called before
    // begin() / beginCellularTransport(): the address is baked into AT+QIOPEN
    // when the data session is built. Numeric IP only - DNS resolution over NTN
    // is not feasible. Defaults to the Particle secure ingress.
    void setEndpoint(const IPAddress& ip, uint16_t port);

    int publish(int code) {
        return proto_.publish(code);
    }

    int publish(int code, const Variant& data) {
        return proto_.publish(code, data);
    }

    int subscribe(int code, constrained::CloudProtocol::OnEvent onEvent) {
        return proto_.subscribe(code, std::move(onEvent));
    }

    int getGNSSLocation(unsigned int maxFixWaitTimeMs = 120000);
    int publishLocation();

    void setMaxPayloadSize(size_t size) {
        maxPayloadSize_ = size;
    }

    // Provide the location used for the NTN location fix (AT+QNWCFG="ntn_locfix").
    // Stores the coordinates so begin() can program them into the modem before
    // NTN registration. Must be called before begin() to take effect on the
    // next registration.
    //
    // forceFixed: when true, getGNSSLocation() short-circuits to these stored
    // coordinates and never queries the modem's GNSS engine. Use this when the
    // device has no GNSS antenna and the app is supplying a static location.
    int setLocationFix(double lat, double lon, double alt, bool forceFixed = false);

    int process(bool force = false);

    // ---- Constrained protocol over the normal Device OS connection --------
    // Runs the same CloudProtocol + secure-UDP stack over a Device OS UDP
    // socket while cellular/WiFi is the active connection (Device OS owns the
    // modem there, so the NTN AT-socket path is unavailable). Both transports
    // share the one secure-UDP session, so the per-direction counter space
    // never forks across radio switches.
    //
    // beginCellularTransport() is idempotent; call it only once the network is
    // up (Particle.connected()). endCellularTransport() must run before the
    // network drops (e.g. ahead of a switch to the satellite profile) and is a
    // safe no-op if the transport never started. processCellularTransport()
    // is the cellular analog of process(): drive it every loop while the
    // transport is active to poll for downlinks and run protocol timers.
    int beginCellularTransport();
    void endCellularTransport();
    int processCellularTransport();
    bool cellularTransportActive() const {
        return transportMode_ == TransportMode::DEVICEOS_UDP && udpStarted_;
    }

    GnssPositioningInfo lastPositionInfo(void) {
        return lastPositionInfo_;
    };

    NtnServingCellInfo servingCellInfo(void) {
        return servingCell_;
    };

    // Terrestrial registration, as of the last registration poll: the operator
    // name from AT+COPS?, or "" when the cellular radio is not registered.
    // Independent of NTN registration - see queryCellularRegistration().
    const char* cellularOperator(void) const {
        return cellularOperator_;
    };

#if SECURE_UDP_ENABLED
    // Downlink secure-verification failures, split by mode: an attack signal
    // (badTag), normal retransmission noise (replay), and an operational
    // storage fault (persistFailed) need different remediation, so they must
    // not share one counter.
    struct SecureRxStats {
        uint32_t malformed = 0;      // too short to parse
        uint32_t badTag = 0;         // authentication failed
        uint32_t replay = 0;         // stale / duplicate counter
        uint32_t persistFailed = 0;  // authenticated, replay floor not persisted
        uint32_t notReady = 0;       // datagram before session init
    };

    const SecureRxStats& secureRxStats() const {
        return secureRxStats_;
    }
#endif

private:

    bool begun_; // true if begin() previously called

    uint8_t registered_ = 0;
    volatile uint8_t ntnInit_ = 0;
    volatile uint8_t ntnConnected_ = 0;
    volatile uint8_t nwConnected_ = NW_CONNECTED_INIT;
    volatile uint8_t nwConnectionDesired_ = NW_STATE_IDLE;
    uint32_t lastReceivedCheck_ = 0;
    uint32_t lastRegistrationCheck_ = 0;
    uint32_t lastServingCellCheck_ = 0;
    uint32_t registrationUpdateMs_ = 0;
    uint32_t noRegistrationTimer_ = 0;
    int errorCount_ = 0;

    bool socketSuspect_ = false;
    uint32_t lastSocketRebuild_ = 0;
    GnssPositioningInfo lastPositionInfo_;
    NtnServingCellInfo servingCell_;

    // NTN location fix coordinates programmed via AT+QNWCFG="ntn_locfix".
    double locLat_ = 0;
    double locLon_ = 0;
    double locAlt_ = 0;
    bool locFixValid_ = false;
    // When true, getGNSSLocation() returns the stored loc{Lat,Lon,Alt}_ without
    // querying the GNSS engine.
    bool locForceFixed_ = false;

    size_t maxPayloadSize_ = 0;
    constrained::CloudProtocol proto_;

    // UDP endpoint for both transports; overridable via setEndpoint(). Numeric
    // IPs only - DNS resolution over NTN is not feasible.
    IPAddress endpointIp_ = IPAddress(52, 5, 13, 97); // secure ingress
    uint16_t  endpointPort_ = 9932;                   // secure ingress

    // Raw passthrough: bypasses secure UDP + CloudProtocol in both directions.
    // See setRawMode(). Nothing below the protocol layer is affected.
    bool rawMode_ = false;
    RawRxHandler rawRxHandler_;

    // Which byte transport tx()/receive uses under the constrained protocol.
    //   NTN_AT_SOCKET: app-owned modem, hex encode + AT+QISENDEX / AT+QIRD.
    //   DEVICEOS_UDP : Device OS UDP socket over the normal connection.
    enum class TransportMode {
        NTN_AT_SOCKET,
        DEVICEOS_UDP,
    };

    TransportMode transportMode_ = TransportMode::NTN_AT_SOCKET;
    UDP udp_;
    bool udpStarted_ = false;
    uint32_t lastUdpReceiveCheck_ = 0;

#if SECURE_UDP_ENABLED
    // Secure UDP session: wraps uplinks and verifies downlinks at the modem
    // boundary. Counter watermarks persist to a flash file (stride rule, §6.2).
    secure_udp::FlashCounterStore secureUdpStore_{"/secure_udp.ctr"};
    secure_udp::SecureUdpSession secureUdp_;
    SecureRxStats secureRxStats_;
#endif

    char publishBuffer[1024] = {};

    // Last +COPS: <oper>, "" when the terrestrial radio is unregistered.
    // Reporting only - NTN registration is owned by servingCell_.state.
    char cellularOperator_[32] = {};

    static int cbCFUN(int type, const char* buf, int len, int* cfun);
    static int cbCOPS(int type, const char* buf, int len, char* network);
    static int cbQCFGEXTquery(int type, const char* buf, int len, int* rxlen);
    static int cbQIACT(int type, const char* buf, int len, int* state);
    static int cbQISTATE(int type, const char* buf, int len, int* state);
    static int cbQIRDquery(int type, const char* buf, int len, int* rxlen);
    static int cbQIRD(int type, const char* buf, int len, char* outBuf);
    static int cbQISENDEX(int type, const char* buf, int len, int* param);
    static int cbQCFGEXTread(int type, const char* buf, int len, char* rxdata);
    static int cbQGPSLOC(int type, const char* buf, int len, GnssPositioningInfo* info);
    static int cbQENG(int type, const char* buf, int len, NtnServingCellInfo* info);
    static int cbQNWCFGNTNLOCFIX(int type, const char* buf, int len, GnssPositioningInfo* info);

    bool locFixMatches(const GnssPositioningInfo& cur) const;
    int queryCellularRegistration(void);
    int queryServingCell(void);
    int openDataSession(void);
    int querySocketState(void);
    bool socketRebuildAllowed(void);
    void noteSocketLost(const char* why);
    int waitAtResponse(unsigned int tries, unsigned int timeout = 1000);
    int publishImpl(int code, const std::optional<Variant>& data = std::nullopt);
    // Byte transport shared by tx() (secure-wrapped) and txRaw() (verbatim):
    // Device OS UDP sendPacket, or hex encode + the AT+QISENDEX retry / socket
    // rebuild loop.
    int txBytes(const uint8_t* buf, size_t len);
    void updateRegistration(bool force = false);

    void receiveData(void);
    void handleInboundDatagram(char* data, size_t len);
    int processErrors(void);
    int connectImpl(void);
    int initProtocolStack(void);
};

} // particle