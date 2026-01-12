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


namespace po = boost::program_options;
namespace fs = std::filesystem;
namespace stdr = std::ranges;
namespace stdv = std::views;

using namespace std::chrono_literals;
using std::vector;
using std::string;
using std::complex;
using std::format;


// Complex floating-point type for samples (fc32 format)
using complexf = std::complex<float>;

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

struct UsrpConfig {
    string args;
    string clock_source, time_source;
    vector<size_t> tx_channels, rx_channels;
    size_t spb; // Samples per buffer
    double rate, rx_bw, tx_bw, delay, freq;
    size_t nsamps{0}; // Number of samples to receive, 0 means until TX complete
    vector<double> tx_rates, rx_rates;
    vector<string> tx_files, rx_files;
    vector<double> tx_freqs, rx_freqs;
    vector<double> tx_gains, rx_gains;
    vector<string> tx_ants, rx_ants;
};

/**
 * Transmits samples from files to USRP using a streaming approach
 *
 * This function reads complex floating-point samples from specified files and streams them
 * to the USRP device using the provided TX streamer. It implements an efficient buffering
 * mechanism to ensure continuous transmission without interruption.
 *
 * @param tx_stream TX streamer to send samples to the USRP
 * @param filenames Vector of file paths containing fc32 format samples to transmit
 * @param spb Samples per buffer - number of samples to process in each iteration
 * @param start_time Time specification for when transmission should begin
 */
void transmit_from_file_worker(const uhd::tx_streamer::sptr &tx_stream, const vector<string> &filenames, size_t spb,
                               const uhd::time_spec_t &start_time) {
    // Initialize TX metadata
    uhd::tx_metadata_t md;
    md.start_of_burst = false;
    md.end_of_burst = false;
    md.has_time_spec = true;
    md.time_spec = start_time;
    double timeout = 5;
    bool first_packet = true;

    // Create buffers for each channel
    vector<std::vector<complexf> > buffs(tx_stream->get_num_channels(), std::vector<complexf>(spb));
    vector<complexf *> buff_ptrs;
    for (auto &buff: buffs) {
        buff_ptrs.push_back(buff.data());
    }

    // Open input files
    auto infiles = stdr::to<vector>(
        filenames
        | stdv::transform([](const string &file) {
            return std::make_shared<std::ifstream>(
                file, std::ios::binary);
        }));

    // std::ranges::transform(filenames, std::back_inserter(infiles), [&](const std::string &f) {
    //     return std::make_shared<std::ifstream>(f, std::ios::binary);
    // });
    size_t num_samps_transmitted = 0;
    bool eof = false;

    // Track buffer state
    size_t buf_valid_samps = 0; // Number of valid samples in buffer
    size_t buf_sent_samps = 0; // Number of samples already sent from buffer

    UHD_LOG_INFO("TX-STREAM", format("Starting transmission from {} file(s)",filenames.size()))
    for (const auto &file: filenames) {
        UHD_LOG_INFO("TX-STREAM", format("{}", file))
    }


    while (!stop_signal_called) {
        /* ---------- Read from files when buffer is empty ---------- */
        if (buf_sent_samps == buf_valid_samps && !eof) {
            buf_sent_samps = 0;
            buf_valid_samps = 0;
            eof = false;

            // Read samples from all files simultaneously
            for (size_t ch = 0; ch < tx_stream->get_num_channels(); ++ch) {
                infiles[ch]->read(
                    reinterpret_cast<char *>(buffs[ch].data()),
                    spb * sizeof(complexf)
                );
                size_t read_samps =
                        infiles[ch]->gcount() / sizeof(complexf);

                if (ch == 0) {
                    buf_valid_samps = read_samps;
                } else {
                    // Take minimum across all channels to ensure synchronized data
                    buf_valid_samps =
                            std::min(buf_valid_samps, read_samps);
                }
            }

            if (buf_valid_samps == 0) {
                eof = true;
            }
        }

        if (buf_valid_samps == 0 && eof) {
            UHD_LOG_DEBUG("TX-STREAM", "Reached end of input files, exiting transmission loop")
            break;
        }

        /* ---------- Send remaining samples ---------- */
        size_t samps_to_send = buf_valid_samps - buf_sent_samps;

        std::vector<complexf *> offset_ptrs(tx_stream->get_num_channels());
        for (size_t ch = 0; ch < tx_stream->get_num_channels(); ++ch) {
            offset_ptrs[ch] = buffs[ch].data() + buf_sent_samps;
        }

        size_t samps_sent = tx_stream->send(
            offset_ptrs,
            samps_to_send,
            md,
            timeout
        );

        if (samps_sent == 0) {
            UHD_LOG_WARNING("TX-STREAM", "send() returned 0 samples");
            continue;
        }
        num_samps_transmitted += samps_sent;
        buf_sent_samps += samps_sent;
        first_packet = false;
        timeout = 0.1;

        /* ---------- End of file and buffer completely sent ---------- */
        if (eof && buf_sent_samps == buf_valid_samps) {
            md.end_of_burst = true;
            tx_stream->send("", 0, md);
            UHD_LOG_DEBUG("TX-STREAM", "End of burst transmitted")
            break;
        }
    }

    // Finalize transmission
    md.end_of_burst = true;
    tx_stream->send("", 0, md);

    // Close all input files
    std::ranges::for_each(infiles, [](auto f) {
        if (f->is_open()) {
            f->close();
        }
    });
    UHD_LOG_INFO("TX-STREAM", "Transmit completed! Samples sent: " << num_samps_transmitted);
}


