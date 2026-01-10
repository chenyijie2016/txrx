# Qwen Code Configuration for UHD Test Project

## Project Overview
- **Project Name**: UHD Test - SDR Application for USRP Devices
- **Language**: C++ (with shell scripting)
- **Domain**: Software Defined Radio (SDR) with UHD (USRP Hardware Driver)
- **IDE**: CLion

## Project Context
This project implements a Software Defined Radio (SDR) application that enables synchronized transmission and reception of samples using USRP hardware through the UHD (USRP Hardware Driver) library. The application focuses on simultaneous TX/RX operations with configurable parameters for frequency, gain, sample rate, and channel selection.

## Key Files
- `txrx_sync.cpp` - Main application implementing synchronized TX/RX functionality
- `workers.cpp` / `workers.h` - Worker threads for file I/O and streaming operations
- `net.sh` - System network buffer configuration script for optimizing USRP communication
- `CMakeLists.txt` - Build configuration for the project
- `README.md` - Project documentation

## Development Preferences
- Follow existing code style in C++ files with consistent formatting
- Maintain compatibility with UHD API and SDR conventions
- Keep CMake build system in sync with source files
- Ensure thread safety in worker implementations
- Preserve signal handling for graceful shutdown

## Common Commands
- Build: `cmake --build .` or `make` in build directory
- Clean build: `rm -rf build && mkdir build && cd build && cmake .. && make`
- Run: `./txrx_sync [options]` in build directory
- Check C++ style: Follow existing code formatting patterns

## Notes for AI Assistant
- When modifying C++ code, maintain compatibility with UHD API
- Be mindful of SDR-specific concepts when making changes (frequencies, gains, sample rates, etc.)
- Follow existing naming conventions and code organization
- Pay attention to thread safety in worker functions
- Preserve proper resource cleanup and exception handling
- Maintain consistency with command-line argument parsing
- Consider network buffer configurations when making changes to streaming parameters