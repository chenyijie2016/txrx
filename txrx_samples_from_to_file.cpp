#if __cplusplus < 202302L
#error "should use C++23 implmentation"
#endif


#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <uhd/convert.hpp>
#include <uhd/exception.hpp>
#include <uhd/property_tree.hpp>
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
#include <ranges>
#include "workers.h"

namespace po = boost::program_options;
namespace fs = std::filesystem;

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


/**
 * Checks if a sensor is locked with timeout
 *
 * @param sensor_names Vector of available sensor names
 * @param sensor_name Name of the sensor to check (e.g., "lo_locked")
 * @param get_sensor_fn Function to retrieve sensor value
 * @param setup_time Time to wait for consecutive locks (default 1.0s)
 * @return true if sensor is locked and stable, false if sensor doesn't exist
 * @throws std::runtime_error if timeout occurs while waiting for lock
 */
bool check_locked_sensor(
    vector<string> sensor_names,
    const char *sensor_name,
    std::function<uhd::sensor_value_t(const std::string &)> get_sensor_fn,
    double setup_time = 1.0) {
    // Check if the requested sensor exists
    if (std::find(sensor_names.begin(), sensor_names.end(), sensor_name)
        == sensor_names.end()) {
        UHD_LOG_WARNING("SENSOR", "Sensor \"" << sensor_name << "\" not available on this device")
        return false;
    }

    const auto setup_timeout = std::chrono::steady_clock::now() + 1s;
    bool lock_detected = false;

    UHD_LOG_INFO("SENSOR", "Waiting for sensor \"" << sensor_name << "\" lock: ")

    while (true) {
        if (lock_detected && (std::chrono::steady_clock::now() > setup_timeout)) {
            UHD_LOG_INFO("SENSOR", "Locked successfully")
            break;
        }
        if (get_sensor_fn(sensor_name).to_bool()) {
            UHD_LOG_TRACE("SENSOR", "Lock detected for \"" << sensor_name << "\"")
            lock_detected = true;
        } else {
            if (std::chrono::steady_clock::now() > setup_timeout) {
                UHD_LOG_ERROR("SENSOR", "Timed out waiting for consecutive locks on sensor \"" << sensor_name << "\"")
                throw std::runtime_error(
                    str(boost::format(
                            "timed out waiting for consecutive locks on sensor \"%s\"")
                        % sensor_name));
            }
            UHD_LOG_TRACE("SENSOR", "Waiting for lock on \"" << sensor_name << "\"")
        }
        std::this_thread::sleep_for(100ms);
    }

    return true;
}

