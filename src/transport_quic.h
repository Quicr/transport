
#pragma once

#include <bytes/bytes.h>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "transport_udp.h"
#include "../../../include/packet.h"
#include "../../../include/transport.hh"

#include <picoquic.h>
#include <picoquic_internal.h>
#include <picoquic_logger.h>
#include <picoquic_utils.h>

using namespace bytes_ns;
namespace pico_sample {

class TransportManager;

class NetTransportQUIC;

// Context shared with th the underlying quic stack
typedef struct st_datagram_ctx_t
{
  TransportManager* transportManager;
  NetTransportQUIC* transport;
} datagram_ctx_t;

typedef enum
{
  alpn_undef = 0,
  alpn_neo_media
} picoquic_alpn_enum;

typedef struct st_picoquic_alpn_list_t
{
  picoquic_alpn_enum alpn_code;
  char const* alpn_val;
} picoquic_alpn_list_t;

static picoquic_alpn_list_t alpn_list[] = { { alpn_neo_media,
                                              "proto-pq-sample" } };

// QUIC transport
class NetTransportQUIC : public NetTransport
{
public:
  NetTransportQUIC(TransportManager*,
                   std::string sfuName_in,
                   uint16_t sfuPort_in);
  NetTransportQUIC(TransportManager*, uint16_t sfuPort_in);
  virtual ~NetTransportQUIC();

  virtual bool ready();
  virtual void close();
  virtual bool doSends();
  virtual bool doRecvs();

  int runQuicProcess();

  // callback registered with the quic stack on transport and data states
  static int datagram_callback(picoquic_cnx_t* cnx,
                               uint64_t stream_id,
                               uint8_t* bytes,
                               size_t length,
                               picoquic_call_back_event_t fin_or_event,
                               void* callback_ctx,
                               void* v_stream_ctx);

  TransportManager* transportManager;

  // Reports if the underlying quic stack is ready
  // for application messages
  std::mutex quicConnectionReadyMutex;
  bool quicConnectionReady;

  std::thread quicTransportThread;
  static int quicTransportThreadFunc(NetTransportQUIC* netTransportQuic)
  {
    return netTransportQuic->runQuicProcess();
  }

  struct QuicClientContext
  {
    std::string server_name;
    uint16_t port;
    std::string sni;
    struct sockaddr_storage server_address;
    socklen_t server_address_len;
  };

private:
  // Kick start Quic's connection context
  int quic_start_connection();

  const bool m_isServer;
  QuicClientContext quic_client_ctx;

  std::string alpn = "proto-pq-sample";
  picoquic_quic_t* quicHandle = nullptr;
  sockaddr_storage local_address;
  uint16_t local_port = 0;

  uint64_t current_time = 0;
  picoquic_cnx_t* cnx;

  NetTransportUDP* udp_socket;
};

} // namespace neo_media
