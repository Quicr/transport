#include <cassert>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string.h> // memcpy
#include <thread>
#include <unistd.h>

#if defined(__linux) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netdb.h>
#endif
#if defined(__linux__)
#include <net/ethernet.h>
#include <netpacket/packet.h>
#elif defined(__APPLE__)
#include <net/if_dl.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "netTransportQUIC.hh"
#include "transport_manager.hh"

#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquic_logger.h"
#include "picoquic_utils.h"
#include "picosocks.h"
#include "picotls.h"

using namespace pico_sample;
#define SERVER_CERT_FILE "cert.pem"
#define SERVER_KEY_FILE "key.pem"

// TODO: this should be scoped
static TransportManager* transportManagerGlobalRef;
static NetTransportQUIC* transportGlobalRef;

int
transport_close_reason(picoquic_cnx_t* cnx)
{
  uint64_t last_err = 0;
  int ret = 0;
  if ((last_err = picoquic_get_local_error(cnx)) != 0) {
    fprintf(
      stdout, "Connection end with local error 0x%" PRIx64 ".\n", last_err);
    ret = -1;
  }
  if ((last_err = picoquic_get_remote_error(cnx)) != 0) {
    fprintf(
      stdout, "Connection end with remote error 0x%" PRIx64 ".\n", last_err);
    ret = -1;
  }
  if ((last_err = picoquic_get_application_error(cnx)) != 0) {
    fprintf(stdout,
            "Connection end with application error 0x%" PRIx64 ".\n",
            last_err);
    ret = -1;
  }
  return ret;
}

int
NetTransportQUIC::datagram_callback(picoquic_cnx_t* cnx,
                                    uint64_t stream_id,
                                    uint8_t* bytes_in,
                                    size_t length,
                                    picoquic_call_back_event_t fin_or_event,
                                    void* callback_ctx,
                                    void* v_stream_ctx)
{
  // std::cout << "datagram_callback <<<\n";
  int ret = 0;
  auto* ctx = (pico_sample::datagram_ctx_t*)callback_ctx;
  if (ctx == nullptr) {
    ctx = new pico_sample::datagram_ctx_t{};
    ctx->transportManager = transportManagerGlobalRef;
    ctx->transport = transportGlobalRef;
    ctx->transport->cnx = cnx;
    picoquic_set_callback(cnx, &NetTransportQUIC::datagram_callback, ctx);
  } else {
    ret = 0;
  }

  assert(ctx != nullptr);

  switch (fin_or_event) {
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
    case picoquic_callback_stream_reset: /* Client reset stream #x */
    case picoquic_callback_stop_sending: /* Client asks server to reset stream
                                                                                                                                                                    #x */
    case picoquic_callback_stream_gap:
    case picoquic_callback_prepare_to_send:
      std::cout << "Unexpected callback " << std::endl;
      if (ctx != nullptr) {
        free(ctx);
        ctx = nullptr;
      }
      std::cout << "picoquic_callback_prepare_to_send" << std::endl;
      break;
    case picoquic_callback_stateless_reset:
    case picoquic_callback_close:             /* Received connection close */
    case picoquic_callback_application_close: /* Received application close */
      if (ctx != nullptr) {
        free(ctx);
        ctx = nullptr;
      }
      std::cout << "picoquic_callback_application_close: "
                << transport_close_reason(cnx) << std::endl;
      picoquic_set_callback(cnx, nullptr, nullptr);
      break;
    case picoquic_callback_version_negotiation:
      break;
    case picoquic_callback_almost_ready:
      std::cout << "picoquic_callback_almost_ready" << std::endl;
      break;
    case picoquic_callback_ready: {
      std::cout << " Callback: Transport Ready" << std::endl;
      if (ctx->transport) {
        std::lock_guard<std::mutex> lock(
          ctx->transport->quicConnectionReadyMutex);
        ctx->transport->quicConnectionReady = true;
      }
    }
      ret = 0;
      break;
    case picoquic_callback_datagram: {
      /* Process the datagram, which contains an address and a QUIC packet */
      auto data = bytes(bytes_in, bytes_in + length);
      ctx->transportManager->recvDataFromNet(data, nullptr, 0);
      break;
    }
      ret = 0;
    default:
      assert(0);
      break;
  }

  return ret;
}

