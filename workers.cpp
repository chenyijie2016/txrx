#include "workers.h"
#include <complex>
#include <fstream>
#include <format>


std::atomic<bool> stop_signal_called(false);

void transmit_from_file_worker(uhd::tx_streamer::sptr tx_stream, const std::vector<std::string> &filenames, size_t spb,
                               const uhd::time_spec_t &start_time) {
    uhd::tx_metadata_t md;
    md.start_of_burst = false;
    md.end_of_burst = false;
    md.has_time_spec = true;
    md.time_spec = start_time;
    double timeout = 5;
    bool first_packet = true;

    std::vector<std::vector<complexf> > buffs(tx_stream->get_num_channels(), std::vector<complexf>(spb));
    std::vector<complexf *> buff_ptrs;
    for (auto &buff: buffs) {
        buff_ptrs.push_back(&buff.front());
    }
    std::vector<std::shared_ptr<std::ifstream> > infiles;
    std::ranges::transform(filenames, std::back_inserter(infiles), [&](const std::string &f) {
        return std::make_shared<std::ifstream>(f, std::ios::binary);
    });
    size_t num_samps_transmitted = 0;
    bool eof = false;

    size_t buf_valid_samps = 0;   // buffer 中有效样点数
    size_t buf_sent_samps  = 0;   // 已经发送的样点数

    while (!stop_signal_called) {

        /* ---------- buffer 不足时才读文件 ---------- */
        if (buf_sent_samps == buf_valid_samps && !eof) {

            buf_sent_samps  = 0;
            buf_valid_samps = 0;
            eof = false;

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
                    buf_valid_samps =
                        std::min(buf_valid_samps, read_samps);
                }
            }

            if (buf_valid_samps == 0) {
                eof = true;
            }
        }

        if (buf_valid_samps == 0 && eof) {
            break;
        }

        /* ---------- 发送剩余样点 ---------- */
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

        /* ---------- 文件数据耗尽，且 buffer 已全部发完 ---------- */
        if (eof && buf_sent_samps == buf_valid_samps) {
            md.end_of_burst = true;
            tx_stream->send("", 0, md);
            break;
        }
    }

    md.end_of_burst = true;
    tx_stream->send("", 0, md);

    // close all
    std::ranges::for_each(infiles, [](auto f) { f->close(); });
    UHD_LOG_INFO("TX-STREAM", "Transmit DONE! samps:" << num_samps_transmitted);
}


void receive_to_file_worker(uhd::rx_streamer::sptr rx_stream, const std::vector<std::string> &filenames, size_t spb,
                            const uhd::time_spec_t &start_time, size_t num_samps_to_recv) {
    // create ofstream

    std::vector<std::shared_ptr<std::ofstream> > outfiles;
    for (auto &file: filenames) {
        outfiles.push_back(std::make_shared<std::ofstream>(file, std::ofstream::binary));

        if (not outfiles.back()->is_open()) {
            throw std::runtime_error("无法打开接收文件:" + file);
        }
        UHD_LOG_INFO("RX-STREAM", "RX channel save to file:" << file);
    }

    // create buffer
    std::vector<std::vector<complexf> > buffs(rx_stream->get_num_channels(), std::vector<complexf>(spb));
    std::vector<complexf *> buff_ptrs;
    for (auto &buff: buffs) {
        buff_ptrs.push_back(&buff.front());
    }

    bool first_packet = true;
    double timeout = 5;
    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    stream_cmd.num_samps = num_samps_to_recv;
    stream_cmd.stream_now = false;
    stream_cmd.time_spec = start_time;

    UHD_LOG_DEBUG("RX-STREAM", "接收开始时间: " << start_time.get_real_secs() << " 秒")

    rx_stream->issue_stream_cmd(stream_cmd);

    uhd::rx_metadata_t md;
    size_t num_samps_received = 0;

    while (not stop_signal_called && (num_samps_received < num_samps_to_recv)) {
        size_t num_rx_samps = rx_stream->recv(buff_ptrs, spb, md, timeout);
        if (first_packet) {
            timeout = 0.1;
            first_packet = false;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            UHD_LOG_WARNING("RX-STREAM", "RX channel received timeout.");
            continue;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            UHD_LOG_WARNING("RX-STREAM", "RX channel received overflow.");
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            UHD_LOG_ERROR("RX-STREAM", "RX channel received error:" << md.strerror());
            throw std::runtime_error("接收错误: " + md.strerror());
        }

        for (auto i = 0; i < outfiles.size(); ++i) {
            outfiles[i]->write(reinterpret_cast<char *>(buffs[i].data()), num_rx_samps * sizeof(complexf));
        }
        num_samps_received += num_rx_samps;
    }
    // stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    // rx_stream->issue_stream_cmd(stream_cmd);
    for (auto &outfile: outfiles) {
        outfile->close();
    }

    UHD_LOG_INFO("RX-STREAM", "Received DONE! number of samples (" << num_samps_received << ")");
}
