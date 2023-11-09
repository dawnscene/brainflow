#include <fstream>
#include <iostream>
#include <numeric>
#include <stdint.h>
#include <string.h>

#include "custom_cast.h"
#include "dawneeg.h"
#include "timestamp.h"

#ifndef _WIN32
#include <errno.h>
#endif

#define DAWNEEG_CMD_PROMPT "$$$"
#define DAWNEEG_CMD_SOFT_RESET "v"
#define DAWNEEG_CMD_DEFAULT "d"
#define DAWNEEG_CMD_START_STREAM "b"
#define DAWNEEG_CMD_STOP_STREAM "s"
#define DAWNEEG_CHAR_TIME_SYNC '<'
#define DAWNEEG_CHAR_TIME_SYNC_RESPONSE '>'

#define DAWNEEG_STREAM_HEADER 0xA0
#define DAWNEEG_STREAM_FOOTER 0xC0

DawnEEG::DawnEEG (int board_id, struct BrainFlowInputParams params) : Board (board_id, params)
{
    serial = NULL;
    is_streaming = false;
    keep_alive = false;
    initialized = false;
    state = (int)BrainFlowExitCodes::SYNC_TIMEOUT_ERROR;
    half_rtt = 1.79769e+308;
    time_correction = 0;
}

DawnEEG::~DawnEEG ()
{
    skip_logs = true;
    release_session ();
}

