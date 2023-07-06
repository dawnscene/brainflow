#include <string>

#include "gforce_pro.h"

#ifdef _WIN32
#include <windows.h>

#include "get_dll_dir.h"


int GforcePro::num_objects = 0;


GforcePro::GforcePro (struct BrainFlowInputParams params)
    : DynLibBoard ((int)BoardIds::GFORCE_PRO_BOARD, params)
{
    GforcePro::num_objects++;
    if (GforcePro::num_objects > 1)
    {
        is_valid = false;
    }
    else
    {
        is_valid = true;
    }
}

GforcePro::~GforcePro ()
{
    skip_logs = true;
    GforcePro::num_objects--;
    release_session ();
}

std::string GforcePro::get_lib_name ()
{
    std::string gforcelib_path = "";
    std::string gforcelib_name = "";
    char gforcelib_dir[1024];
    bool res = get_dll_path (gforcelib_dir);
    if (sizeof (void *) == 4)
    {
        gforcelib_name = "gForceSDKWrapper32.dll";
    }
    else
    {
        gforcelib_name = "gForceSDKWrapper.dll";
    }

    if (res)
    {
        gforcelib_path = std::string (gforcelib_dir) + gforcelib_name;
        // gforcewrapper depends on gforcedll need to ensure that its in search path
        if (const char *env_p = std::getenv ("PATH"))
        {
            std::string path_env ("PATH=");
            path_env += env_p;
            path_env += ";";
            path_env += std::string (gforcelib_dir);
            if (_putenv (path_env.c_str ()) != 0)
            {
                LOG_F(WARNING, "Failed to set PATH to {}", path_env.c_str ());
            }
        }
    }
    else
    {
        gforcelib_path = gforcelib_name;
    }

    LOG_F(1, "use dyn lib: {}", gforcelib_path.c_str ());
    return gforcelib_path;
}

int GforcePro::prepare_session ()
{
    if (!is_valid)
    {
        LOG_F(INFO, "only one GForceLib per process is allowed");
        return (int)BrainFlowExitCodes::ANOTHER_BOARD_IS_CREATED_ERROR;
    }
    return DynLibBoard::prepare_session ();
}

#else

GforcePro::GforcePro (struct BrainFlowInputParams params)
    : Board ((int)BoardIds::GFORCE_PRO_BOARD, params)
{
}

GforcePro::~GforcePro ()
{
}

int GforcePro::prepare_session ()
{
    LOG_F(ERROR, "GforcePro doesnt support Unix.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int GforcePro::config_board (std::string config, std::string &response)
{
    LOG_F(ERROR, "GforcePro doesnt support Unix.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int GforcePro::release_session ()
{
    LOG_F(ERROR, "GforcePro doesnt support Unix.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int GforcePro::stop_stream ()
{
    LOG_F(ERROR, "GforcePro doesnt support Unix.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

int GforcePro::start_stream (int buffer_size, const char *streamer_params)
{
    LOG_F(ERROR, "GforcePro doesnt support Unix.");
    return (int)BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR;
}

#endif
