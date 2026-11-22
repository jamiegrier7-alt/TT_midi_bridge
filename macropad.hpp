#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/uinput.h>

enum class MacroMode { TRACKWERK, CHOOCHOO, PUREDATA };

struct KeyCell {
    int keycode;
    std::string label;
    uint8_t r, g, b;
};

class MacroPadBridge {
public:
    MacroPadBridge(const std::string& serial_dev, MacroMode initial_mode)
        : port_path_(serial_dev), mode_(initial_mode), serial_fd_(-1), uinput_fd_(-1), udp_fd_(-1), system_control_mode_(false) {
        init_profiles();
        init_uinput();
        init_udp();
    }

    ~MacroPadBridge() {
        if (serial_fd_ >= 0) close(serial_fd_);
        if (uinput_fd_ >= 0) {
            ioctl(uinput_fd_, UI_DEV_DESTROY);
            close(uinput_fd_);
        }
        if (udp_fd_ >= 0) close(udp_fd_);
    }

    void set_mode(MacroMode new_mode) {
        mode_ = new_mode;
        std::cout << "[MacroPad] Switched mode to: " << mode_name() << std::endl;
        if (serial_fd_ >= 0) {
            apply_mode_display();
        }
    }

    std::string mode_name() const {
        if (mode_ == MacroMode::TRACKWERK) return "trackwerk";
        if (mode_ == MacroMode::CHOOCHOO) return "choochoo";
        return "puredata";
    }

    void update() {
        // Auto-reconnect serial if disconnected or not yet plugged in
        if (serial_fd_ < 0) {
            if (open_serial()) {
                std::cout << "[MacroPad] Connected on " << port_path_ << std::endl;
                usleep(350000); // Wait 350ms for RP2040 boot screen
                apply_mode_display();
            } else {
                return;
            }
        }

        // 1. Read incoming FUDI lines from MacroPad Serial
        char buf[256];
        ssize_t n = read(serial_fd_, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            rx_accum_ += buf;
            size_t pos = 0;
            while ((pos = rx_accum_.find('\n')) != std::string::npos) {
                std::string line = rx_accum_.substr(0, pos);
                rx_accum_.erase(0, pos + 1);
                handle_serial_line(line);
            }
     } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
         // Actual USB disconnect / I/O error
         std::cerr << "[MacroPad] Serial read error: " << strerror(errno) << std::endl;
         close(serial_fd_);
         serial_fd_ = -1;
         rx_accum_.clear();
         std::cout << "[MacroPad] Disconnected." << std::endl;
     }

        // 2. If in PUREDATA mode, forward incoming UDP FUDI packets to MacroPad OLED/LEDs
        if (mode_ == MacroMode::PUREDATA && udp_fd_ >= 0 && serial_fd_ >= 0) {
            char udp_buf[512];
            ssize_t un = recv(udp_fd_, udp_buf, sizeof(udp_buf) - 1, 0);
            if (un > 0) {
                udp_buf[un] = '\0';
                std::string msg(udp_buf);
                if (!msg.empty() && msg.back() != '\n') msg += "\n";
                write(serial_fd_, msg.c_str(), msg.size());
            }
        }
    }

    void send_fudi(const std::string& cmd) {
        if (serial_fd_ < 0) {
            std::cerr << "[MacroPad] Warning: serial_fd_ < 0, cannot send: " << cmd << std::endl;
            return;
        }
        std::string pkt = cmd + ";\n";
        ssize_t written = write(serial_fd_, pkt.c_str(), pkt.size());
        if (written < 0) {
            std::cerr << "[MacroPad] Error writing to serial: " << strerror(errno) << std::endl;
        } else if (written != static_cast<ssize_t>(pkt.size())) {
            std::cerr << "[MacroPad] Warning: partial write - wrote " << written << "/" << pkt.size() << " bytes" << std::endl;
        }
        std::cout << "[MacroPad] Sent: " << cmd << std::endl;
        usleep(8000); // 8ms pacing for RP2040 buffer
    }

    bool is_system_control_mode() const {
        return system_control_mode_;
    }

    void set_system_control_mode(bool active) {
        system_control_mode_ = active;
    }

