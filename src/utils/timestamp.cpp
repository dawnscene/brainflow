#include "timestamp.h"
#include <chrono>
#include <cstdlib>

double get_timestamp() {
    int64_t local_clock_ns = std::chrono::nanoseconds(std::chrono::steady_clock::now().time_since_epoch()).count();
	const auto ns_per_s = 1000000000;
	const auto seconds_since_epoch = std::lldiv(local_clock_ns, ns_per_s);
	/* For large timestamps, converting to double and then dividing by 1e9 loses precision
	   because double has only 53 bits of precision.
	   So we calculate everything we can as integer and only cast to double at the end */
	return seconds_since_epoch.quot + static_cast<double>(seconds_since_epoch.rem) / ns_per_s;
}

/*
#ifdef _WIN32
double get_timestamp ()
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime (&ft);
    int64_t t = ((int64_t)ft.dwHighDateTime << 32L) | (int64_t)ft.dwLowDateTime;
    return (t - FILETIME_TO_UNIX) / (10.0 * 1000.0 * 1000.0);
}
#else
double get_timestamp ()
{
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (double)(tv.tv_sec) + (double)(tv.tv_usec) / 1000000.0;
}
#endif
*/
