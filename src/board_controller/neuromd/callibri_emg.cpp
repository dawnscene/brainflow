#include "callibri_emg.h"

#if defined _WIN32 || defined __APPLE__

#include "c_ecg_channels.h"
#include "c_eeg_channels.h"
#include "cparams.h"
#include "csignal-channel.h"
#include "sdk_error.h"


int CallibriEMG::apply_initial_settings ()
{
    int exit_code = device_set_SamplingFrequency (device, SamplingFrequencyHz1000);
    if (exit_code != SDK_NO_ERROR)
    {
        char error_msg[1024];
        sdk_last_error_msg (error_msg, 1024);
        LOG_F(ERROR, "Failed to set sampling rate: {}", error_msg);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    exit_code = device_set_Gain (device, Gain6);
    if (exit_code != SDK_NO_ERROR)
    {
        char error_msg[1024];
        sdk_last_error_msg (error_msg, 1024);
        LOG_F(ERROR, "Failed to set gain: {}", error_msg);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    exit_code = device_set_Offset (device, 0);
    if (exit_code != SDK_NO_ERROR)
    {
        char error_msg[1024];
        sdk_last_error_msg (error_msg, 1024);
        LOG_F(ERROR, "Failed to set offset: {}", error_msg);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    if (strcmp (params.other_info.c_str (), "ExternalSwitchInputMioUSB") == 0)
    {
        LOG_F(INFO, "Use electrodes connected to USB");
        exit_code = device_set_ExternalSwitchState (device, ExternalSwitchInputMioUSB);
    }
    else
    {
        LOG_F(INFO, "Use plain electrodes");
        exit_code = device_set_ExternalSwitchState (device, ExternalSwitchInputMioElectrodes);
    }

    if (exit_code != SDK_NO_ERROR)
    {
        char error_msg[1024];
        sdk_last_error_msg (error_msg, 1024);
        LOG_F(ERROR, "Failed to set switch state: {}", error_msg);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    exit_code = device_set_ADCInputState (device, ADCInputResistance);
    if (exit_code != SDK_NO_ERROR)
    {
        char error_msg[1024];
        sdk_last_error_msg (error_msg, 1024);
        LOG_F(ERROR, "Failed to set ADC input state: {}", error_msg);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    exit_code = device_set_HardwareFilterState (device, true);
    if (exit_code != SDK_NO_ERROR)
    {
        char error_msg[1024];
        sdk_last_error_msg (error_msg, 1024);
        LOG_F(ERROR, "Failed to set filter state: {}", error_msg);
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

#endif
