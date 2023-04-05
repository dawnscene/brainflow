#pragma once

#include <algorithm>
#include <stdlib.h>
#include <string>
#include <vector>

#define DAWNEEG_DEFAULT_GAIN 24
#define DAWNEEG_MAX_CHS 32

enum class DawnEEG_CommandTypes : int
{
    NOT_CHANNEL_COMMAND = 0,
    VALID_COMMAND = 1,
    INVALID_COMMAND = 2
};


class DawnEEG_ConfigTracker
{
protected:
    size_t single_command_size = 9;
    std::vector<char> channel_letters;
    std::vector<int> current_gains;
    std::vector<int> old_gains;
    std::vector<int> available_gain_values;

    int apply_single_command (std::string command)
    {
        // start stop validation
        if ((command.size () < single_command_size) || (command.at (0) != 'x') ||
            (command.at (single_command_size - 1) != 'X'))
        {
            return (int)DawnEEG_CommandTypes::NOT_CHANNEL_COMMAND;
        }
        // bias srb1 srb2 validation
        if ((command.at (5) != '0') && (command.at (5) != '1') ||
            (command.at (6) != '0') && (command.at (6) != '1') ||
            (command.at (7) != '0') && (command.at (7) != '1'))
        {
            return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
        }
        // input type check
        if ((command.at (4) < '0') || (command.at (4) > '7'))
        {
            return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
        }
        // gain check
        if ((command.at (3) < '0') || (command.at (3) > '6'))
        {
            return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
        }
        // power check
        if ((command.at (2) != '0') && (command.at (2) != '1'))
        {
            return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
        }
        // channel check
        auto channel_it =
            std::find (channel_letters.begin (), channel_letters.end (), command.at (1));
        if (channel_it == channel_letters.end ())
        {
            return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
        }
        size_t index = std::distance (channel_letters.begin (), channel_it);
        if (index >= current_gains.size ())
        {
            return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
        }
        old_gains[index] = current_gains[index];
        current_gains[index] = available_gain_values[command.at (3) - '0'];
        return (int)DawnEEG_CommandTypes::VALID_COMMAND;
    }

public:
    DawnEEG_ConfigTracker (std::vector<int> default_gains = std::vector<int> (DAWNEEG_MAX_CHS, {DAWNEEG_DEFAULT_GAIN}))
        : current_gains (default_gains), old_gains (default_gains)
    {
        channel_letters = std::vector<char> {
            '1', '2', '3', '4', '5', '6', '7', '8',     // channel 1-8
            'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',     // channel 9-16
            'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K',     // channel 17-24
            'Z', 'X', 'C', 'V', 'B', 'N', 'M', 'L' };   // channel 25-32
        available_gain_values = std::vector<int> {1, 2, 4, 6, 8, 12, 24};

    };

    virtual ~DawnEEG_ConfigTracker ()
    {
    }

    virtual int apply_config (std::string config)
    {
        // x (CHANNEL, POWER_DOWN, GAIN_SET, INPUT_TYPE_SET, BIAS_SET, SRB2_SET, SRB1_SET) X
        // https://docs.openbci.com/Cyton/CytonSDK/

        int res = (int)DawnEEG_CommandTypes::NOT_CHANNEL_COMMAND;

        if (config.size () == 1)
        {
            // restore default settings
            if (config.at (0) == 'd')
            {
                std::copy (current_gains.begin (), current_gains.end (), old_gains.begin ());
                std::fill (current_gains.begin (), current_gains.end (), DAWNEEG_DEFAULT_GAIN);
                return (int)DawnEEG_CommandTypes::VALID_COMMAND;
            }
        }
        else
        {
            for (size_t i = 0; i < config.size ();)
            {
                if (config.at (i) == 'x')
                {
                    if ((config.size () >= i + single_command_size) &&
                        (config.at (i + single_command_size - 1) == 'X'))
                    {
                        res = apply_single_command (config.substr (i, single_command_size));
                        if (res != (int)DawnEEG_CommandTypes::VALID_COMMAND)
                            return res;
                        i += single_command_size;
                    }
                    else
                    {
                        i++;
                    }
                }
                else
                {
                    i++;
                }
            }
        }
        return res;
    }

    virtual int get_gain_for_channel (int channel)
    {
        if (channel > (int)current_gains.size ())
        {
            return 1; // should never happen
        }
        return current_gains[channel];
    }

    virtual void revert_config ()
    {
        std::copy (old_gains.begin (), old_gains.end (), current_gains.begin ());
    }
};
