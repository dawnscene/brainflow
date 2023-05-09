#pragma once

#include <condition_variable>
#include <math.h>
#include <mutex>
#include <string>
#include <thread>

#include "board.h"
#include "board_controller.h"
#include "serial.h"
#include "dawneeg_config_tracker.h"

enum DAWNEEG_BOARD_TYPE {
    DAWNEEG_UNKNOWN = 0,
    DAWNEEG4 = 4,
    DAWNEEG6 = 6,
    DAWNEEG8 = 8,
    DAWNEEG16 = 16,
    DAWNEEG24 = 24,
    DAWNEEG32 = 32
};

#define DAWNEEG_BAUDRATE 115200

class DawnEEG : public Board
{

private:
    volatile bool keep_alive;
    bool initialized;
    bool is_streaming;
    DAWNEEG_BOARD_TYPE board_type;

    std::thread streaming_thread;
    Serial *serial;
    DawnEEG_ConfigTracker config_tracker;
    std::mutex m;
    std::condition_variable cv;
    volatile int state;
    volatile double half_rtt;
    volatile double time_correction;

    int init_board ();
    int soft_reset ();
    int default_config ();

    int send_to_board (const char *msg);
    int send_to_board (const char *msg, std::string &response);
    std::string read_serial_response ();
    void read_thread ();
    int time_sync ();

public:
    DawnEEG (struct BrainFlowInputParams params);
    ~DawnEEG ();

    int prepare_session ();
    int start_stream (int buffer_size, const char *streamer_params);
    int stop_stream ();
    int release_session ();
    int config_board (std::string config, std::string &response);
};