#pragma once
#ifndef CATA_SRC_STUCK_DEBUG_H
#define CATA_SRC_STUCK_DEBUG_H

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "path_info.h"

// TEMPORARY diagnostic scaffold, delete once the hunt is over.
// Hot paths only bump relaxed atomics; a detached watchdog thread flushes one
// line per 200 ms to config/zone_stuck_debug.log, so a wedged main thread
// still leaves a growing trace of per-tick counter deltas.
namespace stuck_debug
{

enum phase : int {
    PH_NONE = 0,
    PH_GPSL,        // zone_manager::get_point_set_loot
    PH_GVZ,         // map::get_vehicle_zones
    PH_MULTIACT,    // generic_multi_activity_handler
    PH_CHECKREQ,    // generic_multi_activity_check_requirement
    PH_DOACT,       // npc::do_player_activity
    PH_COUNT
};

inline const char *phase_name( const int p )
{
    static const char *const names[] = {
        "none", "get_point_set_loot", "get_vehicle_zones", "multi_activity",
        "check_requirement", "do_player_activity"
    };
    return p > 0 && p < PH_COUNT ? names[p] : "?";
}

inline std::string log_path()
{
    std::string p = PATH_INFO::debug();
    const size_t pos = p.find_last_of( "/\\" );
    p.erase( pos == std::string::npos ? 0 : pos + 1 );
    return p + "zone_stuck_debug.log";
}

// Leaked on purpose: never destructed, so writes from the watchdog stay safe
// during shutdown.
inline std::ofstream &out()
{
    static std::ofstream *f = []() {
        std::string path = log_path();
        std::ofstream *s = new std::ofstream( path, std::ios::out | std::ios::trunc );
        if( !s->is_open() ) {
            delete s;
            path = "zone_stuck_debug.log";
            s = new std::ofstream( path, std::ios::out | std::ios::trunc );
        }
        *s << "# file    : " << path << "\n";
        *s << "# columns : t_ms | phase | per-tick delta (200ms) | totals\n";
        s->flush();
        return s;
    }();
    return *f;
}

inline long long now_ms()
{
    static const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0 ).count();
}

// First `first_n` hits are always written, afterwards at most one per `gap_ms`.
inline bool due( long long &last_ms, long long count, long long first_n = 30,
                 long long gap_ms = 1000 )
{
    if( count <= first_n ) {
        return true;
    }
    const long long t = now_ms();
    if( t - last_ms >= gap_ms ) {
        last_ms = t;
        return true;
    }
    return false;
}

// Both the watchdog thread and the game thread may write; every line is
// flushed so the file stays usable even if the process is killed.
inline std::mutex &io_mutex()
{
    static std::mutex m;
    return m;
}

inline void log( const std::string &line )
{
    std::lock_guard<std::mutex> guard( io_mutex() );
    out() << now_ms() << " | " << line << std::endl;
}

enum counter_id : int {
    C_GPSL_CALLS = 0,
    C_GPSL_SCANNED,
    C_GPSL_INSIDE,
    C_GVZ_CALLS,
    C_GVZ_ZONES,
    C_MULTIACT_CALLS,
    C_CHECKREQ_CALLS,
    C_DOACT_ITERS,
    C_MSG_ADDED,
    C_MSG_LOG_SIZE,
    C_COUNT
};

inline std::atomic<long long> &counter( const int idx )
{
    static std::atomic<long long> *v = new std::atomic<long long>[C_COUNT]();
    return v[idx];
}

inline void bump( const int idx, const long long by = 1 )
{
    counter( idx ).fetch_add( by, std::memory_order_relaxed );
}
inline long long read( const int idx )
{
    return counter( idx ).load( std::memory_order_relaxed );
}

inline std::atomic<int> &phase()
{
    static std::atomic<int> v{ 0 };
    return v;
}
// Set on entry only: the phase reads as "the innermost function we entered
// last", which is what we want when sampling a wedged main thread.
inline void enter( const int p )
{
    phase().store( p, std::memory_order_relaxed );
}

inline void watchdog()
{
    long long last[C_COUNT] = {};
    int idle_ticks = 0;
    while( true ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
        long long cur[C_COUNT];
        long long d[C_COUNT];
        long long sum = 0;
        for( int i = 0; i < C_COUNT; i++ ) {
            cur[i] = read( i );
            d[i] = cur[i] - last[i];
            last[i] = cur[i];
            sum += d[i];
        }
        if( sum == 0 ) {
            if( ++idle_ticks < 5 ) {
                continue;
            }
            idle_ticks = 0;
        } else {
            idle_ticks = 0;
        }
        std::string line = std::string( "phase=" ) +
                           phase_name( phase().load( std::memory_order_relaxed ) ) +
                           " | gpsl=+" + std::to_string( d[C_GPSL_CALLS] ) +
                           " scanned=+" + std::to_string( d[C_GPSL_SCANNED] ) +
                           " gvz=+" + std::to_string( d[C_GVZ_CALLS] ) +
                           " gvzZones=+" + std::to_string( d[C_GVZ_ZONES] ) +
                           " multiAct=+" + std::to_string( d[C_MULTIACT_CALLS] ) +
                           " checkReq=+" + std::to_string( d[C_CHECKREQ_CALLS] ) +
                           " doActIters=+" + std::to_string( d[C_DOACT_ITERS] ) +
                           " msgs=+" + std::to_string( d[C_MSG_ADDED] ) +
                           " | totals gpsl=" + std::to_string( cur[C_GPSL_CALLS] ) +
                           " scanned=" + std::to_string( cur[C_GPSL_SCANNED] ) +
                           " gvz=" + std::to_string( cur[C_GVZ_CALLS] ) +
                           " insideGpsl=" + std::to_string( cur[C_GPSL_INSIDE] ) +
                           " msgLog=" + std::to_string( cur[C_MSG_LOG_SIZE] );
        log( line );
    }
}

inline std::atomic<bool> &started_flag()
{
    static std::atomic<bool> v{ false };
    return v;
}

inline void ensure_started()
{
    if( started_flag().load( std::memory_order_relaxed ) ) {
        return;
    }
    static std::once_flag flag;
    std::call_once( flag, []() {
        log( "watchdog started" );
        std::thread( watchdog ).detach();
        started_flag().store( true, std::memory_order_relaxed );
    } );
}

} // namespace stuck_debug

#endif // CATA_SRC_STUCK_DEBUG_H
