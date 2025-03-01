#include "ant_neuro.h"

// implementation for linux and windows
#if defined(_WIN32) || defined(__linux__) && !defined(__ANDROID__)

#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>

#include "get_dll_dir.h"
#include "timestamp.h"

#include "eemagine/sdk/factory.h"
#include "eemagine/sdk/wrapper.h"

using namespace eemagine::sdk;


AntNeuroBoard::AntNeuroBoard (int board_id, struct BrainFlowInputParams params)
    : Board (board_id, params)
{
    // get full path of and neuro library with assumption that this lib is in the same folder
    char ant_neuro_lib_dir[1024];
    bool res = get_dll_path (ant_neuro_lib_dir);
    std::string lib_name = "";
    if (sizeof (void *) == 4)
    {
#ifdef _WIN32
        lib_name = "eego-SDK32.dll";
#else
        lib_name = "libeego-SDK32.so";
#endif
    }
    else
    {
#ifdef _WIN32
        lib_name = "eego-SDK.dll";
#else
        lib_name = "libeego-SDK.so";
#endif
    }
    if (res)
    {
        ant_neuro_lib_path = std::string (ant_neuro_lib_dir) + lib_name;
    }
    else
    {
        ant_neuro_lib_path = lib_name;
    }
    LOG_F(1, "use dyn lib: {}", ant_neuro_lib_path.c_str ());

    keep_alive = false;
    initialized = false;
    amp = NULL;
    stream = NULL;
    try
    {
        sampling_rate = board_descr["default"]["sampling_rate"];
    }
    catch (...)
    {
        sampling_rate = 2000;
    }
}

AntNeuroBoard::~AntNeuroBoard ()
{
    skip_logs = true;
    release_session ();
}

