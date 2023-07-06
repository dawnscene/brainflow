#pragma once

#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <stdio.h>

#include "board_controller.h"
#include "brainflow_boards.h"
#include "brainflow_constants.h"
#include "brainflow_input_params.h"
#include "data_buffer.h"
#include "spinlock.h"
#include "streamer.h"

#define MAX_CAPTURE_SAMPLES (86400 * 250) // should be enough for one day of capturing


class Board
{
public:
    static JNIEnv *java_jnienv; // nullptr unless on java
    static int set_log_level (int log_level);
    static int add_log_file (const char *log_file, loguru::FileMode mode, loguru::Verbosity verbosity);
    static int add_callback (const char* id, loguru::log_handler_t callback, void* user_data, loguru::Verbosity verbosity);

    virtual ~Board ()
    {
        skip_logs = true; // also should be set in inherited class destructor because it will be
                          // called before
        free_packages ();
    }

    Board (int board_id, struct BrainFlowInputParams params)
    {
        skip_logs = false;
        this->board_id = board_id;
        this->params = params;
        try
        {
            board_descr = boards_struct.brainflow_boards_json["boards"][std::to_string (board_id)];
        }
        catch (json::exception &e)
        {
            LOG_F(ERROR, e.what ());
        }
    }
    virtual int prepare_session () = 0;
    virtual int start_stream (int buffer_size, const char *streamer_params) = 0;
    virtual int stop_stream () = 0;
    virtual int release_session () = 0;
    virtual int config_board (std::string config, std::string &response) = 0;

    int get_current_board_data (
        int num_samples, int preset, double *data_buf, int *returned_samples);
    int get_board_data_count (int preset, int *result);
    int get_board_data (int data_count, int preset, double *data_buf);
    int insert_marker (double value, int preset);
    int add_streamer (const char *streamer_params, int preset);
    int delete_streamer (const char *streamer_params, int preset);

    int get_board_id ()
    {
        return board_id;
    }

protected:
    std::map<int, DataBuffer *> dbs;
    std::map<int, std::vector<Streamer *>> streamers;
    bool skip_logs;
    int board_id;
    struct BrainFlowInputParams params;
    json board_descr;
    SpinLock lock;
    std::map<int, std::deque<double>> marker_queues;

    int prepare_for_acquisition (int buffer_size, const char *streamer_params);
    void free_packages ();
    void push_package (double *package, int preset = (int)BrainFlowPresets::DEFAULT_PRESET);
    std::string preset_to_string (int preset);
    int preset_to_int (std::string preset);
    int parse_streamer_params (const char *streamer_params, std::string &streamer_type,
        std::string &streamer_dest, std::string &streamer_mods);

private:
    // reshapes data from DataBuffer format where all channels are mixed to linear buffer
    void reshape_data (int data_count, int preset, const double *buf, double *output_buf);
};
