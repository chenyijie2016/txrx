# Qwen Code Configuration for UHD Test Project

## Project Overview
- **Project Name**: UHD Test
- **Language**: C++ (with Python components)
- **Domain**: Software Defined Radio (SDR) testing with UHD (USRP Hardware Driver)
- **IDE**: CLion

## Project Context
This project appears to be for testing UHD (USRP Hardware Driver) functionality, with C++ files for workers and signal processing, as well as GNU Radio Companion files (`.grc`) and Python scripts.

## Key Files
- `workers.cpp` / `workers.h` - Core C++ worker implementation
- `txrx_samples_from_to_file.cpp` - File-based TX/RX sample processing
- `show_receive.grc` and `show_receive.py` - GNU Radio Companion flowgraph for signal reception
- `net.sh` - Network-related shell script
- `CMakeLists.txt` - Build configuration

## Development Preferences
- Follow existing code style in C++ files
- Maintain consistency with UHD/SDR conventions
- Keep CMake build system in sync

## Common Commands
- Build: `cmake --build`
- Run tests: (if any exist)
- Check C++ style: (project-specific style to follow existing code)

## Notes for AI Assistant
- When modifying C++ code, maintain compatibility with UHD API
- Be mindful of SDR-specific concepts when making changes
- Follow existing naming conventions and code organization