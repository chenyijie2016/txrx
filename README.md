# UHD Test Project - TX/RX Samples From/To File

## Overview

This project provides a Software Defined Radio (SDR) application that simultaneously transmits and receives samples using USRP hardware through the UHD (USRP Hardware Driver) library. The application reads complex floating-point (fc32) samples from files for transmission and saves received samples to files.

## Features

- Simultaneous transmission and reception
- Multi-channel support
- File-based sample input/output (fc32 format)
- Configurable parameters (frequency, gain, sample rate, etc.)
- Real-time LO lock checking
- Clock synchronization with PPS
- Reference clock support (internal, external, GPSDO)

## Components

### Core Files

- `txrx_samples_from_to_file.cpp` - Main application implementing the TX/RX functionality
- `workers.cpp` / `workers.h` - Worker threads for file I/O and streaming
- `show_receive.py` - GNU Radio Companion flowgraph for visualizing received samples
- `net.sh` - System network buffer configuration script
- `CMakeLists.txt` - Build configuration

### Key Functions

- **transmit_from_file_worker**: Handles transmission from file to USRP
- **receive_to_file_worker**: Handles reception from USRP to file
- **check_locked_sensor**: Monitors LO lock status
- **Signal handlers**: Handle graceful shutdown via Ctrl+C

## Dependencies

- UHD (USRP Hardware Driver) 4.9.0 or later
- Boost libraries (program_options, filesystem)
- C++20 compiler
- USRP hardware device

## Building

```bash
mkdir build
cd build
cmake ..
make
```

## Usage

### Basic Command

```bash
./txrx_samples_from_to_file --tx-files tx_data_fc32.bin --rx-files rx_data_fc32.bin --freq 915e6 --rate 5e6
```

### Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--help, -h` | Display help information | N/A |
| `--args` | USRP device arguments | `addr=192.168.180.2` |
| `--tx-files` | Transmit data files (fc32 format) | `tx_data_fc32.bin` |
| `--rx-files` | Receive data files (fc32 format) | `rx_data_fc32.bin` |
| `--tx-ant` | Transmit antenna selection | `TX/RX` |
| `--rx-ant` | Receive antenna selection | `RX2` |
| `--tx-channels` | Transmit channels (space separated) | `0` |
| `--rx-channels` | Receive channels (space separated) | `1` |
| `--spb` | Sample buffer size per packet | `2500` |
| `--rate` | Sample rate (Hz) | `5e6` |
| `--freq` | Center frequency (Hz) | `915e6` |
| `--tx-gain` | Transmit gain (dB) | `10.0` |
| `--rx-gain` | Receive gain (dB) | `10.0` |
| `--bw` | Bandwidth (Hz) | N/A |
| `--delay` | Start delay before transmission (seconds) | `1` |
| `--nsamps` | Number of samples to receive (0 = until TX complete) | `0` |
| `--ref` | Clock reference (internal/external/gpsdo) | `internal` |

### Example Usage

#### Basic TX/RX
```bash
./txrx_samples_from_to_file --tx-files input.fc32 --rx-files output.fc32
```

#### Multiple Channels
```bash
./txrx_samples_from_to_file --tx-channels 0 1 --rx-channels 0 1 --tx-files tx0.fc32 tx1.fc32 --rx-files rx0.fc32 rx1.fc32
```

#### Custom Parameters
```bash
./txrx_samples_from_to_file --freq 2.4e9 --rate 10e6 --tx-gain 20 --rx-gain 15 --tx-files custom_input.fc32 --rx-files custom_output.fc32
```

## File Formats

The application uses complex floating-point (fc32) format for input and output files. Each sample is two 32-bit floats (real and imaginary components), stored in binary format.

## Network Configuration

For optimal performance, run the network buffer configuration script before starting:

```bash
sudo ./net.sh
```

This increases the network buffer sizes to 32MB for both transmit and receive.

## Visualization

The `show_receive.py` script provides a GNU Radio flowgraph to visualize the received samples. You can view the received data using:

```bash
python3 show_receive.py
```

## Workflow

1. Prepare fc32 format input files
2. Configure network buffers with `net.sh`
3. Run the application with appropriate parameters
4. Monitor LO lock status and system information
5. Process received data files

## Signal Handling

The application handles SIGINT (Ctrl+C) for graceful shutdown. During execution, press Ctrl+C to stop the process safely.

## Troubleshooting

- **LO unlock errors**: Check antenna connections and frequency settings
- **Network buffer issues**: Run `net.sh` as root to increase buffer sizes
- **Device not found**: Verify USRP address in `--args` parameter
- **File format errors**: Ensure input files are in fc32 binary format

## Development

The project is structured with:
- Main application logic in `txrx_samples_from_to_file.cpp`
- Threading and streaming workers in `workers.cpp`
- CMake-based build system

For development, please follow existing code style and maintain thread safety in the worker functions.