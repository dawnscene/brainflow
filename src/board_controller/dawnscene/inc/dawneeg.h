#pragma once

#include <condition_variable>
#include <math.h>
#include <mutex>
#include <string>
#include <thread>

#include "board.h"
#include "board_controller.h"
#include "dawneeg_config_tracker.h"
#include <SerialPort.h>


#ifdef _WIN32
#define sleep(sec) Sleep (sec * 1000)
#define msleep(msec) Sleep (msec)
#else
#define msleep(msec) usleep (msec * 1000)
#endif

using namespace LibSerial;

/*
enum DAWNEEG_BOARD_CHANNELS {
    DAWNEEG_UNKNOWN = 0,
    DAWNEEG4 = 4,
    DAWNEEG6 = 6,
    DAWNEEG8 = 8,
    DAWNEEG16 = 16,
    DAWNEEG24 = 24,
    DAWNEEG32 = 32
}; */

#ifndef DAWNEEG_DEFAULT_BAUDRATE
#define DAWNEEG_DEFAULT_BAUDRATE BaudRate::BAUD_2000000
#endif

class DawnEEG : public Board
{

protected:
    volatile bool keep_alive;
    bool initialized;
    bool is_streaming;
    int board_type;

    std::thread streaming_thread;
    SerialPort *serial;
    DawnEEG_ConfigTracker config_tracker;
    std::mutex m;
    std::condition_variable cv;
    volatile int state;
    volatile double half_rtt;
    volatile double time_correction;

    volatile double package_num_aux;
    volatile double battery_voltage;
    volatile double battery_temperature;

    int open_port ();
    int init_board ();
    int soft_reset ();
    int default_config ();
    int time_sync ();

    void read_thread ();

    int send (const std::string &msg);
    int recv (std::string &response);
    int send_receive (const std::string &msg, std::string &response);
    int reset_RTS ();
    int flush ();

public:
    DawnEEG (int board_id, struct BrainFlowInputParams params);
    ~DawnEEG ();

    int prepare_session ();
    int start_stream (int buffer_size, const char *streamer_params);
    int stop_stream ();
    int release_session ();
    int config_board (std::string config, std::string &response);
};

class DawnEEG4 : public DawnEEG
{
public:
    DawnEEG4 (struct BrainFlowInputParams params);
};

class DawnEEG6 : public DawnEEG
{
public:
    DawnEEG6 (struct BrainFlowInputParams params);
};

class DawnEEG8 : public DawnEEG
{
public:
    DawnEEG8 (struct BrainFlowInputParams params);
};

class DawnEEG12 : public DawnEEG
{
public:
    DawnEEG12 (struct BrainFlowInputParams params);
};

class DawnEEG16 : public DawnEEG
{
public:
    DawnEEG16 (struct BrainFlowInputParams params);
};

class DawnEEG18 : public DawnEEG
{
public:
    DawnEEG18 (struct BrainFlowInputParams params);
};

class DawnEEG24 : public DawnEEG
{
public:
    DawnEEG24 (struct BrainFlowInputParams params);
};

class DawnEEG32 : public DawnEEG
{
public:
    DawnEEG32 (struct BrainFlowInputParams params);
};