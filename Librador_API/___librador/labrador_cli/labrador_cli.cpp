// labrador-cli: a command-line interface to the EspoTek Labrador.
//
// This is a thin, scriptable front-end over the Librador API. It exposes the
// same device capabilities as the desktop GUI -- reading sensor/measurement
// data (oscilloscope analog channels and logic-analyzer digital channels),
// reading device info (firmware version/variant), and configuring the device
// (power supply, signal generators, oscilloscope gain/mode, digital outputs,
// reset / bootloader) -- without any GUI or Qt dependency.
//
// Output is deliberately plain text / CSV so it can be piped into other tools.

#include "librador.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// How long to let the USB isochronous stream fill its buffers before we read
// captured analog/digital samples. The polling thread runs in the background
// inside the library, so a freshly connected device needs a moment of data.
constexpr double kDefaultSettleSeconds = 0.25;

void printUsage(const char* prog)
{
    std::printf(
        "labrador-cli -- command-line control of the EspoTek Labrador\n"
        "\n"
        "USAGE:\n"
        "  %s <command> [options]\n"
        "\n"
        "DEVICE INFO / SENSORS (read):\n"
        "  info                       Connect and print firmware version & variant\n"
        "  scope <ch> [opts]          Read analog (oscilloscope) samples from channel 1 or 2\n"
        "  logic <ch> [opts]          Read digital (logic-analyzer) samples from a channel\n"
        "\n"
        "CONFIGURE / CONTROL (write):\n"
        "  psu <volts>                Set programmable power-supply output voltage\n"
        "  mode <0-7>                 Set device acquisition mode (see below)\n"
        "  gain <g>                   Set oscilloscope gain (0.5,1,2,4,8,16,32,64)\n"
        "  siggen <ch> <wave> <freq_hz> <amp_v> <offset_v>\n"
        "                             Drive signal-gen channel (1/2) with a waveform\n"
        "                             wave = sine | square | triangle | sawtooth\n"
        "  digital-out <ch> <on|off>  Set a digital output line (channel 1-4)\n"
        "  reset                      Reset the device\n"
        "  bootloader                 Reboot the device into its DFU bootloader\n"
        "\n"
        "SCOPE/LOGIC OPTIONS:\n"
        "  --seconds <s>     Capture window length          (scope default 0.01, logic 0.01)\n"
        "  --rate <hz>       Requested sample rate          (default: device max)\n"
        "  --delay <s>       Delay back from 'now'          (default 0)\n"
        "  --mode <m>        Acquisition mode to set first  (scope default 1, logic default 3)\n"
        "  --gain <g>        Oscilloscope gain to set first (scope only)\n"
        "  --settle <s>      Buffer fill time before read   (default %.2f)\n"
        "  --stats           Also print min/max/mean to stderr\n"
        "\n"
        "DEVICE MODES (set with `mode` or --mode):\n"
        "  0  single-channel oscilloscope        (375 kSPS, CH1)\n"
        "  1  dual-channel oscilloscope          (375 kSPS, CH1+CH2)\n"
        "  2  dual-channel oscilloscope          (375 kSPS)\n"
        "  3  oscilloscope CH1 + logic analyzer  (375 kSPS)\n"
        "  4  oscilloscope CH2 + logic analyzer  (375 kSPS)\n"
        "  6  single-channel oscilloscope        (750 kSPS, high speed)\n"
        "  7  multimeter                         (375 kSPS)\n"
        "\n"
        "Output is CSV on stdout (`# ` header lines are comments). Exit code is\n"
        "non-zero on failure.\n",
        prog, kDefaultSettleSeconds);
}

// ---- small argument helpers ------------------------------------------------

struct Options {
    double seconds = 0.01;
    double rate = 0.0; // 0 => use device-native rate
    double delay = 0.0;
    int mode = -1; // -1 => use command default
    double gain = -1.0; // -1 => leave unchanged
    double settle = kDefaultSettleSeconds;
    bool stats = false;
};

