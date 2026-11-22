/*
 * TT MIDI Bridge: Standalone Eurorack MIDI System
 * 
 * This application creates a unified hardware interface for standalone music-making
 * on a Raspberry Pi running ChooChooTracker or PureData.
 * 
 * Hardware Setup:
 * - Sweet16 MKII Faderbank (I2C) with modified 16n firmware
 *   → 16 analog faders for real-time control
 *   → Hardware MIDI output to Eurorack CV modules
 * 
 * - Terminal Tedium Pots (SPI via MCP3208 ADC)
 *   → 6 analog potentiometers for auxiliary control
 * 
 * - Raspberry Pi (Controller)
 *   → Runs ChooChooTracker (8-bit retro sequencer)
 *   → Or runs PureData (custom synth/effect patches)
 *   → Bridges hardware controls to software via MIDI
 * 
 * This bridge:
 * 1. Reads faders from Sweet16 MKII via I2C
 * 2. Reads pots from Terminal Tedium via SPI
 * 3. Sends MIDI CC data to ChooChooTracker/PureData
 * 4. Receives MIDI commands from software
 * 5. Outputs hardware MIDI via Sweet16 to Eurorack modules
 * 
 * Usage: ./midi_bridge <14bit|7bit> [trackwerk|choochoo|puredata]
 *   14bit: High precision for PureData patches
 *   7bit:  Standard MIDI for ChooChooTracker
 *   Mode:  Software mode indicator (choochoo=default)
 * 
 * Usage: midi_bridge <14bit|7bit> [trackwerk|choochoo|puredata]
 *   14bit/7bit: Output resolution for faders and pots
 *   trackwerk/choochoo/puredata: Initial MacroPad mode
 */

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <iostream>
#include <vector>
#include <string>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>
#include <alsa/asoundlib.h>
#include <gpiod.h>

#include "macropad.hpp"

// ============================================================================
// Debug Output
// ============================================================================

bool g_debug = false;

#define DEBUG_PRINT(...) \
    do { if (g_debug) { std::fprintf(stderr, "[DEBUG] "); std::fprintf(stderr, __VA_ARGS__); } } while(0)

// ============================================================================
// GPIO Global Objects (libgpiod C API)
// ============================================================================

// GPIO chip and line objects (C API)
static struct gpiod_chip* g_gpio_chip = nullptr;
static struct gpiod_line* g_button_lines[NUM_TT_BUTTONS] = {nullptr, nullptr, nullptr};
static struct gpiod_line* g_gate_in_lines[NUM_TT_GATES_IN] = {nullptr, nullptr, nullptr, nullptr};
static struct gpiod_line* g_gate_out_lines[NUM_TT_GATES_OUT] = {nullptr, nullptr};

// ============================================================================
// Hardware Configuration
// ============================================================================

// Sweet Sixteen Fader Controller (I2C)
constexpr const char* I2C_DEVICE = "/dev/i2c-1";
constexpr int SWEET16_ADDR = 0x34;
constexpr int NUM_FADERS = 16;
constexpr int FADER_MAX_VALUE = 16383;  // 14-bit max
constexpr int FADER_DEADZONE = 2;       // Threshold for change detection

// Terminal Tedium Pots (SPI via MCP3208 ADC)
constexpr const char* SPI_DEVICE = "/dev/spidev0.1";
constexpr uint32_t SPI_SPEED_HZ = 1000000;
constexpr int NUM_TT_POTS = 6;
constexpr uint8_t TT_CC_OFFSET = 16;    // CC 16-21 for pots
constexpr int POT_ADC_MAX = 4095;       // 12-bit ADC
constexpr int POT_DEADZONE = 8;         // Threshold for change detection

// Terminal Tedium Buttons (GPIO inputs)
constexpr int NUM_TT_BUTTONS = 3;       // Button 1, Button 2, and Button 3
constexpr int TT_BUTTON1_GPIO = 23;     // GPIO pin for Button 1 (top)
constexpr int TT_BUTTON2_GPIO = 24;     // GPIO pin for Button 2 (middle)
constexpr int TT_BUTTON3_GPIO = 25;     // GPIO pin for Button 3 (bottom)

