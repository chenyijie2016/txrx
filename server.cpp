#include <atomic>
#include <csignal>
#include <format>
#include <future>
#include <vector>
#include <zmq.hpp>

// POSIX headers for Shared Memory
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <uhd/convert.hpp>
#include <uhd/exception.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <boost/program_options.hpp>

#include "usrp_protocol.pb.h"
#include "usrp_transceiver.h"

namespace po = boost::program_options;
using std::string;

std::atomic<bool> stop_signal_called(false);

void sig_int_handler(int) {
    stop_signal_called = true;
    UHD_LOG_INFO("SIGNAL", "SIGINT received, stopping...")
}

// 辅助函数：将 Protobuf 配置转换为原生结构体
UsrpConfig ConvertConfig(const usrp_proto::UsrpConfig &proto_cfg) {
    UsrpConfig c;
    c.clock_source = proto_cfg.clock_source();
    c.time_source = proto_cfg.time_source();
    c.spb = proto_cfg.spb();
    c.delay = proto_cfg.delay();
    c.rx_samps = proto_cfg.rx_samps();
    c.tx_samps = proto_cfg.tx_samps();

    c.tx_channels.assign(proto_cfg.tx_channels().begin(), proto_cfg.tx_channels().end());
    c.rx_channels.assign(proto_cfg.rx_channels().begin(), proto_cfg.rx_channels().end());
    c.tx_rates.assign(proto_cfg.tx_rates().begin(), proto_cfg.tx_rates().end());
    c.rx_rates.assign(proto_cfg.rx_rates().begin(), proto_cfg.rx_rates().end());
    c.tx_freqs.assign(proto_cfg.tx_freqs().begin(), proto_cfg.tx_freqs().end());
    c.rx_freqs.assign(proto_cfg.rx_freqs().begin(), proto_cfg.rx_freqs().end());
    c.tx_gains.assign(proto_cfg.tx_gains().begin(), proto_cfg.tx_gains().end());
    c.rx_gains.assign(proto_cfg.rx_gains().begin(), proto_cfg.rx_gains().end());
    c.tx_ants.assign(proto_cfg.tx_ants().begin(), proto_cfg.tx_ants().end());
    c.rx_ants.assign(proto_cfg.rx_ants().begin(), proto_cfg.rx_ants().end());


    UHD_LOG_DEBUG("CONFIG", std::format(
        "Converted Config - Clock: {}, Time: {}, SPB: {}, Delay: {}, RX Samps: {}, TX Samps: {}",
        c.clock_source, c.time_source, c.spb, c.delay, c.rx_samps, c.tx_samps
    ));

    return c;
}