private:
    std::string port_path_;
    MacroMode mode_;
    int serial_fd_;
    int uinput_fd_;
    int udp_fd_;
    std::string rx_accum_;
    struct sockaddr_in pd_dest_addr_;
    bool system_control_mode_;  // Track if system reboot/shutdown prompt is active

    // Key indices 1..12 mapped to physical positions
    std::map<int, KeyCell> trackwerk_map_;
    std::map<int, KeyCell> choochoo_map_;

    void init_profiles() {
        trackwerk_map_ = {
            {1,  {KEY_LEFTSHIFT, "SHFT", 0, 0, 0}},
            {5,  {KEY_SPACE,     "PLAY", 0,  0, 0}},
            {9,  {KEY_F11,       "F11",  0, 0,  0}},
            {2,  {KEY_U,         "U",    0, 0, 0}},
            {6,  {0,             "---",  0,  0,  0}},
            {10, {KEY_I,         "I",    0, 0, 0}},
            {3,  {KEY_J,         "J",    0, 0, 0}},
            {7,  {KEY_UP,        "UP",   0,  0, 0}},
            {11, {KEY_K,         "K",    0, 0, 0}},
            {4,  {KEY_LEFT,      "LEFT", 0,  0, 0}},
            {8,  {KEY_DOWN,      "DOWN", 0,  0, 0}},
            {12, {KEY_RIGHT,     "RGHT", 0,  0, 0}}
        };

        choochoo_map_ = {
            {1,  {KEY_W,         "REC",    0, 0, 0}},
            {5,  {KEY_Q,         "LIVE",    0,  0, 0}},
            {9,  {KEY_E,         "DEL",    0, 0, 0}},
            {2,  {KEY_LEFTSHIFT, "SHFT", 0, 0, 0}},
            {6,  {KEY_SPACE,     "PLAY", 0, 0, 0}},
            {10, {KEY_ESC,       "ESC",  0, 0, 0}},
            {3,  {KEY_Z,         "EDIT", 0,  0, 0}},
            {7,  {KEY_UP,        "UP",   0,  0, 0}},
            {11, {KEY_X,         "OPT",  0, 0, 0}},
            {4,  {KEY_LEFT,      "LEFT", 0,  0, 0}},
            {8,  {KEY_DOWN,      "DOWN", 0,  0, 0}},
            {12, {KEY_RIGHT,     "RGHT", 0,  0, 0}}
        };
    }

    bool open_serial() {
        int fd = open(port_path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) return false;

        struct termios tty;
        std::memset(&tty, 0, sizeof(tty));
        if (tcgetattr(fd, &tty) != 0) {
            close(fd);
            return false;
        }
        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);
        cfmakeraw(&tty);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tty);

        int status = 0;
        ioctl(fd, TIOCMGET, &status);
        status |= TIOCM_DTR;
        ioctl(fd, TIOCMSET, &status);

        serial_fd_ = fd;
        return true;
    }

    void init_uinput() {
        uinput_fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (uinput_fd_ < 0) {
            std::cerr << "[MacroPad] Warning: Could not open /dev/uinput (check permissions/sudo)." << std::endl;
            return;
        }
        ioctl(uinput_fd_, UI_SET_EVBIT, EV_KEY);
        for (const auto& kv : trackwerk_map_) {
            if (kv.second.keycode > 0) ioctl(uinput_fd_, UI_SET_KEYBIT, kv.second.keycode);
        }
        for (const auto& kv : choochoo_map_) {
            if (kv.second.keycode > 0) ioctl(uinput_fd_, UI_SET_KEYBIT, kv.second.keycode);
        }

        struct uinput_setup usetup;
        std::memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor  = 0x239A;
        usetup.id.product = 0x8108;
        std::strcpy(usetup.name, "MacroPad-Bridge");

        ioctl(uinput_fd_, UI_DEV_SETUP, &usetup);
        ioctl(uinput_fd_, UI_DEV_CREATE);
    }

    void init_udp() {
        udp_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (udp_fd_ < 0) return;

        // Listen on UDP 3002 (from Pure Data [netsend 3002])
        struct sockaddr_in bind_addr;
        std::memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = htons(3002);
        bind(udp_fd_, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr));

        // Send to UDP 3001 (to Pure Data [netreceive 3001])
        std::memset(&pd_dest_addr_, 0, sizeof(pd_dest_addr_));
        pd_dest_addr_.sin_family = AF_INET;
        pd_dest_addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        pd_dest_addr_.sin_port = htons(3001);
    }

    void send_key(int keycode, int value) {
        if (uinput_fd_ < 0 || keycode <= 0) return;
        struct input_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = EV_KEY;
        ev.code = keycode;
        ev.value = value;
        write(uinput_fd_, &ev, sizeof(ev));

        std::memset(&ev, 0, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        write(uinput_fd_, &ev, sizeof(ev));
    }

    void apply_mode_display() {
        send_fudi("cls");
        send_fudi("rgb_clr");

        if (mode_ == MacroMode::PUREDATA) {
            send_fudi("prn 10 15 2 PD MODE");
            send_fudi("drw_line 10 35 118 35");
            send_fudi("prn 20 45 1 Pure Data UDP");
            return;
        }

        const auto& active_map = (mode_ == MacroMode::TRACKWERK) ? trackwerk_map_ : choochoo_map_;
        std::string title = (mode_ == MacroMode::TRACKWERK) ? "TRACKWERK" : "CHOOCHOO TRACKER";

        send_fudi("prn 2 1 1 " + title);
        send_fudi("drw_line 0 10 127 10");

        // Grid mapping: Row 0..3, Col 0..2 -> Key index = (col * 4) + row + 1
        int col_w = 42;
        int row_h = 12;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 3; ++c) {
                int key_idx = (c * 4) + r + 1;
                const auto& cell = active_map.at(key_idx);
                int x = c * col_w + 1;
                int y = 13 + (r * row_h);

                send_fudi("drw_rect " + std::to_string(x) + " " + std::to_string(y) + " 40 11 0");
                int text_w = static_cast<int>(cell.label.size()) * 6;
                int tx = x + std::max(2, (40 - text_w) / 2);
                send_fudi("prn " + std::to_string(tx) + " " + std::to_string(y + 2) + " 1 " + cell.label);
                send_fudi("rgb " + std::to_string(key_idx) + " " +
                          std::to_string(cell.r) + " " + std::to_string(cell.g) + " " + std::to_string(cell.b));
            }
        }
    }

    void handle_serial_line(std::string line) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ';' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) return;

    std::cout << "[MacroPad] Received: " << line << std::endl;

        // Handle button presses during system control mode (reboot/shutdown prompt)
        int btn = 0, state = 0;
        if (system_control_mode_ && std::sscanf(line.c_str(), "btn %d %d", &btn, &state) == 2 && state == 1) {
            if (btn == 1) {
                std::cout << "[MacroPad] Button 1 pressed - executing REBOOT" << std::endl;
                send_fudi("cls");
                send_fudi("prn 20 25 2 REBOOTING...");
                usleep(500000);  // 500ms delay for display
                system("sudo -n reboot");
                system_control_mode_ = false;
                return;
            } else if (btn == 2) {
                std::cout << "[MacroPad] Button 2 pressed - executing SHUTDOWN" << std::endl;
                send_fudi("cls");
                send_fudi("prn 15 25 2 SHUTTING DOWN...");
                usleep(500000);  // 500ms delay for display
                system("sudo -n shutdown -h now");
                system_control_mode_ = false;
                return;
            } else if (btn == 3) {
                std::cout << "[MacroPad] Button 3 pressed - CANCEL system control" << std::endl;
                send_fudi("cls");
                apply_mode_display();  // Restore normal display
                system_control_mode_ = false;
                return;
            }
        }

        // In PUREDATA mode, forward raw FUDI line straight to Pd on UDP 3001
        if (mode_ == MacroMode::PUREDATA) {
            if (udp_fd_ >= 0) {
                std::string fwd = line + ";\n";
                sendto(udp_fd_, fwd.c_str(), fwd.size(), 0,
reinterpret_cast<struct sockaddr*>(&pd_dest_addr_), sizeof(pd_dest_addr_));
        }
        return;
    }
    // In TRACKWERK / CHOOCHOO mode, parse "btn  " normally
    if (std::sscanf(line.c_str(), "btn %d %d", &btn, &state) == 2) {
        const auto& active_map = (mode_ == MacroMode::TRACKWERK) ? trackwerk_map_ : choochoo_map_;
        auto it = active_map.find(btn);
        if (it != active_map.end() && it->second.keycode > 0 && (state == 0 || state == 1)) {
            std::cout << "[MacroPad] Processing button " << btn << " state " << state << " keycode " << it->second.keycode << std::endl;
            send_key(it->second.keycode, state);
        }
    }
}
};