int AntNeuroBoard::prepare_session ()
{
    if (initialized)
    {
        LOG_F(INFO, "Session is already prepared");
        return (int)BrainFlowExitCodes::STATUS_OK;
    }

    try
    {
        factory fact (ant_neuro_lib_path);
        amp = fact.getAmplifier ();
    }
    catch (const exceptions::notFound &e)
    {
        LOG_F(ERROR, "No devices found, {}", e.what ());
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }
    catch (...)
    {
        LOG_F(ERROR, "Failed to create factory from {}", ant_neuro_lib_path.c_str ());
        return (int)BrainFlowExitCodes::GENERAL_ERROR;
    }

    initialized = true;
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int AntNeuroBoard::start_stream (int buffer_size, const char *streamer_params)
{
    if (stream != NULL)
    {
        LOG_F(ERROR, "Streaming thread already running");
        return (int)BrainFlowExitCodes::STREAM_ALREADY_RUN_ERROR;
    }
    if (amp == NULL)
    {
        LOG_F(ERROR, "Amplifier is not created");
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }
    int res = prepare_for_acquisition (buffer_size, streamer_params);
    if (res != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return res;
    }

    try
    {
        stream = amp->OpenEegStream (
            sampling_rate); // todo do we need other args? If yes pass them via config_board
    }
    catch (...)
    {
        LOG_F(ERROR, "Failed to start acquisition.");
        return (int)BrainFlowExitCodes::STREAM_THREAD_ERROR;
    }
    if (stream == NULL)
    {
        LOG_F(ERROR, "Failed to start acquisition.");
        return (int)BrainFlowExitCodes::STREAM_THREAD_ERROR;
    }

    keep_alive = true;
    streaming_thread = std::thread ([this] { this->read_thread (); });
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int AntNeuroBoard::stop_stream ()
{
    if (stream != NULL)
    {
        keep_alive = false;
        streaming_thread.join ();
        delete stream;
        stream = NULL;
        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    else
    {
        return (int)BrainFlowExitCodes::STREAM_THREAD_IS_NOT_RUNNING;
    }
}

int AntNeuroBoard::release_session ()
{
    if (initialized)
    {
        stop_stream ();
        free_packages ();
        initialized = false;
    }
    if (amp != NULL)
    {
        delete amp;
        amp = NULL;
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

void AntNeuroBoard::read_thread ()
{
    if ((amp == NULL) || (stream == NULL))
    {
        LOG_F(ERROR, "amp or stream not created in thread");
        return;
    }

    int num_rows = board_descr["default"]["num_rows"];
    double *package = new double[num_rows];
    for (int i = 0; i < num_rows; i++)
    {
        package[i] = 0.0;
    }
    std::vector<int> emg_channels;
    std::vector<int> eeg_channels;
    try
    {
        emg_channels = board_descr["default"]["emg_channels"].get<std::vector<int>> ();
    }
    catch (...)
    {
        LOG_F(2, "device has no emg channels");
    }
    try
    {
        eeg_channels = board_descr["default"]["eeg_channels"].get<std::vector<int>> ();
    }
    catch (...)
    {
        LOG_F(2, "device has no eeg channels");
    }
    std::vector<channel> ant_channels = stream->getChannelList ();

    while (keep_alive)
    {
        try
        {
            buffer buf = stream->getData ();
            int buf_channels_len = buf.getChannelCount ();
            for (int i = 0; i < (int)buf.getSampleCount (); i++)
            {
                int eeg_counter = 0;
                int emg_counter = 0;
                for (int j = 0; j < buf_channels_len; j++)
                {
                    if ((ant_channels[j].getType () == channel::reference) &&
                        (eeg_counter < (int)eeg_channels.size ()))
                    {
                        package[eeg_channels[eeg_counter++]] = buf.getSample (j, i);
                    }
                    if ((ant_channels[j].getType () == channel::bipolar) &&
                        (emg_counter < (int)emg_channels.size ()))
                    {
                        package[emg_channels[emg_counter++]] = buf.getSample (j, i);
                    }
                    if (ant_channels[j].getType () == channel::sample_counter)
                    {
                        package[board_descr["default"]["package_num_channel"].get<int> ()] =
                            buf.getSample (j, i);
                    }
                    if (ant_channels[j].getType () == channel::trigger)
                    {
                        package[board_descr["default"]["other_channels"][0].get<int> ()] =
                            buf.getSample (j, i);
                    }
                }
                package[board_descr["default"]["timestamp_channel"].get<int> ()] = get_timestamp ();
                push_package (package);
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        catch (...)
        {
            LOG_F(ERROR, "exception in data thread.");
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
    }
    delete[] package;
}

int AntNeuroBoard::config_board (std::string config, std::string &response)
{
    if (amp == NULL)
    {
        LOG_F(ERROR, "Amplifier is not created");
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }

    std::string prefix = "sampling_rate:";
    if (config.find (prefix) != std::string::npos)
    {
        int new_sampling_rate = 0;
        std::string value = config.substr (prefix.size ());
        try
        {
            new_sampling_rate = std::stoi (value);
        }
        catch (...)
        {
            LOG_F(ERROR, "format is '{}value'", prefix.c_str ());
            return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
        }
        // check that provided value is correct
        std::vector<int> allowed_values = amp->getSamplingRatesAvailable ();
        if (std::find (allowed_values.begin (), allowed_values.end (), new_sampling_rate) !=
            allowed_values.end ())
        {
            sampling_rate = new_sampling_rate;
            return (int)BrainFlowExitCodes::STATUS_OK;
        }
        else
        {
            LOG_F(ERROR, "not supported value provided");
            for (int i = 0; i < (int)allowed_values.size (); i++)
            {
                LOG_F(1, "supported value: {}", allowed_values[i]);
            }
            return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
        }
    }

    LOG_F(ERROR, "format is '{}value'", prefix.c_str ());
    return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
}

// stub for macos
#else
AntNeuroBoard::AntNeuroBoard (int board_id, struct BrainFlowInputParams params)
    : Board (board_id, params)
{
}

AntNeuroBoard::~AntNeuroBoard ()
{
}

int AntNeuroBoard::prepare_session ()
{
    LOG_F(ERROR, "AntNeuroBoard doesnt support MacOS.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int AntNeuroBoard::config_board (std::string config, std::string &response)
{
    LOG_F(ERROR, "AntNeuroBoard doesnt support MacOS.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int AntNeuroBoard::release_session ()
{
    LOG_F(ERROR, "AntNeuroBoard doesnt support MacOS.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int AntNeuroBoard::stop_stream ()
{
    LOG_F(ERROR, "AntNeuroBoard doesnt support MacOS.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int AntNeuroBoard::start_stream (int buffer_size, const char *streamer_params)
{
    LOG_F(ERROR, "AntNeuroBoard doesnt support MacOS.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}
#endif
