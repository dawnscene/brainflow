#pragma once

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

#define DAWNEEG_CMD_PROMPT "$$$"
#define DAWNEEG_CMD_SOFT_RESET "v"
#define DAWNEEG_CMD_DEFAULT "d"
#define DAWNEEG_CMD_START_STREAM "b"
#define DAWNEEG_CMD_STOP_STREAM "s"
#define DAWNEEG_STREAM_HEADER 0xA0
//#define DAWNEEG_STREAM_FOOTER 0xC0
#define DAWNEEG_STREAM_FOOTER 0xC4

class DawnEEG : public Board
{

protected:
    volatile bool keep_alive;
    bool initialized;
    bool is_streaming;
    DAWNEEG_BOARD_TYPE board_type;

    std::thread streaming_thread;
    Serial *serial;
    DawnEEG_ConfigTracker config_tracker;


    virtual int open_port ();
    virtual int set_port_settings ();
    virtual int init_board ();
    virtual int send_to_board (const char *msg);
    virtual int send_to_board (const char *msg, std::string &response);
    virtual std::string read_serial_response ();
    virtual void read_thread ();

public:
    DawnEEG (struct BrainFlowInputParams params);
    virtual ~DawnEEG ();

    virtual int prepare_session ();
    virtual int start_stream (int buffer_size, const char *streamer_params);
    virtual int stop_stream ();
    virtual int release_session ();
    virtual int config_board (std::string config, std::string &response);
};
