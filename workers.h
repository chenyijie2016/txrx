#ifndef WORKERS_H
#define WORKERS_H

#include <uhd/usrp/multi_usrp.hpp>
#include <string>
#include <complex>
#include <atomic>

/**
 * Global flag for signal handling
 * Used to signal all worker threads to stop gracefully when SIGINT is received
 */
extern std::atomic<bool> stop_signal_called;

/**
 * Complex floating-point type alias for samples (fc32 format)
 */
using complexf = std::complex<float>;

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
void transmit_from_file_worker(uhd::tx_streamer::sptr tx_stream, const std::vector<std::string> &filenames, size_t spb, const uhd::time_spec_t &start_time);

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
 * @param samps_to_recv Total number of samples to receive before stopping
 */
void receive_to_file_worker(uhd::rx_streamer::sptr rx_stream, const std::vector<std::string> &filenames, size_t spb, const uhd::time_spec_t &start_time, size_t samps_to_recv);

#endif