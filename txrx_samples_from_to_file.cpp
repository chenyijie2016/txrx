//
// txrx_fc32_sync.cpp
// 同时发送和接收示例程序 - 基于官方示例优化
// 接收在主线程，发送在独立线程
//

#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <uhd/convert.hpp>
#include <uhd/exception.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <format>
#include <chrono>
#include <complex>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <algorithm>
#include <future>
#include <iomanip>
#include "workers.h"
namespace po = boost::program_options;
namespace fs = std::filesystem;

using complex_t = std::complex<float>; // 固定使用fc32格式

// static std::atomic<bool> stop_signal_called(false);

void sig_int_handler(int) {
    stop_signal_called = true;
}


//
// // 发送线程函数 - 从预加载的缓冲区发送
// void transmit_from_buffer(
//     uhd::tx_streamer::sptr tx_stream,
//     const std::vector<complex_t> &tx_buffer,
//     size_t total_samples,
//     size_t samps_per_buff,
//     const uhd::time_spec_t &start_time,
//     std::atomic<bool> &tx_done) {
//     uhd::tx_metadata_t md;
//     md.start_of_burst = false;
//     md.end_of_burst = false;
//     md.has_time_spec = true;
//     md.time_spec = start_time;
//
//     // 准备缓冲区指针
//     std::vector<complex_t> send_buff(samps_per_buff);
//     std::vector<complex_t *> buffs(tx_stream->get_num_channels(), &send_buff.front());
//
//     size_t samples_sent = 0;
//     bool first_packet = true;
//     const auto start_clock = std::chrono::steady_clock::now();
//
//     while (samples_sent < total_samples && !stop_signal_called) {
//         size_t samples_to_send = std::min(samps_per_buff, total_samples - samples_sent);
//
//         // 从预加载缓冲区复制数据到发送缓冲区
//         std::copy(tx_buffer.begin() + samples_sent,
//                   tx_buffer.begin() + samples_sent + samples_to_send,
//                   send_buff.begin());
//
//         md.end_of_burst = (samples_sent + samples_to_send >= total_samples);
//
//         size_t sent_this_iteration = tx_stream->send(buffs, samples_to_send, md, 0.1);
//
//         if (first_packet) {
//             md.start_of_burst = false;
//             md.has_time_spec = false;
//             first_packet = false;
//         }
//
//         if (sent_this_iteration != samples_to_send) {
//             // std::cerr << "发送不完整: 期望发送 " << samples_to_send
//             //           << " 个样本，实际发送 " << sent_this_iteration << " 个" << std::endl;
//             //
//             // // 如果是发送失败且返回0，可能是严重错误
//             // if (sent_this_iteration == 0) {
//             //     std::cerr << "发送失败，可能网络或设备问题" << std::endl;
//             //     // break;
//             // }
//
//             // 部分发送的情况下，更新进度
//             samples_sent += sent_this_iteration;
//         } else {
//             samples_sent += sent_this_iteration;
//         }
//
//         // 显示进度
//         if (samples_sent % (10 * samps_per_buff) < samps_per_buff) {
//             double progress = 100.0 * samples_sent / total_samples;
//             auto now = std::chrono::steady_clock::now();
//             double elapsed = std::chrono::duration<double>(now - start_clock).count();
//             double rate = (samples_sent * sizeof(complex_t)) / elapsed / 1e6;
//
//             std::cout << boost::format("发送进度: %6.2f%% (%d/%d) 速率: %6.2f MB/s")
//                     % progress % samples_sent % total_samples % rate << std::endl;
//         }
//     }
//
//     // 发送结束包
//     if (!stop_signal_called) {
//         try {
//             md.end_of_burst = true;
//             tx_stream->send("", 0, md);
//         } catch (const std::exception &e) {
//             std::cerr << "发送结束包异常: " << e.what() << std::endl;
//         }
//     }
//
//     const auto end_clock = std::chrono::steady_clock::now();
//     double elapsed = std::chrono::duration<double>(end_clock - start_clock).count();
//
//     std::cout << "发送完成: " << samples_sent << " 个样本 ("
//             << std::fixed << std::setprecision(2)
//             << (samples_sent * sizeof(complex_t) / (1024.0 * 1024.0)) << " MB)" << std::endl;
//     std::cout << boost::format("平均发送速率: %.2f MB/s")
//             % (samples_sent * sizeof(complex_t) / elapsed / 1e6) << std::endl;
//
//     tx_done = true;
// }
//
// // 预加载发送数据到内存
// std::vector<complex_t> preload_tx_data(const std::string &filename, size_t &total_samples) {
//     std::ifstream file(filename, std::ios::binary | std::ios::ate);
//     if (!file.is_open()) {
//         throw std::runtime_error("无法打开发送文件: " + filename);
//     }
//
//     std::streamsize file_size = file.tellg();
//     file.seekg(0, std::ios::beg);
//
//     if (file_size % sizeof(complex_t) != 0) {
//         std::cerr << "警告: 文件大小(" << file_size
//                 << "字节)不是" << sizeof(complex_t) << "字节的整数倍" << std::endl;
//     }
//
//     total_samples = file_size / sizeof(complex_t);
//     std::vector<complex_t> buffer(total_samples);
//
//     std::cout << "预加载发送数据: " << total_samples << " 个样本 ("
//             << std::fixed << std::setprecision(2)
//             << (file_size / (1024.0 * 1024.0)) << " MB)" << std::endl;
//
//     if (!file.read(reinterpret_cast<char *>(buffer.data()), file_size)) {
//         throw std::runtime_error("读取发送文件失败: " + filename);
//     }
//
//     file.close();
//
//     // 检查数据范围
//     float max_real = 0, max_imag = 0;
//     for (size_t i = 0; i < std::min<size_t>(1000, total_samples); ++i) {
//         max_real = std::max(max_real, std::abs(buffer[i].real()));
//         max_imag = std::max(max_imag, std::abs(buffer[i].imag()));
//     }
//     std::cout << "数据范围检查 - 前1000个样本的最大绝对值: 实部=" << max_real
//             << ", 虚部=" << max_imag << std::endl;
//
//     return buffer;
// }
//
// // 接收函数 - 主线程中运行
// void receive_to_file(
//     uhd::rx_streamer::sptr rx_stream,
//     const std::vector<size_t> &rx_channels,
//     const std::string &base_filename,
//     size_t samps_per_buff,
//     size_t total_num_samps,
//     const uhd::time_spec_t &start_time,
//     std::atomic<bool> &rx_done) {
//     // 为每个通道创建输出文件
//     std::vector<std::shared_ptr<std::ofstream> > outfiles;
//     for (size_t i = 0; i < rx_channels.size(); i++) {
//         const std::string this_filename = generate_out_filename(base_filename, rx_channels.size(), i);
//         outfiles.push_back(std::shared_ptr<std::ofstream>(
//             new std::ofstream(this_filename, std::ofstream::binary)));
//
//         if (!outfiles[i]->is_open()) {
//             throw std::runtime_error("无法打开接收文件: " + this_filename);
//         }
//         std::cout << "接收通道 " << rx_channels[i] << " 数据将保存到: " << this_filename << std::endl;
//     }
//
//     // 准备接收缓冲区
//     std::vector<std::vector<complex_t> > buffs(
//         rx_channels.size(), std::vector<complex_t>(samps_per_buff));
//     std::vector<complex_t *> buff_ptrs;
//     for (size_t i = 0; i < buffs.size(); i++) {
//         buff_ptrs.push_back(&buffs[i].front());
//     }
//
//     // 配置流命令
//     uhd::stream_cmd_t stream_cmd((total_num_samps == 0)
//                                      ? uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS
//                                      : uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
//     stream_cmd.num_samps = total_num_samps;
//     stream_cmd.stream_now = false;
//     stream_cmd.time_spec = start_time;
//
//     std::cout << "接收开始时间: " << start_time.get_real_secs() << " 秒" << std::endl;
//
//     // 启动接收流
//     rx_stream->issue_stream_cmd(stream_cmd);
//
//     uhd::rx_metadata_t md;
//     size_t num_total_samps = 0;
//     const auto start_clock = std::chrono::steady_clock::now();
//     auto last_update = start_clock;
//     bool overflow_message = true;
//
//     // 接收循环
//     while (!stop_signal_called
//            && (total_num_samps > num_total_samps || total_num_samps == 0)) {
//         // 接收数据，使用较短的超时时间
//         size_t num_rx_samps = rx_stream->recv(buff_ptrs, samps_per_buff, md, 0.1);
//
//         if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
//             // 检查是否应该结束
//             auto now = std::chrono::steady_clock::now();
//             if (std::chrono::duration<double>(now - last_update).count() > 1.0) {
//                 std::cout << "接收超时，继续等待..." << std::endl;
//                 last_update = now;
//             }
//             continue;
//         }
//
//         if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
//             if (overflow_message) {
//                 overflow_message = false;
//                 std::cerr
//                         << boost::format("Got an overflow indication.");
//             }
//             continue;
//         }
//
//         if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
//             throw std::runtime_error("接收错误: " + md.strerror());
//         }
//
//         // 写入文件
//         for (size_t i = 0; i < outfiles.size(); i++) {
//             outfiles[i]->write(
//                 reinterpret_cast<const char *>(buff_ptrs[i]), num_rx_samps * sizeof(complex_t));
//         }
//
//         num_total_samps += num_rx_samps;
//
//         // 定期显示进度
//         auto now = std::chrono::steady_clock::now();
//         if (now - last_update > std::chrono::seconds(1)) {
//             double elapsed = std::chrono::duration<double>(now - start_clock).count();
//             double rate_mbs = num_total_samps * sizeof(complex_t) / elapsed / 1e6;
//             double samples_per_sec = num_total_samps / elapsed;
//
//             std::cout << boost::format("接收进度: %d 样本 (%.2f MB/s, %.2f Msps)")
//                     % num_total_samps % rate_mbs % (samples_per_sec / 1e6) << std::endl;
//             last_update = now;
//         }
//     }
//
//     // 停止接收流
//     stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
//     rx_stream->issue_stream_cmd(stream_cmd);
//
//     // 关闭文件
//     for (size_t i = 0; i < outfiles.size(); i++) {
//         outfiles[i]->close();
//     }
//
//     const auto end_clock = std::chrono::steady_clock::now();
//     double elapsed = std::chrono::duration<double>(end_clock - start_clock).count();
//
//     std::cout << "接收完成: " << num_total_samps << " 个样本 ("
//             << std::fixed << std::setprecision(2)
//             << (num_total_samps * sizeof(complex_t) / (1024.0 * 1024.0)) << " MB)" << std::endl;
//     std::cout << boost::format("平均接收速率: %.2f MB/s (%.2f Msps)")
//             % (num_total_samps * sizeof(complex_t) / elapsed / 1e6)
//             % (num_total_samps / elapsed / 1e6) << std::endl;
//
//     rx_done = true;
// }

