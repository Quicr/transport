#include "object.h"

void Object::process(TransportConnId conn_id, std::optional<DataContextId> data_ctx_id, std::vector<uint8_t>& obj) {
    msgcount++;

    if (msgcount % 2000 == 0 && prev_msgcount != msgcount) {
        prev_msgcount = msgcount;
    }

    uint32_t* msg_len = (uint32_t*)obj.data();
    uint32_t* msg_num = (uint32_t*)(obj.data() + 4);

    if (msg_num == nullptr)
        return;

    if (prev_msg_num && ((*msg_num - prev_msg_num) > 1 || (*msg_num - prev_msg_num) < 0)) {
        logger->info << "GAP: conn_id: " << conn_id << " data_ctx_id: " << (data_ctx_id ? *data_ctx_id : -1)
                     << " msg_len: " << *msg_len
                     << " length: " << obj.size() << " RecvMsg (" << msgcount << ")"
                     << " msg_num: " << *msg_num << " prev_num: " << prev_msg_num << "("
                     << *msg_num - prev_msg_num << ")" << std::flush;
    }

    if (*msg_num % 2000 == 0) {
        logger->info << "conn_id: " << conn_id << " data_ctx_id: " << (data_ctx_id ? *data_ctx_id : -1)
                     << " msg_len: " << *msg_len << " length: " << obj.size() << " RecvMsg (" << msgcount << ")"
                     << " msg_num: " << *msg_num << " prev_num: " << prev_msg_num << "(" << *msg_num - prev_msg_num
                     << ")" << std::flush;
    }

    prev_msg_num = *msg_num;
}

std::vector<uint8_t> Object::encode() {
    std::vector<uint8_t> obj(1000, 0);

    uint32_t* len = (uint32_t*)obj.data();
    uint32_t* num = (uint32_t*)(obj.data() + 4);

    *len = obj.size();
    *num = ++msg_num;

    return std::move(obj);
}