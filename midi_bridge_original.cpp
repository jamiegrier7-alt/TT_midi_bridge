/*
 * MIDI Bridge: I2C/SPI to ALSA MIDI Gateway
 * 
 * This application bridges three hardware controllers to ALSA MIDI:
 * 1. Sweet Sixteen Faders (I2C) - 16 analog faders on I2C bus
 * 2. Terminal Tedium Pots (SPI) - 6 potentiometers via MCP3208 SPI ADC
 * 3. Adafruit MacroPad (Serial) - 12 keys + RGB encoder (handled via serial/USB)
 * 
 * The bridge reads analog values from hardware, converts them to 7-bit or 14-bit
 * MIDI CC values, and broadcasts via ALSA sequencer. It also processes incoming
 * MIDI events and writes them back to Sweet Sixteen via I2C.
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

#include "macropad.hpp"

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

// ============================================================================
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
    snd_seq_event_t* ev = nullptr;
    while (snd_seq_event_input_pending(seq, 1) > 0) {
        if (snd_seq_event_input(seq, &ev) < 0 || !ev) break;
        uint8_t ch = (ev->data.note.channel & 0x0F) + 1;

        switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
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
                uint16_t unsigned_bend = static_cast<uint16_t>(ev->data.control.value + 8192);
                write_midi_i2c(i2c_fd, 0x04, pb_ch, unsigned_bend & 0x7F, (unsigned_bend >> 7) & 0x7F);
                break;
            }

            case SND_SEQ_EVENT_PGMCHANGE: {
                // Program Change 0 = TrackWerk, 1 = ChooChoo, 2 = PureData
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

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " <14bit|7bit> [trackwerk|choochoo|puredata]" << std::endl;
        return 1;
    }

    std::string bit_mode = argv[1];
    bool is_14bit = (bit_mode == "14bit");
    if (bit_mode != "14bit" && bit_mode != "7bit") {
        std::cerr << "Invalid bit mode '" << bit_mode << "'." << std::endl;
        return 1;
    }

    MacroMode macro_mode = MacroMode::CHOOCHOO;
    if (argc == 3) {
        std::string m = argv[2];
        if (m == "trackwerk") macro_mode = MacroMode::TRACKWERK;
        else if (m == "choochoo") macro_mode = MacroMode::CHOOCHOO;
        else if (m == "puredata") macro_mode = MacroMode::PUREDATA;
        else {
            std::cerr << "Invalid MacroPad mode '" << m << "'." << std::endl;
            return 1;
        }
    }

    int i2c_fd = open(I2C_DEVICE, O_RDWR);
    if (i2c_fd < 0 || ioctl(i2c_fd, I2C_SLAVE, SWEET16_ADDR) < 0) {
        std::cerr << "Failed to open Sweet Sixteen on " << I2C_DEVICE << std::endl;
        if (i2c_fd >= 0) close(i2c_fd);
        return 1;
    }

    int spi_fd = open(SPI_DEVICE, O_RDWR);
    if (spi_fd >= 0) {
        uint8_t spi_mode = SPI_MODE_0, spi_bits = 8;
        uint32_t spi_speed = SPI_SPEED_HZ;
        ioctl(spi_fd, SPI_IOC_WR_MODE, &spi_mode);
        ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &spi_bits);
        ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed);
    }

    snd_seq_t* seq_handle = nullptr;
    if (snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        std::cerr << "Error opening ALSA sequencer" << std::endl;
        if (spi_fd >= 0) close(spi_fd);
        close(i2c_fd);
        return 1;
    }
    snd_seq_nonblock(seq_handle, 1);
    snd_seq_set_client_name(seq_handle, "Sweet Sixteen I2C");

    int out_port = snd_seq_create_simple_port(seq_handle, "Faders Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    int in_port = snd_seq_create_simple_port(seq_handle, "MIDI to I2C In",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    MacroPadBridge macropad(MACROPAD_SERIAL, macro_mode);

    std::cout << "Hardware Bridge running [" << bit_mode << " | MacroPad: " << macropad.mode_name() << "]" << std::endl;

    std::vector<uint16_t> last_fader_values(NUM_FADERS, 0xFFFF);
    std::vector<uint8_t> last_pot_values(NUM_TT_POTS, 0xFF);
    std::vector<uint16_t> last_pot_raw(NUM_TT_POTS, 0xFFFF);

    while (true) {
        process_alsa_input(seq_handle, i2c_fd, macropad);
        macropad.update();

        for (uint8_t i = 0; i < NUM_FADERS; ++i) {
            uint16_t raw_val = 0;
            if (read_fader_i2c(i2c_fd, i, raw_val) == 0) {
                if (is_14bit) {
                    if (last_fader_values[i] == 0xFFFF || std::abs(static_cast<int>(raw_val) - static_cast<int>(last_fader_values[i])) > 2) {
                        last_fader_values[i] = raw_val;
                        send_alsa_cc(seq_handle, out_port, MIDI_CHANNEL, i, (raw_val >> 7) & 0x7F);
                        send_alsa_cc(seq_handle, out_port, MIDI_CHANNEL, i + 32, raw_val & 0x7F);
                    }
                } else {
                    uint8_t val_7bit = (raw_val >> 7) & 0x7F;
                    if (last_fader_values[i] != val_7bit) {
                        last_fader_values[i] = val_7bit;
                        send_alsa_cc(seq_handle, out_port, MIDI_CHANNEL, i, val_7bit);
                    }
                }
            }
        }

        if (spi_fd >= 0) {
            for (uint8_t p = 0; p < NUM_TT_POTS; ++p) {
                uint16_t adc_raw = 0;
                if (read_mcp3208_spi(spi_fd, p, adc_raw) == 0) {
                    if (last_pot_raw[p] == 0xFFFF || std::abs(static_cast<int>(adc_raw) - static_cast<int>(last_pot_raw[p])) >= 8) {
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

        usleep(5000);
    }

    snd_seq_close(seq_handle);
    if (spi_fd >= 0) close(spi_fd);
    close(i2c_fd);
    return 0;
}
