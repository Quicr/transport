#include <doctest/doctest.h>

#include "transport/stream_buffer.h"
#include "transport/uintvar.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("StreamBuffer Reader/Writer")
{
    using StreamBufType = qtransport::StreamBuffer<std::uint32_t>;
    std::shared_ptr<StreamBufType> buf = std::make_shared<StreamBufType>();
    bool stop{ false };
    size_t rcount{ 0 }, wcount{ 0 };

    std::thread reader([&buf, &rcount, &stop]() {
        while (!stop) {
            if (const auto val = buf->Front()) {
                FAST_CHECK_EQ(*val, rcount);

                ++rcount;
                buf->Pop();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(60));
            }
        }

        rcount += buf->Size();
        buf->Pop(buf->Size());
    });

    std::thread writer([&buf, &wcount, &stop]() {
        while (!stop) {
            buf->Push(wcount++);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));
    stop = true;

    writer.join();
    reader.join();

    CHECK_EQ(buf->Size(), 0);
    CHECK_EQ(rcount, wcount);
}

/* **************************************************
 * MOQT Test read using stream buffer
 * **************************************************
 */
enum class MoqMessageType : std::uint8_t
{
    kSubscribe = 0x3,
};

enum class MoqFilterType : std::uint8_t
{
    kLatestGroup = 0x1, //
    kLatestObject = 0x2,
    kAbsoluteStart = 0x3, // indicates start group/object are present
    kAbsoluteRange = 0x4  // indicates start and end group/object are present
};

struct LenValue
{
    std::vector<uint8_t> value;

    LenValue() = default;

    LenValue(const std::string& s) { value.assign(s.begin(), s.end()); }

    void operator=(const std::vector<uint8_t>& v) { value = v; }
};

struct MoqSubscribe
{
    // msg_type = 0x3
    uint64_t subscribe_id;
    uint64_t track_alias;
    LenValue name_space;
    LenValue track_name;
    MoqFilterType filter_type;

    uint64_t start_group{ 0 };  // optional based on filter type
    uint64_t start_object{ 0 }; // optional based on filter type
    uint64_t end_group{ 0 };    // optional based on filter type
    uint64_t end_object{ 0 };   // optional based on filter type

    uint64_t num_params;
    // optional track parameters - Not used for this test

    // ----- Internals -------------
    int pos{ 0 };

    /**
     * Decode subscribe from stream buffer
     *
     * @param sbuf        Stream buffer to read subscribe message from
     *
     * @return True if successfully read the full subscribe message, false if
     *    more data is needed.
     */
    bool Decode(qtransport::StreamBuffer<uint8_t>& sbuf)
    {
        switch (pos) {
            case 0: { // subscribe_id
                const auto val = sbuf.DecodeUintV();
                if (!val) {
                    return false;
                }
                subscribe_id = *val;
                ++pos;

                [[fallthrough]];
            }
            case 1: { // track_alias
                const auto val = sbuf.DecodeUintV();
                if (!val) {
                    return false;
                }
                track_alias = *val;
                ++pos;

                [[fallthrough]];
            }
            case 2: { // name_space
                const auto val = sbuf.DecodeBytes();
                if (!val) {
                    return false;
                }
                name_space = *val;
                ++pos;

                [[fallthrough]];
            }
            case 3: { // track_name
                const auto val = sbuf.DecodeBytes();
                if (!val) {
                    return false;
                }
                track_name = *val;
                ++pos;

                [[fallthrough]];
            }
            case 4: { // filter type
                const auto val = sbuf.DecodeUintV();
                if (!val) {
                    return false;
                }

                filter_type = static_cast<MoqFilterType>(*val);

                ++pos;

                [[fallthrough]];
            }
            case 5: { // start_group
                if (filter_type == MoqFilterType::kAbsoluteStart || filter_type == MoqFilterType::kAbsoluteRange) {
                    const auto val = sbuf.DecodeUintV();
                    if (!val) {
                        return false;
                    }
                    start_group = *val;
                }

                ++pos;

                [[fallthrough]];
            }
            case 6: { // start_object
                if (filter_type == MoqFilterType::kAbsoluteStart || filter_type == MoqFilterType::kAbsoluteRange) {
                    const auto val = sbuf.DecodeUintV();
                    if (!val) {
                        return false;
                    }
                    start_object = *val;
                }

                ++pos;

                [[fallthrough]];
            }
            case 7: { // end_group
                if (filter_type == MoqFilterType::kAbsoluteRange) {
                    const auto val = sbuf.DecodeUintV();
                    if (!val) {
                        return false;
                    }
                    end_group = *val;
                }

                ++pos;

                [[fallthrough]];
            }
            case 8: { // end_object
                if (filter_type == MoqFilterType::kAbsoluteRange) {
                    const auto val = sbuf.DecodeUintV();
                    if (!val) {
                        return false;
                    }
                    end_object = *val;
                }

                ++pos;

                [[fallthrough]];
            }

            case 9: { // num params
                const auto val = sbuf.DecodeUintV();
                if (!val) {
                    return false;
                }
                num_params = *val;
                ++pos;

                [[fallthrough]];
            }

            default:
                return true;
        }
    }
};

