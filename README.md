# UHD Test Project - SDR Application for USRP Devices

## Overview

This project implements a Software Defined Radio (SDR) application that enables simultaneous transmission and reception of samples using USRP hardware through the UHD (USRP Hardware Driver) library. The application reads complex floating-point (fc32) samples from files for transmission and saves received samples to files in synchronized operations with configurable parameters for frequency, gain, sample rate, and channel selection.

## Features

- Simultaneous transmission and reception
- Multi-channel support
- File-based sample input/output (fc32 format)
- Configurable parameters (frequency, gain, sample rate, etc.)
- Real-time LO lock checking
- Clock synchronization with PPS
- Reference clock support (internal, external, GPSDO)
- Integrated worker implementations for file I/O and streaming in main application
- Structured configuration management for USRP parameters
- Graceful shutdown handling with signal processing

## Project Structure

### Core Components

- `txrx_sync.cpp` - Main application implementing simultaneous TX/RX functionality with file I/O and integrated worker functions
- `net.sh` - System network buffer configuration script
- `CMakeLists.txt` - Build configuration

## Dependencies

- UHD (USRP Hardware Driver) 4.9.0 or later
- Boost libraries (program_options, thread, system)
- C++17 compiler or later
- USRP hardware device (e.g., X310, B210)

## Building

```bash
mkdir build
cd build
cmake ..
make
```

Or using the cmake-build-debug directory:

```bash
cd cmake-build-debug
cmake ..
make
```

## Usage

### Basic Command

```bash
./txrx_sync --tx-files tx_data.fc32 --rx-files rx_data.fc32 --freq 915e6 --rate 5e6
```

### Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--help, -h` | Show this help message | N/A |
| `--args` | USRP device address string | `"addr=192.168.180.2"` |
| `--tx-files` | TX data files (fc32 format) | `"tx_data_fc32.bin"` |
| `--rx-files` | RX data files (fc32 format) | `"rx_data_fc32.bin"` |
| `--tx-ants` | TX antenna selections (one per channel) | `"TX/RX"` |
| `--rx-ants` | RX antenna selections (one per channel) | `"RX2"` |
| `--tx-channels` | TX channels (space separated) | `0` |
| `--rx-channels` | RX channels (space separated) | `1` |
| `--spb` | Samples per buffer | `2500` |
| `--rate` | Sample rate (Hz) for both TX and RX | N/A (requires --tx-rates and --rx-rates if not set) |
| `--tx-rates` | TX Sample rates (Hz) (one per channel) | `1e6` |
| `--rx-rates` | RX Sample rates (Hz) (one per channel) | `1e6` |
| `--freq` | Center frequency (Hz) for ALL Tx and Rx CHANNELS. IGNORE --tx-freqs and --rx-freqs settings | N/A |
| `--tx-freqs` | TX Center frequencies (Hz) (one per channel) | `915e6` |
| `--rx-freqs` | RX Center frequencies (Hz) (one per channel) | `915e6` |
| `--tx-gains` | TX gains (dB) (one per channel) | `10.0` |
| `--rx-gains` | RX gains (dB) (one per channel) | `10.0` |
| `--rx-bw` | RX Bandwidth (Hz) | N/A |
| `--tx-bw` | TX Bandwidth (Hz) | N/A |
| `--delay` | Delay before start (seconds) | `1` |
| `--nsamps` | Number of samples to receive, 0 means until TX complete | `0` |
| `--clock-source` | Reference: internal, external, gpsdo | `"internal"` |

### Example Usage

#### Basic TX/RX with Files
```bash
./txrx_sync --tx-files tx_data.fc32 --rx-files rx_data.fc32 --freq 915e6 --rate 5e6
```

#### Multiple Channels
```bash
./txrx_sync --tx-channels 0 1 --rx-channels 0 1 --tx-files tx0.fc32 tx1.fc32 --rx-files rx0.fc32 rx1.fc32 --tx-ants TX/RX TX/RX --rx-ants RX2 RX2
```

#### Custom Parameters
```bash
./txrx_sync --args "addr=192.168.10.2" --tx-freqs 2.4e9 2.5e9 --rx-freqs 2.4e9 2.5e9 --tx-gains 20 25 --rx-gains 15 18 --tx-rates 5e6 5e6 --rx-rates 5e6 5e6 --tx-files tx1.fc32 tx2.fc32 --rx-files rx1.fc32 rx2.fc32
```

## File Formats

The application uses complex floating-point (fc32) format for input and output files. Each sample is two 32-bit floats (real and imaginary components), stored in binary format.

## Network Configuration

For optimal network performance with USRP devices, run the network buffer configuration script before starting:

```bash
sudo ./net.sh
```

This script increases the network buffer sizes to improve data throughput between the host computer and USRP device.

## Workflow

1. Prepare fc32 format input files for transmission
2. Configure network buffers with `net.sh`
3. Run the application with appropriate parameters
4. Monitor LO lock status and system information
5. Process received data files

## Signal Handling

The application handles SIGINT (Ctrl+C) for graceful shutdown. During execution, press Ctrl+C to stop the process safely, which will properly clean up resources and terminate worker threads.

## Troubleshooting

- **Device not found**: Verify USRP address in `--args` parameter and network connectivity
- **Clock reference issues**: Ensure proper cabling for external references if using external/gpsdo options
- **Performance issues**: Run `net.sh` as root to configure optimal network buffer sizes
- **LO unlock errors**: Check antenna connections and verify frequency settings are within device specifications

## Development

The project is structured with:
- Main application logic and worker functions consolidated in `txrx_sync.cpp`
- File I/O operations handled by integrated worker functions
- CMake-based build system

The project follows these key design principles:
- Thread-safe operations for concurrent TX/RX
- Proper resource cleanup and exception handling
- Consistent parameter validation
- Clear organization of USRP configuration parameters using structured configuration
- Support for multi-channel operations

For development, please follow existing code style and maintain thread safety in worker functions.