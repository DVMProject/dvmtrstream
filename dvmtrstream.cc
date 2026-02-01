// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Trunk Recorder Stream Plugin
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2025,2026 Patrick McDonnell, W3AXL
 *  Copyright (C) 2026 Bryan Biedenkapp, N2PLL
 *
 */
#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../../trunk-recorder/recorders/recorder.h"
#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/dll/alias.hpp> // for BOOST_DLL_ALIAS
#include <boost/foreach.hpp>
#include <boost/endian/conversion.hpp>
#include <queue>
#include <mutex>
#include <thread>
#include <chrono>
#include <memory>

using namespace boost::asio;

typedef struct plugin_t plugin_t;
typedef struct stream_t stream_t;

struct audio_chunk_t {
    std::vector<uint8_t> data;
    int32_t tgid;
    int32_t src;
    
    audio_chunk_t(const uint8_t* chunk_data, size_t size, int32_t tg, int32_t source) 
        : data(chunk_data, chunk_data + size), tgid(tg), src(source) {}
};

struct plugin_t {
    Config *config;
};

struct stream_t {
    long TGID;
    std::string address;
    std::string short_name;
    long port;
    ip::udp::endpoint remote_endpoint;
    std::queue<audio_chunk_t> chunk_queue;
    std::mutex queue_mutex;
};

std::vector<std::shared_ptr<stream_t>> streams;

struct endpoint_group_t {
    std::string endpoint_key;  // "address:port"
    std::vector<std::shared_ptr<stream_t>> streams;
    std::shared_ptr<boost::asio::steady_timer> timer;
    size_t current_stream_index = 0;
    bool timer_active = false;
};

std::vector<std::shared_ptr<endpoint_group_t>> endpoint_groups;

/**
 * @brief Sets a uint32_t into 4 bytes of a buffer/array. (32-bit value).
 * @ingroup common
 * @param val uint32_t value to set
 * @param buffer uint8_t buffer to set value on
 * @param offset Offset within uint8_t buffer
 */
#define SET_UINT32(val, buffer, offset)                 \
            buffer[0U + offset] = (val >> 24) & 0xFFU;  \
            buffer[1U + offset] = (val >> 16) & 0xFFU;  \
            buffer[2U + offset] = (val >> 8) & 0xFFU;   \
            buffer[3U + offset] = (val >> 0) & 0xFFU;

class DVMTRStream : public Plugin_Api {
    typedef boost::asio::io_service io_service;
    io_service io;
    ip::udp::endpoint remote_endpoint;
    ip::udp::socket udp_socket{io};
    std::unique_ptr<std::thread> io_thread;
    std::unique_ptr<io_service::work> work_guard;

public:
    DVMTRStream() { }

    int parse_config(json config_data) {
        std::map<std::string, std::shared_ptr<endpoint_group_t>> endpoint_map;
        
        for (json element : config_data["streams"]) {
            auto stream = std::make_shared<stream_t>();
            stream->TGID = element["TGID"];
            stream->address = element["address"];
            stream->port = element["port"];
            stream->short_name = element["shortName"];   // we require shortName because dvmbridge can only handle one system ID at a time
            stream->remote_endpoint = ip::udp::endpoint(ip::address::from_string(stream->address), stream->port);
            
            // create endpoint key
            std::string endpoint_key = stream->address + ":" + std::to_string(stream->port);
            
            // find or create endpoint group
            if (endpoint_map.find(endpoint_key) == endpoint_map.end()) {
                auto group = std::make_shared<endpoint_group_t>();
                group->endpoint_key = endpoint_key;
                endpoint_map[endpoint_key] = group;
                endpoint_groups.push_back(group);
                BOOST_LOG_TRIVIAL(info) << "dvmtrstream: created endpoint group for " << endpoint_key;
            }
            
            endpoint_map[endpoint_key]->streams.push_back(stream);
            streams.push_back(stream);
            
            BOOST_LOG_TRIVIAL(info) << "dvmtrstream: will stream audio from TGID " << stream->TGID << " on System " << stream->short_name << " to " << stream->address << " on port " << stream->port;
        }
        
        BOOST_LOG_TRIVIAL(info) << "dvmtrstream: configured " << endpoint_groups.size() << " endpoint group(s) with " << streams.size() << " total stream(s)";
        return 0;
    }