// Terminal Tedium Gate Inputs (clock/gate inputs - GPIO)
constexpr int NUM_TT_GATES_IN = 4;
constexpr int TT_GATE_IN1_GPIO = 4;     // GPIO pin for Gate Input 1
constexpr int TT_GATE_IN2_GPIO = 14;    // GPIO pin for Gate Input 2
constexpr int TT_GATE_IN3_GPIO = 17;    // GPIO pin for Gate Input 3
constexpr int TT_GATE_IN4_GPIO = 27;    // GPIO pin for Gate Input 4

// Terminal Tedium Gate Outputs (GPIO - for LED/gate output)
constexpr int NUM_TT_GATES_OUT = 2;
constexpr int TT_GATE_OUT1_GPIO = 16;   // GPIO pin for Gate Output 1
constexpr int TT_GATE_OUT2_GPIO = 26;   // GPIO pin for Gate Output 2

// Adafruit MacroPad (Serial USB)
constexpr const char* MACROPAD_SERIAL = "/dev/serial/by-id/usb-Adafruit_MacroPad_RP2040_DF643CF013616025-if00";

// MIDI Configuration
constexpr uint8_t MIDI_CHANNEL = 0;
constexpr uint16_t POLL_INTERVAL_US = 5000;

// ============================================================================
// I2C Fader Reading (Sweet Sixteen)
// ============================================================================

/**
 * read_fader_i2c: Read a single fader value from Sweet Sixteen via I2C
 * 
 * Sweet Sixteen protocol: write fader index, read back 2 bytes (MSB first)
 * Values are 14-bit (0-16383), clamped to 14-bit range.
 * 
 * @param fd: I2C file descriptor
 * @param fader_index: Which fader to read (0-15)
 * @param out_val: Output parameter for the 14-bit fader value
 * @return: 0 on success, -1 on I2C error
 */
int read_fader_i2c(int fd, uint8_t fader_index, uint16_t& out_val) {
    if (write(fd, &fader_index, 1) != 1) return -1;
    uint8_t buf[2] = {0, 0};
    if (read(fd, buf, 2) != 2) return -1;
    out_val = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    if (out_val > FADER_MAX_VALUE) out_val = FADER_MAX_VALUE;
    return 0;
}

// ============================================================================
// SPI ADC Reading (Terminal Tedium via MCP3208)
// ============================================================================

/**
 * read_mcp3208_spi: Read a single channel from MCP3208 12-bit ADC via SPI
 * 
 * MCP3208 protocol (3-byte transaction):
 *  - Byte 0: 0x06 | (channel >> 2)       [start + config]
 *  - Byte 1: (channel & 0x03) << 6       [channel selection]
 *  - Byte 2: 0x00                        [dummy clock cycles]
 * 
 * Response: 12-bit value across bytes 1-2 (bits 11-0 of result)
 * 
 * @param spi_fd: SPI file descriptor
 * @param channel: ADC channel to read (0-7)
 * @param out_val: Output parameter for 12-bit ADC value (0-4095)
 * @return: 0 on success, -1 on SPI error
 */
int read_mcp3208_spi(int spi_fd, uint8_t channel, uint16_t& out_val) {
    if (spi_fd < 0 || channel > 7) return -1;
    uint8_t tx[3] = {
        static_cast<uint8_t>(0x06 | ((channel & 0x04) >> 2)),
        static_cast<uint8_t>((channel & 0x03) << 6),
        0x00
    };
    uint8_t rx[3] = {0, 0, 0};

    struct spi_ioc_transfer tr;
    std::memset(&tr, 0, sizeof(tr));
    tr.tx_buf = reinterpret_cast<unsigned long>(tx);
    tr.rx_buf = reinterpret_cast<unsigned long>(rx);
    tr.len = 3;
    tr.speed_hz = SPI_SPEED_HZ;
    tr.bits_per_word = 8;

    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) return -1;
    out_val = ((static_cast<uint16_t>(rx[1]) & 0x0F) << 8) | rx[2];
    return 0;
}

