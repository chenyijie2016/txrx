#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <uhd/convert.hpp>
#include <uhd/exception.hpp>
#include <uhd/property_tree.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
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
#include <ranges>
#include <numeric>  // for std::iota
#include "utils.h"
#include "usrp_transceiver.h"


namespace po = boost::program_options;
namespace fs = std::filesystem;
namespace stdr = std::ranges;
namespace stdv = std::views;

using namespace std::chrono_literals;
using std::vector;
using std::string;
using std::complex;
using std::format;

// External global flag for signal handling (defined in workers.cpp)
std::atomic<bool> stop_signal_called{false};

/**
 * Signal handler for graceful shutdown
 * Sets the stop_signal_called flag to true when SIGINT is received
 */
void sig_int_handler(int) {
    stop_signal_called = true;
    UHD_LOG_INFO("SIGNAL", "SIGINT received, stopping...")
}


int UHD_SAFE_MAIN(int argc, char* argv[]) {
    // Initialize USRP device and file parameters
    UsrpConfig config{};
    string args;
    // Program description
    const string program_doc =
            "Simultaneous TX/RX samples from/to file.\nDesigned specifically for multi-channel synchronous transmission and reception\n";

    // Configure command line options
    po::options_description desc("Command line options");
    desc.add_options()
            ("help,h", "Show this help message")
            ("args", po::value<string>(&args)->default_value("addr=192.168.180.2"),
             "USRP device address string")
            ("tx-files",
             po::value<vector<string> >(&config.tx_files)->multitoken()->default_value(
                 {"tx_data_fc32.bin"}, "tx_data_fc32.bin"),
             "TX data files (fc32 format)")
            ("rx-files",
             po::value<vector<string> >(&config.rx_files)->multitoken()->default_value(
                 {"rx_data_fc32.bin"}, "rx_data_fc32.bin"),
             "RX data files (fc32 format)")
            ("tx-ants", po::value<vector<string> >(&config.tx_ants)->multitoken()->default_value({"TX/RX"}, "TX/RX"),
             "TX antenna selection")
            ("rx-ants", po::value<vector<string> >(&config.rx_ants)->multitoken()->default_value({"RX2"}, "RX2"),
             "RX antenna selection")
            ("tx-channels",
             po::value<vector<size_t> >(&config.tx_channels)->multitoken()->default_value({0}, "0"),
             "TX channels (space separated)")
            ("rx-channels",
             po::value<vector<size_t> >(&config.rx_channels)->multitoken()->default_value({1}, "1"),
             "RX channels (space separated)")
            ("spb", po::value<size_t>(&config.spb)->default_value(2500),
             "Samples per buffer")
            ("rate", po::value<double>(&config.rate),
             "Sample rate (Hz)")
            ("tx-rates", po::value<vector<double> >(&config.tx_rates)->multitoken()->default_value({1e6}, "1e6"),
             "Tx Sample rate (Hz)")
            ("rx-rates", po::value<vector<double> >(&config.rx_rates)->multitoken()->default_value({1e6}, "1e6"),
             "Rx Sample rate (Hz)")
            ("freq", po::value<double>(&config.freq),
             "Center frequency (Hz) for ALL Tx and Rx CHANNELS. IGNORE --tx-freqs and --rx-freqs settings")
            ("tx-freqs", po::value<vector<double> >(&config.tx_freqs)->multitoken()->default_value({915e6}, "915e6"),
             "TX Center frequencies (Hz)")
            ("rx-freqs", po::value<vector<double> >(&config.rx_freqs)->multitoken()->default_value({915e6}, "915e6"),
             "RX Center frequencies (Hz)")
            ("tx-gains", po::value<vector<double> >(&config.tx_gains)->multitoken()->default_value({10.0}, "10.0"),
             "TX gain (dB)")
            ("rx-gains", po::value<vector<double> >(&config.rx_gains)->multitoken()->default_value({10.0}, "10.0"),
             "RX gain (dB)")
            ("rx-bw", po::value<double>(&config.rx_bw),
             "RX Bandwidth (Hz)")
            ("tx-bw", po::value<double>(&config.tx_bw),
             "TX Bandwidth (Hz)")
            ("delay", po::value<double>(&config.delay)->default_value(1),
             "Delay before start (seconds)")
            ("nsamps", po::value<size_t>(&config.nsamps)->default_value(5e6),
             "Number of samples to receive, 0 means until TX complete")
            ("clock-source", po::value<string>(&config.clock_source)->default_value("internal"),
             "Reference: internal, external, gpsdo");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.contains("help")) {
            UHD_LOG_INFO("MAIN", program_doc)
            std::cout << desc << std::endl;
            return EXIT_SUCCESS;
        }

        po::notify(vm);
    } catch (const std::exception &e) {
        UHD_LOG_ERROR("MAIN", "Error parsing command line: " << e.what())
        std::cerr << desc << std::endl;
        return EXIT_FAILURE;
    }

    // Register signal handler for graceful shutdown
    UHD_LOG_TRACE("SYSTEM", "Registering signal handler")
    std::signal(SIGINT, &sig_int_handler);



    if (vm.contains("rate")) {
        stdr::fill(config.tx_rates, config.rate);
        stdr::fill(config.rx_rates, config.rate);
        UHD_LOG_INFO("CONFIG", format("Set Tx and Rx rate to {:.3f} Mhz", config.rate/1e6))
    }

    // Create UsrpTransceiver instance
    UsrpTransceiver transceiver(args);

    if (not transceiver.ValidateConfiguration(config)) {
        UHD_LOG_ERROR("SYSTEM", "Invalid configuration provided");
        return EXIT_FAILURE;
    }
    transceiver.ApplyConfiguration(config);
    // Start transmission thread
    auto TxBuffer = LoadFileToBuffer(config);

    UHD_LOG_INFO("SYSTEM", "Starting transmission thread...");
    auto transmit_thread = std::async(std::launch::async,
                                      &UsrpTransceiver::TransmitFromBuffer,
                                      &transceiver,
                                      std::ref(TxBuffer));

    // Launch receive operation to get buffer via future
    auto receive_future = std::async(std::launch::async,
                                     &UsrpTransceiver::ReceiveToBuffer,
                                     &transceiver);

    // Wait for transmission to complete
    transmit_thread.wait();

    // Get the received buffer from the future
    auto RxBuffer = receive_future.get();

    // Write the received buffer to files
    WriteBufferToFile(config, RxBuffer);

    stop_signal_called = true;
    UHD_LOG_INFO("SYSTEM", "TX-RX operation finished!")


    return EXIT_SUCCESS;
}