/**
 * Receives samples from USRP to files using a streaming approach
 *
 * This function receives complex floating-point samples from the USRP device using the
 * provided RX streamer and writes them to specified files. The function streams until
 * the specified number of samples have been received or the stop signal is called.
 *
 * @param rx_stream RX streamer to receive samples from the USRP
 * @param filenames Vector of file paths to save the received fc32 format samples
 * @param spb Samples per buffer - number of samples to process in each iteration
 * @param start_time Time specification for when reception should begin
 * @param num_samps_to_recv Total number of samples to receive before stopping
 */
void ReceiveToFileWorker(const uhd::rx_streamer::sptr &rx_stream, const std::vector<std::string> &filenames,
                         size_t spb,
                         const uhd::time_spec_t &start_time, size_t num_samps_to_recv) {
    // Create output files for each channel
    std::vector<std::shared_ptr<std::ofstream> > outfiles;

    for (auto &file: filenames) {
        outfiles.push_back(std::make_shared<std::ofstream>(file, std::ofstream::binary));

        if (not outfiles.back()->is_open()) {
            UHD_LOG_ERROR("RX-STREAM", format("Cannot open receive file: {}" , file));
            throw std::runtime_error("Cannot open receive file:" + file);
        }
        UHD_LOG_INFO("RX-STREAM", format("Rx channel saving to file: {}", file));
    }

    // Create buffers for each channel
    std::vector<std::vector<complexf> > buffs(rx_stream->get_num_channels(), std::vector<complexf>(spb));
    std::vector<complexf *> buff_ptrs;
    for (auto &buff: buffs) {
        buff_ptrs.push_back(&buff.front());
    }

    // Initialize reception parameters
    bool first_packet = true;
    double timeout = 5;
    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    stream_cmd.num_samps = num_samps_to_recv;
    stream_cmd.stream_now = false;
    stream_cmd.time_spec = start_time;

    UHD_LOG_INFO("RX-STREAM", "Starting reception, will receive " << num_samps_to_recv << " samples")
    UHD_LOG_DEBUG("RX-STREAM", "Reception start time: " << start_time.get_real_secs() << " seconds")

    // Issue stream command to start reception
    rx_stream->issue_stream_cmd(stream_cmd);

    uhd::rx_metadata_t md;
    size_t num_samps_received = 0;

    // Main reception loop
    while (not stop_signal_called && (num_samps_received < num_samps_to_recv)) {
        size_t num_rx_samps = rx_stream->recv(buff_ptrs, spb, md, timeout);

        if (first_packet) {
            timeout = 0.1; // Reduce timeout after first packet
            first_packet = false;
        }

        // Handle different error conditions
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            UHD_LOG_WARNING("RX-STREAM", "RX channel received timeout.");
            continue;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            UHD_LOG_WARNING("RX-STREAM", "RX channel received overflow.");
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            UHD_LOG_ERROR("RX-STREAM", "RX channel received error: " << md.strerror());
            throw std::runtime_error("Receive error: " + md.strerror());
        }

        // Write received samples to output files
        for (auto i = 0; i < outfiles.size(); ++i) {
            outfiles[i]->write(reinterpret_cast<char *>(buffs[i].data()), num_rx_samps * sizeof(complexf));
        }
        num_samps_received += num_rx_samps;
    }

    // Close all output files
    for (auto &outfile: outfiles) {
        if (outfile->is_open()) {
            outfile->close();
        }
    }

    UHD_LOG_INFO("RX-STREAM", "Receive completed! Samples received: " << num_samps_received);
}


