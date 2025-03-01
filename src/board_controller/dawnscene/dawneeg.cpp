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

DawnEEG::DawnEEG (int board_id, struct BrainFlowInputParams params)
    : Board (board_id, params)
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
        LOG_F(INFO, "Session is already prepared");
        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    if (params.serial_port.empty ())
    {
        LOG_F(ERROR, "Serial port is not specified.");
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }
    if ((params.timeout > 6000) || (params.timeout < 1))
    {
        params.timeout = 100;
    }

    int ec = (int)BrainFlowExitCodes::STATUS_OK;

    do
    {
        // create serial object
        serial = Serial::create (params.serial_port.c_str (), this);

        if (serial->is_port_open ())
        {
            LOG_F(ERROR, "Port {} already open", serial->get_port_name ());
            ec = (int)BrainFlowExitCodes::PORT_ALREADY_OPEN_ERROR;
            break;
        }

        LOG_F(INFO, "Opening port {}", serial->get_port_name ());

        int result = serial->open_serial_port ();
        if (result < 0)
        {
            LOG_F(ERROR,
                "Make sure you provided correct port name and have permissions to open it(run with "
                "sudo/admin). Also, close all other apps using this port.");
            ec = (int)BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR;
            break;
        }

        LOG_F(2, "Port {} is open", serial->get_port_name ());

        result = serial->set_serial_port_settings (params.timeout, false); // timeout in milliseconds
        if (result < 0)
        {
            LOG_F(ERROR, "Unable to set port settings, result is {}", result);
            ec =  (int)BrainFlowExitCodes::SET_PORT_ERROR;
            break;
        }

        result = serial->set_custom_baudrate (DAWNEEG_BAUDRATE);
        if (result < 0)
        {
            LOG_F(ERROR, "Unable to set custom baud rate, result is {}", result);
            ec =   (int)BrainFlowExitCodes::SET_PORT_ERROR;
            break;
        }

        LOG_F(2, "Set custom baud rate to {}", DAWNEEG_BAUDRATE);

        // set initial settings
        ec = init_board ();
        if (ec != (int)BrainFlowExitCodes::STATUS_OK)
        {
            break;
        }

        // calc time before start stream
        for (int i = 0; i < 20; i++)
        {
            ec = time_sync ();
            if (ec != (int)BrainFlowExitCodes::STATUS_OK)
            {
                break;
            }
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
        LOG_F(ERROR, "You need to call prepare_session before config_board");
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }
    if (is_streaming)
    {
        LOG_F(ERROR, "Streaming thread already running");
        return (int)BrainFlowExitCodes::STREAM_ALREADY_RUN_ERROR;
    }

    int ec = prepare_for_acquisition (buffer_size, streamer_params);
    if (ec != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return ec;
    }

    // start streaming
    ec = send_to_board (DAWNEEG_CMD_START_STREAM);
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
        LOG_F(ERROR, "No data received in 3sec, stopping thread");
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
        ec = send_to_board (DAWNEEG_CMD_STOP_STREAM);
        if (ec != (int)BrainFlowExitCodes::STATUS_OK)
        {
            return ec;
        } 

        // free kernel buffer
        unsigned char b;
        int res = 1;
        int max_attempt = 400000; // to dont get to infinite loop
        int current_attempt = 0;
        while (res == 1)
        {
            res = serial->read_from_serial_port (&b, 1);
            current_attempt++;
            if (current_attempt == max_attempt)
            {
                LOG_F(ERROR, "Command 's' was sent but streaming is still running.");
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
        LOG_F(ERROR, "You need to call prepare_session before config_board");
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }

    if (config_tracker.apply_config (config) == (int)DawnEEG_CommandTypes::INVALID_COMMAND)
    {
        LOG_F(WARNING, "Invalid command: {}", config.c_str ());
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }

    if (!initialized)
    {
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }

    ec = (int)BrainFlowExitCodes::STATUS_OK;
    if (is_streaming)
    {
        LOG_F(WARNING,
            "You are changing board params during streaming, it may lead to sync mismatch between "
            "data acquisition thread and device");
        ec = send_to_board (config.c_str ());
    }
    else
    {
        // read response if streaming is not running
        ec = send_to_board (config.c_str (), response);
    }

    if (ec != (int)BrainFlowExitCodes::STATUS_OK)
    {
        config_tracker.revert_config ();
    }
    return ec;
}

int DawnEEG::init_board ()
{
    int result;

    // In case brainflow crashes while the board is still streaming:
    // Stop streaming, pause, and flush buffer
    result = send_to_board (DAWNEEG_CMD_STOP_STREAM);
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;
#ifdef _WIN32
    Sleep (1000);
#else
    sleep (1);
#endif
    serial->flush_buffer();

    result = soft_reset();
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;

    return (int)BrainFlowExitCodes::STATUS_OK;
}

int DawnEEG::soft_reset() {
    std::string response = "";
    int result;


    result = send_to_board (DAWNEEG_CMD_SOFT_RESET, response);
    if (result != (int)BrainFlowExitCodes::STATUS_OK)
        return result;

    if (response.find(DAWNEEG_CMD_PROMPT, 9) != std::string::npos)
    {
        switch (board_id) {
            case (int)BoardIds::DAWNEEG4_BOARD:
                if (response.find("DawnEEG4") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG6_BOARD:
                if (response.find("DawnEEG6") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG8_BOARD:
                if (response.find("DawnEEG8") == std::string::npos && response.find("DawnEEG") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG12_BOARD:
                if (response.find("DawnEEG12") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG16_BOARD:
                if (response.find("DawnEEG16") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG18_BOARD:
                if (response.find("DawnEEG18") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG24_BOARD:
                if (response.find("DawnEEG24") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            case (int)BoardIds::DAWNEEG32_BOARD:
                if (response.find("DawnEEG32") == std::string::npos)
                    return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
                break;
            default:
                return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
        }

        LOG_F(INFO, "Board detected: {}", board_descr["default"]["name"].dump());

        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    else
    {
        LOG_F(ERROR, "board doesnt send welcome characters! Msg: {}",
            response.c_str ());
        return (int)BrainFlowExitCodes::INITIAL_MSG_ERROR;
    }
}

// set default settings
int DawnEEG::default_config () {
    int ec;
    std::string response;
    ec = config_board (DAWNEEG_CMD_DEFAULT, response);
    if (ec != (int)BrainFlowExitCodes::STATUS_OK || response.substr (0, 7).compare ("Failure") == 0)
    {
        LOG_F(ERROR, "Board config ec.");
        LOG_F(2, "Read {}", response.c_str ());
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }
    return ec;
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
    int buf_length = NUM_SAMPLE_NUMBER_BYTES + NUM_DATA_BYTES_PER_CHANNEL * num_eeg_channels + NUM_AUX_BYTES + NUM_FOOTER_BYTES;
    unsigned char *buf = new unsigned char[buf_length];

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

    std::vector<int> eeg_channels = board_descr["default"]["eeg_channels"];
    std::vector<int> temperature_channels = board_descr["auxiliary"]["temperature_channels"];

    while (keep_alive)
    {
       // check start byte
        result = serial->read_from_serial_port (buf, 1);
        if (result != 1)
        {
            LOG_F(1, "unable to read 1 byte");
            continue;
        }
        if (buf[0] != DAWNEEG_STREAM_HEADER)
        {
            continue;
        }

        int remaining_bytes = buf_length;
        int pos = 0;
        while ((remaining_bytes > 0) && (keep_alive))
        {
            result = serial->read_from_serial_port (buf + pos, remaining_bytes);
            remaining_bytes -= result;
            pos += result;
        }
        if (!keep_alive)
        {
            break;
        }

        if ((buf[buf_length-1] != DAWNEEG_STREAM_FOOTER))
        {
            LOG_F(WARNING, "Wrong end byte {}", buf[buf_length-1]);
            continue;
        }

        if (this->state != (int)BrainFlowExitCodes::STATUS_OK)
        {
            LOG_F(INFO, "Received first package, streaming is started");
            {
                std::lock_guard<std::mutex> lk (this->m);
                this->state = (int)BrainFlowExitCodes::STATUS_OK;
            }
            this->cv.notify_one ();
            LOG_F(1, "Start streaming");
        }

        // package num
        int package_num = buf[0];
        package[board_descr["default"]["package_num_channel"].get<int> ()] = (double)package_num;

        // eeg
        for (unsigned int i = 0; i < eeg_channels.size (); i++)
        {
            double eeg_scale = (double)(4.5 / float ((pow (2, 23) - 1)) /
                config_tracker.get_gain_for_channel (i) * 1000000.);
            package[eeg_channels[i]] = eeg_scale * cast_24bit_to_int32 (buf + 1 + 3 * i);
        }

        // timestamp
        double device_timestamp = 
            ((buf[buf_length - 5] << 24) | (buf[buf_length - 4] << 16) | (buf[buf_length - 3] << 8) | (buf[buf_length - 2])) / 1000.0   // millisecond part
            + (((buf[buf_length - 7] & 0x03) << 8) | (buf[buf_length - 6])) / 1000000.0;  // microsecond part
        package[board_descr["default"]["timestamp_channel"].get<int> ()] = device_timestamp + time_correction;

        // marker & triggers
        package[board_descr["default"]["marker_channel"].get<int> ()] =
            (buf[buf_length - 7] >> 4) & 0x0F;
        package[board_descr["default"]["trigger1_channel"].get<int> ()] =
            (buf[buf_length - 7] >> 2) & 0x01;
        package[board_descr["default"]["trigger2_channel"].get<int> ()] =
            (buf[buf_length - 7] >> 3) & 0x01;

        push_package (package);

        if (package_num % 8 == 0)
        {
            package_num_aux = (double)(package_num / 8);
            battery_temperature = 0.0;
            battery_voltage = 0.0;
            package_aux[board_descr["auxiliary"]["package_num_channel"].get<int> ()] =
                package_num_aux;
            package_aux[board_descr["auxiliary"]["timestamp_channel"].get<int> ()] =
                device_timestamp + time_correction;
            package_aux[board_descr["auxiliary"]["marker_channel"].get<int> ()] =
                (buf[buf_length - 7] >> 4) & 0x0F;
            battery_temperature = buf[buf_length - 8] * 256; // temperature MSB
        }
        else if (package_num % 8 == 1)
        {
            battery_temperature += buf[buf_length - 8]; // temperature LSB
            package_aux[temperature_channels[0]] = battery_temperature;
        }
        else if (package_num % 8 == 2)
        {
            battery_voltage = buf[buf_length - 8] * 256; // voltage MSB
        }
        else if (package_num % 8 == 3)
        {
            battery_voltage += buf[buf_length - 8]; // voltage LSB
            package_aux[board_descr["auxiliary"]["battery_channel"].get<int> ()] =
                battery_voltage / 1000.0;
            push_package (package_aux, (int)BrainFlowPresets::AUXILIARY_PRESET);
        }
    }
    delete[] package;
    delete[] buf;
    LOG_F(1, "Stop streaming");
}

int DawnEEG::send_to_board (const char *msg)
{
    int length = (int)strlen (msg);
    LOG_F(1, "Sending {} to the board", msg);
    int result = serial->send_to_serial_port ((const void *)msg, length);
    if (result != length)
    {
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    return (int)BrainFlowExitCodes::STATUS_OK;
}

int DawnEEG::send_to_board (const char *msg, std::string &response)
{
    int length = (int)strlen (msg);
    LOG_F(1, "Sending {} to the board", msg);
    int result = serial->send_to_serial_port ((const void *)msg, length);
    if (result != length)
    {
        response = "";
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }
    response = read_serial_response ();
    LOG_F(1, "Board response: {}", response);

    return (int)BrainFlowExitCodes::STATUS_OK;
}

std::string DawnEEG::read_serial_response ()
{
    constexpr int max_tmp_size = 4096;
    unsigned char tmp_array[max_tmp_size];
    unsigned char tmp;
    int tmp_id = 0;
    while (serial->read_from_serial_port (&tmp, 1) == 1) // read 1 byte from serial port
    {
        if (tmp_id == 0)
                serial->set_serial_port_settings (100, true);
        if (tmp_id < max_tmp_size)
        {
            tmp_array[tmp_id] = tmp;
            tmp_id++;
        }
        else
        {
            serial->flush_buffer ();
            break;
        }
    }
    tmp_id = (tmp_id == max_tmp_size) ? tmp_id - 1 : tmp_id;
    tmp_array[tmp_id] = '\0';

    serial->set_serial_port_settings (params.timeout, true);
    return std::string ((const char *)tmp_array);
}

int DawnEEG::time_sync ()
{
    constexpr int bytes_to_calc_rtt = 14;
    std::string time_response;
    uint8_t b[bytes_to_calc_rtt];

    double T1 = get_timestamp ();

    int res = serial->send_to_serial_port ("<123456123456<", bytes_to_calc_rtt);
    serial->flush_buffer();
    LOG_F(2, "Sending time calc command to device");

    if (res != bytes_to_calc_rtt)
    {
        LOG_F(WARNING, "Failed to send time calc command to device");
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    unsigned char tmp;
    int ptr = -1;
    while (serial->read_from_serial_port (&tmp, 1) == 1) // read 1 byte from serial port
    {
        if (ptr < bytes_to_calc_rtt)
        {
            ptr++;
            b[ptr] = tmp;
        }
        if (ptr >= bytes_to_calc_rtt - 1)
        {
            serial->flush_buffer ();
            break;
        }
    }
    res = ptr+1;

    double T4 = get_timestamp ();
    if (res != bytes_to_calc_rtt)
    {
        LOG_F(WARNING, "Failed to recv resp from time calc command, resp size {}", res);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }
    if (b[0] != DAWNEEG_CHAR_TIME_SYNC_RESPONSE || b[bytes_to_calc_rtt - 1] != DAWNEEG_CHAR_TIME_SYNC_RESPONSE)
    {
        LOG_F(WARNING, "Incorrect time calc response received");
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    double T2 = (double)(((b[3]<<24) + (b[4]<<16) + (b[5]<<8) + b[6])) / 1000 + (double)(((b[1] & 0x03)<<8) + b[2])/1000000;
    double T3 = (double)(((b[9]<<24) + (b[10]<<16) + (b[11]<<8) + b[12])) / 1000 + (double)(((b[7] & 0x03)<<8) + b[8])/1000000;
    LOG_F(2, "T1 {:.6f} T2 {:.6f} T3 {:.6f} T4 {:.6f}", T1, T2, T3, T4);

    double duration = (T4 - T1) - (T3 - T2);

    LOG_F(2, "host_timestamp {:.6f} device_timestamp {:.6f} half_rtt {:.6f} time_correction {:.6f}", (T4 + T1) / 2, (T3 + T2) / 2, duration / 2, ((T4 + T1) - (T3 + T2))/2);

    if (half_rtt > duration / 2)
    {
        half_rtt = duration / 2;    // get minimal half-rtt
        time_correction = ((T4 + T1) - (T3 + T2))/2;
        LOG_F(2, "Updated: half_rtt = {:.6f}, time_correction = {:.6f}", half_rtt, time_correction);
    }

    return (int)BrainFlowExitCodes::STATUS_OK;
}

DawnEEG4::DawnEEG4 (struct BrainFlowInputParams params) : DawnEEG ((int)BoardIds::DAWNEEG4_BOARD, params) {
}

DawnEEG6::DawnEEG6 (struct BrainFlowInputParams params) : DawnEEG ((int)BoardIds::DAWNEEG6_BOARD, params) {
}

DawnEEG8::DawnEEG8 (struct BrainFlowInputParams params) : DawnEEG ((int)BoardIds::DAWNEEG8_BOARD, params) {
}

DawnEEG12::DawnEEG12 (struct BrainFlowInputParams params) : DawnEEG ((int)BoardIds::DAWNEEG12_BOARD, params) {
}

DawnEEG16::DawnEEG16 (struct BrainFlowInputParams params) : DawnEEG ((int)BoardIds::DAWNEEG16_BOARD, params) {
}

DawnEEG18::DawnEEG18 (struct BrainFlowInputParams params) : DawnEEG ((int)BoardIds::DAWNEEG18_BOARD, params) {
}

DawnEEG24::DawnEEG24 (struct BrainFlowInputParams params) : DawnEEG ((int)BoardIds::DAWNEEG24_BOARD, params) {
}

DawnEEG32::DawnEEG32 (struct BrainFlowInputParams params) : DawnEEG ((int)BoardIds::DAWNEEG32_BOARD, params) {
}