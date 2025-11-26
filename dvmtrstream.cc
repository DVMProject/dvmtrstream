#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../../trunk-recorder/recorders/recorder.h"
#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/dll/alias.hpp> // for BOOST_DLL_ALIAS
#include <boost/foreach.hpp>

using namespace boost::asio;

typedef struct plugin_t plugin_t;
typedef struct stream_t stream_t;
std::vector<stream_t> streams;

struct plugin_t {
    Config *config;
};

struct stream_t {
    long TGID;
    long tcp_index;
    std::string address;
    std::string short_name;
    long port;
    ip::udp::endpoint remote_endpoint;
    bool sendTGID = false;
    bool sendJSON = false;
    bool sendCallStart = false;
    bool sendCallEnd = false;
    bool tcp = false;
};

class DVMTRStream : public Plugin_Api {
    typedef boost::asio::io_service io_service;
    io_service io;
    ip::udp::endpoint remote_endpoint;
    ip::udp::socket udp_socket{io};

public:
    DVMTRStream() {
    }

    int parse_config(json config_data) {
        for (json element : config_data["streams"]) {
            stream_t stream;
            stream.TGID = element["TGID"];
            stream.address = element["address"];
            stream.port = element["port"];
            stream.short_name = element["shortName"];   // we require shortName because dvmbridge can only handle one system ID at a time
            stream.remote_endpoint = ip::udp::endpoint(ip::address::from_string(stream.address), stream.port);
            BOOST_LOG_TRIVIAL(info) << "dvmtrstream will stream audio from TGID " << stream.TGID << " on System " << stream.short_name << " to " << stream.address << " on port " << stream.port << " tcp is " << stream.tcp;
            streams.push_back(stream);
        }
        return 0;
    }

    int audio_stream(Call *call, Recorder *recorder, int16_t *samples, int sampleCount) {

        System *call_system = call->get_system();
        int32_t call_tgid = call->get_talkgroup();
        int32_t call_src = call->get_current_source_id();
        std::string call_short_name = call->get_short_name();
        std::vector<unsigned long> unsigned_patched_talkgroups = call_system->get_talkgroup_patch(call_tgid);
        std::vector<long> patched_talkgroups;
        // Convert unsigned long to signed long, preserving negative values
        for (auto tgid : unsigned_patched_talkgroups) {
            patched_talkgroups.push_back(static_cast<long>(tgid));
        }

        if (call_src == -1) {
            if (call->get_transmissions().size() > 0) {
                // Get the source from the most recent transmission
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
        boost::system::error_code error;

        // Ignore audio if it's not 8kHz (TODO: resample if it is)
        if (wav_hz != 8000)
        {
            BOOST_LOG_TRIVIAL(warning) << "ignoring audio at " << wav_hz << " Hz samplerate, not currently supported!";
            return 1;
        }

        BOOST_FOREACH (auto stream, streams) {
            if (0 == stream.short_name.compare(call_short_name) || (0 == stream.short_name.compare(""))) { // Check if shortName matches or is not specified
                if (patched_talkgroups.size() == 0) {
                    patched_talkgroups.push_back(call_tgid); // call_info.talkgroup may be negative - we cast stream.TGID to signed for comparison
                }
                BOOST_FOREACH (auto TGID, patched_talkgroups) {
                    if ((TGID == static_cast<long>(stream.TGID))) {
                        BOOST_LOG_TRIVIAL(debug) << "got " << sampleCount << " samples - " << sampleCount * 2 << " bytes from recorder " << recorder_id << " for TGID " << TGID;
                        // Create our data buffer to send
                        std::vector<boost::asio::const_buffer> send_buffer;
                        // Add the audio sample length (uint32)
                        send_buffer.push_back(buffer(&sampleCount, 4));
                        // Add the audio samples
                        send_buffer.push_back(buffer(samples, sampleCount * 2));
                        // Add the source ID (uint32)
                        send_buffer.push_back(buffer(&call_src, 4));
                        // Send the buffer
                        udp_socket.send_to(send_buffer, stream.remote_endpoint, 0, error);
                    }
                }
            }
        }
        return 0;
    }

    int start() {
        udp_socket.open(ip::udp::v4());
        return 0;
    }

    int stop() {
        udp_socket.close();
        return 0;
    }

    static boost::shared_ptr<DVMTRStream> create() {
        return boost::shared_ptr<DVMTRStream>(
            new DVMTRStream());
    }
    };

BOOST_DLL_ALIAS(
    DVMTRStream::create, // <-- this function is exported with...
    create_plugin          // <-- ...this alias name
)
