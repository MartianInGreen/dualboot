// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 The Official Mesh Admin
#ifdef MDB_NET_OTA

#include "mdb_wifi.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <stdio.h>
#include <string.h>

namespace mdb_wifi {

// Forward decl so start()'s failure paths can call the public teardown
// without depending on definition order below.
void stop();

static int listen_sock = -1;
static bool mdns_up = false;

// Construct "mdb-XXYY" from the last two bytes of the WiFi MAC. Same
// pattern as the AP SSID so a device's hostname matches its softAP name.
static void hostname_from_mac(char* out, size_t out_size) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(out, out_size, "mdb-%02x%02x", mac[4], mac[5]);
}

// Bring up mDNS responder advertising hostname `mdb-XXYY.local` and a
// `_mdb-netota._tcp` service on the listener port. Lets the pusher
// reach the device via `mdb-XXYY.local` instead of needing to look up
// the DHCP lease — the difference between "headless works" and
// "operator has to ARP-scan the LAN."
static void start_mdns(uint16_t port) {
  if (mdns_up) return;
  char host[16];
  hostname_from_mac(host, sizeof(host));
  if (MDNS.begin(host)) {
    MDNS.addService("mdb-netota", "tcp", port);
    mdns_up = true;
  }
}

static void stop_mdns() {
  if (!mdns_up) return;
  MDNS.end();
  mdns_up = false;
}

static bool start_listener(uint16_t port) {
  listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_sock < 0) return false;

  int one = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(listen_sock);
    listen_sock = -1;
    return false;
  }
  if (listen(listen_sock, 1) < 0) {
    close(listen_sock);
    listen_sock = -1;
    return false;
  }
  return true;
}

bool start(mdb_net_ota::Transport t, const char* ssid, const char* psk,
           uint16_t port, char* out_status, size_t out_status_size) {
  if (!ssid || !psk) return false;

  WiFi.persistent(false);  // don't write SSID/PSK into the WiFi NVS namespace

  if (t == mdb_net_ota::Transport::AP) {
#ifdef MDB_NET_OTA_AP
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(ssid, psk)) {
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(100);  // let the IP stack settle
    IPAddress ip = WiFi.softAPIP();
    snprintf(out_status, out_status_size, "%u.%u.%u.%u:%u",
             ip[0], ip[1], ip[2], ip[3], (unsigned)port);
#else
    return false;  // _AP not compiled in
#endif
  } else {
#ifdef MDB_NET_OTA_STA
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, psk);
    // 30 s connect timeout — long enough for slow APs, short enough that
    // a misconfigured SSID doesn't hang the device beyond reason.
    uint32_t deadline = millis() + 30000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
      delay(200);
    }
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.mode(WIFI_OFF);
      return false;
    }
    IPAddress ip = WiFi.localIP();
    start_mdns(port);
    // Show mDNS hostname rather than the raw IP — the operator can plug
    // it directly into `mdb_push.py --host mdb-XXYY.local`. IP is logged
    // to Serial as a fallback for networks that block multicast.
    char host[16];
    hostname_from_mac(host, sizeof(host));
    snprintf(out_status, out_status_size, "%s.local:%u", host, (unsigned)port);
    Serial.printf("[mdb_wifi] STA up: %s.local @ %u.%u.%u.%u:%u\n",
                  host, ip[0], ip[1], ip[2], ip[3], (unsigned)port);
#else
    return false;  // _STA not compiled in
#endif
  }

  // If listener bind/listen fails, tear down WiFi we just brought up so
  // we don't leave an AP broadcasting or an STA association lingering.
  // Callers see start()==false and bail without calling stop() themselves.
  if (!start_listener(port)) {
    stop();
    return false;
  }
  return true;
}

void stop() {
  stop_mdns();
  if (listen_sock >= 0) {
    close(listen_sock);
    listen_sock = -1;
  }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

int accept_one(uint32_t timeout_ms) {
  if (listen_sock < 0) return -1;

  // Use select() so we can honor a wall-clock timeout; accept() on its
  // own would block indefinitely. The listener stays in blocking mode
  // because once select() reports ready we want accept() to succeed
  // immediately rather than juggle EAGAIN.
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(listen_sock, &rfds);
  struct timeval tv = {
    .tv_sec  = (long)(timeout_ms / 1000),
    .tv_usec = (long)((timeout_ms % 1000) * 1000),
  };
  int r = select(listen_sock + 1, &rfds, nullptr, nullptr, &tv);
  if (r <= 0) return -1;

  struct sockaddr_in peer;
  socklen_t peer_len = sizeof(peer);
  return accept(listen_sock, (struct sockaddr*)&peer, &peer_len);
}

int read_bytes(int sock, void* buf, size_t n, uint32_t timeout_ms) {
  if (sock < 0 || n == 0) return -1;

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(sock, &rfds);
  struct timeval tv = {
    .tv_sec  = (long)(timeout_ms / 1000),
    .tv_usec = (long)((timeout_ms % 1000) * 1000),
  };
  int r = select(sock + 1, &rfds, nullptr, nullptr, &tv);
  if (r <= 0) return -1;

  ssize_t got = recv(sock, buf, n, 0);
  return (int)got;
}

bool write_byte(int sock, uint8_t b) {
  if (sock < 0) return false;
  ssize_t n = send(sock, &b, 1, 0);
  return n == 1;
}

void current_ip(char* out, size_t out_size) {
  IPAddress ip = (WiFi.getMode() & WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP();
  snprintf(out, out_size, "%u.%u.%u.%u",
           ip[0], ip[1], ip[2], ip[3]);
}

void close_socket(int sock) {
  if (sock >= 0) close(sock);
}

}  // namespace mdb_wifi

#endif  // MDB_NET_OTA