int UHD_SAFE_MAIN(int argc, char* argv[]) {
    // Initialize USRP device and file parameters
    std::string args, tx_file, rx_file;
    std::string tx_ant, rx_ant, ref;
    std::string tx_channels_str, rx_channels_str;
    std::vector<size_t> tx_channels, rx_channels;
    size_t spb; // Samples per buffer
    double rate, tx_gain, rx_gain, bw, delay, freq;
    size_t nsamps = 0; // Number of samples to receive, 0 means until TX complete
    std::vector<std::string> tx_files, rx_files;
    vector<double> tx_freqs, rx_freqs;

    // Program description
    const std::string program_doc =
            "Simultaneous TX/RX samples from/to file - Optimized from official UHD example\n";

    // Configure command line options
    po::options_description desc("Command line options");
    desc.add_options()
            ("help,h", "Show this help message")
            ("args", po::value<std::string>(&args)->default_value("addr=192.168.180.2"),
             "USRP device address string")
            ("tx-files",
             po::value<std::vector<std::string> >(&tx_files)->multitoken()->default_value(
                 {"tx_data_fc32.bin"}, "tx_data_fc32.bin"),
             "TX data files (fc32 format)")
            ("rx-files",
             po::value<std::vector<std::string> >(&rx_files)->multitoken()->default_value(
                 {"rx_data_fc32.bin"}, "rx_data_fc32.bin"),
             "RX data files (fc32 format)")
            ("tx-ant", po::value<std::string>(&tx_ant)->default_value("TX/RX"),
             "TX antenna selection")
            ("rx-ant", po::value<std::string>(&rx_ant)->default_value("RX2"),
             "RX antenna selection")
            ("tx-channels",
             po::value<std::vector<size_t> >(&tx_channels)->multitoken()->default_value({0}, "0"),
             "TX channels (space separated)")
            ("rx-channels",
             po::value<std::vector<size_t> >(&rx_channels)->multitoken()->default_value({1}, "1"),
             "RX channels (space separated)")
            ("spb", po::value<size_t>(&spb)->default_value(2500),
             "Samples per buffer")
            ("rate", po::value<double>(&rate)->default_value(5e6),
             "Sample rate (Hz)")
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
            ("bw", po::value<double>(&bw),
             "Bandwidth (Hz)")
            ("delay", po::value<double>(&delay)->default_value(1),
             "Delay before start (seconds)")
            ("nsamps", po::value<size_t>(&nsamps)->default_value(0),
             "Number of samples to receive, 0 means until TX complete")
            ("ref", po::value<std::string>(&ref)->default_value("internal"),
             "Reference: internal, external, gpsdo");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
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
    // UHD_LOG_INFO("MAIN", "" << __cplusplus)
    // Register signal handler for graceful shutdown
    UHD_LOG_TRACE("SYSTEM", "Registering signal handler")
    std::signal(SIGINT, &sig_int_handler);


    // Create USRP device
    UHD_LOG_INFO("SYSTEM", "Creating USRP device with args: " << args)
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);
    // Display device information
    UHD_LOG_INFO("SYSTEM", "USRP device info: " << usrp->get_pp_string());

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
    if (not all_of(tx_files, [](const std::string &filename) {
        return fs::exists(filename);
    })) {
        UHD_LOG_ERROR("PRE-CHECK", "TX input files do not exist!")
        return EXIT_FAILURE;
    }

    // Verify all TX files have the same size
    std::vector<std::uintmax_t> sizes;
    transform(tx_files, std::back_inserter(sizes),
              [](const std::string &f) { return fs::file_size(f); });

    if (adjacent_find(sizes, std::not_equal_to{}) != sizes.end()) {
        UHD_LOG_ERROR("PRE-CHECK", "TX file sizes mismatch")
        return EXIT_FAILURE;
    }

    // Configure TX channels
    UHD_LOG_TRACE("CONFIG", "Configuring TX channels")
    for (size_t ch: tx_channels) {
        // Set TX gain
        if (vm.count("tx-gain")) {
            UHD_LOG_INFO("CONFIG", std::format("Setting TX gain to: {:.2f} dB", tx_gain))
            usrp->set_tx_gain(tx_gain, ch);
            UHD_LOG_INFO("CONFIG", std::format("Actual TX gain: {:.2f} dB", usrp->get_tx_gain(ch)))
        }


        UHD_LOG_INFO("CONFIG", std::format("Actual TX frequency: {:.3f} MHz", usrp->get_tx_freq(ch) / 1e6))

        if (!tx_ant.empty()) {
            usrp->set_tx_antenna(tx_ant, ch);
            UHD_LOG_INFO("CONFIG", std::format("TX antenna: {}", usrp->get_tx_antenna(ch)));
        }
    }

    // Configure RX channels
    UHD_LOG_TRACE("CONFIG", "Configuring RX channels")
    for (size_t ch: rx_channels) {
        if (vm.count("rx-gain")) {
            UHD_LOG_INFO("CONFIG", std::format("Setting RX gain to: {:.1f} dB",rx_gain))
            usrp->set_rx_gain(rx_gain, ch);
            // usrp->set_normalized_rx_gain(0.3);
            UHD_LOG_INFO("CONFIG", std::format("Actual RX gain: {:.1f} dB", usrp->get_rx_gain(ch)))
        }

        // uhd::tune_request_t tune_request(freq);
        // usrp->set_rx_freq(tune_request, ch);
        UHD_LOG_INFO("CONFIG", std::format("Actual RX frequency: {:.3f} MHz", usrp->get_rx_freq(ch) / 1e6))

        if (!rx_ant.empty()) {
            usrp->set_rx_antenna(rx_ant, ch);
            UHD_LOG_INFO("CONFIG", std::format("Actual RX antenna: {}", usrp->get_rx_antenna(ch)))
        }
    }

    // Configure clock reference
    UHD_LOG_INFO("CONFIG", std::format("Setting clock reference to: {}", ref));
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

    // Display current USRP time
    UHD_LOG_INFO("CONFIG", std::format("Current USRP time: {:.6f} seconds", usrp->get_time_now().get_real_secs()));


    UHD_ASSERT_THROW(tx_freqs.size() >= tx_channels.size())
    UHD_ASSERT_THROW(rx_freqs.size() >= rx_channels.size())
    // Sync tx and tx
    UHD_LOG_INFO("CONFIG", "Start Sync tune Request for TX and RX")
    usrp->set_command_time(uhd::time_spec_t(0.5),
                           uhd::usrp::multi_usrp::ALL_MBOARDS);
    for (const auto &[ch, freq]: zip(tx_channels, tx_freqs)) {
        usrp->set_tx_freq(freq, ch);
    }
    for (const auto &[ch, freq]: zip(rx_channels, rx_freqs)) {
        usrp->set_rx_freq(freq, ch);
    }

    usrp->clear_command_time(uhd::usrp::multi_usrp::ALL_MBOARDS);

    for (auto ch: tx_channels) {
        UHD_LOG_INFO("CONFIG", std::format("TX channel {} freq set to {:.3f} MHz", ch, usrp->get_tx_freq(ch)/1e6));
    }
    for (auto ch: rx_channels) {
        UHD_LOG_INFO("CONFIG", std::format("RX channel {} freq set to {:.3f} MHz", ch, usrp->get_rx_freq(ch)/1e6));
    }


    // Set sample rate
    UHD_LOG_INFO("CONFIG", std::format("Setting sample rate to: {:.3f} Msps", rate/1e6));
    usrp->set_tx_rate(rate, uhd::usrp::multi_usrp::ALL_CHANS);
    usrp->set_rx_rate(rate, uhd::usrp::multi_usrp::ALL_CHANS);
    UHD_LOG_INFO("CONFIG", std::format("Actual TX sample rate: {:.3f} Msps", usrp->get_tx_rate()/1e6));
    UHD_LOG_INFO("CONFIG", std::format("Actual RX sample rate: {:.3f} Msps", usrp->get_rx_rate()/1e6));


    // Bandwidth settings are optional
    // if (vm.count("bw")) {
    //     UHD_LOG_INFO("CONFIG", std::format("Setting bandwidth to: {:.3f} MHz", bw/1e6));
    //     usrp->set_tx_bandwidth(bw);
    //     usrp->set_rx_bandwidth(bw);
    //     UHD_LOG_INFO("CONFIG", std::format("Actual TX bandwidth: {:.3f} MHz", usrp->get_tx_bandwidth()/1e6));
    //     UHD_LOG_INFO("CONFIG", std::format("Actual RX bandwidth: {:.3f} MHz", usrp->get_rx_bandwidth()/1e6));
    // }

    // Configure Freqs


    // Check LO lock status
    UHD_LOG_INFO("SYSTEM", "Checking LO lock status...");

    // Check TX LO lock
    for (size_t ch: tx_channels) {
        auto sensor_names = usrp->get_tx_sensor_names(ch);
        if (check_locked_sensor(
            sensor_names,
            "lo_locked",
            [usrp, ch](const std::string &sensor_name) {
                return usrp->get_tx_sensor(sensor_name, ch);
            })) {
            UHD_LOG_INFO("SYSTEM", "TX channel " << ch << " LO is locked");
        }
    }

    // Check RX LO lock
    for (size_t ch: rx_channels) {
        auto sensor_names = usrp->get_rx_sensor_names(ch);
        if (check_locked_sensor(
            sensor_names,
            "lo_locked",
            [usrp, ch](const std::string &sensor_name) {
                return usrp->get_rx_sensor(sensor_name, ch);
            })) {
            UHD_LOG_INFO("SYSTEM", "RX channel " << ch << " LO is locked");
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
    UHD_LOG_INFO("THREAD", "Starting transmission thread...");
    auto transmit_thread = std::async(std::launch::async, transmit_from_file_worker,
                                      tx_stream, tx_files, spb, seconds_in_future
    );
    size_t nums_to_recv = sizes.front() / sizeof(complexf);

    auto receive_thread = std::async(std::launch::async, receive_to_file_worker,
                                     rx_stream, rx_files, spb, seconds_in_future, nums_to_recv);

    // Wait for both threads to complete
    UHD_LOG_TRACE("THREAD", "Waiting for TX and RX threads to complete");
    transmit_thread.wait();
    receive_thread.wait();

    stop_signal_called = true;
    UHD_LOG_INFO("SYSTEM", "TX-RX operation finished!")


    return EXIT_SUCCESS;
}
