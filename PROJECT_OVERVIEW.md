# Project Overview

## What This Is

A bridge that connects Eurorack versions of the  Adafruit macropad, Tesseract Sweet Sixteen MKII faderbank and Terminal Tedium module to music software running on a Raspberry Pi.

**Core Concept:**
- Use **Sweet16 MKII** (16n firmware) as your physical interface and hardware midi port
- Use **Terminal Tedium** pots for auxiliary control
- Use **Adafruit Macropad for software defined switch controls and parmeter display
- Run **ChooChooTracker** (8-bit retro sequencer) or **PureData** (live coding synth)

## Hardware Stack

| Device | Role | Connection | Purpose |
|--------|------|-----------|---------|
| Sweet16 MKII | Fader Control | I2C (0x34) | 16 faders → Software MIDI + Hardware MIDI OUT |
| Terminal Tedium | Pots | SPI (MCP3208) | 6 pots → Auxiliary MIDI CC data |
| Adafruit MacroPad | Mode Control | USB Serial | 12 keys + encoder + OLED display |
| Raspberry Pi | Sequencer Host | Local | Runs ChooChooTracker or PureData |
| Eurorack Modules | Synth/Effects | MIDI-CV input | Receives MIDI from Sweet16 hardware output |

## Software Stack

| Component | Role |
|-----------|------|
| `midi_bridge.cpp` | I2C/SPI to ALSA MIDI bridge + MacroPad interface |
| `macropad.hpp` | MacroPad serial communication and HID/UDP interface |
| `16n_firmware` | modified Sweet16 MKII firmware with I2C + hardware MIDI |
| `macro-3.ino` | modified Adafruit MacroPad firmware with key profiles and 2 way serial control (for display drawing and lights)|
| **ChooChooTracker** | 8-bit retro style sequencer/tracker |
| **PureData** | Real-time audio synthesis and processing |
| **ALSA** | Linux audio system for MIDI routing |

## Signal Flow

```
Physical Hardware
├─ Sweet16 MKII Faders
│  └─ Read via I2C
│     └─ Convert to MIDI CC 0-15
│        ├─ Send to ChooChooTracker/PureData
│        └─ Also output via hardware MIDI
│
├─ Terminal Tedium Pots
│  └─ Read via SPI ADC
│     └─ Convert to MIDI CC 16-21
│        └─ Send to ChooChooTracker/PureData
│
└─ Adafruit MacroPad
   └─ Read via USB Serial
      ├─ 12 programmable keys
      ├─ Rotary encoder
      └─ OLED display feedback

Software
├─ ChooChooTracker
│  ├─ Receives CC data from hardware
│  ├─ Receives key commands from MacroPad
│  ├─ Generates MIDI notes to hardware synths
│  └─ Outputs via Sweet16 hardware MIDI
│
└─ PureData
   ├─ Receives CC data for parameter modulation
   ├─ Receives UDP FUDI from MacroPad
   ├─ Runs custom synth/effect patches
   └─ Outputs via Sweet16 hardware MIDI

Output
└─  Midi to cv etc.. 
```


## Project Components

### midi_bridge.cpp
The core I2C/SPI to ALSA MIDI converter with MacroPad support:
- Reads Sweet16 faders via I2C
- Reads Terminal Tedium pots via SPI
- Sends MIDI CC data to software
- Receives MIDI commands and outputs via I2C
- Provides hysteresis-filtered analog-to-MIDI conversion
- Manages MacroPad serial communication

### macropad.hpp
MacroPad interface library:
- Serial communication with Adafruit MacroPad
- Key mapping profiles for each mode (TrackWerk, ChooChoo, PureData)
- OLED display updates
- HID keyboard event generation
- UDP FUDI socket for PureData communication

### 16n Firmware
Modified 16n faderbank firmware for Sweet16 MKII:
- Enables I2C slave mode for Raspberry Pi communication
- Provides hardware MIDI output jack
- Converts I2C commands to hardware MIDI
- Receives MIDI clock for sync

### MacroPad Firmware
Adafruit MacroPad customization:
- 12 programmable keys with RGB feedback
- OLED display for mode and key labels
- Rotary encoder for navigation
- Mode-specific key profiles

## Configuration & Customization

### MIDI Mappings
Faders and pots send on fixed CC numbers:
- Sweet16 faders: CC 0-15
- Terminal Tedium pots: CC 16-21

Can be customized by editing constants in midi_bridge.cpp

### Deadzone Filtering
Analog noise is filtered with hysteresis:
- Faders: 2 LSB threshold
- Pots: 8 ADC counts threshold

Adjustable in code if you need more/less sensitivity

### Resolution Modes
- **7-bit mode**: Standard MIDI CC (0-127) - good for ChooChooTracker
- **14-bit mode**: High precision (MSB + LSB) - good for PureData patches

## Performance Characteristics

- **Latency**: 5-10ms (5ms polling interval + ALSA processing)
- **Resolution**: 14-bit faders, 12-bit pots (downsampled to 7-bit MIDI)
- **Polling**: 200Hz refresh rate
- **I2C Speed**: 100kHz standard mode
- **SPI Speed**: 1MHz

Suitable for real-time performance control. Not suitable for audio-rate modulation.

## References & Resources

