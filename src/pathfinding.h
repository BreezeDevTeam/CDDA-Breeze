#pragma once
#ifndef CATA_SRC_PATHFINDING_H
#define CATA_SRC_PATHFINDING_H

#include <bitset>
#include <cstddef>
#include <cstdint>

#include "coordinates.h"
#include "game_constants.h"
#include "mdarray.h"

enum pf_special : int {
    PF_NORMAL = 0x00,    // Plain boring tile (grass, dirt, floor etc.)
    PF_SLOW = 0x01,      // Tile with move cost >2
    PF_WALL = 0x02,      // Unpassable ter/furn/vehicle
    PF_VEHICLE = 0x04,   // Any vehicle tile (passable or not)
    PF_FIELD = 0x08,     // Dangerous field
    PF_TRAP = 0x10,      // Dangerous trap
    PF_UPDOWN = 0x20,    // Stairs, ramp etc. (terrain only)
    PF_CLIMBABLE = 0x40, // 0 move cost but can be climbed on examine
    PF_SHARP = 0x80,     // sharp items (barbed wire, etc)
    // Vertical transitions and open shafts, with terrain-or-furniture semantics
    // (i.e. matching map::has_flag()).  They are intentionally not part of the
    // pathfinding "non normal" mask; the sound propagation code uses them as a
    // cheap pre-filter for tiles that can pass sound between z-levels.
    PF_VERTICAL_UP = 0x100,   // GOES_UP or RAMP_UP
    PF_VERTICAL_DOWN = 0x200, // GOES_DOWN or RAMP_DOWN
    PF_NO_FLOOR = 0x400,      // NO_FLOOR, an open shaft
};

constexpr pf_special operator | ( pf_special lhs, pf_special rhs )
{
    return static_cast<pf_special>( static_cast< int >( lhs ) | static_cast< int >( rhs ) );
}

constexpr pf_special operator & ( pf_special lhs, pf_special rhs )
{
    return static_cast<pf_special>( static_cast< int >( lhs ) & static_cast< int >( rhs ) );
}

inline pf_special &operator |= ( pf_special &lhs, pf_special rhs )
{
    lhs = static_cast<pf_special>( static_cast< int >( lhs ) | static_cast< int >( rhs ) );
    return lhs;
}

inline pf_special &operator &= ( pf_special &lhs, pf_special rhs )
{
    lhs = static_cast<pf_special>( static_cast< int >( lhs ) & static_cast< int >( rhs ) );
    return lhs;
}

struct pathfinding_cache {
    pathfinding_cache();

    // Whole z-level invalidation.  Used for events that can touch arbitrary tiles
    // of the level: map generation, map window shift, vehicle changes.
    bool dirty = false;

    // Submap-granular invalidation.  A single submap had its terrain, furniture,
    // traps or fields changed, so only that 12x12 block has to be recomputed
    // instead of the whole 121 submap (17424 tile) level.
    // Bits are indexed by ( y / SEEY ) * my_MAPSIZE + ( x / SEEX ) of the map window.
    std::bitset<MAPSIZE * MAPSIZE> submap_dirty;

    cata::mdarray<pf_special, point_bub_ms> special;
    cata::mdarray<int16_t, point_bub_ms> cost;
};

struct pathfinding_settings {
    int bash_strength = 0;
    int max_dist = 0;
    // At least 2 times the above, usually more
    int max_length = 0;

    // Expected terrain cost (2 is flat ground) of climbing a wire fence
    // 0 means no climbing
    int climb_cost = 0;

    bool allow_open_doors = false;
    bool avoid_traps = false;
    bool allow_climb_stairs = true;
    bool avoid_rough_terrain = false;
    bool avoid_sharp = false;

    pathfinding_settings() = default;
    pathfinding_settings( const pathfinding_settings & ) = default;
    pathfinding_settings( int bs, int md, int ml, int cc, bool aod, bool at, bool acs, bool art,
                          bool as )
        : bash_strength( bs ), max_dist( md ), max_length( ml ), climb_cost( cc ),
          allow_open_doors( aod ), avoid_traps( at ), allow_climb_stairs( acs ), avoid_rough_terrain( art ),
          avoid_sharp( as ) {}

    pathfinding_settings &operator=( const pathfinding_settings & ) = default;
};

#endif // CATA_SRC_PATHFINDING_H
