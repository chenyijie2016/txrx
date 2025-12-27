#include "workers.h"
#include <complex>
#include <fstream>
#include <format>

// Global flag for signal handling
std::atomic<bool> stop_signal_called(false);

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
void transmit_from_file_worker(uhd::tx_streamer::sptr tx_stream, const std::vector<std::string> &filenames, size_t spb,
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
    std::vector<std::vector<complexf> > buffs(tx_stream->get_num_channels(), std::vector<complexf>(spb));
    std::vector<complexf *> buff_ptrs;
    for (auto &buff: buffs) {
        buff_ptrs.push_back(&buff.front());
    }

    // Open input files
    std::vector<std::shared_ptr<std::ifstream> > infiles;
    std::ranges::transform(filenames, std::back_inserter(infiles), [&](const std::string &f) {
        return std::make_shared<std::ifstream>(f, std::ios::binary);
    });
    size_t num_samps_transmitted = 0;
    bool eof = false;

    // Track buffer state
    size_t buf_valid_samps = 0;   // Number of valid samples in buffer
    size_t buf_sent_samps  = 0;   // Number of samples already sent from buffer

    UHD_LOG_INFO("TX-STREAM", "Starting transmission from " << filenames.size() << " file(s)")

    while (!stop_signal_called) {

        /* ---------- Read from files when buffer is empty ---------- */
        if (buf_sent_samps == buf_valid_samps && !eof) {

            buf_sent_samps  = 0;
            buf_valid_samps = 0;
            eof = false;

            // Read samples from all files simultaneously
            for (size_t ch = 0; ch < tx_stream->get_num_channels(); ++ch) {
                infiles[ch]->read(
                    reinterpret_cast<char*>(buffs[ch].data()),
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

        std::vector<complexf*> offset_ptrs(tx_stream->get_num_channels());
        for (size_t ch = 0; ch < tx_stream->get_num_channels(); ++ch) {
            offset_ptrs[ch] = buffs[ch].data() + buf_sent_samps;
        }

        size_t samps_sent = tx_stream->send(
            offset_ptrs,
            samps_to_send,
            md
        );

        if (samps_sent == 0) {
            UHD_LOG_WARNING("TX-STREAM", "send() returned 0 samples");
            continue;
        }
        num_samps_transmitted += samps_sent;
        buf_sent_samps += samps_sent;
        first_packet = false;

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
void receive_to_file_worker(uhd::rx_streamer::sptr rx_stream, const std::vector<std::string> &filenames, size_t spb,
                            const uhd::time_spec_t &start_time, size_t num_samps_to_recv) {
    // Create output files for each channel
    std::vector<std::shared_ptr<std::ofstream> > outfiles;
    for (auto &file: filenames) {
        outfiles.push_back(std::make_shared<std::ofstream>(file, std::ofstream::binary));

        if (not outfiles.back()->is_open()) {
            UHD_LOG_ERROR("RX-STREAM", "Cannot open receive file: " << file);
            throw std::runtime_error("Cannot open receive file:" + file);
        }
        UHD_LOG_INFO("RX-STREAM", "RX channel saving to file: " << file);
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
