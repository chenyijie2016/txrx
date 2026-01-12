#ifndef USRP_TRANSCEIVER_H
#define USRP_TRANSCEIVER_H

#include "utils.h"

class UsrpTransceiver {
private:
    uhd::usrp::multi_usrp::sptr usrp;
    UsrpConfig config;

public:
    explicit UsrpTransceiver(UsrpConfig cfg);

    ~UsrpTransceiver();

    /**
     * Validates the USRP configuration
     * @return true if configuration is valid, false otherwise
     */
    bool ValidateConfiguration();

    /**
     * Applies the USRP configuration to the device
     */
    void ApplyConfiguration();

    /**
     * Transmits samples from a buffer to USRP using a streaming approach
     *
     * @param tx_stream TX streamer to send samples to the USRP
     * @param buffs Buffer containing complex samples organized by channel
     * @param spb Samples per buffer - number of samples to process in each iteration
     * @param start_time Time specification for when transmission should begin
     */
    void TransmitFromBuffer(const uhd::tx_streamer::sptr &tx_stream,
                            std::vector<std::vector<complexf> > &buffs,
                            size_t spb,
                            const uhd::time_spec_t &start_time);

    /**
     * Receives samples from USRP to a buffer using a streaming approach
     *
     * @param rx_stream RX streamer to receive samples from the USRP
     * @param spb Samples per buffer - number of samples to process in each iteration
     * @param start_time Time specification for when reception should begin
     * @param num_samps_to_recv Total number of samples to receive before stopping
     * @return A vector of vectors containing the received complex samples, one per channel
     */
    std::vector<std::vector<complexf> > ReceiveToBuffer(const uhd::rx_streamer::sptr &rx_stream,
                                                        size_t spb,
                                                        const uhd::time_spec_t &start_time,
                                                        size_t num_samps_to_recv);

    // Getter methods
    uhd::usrp::multi_usrp::sptr &get_usrp() { return usrp; }
    const UsrpConfig &get_config() const { return config; }
};

#endif // USRP_TRANSCEIVER_H