static size_t nb_alpn_list = sizeof(alpn_list) / sizeof(picoquic_alpn_list_t);

picoquic_alpn_enum
picoquic_parse_alpn_nz(char const* alpn, size_t len)
{
  picoquic_alpn_enum code = alpn_undef;

  if (alpn != NULL) {
    for (size_t i = 0; i < nb_alpn_list; i++) {
      if (memcmp(alpn, alpn_list[i].alpn_val, len) == 0 &&
          alpn_list[i].alpn_val[len] == 0) {
        code = alpn_list[i].alpn_code;
        break;
      }
    }
  }

  return code;
}

int
picoquic_server_callback(picoquic_cnx_t* cnx,
                         uint64_t stream_id,
                         uint8_t* bytes,
                         size_t length,
                         picoquic_call_back_event_t fin_or_event,
                         void* callback_ctx,
                         void* v_stream_ctx)
{
  int ret = 0;
  ret = NetTransportQUIC::datagram_callback(
    cnx, stream_id, bytes, length, fin_or_event, callback_ctx, v_stream_ctx);
  return ret;
}

/* Callback from the TLS stack upon receiving a list of proposed ALPN in the
 * Client Hello */
size_t
picoquic_select_alpn(picoquic_quic_t* quic, ptls_iovec_t* list, size_t count)
{
  size_t ret = count;

  for (size_t i = 0; i < count; i++) {
    if (picoquic_parse_alpn_nz((const char*)list[i].base, list[i].len) !=
        alpn_undef) {
      ret = i;
      break;
    }
  }

  return ret;
}

NetTransportQUIC::~NetTransportQUIC()
{
  close();
}

void
NetTransportQUIC::close()
{}

bool
NetTransportQUIC::doRecvs()
{
  return false;
}

bool
NetTransportQUIC::doSends()
{
  return false;
}

int
NetTransportQUIC::quic_start_connection()
{
  // create client connection context
  std::cout << "starting client connection to " << quic_client_ctx.sni
            << std::endl;
  cnx = picoquic_create_cnx(quicHandle,
                            picoquic_null_connection_id,
                            picoquic_null_connection_id,
                            (struct sockaddr*)&quic_client_ctx.server_address,
                            picoquic_get_quic_time(quicHandle),
                            0,
                            quic_client_ctx.sni.data(),
                            alpn.data(),
                            1);

  assert(cnx != nullptr);

  auto* datagram_ctx = new datagram_ctx_t{};
  datagram_ctx->transportManager = transportManager;
  datagram_ctx->transport = this;
  picoquic_set_callback(cnx, datagram_callback, (void*)datagram_ctx);
  cnx->local_parameters.max_datagram_frame_size = 1500;

  int ret = picoquic_start_client_cnx(cnx);
  assert(ret == 0);
  std::cout << "Started Quic Client Connection" << std::endl;
  return ret;
}