/**
 * init_gpio_libgpiod: Initialize all GPIO pins using libgpiod
 * 
 * This initializes all Terminal Tedium GPIO pins for inputs and outputs
 * using the modern libgpiod character device interface.
 * Returns 0 on success, -1 on error.
 */
int init_gpio_libgpiod() {
    try {
        // Open the GPIO chip (gpiochip0 on Raspberry Pi)
        g_gpio_chip = new chip("gpiochip0");
        if (!g_gpio_chip) {
            std::cerr << "Failed to open GPIO chip" << std::endl;
            return -1;
        }
        
        DEBUG_PRINT("Opened GPIO chip: gpiochip0\n");
        
        // Configure button lines (GPIO 23, 24, 25) as inputs
        line_settings button_settings;
        button_settings.set_direction(line::direction::INPUT);
        
        request_builder button_builder(*g_gpio_chip);
        button_builder.set_consumer("tt_buttons");
        button_builder.add_line_settings(TT_BUTTON1_GPIO, button_settings);
        button_builder.add_line_settings(TT_BUTTON2_GPIO, button_settings);
        button_builder.add_line_settings(TT_BUTTON3_GPIO, button_settings);
        
        g_button_lines = new line_request(button_builder);
        if (!g_button_lines) {
            std::cerr << "Failed to request button lines" << std::endl;
            return -1;
        }
        
        std::cout << "[TT] Button lines initialized (GPIO " << TT_BUTTON1_GPIO << ", "
                  << TT_BUTTON2_GPIO << ", " << TT_BUTTON3_GPIO << ")" << std::endl;
        
        // Configure gate input lines (GPIO 4, 14, 17, 27) as inputs
        line_settings gate_in_settings;
        gate_in_settings.set_direction(line::direction::INPUT);
        
        request_builder gate_in_builder(*g_gpio_chip);
        gate_in_builder.set_consumer("tt_gate_in");
        gate_in_builder.add_line_settings(TT_GATE_IN1_GPIO, gate_in_settings);
        gate_in_builder.add_line_settings(TT_GATE_IN2_GPIO, gate_in_settings);
        gate_in_builder.add_line_settings(TT_GATE_IN3_GPIO, gate_in_settings);
        gate_in_builder.add_line_settings(TT_GATE_IN4_GPIO, gate_in_settings);
        
        g_gate_in_lines = new line_request(gate_in_builder);
        if (!g_gate_in_lines) {
            std::cerr << "Failed to request gate input lines" << std::endl;
            return -1;
        }
        
        std::cout << "[TT] Gate input lines initialized (GPIO " << TT_GATE_IN1_GPIO << ", "
                  << TT_GATE_IN2_GPIO << ", " << TT_GATE_IN3_GPIO << ", " << TT_GATE_IN4_GPIO << ")" << std::endl;
        
        // Configure gate output lines (GPIO 16, 26) as outputs
        line_settings gate_out_settings;
        gate_out_settings.set_direction(line::direction::OUTPUT);
        gate_out_settings.set_output_value(line::value::INACTIVE);
        
        request_builder gate_out_builder(*g_gpio_chip);
        gate_out_builder.set_consumer("tt_gate_out");
        gate_out_builder.add_line_settings(TT_GATE_OUT1_GPIO, gate_out_settings);
        gate_out_builder.add_line_settings(TT_GATE_OUT2_GPIO, gate_out_settings);
        
        g_gate_out_lines = new line_request(gate_out_builder);
        if (!g_gate_out_lines) {
            std::cerr << "Failed to request gate output lines" << std::endl;
            return -1;
        }
        
        std::cout << "[TT] Gate output lines initialized (GPIO " << TT_GATE_OUT1_GPIO << ", "
                  << TT_GATE_OUT2_GPIO << ")" << std::endl;
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "GPIO initialization error: " << e.what() << std::endl;
        return -1;
    }
}

