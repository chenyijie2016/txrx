#include <atomic>
#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>
#include <csignal>
#include <format>
#include <future>
#include <uhd/convert.hpp>
#include <uhd/exception.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <vector>

#include "usrp_transceiver.h"
#include "utils.h"

namespace po = boost::program_options;
namespace fs = std::filesystem;
namespace stdr = std::ranges;
namespace stdv = std::views;

using namespace std::chrono_literals;
using std::complex;
using std::format;
using std::string;
using std::vector;

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

int UHD_SAFE_MAIN(int argc, char *argv[]) {
    // Initialize USRP device and file parameters
    UsrpConfig config{};
    string args;
    double rate, freq;
    // Program description
    const string program_doc = "Simultaneous TX/RX samples from/to file.\nDesigned specifically for "
                               "multi-channel "
                               "synchronous transmission and reception\n";

    // Configure command line options
    po::options_description desc("Command line options");
    auto option = desc.add_options();
    option("help,h", "Show this help message");
    option("args", po::value<string>(&args)->default_value("addr=192.168.180.2"), "USRP device address string");
    option("tx-files", po::value<vector<string>>(&config.tx_files)->multitoken()->default_value({"tx_data_fc32.bin"}, "tx_data_fc32.bin"),
           "TX data files (fc32 format)");
    option("rx-files", po::value<vector<string>>(&config.rx_files)->multitoken()->default_value({"rx_data_fc32.bin"}, "rx_data_fc32.bin"),
           "RX data files (fc32 format)");
    option("tx-ants", po::value<vector<string>>(&config.tx_ants)->multitoken()->default_value({"TX/RX"}, "TX/RX"), "TX antenna selection");
    option("rx-ants", po::value<vector<string>>(&config.rx_ants)->multitoken()->default_value({"RX2"}, "RX2"), "RX antenna selection");
    option("tx-channels", po::value<vector<size_t>>(&config.tx_channels)->multitoken()->default_value({0}, "0"), "TX channels (space separated)");
    option("rx-channels", po::value<vector<size_t>>(&config.rx_channels)->multitoken()->default_value({1}, "1"), "RX channels (space separated)");
    option("spb", po::value<size_t>(&config.spb)->default_value(2500), "Samples per buffer");
    option("rate", po::value<double>(&rate), "Sample rate (Hz)");
    option("tx-rates", po::value<vector<double>>(&config.tx_rates)->multitoken()->default_value({1e6}, "1e6"),
           "Tx Sample rate (Hz)")("rx-rates", po::value<vector<double>>(&config.rx_rates)->multitoken()->default_value({1e6}, "1e6"), "Rx Sample rate (Hz)");
    option("freq", po::value<double>(&freq),
           "Center frequency (Hz) for ALL Tx and Rx Channels. IGNORE --tx-freqs and "
           "--rx-freqs settings");
    option("tx-freqs", po::value<vector<double>>(&config.tx_freqs)->multitoken()->default_value({915e6}, "915e6"), "TX Center frequencies (Hz)");
    option("rx-freqs", po::value<vector<double>>(&config.rx_freqs)->multitoken()->default_value({915e6}, "915e6"), "RX Center frequencies (Hz)");
    option("tx-gains", po::value<vector<double>>(&config.tx_gains)->multitoken()->default_value({10.0}, "10.0"), "TX gain (dB)");
    option("rx-gains", po::value<vector<double>>(&config.rx_gains)->multitoken()->default_value({10.0}, "10.0"), "RX gain (dB)");
    //("rx-bw", po::value<double>(&config.rx_bw),
    //                      "RX Bandwidth (Hz)")("tx-bw", po::value<double>(&config.tx_bw), "TX Bandwidth (Hz)");
    option("delay", po::value<double>(&config.delay)->default_value(1), "Delay before start (seconds)");
    option("nsamps", po::value<size_t>(&config.nsamps)->default_value(5e6), "Number of samples to receive, 0 means until TX complete");
    option("clock-source", po::value<string>(&config.clock_source)->default_value("internal"), "Reference: internal, external, gpsdo");
    option("time-source", po::value<string>(&config.time_source)->default_value("internal"), "Time Source");

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
        config.tx_rates.resize(config.tx_channels.size());
        config.rx_rates.resize(config.rx_channels.size());
        stdr::fill(config.tx_rates, rate);
        stdr::fill(config.rx_rates, rate);
        UHD_LOG_INFO("CONFIG", format("Set Tx and Rx rate to {:.3f} Mhz", rate / 1e6))
    }

    if (vm.contains("freq")) {
        config.tx_freqs.resize(config.tx_channels.size());
        config.rx_freqs.resize(config.rx_channels.size());
        stdr::fill(config.tx_freqs, freq);
        stdr::fill(config.tx_freqs, freq);
        UHD_LOG_INFO("CONFIG", format("Set Tx and Rx freq to {:.3f} Mhz", rate / 1e6))
    }

    // Create UsrpTransceiver instance
    UsrpTransceiver transceiver(args);

    if (not transceiver.ValidateConfiguration(config)) {
        UHD_LOG_ERROR("SYSTEM", "Invalid configuration provided");
        return EXIT_FAILURE;
    }
    // for (int i = 0; i < 2; i++)
    {
        transceiver.ApplyConfiguration(config);
        // Start transmission thread
        auto TxBuffer = LoadFileToBuffer(config);

        UHD_LOG_INFO("SYSTEM", "Starting transmission thread...");
        auto transmit_thread =
                std::async(std::launch::async, &UsrpTransceiver::TransmitFromBuffer, &transceiver, std::ref(TxBuffer), std::ref(stop_signal_called));

        // Launch receive operation to get buffer via future
        auto receive_future = std::async(std::launch::async, &UsrpTransceiver::ReceiveToBuffer, &transceiver, std::ref(stop_signal_called));

        // Wait for transmission to complete
        transmit_thread.wait();

        // Get the received buffer from the future
        auto RxBuffer = receive_future.get();

        // Write the received buffer to files
        WriteBufferToFile(config, RxBuffer);
    }
    stop_signal_called = true;
    UHD_LOG_INFO("SYSTEM", "TX-RX operation finished!")

    return EXIT_SUCCESS;
}