// Client Transport
NetTransportQUIC::NetTransportQUIC(TransportManager* t,
                                   std::string sfuName,
                                   uint16_t sfuPort)
  : transportManager(t)
  , quicConnectionReady(false)
  , m_isServer(false)
{
  std::cout << "Quic Client Transport" << std::endl;
  udp_socket = new NetTransportUDP{ sfuName, sfuPort };
  assert(udp_socket);

  // TODO: remove the duplication
  std::string sPort = std::to_string(htons(sfuPort));
  struct addrinfo hints = {}, *address_list = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  int err = getaddrinfo(sfuName.c_str(), sPort.c_str(), &hints, &address_list);
  if (err) {
    assert(0);
  }
  struct addrinfo *item = nullptr, *found_addr = nullptr;
  for (item = address_list; item != nullptr; item = item->ai_next) {
    if (item->ai_family == AF_INET && item->ai_socktype == SOCK_DGRAM &&
        item->ai_protocol == IPPROTO_UDP) {
      found_addr = item;
      break;
    }
  }

  if (found_addr == nullptr) {
    assert(0);
  }

  struct sockaddr_in* ipv4_dest =
    (struct sockaddr_in*)&quic_client_ctx.server_address;
  memcpy(ipv4_dest, found_addr->ai_addr, found_addr->ai_addrlen);
  ipv4_dest->sin_port = htons(sfuPort);
  quic_client_ctx.server_address_len = sizeof(quic_client_ctx.server_address);
  quic_client_ctx.server_name = sfuName;
  quic_client_ctx.port = sfuPort;

  // create quic client context
  auto ticket_store_filename = "token-store.bin";

  /* Create QUIC context */
  current_time = picoquic_current_time();
  quicHandle = picoquic_create(1,
                               NULL,
                               NULL,
                               NULL,
                               alpn.data(),
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               current_time,
                               NULL,
                               "ticket-store.bin",
                               NULL,
                               0);

  assert(quicHandle != nullptr);

  picoquic_set_default_congestion_algorithm(quicHandle, picoquic_bbr_algorithm);

  if (picoquic_load_retry_tokens(quicHandle, ticket_store_filename) != 0) {
    fprintf(stderr,
            "No token file present. Will create one as <%s>.\n",
            ticket_store_filename);
  }

  (void)picoquic_set_default_connection_id_length(quicHandle, 8);

  picoquic_set_textlog(quicHandle, "clientlog.txt");
  picoquic_set_log_level(quicHandle, 2);

  udp_socket = new NetTransportUDP{ sfuName, sfuPort };
  transportManagerGlobalRef = transportManager;
  transportGlobalRef = this;
  // start the quic thread
  quicTransportThread = std::thread(quicTransportThreadFunc, this);
}

// server
NetTransportQUIC::NetTransportQUIC(TransportManager* t, uint16_t sfuPort)
  : transportManager(t)
  , quicConnectionReady(false)
  , m_isServer(true)

{
  std::cout << "Quic Server Transport" << std::endl;
  char default_server_cert_file[512];
  char default_server_key_file[512];
  const char* server_cert_file = nullptr;
  const char* server_key_file = nullptr;

  picoquic_get_input_path(default_server_cert_file,
                          sizeof(default_server_cert_file),
                          "/tmp",
                          SERVER_CERT_FILE);
  server_cert_file = default_server_cert_file;

  picoquic_get_input_path(default_server_key_file,
                          sizeof(default_server_key_file),
                          "/tmp",
                          SERVER_KEY_FILE);
  server_key_file = default_server_key_file;

  quicHandle = picoquic_create(1,
                               server_cert_file,
                               server_key_file,
                               NULL,
                               alpn.data(),
                               picoquic_server_callback,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               current_time,
                               NULL,
                               NULL,
                               NULL,
                               0);

  assert(quicHandle != nullptr);

  picoquic_set_alpn_select_fn(quicHandle, picoquic_select_alpn);
  picoquic_set_default_congestion_algorithm(quicHandle, picoquic_bbr_algorithm);

  picoquic_set_textlog(quicHandle, "serverlog.txt");
  picoquic_set_log_level(quicHandle, 2);

  std::cout << "Setting up udp socket " << std::endl;

  udp_socket = new NetTransportUDP{ sfuPort };
  transportManagerGlobalRef = transportManager;
  transportGlobalRef = this;
  quicTransportThread = std::thread(quicTransportThreadFunc, this);
}

static void
print_sock_info(const std::string& debug_string, sockaddr_storage* addr)
{

  char hoststr[NI_MAXHOST];
  char portstr[NI_MAXSERV];
  socklen_t len = sizeof(struct sockaddr_storage);
  int rc = getnameinfo((struct sockaddr*)addr,
                       len,
                       hoststr,
                       sizeof(hoststr),
                       portstr,
                       sizeof(portstr),
                       NI_NUMERICHOST | NI_NUMERICSERV);
  if (rc != 0) {
    std::cout << "getnameinfo error = " << gai_strerror(rc) << "\n";
    // assert(0);
  } else {
    std::cout << debug_string << " host: " << hoststr << " port: " << portstr
              << std::endl;
  }
}