    int audio_stream(Call *call, Recorder *recorder, int16_t *samples, int sampleCount) {
        System *call_system = call->get_system();
        int32_t call_tgid = call->get_talkgroup();
        int32_t call_src = call->get_current_source_id();
        std::string call_short_name = call->get_short_name();
        std::vector<unsigned long> unsigned_patched_talkgroups = call_system->get_talkgroup_patch(call_tgid);
        std::vector<long> patched_talkgroups;

        // convert unsigned long to signed long, preserving negative values
        for (auto tgid : unsigned_patched_talkgroups) {
            patched_talkgroups.push_back(static_cast<long>(tgid));
        }

        if (call_src == -1) {
            if (call->get_transmissions().size() > 0) {
                // get the source from the most recent transmission
                auto transmissions = call->get_transmissions();
                call_src = transmissions.back().source;
                BOOST_LOG_TRIVIAL(debug) << "using source " << call_src << " from most recent transmission";
            } else {
                BOOST_LOG_TRIVIAL(debug) << "no source found for call - leaving src as -1";
            }
        }

        Recorder &local_recorder = *recorder;
        int recorder_id = local_recorder.get_num();
        long wav_hz = local_recorder.get_wav_hz();

        // ignore audio if it's not 8kHz (TODO: resample if it is)
        if (wav_hz != 8000) {
            BOOST_LOG_TRIVIAL(warning) << "ignoring audio at " << wav_hz << " Hz samplerate, not currently supported!";
            return 1;
        }

        BOOST_FOREACH (auto& stream, streams) {
            if (0 == stream->short_name.compare(call_short_name) || (0 == stream->short_name.compare(""))) { // Check if shortName matches or is not specified
                if (patched_talkgroups.size() == 0) {
                    patched_talkgroups.push_back(call_tgid); // call_info.talkgroup may be negative - we cast stream.TGID to signed for comparison
                }
                BOOST_FOREACH (auto TGID, patched_talkgroups) {
                    if ((TGID == static_cast<long>(stream->TGID))) {
                        BOOST_LOG_TRIVIAL(debug) << "got " << sampleCount << " samples - " << sampleCount * 2 << " bytes from recorder " << recorder_id << " for TGID " << TGID;

                        int32_t bytes = sampleCount * 2;
                        const int16_t chunkSize = 320; // 20ms of 8kHz 16-bit audio (160 samples * 2 bytes)
                        int16_t totalChunks = bytes / chunkSize;

                        uint8_t* sampleBytes = (uint8_t*)samples;

                        // Queue chunks instead of sending immediately
                        std::lock_guard<std::mutex> lock(stream->queue_mutex);
                        for (int i = 0; i < totalChunks; i++) {
                            // Calculate the starting position of the current chunk
                            uint8_t* curChunkStart = sampleBytes + (i * chunkSize);

                            // Create and queue the chunk
                            audio_chunk_t audio_chunk(curChunkStart, chunkSize, call_tgid, call_src);
                            stream->chunk_queue.push(audio_chunk);
                        }
                        
                        size_t queue_size = stream->chunk_queue.size();
                        if (queue_size > 50) {
                            BOOST_LOG_TRIVIAL(warning) << "stream queue for TGID " << TGID << " has " << queue_size << " chunks (" << queue_size * 20 << "ms backlog)";
                        }
                    }
                }
            }
        }
        return 0;
    }

    int start() {
        udp_socket.open(ip::udp::v4());
        
        // create work guard to keep io_service running
        work_guard = std::make_unique<io_service::work>(io);
        
        // initialize timer for each endpoint group
        for (auto& group : endpoint_groups) {
            group->timer = std::make_shared<boost::asio::steady_timer>(io);
            group->timer_active = true;
            schedule_next_send(group.get());
        }
        
        // start io_service in a separate thread
        io_thread = std::make_unique<std::thread>([this]() {
            BOOST_LOG_TRIVIAL(info) << "dvmtrstream: starting io_service thread";
            io.run();
            BOOST_LOG_TRIVIAL(info) << "dvmtrstream: io_service thread stopped";
        });
        
        return 0;
    }

