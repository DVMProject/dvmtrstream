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

// ---------------------------------------------------------------------------
//  Macros
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
//  Structure Declaration
// ---------------------------------------------------------------------------

struct plugin_t {
    Config *config;
};

/**
 * @brief Represents an individual stream audio chunk.
 */

struct audio_chunk_t {
    std::vector<uint8_t> data;
    int32_t tgid;
    int32_t src;
    
    audio_chunk_t(const uint8_t* chunk_data, size_t size, int32_t tg, int32_t source) 
        : data(chunk_data, chunk_data + size), tgid(tg), src(source) {}
};

/**
 * @brief Represents an individual call audio stream.
 */

struct stream_t {
    long TGID;
    std::string address;
    std::string short_name;
    long port;
    ip::udp::endpoint remote_endpoint;
    std::queue<audio_chunk_t> chunk_queue;
    std::mutex queue_mutex;
    bool silence_leader_injected = false;  // Track if silence leader has been added
    int32_t last_call_tgid = -1;  // Last talkgroup ID
    int32_t last_call_src = -1;   // Last source ID
};

std::vector<std::shared_ptr<stream_t>> streams;

/**
 * @brief Represents an dvmbridge endpoint group.
 */

 struct endpoint_group_t {
    std::string endpoint_key;  // "address:port"
    std::vector<std::shared_ptr<stream_t>> streams;
    std::shared_ptr<boost::asio::steady_timer> timer;
    size_t current_stream_index = 0;
    bool timer_active = false;
    int inter_stream_delay_ms = 0;  // Delay between switching streams (ms)
    bool switching_stream = false;  // True when in delay period between streams
    int silence_leader_ms = 0;  // Silence to inject before stream audio (ms)
};

std::vector<std::shared_ptr<endpoint_group_t>> endpoint_groups;

// ---------------------------------------------------------------------------
//  Class Declaration
// ---------------------------------------------------------------------------

/**
 * @brief Implements the actual DVM trunk-recorder dvmbridge stream plugin.
 */
class DVMTRStream : public Plugin_Api {
    typedef boost::asio::io_service io_service;
    io_service io;
    ip::udp::endpoint remote_endpoint;
    ip::udp::socket udp_socket{io};
    std::unique_ptr<std::thread> io_thread;
    std::unique_ptr<io_service::work> work_guard;

    int global_silence_leader = 0; // global silence leader in ms
    int global_inter_stream_delay = 0; // global inter-stream delay in ms

public:
    /**
     * @brief Initializes a new instance of the DVMTRStream class
     */
    DVMTRStream() { }