/**
 * read_button_libgpiod: Read a button state using libgpiod
 * 
 * @param button_num: Button index (0, 1, or 2)
 * @return: 1 if pressed, 0 if not pressed, -1 on error
 */
int read_button_libgpiod(int button_num) {
    if (!g_button_lines || button_num < 0 || button_num >= NUM_TT_BUTTONS) return -1;
    
    try {
        // Note: line indexing is 0-2 for the 3 button lines
        line::value val = g_button_lines->get_value(button_num);
        return (val == line::value::ACTIVE) ? 1 : 0;
    } catch (const std::exception& e) {
        DEBUG_PRINT("Button %d read error: %s\n", button_num, e.what());
        return -1;
    }
}

/**
 * read_gate_input_libgpiod: Read a gate input state using libgpiod
 * 
 * @param gate_num: Gate input index (0, 1, 2, or 3)
 * @return: 1 if high, 0 if low, -1 on error
 */
int read_gate_input_libgpiod(int gate_num) {
    if (!g_gate_in_lines || gate_num < 0 || gate_num >= NUM_TT_GATES_IN) return -1;
    
    try {
        line::value val = g_gate_in_lines->get_value(gate_num);
        return (val == line::value::ACTIVE) ? 1 : 0;
    } catch (const std::exception& e) {
        DEBUG_PRINT("Gate input %d read error: %s\n", gate_num, e.what());
        return -1;
    }
}

/**
 * set_gate_output_libgpiod: Set a gate output state using libgpiod
 * 
 * @param gate_num: Gate output index (0 or 1)
 * @param value: 1 for high/active, 0 for low/inactive
 * @return: 0 on success, -1 on error
 */
int set_gate_output_libgpiod(int gate_num, int value) {
    if (!g_gate_out_lines || gate_num < 0 || gate_num >= NUM_TT_GATES_OUT) return -1;
    
    try {
        line::value val = (value != 0) ? line::value::ACTIVE : line::value::INACTIVE;
        g_gate_out_lines->set_value(gate_num, val);
        return 0;
    } catch (const std::exception& e) {
        DEBUG_PRINT("Gate output %d write error: %s\n", gate_num, e.what());
        return -1;
    }
}

/**
 * cleanup_gpio_libgpiod: Clean up GPIO resources
 */
void cleanup_gpio_libgpiod() {
    if (g_button_lines) {
        delete g_button_lines;
        g_button_lines = nullptr;
    }
    if (g_gate_in_lines) {
        delete g_gate_in_lines;
        g_gate_in_lines = nullptr;
    }
    if (g_gate_out_lines) {
        delete g_gate_out_lines;
        g_gate_out_lines = nullptr;
    }
    if (g_gpio_chip) {
        delete g_gpio_chip;
        g_gpio_chip = nullptr;
    }
}
// MIDI Output (I2C + ALSA)
// ============================================================================

/**
 * write_midi_i2c: Send MIDI command to Sweet Sixteen via I2C
 * 
 * Sweet Sixteen I2C packet format:
 *  - Byte 0: Command (0x01=NoteOn, 0x02=NoteOff, 0x03=CC, 0x04=PitchBend)
 *  - Byte 1: Channel (1-indexed, 1-16)
 *  - Byte 2: Data1 (note/CC, 7-bit)
 *  - Byte 3: Data2 (velocity/value, 7-bit)
 * 
 * @param fd: I2C file descriptor
 * @param cmd: MIDI command type
 * @param ch_1_indexed: MIDI channel (1-16)
 * @param data1: Note number or CC number
 * @param data2: Velocity or CC value
 * @return: 0 on success, -1 on I2C error
 */
int write_midi_i2c(int fd, uint8_t cmd, uint8_t ch_1_indexed, uint8_t data1, uint8_t data2) {
    uint8_t pkt[4] = {
        cmd,
        ch_1_indexed,
        static_cast<uint8_t>(data1 & 0x7F),
        static_cast<uint8_t>(data2 & 0x7F)
    };
    return (write(fd, pkt, 4) == 4) ? 0 : -1;
}

