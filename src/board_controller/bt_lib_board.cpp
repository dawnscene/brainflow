#include <string.h>

#include "bt_lib_board.h"

#include "bluetooth_types.h"
#include "get_dll_dir.h"


BTLibBoard::BTLibBoard (int board_id, struct BrainFlowInputParams params) : Board (board_id, params)
{
    char bluetoothlib_dir[1024];
    bool res = get_dll_path (bluetoothlib_dir);
    std::string bluetoothlib_path = "";
#ifdef _WIN32
    std::string lib_name;
    if (sizeof (void *) == 4)
    {
        lib_name = "BrainFlowBluetooth32.dll";
    }
    else
    {
        lib_name = "BrainFlowBluetooth.dll";
    }
#elif defined(__APPLE__)
    std::string lib_name = "libBrainFlowBluetooth.dylib";
#else
    std::string lib_name = "libBrainFlowBluetooth.so";
#endif
    if (res)
    {
        bluetoothlib_path = std::string (bluetoothlib_dir) + lib_name;
    }
    else
    {
        bluetoothlib_path = lib_name;
    }

    LOG_F(1, "use dyn lib: {}", bluetoothlib_path.c_str ());
    dll_loader = new DLLLoader (bluetoothlib_path.c_str ());
    initialized = false;
}

BTLibBoard::~BTLibBoard ()
{
    skip_logs = true;
    BTLibBoard::release_session ();
}

int BTLibBoard::prepare_session ()
{
    if (initialized)
    {
        LOG_F(INFO, "Session is already prepared");
        return (int)BrainFlowExitCodes::STATUS_OK;
    }

    int return_res = (int)BrainFlowExitCodes::STATUS_OK;
    if (!dll_loader->load_library ())
    {
        LOG_F(ERROR, "Failed to load library");
        return_res = (int)BrainFlowExitCodes::GENERAL_ERROR;
    }
    else
    {
        LOG_F(1, "Library is loaded");
    }

    if (params.ip_port <= 0)
    {
        params.ip_port = 1;
    }
    LOG_F(INFO, "Use bluetooth port: {}", params.ip_port);
    if ((params.mac_address.empty ()) && (return_res == (int)BrainFlowExitCodes::STATUS_OK))
    {
        LOG_F(WARNING, "mac address is not provided, trying to autodiscover device");
        int res = find_bt_addr (get_name_selector ().c_str ());
        if (res == (int)SocketBluetoothReturnCodes::UNIMPLEMENTED_ERROR)
        {
            LOG_F(ERROR, "autodiscovery for this OS is not supported");
            return_res = (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
        }
        else if (res == (int)SocketBluetoothReturnCodes::DEVICE_IS_NOT_DISCOVERABLE)
        {
            LOG_F(ERROR, "check that device paired and connected");
            return_res = (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
        }
        else if (res != (int)SocketBluetoothReturnCodes::STATUS_OK)
        {
            LOG_F(ERROR, "failed to autodiscover device: {}", res);
            return_res = (int)BrainFlowExitCodes::GENERAL_ERROR;
        }
        else
        {
            LOG_F(INFO, "found device {}", params.mac_address.c_str ());
        }
    }

    if (return_res == (int)BrainFlowExitCodes::STATUS_OK)
    {
        initialized = true;
    }
    else
    {
        dll_loader->free_library ();
        delete dll_loader;
        dll_loader = NULL;
    }

    return return_res;
}

int BTLibBoard::release_session ()
{
    if (dll_loader != NULL)
    {
        dll_loader->free_library ();
        delete dll_loader;
        dll_loader = NULL;
    }
    initialized = false;
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int BTLibBoard::config_board (std::string config, std::string &response)
{
    int res = bluetooth_write_data (config.c_str (), (int)strlen (config.c_str ()));
    if (res != (int)strlen (config.c_str ()))
    {
        LOG_F(ERROR, "failed to config device, res: {}", res);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int BTLibBoard::bluetooth_open_device ()
{
    int (*func_open) (int, char *) =
        (int (*) (int, char *))dll_loader->get_address ("bluetooth_open_device");
    if (func_open == NULL)
    {
        LOG_F(ERROR, "failed to get function address for bluetooth_open_device");
        return (int)BrainFlowExitCodes::GENERAL_ERROR;
    }

    int res = func_open (params.ip_port, const_cast<char *> (params.mac_address.c_str ()));
    if (res != (int)SocketBluetoothReturnCodes::STATUS_OK)
    {
        LOG_F(ERROR, "failed to open bt connection: {}", res);
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int BTLibBoard::bluetooth_close_device ()
{
    int (*func_close) (char *) =
        (int (*) (char *))dll_loader->get_address ("bluetooth_close_device");
    if (func_close == NULL)
    {
        LOG_F(ERROR, "failed to get function address for bluetooth_close_device");
        return (int)BrainFlowExitCodes::GENERAL_ERROR;
    }
    int res = func_close (const_cast<char *> (params.mac_address.c_str ()));
    if (res != (int)SocketBluetoothReturnCodes::STATUS_OK)
    {
        LOG_F(ERROR, "failed to close bt connection: {}", res);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int BTLibBoard::bluetooth_write_data (const char *command, int len)
{
    int (*func_config) (char *, int, char *) =
        (int (*) (char *, int, char *))dll_loader->get_address ("bluetooth_write_data");
    if (func_config == NULL)
    {
        LOG_F(ERROR, "failed to get function address for bluetooth_write_data");
        return (int)BrainFlowExitCodes::GENERAL_ERROR;
    }

    int res = func_config (
        const_cast<char *> (command), len, const_cast<char *> (params.mac_address.c_str ()));
    return res;
}

int BTLibBoard::bluetooth_get_data (char *data, int len)
{
    int (*func_get) (char *, int, char *) =
        (int (*) (char *, int, char *))dll_loader->get_address ("bluetooth_get_data");
    if (func_get == NULL)
    {
        LOG_F(ERROR, "failed to get function address for bluetooth_get_data");
        return (int)BrainFlowExitCodes::GENERAL_ERROR;
    }

    return func_get (data, len, const_cast<char *> (params.mac_address.c_str ()));
}

int BTLibBoard::find_bt_addr (const char *name_selector)
{
    int (*func_find) (char *, char *, int *) =
        (int (*) (char *, char *, int *))dll_loader->get_address ("bluetooth_discover_device");
    if (func_find == NULL)
    {
        LOG_F(ERROR, "failed to get function address for bluetooth_discover_device");
        return (int)BrainFlowExitCodes::GENERAL_ERROR;
    }
    char mac_addr[40];
    int len = 0;
    int res = func_find (const_cast<char *> (name_selector), mac_addr, &len);
    if (res == (int)SocketBluetoothReturnCodes::STATUS_OK)
    {
        std::string mac_string = mac_addr;
        params.mac_address = mac_string.substr (0, len);
    }

    return res;
}