std::vector<std::uint8_t>&
operator<<(std::vector<uint8_t>& v, const std::vector<uint8_t>& o)
{
    v.insert(v.end(), o.begin(), o.end());
    return v;
}

std::vector<std::uint8_t>&
operator<<(std::vector<uint8_t>& v, const LenValue& lv)
{
    v << qtransport::ToUintV(lv.value.size());

    if (lv.value.size()) {
        v.insert(v.end(), lv.value.begin(), lv.value.end());
        ;
    }
    return v;
}

std::vector<std::uint8_t>&
operator<<(std::vector<uint8_t>& v, const MoqSubscribe& moqt_sub)
{
    v << qtransport::ToUintV(static_cast<uint64_t>(MoqMessageType::kSubscribe));
    v << qtransport::ToUintV(moqt_sub.subscribe_id);
    v << qtransport::ToUintV(moqt_sub.track_alias);
    v << moqt_sub.name_space;
    v << moqt_sub.track_name;
    v << qtransport::ToUintV(static_cast<uint64_t>(moqt_sub.filter_type));

    switch (moqt_sub.filter_type) {
        case MoqFilterType::kLatestGroup:
            [[fallthrough]];
        case MoqFilterType::kLatestObject:
            break;

        case MoqFilterType::kAbsoluteStart:
            v << qtransport::ToUintV(moqt_sub.start_group);
            v << qtransport::ToUintV(moqt_sub.start_object);
            break;

        case MoqFilterType::kAbsoluteRange:
            v << qtransport::ToUintV(moqt_sub.start_group);
            v << qtransport::ToUintV(moqt_sub.start_object);
            v << qtransport::ToUintV(moqt_sub.end_group);
            v << qtransport::ToUintV(moqt_sub.end_object);
            break;
    }

    v << qtransport::ToUintV(moqt_sub.num_params);

    return v;
}

TEST_CASE("StreamBuffer parse MOQT Subscribe")
{
    MoqSubscribe s_sub{ .subscribe_id = 100,
                        .track_alias = 1234567,
                        .name_space = std::string("moq://cisco.com/tim"),
                        .track_name = std::string("video/primary/best"),
                        .filter_type = MoqFilterType::kAbsoluteStart,
                        .start_group = 2002,
                        .start_object = 3003,
                        .end_group = 4004,
                        .end_object = 5005,
                        .num_params = 9001 };

    qtransport::StreamBuffer<uint8_t> sbuf;
    std::vector<uint8_t> net_data;
    net_data << s_sub;

    // Split data to mimic SFRAME transmission of a byte stream
    std::vector<std::vector<uint8_t>> data_slices;

    for (size_t i = 0; i < net_data.size(); i += 10) {
        const size_t len = i + 10 < net_data.size() ? i + 10 : i + (net_data.size() - i);
        data_slices.push_back({ net_data.begin() + i, net_data.begin() + len });
    }

    // Push slices to stream buffer while at the same time try to parse the subscribe
    std::optional<std::uint64_t> message_type;
    MoqSubscribe r_sub;

    for (auto& v : data_slices) {
        sbuf.Push(v);

        if (!message_type) {
            message_type = sbuf.DecodeUintV();
            CHECK_EQ(*message_type, static_cast<uint64_t>(MoqMessageType::kSubscribe));
        }

        if (r_sub.Decode(sbuf)) {
            CHECK_EQ(s_sub.subscribe_id, r_sub.subscribe_id);
            CHECK_EQ(s_sub.track_alias, r_sub.track_alias);
            CHECK_EQ(s_sub.start_group, r_sub.start_group);
            CHECK_EQ(s_sub.start_object, r_sub.start_object);
            CHECK_EQ(s_sub.num_params, r_sub.num_params);

            /*
            std::string ns (r_sub.name_space.value.begin(), r_sub.name_space.value.end());
            std::string tn (r_sub.track_name.value.begin(), r_sub.track_name.value.end());
            std::cout << "Decoded subscribe, "
                      << " subscribe_id: " << r_sub.subscribe_id
                      << " track_alias: " << r_sub.track_alias
                      << " namespace: " << ns
                      << " track name: " << tn
                      << " filter_type: " << static_cast<uint64_t>(r_sub.filter_type)
                      << " start_group: " << r_sub.start_group
                      << " start_object: " << r_sub.start_object
                      << " end_group: " << r_sub.end_group
                      << " end_object: " << r_sub.end_object
                      << " num_params: " << r_sub.num_params
                      << std::endl;
            */
            break;
        }
    }
}