/**
 * send_alsa_cc: Broadcast a MIDI Control Change (CC) event via ALSA
 * 
 * @param seq: ALSA sequencer handle
 * @param port: ALSA output port
 * @param channel: MIDI channel (0-15)
 * @param cc: CC number (0-127)
 * @param value: CC value (0-127)
 */
void send_alsa_cc(snd_seq_t* seq, int port, uint8_t channel, uint8_t cc, uint8_t value) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_controller(&ev, channel, cc, value);
    snd_seq_event_output_direct(seq, &ev);
}

// ============================================================================
// ALSA Input Processing
// ============================================================================

/**
 * process_alsa_input: Process incoming MIDI events from ALSA sequencer
 * 
 * Supported events:
 *  - NoteOn/NoteOff: Written to Sweet Sixteen
 *  - CC: Written to Sweet Sixteen (and can control other devices)
 *  - PitchBend: Written to Sweet Sixteen as 14-bit value
 *  - Program Change: Sets MacroPad mode (0=TrackWerk, 1=ChooChoo, 2=PureData)
 * 
 * @param seq: ALSA sequencer handle
 * @param i2c_fd: I2C file descriptor for Sweet Sixteen
 * @param macropad: MacroPad bridge for mode changes
 */
void process_alsa_input(snd_seq_t* seq, int i2c_fd, MacroPadBridge& macropad) {
    snd_seq_event_t* ev = nullptr;
    while (snd_seq_event_input_pending(seq, 1) > 0) {
        if (snd_seq_event_input(seq, &ev) < 0 || !ev) break;
        uint8_t ch = (ev->data.note.channel & 0x0F) + 1;

        switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                // NoteOn with velocity 0 is treated as NoteOff
                if (ev->data.note.velocity > 0)
                    write_midi_i2c(i2c_fd, 0x01, ch, ev->data.note.note, ev->data.note.velocity);
                else
                    write_midi_i2c(i2c_fd, 0x02, ch, ev->data.note.note, 0);
                break;

            case SND_SEQ_EVENT_NOTEOFF:
                write_midi_i2c(i2c_fd, 0x02, ch, ev->data.note.note, ev->data.note.velocity);
                break;

            case SND_SEQ_EVENT_CONTROLLER: {
                uint8_t cc_ch = (ev->data.control.channel & 0x0F) + 1;
                write_midi_i2c(i2c_fd, 0x03, cc_ch, ev->data.control.param, ev->data.control.value);
                break;
            }

            case SND_SEQ_EVENT_PITCHBEND: {
                uint8_t pb_ch = (ev->data.control.channel & 0x0F) + 1;
                // PitchBend is signed -8192 to +8192; convert to unsigned 14-bit value
                uint16_t unsigned_bend = static_cast<uint16_t>(ev->data.control.value + 8192);
                write_midi_i2c(i2c_fd, 0x04, pb_ch, unsigned_bend & 0x7F, (unsigned_bend >> 7) & 0x7F);
                break;
            }

            case SND_SEQ_EVENT_PGMCHANGE: {
                // Program Change selects MacroPad mode
                // 0 = TrackWerk, 1 = ChooChoo, 2 = PureData
                int pgm = ev->data.control.value;
                if (pgm == 0) macropad.set_mode(MacroMode::TRACKWERK);
                else if (pgm == 1) macropad.set_mode(MacroMode::CHOOCHOO);
                else if (pgm == 2) macropad.set_mode(MacroMode::PUREDATA);
                break;
            }

            default:
                break;
        }
    }
}

// ============================================================================
// Hardware Initialization
// ============================================================================

/**
 * init_i2c: Initialize I2C connection to Sweet Sixteen
 * 
 * @return: File descriptor on success, negative value on error
 */
int init_i2c() {
    int fd = open(I2C_DEVICE, O_RDWR);
    if (fd < 0) {
        std::cerr << "Failed to open " << I2C_DEVICE << std::endl;
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, SWEET16_ADDR) < 0) {
        std::cerr << "Failed to set I2C slave address 0x" << std::hex << SWEET16_ADDR << std::endl;
        close(fd);
        return -1;
    }
    return fd;
}

