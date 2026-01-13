#include <atomic>
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/program_options.hpp>
#include <csignal>
#include <format>
#include <future>
#include <nlohmann/json.hpp>
#include <uhd/convert.hpp>
#include <uhd/exception.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <vector>
#include <zmq.hpp>
#include "usrp_transceiver.h"


namespace po = boost::program_options;
namespace bip = boost::interprocess;
using json = nlohmann::json;
using std::string;

// Global flag for signal handling
std::atomic<bool> stop_signal_called;

void sig_int_handler(int) {
    stop_signal_called = true;
    UHD_LOG_INFO("SIGNAL", "SIGINT received, stopping...")
}

// Custom JSON deserializer for UsrpConfig
void from_json(const json& j, UsrpConfig& c) {
    j.at("tx_channels").get_to(c.tx_channels);
    j.at("rx_channels").get_to(c.rx_channels);
    j.at("spb").get_to(c.spb);
    j.at("delay").get_to(c.delay);
    j.at("nsamps").get_to(c.nsamps);
    j.at("tx_rates").get_to(c.tx_rates);
    j.at("rx_rates").get_to(c.rx_rates);
    j.at("tx_freqs").get_to(c.tx_freqs);
    j.at("rx_freqs").get_to(c.rx_freqs);
    j.at("tx_gains").get_to(c.tx_gains);
    j.at("rx_gains").get_to(c.rx_gains);
    j.at("tx_ants").get_to(c.tx_ants);
    j.at("rx_ants").get_to(c.rx_ants);
    j.at("clock_source").get_to(c.clock_source);
    j.at("time_source").get_to(c.time_source);
}

int UHD_SAFE_MAIN(int argc, char *argv[]) {
    string args;
    uint16_t port;

    po::options_description desc("Command line options");
    auto option = desc.add_options();
    option("help,h", "Show this help message");
    option("port", po::value<uint16_t>(&port)->default_value(5555), "zmq port number");
    option("args", po::value<string>(&args)->default_value("addr=192.168.180.2"), "USRP device args");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception &e) {
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, &sig_int_handler);
    UsrpTransceiver transceiver(args);

    zmq::context_t ctx{1};
    zmq::socket_t sock{ctx, zmq::socket_type::rep};
    sock.bind(std::format("tcp://*:{}", port));

    UHD_LOG_INFO("SERVER", std::format("ZMQ Server live on port {}", port));

    while (not stop_signal_called) {
        zmq::message_t request;
        auto res = sock.recv(request, zmq::recv_flags::none);
        if (!res) continue;

        json req_json = json::parse(request.to_string());
        string cmd = req_json.value("cmd", "");
        json reply;

        try {
            if (cmd == "EXECUTE") {
                UsrpConfig config = req_json.at("config").get<UsrpConfig>();
                string tx_shm_name = req_json.at("tx_shm_name").get<string>();

                transceiver.ApplyConfiguration(config);

                // 映射共享内存
                bip::shared_memory_object tx_shm(bip::open_only, tx_shm_name.c_str(), bip::read_only);
                bip::mapped_region tx_region(tx_shm, bip::read_only);

                size_t num_tx_ch = config.tx_channels.size();
                // 每个通道的采样数 = 总字节数 / (通道数 * 每个采样字节数)
                size_t samps_per_ch = tx_region.get_size() / (num_tx_ch * sizeof(complexf));

                // 拆分多通道数据
                std::vector<std::vector<complexf>> tx_buffs(num_tx_ch, std::vector<complexf>(samps_per_ch));
                complexf* raw_ptr = static_cast<complexf*>(tx_region.get_address());

                for(size_t i = 0; i < num_tx_ch; ++i) {
                    std::copy(raw_ptr + (i * samps_per_ch),
                              raw_ptr + ((i + 1) * samps_per_ch),
                              tx_buffs[i].begin());
                }

                UHD_LOG_INFO("SERVER", std::format("Loaded {} channels, {} samples per channel", num_tx_ch, samps_per_ch));

                // 执行收发 (与之前一致)
                auto tx_thread = std::async(std::launch::async, &UsrpTransceiver::TransmitFromBuffer,
                                            &transceiver, std::ref(tx_buffs), std::ref(stop_signal_called));
                auto rx_future = std::async(std::launch::async, &UsrpTransceiver::ReceiveToBuffer,
                                            &transceiver, std::ref(stop_signal_called));

                tx_thread.wait();
                auto rx_buffs = rx_future.get();

                // --- 3. 写入 RX 共享内存 (多通道) ---
                string rx_shm_name = "usrp_rx_shm";
                bip::shared_memory_object::remove(rx_shm_name.c_str());

                size_t num_rx_ch = rx_buffs.size();
                size_t rx_samps_per_ch = rx_buffs[0].size();
                size_t total_rx_bytes = num_rx_ch * rx_samps_per_ch * sizeof(complexf);

                bip::shared_memory_object rx_shm(bip::create_only, rx_shm_name.c_str(), bip::read_write);
                rx_shm.truncate(total_rx_bytes);
                bip::mapped_region rx_region(rx_shm, bip::read_write);

                complexf* rx_ptr = static_cast<complexf*>(rx_region.get_address());
                for(size_t i = 0; i < num_rx_ch; ++i) {
                    std::copy(rx_buffs[i].begin(), rx_buffs[i].end(), rx_ptr + i * rx_samps_per_ch);
                }

                reply["status"] = "SUCCESS";
                reply["rx_shm_name"] = rx_shm_name;
                reply["rx_nsamps_per_ch"] = rx_samps_per_ch;
                reply["num_rx_ch"] = num_rx_ch;
            }
            else if (cmd == "RELEASE") {
                bip::shared_memory_object::remove("usrp_rx_shm");
                reply["status"] = "RELEASED";
            }
        } catch (const std::exception& e) {
            reply["status"] = "ERROR";
            reply["msg"] = e.what();
        }
        sock.send(zmq::buffer(reply.dump()), zmq::send_flags::none);
    }
    return EXIT_SUCCESS;
}