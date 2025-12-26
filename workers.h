#ifndef WORKERS_H
#define WORKERS_H

#include <uhd/usrp/multi_usrp.hpp>
#include <string>

extern  std::atomic<bool> stop_signal_called;
using complexf = std::complex<float>;

void transmit_from_file_worker(uhd::tx_streamer::sptr tx_stream, const std::vector<std::string> &filenames, size_t spb, const uhd::time_spec_t &start_time);

void receive_to_file_worker(uhd::rx_streamer::sptr rx_stream, const std::vector<std::string> &filenames, size_t spb, const uhd::time_spec_t &start_time, size_t samps_to_recv);



#endif