// Parse "--key value" style options out of argv starting at `start`.
// Positional args already consumed by the caller. Returns false on a bad flag.
bool parseOptions(int argc, char** argv, int start, Options& opt)
{
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        auto needVal = [&](double& dst) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", a.c_str());
                return false;
            }
            dst = std::atof(argv[++i]);
            return true;
        };
        if (a == "--seconds") { if (!needVal(opt.seconds)) return false; }
        else if (a == "--rate") { if (!needVal(opt.rate)) return false; }
        else if (a == "--delay") { if (!needVal(opt.delay)) return false; }
        else if (a == "--settle") { if (!needVal(opt.settle)) return false; }
        else if (a == "--gain") { if (!needVal(opt.gain)) return false; }
        else if (a == "--mode") {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: --mode requires a value\n"); return false; }
            opt.mode = std::atoi(argv[++i]);
        }
        else if (a == "--stats") { opt.stats = true; }
        else {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            return false;
        }
    }
    return true;
}

// ---- connection lifecycle --------------------------------------------------

// Bring up the library and open the USB device. Prints a helpful diagnostic on
// failure (the most common cause on Linux is missing udev permissions).
bool connectDevice()
{
    int error = librador_init();
    if (error) {
        std::fprintf(stderr, "error: librador_init failed (%d)\n", error);
        return false;
    }
    error = librador_setup_usb();
    if (error) {
        std::fprintf(stderr,
            "error: could not open Labrador over USB (code %d).\n"
            "  - Is the device plugged in?\n"
            "  - On Linux you may need udev rules / to run as root.\n"
            "    See Desktop_Interface/build_linux/69-labrador.rules in this repo.\n",
            error);
        return false;
    }
    return true;
}

void disconnectDevice()
{
    librador_exit();
}

// ---- commands --------------------------------------------------------------

int cmdInfo()
{
    if (!connectDevice()) return 1;
    uint16_t version = librador_get_device_firmware_version();
    uint8_t variant = librador_get_device_firmware_variant();
    std::printf("Labrador connected\n");
    std::printf("  firmware_version : %u\n", version);
    std::printf("  firmware_variant : %u\n", variant);
    disconnectDevice();
    return 0;
}

void printStats(const std::vector<double>& v)
{
    if (v.empty()) return;
    double mn = v[0], mx = v[0], sum = 0.0;
    for (double x : v) {
        if (x < mn) mn = x;
        if (x > mx) mx = x;
        sum += x;
    }
    std::fprintf(stderr, "# samples=%zu min=%.6f max=%.6f mean=%.6f\n",
        v.size(), mn, mx, sum / static_cast<double>(v.size()));
}

int cmdScope(int channel, const Options& in)
{
    Options opt = in;
    if (opt.mode < 0) opt.mode = 1; // dual-channel scope by default
    if (channel != 1 && channel != 2) {
        std::fprintf(stderr, "error: scope channel must be 1 or 2\n");
        return 2;
    }
    if (!connectDevice()) return 1;

    int error = librador_set_device_mode(opt.mode);
    if (error) {
        std::fprintf(stderr, "error: set_device_mode(%d) failed (%d)\n", opt.mode, error);
        disconnectDevice();
        return 1;
    }
    if (opt.gain > 0) {
        error = librador_set_oscilloscope_gain(opt.gain);
        if (error) {
            std::fprintf(stderr, "error: set_oscilloscope_gain(%g) failed (%d)\n", opt.gain, error);
            disconnectDevice();
            return 1;
        }
    }

    // Let the background USB stream accumulate data before sampling.
    std::this_thread::sleep_for(std::chrono::duration<double>(opt.settle));

    double rate = opt.rate > 0 ? opt.rate : 375000.0;
    std::vector<double>* data =
        librador_get_analog_data(channel, opt.seconds, rate, opt.delay, 0);
    if (!data) {
        std::fprintf(stderr,
            "error: no analog data returned. The capture buffer may not be full "
            "yet -- try a larger --settle or smaller --seconds.\n");
        disconnectDevice();
        return 1;
    }

    std::printf("# labrador scope: channel=%d mode=%d rate_hz=%g window_s=%g\n",
        channel, opt.mode, rate, opt.seconds);
    std::printf("time_s,voltage\n");
    double dt = 1.0 / rate;
    for (size_t i = 0; i < data->size(); ++i) {
        std::printf("%.9f,%.6f\n", static_cast<double>(i) * dt, (*data)[i]);
    }
    if (opt.stats) printStats(*data);

    disconnectDevice();
    return 0;
}

