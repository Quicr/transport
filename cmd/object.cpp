#include "object.h"

void
Object::process(TransportConnId conn_id, std::optional<DataContextId> data_ctx_id, std::vector<uint8_t>& obj)
{
    msgcount++;

    if (msgcount % 2000 == 0 && prev_msgcount != msgcount) {
        prev_msgcount = msgcount;
    }

    uint32_t* msg_len = (uint32_t*)obj.data();
    uint32_t* msg_num = (uint32_t*)(obj.data() + 4);

    if (msg_num == nullptr)
        return;

    if (prev_msg_num && ((*msg_num - prev_msg_num) > 1 || (*msg_num - prev_msg_num) < 0)) {
        SPDLOG_LOGGER_INFO(logger,
                           "GAP: conn_id: {0} data_ctx_id: {1} msg_len: {2} length: {3} RecvMsg ({4}) msg_num: {5} "
                           "prev_num: {6} ({7})",
                           conn_id,
                           (data_ctx_id ? *data_ctx_id : 0),
                           *msg_len,
                           obj.size(),
                           msgcount,
                           *msg_num,
                           prev_msg_num,
                           *msg_num - prev_msg_num);
    }

    if (*msg_num % 2000 == 0) {
        SPDLOG_LOGGER_INFO(
          logger,
          "conn_id: {0} data_ctx_id: {1} msg_len: {2} length: {3} RecvMsg ({4}) msg_num: {5} prev_num: {6} ({7})",
          conn_id,
          (data_ctx_id ? *data_ctx_id : 0),
          *msg_len,
          obj.size(),
          msgcount,
          *msg_num,
          prev_msg_num,
          *msg_num - prev_msg_num);
    }

    prev_msg_num = *msg_num;
}

std::vector<uint8_t>
Object::encode()
{
    std::vector<uint8_t> obj(1000, 0);

    uint32_t* len = (uint32_t*)obj.data();
    uint32_t* num = (uint32_t*)(obj.data() + 4);

    *len = obj.size();
    *num = ++msg_num;

    return obj;
}