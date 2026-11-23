#include "pch.h"
#include "ll_fps_limiter.h"

void FpsLimiter::limit()
{
    if (minimum_interval_us == 0 || !is_limiting())
    {
        pacer.Reset();
        return;
    }

    pacer.Pace(minimum_interval_us);
}

void FpsLimiter::get_sleep_status(SleepParams* sleep_params)
{
    sleep_params->low_latency_enabled = is_enabled();
    sleep_params->fullscreen_vrr = true;
    sleep_params->control_panel_vsync_override = false;
};

void FpsLimiter::set_sleep_mode(SleepMode* sleep_mode)
{
    low_latency_enabled = sleep_mode->low_latency_enabled;
    minimum_interval_us = sleep_mode->minimum_interval_us;
};

void FpsLimiter::sleep(std::optional<uint32_t> frame_id)
{
    last_sleep_framecount = simulation_framecount;
    limit();
};

void FpsLimiter::set_marker(IUnknown* pDevice, const MarkerParams& marker_params)
{
    if (marker_params.marker_type != MarkerType::SIMULATION_START)
        return;

    simulation_framecount++;

    // Only when the game doesn't send sleep calls
    if (last_sleep_framecount + sleep_call_timeout < simulation_framecount)
        limit();
};