int cmdLogic(int channel, const Options& in)
{
    Options opt = in;
    if (opt.mode < 0) opt.mode = 3; // a mode that streams the digital channels
    if (!connectDevice()) return 1;

    int error = librador_set_device_mode(opt.mode);
    if (error) {
        std::fprintf(stderr, "error: set_device_mode(%d) failed (%d)\n", opt.mode, error);
        disconnectDevice();
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(opt.settle));

    double rate = opt.rate > 0 ? opt.rate : 750000.0;
    std::vector<uint8_t>* data =
        librador_get_digital_data(channel, opt.seconds, rate, opt.delay);
    if (!data) {
        std::fprintf(stderr,
            "error: no digital data returned. Try a larger --settle or smaller --seconds.\n");
        disconnectDevice();
        return 1;
    }

    std::printf("# labrador logic: channel=%d mode=%d rate_hz=%g window_s=%g\n",
        channel, opt.mode, rate, opt.seconds);
    std::printf("time_s,bit\n");
    double dt = 1.0 / rate;
    for (size_t i = 0; i < data->size(); ++i) {
        std::printf("%.9f,%u\n", static_cast<double>(i) * dt, (*data)[i] ? 1u : 0u);
    }

    disconnectDevice();
    return 0;
}

int cmdPsu(double volts)
{
    if (!connectDevice()) return 1;
    int error = librador_set_power_supply_voltage(volts);
    if (error) {
        std::fprintf(stderr,
            "error: set_power_supply_voltage(%g) failed (%d) -- value likely out of range\n",
            volts, error);
        disconnectDevice();
        return 1;
    }
    std::printf("Power supply set to %.3f V\n", volts);
    disconnectDevice();
    return 0;
}

int cmdMode(int mode)
{
    if (!connectDevice()) return 1;
    int error = librador_set_device_mode(mode);
    if (error) {
        std::fprintf(stderr, "error: set_device_mode(%d) failed (%d)\n", mode, error);
        disconnectDevice();
        return 1;
    }
    std::printf("Device mode set to %d\n", mode);
    disconnectDevice();
    return 0;
}

int cmdGain(double gain)
{
    if (!connectDevice()) return 1;
    int error = librador_set_oscilloscope_gain(gain);
    if (error) {
        std::fprintf(stderr,
            "error: set_oscilloscope_gain(%g) failed (%d). Valid: 0.5,1,2,4,8,16,32,64\n",
            gain, error);
        disconnectDevice();
        return 1;
    }
    std::printf("Oscilloscope gain set to %g\n", gain);
    disconnectDevice();
    return 0;
}

int cmdSiggen(int channel, const std::string& wave, double freq, double amp, double offset)
{
    if (!connectDevice()) return 1;
    int error;
    if (wave == "sine") error = librador_send_sin_wave(channel, freq, amp, offset);
    else if (wave == "square") error = librador_send_square_wave(channel, freq, amp, offset);
    else if (wave == "triangle") error = librador_send_triangle_wave(channel, freq, amp, offset);
    else if (wave == "sawtooth") error = librador_send_sawtooth_wave(channel, freq, amp, offset);
    else {
        std::fprintf(stderr, "error: unknown waveform '%s' (sine|square|triangle|sawtooth)\n", wave.c_str());
        disconnectDevice();
        return 2;
    }
    if (error) {
        std::fprintf(stderr,
            "error: signal generator failed (%d). Check channel (1/2), voltage range, frequency.\n",
            error);
        disconnectDevice();
        return 1;
    }
    std::printf("Channel %d: %s wave %g Hz, amplitude %g V, offset %g V\n",
        channel, wave.c_str(), freq, amp, offset);
    disconnectDevice();
    return 0;
}