int UHD_SAFE_MAIN(int argc, char *argv[]) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    string args;
    uint16_t port;

    po::options_description desc("Command line options");
    desc.add_options()
        ("help,h", "Show help")
        ("port", po::value<uint16_t>(&port)->default_value(5555))
        ("args", po::value<string>(&args)->default_value("addr=192.168.10.101"));

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    std::signal(SIGINT, &sig_int_handler);
    UsrpTransceiver transceiver(args);

    zmq::context_t ctx{1};
    zmq::socket_t sock{ctx, zmq::socket_type::rep};
    sock.bind(std::format("tcp://*:{}", port));

    UHD_LOG_INFO("SERVER", std::format("ZMQ Server live on port {} (POSIX SHM Mode)", port));

    while (not stop_signal_called) {
        zmq::message_t request_msg;
        if (auto res = sock.recv(request_msg, zmq::recv_flags::none); !res || request_msg.size() == 0)
            continue;

        usrp_proto::Request req_proto;
        usrp_proto::Response reply_proto;
        reply_proto.set_status(usrp_proto::STATUS_UNKNOWN);

        if (!req_proto.ParseFromArray(request_msg.data(), request_msg.size())) {
            UHD_LOG_ERROR("SERVER", "Protobuf parse error");
            continue;
        }

        try {
            if (req_proto.cmd() == usrp_proto::EXECUTE) {
                UsrpConfig config = ConvertConfig(req_proto.config());
                string tx_shm_name = req_proto.tx_shm_name();

                if (!transceiver.ValidateConfiguration(config, false)) {
                    throw std::runtime_error("Configuration validation failed");
                }
                transceiver.ApplyConfiguration(config);

                // --- 1. 使用 POSIX 接口打开 TX 共享内存 ---
                UHD_LOG_INFO("SERVER", std::format("Opening TX SHM: {}", tx_shm_name));
                int tx_fd = shm_open(tx_shm_name.c_str(), O_RDONLY, 0666);
                if (tx_fd == -1) throw std::runtime_error(std::format("shm_open TX failed: {}", strerror(errno)));

                struct stat tx_st;
                fstat(tx_fd, &tx_st);
                
                void* tx_ptr = mmap(nullptr, tx_st.st_size, PROT_READ, MAP_SHARED, tx_fd, 0);
                if (tx_ptr == MAP_FAILED) {
                    close(tx_fd);
                    throw std::runtime_error("mmap TX failed");
                }

                // 拆分通道数据
                size_t num_tx_ch = config.tx_channels.size();
                // size_t samps_per_ch = tx_st.st_size / (num_tx_ch * sizeof(complexf));
                std::vector<std::vector<complexf>> tx_buffs(num_tx_ch, std::vector<complexf>(config.tx_samps));
                complexf *raw_tx_ptr = static_cast<complexf *>(tx_ptr);

                for (size_t i = 0; i < num_tx_ch; ++i) {
                    std::copy(raw_tx_ptr + (i * config.tx_samps), raw_tx_ptr + ((i + 1) * config.tx_samps), tx_buffs[i].data());
                }

                // 拷贝完后即可解除映射
                munmap(tx_ptr, tx_st.st_size);
                close(tx_fd);

                // --- 2. 执行收发 ---
                auto tx_thread = std::async(std::launch::async, &UsrpTransceiver::TransmitFromBuffer, &transceiver, std::ref(tx_buffs), std::ref(stop_signal_called));
                auto rx_future = std::async(std::launch::async, &UsrpTransceiver::ReceiveToBuffer, &transceiver, std::ref(stop_signal_called));

                tx_thread.wait();
                auto rx_buffs = rx_future.get();

                // --- 3. 使用 POSIX 接口创建 RX 共享内存 ---
                string rx_shm_name = "/usrp_rx_shm";
                shm_unlink(rx_shm_name.c_str()); // 确保干净

                size_t num_rx_ch = rx_buffs.size();
                size_t rx_samps_per_ch = rx_buffs[0].size();
                size_t total_rx_bytes = num_rx_ch * rx_samps_per_ch * sizeof(complexf);

                int rx_fd = shm_open(rx_shm_name.c_str(), O_CREAT | O_RDWR, 0666);
                if (rx_fd == -1) throw std::runtime_error("shm_open RX failed");
                ftruncate(rx_fd, total_rx_bytes);

                void* rx_ptr = mmap(nullptr, total_rx_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, rx_fd, 0);
                complexf *raw_rx_ptr = static_cast<complexf *>(rx_ptr);
                
                for (size_t i = 0; i < num_rx_ch; ++i) {
                    std::copy(rx_buffs[i].begin(), rx_buffs[i].end(), raw_rx_ptr + i * rx_samps_per_ch);
                }

                munmap(rx_ptr, total_rx_bytes);
                close(rx_fd);

                reply_proto.set_status(usrp_proto::SUCCESS);
                reply_proto.set_rx_shm_name(rx_shm_name);
                reply_proto.set_rx_nsamps_per_ch(rx_samps_per_ch);
                reply_proto.set_num_rx_ch(num_rx_ch);

            } else if (req_proto.cmd() == usrp_proto::RELEASE) {
                shm_unlink("/usrp_rx_shm");
                reply_proto.set_status(usrp_proto::RELEASED);
            }
        } catch (const std::exception &e) {
            reply_proto.set_status(usrp_proto::ERROR);
            reply_proto.set_msg(e.what());
            UHD_LOG_ERROR("SERVER", std::format("Exception: {}", e.what()));
        }

        string serialized_reply;
        reply_proto.SerializeToString(&serialized_reply);
        sock.send(zmq::buffer(serialized_reply), zmq::send_flags::none);
    }

    google::protobuf::ShutdownProtobufLibrary();
    return EXIT_SUCCESS;
}