/**
 * init_spi: Initialize SPI connection for MCP3208 ADC
 * 
 * @return: File descriptor on success, negative value on error (optional device)
 */
int init_spi() {
    int fd = open(SPI_DEVICE, O_RDWR);
    if (fd < 0) {
        std::cerr << "Warning: SPI device " << SPI_DEVICE << " not available (Terminal Tedium pots disabled)" << std::endl;
        return -1;
    }

    uint8_t spi_mode = SPI_MODE_0, spi_bits = 8;
    uint32_t spi_speed = SPI_SPEED_HZ;
    
    if (ioctl(fd, SPI_IOC_WR_MODE, &spi_mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &spi_bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed) < 0) {
        std::cerr << "Warning: Failed to configure SPI (Terminal Tedium pots disabled)" << std::endl;
        close(fd);
        return -1;
    }
    return fd;
}

/**
 * init_alsa: Initialize ALSA sequencer and ports
 * 
 * @param seq_handle: Output parameter for ALSA sequencer handle
 * @param out_port: Output parameter for output port ID
 * @param in_port: Output parameter for input port ID
 * @return: 0 on success, negative value on error
 */
int init_alsa(snd_seq_t** seq_handle, int& out_port, int& in_port) {
    if (snd_seq_open(seq_handle, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        std::cerr << "Error opening ALSA sequencer" << std::endl;
        return -1;
    }
    
    snd_seq_nonblock(*seq_handle, 1);
    snd_seq_set_client_name(*seq_handle, "MIDI Bridge");

    out_port = snd_seq_create_simple_port(*seq_handle, "Hardware Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    in_port = snd_seq_create_simple_port(*seq_handle, "Hardware In",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    if (out_port < 0 || in_port < 0) {
        std::cerr << "Error creating ALSA ports" << std::endl;
        snd_seq_close(*seq_handle);
        return -1;
    }
    return 0;
}

// ============================================================================
// Main Program
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <14bit|7bit> [trackwerk|choochoo|puredata] [--debug]" << std::endl;
        return 1;
    }

    // Parse command-line arguments
    std::string bit_mode = argv[1];
    bool is_14bit = (bit_mode == "14bit");
    if (bit_mode != "14bit" && bit_mode != "7bit") {
        std::cerr << "Invalid bit mode '" << bit_mode << "'. Use '14bit' or '7bit'." << std::endl;
        return 1;
    }

    MacroMode macro_mode = MacroMode::CHOOCHOO;
    int arg_idx = 2;
    
    if (arg_idx < argc) {
        std::string m = argv[arg_idx];
        if (m == "trackwerk") macro_mode = MacroMode::TRACKWERK;
        else if (m == "choochoo") macro_mode = MacroMode::CHOOCHOO;
        else if (m == "puredata") macro_mode = MacroMode::PUREDATA;
        else if (m == "--debug") {
            g_debug = true;
            arg_idx++;
        } else {
            std::cerr << "Invalid MacroPad mode '" << m << "'." << std::endl;
            return 1;
        }
        arg_idx++;
    }
    
    if (arg_idx < argc && std::string(argv[arg_idx]) == "--debug") {
        g_debug = true;
    }

    // Initialize hardware
    int i2c_fd = init_i2c();
    if (i2c_fd < 0) return 1;

    int spi_fd = init_spi();

    snd_seq_t* seq_handle = nullptr;
    int out_port = -1, in_port = -1;
    if (init_alsa(&seq_handle, out_port, in_port) < 0) {
        if (spi_fd >= 0) close(spi_fd);
        close(i2c_fd);
        return 1;
    }

    MacroPadBridge macropad(MACROPAD_SERIAL, macro_mode);

    std::cout << "MIDI Bridge active [" << bit_mode << " mode | MacroPad: " 
              << macropad.mode_name() << "]" << std::endl;

    // Initialize Terminal Tedium GPIO using libgpiod
    if (init_gpio_libgpiod() < 0) {
        std::cerr << "Failed to initialize Terminal Tedium GPIO. Exiting." << std::endl;
        if (spi_fd >= 0) close(spi_fd);
        close(i2c_fd);
        snd_seq_close(seq_handle);
        return 1;
    }
    
    std::cout << "[TT] Terminal Tedium GPIO initialized (libgpiod):" << std::endl;
    std::cout << "  - Buttons: GPIO " << TT_BUTTON1_GPIO << ", " << TT_BUTTON2_GPIO << ", " << TT_BUTTON3_GPIO << std::endl;
    std::cout << "  - Gate Inputs: GPIO " << TT_GATE_IN1_GPIO << ", " << TT_GATE_IN2_GPIO << ", " 
              << TT_GATE_IN3_GPIO << ", " << TT_GATE_IN4_GPIO << std::endl;
    std::cout << "  - Gate Outputs: GPIO " << TT_GATE_OUT1_GPIO << ", " << TT_GATE_OUT2_GPIO << std::endl;
    
    // Flash gate outputs to show GPIO is working
    DEBUG_PRINT("GPIO Health Check: Flashing gate outputs...\n");
    set_gate_output_libgpiod(0, 1);
    usleep(100000);
    set_gate_output_libgpiod(0, 0);
    usleep(100000);
    set_gate_output_libgpiod(1, 1);
    usleep(100000);
    set_gate_output_libgpiod(1, 0);

    // State tracking for hysteresis (deadzone filtering)
    std::vector<uint16_t> last_fader_values(NUM_FADERS, 0xFFFF);
    std::vector<uint8_t> last_pot_values(NUM_TT_POTS, 0xFF);
    std::vector<uint16_t> last_pot_raw(NUM_TT_POTS, 0xFFFF);
    
    // Terminal Tedium button state tracking
    int last_btn1_state = 0, last_btn2_state = 0, last_btn3_state = 0;
    
    // Terminal Tedium gate input state tracking
    int last_gate_in[NUM_TT_GATES_IN] = {0, 0, 0, 0};

    // Main event loop
    while (true) {
        // Process incoming MIDI events (e.g., from DAW)
        process_alsa_input(seq_handle, i2c_fd, macropad);

        // Update MacroPad state - capture any button presses for system control
        macropad.update();

        // Poll Terminal Tedium buttons
        int btn1_state = read_button_libgpiod(0);
        int btn2_state = read_button_libgpiod(1);
        int btn3_state = read_button_libgpiod(2);
        
        DEBUG_PRINT("TT Button states: btn1=%d btn2=%d btn3=%d\n", btn1_state, btn2_state, btn3_state);
        
        // Detect button events (rising edges - when button is pressed)
        if (btn1_state == 1 && last_btn1_state == 0) {
            DEBUG_PRINT("TT Button 1 PRESSED\n");
            macropad.send_fudi("cls");
            macropad.send_fudi("prn 20 10 1 BUTTON 1");
            macropad.send_fudi("prn 20 25 1 PRESSED");
            std::cout << "[TT] Button 1 pressed" << std::endl;
        }
        
        if (btn2_state == 1 && last_btn2_state == 0) {
            DEBUG_PRINT("TT Button 2 PRESSED\n");
            macropad.send_fudi("cls");
            macropad.send_fudi("prn 20 10 1 BUTTON 2");
            macropad.send_fudi("prn 20 25 1 PRESSED");
            std::cout << "[TT] Button 2 pressed" << std::endl;
        }
        
        if (btn3_state == 1 && last_btn3_state == 0) {
            DEBUG_PRINT("TT Button 3 PRESSED\n");
            macropad.send_fudi("cls");
            macropad.send_fudi("prn 20 10 1 BUTTON 3");
            macropad.send_fudi("prn 20 25 1 PRESSED");
            std::cout << "[TT] Button 3 pressed" << std::endl;
        }
        
        // Detect button combo: 1+2 pressed (rising edge)
        if (btn1_state == 1 && btn2_state == 1 && last_btn1_state == 0) {
            DEBUG_PRINT("TT Button combo 1+2 DETECTED\n");
            macropad.set_system_control_mode(true);
            // Send display message to MacroPad
            macropad.send_fudi("cls");
            macropad.send_fudi("prn 20 10 1 REBOOT?");
            macropad.send_fudi("prn 20 25 1 SHUTDOWN?");
            macropad.send_fudi("prn 15 40 1 CANCEL?");
            std::cout << "[TT] Button combo 1+2 detected - showing system control prompt" << std::endl;
        }
        
        last_btn1_state = btn1_state;
        last_btn2_state = btn2_state;
        last_btn3_state = btn3_state;

        // Poll Terminal Tedium gate inputs (for future use)
        int gate_in_states[NUM_TT_GATES_IN];
        gate_in_states[0] = read_gate_input_libgpiod(0);
        gate_in_states[1] = read_gate_input_libgpiod(1);
        gate_in_states[2] = read_gate_input_libgpiod(2);
        gate_in_states[3] = read_gate_input_libgpiod(3);
        
        DEBUG_PRINT("TT Gate IN states: in1=%d in2=%d in3=%d in4=%d\n", 
                    gate_in_states[0], gate_in_states[1], gate_in_states[2], gate_in_states[3]);
        
        // Detect gate input changes (rising edges)
        for (int g = 0; g < NUM_TT_GATES_IN; ++g) {
            if (gate_in_states[g] == 1 && last_gate_in[g] == 0) {
                DEBUG_PRINT("TT Gate Input %d TRIGGERED\n", g + 1);
                // Gate input triggered - available for future use
            }
            last_gate_in[g] = gate_in_states[g];
        }

        // Poll and send fader values
        for (uint8_t i = 0; i < NUM_FADERS; ++i) {
            uint16_t raw_val = 0;
            if (read_fader_i2c(i2c_fd, i, raw_val) == 0) {
                if (is_14bit) {
                    // 14-bit mode: send as two 7-bit CC values
                    // CC N = MSB (bits 13-7), CC N+32 = LSB (bits 6-0)
                    if (last_fader_values[i] == 0xFFFF || 
                        std::abs(static_cast<int>(raw_val) - static_cast<int>(last_fader_values[i])) > FADER_DEADZONE) {
                        last_fader_values[i] = raw_val;
                        send_alsa_cc(seq_handle, out_port, MIDI_CHANNEL, i, (raw_val >> 7) & 0x7F);
                        send_alsa_cc(seq_handle, out_port, MIDI_CHANNEL, i + 32, raw_val & 0x7F);
                    }
                } else {
                    // 7-bit mode: send only MSB
                    uint8_t val_7bit = (raw_val >> 7) & 0x7F;
                    if (last_fader_values[i] != val_7bit) {
                        last_fader_values[i] = val_7bit;
                        send_alsa_cc(seq_handle, out_port, MIDI_CHANNEL, i, val_7bit);
                    }
                }
            }
        }

        // Poll and send pot values (if SPI is available)
        if (spi_fd >= 0) {
            for (uint8_t p = 0; p < NUM_TT_POTS; ++p) {
                uint16_t adc_raw = 0;
                if (read_mcp3208_spi(spi_fd, p, adc_raw) == 0) {
                    // Apply hysteresis to avoid jitter
                    if (last_pot_raw[p] == 0xFFFF || 
                        std::abs(static_cast<int>(adc_raw) - static_cast<int>(last_pot_raw[p])) >= POT_DEADZONE) {
                        last_pot_raw[p] = adc_raw;
                        uint8_t val_7bit = (adc_raw >> 5) & 0x7F;
                        if (last_pot_values[p] != val_7bit) {
                            last_pot_values[p] = val_7bit;
                            send_alsa_cc(seq_handle, out_port, MIDI_CHANNEL, TT_CC_OFFSET + p, val_7bit);
                        }
                    }
                }
            }
        }

        usleep(POLL_INTERVAL_US);
    }

    // Cleanup
    cleanup_gpio_libgpiod();
    snd_seq_close(seq_handle);
    if (spi_fd >= 0) close(spi_fd);
    close(i2c_fd);
    return 0;
}