bool
NetTransportQUIC::ready()
{
  bool ret;
  {
    std::lock_guard<std::mutex> lock(quicConnectionReadyMutex);
    ret = quicConnectionReady;
  }
  if (ret) {
    std::cout << "NetTransportQUIC::ready()" << std::endl;
  }
  return ret;
}

// Main quic process thread
// 1. check for incoming packets
// 2. check for outgoing packets
int
NetTransportQUIC::runQuicProcess()
{

  // create the quic client connection context
  if (!m_isServer) {
    auto ret = quic_start_connection();
    assert(ret == 0);
  }

  picoquic_quic_t* quic = quicHandle;
  int if_index = 0;
  picoquic_connection_id_t log_cid;
  picoquic_cnx_t* last_cnx = nullptr;

  while (!transportManager->shutDown) {
    Packet packet;

    //  call to next wake delay, pass it to select()
    auto got = udp_socket->doRecvs(packet);
    if (local_port == 0) {
      if (picoquic_get_local_address(udp_socket->fd, &local_address) != 0) {
        memset(&local_address, 0, sizeof(struct sockaddr_storage));
        fprintf(stderr, "Could not read local address.\n");
      }
      // todo: support AF_INET6
      local_port = ((struct sockaddr_in*)&local_address)->sin_port;
      std::cout << "Found local port  " << local_port << std::endl;
    }

    if (got) {
      // std::cout << "Recvd data from net:" << packet.data.size() << "
      // bytes\n";
      // let the quic stack know of the incoming packet
      uint64_t curr_time = picoquic_get_quic_time(quicHandle);

      // print_sock_info("incoming: peer: ", &packet.addr);
      // print_sock_info("incoming: local: ", &local_address);

      int ret =
        picoquic_incoming_packet(quic,
                                 reinterpret_cast<uint8_t*>(packet.data.data()),
                                 packet.data.size(),
                                 (struct sockaddr*)&packet.addr,
                                 (struct sockaddr*)&local_address,
                                 -1,
                                 0,
                                 curr_time);
      assert(ret == 0);
    }

    // dequeue from the application
    auto maybe_data = transportManager->getDataToSendToNet();
    // delay might not be good idea here.

    uint64_t curr_time_send = picoquic_current_time();
    if (maybe_data.has_value()) {
      auto data = std::move(maybe_data.value());
      // std::cout << "enqueueing datagram " << data.size() << std::endl;
      int ret = picoquic_queue_datagram_frame(cnx, data.size(), data.data());
      assert(ret == 0);
    }

    // verify if there are any packets from the underlying quic context
    size_t send_length = 0;
    bytes send_buffer;
    send_buffer.resize(1500);
    struct sockaddr_storage peer_addr;
    struct sockaddr_storage local_addr;

    int ret = picoquic_prepare_next_packet(quicHandle,
                                           curr_time_send,
                                           send_buffer.data(),
                                           send_buffer.size(),
                                           &send_length,
                                           &peer_addr,
                                           &local_addr,
                                           &if_index,
                                           &log_cid,
                                           &last_cnx);

    assert(ret == 0);

    if (send_length > 0) {
      send_buffer.resize(send_length);
      Packet send_packet;
      send_packet.data = std::move(send_buffer);
      if (packet.empty()) {
        send_packet.addr_len = udp_socket->sfuAddrLen;
        memcpy(&send_packet.addr, &udp_socket->sfuAddr, udp_socket->sfuAddrLen);
      } else {
        send_packet.addr_len = packet.addr_len;
        memcpy(&send_packet.addr, &packet.addr, packet.addr_len);
      }
      udp_socket->doSends(send_packet);
    }
  } // !transport_shutdown

  std::cout << "DONE" << std::endl;
  assert(0);
  // return true;
}