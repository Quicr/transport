#pragma once

#include <transport/transport.h>

using namespace qtransport;

class Object
{
  public:
    Object(const cantina::LoggerPointer& logger) :
      logger(std::make_shared<cantina::Logger>("OBJ", logger)) {}

    void process(TransportConnId conn_id, std::optional<DataContextId> data_ctx_id, std::vector<uint8_t>& obj);
    std::vector<uint8_t> encode();

  private:
    cantina::LoggerPointer logger;

    uint64_t msgcount{ 0 };
    uint64_t prev_msgcount{ 0 };
    uint64_t msg_num { 0 };
    uint32_t prev_msg_num{ 0 };
};