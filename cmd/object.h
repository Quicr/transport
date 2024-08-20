#pragma once

#include <transport/transport.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

using namespace qtransport;

class Object
{
  public:
    Object(std::shared_ptr<spdlog::logger> log) :
      logger(std::move(log)) {}

    void process(TransportConnId conn_id, std::optional<DataContextId> data_ctx_id, std::vector<uint8_t>& obj);
    std::vector<uint8_t> encode();

  private:
    std::shared_ptr<spdlog::logger> logger;

    uint64_t msgcount{ 0 };
    uint64_t prev_msgcount{ 0 };
    uint64_t msg_num { 0 };
    uint32_t prev_msg_num{ 0 };
};