int DawnEEG::prepare_session ()
{
    // check params
    if (initialized)
    {
        LOG_F (INFO, "Session is already prepared");
        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    if (params.serial_port.empty ())
    {
        LOG_F (ERROR, "Serial port is not specified.");
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }
    if ((params.timeout > 6000) || (params.timeout < 1))
    {
        params.timeout = 100;
    }

    int ec = (int)BrainFlowExitCodes::STATUS_OK;

    do
    {
        ec = open_port ();
        if (ec != (int)BrainFlowExitCodes::STATUS_OK)
        {
            break;
        }

        // set initial settings
        ec = init_board ();
        if (ec != (int)BrainFlowExitCodes::STATUS_OK)
        {
            break;
        }

        // calc time before start stream
        ec = time_sync ();
        if (ec != (int)BrainFlowExitCodes::STATUS_OK)
        {
            break;
        }

        initialized = true;

        ec = default_config ();

        break;
    } while (true);

    if (ec != (int)BrainFlowExitCodes::STATUS_OK)
    {
        if (serial)
        {
            delete serial;
            serial = NULL;
        }
        initialized = false;
    }

    return ec;
}

int DawnEEG::start_stream (int buffer_size, const char *streamer_params)
{
    if (!initialized)
    {
        LOG_F (ERROR, "You need to call prepare_session before config_board");
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }
    if (is_streaming)
    {
        LOG_F (ERROR, "Streaming thread already running");
        return (int)BrainFlowExitCodes::STREAM_ALREADY_RUN_ERROR;
    }

    int ec = prepare_for_acquisition (buffer_size, streamer_params);
    if (ec != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return ec;
    }

    // start streaming
    ec = send (DAWNEEG_CMD_START_STREAM);
    if (ec != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return ec;
    }
    keep_alive = true;
    streaming_thread = std::thread ([this] { this->read_thread (); });
    // wait for data to ensure that everything is okay
    std::unique_lock<std::mutex> lk (this->m);
    auto sec = std::chrono::seconds (1);
    if (cv.wait_for (lk, 3 * sec,
            [this] { return this->state != (int)BrainFlowExitCodes::SYNC_TIMEOUT_ERROR; }))
    {
        this->is_streaming = true;
        return this->state;
    }
    else
    {
        LOG_F (ERROR, "No data received in 3sec, stopping thread");
        this->is_streaming = true;
        this->stop_stream ();
        return (int)BrainFlowExitCodes::SYNC_TIMEOUT_ERROR;
    }
}

int DawnEEG::stop_stream ()
{
    if (is_streaming)
    {
        keep_alive = false;
        is_streaming = false;
        if (streaming_thread.joinable ())
        {
            streaming_thread.join ();
        }
        this->state = (int)BrainFlowExitCodes::SYNC_TIMEOUT_ERROR;

        int ec;
        ec = send (DAWNEEG_CMD_STOP_STREAM);
        if (ec != (int)BrainFlowExitCodes::STATUS_OK)
        {
            return ec;
        }

        // free kernel buffer
        std::string response;

        int max_attempt = 10 * 1000 / params.timeout; // max 10s to dont get to infinite loop
        int current_attempt = 0;
        while (1)
        {
            ec = recv (response);
            if (response.size () == 0)
            {
                break;
            }
            current_attempt++;
            if (current_attempt == max_attempt)
            {
                LOG_F (ERROR, "Command 's' was sent but streaming is still running.");
                return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
            }
        }

        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    else
    {
        return (int)BrainFlowExitCodes::STREAM_THREAD_IS_NOT_RUNNING;
    }
}

int DawnEEG::release_session ()
{
    if (initialized)
    {
        if (is_streaming)
        {
            stop_stream ();
        }
        free_packages ();
        initialized = false;
        if (serial)
        {
            delete serial;
            serial = NULL;
        }
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int DawnEEG::config_board (std::string config, std::string &response)
{
    int ec;

    if (serial == NULL)
    {
        LOG_F (ERROR, "You need to call prepare_session before config_board");
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }

    if (config_tracker.apply_config (config) == (int)DawnEEG_CommandTypes::INVALID_COMMAND)
    {
        LOG_F (WARNING, "Invalid command: {}", config.c_str ());
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }

    if (!initialized)
    {
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }

    do
    {
        LOG_F (INFO, "Config board: \"{}\"", config);
        ec = send (config);
        if (ec != (int)BrainFlowExitCodes::STATUS_OK)
        {
            break;
        }

        if (is_streaming)
        {
            LOG_F (WARNING,
                "You are changing board params during streaming, it may lead to sync mismatch "
                "between data acquisition thread and device");
        }
        else
        {
            // read response if streaming is not running
            ec = recv (response);
            if (ec != (int)BrainFlowExitCodes::STATUS_OK ||
                response.substr (0, 7).compare ("Failure") == 0)
            {
                LOG_F (ERROR, "Board config '{}' error", config.c_str ());
                LOG_F (ERROR, "Config response:\r\n{}", response.c_str ());
            }
        }

        break;
    } while (1);

    if (ec != (int)BrainFlowExitCodes::STATUS_OK)
    {
        config_tracker.revert_config ();
    }
    return ec;
}

int DawnEEG::open_port ()
{
    BaudRate baudrate;

    switch (params.serial_baudrate)
    {
        case 0:
            baudrate = DAWNEEG_DEFAULT_BAUDRATE;
            break;
        case 115200:
            baudrate = BaudRate::BAUD_115200;
            break;
        case 230400:
            baudrate = BaudRate::BAUD_230400;
            break;
        case 460800:
            baudrate = BaudRate::BAUD_460800;
            break;
        case 921600:
            baudrate = BaudRate::BAUD_921600;
            break;
        case 1000000:
            baudrate = BaudRate::BAUD_1000000;
            break;
        case 2000000:
            baudrate = BaudRate::BAUD_2000000;
            break;
        case 4000000:
            baudrate = BaudRate::BAUD_4000000;
            break;
        default:
            baudrate = BaudRate::BAUD_INVALID;
            break;
    }

    if (baudrate == BaudRate::BAUD_INVALID)
    {
        LOG_F (ERROR, "Invalid baud rate {}", params.serial_baudrate);
        return (int)BrainFlowExitCodes::SET_PORT_ERROR;
    }

    LOG_F (INFO, "Set baud rate to {}", params.serial_baudrate);
    LOG_F (INFO, "Opening port {}", params.serial_port.c_str ());

    try
    {
        if (serial)
        {
            if (serial->IsOpen ())
            {
                serial->Close ();
            }
            delete serial;
        }

        // create serial object
        serial = new SerialPort (params.serial_port.c_str (), baudrate, CharacterSize::CHAR_SIZE_8,
            FlowControl::FLOW_CONTROL_HARDWARE, Parity::PARITY_NONE, StopBits::STOP_BITS_1);
    }
    catch (...)
    {
        LOG_F (ERROR,
            "Make sure you provided correct port name and have permissions to open it(run with "
            "sudo/admin). Also, close all other apps using this port.");
        return (int)BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR;
    }

    LOG_F (INFO, "Port {} opened", params.serial_port.c_str ());
    return (int)BrainFlowExitCodes::STATUS_OK;
}


int DawnEEG::init_board ()
{
    int result = (int)BrainFlowExitCodes::STATUS_OK;

    result = reset_RTS ();
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;

    result = soft_reset ();
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;

    return (int)BrainFlowExitCodes::STATUS_OK;
}

int DawnEEG::soft_reset ()
{
    std::string response;
    int result;

    // In case brainflow crashes while the board is still streaming:
    // Stop streaming, pause, and flush buffer
    LOG_F (INFO, "Stop stream");
    result = send (DAWNEEG_CMD_STOP_STREAM);
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;

    result = recv (response);
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;

    LOG_F (INFO, "Reset board");
    result = send (DAWNEEG_CMD_SOFT_RESET);
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;

    result = recv (response);
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;

    if (response.find (DAWNEEG_CMD_PROMPT) != std::string::npos)
    {
        switch (board_id)
        {
            case (int)BoardIds::DAWNEEG4_BOARD:
                if (response.find ("DawnEEG4") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG6_BOARD:
                if (response.find ("DawnEEG6") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG8_BOARD:
                if (response.find ("DawnEEG8") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG12_BOARD:
                if (response.find ("DawnEEG12") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG16_BOARD:
                if (response.find ("DawnEEG16") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG18_BOARD:
                if (response.find ("DawnEEG18") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG24_BOARD:
                if (response.find ("DawnEEG24") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG32_BOARD:
                if (response.find ("DawnEEG32") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            default:
                return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
        }

        LOG_F (INFO, "Detected board: {}", board_descr["default"]["name"].dump ());

        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    else
    {
        LOG_F (ERROR, "Board doesnt send welcome characters! Msg:\r\n{}", response.c_str ());
        return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
    }
}

// set default settings
int DawnEEG::default_config ()
{
    std::string response;

    LOG_F (INFO, "Set channels to default");

    int ec = config_board (DAWNEEG_CMD_DEFAULT, response);
    return ec;
}


int DawnEEG::time_sync ()
{
    std::string b;
    int result;

    LOG_F (INFO, "Time sync");

    for (int i = 0; i < 20; i++)
    {
        double T1 = get_timestamp ();

        LOG_F (1, "Sending time calc command to device");

        result = send ("<123456123456<");
        if (result != (int)BrainFlowExitCodes::STATUS_OK)
        {
            LOG_F (WARNING, "Failed to send time calc command to device");
            return result;
        }

        result = recv (b);
        double T4 = get_timestamp ();
        if (result != (int)BrainFlowExitCodes::STATUS_OK)
        {
            LOG_F (WARNING, "Failed to recv resp from time calc command");
            return result;
        }

        if (b.size () != 14 || b[0] != DAWNEEG_CHAR_TIME_SYNC_RESPONSE ||
            b[13] != DAWNEEG_CHAR_TIME_SYNC_RESPONSE)
        {
            LOG_F (WARNING, "Incorrect time calc response received");
            return (int)BrainFlowExitCodes::INCOMMING_MSG_ERROR;
        }

        double T2 = (double)(((b[3] << 24) + (b[4] << 16) + (b[5] << 8) + b[6])) / 1000 +
            (double)(((b[1] & 0x03) << 8) + b[2]) / 1000000;
        double T3 = (double)(((b[9] << 24) + (b[10] << 16) + (b[11] << 8) + b[12])) / 1000 +
            (double)(((b[7] & 0x03) << 8) + b[8]) / 1000000;
        LOG_F (2, "T1 {:.6f} T2 {:.6f} T3 {:.6f} T4 {:.6f}", T1, T2, T3, T4);

        double duration = (T4 - T1) - (T3 - T2);

        LOG_F (2,
            "host_timestamp {:.6f} device_timestamp {:.6f} half_rtt {:.6f} time_correction {:.6f}",
            (T4 + T1) / 2, (T3 + T2) / 2, duration / 2, ((T4 + T1) - (T3 + T2)) / 2);

        if (half_rtt > duration / 2)
        {
            half_rtt = duration / 2; // get minimal half-rtt
            time_correction = ((T4 + T1) - (T3 + T2)) / 2;
            LOG_F (1, "Updated: half_rtt = {:.6f}, time_correction = {:.6f}", half_rtt,
                time_correction);
        }
    }

    LOG_F (INFO, "half_rtt = {:.6f}, time_correction = {:.6f}", half_rtt, time_correction);

    return (int)BrainFlowExitCodes::STATUS_OK;
}

#define NUM_HEADER_BYTES 1
#define NUM_SAMPLE_NUMBER_BYTES 1
#define NUM_DATA_BYTES_PER_CHANNEL 3
#define NUM_AUX_BYTES 7
#define NUM_FOOTER_BYTES 1
void DawnEEG::read_thread ()
{
    /*  DawnEEG8
        Byte 1: 0xA0
        Byte 2: Sample Number
        Bytes 3-5: Data value for EEG channel 1
        Bytes 6-8: Data value for EEG channel 2
        Bytes 9-11: Data value for EEG channel 3
        Bytes 12-14: Data value for EEG channel 4
        Bytes 15-17: Data value for EEG channel 5
        Bytes 18-20: Data value for EEG channel 6
        Bytes 21-23: Data value for EEG channel 6
        Bytes 24-26: Data value for EEG channel 8
        Aux Data Bytes 27-33: 7 bytes of data
        Byte 34: 0xC0
    */
    int result;
    int num_eeg_channels = board_descr["default"]["num_eeg_channels"];
    std::vector<int> eeg_channels = board_descr["default"]["eeg_channels"];
    int package_num_channel = board_descr["default"]["package_num_channel"].get<int> ();
    int package_num_channel_aux = board_descr["auxiliary"]["package_num_channel"].get<int> ();
    int timestamp_channel = board_descr["default"]["timestamp_channel"].get<int> ();
    int timestamp_channel_aux = board_descr["auxiliary"]["timestamp_channel"].get<int> ();
    int marker_channel = board_descr["default"]["marker_channel"].get<int> ();
    int marker_channel_aux = board_descr["auxiliary"]["marker_channel"].get<int> ();
    int trigger1_channel = board_descr["default"]["trigger1_channel"].get<int> ();
    int trigger2_channel = board_descr["default"]["trigger2_channel"].get<int> ();
    std::vector<int> temperature_channels = board_descr["auxiliary"]["temperature_channels"];
    int battery_channel = board_descr["auxiliary"]["battery_channel"].get<int> ();

    int buf_length = NUM_SAMPLE_NUMBER_BYTES + NUM_DATA_BYTES_PER_CHANNEL * num_eeg_channels +
        NUM_AUX_BYTES + NUM_FOOTER_BYTES;
    // unsigned char *buf = new unsigned char[buf_length];
    LibSerial::DataBuffer buf (buf_length);

    int num_rows = board_descr["default"]["num_rows"];
    int num_rows_aux = board_descr["auxiliary"]["num_rows"];

    double *package = new double[num_rows];
    double *package_aux = new double[num_rows_aux];

    for (int i = 0; i < num_rows; i++)
    {
        package[i] = 0.0;
    }
    for (int i = 0; i < num_rows_aux; i++)
    {
        package_aux[i] = 0.0;
    }

    while (keep_alive)
    {
        unsigned char header;
        try
        {
            result = (int)BrainFlowExitCodes::STATUS_OK;
            // check start byte
            serial->ReadByte (header, params.timeout);
        }
        catch (...)
        {
            LOG_F (1, "Unable to read package header");
            result = (int)BrainFlowExitCodes::INCOMMING_MSG_ERROR;
            break;
        }

        if (header != DAWNEEG_STREAM_HEADER)
        {
            LOG_F (ERROR, "Wrong header: '{:c}'({:#X})", header, header);
            continue;
        }

        try
        {
            serial->Read (buf, buf_length, params.timeout);
        }
        catch (...)
        {
            LOG_F (1, "Unable to read package");
            result = (int)BrainFlowExitCodes::INCOMMING_MSG_ERROR;
            break;
        }

        if (!keep_alive)
        {
            break;
        }

        if ((buf[buf_length - 1] != DAWNEEG_STREAM_FOOTER))
        {
            LOG_F (WARNING, "Wrong end byte {}", buf[buf_length - 1]);
            continue;
        }

        if (this->state != (int)BrainFlowExitCodes::STATUS_OK)
        {
            LOG_F (INFO, "Received first package, streaming is started");
            {
                std::lock_guard<std::mutex> lk (this->m);
                this->state = (int)BrainFlowExitCodes::STATUS_OK;
            }
            this->cv.notify_one ();
        }

        // package num
        int package_num = buf[0];
        package[package_num_channel] = (double)package_num;

        // eeg
        for (unsigned int i = 0; i < eeg_channels.size (); i++)
        {
            double eeg_scale = (double)(4.5 / float ((pow (2, 23) - 1)) /
                config_tracker.get_gain_for_channel (i) * 1000000.);
            package[eeg_channels[i]] = eeg_scale * cast_24bit_to_int32 (&buf[1 + 3 * i]);
        }

        // timestamp
        double device_timestamp = ((buf[buf_length - 5] << 24) | (buf[buf_length - 4] << 16) |
                                      (buf[buf_length - 3] << 8) | (buf[buf_length - 2])) /
                1000.0 // millisecond part
            + (((buf[buf_length - 7] & 0x03) << 8) | (buf[buf_length - 6])) /
                1000000.0; // microsecond part
        package[timestamp_channel] = device_timestamp + time_correction;

        // marker & triggers
        package[marker_channel] = (buf[buf_length - 7] >> 4) & 0x0F;
        package[trigger1_channel] = (buf[buf_length - 7] >> 2) & 0x01;
        package[trigger1_channel] = (buf[buf_length - 7] >> 3) & 0x01;

        push_package (package);

        switch (package_num & 0x07)
        {
            case 0x00:
                package_num_aux = (double)(package_num >> 3);
                battery_temperature = 0.0;
                battery_voltage = 0.0;
                package_aux[package_num_channel_aux] = package_num_aux;
                package_aux[timestamp_channel_aux] = device_timestamp + time_correction;
                package_aux[marker_channel_aux] = (buf[buf_length - 7] >> 4) & 0x0F;
                battery_temperature = buf[buf_length - 8] << 8; // temperature MSB
                break;
            case 0x01:
                battery_temperature += buf[buf_length - 8]; // temperature LSB
                package_aux[temperature_channels[0]] = battery_temperature;
                break;
            case 0x02:
                battery_voltage = buf[buf_length - 8] << 8; // voltage MSB
                break;
            case 0x03:
                battery_voltage += buf[buf_length - 8]; // voltage LSB
                package_aux[battery_channel] = battery_voltage / 1000.0;
                push_package (package_aux, (int)BrainFlowPresets::AUXILIARY_PRESET);
                break;
            case 0x04:
                break;
        }
    }
    delete[] package;
    delete[] package_aux;
    LOG_F (1, "Stop streaming");
}

int DawnEEG::send (const std::string &msg)
{
    std::size_t length = msg.length ();
    LOG_F (1, "Sending to board: \"{}\"", msg.c_str ());
    try
    {
        serial->Write (msg);
    }
    catch (...)
    {
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int DawnEEG::recv (std::string &response)
{
    std::string data;
    response = "";

    try
    {
        // read first character, if time out, throw ReadTimeout exception
        serial->Read (data, 1, params.timeout);
    }
    catch (ReadTimeout)
    {
        LOG_F (1, "Board response: <NULL>");
        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    catch (...)
    {
        int ec = (int)BrainFlowExitCodes::INCOMMING_MSG_ERROR;
        return ec;
    }

    response = data;

    do
    {
        try
        {
            // read remaining characters， set timeout to 1ms
            serial->Read (data, 0, 1);
        }
        catch (ReadTimeout)
        {
            response += data;
        }
        catch (...)
        {
            int ec = (int)BrainFlowExitCodes::INCOMMING_MSG_ERROR;
            return ec;
        }
    } while (serial->GetNumberOfBytesAvailable () > 0);

    LOG_F (1, "Board response: \"{}\"", response.c_str ());
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int DawnEEG::send_receive (const std::string &msg, std::string &response)
{
    int ret;
    ret = send (msg);
    if (ret != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return ret;
    }

    ret = recv (response);
    return ret;
}

int DawnEEG::reset_RTS ()
{
    try
    {
        msleep (10);
        serial->SetRTS (false);
        msleep (10);
        serial->SetRTS (true);
        msleep (10);
        serial->FlushInputBuffer ();
        msleep (10);
    }
    catch (...)
    {
        int ec = (int)BrainFlowExitCodes::SET_PORT_ERROR;
        return ec;
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int DawnEEG::flush ()
{
    try
    {
        serial->FlushInputBuffer ();
        msleep (10);
    }
    catch (...)
    {
        int ec = (int)BrainFlowExitCodes::SET_PORT_ERROR;
        return ec;
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

DawnEEG4::DawnEEG4 (struct BrainFlowInputParams params)
    : DawnEEG ((int)BoardIds::DAWNEEG4_BOARD, params)
{
}

DawnEEG6::DawnEEG6 (struct BrainFlowInputParams params)
    : DawnEEG ((int)BoardIds::DAWNEEG6_BOARD, params)
{
}

DawnEEG8::DawnEEG8 (struct BrainFlowInputParams params)
    : DawnEEG ((int)BoardIds::DAWNEEG8_BOARD, params)
{
}

DawnEEG12::DawnEEG12 (struct BrainFlowInputParams params)
    : DawnEEG ((int)BoardIds::DAWNEEG12_BOARD, params)
{
}

DawnEEG16::DawnEEG16 (struct BrainFlowInputParams params)
    : DawnEEG ((int)BoardIds::DAWNEEG16_BOARD, params)
{
}

DawnEEG18::DawnEEG18 (struct BrainFlowInputParams params)
    : DawnEEG ((int)BoardIds::DAWNEEG18_BOARD, params)
{
}

DawnEEG24::DawnEEG24 (struct BrainFlowInputParams params)
    : DawnEEG ((int)BoardIds::DAWNEEG24_BOARD, params)
{
}

DawnEEG32::DawnEEG32 (struct BrainFlowInputParams params)
    : DawnEEG ((int)BoardIds::DAWNEEG32_BOARD, params)
{
}