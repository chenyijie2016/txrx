#include <atomic>
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/program_options.hpp>
#include <csignal>
#include <format>
#include <future>
#include <uhd/convert.hpp>
#include <uhd/exception.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <vector>
#include <zmq.hpp>
#include "usrp_protocol.pb.h"
#include "usrp_transceiver.h"


namespace po = boost::program_options;
namespace bip = boost::interprocess;

using std::string;

// Global flag for signal handling
std::atomic<bool> stop_signal_called;

void sig_int_handler(int) {
    stop_signal_called = true;
    UHD_LOG_INFO("SIGNAL", "SIGINT received, stopping...")
}

/**
 * 辅助函数：将 Protobuf 配置对象转换为 C++ 原生结构体
 * 这使得我们可以保持 UsrpTransceiver 类不做修改
 */
UsrpConfig ConvertConfig(const usrp_proto::UsrpConfig &proto_cfg) {
    UsrpConfig c;
    c.clock_source = proto_cfg.clock_source();
    c.time_source = proto_cfg.time_source();
    c.spb = proto_cfg.spb();
    c.delay = proto_cfg.delay();
    c.nsamps = proto_cfg.nsamps();

    // Helper lambda to copy repeated fields to std::vector
    auto copy_vec = [](const auto &proto_vec) {
        return std::vector<typename std::decay_t<decltype(proto_vec)>::value_type>(proto_vec.begin(), proto_vec.end());
    };

    // 显式转换类型不匹配的字段 (protobuf repeated uint32 -> vector<size_t>)
    c.tx_channels.assign(proto_cfg.tx_channels().begin(), proto_cfg.tx_channels().end());
    c.rx_channels.assign(proto_cfg.rx_channels().begin(), proto_cfg.rx_channels().end());

    // 直接复制匹配的字段
    c.tx_rates = copy_vec(proto_cfg.tx_rates());
    c.rx_rates = copy_vec(proto_cfg.rx_rates());
    c.tx_freqs = copy_vec(proto_cfg.tx_freqs());
    c.rx_freqs = copy_vec(proto_cfg.rx_freqs());
    c.tx_gains = copy_vec(proto_cfg.tx_gains());
    c.rx_gains = copy_vec(proto_cfg.rx_gains());
    c.tx_ants = copy_vec(proto_cfg.tx_ants());
    c.rx_ants = copy_vec(proto_cfg.rx_ants());
    c.tx_files = copy_vec(proto_cfg.tx_files());
    c.rx_files = copy_vec(proto_cfg.rx_files());

    return c;
}

int UHD_SAFE_MAIN(int argc, char *argv[]) {
    // 验证 Protobuf 库版本兼容性
    GOOGLE_PROTOBUF_VERIFY_VERSION;

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
        zmq::message_t request_msg;
        if (auto res = sock.recv(request_msg, zmq::recv_flags::none); !res)
            continue;

        // --- 1. 解析 Protobuf 请求 ---
        usrp_proto::Request req_proto;
        usrp_proto::Response reply_proto;

        // 默认状态
        reply_proto.set_status(usrp_proto::STATUS_UNKNOWN);

        bool parse_success = req_proto.ParseFromArray(request_msg.data(), request_msg.size());

        if (!parse_success) {
            UHD_LOG_ERROR("SERVER", "Failed to parse Protobuf message");
            reply_proto.set_status(usrp_proto::ERROR);
            reply_proto.set_msg("Protobuf parse error");
            string serialized_reply;
            reply_proto.SerializeToString(&serialized_reply);
            sock.send(zmq::buffer(serialized_reply), zmq::send_flags::none);
            continue;
        }


        try {
            if (req_proto.cmd() == usrp_proto::EXECUTE) {
                UsrpConfig config = ConvertConfig(req_proto.config());
                string tx_shm_name = req_proto.tx_shm_name();

                if (transceiver.ValidateConfiguration(config, false)) {
                    transceiver.ApplyConfiguration(config);
                } else {
                    reply_proto.set_status(usrp_proto::FAILED);
                    reply_proto.set_msg("Configuration validation failed");
                    string serialized_reply;
                    reply_proto.SerializeToString(&serialized_reply);
                    sock.send(zmq::buffer(serialized_reply), zmq::send_flags::none);
                    continue;
                }

                // 映射共享内存
                bip::shared_memory_object tx_shm(bip::open_only, tx_shm_name.c_str(), bip::read_only);
                bip::mapped_region tx_region(tx_shm, bip::read_only);

                size_t num_tx_ch = config.tx_channels.size();
                // 每个通道的采样数 = 总字节数 / (通道数 * 每个采样字节数)
                size_t samps_per_ch = tx_region.get_size() / (num_tx_ch * sizeof(complexf));

                // 拆分多通道数据
                std::vector<std::vector<complexf>> tx_buffs(num_tx_ch, std::vector<complexf>(samps_per_ch));
                complexf *raw_ptr = static_cast<complexf *>(tx_region.get_address());

                for (size_t i = 0; i < num_tx_ch; ++i) {
                    std::copy(raw_ptr + (i * samps_per_ch), raw_ptr + ((i + 1) * samps_per_ch), tx_buffs[i].begin());
                }

                UHD_LOG_INFO("SERVER", std::format("Loaded {} channels, {} samples per channel", num_tx_ch, samps_per_ch));

                // 执行收发 (与之前一致)
                auto tx_thread =
                        std::async(std::launch::async, &UsrpTransceiver::TransmitFromBuffer, &transceiver, std::ref(tx_buffs), std::ref(stop_signal_called));
                auto rx_future = std::async(std::launch::async, &UsrpTransceiver::ReceiveToBuffer, &transceiver, std::ref(stop_signal_called));

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

                complexf *rx_ptr = static_cast<complexf *>(rx_region.get_address());
                for (size_t i = 0; i < num_rx_ch; ++i) {
                    std::copy(rx_buffs[i].begin(), rx_buffs[i].end(), rx_ptr + i * rx_samps_per_ch);
                }

                // 设置 Protobuf 响应
                reply_proto.set_status(usrp_proto::SUCCESS);
                reply_proto.set_rx_shm_name(rx_shm_name);
                reply_proto.set_rx_nsamps_per_ch(rx_samps_per_ch);
                reply_proto.set_num_rx_ch(num_rx_ch);

            } else if (req_proto.cmd() == usrp_proto::RELEASE) {
                bip::shared_memory_object::remove("usrp_rx_shm");
                reply_proto.set_status(usrp_proto::RELEASED);
            } else {
                reply_proto.set_status(usrp_proto::ERROR);
                reply_proto.set_msg("Unknown command");
            }
        } catch (const std::exception &e) {
            reply_proto.set_status(usrp_proto::ERROR);
            reply_proto.set_msg(e.what());
            UHD_LOG_ERROR("SERVER", std::format("Exception: {}", e.what()));
        }
        string serialized_reply;
        if (!reply_proto.SerializeToString(&serialized_reply)) {
            UHD_LOG_ERROR("SERVER", "Failed to serialize reply");
        }
        sock.send(zmq::buffer(serialized_reply), zmq::send_flags::none);
    }

    // 清理 Protobuf 库资源 (可选，用于内存泄漏检测等)
    google::protobuf::ShutdownProtobufLibrary();
    UHD_LOG_INFO("SERVER", "Server shutting down gracefully.");

    return EXIT_SUCCESS;
}