int cmdDigitalOut(int channel, bool on)
{
    if (!connectDevice()) return 1;
    int error = librador_set_digital_out(channel, on);
    if (error) {
        std::fprintf(stderr,
            "error: set_digital_out(ch=%d) failed (%d). Channel must be 1-4.\n",
            channel, error);
        disconnectDevice();
        return 1;
    }
    std::printf("Digital output %d set %s\n", channel, on ? "ON" : "OFF");
    disconnectDevice();
    return 0;
}

int cmdReset(bool bootloader)
{
    if (!connectDevice()) return 1;
    int error = bootloader ? librador_jump_to_bootloader() : librador_reset_device();
    if (error) {
        std::fprintf(stderr, "error: %s failed (%d)\n",
            bootloader ? "jump_to_bootloader" : "reset_device", error);
        disconnectDevice();
        return 1;
    }
    std::printf("%s requested\n", bootloader ? "Bootloader" : "Device reset");
    disconnectDevice();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        printUsage(argv[0]);
        return 2;
    }

    std::string cmd = argv[1];

    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        printUsage(argv[0]);
        return 0;
    }

    if (cmd == "info") {
        return cmdInfo();
    }

    if (cmd == "scope" || cmd == "logic") {
        if (argc < 3) {
            std::fprintf(stderr, "error: %s requires a channel number\n", cmd.c_str());
            return 2;
        }
        int channel = std::atoi(argv[2]);
        Options opt;
        if (!parseOptions(argc, argv, 3, opt)) return 2;
        return cmd == "scope" ? cmdScope(channel, opt) : cmdLogic(channel, opt);
    }

    if (cmd == "psu") {
        if (argc < 3) { std::fprintf(stderr, "error: psu requires a voltage\n"); return 2; }
        return cmdPsu(std::atof(argv[2]));
    }

    if (cmd == "mode") {
        if (argc < 3) { std::fprintf(stderr, "error: mode requires a number 0-7\n"); return 2; }
        return cmdMode(std::atoi(argv[2]));
    }

    if (cmd == "gain") {
        if (argc < 3) { std::fprintf(stderr, "error: gain requires a value\n"); return 2; }
        return cmdGain(std::atof(argv[2]));
    }

    if (cmd == "siggen") {
        if (argc < 7) {
            std::fprintf(stderr,
                "error: siggen <ch> <sine|square|triangle|sawtooth> <freq_hz> <amp_v> <offset_v>\n");
            return 2;
        }
        return cmdSiggen(std::atoi(argv[2]), argv[3],
            std::atof(argv[4]), std::atof(argv[5]), std::atof(argv[6]));
    }

    if (cmd == "digital-out") {
        if (argc < 4) {
            std::fprintf(stderr, "error: digital-out <ch> <on|off>\n");
            return 2;
        }
        std::string state = argv[3];
        bool on;
        if (state == "on" || state == "1" || state == "high") on = true;
        else if (state == "off" || state == "0" || state == "low") on = false;
        else { std::fprintf(stderr, "error: state must be on|off\n"); return 2; }
        return cmdDigitalOut(std::atoi(argv[2]), on);
    }

    if (cmd == "reset") {
        return cmdReset(false);
    }

    if (cmd == "bootloader") {
        return cmdReset(true);
    }

    std::fprintf(stderr, "error: unknown command '%s'\n\n", cmd.c_str());
    printUsage(argv[0]);
    return 2;
}
