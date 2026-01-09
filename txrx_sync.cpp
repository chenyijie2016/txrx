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
#include "workers.h"

namespace po = boost::program_options;
namespace fs = std::filesystem;
namespace stdr = std::ranges;
namespace stdv = std::views;

using namespace std::chrono_literals;
using std::ranges::all_of;
using std::ranges::any_of;
using std::ranges::transform;
using std::ranges::adjacent_find;
using std::views::enumerate;
using std::views::zip;
using std::vector;
using std::string;
using std::complex;
using std::format;


// Complex floating-point type for samples (fc32 format)
using complex_t = std::complex<float>;

// External global flag for signal handling (defined in workers.cpp)
extern std::atomic<bool> stop_signal_called;

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
    string args, tx_file, rx_file;
    string tx_ant, rx_ant, ref;
    vector<size_t> tx_channels, rx_channels;
    size_t spb; // Samples per buffer
    double rate, tx_gain, rx_gain, rx_bw, tx_bw, delay, freq, tx_rate, rx_rate;
    size_t nsamps = 0; // Number of samples to receive, 0 means until TX complete
    vector<string> tx_files, rx_files;
    vector<double> tx_freqs, rx_freqs;

    // Program description
    const string program_doc =
            "Simultaneous TX/RX samples from/to file - Optimized from official UHD example\n";

    // Configure command line options
    po::options_description desc("Command line options");
    desc.add_options()
            ("help,h", "Show this help message")
            ("args", po::value<string>(&args)->default_value("addr=192.168.180.2"),
             "USRP device address string")
            ("tx-files",
             po::value<vector<string> >(&tx_files)->multitoken()->default_value(
                 {"tx_data_fc32.bin"}, "tx_data_fc32.bin"),
             "TX data files (fc32 format)")
            ("rx-files",
             po::value<vector<string> >(&rx_files)->multitoken()->default_value(
                 {"rx_data_fc32.bin"}, "rx_data_fc32.bin"),
             "RX data files (fc32 format)")
            ("tx-ant", po::value<string>(&tx_ant)->default_value("TX/RX"),
             "TX antenna selection")
            ("rx-ant", po::value<string>(&rx_ant)->default_value("RX2"),
             "RX antenna selection")
            ("tx-channels",
             po::value<vector<size_t> >(&tx_channels)->multitoken()->default_value({0}, "0"),
             "TX channels (space separated)")
            ("rx-channels",
             po::value<vector<size_t> >(&rx_channels)->multitoken()->default_value({1}, "1"),
             "RX channels (space separated)")
            ("spb", po::value<size_t>(&spb)->default_value(2500),
             "Samples per buffer")
            ("rate", po::value<double>(&rate),
             "Sample rate (Hz)")
            ("tx-rate", po::value<double>(&tx_rate)->default_value(5e6),
             "Tx Sample rate (Hz)")
            ("rx-rate", po::value<double>(&rx_rate)->default_value(5e6),
             "Rx Sample rate (Hz)")
            ("freq", po::value<double>(&freq),
             "Center frequency (Hz) for ALL Tx and Rx CHANNELS. IGNORE --tx-freqs and --rx-freqs settings")
            ("tx-freqs", po::value<vector<double> >(&tx_freqs)->multitoken()->default_value({915e6}, "915e6"),
             "TX Center frequencies (Hz)")
            ("rx-freqs", po::value<vector<double> >(&rx_freqs)->multitoken()->default_value({915e6}, "915e6"),
             "RX Center frequencies (Hz)")
            ("tx-gain", po::value<double>(&tx_gain)->default_value(10.0),
             "TX gain (dB)")
            ("rx-gain", po::value<double>(&rx_gain)->default_value(10.0),
             "RX gain (dB)")
            ("rx-bw", po::value<double>(&rx_bw),
             "RX Bandwidth (Hz)")
            ("tx-bw", po::value<double>(&tx_bw),
             "TX Bandwidth (Hz)")
            ("delay", po::value<double>(&delay)->default_value(1),
             "Delay before start (seconds)")
            ("nsamps", po::value<size_t>(&nsamps)->default_value(0),
             "Number of samples to receive, 0 means until TX complete")
            ("ref", po::value<string>(&ref)->default_value("internal"),
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


    // Create USRP device
    UHD_LOG_INFO("SYSTEM", format("Creating USRP device with args: {}", args));
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);
    // Display device information
    UHD_LOG_INFO("SYSTEM", format("USRP device info: {}", usrp->get_pp_string()));

    if (vm.contains("rate")) {
        rx_rate = rate;
        tx_rate = rate;
        UHD_LOG_INFO("CONFIG", format("Set Tx and Rx rate to {:.3f} Mhz", rate/1e6))
    }


    // Check channel validity
    size_t total_tx_channels = usrp->get_tx_num_channels();
    size_t total_rx_channels = usrp->get_rx_num_channels();

    {
        std::ostringstream oss;
        oss << "TX channels: ";
        for (size_t ch: tx_channels) oss << ch << " ";
        UHD_LOG_INFO("CONFIG", oss.str());
    }

    {
        std::ostringstream oss;
        oss << "RX channels: ";
        for (size_t ch: rx_channels) oss << ch << " ";
        UHD_LOG_INFO("CONFIG", oss.str());
    }

    if (any_of(tx_channels, [&](const auto &ch) {
        return ch >= total_tx_channels;
    })) {
        UHD_LOG_ERROR("CONFIG", "TX channels are not supported");
        return EXIT_FAILURE;
    }

    if (any_of(rx_channels, [&](const auto &ch) {
        return ch >= total_rx_channels;
    })) {
        UHD_LOG_ERROR("CONFIG", "RX channels are not supported");
        return EXIT_FAILURE;
    }


    UHD_ASSERT_THROW(total_tx_channels >= tx_files.size())
    UHD_ASSERT_THROW(total_rx_channels >= rx_files.size())


    // Verify TX input files exist
    if (not all_of(tx_files, [](const string &filename) {
        return fs::exists(filename);
    })) {
        UHD_LOG_ERROR("PRE-CHECK", "TX input files do not exist!")
        return EXIT_FAILURE;
    }

    // Verify all TX files have the same size
    vector<std::uintmax_t> sizes = tx_files
    | stdv::transform([](const string& f) {
          return fs::file_size(f);
      })
    | stdr::to<vector>();

    // vector<std::uintmax_t> sizes;
    // transform(tx_files, std::back_inserter(sizes),
    //           [](const string &f) { return fs::file_size(f); });


    if (adjacent_find(sizes, std::not_equal_to{}) != sizes.end()) {
        UHD_LOG_ERROR("PRE-CHECK", "Tx file sizes mismatch")
        return EXIT_FAILURE;
    }

    // Configure TX channels
    UHD_LOG_TRACE("CONFIG", "Configuring Tx channels")
    for (size_t ch: tx_channels) {
        // Set TX gain
        if (vm.contains("tx-gain")) {
            UHD_LOG_INFO("CONFIG", format("Setting Tx(ch={}) gain to: {:.2f} dB",ch, tx_gain))
            usrp->set_tx_gain(tx_gain, ch);
            UHD_LOG_INFO("CONFIG", format("Actual Tx(ch={}) gain: {:.2f} dB",ch, usrp->get_tx_gain(ch)))
        }

        if (!tx_ant.empty()) {
            usrp->set_tx_antenna(tx_ant, ch);
            UHD_LOG_INFO("CONFIG", format("Tx(ch={}) antenna: {}",ch, usrp->get_tx_antenna(ch)));
        }

        if (vm.contains("tx-bw")) {
            UHD_LOG_INFO("CONFIG", format("Setting Tx(ch={}) bandwidth={:.3f} MHz",ch,tx_bw/1e6))
            usrp->set_tx_bandwidth(tx_bw, ch);
        }
        UHD_LOG_INFO("CONFIG", format("Tx(ch={}) bandwidth={:.3f} MHz", ch, usrp->get_tx_bandwidth(ch)/1e6))

    }

    // Configure RX channels
    UHD_LOG_TRACE("CONFIG", "Configuring RX channels")
    for (size_t ch: rx_channels) {
        if (vm.contains("rx-gain")) {
            UHD_LOG_INFO("CONFIG", format("Setting Rx(ch={}) gain to: {:.1f} dB",ch,rx_gain))
            usrp->set_rx_gain(rx_gain, ch);
            // usrp->set_normalized_rx_gain(0.3);
            UHD_LOG_INFO("CONFIG", format("Actual Rx(ch={}) gain: {:.1f} dB",ch, usrp->get_rx_gain(ch)))
        }

        if (!rx_ant.empty()) {
            usrp->set_rx_antenna(rx_ant, ch);
            UHD_LOG_INFO("CONFIG", format("Actual Rx(ch={}) antenna: {}",ch, usrp->get_rx_antenna(ch)))
        }
        if (vm.contains("rx-bw")) {
            UHD_LOG_INFO("CONFIG", format("Setting Rx(ch={}) bandwidth={:.3f} MHz",ch,rx_bw/1e6))
            usrp->set_rx_bandwidth(rx_bw, ch);
        }
        UHD_LOG_INFO("CONFIG", format("Rx(ch={}) bandwidth={:.3f} MHz", ch, usrp->get_rx_bandwidth(ch)/1e6))
    }

    // Configure clock reference
    UHD_LOG_INFO("CONFIG", format("Setting clock reference to: {}", ref));
    usrp->set_clock_source(ref);

    // Configure time reference
    if (ref == "external" || ref == "gpsdo") {
        UHD_LOG_INFO("CONFIG", std::format("Setting time reference to: {}", ref));
        usrp->set_time_source("external");
    } else {
        usrp->set_time_source("internal");
    }

    // Wait for PPS sync and set time
    UHD_LOG_INFO("CONFIG", "Waiting for PPS sync and setting time...");
    // usrp->set_time_unknown_pps(uhd::time_spec_t(0.0));

    const uhd::time_spec_t last_pps_time = usrp->get_time_last_pps();
    while (last_pps_time == usrp->get_time_last_pps()) {
        std::this_thread::sleep_for(100ms);
    }
    // This command will be processed fairly soon after the last PPS edge:
    usrp->set_time_next_pps(uhd::time_spec_t(0.0));
    std::this_thread::sleep_for(1100ms);
    // Display current USRP time
    UHD_LOG_INFO("CONFIG", std::format("Current USRP time: {:.6f} seconds", usrp->get_time_now().get_real_secs()));


    UHD_ASSERT_THROW(tx_freqs.size() >= tx_channels.size())
    UHD_ASSERT_THROW(rx_freqs.size() >= rx_channels.size())
    // Sync tx and tx
    UHD_LOG_INFO("CONFIG", "Start Sync tune Request for Tx and Rx")
    usrp->set_command_time(uhd::time_spec_t(0.5),
                           uhd::usrp::multi_usrp::ALL_MBOARDS);
    for (const auto &[ch, freq_]: zip(tx_channels, tx_freqs)) {
        uhd::tune_request_t tune_req(freq_);
        tune_req.args = uhd::device_addr_t("mode_n=integer");
        usrp->set_tx_freq(tune_req, ch);
    }
    for (const auto &[ch, freq_]: zip(rx_channels, rx_freqs)) {
        uhd::tune_request_t tune_req(freq_);
        tune_req.args = uhd::device_addr_t("mode_n=integer");
        usrp->set_rx_freq(tune_req, ch);
    }

    usrp->clear_command_time(uhd::usrp::multi_usrp::ALL_MBOARDS);

    std::this_thread::sleep_for(500ms);

    for (auto ch: tx_channels) {
        UHD_LOG_INFO("CONFIG", std::format("Tx channel {} freq set to {:.3f} MHz", ch, usrp->get_tx_freq(ch)/1e6));
    }
    for (auto ch: rx_channels) {
        UHD_LOG_INFO("CONFIG", std::format("Rx channel {} freq set to {:.3f} MHz", ch, usrp->get_rx_freq(ch)/1e6));
    }


    // Set sample rate
    UHD_LOG_INFO("CONFIG", std::format("Setting sample rate to: Tx={:.3f} Rx={:.3f} Msps", tx_rate/1e6,rx_rate/1e6));
    usrp->set_tx_rate(tx_rate, uhd::usrp::multi_usrp::ALL_CHANS);
    usrp->set_rx_rate(rx_rate, uhd::usrp::multi_usrp::ALL_CHANS);
    UHD_LOG_INFO("CONFIG", std::format("Actual Tx sample rate: {:.3f} Msps", usrp->get_tx_rate()/1e6));
    UHD_LOG_INFO("CONFIG", std::format("Actual Rx sample rate: {:.3f} Msps", usrp->get_rx_rate()/1e6));


    // Check LO lock status
    UHD_LOG_INFO("SYSTEM", "Checking LO lock status...");

    // Check TX LO lock
    for (size_t ch: tx_channels) {
        if (auto sensor_names = usrp->get_tx_sensor_names(ch);
            std::ranges::find(sensor_names, "lo_locked") != sensor_names.end()) {
            auto lo_locked = usrp->get_tx_sensor("lo_locked", ch);
            UHD_LOG_INFO("SYSTEM", format("Checking Tx(ch={}): {}",ch, lo_locked.to_pp_string()));
            UHD_ASSERT_THROW(lo_locked.to_bool());
        }
    }

    // Check RX LO lock
    for (size_t ch: rx_channels) {
        if (auto sensor_names = usrp->get_rx_sensor_names(ch);
            std::ranges::find(sensor_names, "lo_locked") != sensor_names.end()) {
            auto lo_locked = usrp->get_rx_sensor("lo_locked", ch);
            UHD_LOG_INFO("SYSTEM", format("Checking Rx(ch={}): {}",ch, lo_locked.to_pp_string()));
            UHD_ASSERT_THROW(lo_locked.to_bool());
        }
    }
    // Check Ref lock
    UHD_LOG_INFO("SYSTEM", "Checking REF lock status...");
    size_t num_mboards = usrp->get_num_mboards();
    if (ref == "external") {
        for (auto mboard: std::views::iota(static_cast<size_t>(0), num_mboards)) {
            if (auto sensor_names = usrp->get_mboard_sensor_names(mboard);
                std::ranges::find(sensor_names, "ref_locked") != sensor_names.end()) {
                auto ref_locked = usrp->get_mboard_sensor("ref_locked", mboard);
                UHD_LOG_INFO("SYSTEM", format("Checking mboard(={}): {}", mboard, ref_locked.to_pp_string()));
                UHD_ASSERT_THROW(ref_locked.to_bool());
            }
        }
    }
    if (ref == "mimo") {
        for (auto mboard: std::views::iota(static_cast<size_t>(0), num_mboards)) {
            if (auto sensor_names = usrp->get_mboard_sensor_names(mboard);
                std::ranges::find(sensor_names, "mimo_locked") != sensor_names.end()) {
                auto ref_locked = usrp->get_mboard_sensor("mimo_locked", mboard);
                UHD_LOG_INFO("SYSTEM", format("Checking mboard(={}): {}", mboard, ref_locked.to_pp_string()));
                UHD_ASSERT_THROW(ref_locked.to_bool());
            }
        }
    }


    // Create TX stream
    UHD_LOG_TRACE("STREAM", "Creating TX stream");
    uhd::stream_args_t tx_stream_args("fc32", "sc16");
    tx_stream_args.channels = tx_channels;
    uhd::tx_streamer::sptr tx_stream = usrp->get_tx_stream(tx_stream_args);

    // Create RX stream
    UHD_LOG_TRACE("STREAM", "Creating RX stream");
    uhd::stream_args_t rx_stream_args("fc32", "sc16");
    rx_stream_args.channels = rx_channels;
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(rx_stream_args);

    // Calculate start time
    uhd::time_spec_t seconds_in_future = usrp->get_time_now() + uhd::time_spec_t(delay);
    UHD_LOG_INFO(
        "SYSTEM",
        std::format("Start time: {:.3f} seconds in the future (absolute time: {:.6f})", delay, seconds_in_future.
            get_real_secs()));


    // Start transmission thread
    UHD_LOG_INFO("SYSTEM", "Starting transmission thread...");
    auto transmit_thread = std::async(std::launch::async, transmit_from_file_worker,
                                      tx_stream, tx_files, spb, seconds_in_future
    );
    size_t nums_to_recv = sizes.front() / sizeof(complexf);

    auto receive_thread = std::async(std::launch::async, receive_to_file_worker,
                                     rx_stream, rx_files, spb, seconds_in_future, nums_to_recv);

    // Wait for both threads to complete
    UHD_LOG_TRACE("SYSTEM", "Waiting for TX and RX threads to complete");
    transmit_thread.wait();
    receive_thread.wait();

    stop_signal_called = true;
    UHD_LOG_INFO("SYSTEM", "TX-RX operation finished!")


    return EXIT_SUCCESS;
}