    /**
     * @brief Helper method to parse configuration data from JSON.
     * @param config_data 
     * @returns int 
     */
    int parse_config(json config_data) {
        std::map<std::string, std::shared_ptr<endpoint_group_t>> endpoint_map;

        // read global inter-stream delay if configured
        if (config_data.contains("interCallDelay")) {
            global_inter_stream_delay = config_data["interCallDelay"];
            BOOST_LOG_TRIVIAL(info) << "dvmtrstream: inter-stream/inter-call delay set to " << global_inter_stream_delay << "ms";
        }

        // read global silence leader if configured
        if (config_data.contains("silenceLeader")) {
            global_silence_leader = config_data["silenceLeader"];
            BOOST_LOG_TRIVIAL(info) << "dvmtrstream: silence leader set to " << global_silence_leader << "ms";
        }

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
                group->inter_stream_delay_ms = global_inter_stream_delay;
                group->silence_leader_ms = global_silence_leader;
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

    /**
     * @brief Audio streaming handler called by trunk-recorder for each audio buffer.
     * @param call 
     * @param recorder 
     * @param samples 
     * @param sampleCount 
     * @returns int 
     */
    int audio_stream(Call* call, Recorder* recorder, int16_t* samples, int sampleCount) {
        System* call_system = call->get_system();
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

        Recorder& local_recorder = *recorder;
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
                        int32_t remainingBytes = bytes % chunkSize;

                        uint8_t* sampleBytes = (uint8_t*)samples;

                        // queue chunks instead of sending immediately
                        std::lock_guard<std::mutex> lock(stream->queue_mutex);
                        
                        // are we injecting a silence leader?
                        if (global_silence_leader > 0) {
                            // check if call source or talkgroup changed
                            bool call_changed = (stream->last_call_tgid != call_tgid || stream->last_call_src != call_src);

                            // inject silence leader if configured and call changed
                            if (call_changed && !stream->silence_leader_injected) {
                                // find the endpoint group for this stream to get silence_leader_ms
                                int silence_leader_ms = 0;
                                for (auto& group : endpoint_groups) {
                                    for (auto& grp_stream : group->streams) {
                                        if (grp_stream.get() == stream.get()) {
                                            silence_leader_ms = group->silence_leader_ms;
                                            break;
                                        }
                                    }
                                    if (silence_leader_ms > 0) break;
                                }

                                if (silence_leader_ms > 0) {
                                    // each chunk is 20ms, calculate how many silence chunks needed
                                    int silence_chunks = (silence_leader_ms + 19) / 20;  // round up

                                    uint8_t silenceChunk[320];
                                    ::memset(silenceChunk, 0, sizeof(silenceChunk));

                                    for (int i = 0; i < silence_chunks; i++) {
                                        audio_chunk_t silent_chunk(silenceChunk, chunkSize, call_tgid, call_src);
                                        stream->chunk_queue.push(silent_chunk);
                                    }

                                    stream->silence_leader_injected = true;
                                    BOOST_LOG_TRIVIAL(debug) << "injected " << silence_chunks << " silence chunks (" 
                                                            << (silence_chunks * 20) << "ms) before audio for TGID " << TGID 
                                                            << " SRC " << call_src << " (call changed)";
                                }
                            }
                        }

                        // update last call tracking
                        stream->last_call_tgid = call_tgid;
                        stream->last_call_src = call_src;
                        
                        // queue complete chunks
                        for (int i = 0; i < totalChunks; i++) {
                            // calculate the starting position of the current chunk
                            uint8_t* curChunkStart = sampleBytes + (i * chunkSize);

                            // queue the chunk
                            audio_chunk_t audio_chunk(curChunkStart, chunkSize, call_tgid, call_src);
                            stream->chunk_queue.push(audio_chunk);
                        }

                        // handle partial chunk - pad with silence to full 320 bytes
                        if (remainingBytes > 0) {
                            uint8_t paddedChunk[320];
                            ::memset(paddedChunk, 0, sizeof(paddedChunk)); // Zero = silence for 16-bit audio

                            // copy partial audio data
                            uint8_t* partialStart = sampleBytes + (totalChunks * chunkSize);
                            ::memcpy(paddedChunk, partialStart, remainingBytes);

                            // queue padded chunk
                            audio_chunk_t audio_chunk(paddedChunk, chunkSize, call_tgid, call_src);
                            stream->chunk_queue.push(audio_chunk);

                            BOOST_LOG_TRIVIAL(debug) << "padded partial chunk: " << remainingBytes << " bytes + " 
                                                     << (chunkSize - remainingBytes) << " silence bytes for TGID " << TGID;
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

    /**
     * @brief Starts the plugin.
     * @returns int 
     */
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

    /**
     * @brief Stops the plugin.
     * @returns int 
     */
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
    /**
     * @brief Helper method to schedule the next send for an endpoint group.
     * @param group 
     */
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

    /**
     * @brief Helper method to send the next audio chunk for an endpoint group.
     * @param group 
     */
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

        // if the actual talkgroup is negative, set to 0
        if (chunk.tgid < 0) {
            dstId = 0;
        }

        // limit to 24 bits
        if (dstId > 0xFFFFFF) {
            dstId = 0;
        }

        SET_UINT32(dstId, chunkBuffer, 4 + chunk.data.size());

        uint32_t srcId = static_cast<uint32_t>(chunk.src);

        // if the actual source is negative, set to 0
        if (chunk.src < 0) {
            srcId = 0;
        }

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
            // reset silence leader flag for next transmission
            active_stream->silence_leader_injected = false;

            // check if inter-stream delay is configured
            if (group->inter_stream_delay_ms > 0) {
                // enter delay period before switching to next stream
                group->switching_stream = true;
                BOOST_LOG_TRIVIAL(debug) << "endpoint " << group->endpoint_key 
                                         << ": Stream TGID " << active_stream->TGID 
                                         << " complete, delaying " << group->inter_stream_delay_ms << "ms before next stream";

                // schedule with delay
                if (group->timer && group->timer_active) {
                    group->timer->expires_from_now(std::chrono::milliseconds(group->inter_stream_delay_ms));
                    group->timer->async_wait([this, group](const boost::system::error_code& error) {
                        if (!error && group->timer_active) {
                            group->switching_stream = false;
                            group->current_stream_index = (group->current_stream_index + 1) % group->streams.size();
                            send_next_chunk(group);
                        }
                    });
                }
                return;  // don't schedule normal send, we scheduled the delayed one
            } else {
                // no delay, switch immediately
                group->current_stream_index = (group->current_stream_index + 1) % group->streams.size();
            }
        }

        // schedule next send for this endpoint group
        schedule_next_send(group);
    }
};

BOOST_DLL_ALIAS(
    DVMTRStream::create, // <-- this function is exported with...
    create_plugin        // <-- ...this alias name
)
