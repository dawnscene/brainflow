#pragma once

#include <algorithm>
#include <stdlib.h>
#include <string>
#include <vector>

#define DAWNEEG_DEFAULT_GAIN 24
#define DAWNEEG_MAX_CHS 32

enum class DawnEEG_CommandTypes : int
{
    VALID_COMMAND = 0,
    INVALID_COMMAND = 1
};

#define SIZE_CHANNEL_COMMAND 9
#define SIZE_IMPEDANCE_COMMAND 5
#define SIZE_SAMPLE_RATE_COMMAND 2
#define SIZE_ON_OFF_COMMAND 3

class DawnEEG_ConfigTracker
{
protected:
    std::vector<char> channel_letters;
    std::vector<int> current_gains;
    std::vector<int> old_gains;
    std::vector<int> available_gain_values;

    int apply_single_channel_command (std::string command)
    {
        // start stop validation
        if ((command.size () < SIZE_CHANNEL_COMMAND) || (command.at (0) != 'x') ||
            (command.at (SIZE_CHANNEL_COMMAND - 1) != 'X'))
        {
            return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
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

    int apply_single_impedance_command (std::string command)
    {
        // start stop validation
        if ((command.size () < SIZE_IMPEDANCE_COMMAND) || (command.at (0) != 'z') ||
            (command.at (SIZE_IMPEDANCE_COMMAND - 1) != 'Z'))
        {
            return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
        }
        if ((command.at (3) < '0') || (command.at (3) > '1'))
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

        int res;

        for (size_t i = 0; i < config.size ();)
        {
            switch(config.at(i))
            {
                case 'd':
                    std::copy (current_gains.begin (), current_gains.end (), old_gains.begin ());
                    std::fill (current_gains.begin (), current_gains.end (), DAWNEEG_DEFAULT_GAIN);
                    i++;
                    break;
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case 'q':
                case 'w':
                case 'e':
                case 'r':
                case 't':
                case 'y':
                case 'u':
                case 'i':
                case '!':
                case '@':
                case '#':
                case '$':
                case '%':
                case '^':
                case '&':
                case '*':
                case 'Q':
                case 'W':
                case 'E':
                case 'R':
                case 'T':
                case 'Y':
                case 'U':
                case 'I':
                case '0':
                case '-':
                case '=':
                case 'p':
                case '[':
                case ']':
                case 'D':
                case '?':
                case 'V':
                    i++;
                    break;
                case 'o':
                case 'O':
                    if (config.size() >= i + SIZE_ON_OFF_COMMAND)
                        i += SIZE_ON_OFF_COMMAND;
                    else
                        return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
                    break;
                case '~':
                    if (config.size() >= i + SIZE_SAMPLE_RATE_COMMAND)
                        i += SIZE_SAMPLE_RATE_COMMAND;
                    else
                        return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
                    break;
                case 'x':
                    if ((config.size () >= i + SIZE_CHANNEL_COMMAND) &&
                        (config.at (i + SIZE_CHANNEL_COMMAND - 1) == 'X'))
                    {
                        res = apply_single_channel_command (config.substr (i, SIZE_CHANNEL_COMMAND));
                        if (res != (int)DawnEEG_CommandTypes::VALID_COMMAND)
                            return res;
                        i += SIZE_CHANNEL_COMMAND;
                    }
                    else
                    {
                        return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
                    }
                    break;
                case 'z':
                    if ((config.size () >= i + SIZE_IMPEDANCE_COMMAND) &&
                        (config.at (i + SIZE_IMPEDANCE_COMMAND - 1) == 'Z'))
                    {
                        res = apply_single_impedance_command (config.substr (i, SIZE_IMPEDANCE_COMMAND));
                        if (res != (int)DawnEEG_CommandTypes::VALID_COMMAND)
                            return res;
                        i += SIZE_IMPEDANCE_COMMAND;
                    }
                    else
                    {
                        return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
                    }
                    break;
                default:
                    return (int)DawnEEG_CommandTypes::INVALID_COMMAND;
            }
        }

        return (int)DawnEEG_CommandTypes::VALID_COMMAND;
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