bool ValidateUsrpConfiguration(const UsrpConfig &config, const uhd::usrp::multi_usrp::sptr &usrp) {
    // Check channel validity
    size_t total_tx_channels = usrp->get_tx_num_channels();
    size_t total_rx_channels = usrp->get_rx_num_channels();

    {
        std::ostringstream oss;
        oss << "TX channels: ";
        for (size_t ch: config.tx_channels) oss << ch << " ";
        UHD_LOG_INFO("CHECK", oss.str());
    }

    {
        std::ostringstream oss;
        oss << "RX channels: ";
        for (size_t ch: config.rx_channels) oss << ch << " ";
        UHD_LOG_INFO("CHECK", oss.str());
    }

    if (stdr::any_of(config.tx_channels, [&](const auto &ch) {
        return ch >= total_tx_channels;
    })) {
        UHD_LOG_ERROR("CHECK", "TX channels are not supported");
        return false;
    }

    if (stdr::any_of(config.rx_channels, [&](const auto &ch) {
        return ch >= total_rx_channels;
    })) {
        UHD_LOG_ERROR("CHECK", "RX channels are not supported");
    }
    vector<size_t> tx_sizes = {
        config.tx_channels.size(),
        config.tx_files.size(),
        config.tx_ants.size(),
        config.tx_gains.size(),
        config.tx_freqs.size()
    };
    if (stdr::adjacent_find(tx_sizes, std::not_equal_to{}) != tx_sizes.end()) {
        UHD_LOG_ERROR("CHECK", "Tx configurations mismatch!");
        return false;
    }

    vector<size_t> rx_sizes = {
        config.rx_channels.size(),
        config.rx_files.size(),
        config.rx_ants.size(),
        config.rx_gains.size(),
        config.rx_freqs.size()
    };
    if (stdr::adjacent_find(rx_sizes, std::not_equal_to{}) != rx_sizes.end()) {
        UHD_LOG_ERROR("CHECK", "Rx configurations mismatch!");
        return false;
    }


    // Verify TX input files exist
    if (not stdr::all_of(config.tx_files, [](const string &filename) {
        return fs::exists(filename);
    })) {
        UHD_LOG_ERROR("CHECK", "TX input files do not exist!")
        return EXIT_FAILURE;
    }

    // Verify all TX files have the same size
    vector<std::uintmax_t> sizes = stdr::to<vector>(
        config.tx_files
        | stdv::transform([](const string &f) {
            return fs::file_size(f);
        }));


    if (stdr::adjacent_find(sizes, std::not_equal_to{}) != sizes.end()) {
        UHD_LOG_ERROR("CHECK", "Tx file sizes mismatch")
        return false;
    }
    UHD_LOG_INFO("CHECK", "The input parameters appear to be correct.")
    return true;
}