// 解析通道字符串
// std::vector<size_t> parse_channels(const std::string &channels_str) {
//     std::vector<size_t> channels;
//     std::vector<std::string> channel_strs;
//     boost::split(channel_strs, channels_str, boost::is_any_of(", "));
//
//     for (const auto &ch_str: channel_strs) {
//         if (!ch_str.empty()) {
//             try {
//                 channels.push_back(std::stoi(ch_str));
//             } catch (const std::exception &e) {
//                 throw std::runtime_error("无效的通道号: " + ch_str);
//             }
//         }
//     }
//
//     return channels;
// }

// 检查锁定的传感器
bool check_locked_sensor(
    std::vector<std::string> sensor_names,
    const char *sensor_name,
    std::function<uhd::sensor_value_t(const std::string &)> get_sensor_fn,
    double setup_time = 1.0) {
    if (std::find(sensor_names.begin(), sensor_names.end(), sensor_name)
        == sensor_names.end()) {
        return false;
    }

    const auto setup_timeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool lock_detected = false;

    std::cout << "等待 \"" << sensor_name << "\": ";
    std::cout.flush();

    while (true) {
        if (lock_detected && (std::chrono::steady_clock::now() > setup_timeout)) {
            std::cout << " locked." << std::endl;
            break;
        }
        if (get_sensor_fn(sensor_name).to_bool()) {
            std::cout << "+";
            std::cout.flush();
            lock_detected = true;
        } else {
            if (std::chrono::steady_clock::now() > setup_timeout) {
                std::cout << std::endl;
                throw std::runtime_error(
                    str(boost::format(
                            "timed out waiting for consecutive locks on sensor \"%s\"")
                        % sensor_name));
            }
            std::cout << "_";
            std::cout.flush();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return true;
}

int UHD_SAFE_MAIN(int argc, char* argv[]) {
    // 变量定义
    std::string args, tx_file, rx_file;
    std::string tx_ant, rx_ant, ref;
    std::string tx_channels_str, rx_channels_str;
    std::vector<size_t> tx_channels, rx_channels;
    size_t spb;
    double rate, freq, tx_gain, rx_gain, bw, delay;
    size_t nsamps = 0; // 接收样本数，0表示直到发送完成
    std::vector<std::string> tx_files, rx_files;
    // 程序说明
    const std::string program_doc =
            "同时发送和接收示例程序 - 基于官方示例优化\n"
            "接收在主线程，发送在独立线程\n\n"
            "使用示例:\n"
            "  1. 两台同步的USRP N210 (连接到Octoclock):\n"
            "     txrx_fc32_sync \\\n"
            "       --args \"addr0=192.168.10.2,addr1=192.168.10.3\" \\\n"
            "       --tx-files tx_data_fc32.bin \\\n"
            "       --rx-files rx_data_fc32.bin \\\n"
            "       --tx-channels 0 \\\n"
            "       --rx-channels 1 \\\n"
            "       --freq 915e6 --rate 1e6 \\\n"
            "       --tx-gain 10 --rx-gain 10 \\\n"
            "       --delay 2.0 --ref external\n\n"
            "  2. 单台USRP N210自环测试:\n"
            "     txrx_fc32_sync \\\n"
            "       --args \"addr=192.168.10.2\" \\\n"
            "       --tx-files tx_data_fc32.bin \\\n"
            "       --rx-files rx_data_fc32.bin \\\n"
            "       --tx-channels 0 \\\n"
            "       --rx-channels 1 \\\n"
            "       --freq 915e6 --rate 1e6 \\\n"
            "       --tx-gain 10 --rx-gain 10 \\\n"
            "       --delay 1.0 --ref internal\n";

    // 设置命令行选项
    po::options_description desc("命令行选项");
    std::vector<std::string> default_tx_files = {"tx_data_fc32.bin"};
    std::vector<std::string> default_rx_files = {"rx_data_fc32.bin"};
    std::vector<size_t> default_tx_channels = {0};
    std::vector<size_t> default_rx_channels = {1};
    desc.add_options()
            ("help,h", "显示帮助信息")
            ("args", po::value<std::string>(&args)->default_value("addr=192.168.180.2"),
             "USRP设备参数")
            ("tx-files",
             po::value<std::vector<std::string> >(&tx_files)->multitoken()->default_value(default_tx_files, "tx_data_fc32.bin"),
             "发送数据文件(fc32格式)")
            ("rx-files",
             po::value<std::vector<std::string> >(&rx_files)->multitoken()->default_value(default_rx_files, "rx_data_fc32.bin"),
             "接收数据文件(fc32格式)")
            ("tx-ant", po::value<std::string>(&tx_ant)->default_value("TX/RX"),
             "发送天线选择")
            ("rx-ant", po::value<std::string>(&rx_ant)->default_value("RX2"),
             "接收天线选择")
            ("tx-channels", po::value<std::vector<size_t> >(&tx_channels)->multitoken()->default_value(default_tx_channels,"0"),
             "发送通道(space分隔)")
            ("rx-channels", po::value<std::vector<size_t> >(&rx_channels)->multitoken()->default_value(default_rx_channels,"1"),
             "接收通道(space分隔)")
            ("spb", po::value<size_t>(&spb)->default_value(2500),
             "缓冲区大小(样本数)")
            ("rate", po::value<double>(&rate)->default_value(5e6),
             "采样率 (Hz)")
            ("freq", po::value<double>(&freq)->default_value(915e6),
             "中心频率 (Hz)")
            ("tx-gain", po::value<double>(&tx_gain)->default_value(10.0),
             "发送增益 (dB)")
            ("rx-gain", po::value<double>(&rx_gain)->default_value(10.0),
             "接收增益 (dB)")
            ("bw", po::value<double>(&bw),
             "带宽 (Hz)")
            ("delay", po::value<double>(&delay)->default_value(1),
             "开始前的延迟 (秒)")
            ("nsamps", po::value<size_t>(&nsamps)->default_value(0),
             "接收样本数，0表示直到发送完成")
            ("ref", po::value<std::string>(&ref)->default_value("internal"),
             "时钟参考: internal, external, gpsdo");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << program_doc << std::endl;
            std::cout << desc << std::endl;
            return EXIT_SUCCESS;
        }

        po::notify(vm);
    } catch (const std::exception &e) {
        std::cerr << "错误: " << e.what() << std::endl;
        std::cerr << desc << std::endl;
        return EXIT_FAILURE;
    }

    // 注册信号处理器
    std::signal(SIGINT, &sig_int_handler);


    // 创建USRP设备
    UHD_LOG_INFO("SYSTEM", "创建USRP设备: " << args)
    // std::cout << "创建USRP设备: " << args << std::endl;
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);
    // usrp->get_num_mboards();
    UHD_LOG_INFO("SYSTEM", usrp->get_pp_string());
    // 显示设备信息
    // std::string mboard_name = usrp->get_mboard_name();
    // std::cout << "设备型号: " << mboard_name << std::endl;
    // std::cout << "发送通道总数: " << usrp->get_tx_num_channels() << std::endl;
    // std::cout << "接收通道总数: " << usrp->get_rx_num_channels() << std::endl;

    // 解析通道
    // tx_channels = parse_channels(tx_channels_str);
    // rx_channels = parse_channels(rx_channels_str);

    // 检查通道有效性
    size_t total_tx_channels = usrp->get_tx_num_channels();
    size_t total_rx_channels = usrp->get_rx_num_channels();

    std::cout << "发送通道: ";
    for (size_t ch: tx_channels) std::cout << ch << " ";
    std::cout << std::endl;

    std::cout << "接收通道: ";
    for (size_t ch: rx_channels) std::cout << ch << " ";
    std::cout << std::endl;


    UHD_ASSERT_THROW(total_tx_channels >= tx_files.size())
    UHD_ASSERT_THROW(total_rx_channels >= rx_files.size())

    // check tx_file exists
    if (not std::ranges::all_of(tx_files, [](std::string &filename) {
        return fs::exists(filename);
    })) {
        UHD_LOG_ERROR("PRE-CHECK", "tx-files not exists!")
        return EXIT_FAILURE;
    }


    // check all tx_files have same file size
    std::vector<std::uintmax_t> sizes;
    std::ranges::transform(tx_files, std::back_inserter(sizes),
                           [](const std::string &f) { return std::filesystem::file_size(f); });

    if (std::ranges::adjacent_find(sizes, std::not_equal_to{}) != sizes.end()) {
        UHD_LOG_ERROR("PRE-CHECK", "tx-files sizes mismatch")
        return EXIT_FAILURE;
    }


    for (size_t ch: tx_channels) {
        if (ch >= total_tx_channels) {
            throw std::runtime_error("无效的发送通道: " + std::to_string(ch) +
                                     " (总发送通道数: " + std::to_string(total_tx_channels) + ")");
        }
        // 设置增益
        if (vm.count("tx-gain")) {
            std::cout << boost::format("设置发送增益: %f dB") % tx_gain << std::endl;
            usrp->set_tx_gain(tx_gain, ch);
            std::cout << boost::format("实际发送增益: %f dB") % usrp->get_tx_gain(ch) << std::endl;
        }

        uhd::tune_request_t tune_request(freq);
        usrp->set_tx_freq(tune_request, ch);
        std::cout << boost::format("实际发送频率: %f MHz") % (usrp->get_tx_freq(ch) / 1e6) << std::endl;

        if (!tx_ant.empty()) {
            std::cout << "设置发送天线: " << tx_ant << std::endl;
            usrp->set_tx_antenna(tx_ant, ch);
            std::cout << "实际发送天线: " << usrp->get_tx_antenna() << std::endl;
        }
    }

    for (size_t ch: rx_channels) {
        if (ch >= total_rx_channels) {
            throw std::runtime_error("无效的接收通道: " + std::to_string(ch) +
                                     " (总接收通道数: " + std::to_string(total_rx_channels) + ")");
        }
        if (vm.count("rx-gain")) {
            std::cout << boost::format("设置接收增益: %f dB") % rx_gain << std::endl;
            usrp->set_rx_gain(rx_gain, ch);
            // usrp->set_normalized_rx_gain(0.3);
            std::cout << boost::format("实际接收增益: %f dB") % usrp->get_rx_gain(ch) << std::endl;
        }

        uhd::tune_request_t tune_request(freq);
        usrp->set_rx_freq(tune_request, ch);
        std::cout << boost::format("实际接收频率: %f MHz") % (usrp->get_rx_freq(ch) / 1e6) << std::endl;


        if (!rx_ant.empty()) {
            std::cout << "设置接收天线: " << rx_ant << std::endl;
            usrp->set_rx_antenna(rx_ant, ch);
            std::cout << "实际接收天线: " << usrp->get_rx_antenna(ch) << std::endl;
        }
    }


    // 设置时钟参考
    std::cout << "设置时钟参考: " << ref << std::endl;
    usrp->set_clock_source(ref);

    // 设置时间参考
    if (ref == "external" || ref == "gpsdo") {
        std::cout << "设置时钟参考: " << ref << std::endl;
        usrp->set_time_source("external");
    } else {
        usrp->set_time_source("internal");
    }

    // 等待PPS同步并设置时间
    std::cout << "等待PPS同步并设置时间..." << std::endl;
    // usrp->set_time_unknown_pps(uhd::time_spec_t(0.0));

    const uhd::time_spec_t last_pps_time = usrp->get_time_last_pps();
    while (last_pps_time == usrp->get_time_last_pps()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // This command will be processed fairly soon after the last PPS edge:
    usrp->set_time_next_pps(uhd::time_spec_t(0.0));


    // 显示当前时间
    std::cout << "当前USRP时间: " << usrp->get_time_now().get_real_secs() << " 秒" << std::endl;

    // 设置采样率
    std::cout << boost::format("设置采样率: %f Msps") % (rate / 1e6) << std::endl;
    usrp->set_tx_rate(rate);
    usrp->set_rx_rate(rate);
    std::cout << boost::format("实际发送采样率: %f Msps") % (usrp->get_tx_rate() / 1e6) << std::endl;
    std::cout << boost::format("实际接收采样率: %f Msps") % (usrp->get_rx_rate() / 1e6) << std::endl;


    // 设置带宽
    // if (vm.count("bw")) {
    //     std::cout << boost::format("设置带宽: %f MHz") % (bw / 1e6) << std::endl;
    //     usrp->set_tx_bandwidth(bw);
    //     usrp->set_rx_bandwidth(bw);
    //     std::cout << boost::format("实际发送带宽: %f MHz") % (usrp->get_tx_bandwidth() / 1e6) << std::endl;
    //     std::cout << boost::format("实际接收带宽: %f MHz") % (usrp->get_rx_bandwidth() / 1e6) << std::endl;
    // }

    // 设置天线


    // 检查锁相环锁定
    std::cout << "检查LO锁定..." << std::endl;

    // 检查发送LO锁定
    for (size_t ch: tx_channels) {
        auto sensor_names = usrp->get_tx_sensor_names(ch);
        if (check_locked_sensor(
            sensor_names,
            "lo_locked",
            [usrp, ch](const std::string &sensor_name) {
                return usrp->get_tx_sensor(sensor_name, ch);
            })) {
            std::cout << "发送通道 " << ch << " LO已锁定" << std::endl;
        }
    }

    // 检查接收LO锁定
    for (size_t ch: rx_channels) {
        auto sensor_names = usrp->get_rx_sensor_names(ch);
        if (check_locked_sensor(
            sensor_names,
            "lo_locked",
            [usrp, ch](const std::string &sensor_name) {
                return usrp->get_rx_sensor(sensor_name, ch);
            })) {
            std::cout << "接收通道 " << ch << " LO已锁定" << std::endl;
        }
    }

    // 检查参考时钟锁定
    // if (ref == "external" || ref == "gpsdo") {
    //     auto mboard_sensors = usrp->get_mboard_sensor_names(0);
    //     if (check_locked_sensor(
    //         mboard_sensors,
    //         "ref_locked",
    //         [usrp](const std::string& sensor_name) {
    //             return usrp->get_mboard_sensor(sensor_name);
    //         })) {
    //         std::cout << "参考时钟已锁定" << std::endl;
    //     }
    // }

    // 预加载发送数据
    // std::cout << "预加载发送数据..." << std::endl;
    // size_t total_tx_samples = 0;
    // std::vector<complex_t> tx_buffer = preload_tx_data(tx_file, total_tx_samples);

    // 计算发送持续时间
    // double tx_duration = total_tx_samples / rate;
    // std::cout << boost::format("发送数据持续时间: %.3f 秒") % tx_duration << std::endl;

    // 如果未指定接收样本数，则根据发送持续时间计算
    // if (nsamps == 0) {
    //     // 接收比发送稍长一点的时间，确保完整接收
    //     nsamps = static_cast<size_t>(tx_duration * rate);
    //     std::cout << boost::format("自动设置接收样本数: %d") % nsamps << std::endl;
    // }

    // 计算缓冲区大小
    // if (spb == 0) {
    //     // 根据设备类型和采样率自动选择缓冲区大小
    //     if (mboard_name.find("N210") != std::string::npos) {
    //         if (rate <= 1e6) spb = 4096;
    //         else if (rate <= 2e6) spb = 2048;
    //         else if (rate <= 5e6) spb = 1024;
    //         else spb = 512;
    //     } else {
    //         spb = 10000; // 默认值
    //     }
    //     std::cout << boost::format("自动计算缓冲区大小: %d 样本") % spb << std::endl;
    // }

    // 创建发送流
    uhd::stream_args_t tx_stream_args("fc32", "sc16");
    tx_stream_args.channels = tx_channels;
    uhd::tx_streamer::sptr tx_stream = usrp->get_tx_stream(tx_stream_args);

    // 创建接收流
    uhd::stream_args_t rx_stream_args("fc32", "sc16");
    rx_stream_args.channels = rx_channels;
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(rx_stream_args);

    // 计算开始时间
    uhd::time_spec_t seconds_in_future = usrp->get_time_now() + uhd::time_spec_t(delay);
    std::cout << boost::format("开始时间: %.3f 秒后 (绝对时间: %.6f)")
            % delay % seconds_in_future.get_real_secs() << std::endl;

    // 创建原子标志
    // std::atomic<bool> tx_done(false);
    // std::atomic<bool> rx_done(false);

    // 启动发送线程
    std::cout << "启动发送线程..." << std::endl;
    auto transmit_thread = std::async(std::launch::async, transmit_from_file_worker,
                                      tx_stream, tx_files, spb, seconds_in_future
    );
    size_t nums_to_recv = sizes.front() / sizeof(complexf);

    auto receive_thread = std::async(std::launch::async, receive_to_file_worker,
                                     rx_stream, rx_files, spb, seconds_in_future, nums_to_recv);


    // std::thread transmit_thread([&]() {
    //     transmit_from_buffer(
    //         tx_stream, tx_buffer, total_tx_samples, spb, seconds_in_future, tx_done);
    // });

    // 确保发送线程已经启动并准备就绪
    // std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 在主线程中运行接收
    // std::cout << "在主线程中启动接收..." << std::endl;
    // std::cout << "发送样本总数: " << total_tx_samples << std::endl;
    //
    // if (nsamps > 0) {
    //     std::cout << "接收样本总数: " << nsamps << std::endl;
    // } else {
    //     std::cout << "接收样本总数: 直到发送完成" << std::endl;
    // }

    // 设置信号处理器以便可以随时中断
    if (nsamps == 0) {
        std::cout << "按Ctrl+C停止..." << std::endl;
    }
    transmit_thread.wait();
    receive_thread.wait();

    stop_signal_called = true;
    UHD_LOG_INFO("SYSTEM", "TX-RX Finished!")


    return EXIT_SUCCESS;
}