    int stop() {
        // stop all endpoint group timers
        for (auto& group : endpoint_groups) {
            group->timer_active = false;
            if (group->timer) {
                group->timer->cancel();
            }
        }
        
        // stop io_service
        work_guard.reset();
        io.stop();
        
        // wait for io_thread to finish
        if (io_thread && io_thread->joinable()) {
            io_thread->join();
        }
        
        udp_socket.close();
        return 0;
    }

    static boost::shared_ptr<DVMTRStream> create() {
        return boost::shared_ptr<DVMTRStream>(
            new DVMTRStream());
    }

private:
    void schedule_next_send(endpoint_group_t* group) {
        if (!group || !group->timer || !group->timer_active) 
            return;

        group->timer->expires_from_now(std::chrono::milliseconds(15));
        group->timer->async_wait([this, group](const boost::system::error_code& error) {
            if (!error && group->timer_active) {
                send_next_chunk(group);
            }
        });
    }

    void send_next_chunk(endpoint_group_t* group) {
        if (!group || group->streams.empty()) {
            schedule_next_send(group);
            return;
        }

        // find next stream with data in this endpoint group, starting from current_stream_index
        stream_t* active_stream = nullptr;
        size_t start_index = group->current_stream_index;
        
        for (size_t i = 0; i < group->streams.size(); i++) {
            size_t check_index = (start_index + i) % group->streams.size();
            auto& stream = group->streams[check_index];
            
            std::lock_guard<std::mutex> lock(stream->queue_mutex);
            if (!stream->chunk_queue.empty()) {
                // found stream with data - check if it's the current one or need to switch
                if (check_index != group->current_stream_index) {
                    group->current_stream_index = check_index;
                    BOOST_LOG_TRIVIAL(debug) << "endpoint " << group->endpoint_key << ": Switching to stream TGID " << stream->TGID;
                }
                active_stream = stream.get();
                break;
            }
        }

        if (!active_stream) {
            // no streams have data in this group, check again later
            schedule_next_send(group);
            return;
        }

        // send chunk from active stream
        std::unique_lock<std::mutex> lock(active_stream->queue_mutex);
        audio_chunk_t chunk = active_stream->chunk_queue.front();
        active_stream->chunk_queue.pop();
        bool has_more = !active_stream->chunk_queue.empty();
        lock.unlock();

        // send the chunk
        boost::system::error_code error;

        uint8_t chunkBuffer[332];
        ::memset(chunkBuffer, 0, sizeof(chunkBuffer));
        SET_UINT32(chunk.data.size(), chunkBuffer, 0);
        std::memcpy(chunkBuffer + 4, chunk.data.data(), chunk.data.size());

        uint32_t dstId = static_cast<uint32_t>(chunk.tgid);
        
        // limit to 24 bits
        if (dstId > 0xFFFFFF) {
            dstId = 0;
        }

        SET_UINT32(dstId, chunkBuffer, 4 + chunk.data.size());

        uint32_t srcId = static_cast<uint32_t>(chunk.src);

        // limit to 24 bits
        if (srcId > 0xFFFFFF) {
            srcId = 0;
        }

        SET_UINT32(srcId, chunkBuffer, 8 + chunk.data.size());

        std::vector<boost::asio::const_buffer> txBuffer;
        txBuffer.push_back(buffer(chunkBuffer, sizeof(chunkBuffer)));

        udp_socket.send_to(txBuffer, active_stream->remote_endpoint, 0, error);

        if (error) {
            BOOST_LOG_TRIVIAL(error) << "error sending UDP packet: " << error.message();
        }

        // if current stream is empty, move to next stream on next cycle
        if (!has_more) {
            group->current_stream_index = (group->current_stream_index + 1) % group->streams.size();
        }

        // schedule next send for this endpoint group
        schedule_next_send(group);
    }
};

BOOST_DLL_ALIAS(
    DVMTRStream::create, // <-- this function is exported with...
    create_plugin        // <-- ...this alias name
)