void ApplyUsrpConfiguration(const UsrpConfig &config, const uhd::usrp::multi_usrp::sptr &usrp) {
    UHD_LOG_INFO("CONFIG", format("====== Configuring Tx ======"))
    for (const auto &[index, ch]: stdv::enumerate(config.tx_channels)) {
        UHD_LOG_INFO("CONFIG", format("====== Tx Channel {}", ch))

        // gain
        usrp->set_tx_gain(config.tx_gains[index], ch);
        UHD_LOG_INFO("CONFIG", format("Gain: {:.2f} dB", usrp->get_tx_gain(ch)))

        // ant
        usrp->set_tx_antenna(config.tx_ants[index], ch);
        UHD_LOG_INFO("CONFIG", format("Ant : {}", usrp->get_tx_antenna(ch)));

        // bw
        // if (vm.contains("tx-bw")) {
        //     UHD_LOG_INFO("CONFIG", format("Setting Tx(ch={}) bandwidth={:.3f} MHz",ch,tx_bw/1e6))
        //     usrp->set_tx_bandwidth(tx_bw, ch);
        //     UHD_LOG_INFO("CONFIG", format("Tx(ch={}) bandwidth={:.3f} MHz", ch, usrp->get_tx_bandwidth(ch)/1e6))
        // }
        // rate
        usrp->set_tx_rate(config.tx_rates[index], ch);
        UHD_LOG_INFO("CONFIG", std::format("Rate: {:.3f} Msps", usrp->get_tx_rate(ch)/1e6));
    }
    UHD_LOG_INFO("CONFIG", format("============================"))
    UHD_LOG_INFO("CONFIG", format("====== Configuring Rx ======"))
    // Configure RX channels
    for (const auto &[index, ch]: stdv::enumerate(config.rx_channels)) {
        UHD_LOG_INFO("CONFIG", format("====== Rx Channel {}", ch))

        usrp->set_rx_gain(config.rx_gains[index], ch);
        UHD_LOG_INFO("CONFIG", format("Gain: {:.1f} dB", usrp->get_rx_gain(ch)))


        usrp->set_rx_antenna(config.rx_ants[index], ch);
        UHD_LOG_INFO("CONFIG", format("Ant : {}", usrp->get_rx_antenna(ch)))

        // if (vm.contains("rx-bw")) {
        //     UHD_LOG_INFO("CONFIG", format("Setting Rx(ch={}) bandwidth={:.3f} MHz",ch,rx_bw/1e6))
        //     usrp->set_rx_bandwidth(rx_bw, ch);
        //     UHD_LOG_INFO("CONFIG", format("Rx(ch={}) bandwidth={:.3f} MHz", ch, usrp->get_rx_bandwidth(ch)/1e6))
        // }


        // rate
        usrp->set_rx_rate(config.rx_rates[index], ch);
        UHD_LOG_INFO("CONFIG", std::format("Rate: {:.3f} Msps", usrp->get_rx_rate(ch)/1e6));
    }
    UHD_LOG_INFO("CONFIG", format("============================"))
    // Configure clock reference
    UHD_LOG_INFO("CONFIG", format("Setting clock reference to: {}", config.clock_source));
    usrp->set_clock_source(config.clock_source);

    // Configure time reference
    if (config.clock_source == "external" || config.clock_source == "gpsdo") {
        UHD_LOG_INFO("CONFIG", std::format("Setting time reference to: {}", config.clock_source));
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

    // Sync tx and tx
    UHD_LOG_INFO("CONFIG", "Start Sync tune Request for Tx and Rx")
    usrp->set_command_time(uhd::time_spec_t(0.5),
                           uhd::usrp::multi_usrp::ALL_MBOARDS);
    for (const auto &[ch, freq_]: stdv::zip(config.tx_channels, config.tx_freqs)) {
        uhd::tune_request_t tune_req(freq_);
        tune_req.args = uhd::device_addr_t("mode_n=integer");
        usrp->set_tx_freq(tune_req, ch);
    }
    for (const auto &[ch, freq_]: stdv::zip(config.rx_channels, config.rx_freqs)) {
        uhd::tune_request_t tune_req(freq_);
        tune_req.args = uhd::device_addr_t("mode_n=integer");
        usrp->set_rx_freq(tune_req, ch);
    }

    usrp->clear_command_time(uhd::usrp::multi_usrp::ALL_MBOARDS);

    std::this_thread::sleep_for(500ms);

    for (const size_t ch: config.tx_channels) {
        UHD_LOG_INFO("CONFIG", std::format("Tx channel {} freq set to {:.3f} MHz", ch, usrp->get_tx_freq(ch)/1e6));
    }
    for (const size_t ch: config.rx_channels) {
        UHD_LOG_INFO("CONFIG", std::format("Rx channel {} freq set to {:.3f} MHz", ch, usrp->get_rx_freq(ch)/1e6));
    }


    // Check LO lock status
    UHD_LOG_INFO("SYSTEM", "Checking LO lock status...");

    // Check TX LO lock
    for (size_t ch: config.tx_channels) {
        if (auto sensor_names = usrp->get_tx_sensor_names(ch);
            std::ranges::find(sensor_names, "lo_locked") != sensor_names.end()) {
            auto lo_locked = usrp->get_tx_sensor("lo_locked", ch);
            UHD_LOG_INFO("SYSTEM", format("Checking Tx(ch={}): {}",ch, lo_locked.to_pp_string()));
            UHD_ASSERT_THROW(lo_locked.to_bool());
        }
    }

    // Check RX LO lock
    for (size_t ch: config.rx_channels) {
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
    if (config.clock_source == "external") {
        for (auto mboard: std::views::iota(static_cast<size_t>(0), num_mboards)) {
            if (auto sensor_names = usrp->get_mboard_sensor_names(mboard);
                std::ranges::find(sensor_names, "ref_locked") != sensor_names.end()) {
                auto ref_locked = usrp->get_mboard_sensor("ref_locked", mboard);
                UHD_LOG_INFO("SYSTEM", format("Checking mboard(={}): {}", mboard, ref_locked.to_pp_string()));
                UHD_ASSERT_THROW(ref_locked.to_bool());
            }
        }
    }
    if (config.clock_source == "mimo") {
        for (auto mboard: std::views::iota(static_cast<size_t>(0), num_mboards)) {
            if (auto sensor_names = usrp->get_mboard_sensor_names(mboard);
                std::ranges::find(sensor_names, "mimo_locked") != sensor_names.end()) {
                auto ref_locked = usrp->get_mboard_sensor("mimo_locked", mboard);
                UHD_LOG_INFO("SYSTEM", format("Checking mboard(={}): {}", mboard, ref_locked.to_pp_string()));
                UHD_ASSERT_THROW(ref_locked.to_bool());
            }
        }
    }
}


int UHD_SAFE_MAIN(int argc, char* argv[]) {
    // Initialize USRP device and file parameters
    UsrpConfig config{};

    // Program description
    const string program_doc =
            "Simultaneous TX/RX samples from/to file.\nDesigned specifically for multi-channel synchronous transmission and reception\n";

    // Configure command line options
    po::options_description desc("Command line options");
    desc.add_options()
            ("help,h", "Show this help message")
            ("args", po::value<string>(&config.args)->default_value("addr=192.168.180.2"),
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
            ("nsamps", po::value<size_t>(&config.nsamps)->default_value(0),
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


    // Create USRP device
    UHD_LOG_INFO("SYSTEM", format("Creating USRP device with args: {}", config.args));
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(config.args);
    // Display device information
    UHD_LOG_INFO("SYSTEM", format("USRP device info: {}", usrp->get_pp_string()));

    if (vm.contains("rate")) {
        stdr::fill(config.tx_rates, config.rate);
        stdr::fill(config.rx_rates, config.rate);
        UHD_LOG_INFO("CONFIG", format("Set Tx and Rx rate to {:.3f} Mhz", config.rate/1e6))
    }
    if (not ValidateUsrpConfiguration(config, usrp)) {
        UHD_LOG_ERROR("SYSTEM", "Invalid configuration provided");
        return EXIT_FAILURE;
    }
    ApplyUsrpConfiguration(config, usrp);

    // Create TX stream
    UHD_LOG_TRACE("STREAM", "Creating TX stream");
    uhd::stream_args_t tx_stream_args("fc32", "sc16");
    tx_stream_args.channels = config.tx_channels;
    uhd::tx_streamer::sptr tx_stream = usrp->get_tx_stream(tx_stream_args);

    // Create RX stream
    UHD_LOG_TRACE("STREAM", "Creating RX stream");
    uhd::stream_args_t rx_stream_args("fc32", "sc16");
    rx_stream_args.channels = config.rx_channels;
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(rx_stream_args);

    // Calculate start time
    uhd::time_spec_t seconds_in_future = usrp->get_time_now() + uhd::time_spec_t(config.delay);
    UHD_LOG_INFO(
        "SYSTEM",
        std::format("Start time: {:.3f} seconds in the future (absolute time: {:.6f})", config.delay, seconds_in_future.
            get_real_secs()));


    // Start transmission thread
    UHD_LOG_INFO("SYSTEM", "Starting transmission thread...");
    auto transmit_thread = std::async(std::launch::async, transmit_from_file_worker,
                                      tx_stream, config.tx_files, config.spb, seconds_in_future
    );
    // size_t nums_to_recv = sizes.front() / sizeof(complexf);

    auto receive_thread = std::async(std::launch::async, ReceiveToFileWorker,
                                     rx_stream, config.rx_files, config.spb, seconds_in_future, config.nsamps);

    // Wait for both threads to complete
    UHD_LOG_TRACE("SYSTEM", "Waiting for TX and RX threads to complete");
    transmit_thread.wait();
    receive_thread.wait();

    stop_signal_called = true;
    UHD_LOG_INFO("SYSTEM", "TX-RX operation finished!")


    return EXIT_SUCCESS;
}
