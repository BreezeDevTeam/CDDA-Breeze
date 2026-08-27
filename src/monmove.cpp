// Monster movement code; essentially, the AI
#include "monster.h" // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <functional>
#include <list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "behavior.h"
#include "bionics.h"
#include "cached_options.h"
#include "cata_utility.h"
#include "character.h"
#include "colony.h"
#include "creature_tracker.h"
#include "debug.h"
#include "field.h"
#include "field_type.h"
#include "flood_fill.h"
#include "game.h"
#include "game_constants.h"
#include "line.h"
#include "make_static.h"
#include "map.h"
#include "map_iterator.h"
#include "mapdata.h"
#include "mattack_common.h"
#include "memory_fast.h"
#include "messages.h"
#include "monfaction.h"
#include "monster_oracle.h"
#include "mtype.h"
#include "npc.h"
#include "options.h"
#include "pathfinding.h"
#include "pimpl.h"
#include "rng.h"
#include "scent_map.h"
#include "sounds.h"
#include "string_formatter.h"
#include "submap.h"
#include "tileray.h"
#include "translations.h"
#include "trap.h"
#include "units.h"
#include "vehicle.h"
#include "viewer.h"
#include "vpart_position.h"

static const efftype_id effect_bouldering( "bouldering" );
static const efftype_id effect_countdown( "countdown" );
static const efftype_id effect_docile( "docile" );
static const efftype_id effect_downed( "downed" );
static const efftype_id effect_dragging( "dragging" );
static const efftype_id effect_grabbed( "grabbed" );
static const efftype_id effect_harnessed( "harnessed" );
static const efftype_id effect_led_by_leash( "led_by_leash" );
static const efftype_id effect_no_sight( "no_sight" );
static const efftype_id effect_operating( "operating" );
static const efftype_id effect_pacified( "pacified" );
static const efftype_id effect_pet("pet");
static const efftype_id effect_pushed( "pushed" );
static const efftype_id effect_stunned( "stunned" );

static const itype_id itype_pressurized_tank( "pressurized_tank" );

static const material_id material_iflesh( "iflesh" );

static const species_id species_FUNGUS( "FUNGUS" );
static const species_id species_ZOMBIE( "ZOMBIE" );

namespace
{
// The first phase shared path suffixes.  Phase two adds a target-rooted,
// incrementally expanded distance field.  It mirrors the useful part of CBN's
// destination Dijkstra maps while staying inside Breeze's existing tripoint
// and map::route interfaces.
constexpr int fallback_monster_path_radius = 24;
constexpr int cross_z_monster_path_radius = 96;
constexpr int reverse_field_radius = 64;
constexpr int reverse_field_cross_z_radius = 72;
constexpr std::size_t monster_route_cache_max_edges = 8192;
constexpr std::size_t monster_reverse_field_max_count = 24;
constexpr std::size_t monster_reverse_field_request_budget = 1024;
constexpr int monster_stair_route_bucket_size = 24;
constexpr int monster_stair_route_memory_turns = 60;
constexpr int monster_stair_route_target_tolerance = 4;
constexpr std::size_t monster_cross_z_route_default_turn_budget = 8;
constexpr std::size_t monster_priority_cross_z_route_default_turn_budget = 4;
constexpr std::size_t monster_reverse_field_default_turn_budget = 24576;
constexpr std::size_t monster_reverse_field_min_turn_budget = 2048;
constexpr std::size_t monster_route_search_default_turn_budget = 32;
constexpr std::size_t monster_route_search_min_turn_budget = 4;
// Tighten expensive planning earlier.  Once the budget is spent, confirmed
// hostile pursuit degrades to cheap local pressure instead of losing turns.
constexpr std::size_t monster_pathfinding_horde_size_step = 32;

int quantize_monster_bash_strength( int strength )
{
    if( strength <= 0 ) {
        return 0;
    }

    int result = 1;
    while( result < strength && result < 128 ) {
        result *= 2;
    }
    return result;
}

struct monster_route_cache_key {
    mtype_id type_id;
    tripoint target;
    int bash_strength_quanta = 0;
    bool can_open_doors = false;

    bool operator<( const monster_route_cache_key &rhs ) const
    {
        return std::tie( type_id, target.x, target.y, target.z,
                         bash_strength_quanta, can_open_doors ) <
               std::tie( rhs.type_id, rhs.target.x, rhs.target.y, rhs.target.z,
                         rhs.bash_strength_quanta, rhs.can_open_doors );
    }
};

struct monster_route_cache_entry {
    std::map<tripoint, tripoint> next_steps;
};

struct monster_z_route_cache_key {
    monster_route_cache_key route_key;
    int source_z = 0;

    bool operator<( const monster_z_route_cache_key &rhs ) const
    {
        if( route_key < rhs.route_key ) {
            return true;
        }
        if( rhs.route_key < route_key ) {
            return false;
        }
        return source_z < rhs.source_z;
    }
};

struct monster_z_route_plan {
    tripoint approach;
    tripoint transition;
};

struct monster_stair_route_cache_key {
    int source_z = 0;
    int target_z = 0;
    int target_x_bucket = 0;
    int target_y_bucket = 0;

    bool operator<( const monster_stair_route_cache_key &rhs ) const
    {
        return std::tie( source_z, target_z, target_x_bucket, target_y_bucket ) <
               std::tie( rhs.source_z, rhs.target_z, rhs.target_x_bucket,
                          rhs.target_y_bucket );
    }
};

struct monster_stair_route_cache_entry {
    tripoint_abs_ms target;
    tripoint_abs_ms approach;
    tripoint_abs_ms transition;
    time_point last_confirmed;
};

constexpr std::size_t monster_reverse_field_cell_count =
    static_cast<std::size_t>( MAPSIZE_X ) * static_cast<std::size_t>( MAPSIZE_Y );

std::size_t monster_reverse_field_index( const tripoint &p )
{
    return static_cast<std::size_t>( p.x ) * static_cast<std::size_t>( MAPSIZE_Y ) +
           static_cast<std::size_t>( p.y );
}

tripoint monster_reverse_field_point( int index, int z )
{
    return tripoint( index / MAPSIZE_Y, index % MAPSIZE_Y, z );
}

struct monster_reverse_field_node {
    int cost = 0;
    tripoint position;
};

struct monster_reverse_field_node_greater {
    bool operator()( const monster_reverse_field_node &lhs,
                     const monster_reverse_field_node &rhs ) const
    {
        return lhs.cost > rhs.cost;
    }
};

struct monster_reverse_field_entry {
    tripoint target;
    std::priority_queue<monster_reverse_field_node,
        std::vector<monster_reverse_field_node>,
        monster_reverse_field_node_greater> frontier;

    // Reverse fields are same-Z and use bounded local map coordinates. Dense
    // arrays avoid hashing a tripoint for every Dijkstra lookup. Generation
    // stamps also make reset O(1) instead of clearing three hash containers.
    std::vector<int> costs;
    std::vector<int> next_indices;
    std::vector<int> generations;
    std::vector<unsigned char> settled;
    int generation = 0;

    monster_reverse_field_entry()
        : costs( monster_reverse_field_cell_count, 0 ),
          next_indices( monster_reverse_field_cell_count, -1 ),
          generations( monster_reverse_field_cell_count, 0 ),
          settled( monster_reverse_field_cell_count, 0 )
    {
    }

    void reset( const tripoint &new_target )
    {
        target = new_target;
        frontier = decltype( frontier )();

        if( generation == std::numeric_limits<int>::max() ) {
            std::fill( generations.begin(), generations.end(), 0 );
            generation = 1;
        } else {
            ++generation;
        }

        const std::size_t target_index = monster_reverse_field_index( target );
        generations[target_index] = generation;
        settled[target_index] = 0;
        costs[target_index] = 0;
        next_indices[target_index] = static_cast<int>( target_index );
        frontier.push( { 0, target } );
    }

    bool is_settled( const tripoint &p ) const
    {
        const std::size_t index = monster_reverse_field_index( p );
        return generations[index] == generation && settled[index] != 0;
    }

    bool settle_node( const tripoint &p, int expected_cost )
    {
        const std::size_t index = monster_reverse_field_index( p );
        if( generations[index] != generation || settled[index] != 0 ||
            costs[index] != expected_cost ) {
            return false;
        }
        settled[index] = 1;
        return true;
    }

    bool relax( const tripoint &p, int new_cost, const tripoint &next )
    {
        const std::size_t index = monster_reverse_field_index( p );
        const int next_index = static_cast<int>( monster_reverse_field_index( next ) );

        if( generations[index] != generation ) {
            generations[index] = generation;
            settled[index] = 0;
            costs[index] = new_cost;
            next_indices[index] = next_index;
            return true;
        }
        if( settled[index] != 0 || new_cost >= costs[index] ) {
            return false;
        }

        costs[index] = new_cost;
        next_indices[index] = next_index;
        return true;
    }

    bool next_step( const tripoint &from, tripoint &to ) const
    {
        const std::size_t index = monster_reverse_field_index( from );
        if( generations[index] != generation ) {
            return false;
        }

        const int next_index = next_indices[index];
        if( next_index < 0 || next_index == static_cast<int>( index ) ) {
            return false;
        }

        to = monster_reverse_field_point( next_index, from.z );
        return true;
    }
};

// Fixed, tiny search pattern around a confirmed last-known position.  Four
// deterministic lanes spread a horde over nearby rooms without creating one
// unique reverse field per monster.
const std::array<point, 16> hostile_search_offsets = {{
    point( 2, 0 ), point( 0, 2 ), point( -2, 0 ), point( 0, -2 ),
    point( 3, 3 ), point( -3, 3 ), point( -3, -3 ), point( 3, -3 ),
    point( 5, 0 ), point( 0, 5 ), point( -5, 0 ), point( 0, -5 ),
    point( 6, 3 ), point( -3, 6 ), point( -6, -3 ), point( 3, -6 )
}};



// Search waypoints are clues, not omniscient destinations.  Keep them inside
// the open area visible from the last confirmed point.  Closed doors and walls
// require a new sound or sighting instead of being opened by a blind spiral.
std::optional<std::pair<int, tripoint_abs_ms>> next_hostile_search_waypoint(
    map &here, const tripoint_abs_ms &memory_origin,
    const std::optional<tripoint_abs_ms> &previous_sighting, int lane,
    int first_step, int search_count )
{
    const tripoint local_origin = here.getlocal( memory_origin );
    if( !here.inbounds( local_origin ) ) {
        return std::nullopt;
    }

    point heading = point_zero;
    bool has_heading = false;
    if( previous_sighting && previous_sighting->z() == memory_origin.z() ) {
        const tripoint local_previous = here.getlocal( *previous_sighting );
        if( here.inbounds( local_previous ) ) {
            heading.x = std::clamp( local_origin.x - local_previous.x, -1, 1 );
            heading.y = std::clamp( local_origin.y - local_previous.y, -1, 1 );
            has_heading = heading != point_zero;
        }
    }

    for( int step = std::max( 1, first_step ); step <= search_count; ++step ) {
        point offset;
        bool projected_from_sighting = false;

        if( has_heading && step <= 4 ) {
            const point side( -heading.y, heading.x );
            if( step == 1 ) {
                offset = point( heading.x * 2, heading.y * 2 );
            } else if( step == 2 ) {
                offset = point( heading.x * 4, heading.y * 4 );
            } else {
                const int side_sign = ( ( lane + step ) & 1 ) == 0 ? 1 : -1;
                offset = point( heading.x * 3 + side.x * 2 * side_sign,
                                heading.y * 3 + side.y * 2 * side_sign );
            }
            projected_from_sighting = true;
        } else {
            const int generic_step = has_heading ? step - 4 : step;
            const int index = ( std::max( 1, generic_step ) - 1 + lane * 2 ) %
                              search_count;
            offset = hostile_search_offsets[index];
        }

        const tripoint candidate = local_origin + offset;
        if( candidate.z != local_origin.z || !here.inbounds( candidate ) ||
            here.impassable( candidate ) ) {
            continue;
        }

        if( !projected_from_sighting ) {
            const int range =
                std::max( 1, rl_dist( local_origin, candidate ) + 1 );
            if( !here.clear_path( local_origin, candidate, range, 1, 100 ) ) {
                continue;
            }
        }

        return std::make_pair( step, here.getglobal( candidate ) );
    }
    return std::nullopt;
}

constexpr int hostile_transition_hint_radius = 2;
constexpr int witnessed_hostile_transition_hint_radius = 12;

bool is_monster_stair_transition( const map &here, const tripoint &approach,
                                  const tripoint &transition );
bool is_monster_stair_transition_usable( const monster &critter, const map &here,
        const tripoint &approach, const tripoint &transition );

std::optional<monster_z_route_plan> infer_hostile_memory_transition(
    monster &critter, map &here, const tripoint_abs_ms &memory_origin,
    int search_lane, int wanted_z_direction = 0,
    int hint_radius = hostile_transition_hint_radius )
{
    const tripoint local_memory = here.getlocal( memory_origin );
    if( !here.inbounds( local_memory ) ) {
        return std::nullopt;
    }

    // If the target is remembered on another floor, the useful stair approach
    // is on our current floor.  The old code searched memory_origin.z instead,
    // which could literally select a stair leading away from the target.
    const tripoint search_origin( local_memory.xy(), critter.posz() );
    if( !here.inbounds( search_origin ) ) {
        return std::nullopt;
    }

    if( wanted_z_direction == 0 && local_memory.z != search_origin.z ) {
        wanted_z_direction = local_memory.z > search_origin.z ? 1 : -1;
    }

    std::optional<monster_z_route_plan> best_plan;
    std::tuple<int, int, int, int, int> best_score(
        std::numeric_limits<int>::max(), 0, 0, 0, 0 );
    const bool can_bash = critter.bash_estimate() > 0;

    for( const tripoint &candidate :
         here.points_in_radius( search_origin, hint_radius ) ) {
        if( candidate.z != search_origin.z ) {
            continue;
        }

        const bool has_stair_down =
            here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, candidate );
        const bool has_stair_up =
            here.has_flag( ter_furn_flag::TFLAG_GOES_UP, candidate );
        const bool has_ramp_down =
            here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, candidate );
        const bool has_ramp_up =
            here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, candidate );

        for( const int dz : { -1, 1 } ) {
            if( wanted_z_direction != 0 && dz != wanted_z_direction ) {
                continue;
            }
            if( dz < 0 && !has_stair_down && !has_ramp_down ) {
                continue;
            }
            if( dz > 0 && !has_stair_up && !has_ramp_up ) {
                continue;
            }

            std::optional<tripoint> destination;
            const bool via_ramp = dz < 0 ? has_ramp_down : has_ramp_up;
            if( via_ramp ) {
                const tripoint ramp_destination( candidate.xy(), candidate.z + dz );
                if( here.inbounds( ramp_destination ) &&
                    here.valid_move( candidate, ramp_destination, can_bash, true, true ) ) {
                    destination = ramp_destination;
                }
            } else {
                bool rope_ladder = false;
                const std::optional<tripoint> stair_destination =
                    g->find_or_make_stairs( here, candidate.z + dz, rope_ladder,
                                            false, candidate );
                if( stair_destination && here.inbounds( *stair_destination ) ) {
                    const bool paired_stair = dz < 0 ?
                        here.has_flag( ter_furn_flag::TFLAG_GOES_UP, *stair_destination ) :
                        here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, *stair_destination );
                    if( paired_stair ) {
                        destination = *stair_destination;
                    }
                }
            }

            if( !destination ) {
                continue;
            }

            if( !is_monster_stair_transition_usable( critter, here, candidate,
                    *destination ) ) {
                continue;
            }

            // Nearest witnessed approach wins.  Direction bias remains useful
            // only when no direction was requested, such as speculative search
            // after reaching a same-Z last-known position.
            const int distance = rl_dist( search_origin, candidate );
            const int direction_bias = wanted_z_direction != 0 ? 0 :
                                       ( ( search_lane & 1 ) == 0 ?
                                         ( dz < 0 ? 0 : 1 ) :
                                         ( dz > 0 ? 0 : 1 ) );
            const int landing_distance = rl_dist( destination->xy(), local_memory.xy() );
            const std::tuple<int, int, int, int, int> score(
                distance, direction_bias, landing_distance,
                candidate.x, candidate.y );
            if( score < best_score ) {
                best_score = score;
                best_plan = monster_z_route_plan { candidate, *destination };
            }
        }
    }

    return best_plan;
}

bool hostile_disappearance_portal_is_plausible(
    map &here, const tripoint_abs_ms &memory_origin,
    const std::optional<tripoint_abs_ms> &previous_sighting )
{
    const tripoint local_origin = here.getlocal( memory_origin );
    if( !here.inbounds( local_origin ) ) {
        return false;
    }

    const auto is_vertical_portal = [&]( const tripoint &p ) {
        return here.has_flag( ter_furn_flag::TFLAG_GOES_UP, p ) ||
               here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, p ) ||
               here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, p ) ||
               here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, p );
    };

    if( is_vertical_portal( local_origin ) ) {
        return true;
    }

    if( !previous_sighting || previous_sighting->z() != memory_origin.z() ) {
        return false;
    }

    const tripoint local_previous = here.getlocal( *previous_sighting );
    if( !here.inbounds( local_previous ) ) {
        return false;
    }

    const point heading(
        std::clamp( local_origin.x - local_previous.x, -1, 1 ),
        std::clamp( local_origin.y - local_previous.y, -1, 1 ) );
    if( heading == point_zero ) {
        return false;
    }

    const tripoint ahead = local_origin + heading;
    return here.inbounds( ahead ) && is_vertical_portal( ahead );
}

std::optional<monster_z_route_plan> infer_recent_hostile_escape_transition(
    monster &critter, map &here, const tripoint_abs_ms &memory_origin,
    const std::optional<tripoint_abs_ms> &previous_sighting,
    int search_lane, int wanted_z_direction, int hint_radius )
{
    const tripoint local_origin = here.getlocal( memory_origin );
    if( !here.inbounds( local_origin ) ) {
        return std::nullopt;
    }

    point heading = point_zero;
    bool has_heading = false;
    if( previous_sighting && previous_sighting->z() == memory_origin.z() ) {
        const tripoint local_previous = here.getlocal( *previous_sighting );
        if( here.inbounds( local_previous ) ) {
            heading.x = std::clamp( local_origin.x - local_previous.x, -1, 1 );
            heading.y = std::clamp( local_origin.y - local_previous.y, -1, 1 );
            has_heading = heading != point_zero;
        }
    }

    std::optional<monster_z_route_plan> best_plan;
    std::tuple<int, int, int, int> best_score(
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max(), 0, 0 );

    for( const tripoint &candidate :
         here.points_in_radius( local_origin, hint_radius ) ) {
        if( candidate.z != local_origin.z ) {
            continue;
        }

        const bool has_requested_portal = wanted_z_direction < 0 ?
            ( here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, candidate ) ||
              here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, candidate ) ) :
            ( here.has_flag( ter_furn_flag::TFLAG_GOES_UP, candidate ) ||
              here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, candidate ) );
        if( !has_requested_portal ) {
            continue;
        }

        const int dx = candidate.x - local_origin.x;
        const int dy = candidate.y - local_origin.y;
        const int distance = rl_dist( local_origin, candidate );

        int evidence_penalty = distance == 0 ? 0 : 8;
        if( has_heading ) {
            const int forward = dx * heading.x + dy * heading.y;
            const int lateral = std::abs( dx * heading.y - dy * heading.x );

            // A sampled last sighting can be several tiles before the stair,
            // but a portal well behind the observed heading is not a credible
            // disappearance exit. Smart pursuit gets a slightly wider cone.
            const int lateral_limit = std::max( 2, hint_radius / 2 );
            if( forward < -1 || lateral > lateral_limit ) {
                continue;
            }
            evidence_penalty = std::max( 0, lateral * 3 - forward );
        } else if( distance > 2 ) {
            // With no movement heading, only trust a portal almost on top of
            // the last real sighting.
            continue;
        }

        const tripoint_abs_ms candidate_abs = here.getglobal( candidate );
        const std::optional<monster_z_route_plan> plan =
            infer_hostile_memory_transition(
                critter, here, candidate_abs, search_lane,
                wanted_z_direction, 0 );
        if( !plan ) {
            continue;
        }

        const std::tuple<int, int, int, int> score(
            evidence_penalty, distance, candidate.x, candidate.y );
        if( score < best_score ) {
            best_score = score;
            best_plan = plan;
        }
    }

    return best_plan;
}

std::optional<time_point> monster_path_cache_turn;
std::map<monster_route_cache_key, monster_route_cache_entry> monster_route_cache;
std::map<monster_z_route_cache_key, monster_z_route_plan> monster_z_route_cache;
std::map<monster_route_cache_key, std::unique_ptr<monster_reverse_field_entry>>
monster_reverse_fields;
std::vector<std::unique_ptr<monster_reverse_field_entry>> monster_reverse_field_pool;
std::map<monster_stair_route_cache_key, monster_stair_route_cache_entry>
monster_stair_route_cache;
std::size_t monster_route_cache_edges = 0;
std::size_t monster_reverse_field_nodes = 0;
std::size_t monster_cross_z_route_search_turn_budget = monster_cross_z_route_default_turn_budget;
std::size_t monster_cross_z_route_searches = 0;
std::size_t monster_priority_cross_z_route_searches = 0;
std::size_t monster_reverse_field_turn_budget = monster_reverse_field_default_turn_budget;
std::size_t monster_route_search_turn_budget = monster_route_search_default_turn_budget;
std::size_t monster_route_searches = 0;
std::optional<bool> monster_improved_pathfinding_mode;
int monster_pathfinding_mode_generation = 1;

void clear_monster_path_caches()
{
    monster_route_cache.clear();
    monster_z_route_cache.clear();
    monster_route_cache_edges = 0;
    monster_reverse_field_nodes = 0;
    monster_cross_z_route_searches = 0;
    monster_priority_cross_z_route_searches = 0;
    monster_route_searches = 0;

    for( auto &field_pair : monster_reverse_fields ) {
        monster_reverse_field_pool.emplace_back( std::move( field_pair.second ) );
    }
    monster_reverse_fields.clear();
}

int monster_stair_route_bucket( int coordinate )
{
    return static_cast<int>( std::floor( static_cast<double>( coordinate ) /
                                         monster_stair_route_bucket_size ) );
}

monster_stair_route_cache_key make_monster_stair_route_cache_key(
    const tripoint_abs_ms &target, int source_z )
{
    return { source_z, target.z(), monster_stair_route_bucket( target.x() ),
             monster_stair_route_bucket( target.y() ) };
}

bool is_monster_stair_transition( const map &here, const tripoint &approach,
                                  const tripoint &transition )
{
    const int dz = transition.z - approach.z;
    if( dz == 1 ) {
        return ( here.has_flag( ter_furn_flag::TFLAG_GOES_UP, approach ) &&
                 here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, transition ) ) ||
               here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, approach );
    }
    if( dz == -1 ) {
        return ( here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, approach ) &&
                 here.has_flag( ter_furn_flag::TFLAG_GOES_UP, transition ) ) ||
               here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, approach );
    }
    return false;
}

bool is_monster_stair_transition_usable( const monster &critter, const map &here,
        const tripoint &approach, const tripoint &transition )
{
    if( !is_monster_stair_transition( here, approach, transition ) ) {
        return false;
    }

    const int dz = transition.z - approach.z;
    const bool via_ramp = dz > 0 ?
                          here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, approach ) :
                          here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, approach );
    if( via_ramp ) {
        return true;
    }

    // CBN/CCB treat ordinary paired stairs as usable by every monster.  Only
    // a difficult ladder/vertical transition needs the monster's climbing
    // ability; do not let a cached route accidentally grant wall climbing.
    return !here.has_flag( ter_furn_flag::TFLAG_DIFFICULT_Z, approach ) ||
           critter.can_climb();
}

void remember_monster_stair_route( map &here, const tripoint_abs_ms &target,
                                   const tripoint &approach, const tripoint &transition )
{
    if( !is_monster_stair_transition( here, approach, transition ) ) {
        return;
    }

    constexpr std::size_t max_stair_route_entries = 64;
    const monster_stair_route_cache_key key = make_monster_stair_route_cache_key(
            target, approach.z );
    if( monster_stair_route_cache.find( key ) == monster_stair_route_cache.end() &&
        monster_stair_route_cache.size() >= max_stair_route_entries ) {
        const auto oldest = std::min_element( monster_stair_route_cache.begin(),
        monster_stair_route_cache.end(), []( const auto &lhs, const auto &rhs ) {
            return lhs.second.last_confirmed < rhs.second.last_confirmed;
        } );
        monster_stair_route_cache.erase( oldest );
    }

    monster_stair_route_cache[key] = {
        target, here.getglobal( approach ), here.getglobal( transition ), calendar::turn
    };
}

std::optional<monster_z_route_plan> get_remembered_monster_stair_route(
    const monster &critter, map &here, const tripoint_abs_ms &target, int source_z )
{
    const monster_stair_route_cache_key key = make_monster_stair_route_cache_key(
            target, source_z );
    const auto iter = monster_stair_route_cache.find( key );
    if( iter == monster_stair_route_cache.end() ) {
        return std::nullopt;
    }
    if( calendar::turn > iter->second.last_confirmed +
        time_duration::from_turns( monster_stair_route_memory_turns ) ) {
        monster_stair_route_cache.erase( iter );
        return std::nullopt;
    }
    if( rl_dist( target, iter->second.target ) >
        monster_stair_route_target_tolerance ) {
        return std::nullopt;
    }

    const tripoint approach = here.getlocal( iter->second.approach );
    const tripoint transition = here.getlocal( iter->second.transition );
    if( approach.z != source_z || transition.z != target.z() ||
        !here.inbounds( approach ) || !here.inbounds( transition ) ||
        !is_monster_stair_transition_usable( critter, here, approach, transition ) ) {
        monster_stair_route_cache.erase( iter );
        return std::nullopt;
    }

    iter->second.last_confirmed = calendar::turn;
    return monster_z_route_plan { approach, transition };
}

void update_monster_pathfinding_budgets()
{
    std::size_t monster_count = 0;
    if( g != nullptr ) {
        for( const monster &mon : g->all_monsters() ) {
            ( void )mon;
            ++monster_count;
        }
    }
    const std::size_t horde_factor = std::max<std::size_t>( 1,
            ( monster_count + monster_pathfinding_horde_size_step - 1 ) /
            monster_pathfinding_horde_size_step );
    monster_reverse_field_turn_budget = std::max<std::size_t>( monster_reverse_field_min_turn_budget,
            monster_reverse_field_default_turn_budget / horde_factor );
    monster_route_search_turn_budget = std::max<std::size_t>( monster_route_search_min_turn_budget,
            monster_route_search_default_turn_budget / horde_factor );
    // Reserve a small, separate lane for cross-Z searches.  A horde must not
    // be able to spend the entire ordinary route budget before it discovers a
    // usable stair portal.
    monster_cross_z_route_search_turn_budget = monster_cross_z_route_default_turn_budget;
}

void refresh_monster_path_caches()
{
    if( !monster_path_cache_turn || *monster_path_cache_turn != calendar::turn ) {
        monster_path_cache_turn = calendar::turn;
        clear_monster_path_caches();
        update_monster_pathfinding_budgets();
    }
}

int refresh_monster_pathfinding_mode( bool improved )
{
    if( !monster_improved_pathfinding_mode ||
        *monster_improved_pathfinding_mode != improved ) {
        monster_improved_pathfinding_mode = improved;
        ++monster_pathfinding_mode_generation;
        monster_path_cache_turn.reset();
        clear_monster_path_caches();
        monster_stair_route_cache.clear();
    }
    return monster_pathfinding_mode_generation;
}

monster_reverse_field_entry *get_monster_reverse_field(
    const monster_route_cache_key &key )
{
    const auto existing = monster_reverse_fields.find( key );
    if( existing != monster_reverse_fields.end() ) {
        return existing->second.get();
    }
    if( monster_reverse_fields.size() >= monster_reverse_field_max_count ) {
        return nullptr;
    }

    std::unique_ptr<monster_reverse_field_entry> field;
    if( monster_reverse_field_pool.empty() ) {
        field = std::make_unique<monster_reverse_field_entry>();
    } else {
        field = std::move( monster_reverse_field_pool.back() );
        monster_reverse_field_pool.pop_back();
    }
    field->reset( key.target );
    monster_reverse_field_entry *result = field.get();
    monster_reverse_fields.emplace( key, std::move( field ) );
    return result;
}
} // namespace

bool monster::is_immune_field( const field_type_id &fid ) const
{
    if( fid == fd_fungal_haze ) {
        return has_flag( MF_NO_BREATHE ) || type->in_species( species_FUNGUS );
    }
    if( fid == fd_fungicidal_gas ) {
        return !type->in_species( species_FUNGUS );
    }
    if( fid == fd_insecticidal_gas ) {
        return !made_of( material_iflesh ) || has_flag( MF_INSECTICIDEPROOF );
    }
    if( fid == fd_web ) {
        return has_flag( MF_WEBWALK );
    }
    if( fid == fd_sludge || fid == fd_sap ) {
        return flies();
    }
    const field_type &ft = fid.obj();
    if( ft.has_fume ) {
        return has_flag( MF_NO_BREATHE );
    }
    if( ft.has_acid ) {
        return has_flag( MF_ACIDPROOF ) || flies();
    }
    if( ft.has_fire ) {
        return has_flag( MF_FIREPROOF );
    }
    if( ft.has_elec ) {
        return has_flag( MF_ELECTRIC );
    }
    if( ft.immune_mtypes.count( type->id ) > 0 ) {
        return true;
    }
    // No specific immunity was found, so fall upwards
    return Creature::is_immune_field( fid );
}

static bool z_is_valid( int z )
{
    return z >= -OVERMAP_DEPTH && z <= OVERMAP_HEIGHT;
}

bool monster::will_move_to( const tripoint &p ) const
{
    map &here = get_map();
    if( here.impassable( p ) ) {
        if( digging() ) {
            if( !here.has_flag( ter_furn_flag::TFLAG_BURROWABLE, p ) ) {
                return false;
            }
        } else if( !( can_climb() && here.has_flag( ter_furn_flag::TFLAG_CLIMBABLE, p ) ) ) {
            return false;
        }
    }

    if( ( !can_submerge() && !flies() ) && here.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, p ) ) {
        return false;
    }

    if( digs() && !here.has_flag( ter_furn_flag::TFLAG_DIGGABLE, p ) &&
        !here.has_flag( ter_furn_flag::TFLAG_BURROWABLE, p ) ) {
        return false;
    }

    if( has_flag( MF_AQUATIC ) && (
            !here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, p ) ||
            // AQUATIC (confined to water) monster avoid vehicles, unless they are already underneath one
            ( here.veh_at( p ) && !here.veh_at( pos() ) )
        ) ) {
        return false;
    }

    if( has_flag( MF_SUNDEATH ) && g->is_in_sunlight( p ) ) {
        return false;
    }

    if( get_size() > creature_size::medium &&
        here.has_flag_ter( ter_furn_flag::TFLAG_SMALL_PASSAGE, p ) ) {
        return false; // if a large critter, can't move through tight passages
    }

    // Various avoiding behaviors.

    bool avoid_fire = has_flag( MF_AVOID_FIRE );
    bool avoid_fall = has_flag( MF_AVOID_FALL );
    bool avoid_simple = has_flag( MF_AVOID_DANGER_1 );
    bool avoid_complex = has_flag( MF_AVOID_DANGER_2 );
    /*
     * Because some avoidance behaviors are supersets of others,
     * we can cascade through the implications. Complex implies simple,
     * and simple implies fire and fall.
     * unfortunately, fall does not necessarily imply fire, nor the converse.
     */
    if( avoid_complex ) {
        avoid_simple = true;
    }
    if( avoid_simple ) {
        avoid_fire = true;
        avoid_fall = true;
    }

    // technically this will shortcut in evaluation from fire or fall
    // before hitting simple or complex but this is more explicit
    if( avoid_fire || avoid_fall || avoid_simple || avoid_complex ) {
        const ter_id target = here.ter( p );

        // Don't enter lava if we have any concept of heat being bad
        if( avoid_fire && target == t_lava ) {
            return false;
        }

        if( avoid_fall ) {
            // Don't throw ourselves off cliffs if we have a concept of falling
            if( !here.has_floor_or_support( p ) && !flies() ) {
                return false;
            }

            // Don't enter open pits ever unless tiny, can fly or climb well
            if( !( type->size == creature_size::tiny || can_climb() ) &&
                ( target == t_pit || target == t_pit_spiked || target == t_pit_glass ) ) {
                return false;
            }
        }

        // Some things are only avoided if we're not attacking
        if( attitude( &get_player_character() ) != MATT_ATTACK ) {
            // Sharp terrain is ignored while attacking
            if( avoid_simple && here.has_flag( ter_furn_flag::TFLAG_SHARP, p ) &&
                !( type->size == creature_size::tiny || flies() ||
                   get_armor_cut( bodypart_id( "torso" ) ) >= 10 ) ) {
                return false;
            }
        }

        const field &target_field = here.field_at( p );

        // Higher awareness is needed for identifying these as threats.
        if( avoid_complex ) {
            // Don't enter any dangerous fields
            if( is_dangerous_fields( target_field ) ) {
                return false;
            }
            // Don't step on any traps (if we can see)
            const trap &target_trap = here.tr_at( p );
            if( has_flag( MF_SEES ) && !target_trap.is_benign() && here.has_floor( p ) ) {
                return false;
            }
        }

        // Without avoid_complex, only fire and electricity are checked for field avoidance.
        if( avoid_fire && target_field.find_field( fd_fire ) && !is_immune_field( fd_fire ) ) {
            return false;
        }
        if( avoid_simple && target_field.find_field( fd_electricity ) &&
            !is_immune_field( fd_electricity ) ) {
            return false;
        }
    }

    return true;
}

bool monster::can_reach_to( const tripoint &p ) const
{
    map &here = get_map();
    if( p.z > pos().z && z_is_valid( pos().z ) ) {
        if( here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, tripoint( p.xy(), p.z - 1 ) ) ) {
            return true;
        }
        if( here.has_flag( ter_furn_flag::TFLAG_DIFFICULT_Z, pos() ) && !can_climb() ) {
            return false;
        }
        if( !here.has_flag( ter_furn_flag::TFLAG_GOES_UP, pos() ) &&
            !here.has_flag( ter_furn_flag::TFLAG_NO_FLOOR, p ) ) {
            // can't go through the roof
            return false;
        }
    } else if( p.z < pos().z && z_is_valid( pos().z ) ) {
        if( here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, pos() ) ) {
            return true;
        }
        if( here.has_flag( ter_furn_flag::TFLAG_DIFFICULT_Z, pos() ) && !can_climb() ) {
            return false;
        }
        if( !here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, pos() ) ) {
            // can't go through the floor
            // you would fall anyway if there was no floor, so no need to check for that here
            return false;
        }
    }
    return true;
}

bool monster::can_move_to( const tripoint &p ) const
{
    return can_reach_to( p ) && will_move_to( p );
}

float monster::rate_target( Creature &c, float best, bool smart ) const
{
    const FastDistanceApproximation d = rl_dist_fast( pos(), c.pos() );
    if( d <= 0 ) {
        return FLT_MAX;
    }

    // Check a very common and cheap case first
    if( !smart && d >= best ) {
        return FLT_MAX;
    }

    if( !sees( c ) ) {
        return FLT_MAX;
    }

    if( !smart ) {
        return static_cast<int>( d );
    }

    float power = c.power_rating();
    monster *mon = dynamic_cast< monster * >( &c );
    // Their attitude to us and not ours to them, so that bobcats won't get gunned down
    if( mon != nullptr && mon->attitude_to( *this ) == Attitude::HOSTILE ) {
        power += 2;
    }

    if( power > 0 ) {
        return static_cast<int>( d ) / power;
    }

    return FLT_MAX;
}

/** This is lazily evaluated in monster::plan(). Each monster in a zone is visited
 * as it flood fills, then the zone number is incremented. At the end all monsters in
 * the same zone will have the same zone number assigned, which can be used to have monsters in
 * different zones ignore each other very cheaply.
 */
static void flood_fill_zone(Creature& origin)
{
    static int zone_number = 1;
    static int zone_tick = 1;
    map& here = get_map();
    if (here.get_visitable_zones_cache_dirty()) {
        zone_tick = zone_tick > 0 ? -1 : 1;
        here.set_visitable_zones_cache_dirty(false);
        zone_number = 1;
    }
    // This check insures we only flood fill when the target monster has an uninitialized zone,
    // or if it has a zone from last turn.  In other words it only triggers on
    // the first monster in a zone each turn. We can detect this because the sign
    // of the zone numbers changes on every invalidation.
    int old_zone = origin.get_reachable_zone();
    // Compare with zone_tick == old_zone && old_zone != 0
    if ((zone_tick > 0 && old_zone > 0) ||
        (zone_tick < 0 && old_zone < 0)) {
        return;
    }
    creature_tracker& tracker = get_creature_tracker();

    ff::flood_fill_visit_10_connected(origin.pos_bub(),
        [&here](const tripoint_bub_ms& loc, int direction) {
            if (direction == 0) {
                return here.inbounds(loc) && (here.is_transparent_wo_fields(loc.raw()) ||
                    here.passable(loc));
            }
            if (direction == 1) {
                const maptile& up = here.maptile_at(loc);
                const ter_t& up_ter = up.get_ter_t();
                if (up_ter.id.is_null()) {
                    return false;
                }
                if (((up_ter.movecost != 0 && up.get_furn_t().movecost >= 0) ||
                    here.is_transparent_wo_fields(loc.raw())) &&
                    (up_ter.has_flag(ter_furn_flag::TFLAG_NO_FLOOR) ||
                        up_ter.has_flag(ter_furn_flag::TFLAG_GOES_DOWN))) {
                    return true;
                }
            }
            if (direction == -1) {
                const maptile& up = here.maptile_at(loc + tripoint_above);
                const ter_t& up_ter = up.get_ter_t();
                if (up_ter.id.is_null()) {
                    return false;
                }
                const maptile& down = here.maptile_at(loc);
                const ter_t& down_ter = up.get_ter_t();
                if (down_ter.id.is_null()) {
                    return false;
                }
                if (((down_ter.movecost != 0 && down.get_furn_t().movecost >= 0) ||
                    here.is_transparent_wo_fields(loc.raw())) &&
                    (up_ter.has_flag(ter_furn_flag::TFLAG_NO_FLOOR) ||
                        up_ter.has_flag(ter_furn_flag::TFLAG_GOES_DOWN))) {
                    return true;
                }
            }
            return false;
        },
        [&tracker](const tripoint_bub_ms& loc) {
            Creature* creature = tracker.creature_at<Creature>(loc,true);
            if (creature) {
                creature->set_reachable_zone(zone_number * zone_tick);
            }
        });
    if (zone_number == std::numeric_limits<int>::max()) {
        zone_number = 1;
    }
    else {
        zone_number++;
    }
}




void monster::plan()
{
    const auto &factions = g->critter_tracker->factions();

    // Bots are more intelligent than most living stuff
    bool smart_planning = has_flag( MF_PRIORITIZE_TARGETS );
    const bool improved_pathfinding =
        get_option<std::string>( "MONSTER_PATHFINDING" ) != "classic";
    // plan() owns this transient bit. Ordinary patrol, pet, leash and
    // work destinations must never inherit aggressive route-failure fallback.
    hostile_pursuit_active = false;
    if( !improved_pathfinding ) {
        witnessed_hostile_transition_origin.reset();
        witnessed_hostile_transition_destination.reset();
        witnessed_hostile_transition_memory_turns = 0;
        hostile_memory_origin_position.reset();
        hostile_memory_search_origin_position.reset();
        hostile_memory_portal_approach.reset();
        hostile_memory_portal_transition.reset();
        hostile_memory_target_is_avatar = false;
        previous_hostile_sighting_position.reset();
        if( last_hostile_target_position ) {
            unset_dest();
        }
        last_hostile_target_position.reset();
        hostile_target_memory_turns = 0;
        hostile_search_turns = 0;
        hostile_search_step = 0;
        hostile_search_waypoint_turns = 0;
        hostile_search_lane = 0;
        hostile_transition_attempts = 0;
        hostile_search_deadline.reset();
        last_hostile_sighting_turn.reset();
    } else {
        if( hostile_target_memory_turns > 0 ) {
            --hostile_target_memory_turns;
        }
        if( witnessed_hostile_transition_memory_turns > 0 ) {
            --witnessed_hostile_transition_memory_turns;
        }
        if( witnessed_hostile_transition_memory_turns <= 0 ) {
            witnessed_hostile_transition_origin.reset();
            witnessed_hostile_transition_destination.reset();
            witnessed_hostile_transition_memory_turns = 0;
        }
        if( hostile_search_turns > 0 ) {
            --hostile_search_turns;
        }
        const bool search_deadline_expired = hostile_search_deadline &&
            calendar::turn >= *hostile_search_deadline;
        if( hostile_target_memory_turns <= 0 ||
            ( hostile_search_step > 0 &&
              ( hostile_search_turns <= 0 || search_deadline_expired ) ) ) {
            if( last_hostile_target_position ) {
                unset_dest();
            }
            last_hostile_target_position.reset();
            hostile_memory_origin_position.reset();
            hostile_memory_search_origin_position.reset();
            hostile_memory_portal_approach.reset();
            hostile_memory_portal_transition.reset();
            hostile_memory_target_is_avatar = false;
            previous_hostile_sighting_position.reset();
            hostile_target_memory_turns = 0;
            hostile_search_turns = 0;
            hostile_search_step = 0;
            hostile_search_waypoint_turns = 0;
            hostile_search_lane = 0;
            hostile_transition_attempts = 0;
            hostile_search_deadline.reset();
        }
    }
    Creature *target = nullptr;
    int max_sight_range = std::max( type->vision_day, type->vision_night );
    // 8.6f is rating for tank drone 60 tiles away, moose 16 or boomer 33
    float dist = !smart_planning ? max_sight_range : 8.6f;
    bool fleeing = false;
    bool docile = friendly != 0 && has_effect( effect_docile );

    const bool angers_hostile_weak = type->has_anger_trigger( mon_trigger::HOSTILE_WEAK );
    const bool fears_hostile_weak = type->has_fear_trigger( mon_trigger::HOSTILE_WEAK );
    const bool placate_hostile_weak = type->has_placate_trigger( mon_trigger::HOSTILE_WEAK );
    const int angers_hostile_near = type->has_anger_trigger( mon_trigger::HOSTILE_CLOSE ) ? 5 : 0;
    const int angers_hostile_seen = type->has_anger_trigger( mon_trigger::HOSTILE_SEEN ) ? rng( 0,
                                    2 ) : 0;
    const int angers_mating_season = type->has_anger_trigger( mon_trigger::MATING_SEASON ) ? 3 : 0;
    const int angers_cub_threatened = type->has_anger_trigger( mon_trigger::PLAYER_NEAR_BABY ) ? 8 : 0;
    const int fears_hostile_near = type->has_fear_trigger( mon_trigger::HOSTILE_CLOSE ) ? 5 : 0;
    const int fears_hostile_seen = type->has_fear_trigger( mon_trigger::HOSTILE_SEEN ) ? rng( 0,
                                   2 ) : 0;

    map &here = get_map();
    std::bitset<OVERMAP_LAYERS> seen_levels = here.get_inter_level_visibility( pos().z );
    bool group_morale = has_flag( MF_GROUP_MORALE ) && morale < type->morale;
    bool swarms = has_flag( MF_SWARMS );
    monster_attitude mood = attitude();
    Character &player_character = get_player_character();
    // Clear only a stale destination aimed directly at the player.  Do not erase
    // combat, fleeing, patrol or ordinary roaming destinations.
    const bool pet_without_auto_follow = is_pet() && !is_pet_follow() &&
                                         !has_effect( effect_led_by_leash );
    if( pet_without_auto_follow && get_dest() == player_character.get_location() ) {
        unset_dest();
    }
    // If we can see the player, move toward them or flee.
    if( friendly == 0 && seen_levels.test( player_character.pos().z + OVERMAP_DEPTH ) &&
        sees( player_character ) ) {
        dist = rate_target( player_character, dist, smart_planning );
        fleeing = fleeing || is_fleeing( player_character );
        target = &player_character;
        if( !fleeing && anger <= 20 ) {
            anger += angers_hostile_seen;
        }
        if( !fleeing ) {
            morale -= fears_hostile_seen;
            // Decide that the player is too annoying, less likely than the other triggers
            if( angers_hostile_seen && x_in_y( anger, 200 ) ) {
                add_msg_debug( debugmode::DF_MONSTER, "%s's character aggro triggered by seeing you", name() );
                aggro_character = true;
            }
        }
        if( dist <= 5 ) {
            if( anger <= 30 ) {
                anger += angers_hostile_near;
            }
            if( angers_hostile_near && x_in_y( anger, 100 ) ) {
                add_msg_debug( debugmode::DF_MONSTER, "%s's character aggro triggered by proximity", name() );
                aggro_character = true;
            }
            morale -= fears_hostile_near;
            if( angers_mating_season > 0  && anger <= 30 ) {
                bool mating_angry = false;
                season_type season = season_of_year( calendar::turn );
                for( const std::string &elem : type->baby_flags ) {
                    if( ( season == SUMMER && elem == "SUMMER" ) ||
                        ( season == WINTER && elem == "WINTER" ) ||
                        ( season == SPRING && elem == "SPRING" ) ||
                        ( season == AUTUMN && elem == "AUTUMN" ) ) {
                        mating_angry = true;
                        break;
                    }
                }
                if( mating_angry ) {
                    anger += angers_mating_season;
                    if( x_in_y( anger, 100 ) ) {
                        add_msg_debug( debugmode::DF_MONSTER, "%s's character aggro triggered by season", name() );
                        aggro_character = true;
                    }
                }
            }
        }
        if( angers_cub_threatened > 0 ) {
            for( monster &tmp : g->all_monsters() ) {
                if( type->baby_monster == tmp.type->id ) {
                    // baby nearby; is the player too close?
                    dist = tmp.rate_target( player_character, dist, smart_planning );
                    if( dist <= 3 ) {
                        //proximity to baby; monster gets furious and less likely to flee
                        anger += angers_cub_threatened;
                        morale += angers_cub_threatened / 2;
                        add_msg_debug( debugmode::DF_MONSTER, "%s's character aggro triggered by threatening %s", name(),
                                       tmp.name() );
                        aggro_character = true;
                    }
                }
            }
        }
    } else if( friendly != 0 && !docile ) {
        for( monster &tmp : g->all_monsters() ) {
            if( tmp.friendly == 0 && tmp.attitude_to( *this ) == Attitude::HOSTILE &&
                seen_levels.test( tmp.pos().z + OVERMAP_DEPTH ) ) {
                float rating = rate_target( tmp, dist, smart_planning );
                if( rating < dist ) {
                    target = &tmp;
                    dist = rating;
                }
            }
        }
    }

    if( docile ) {
        if( friendly != 0 && target != nullptr ) {
            set_dest( target->get_location() );
        }

        return;
    }

    int valid_targets = ( target == nullptr ) ? 0 : 1;
    for( npc &who : g->all_npcs() ) {
        mf_attitude faction_att = faction.obj().attitude( who.get_monster_faction() );
        if( faction_att == MFA_NEUTRAL || faction_att == MFA_FRIENDLY ) {
            continue;
        }
        if( !seen_levels.test( who.pos().z + OVERMAP_DEPTH ) ) {
            continue;
        }

        float rating = rate_target( who, dist, smart_planning );
        bool fleeing_from = is_fleeing( who );
        if( rating == dist && ( fleeing || attitude( &who ) == MATT_ATTACK ||
                                attitude( &who ) == MATT_FOLLOW ) ) {
            ++valid_targets;
            if( one_in( valid_targets ) ) {
                target = &who;
            }
        }
        // Switch targets if closer and hostile or scarier than current target
        if( ( rating < dist && fleeing ) ||
            ( faction_att == MFA_HATE ) ||
            ( rating < dist && attitude( &who ) == MATT_ATTACK ) ||
            ( !fleeing && fleeing_from ) ) {
            target = &who;
            dist = rating;
            valid_targets = 1;
        }
        fleeing = fleeing || fleeing_from;
        if( rating <= 5 ) {
            if( anger <= 30 ) {
                anger += angers_hostile_near;
            }
            if( angers_hostile_near && x_in_y( anger, 100 ) ) {
                add_msg_debug( debugmode::DF_MONSTER, "%s's character aggro triggered by proximity to %s", name(),
                               who.name );
                aggro_character = true;
            }
            morale -= fears_hostile_near;
            if( angers_mating_season > 0 && anger <= 30 ) {
                bool mating_angry = false;
                season_type season = season_of_year( calendar::turn );
                for( const std::string &elem : type->baby_flags ) {
                    if( ( season == SUMMER && elem == "SUMMER" ) ||
                        ( season == WINTER && elem == "WINTER" ) ||
                        ( season == SPRING && elem == "SPRING" ) ||
                        ( season == AUTUMN && elem == "AUTUMN" ) ) {
                        mating_angry = true;
                        break;
                    }
                }
                if( mating_angry ) {
                    anger += angers_mating_season;
                    if( x_in_y( anger, 100 ) ) {
                        add_msg_debug( debugmode::DF_MONSTER, "%s's character aggro triggered by season", name() );
                        aggro_character = true;
                    }
                }
            }
        }
        if( !fleeing && anger <= 20 && valid_targets != 0 ) {
            anger += angers_hostile_seen;
        }
        if( !fleeing && valid_targets != 0 ) {
            morale -= fears_hostile_seen;
            // Decide that characters are too annoying
            if( angers_hostile_seen && x_in_y( anger, 200 ) ) {
                add_msg_debug( debugmode::DF_MONSTER, "%s's character aggro triggered by seeing %s", name(),
                               who.name );
                aggro_character = true;
            }
        }
    }

    fleeing = fleeing || ( mood == MATT_FLEE );
    // Throttle monster thinking, if there are no apparent threats, stop paying attention.
    constexpr int max_turns_for_rate_limiting = 1800;
    constexpr double max_turns_to_skip = 600.0;
    // Outputs a range from 0.0 - 1.0.
    float rate_limiting_factor = 1.0 - logarithmic_range( 0, max_turns_for_rate_limiting,
                                 turns_since_target );
    int turns_to_skip = max_turns_to_skip * rate_limiting_factor;
    if( friendly == 0 && ( turns_to_skip == 0 || turns_since_target % turns_to_skip == 0 ) ) {
        flood_fill_zone( *this );
        for( const auto &fac_list : factions ) {
            mf_attitude faction_att = faction.obj().attitude( fac_list.first );
            if( faction_att == MFA_NEUTRAL || faction_att == MFA_FRIENDLY ) {
                continue;
            }

            for( const auto &fac : fac_list.second ) {
                if( !seen_levels.test( fac.first + OVERMAP_DEPTH ) ) {
                    continue;
                }
                for( const weak_ptr_fast<monster> &weak : fac.second ) {
                    const shared_ptr_fast<monster> shared = weak.lock();
                    if( !shared ) {
                        continue;
                    }
                    monster &mon = *shared;
                    if (get_reachable_zone() != mon.get_reachable_zone()) {
                        continue;
                    }
                    float rating = rate_target( mon, dist, smart_planning );
                    if( rating == dist ) {
                        ++valid_targets;
                        if( one_in( valid_targets ) ) {
                            target = &mon;
                        }
                    }
                    if( rating < dist ) {
                        target = &mon;
                        dist = rating;
                        valid_targets = 1;
                    }
                    if( rating <= 5 ) {
                        if( anger <= 30 ) {
                            anger += angers_hostile_near;
                        }
                        morale -= fears_hostile_near;
                    }
                    if( !fleeing && anger <= 20 && valid_targets != 0 ) {
                        anger += angers_hostile_seen;
                    }
                    if( !fleeing && valid_targets != 0 ) {
                        morale -= fears_hostile_seen;
                    }
                }
            }
        }
    }
    if( target == nullptr ) {
        // Just avoiding overflow.
        turns_since_target = std::min( turns_since_target + 1, max_turns_for_rate_limiting );
    } else {
        turns_since_target = 0;
    }

    // Friendly monsters here
    // Avoid for hordes of same-faction stuff or it could get expensive
    const auto actual_faction = friendly == 0 ? faction : STATIC( mfaction_str_id( "player" ) );
    const auto &myfaction_iter = factions.find( actual_faction );
    if( myfaction_iter == factions.end() ) {
        /*DebugLog( D_ERROR, D_GAME ) << disp_name() << " tried to find faction "
                                    << actual_faction.id().str()
                                    << " which wasn't loaded in game::monmove";*/
        swarms = false;
        group_morale = false;
    }
    swarms = swarms && target == nullptr; // Only swarm if we have no target
    if( group_morale || swarms ) {
        for( const auto &fac : myfaction_iter->second ) {
            if( !seen_levels.test( fac.first + OVERMAP_DEPTH ) ) {
                continue;
            }
            for( const weak_ptr_fast<monster> &weak : fac.second ) {
                const shared_ptr_fast<monster> shared = weak.lock();
                if( !shared ) {
                    continue;
                }
                monster &mon = *shared;
                float rating = rate_target( mon, dist, smart_planning );
                if( group_morale && rating <= 10 ) {
                    morale += 10 - rating;
                }
                if( swarms ) {
                    if( rating < 5 ) { // Too crowded here
                        wander_pos = get_location() + point( rng( 1, 3 ), rng( 1, 3 ) );
                        wandf = 2;
                        target = nullptr;
                        // Swarm to the furthest ally you can see
                    } else if( rating < FLT_MAX && rating > dist && wandf <= 0 ) {
                        target = &mon;
                        dist = rating;
                    }
                }
            }
        }
    }

    // Operating monster keep you safe while they operate, how nice....
    if( type->has_special_attack( "OPERATE" ) ) {
        if( has_effect( effect_operating ) ) {
            friendly = 100;
            for( Creature *critter : here.get_creatures_in_radius( pos(), 6 ) ) {
                monster *mon = dynamic_cast<monster *>( critter );
                if( mon != nullptr && mon->type->in_species( species_ZOMBIE ) ) {
                    anger = 100;
                } else {
                    anger = 0;
                }
            }
        }
    }

    if( has_effect( effect_dragging ) ) {

        if( type->has_special_attack( "OPERATE" ) ) {

            bool found_path_to_couch = false;
            tripoint tmp( pos() + point( 12, 12 ) );
            tripoint couch_loc;
            for( const tripoint &couch_pos : here.find_furnitures_with_flag_in_radius( pos(), 10,
                    ter_furn_flag::TFLAG_AUTODOC_COUCH ) ) {
                if( here.clear_path( pos(), couch_pos, 10, 0, 100 ) ) {
                    if( rl_dist( pos(), couch_pos ) < rl_dist( pos(), tmp ) ) {
                        tmp = couch_pos;
                        found_path_to_couch = true;
                        couch_loc = couch_pos;
                    }
                }
            }

            if( !found_path_to_couch ) {
                anger = 0;
                remove_effect( effect_dragging );
            } else {
                set_dest( here.getglobal( couch_loc ) );
            }
        }

    } else if( target != nullptr ) {

        const tripoint_abs_ms dest = target->get_location();
        Creature::Attitude att_to_target = attitude_to( *target );
        if( att_to_target == Attitude::HOSTILE && !fleeing ) {
            if( improved_pathfinding ) {
                hostile_pursuit_active = true;
                const std::optional<tripoint_abs_ms> prior_real_sighting =
                    hostile_memory_origin_position;
                const std::optional<tripoint_abs_ms> prior_previous_sighting =
                    previous_hostile_sighting_position;
                const bool recent_direct_sighting =
                    hostile_memory_origin_position && last_hostile_sighting_turn &&
                    calendar::turn <= *last_hostile_sighting_turn +
                    time_duration::from_turns( 2 );

                if( recent_direct_sighting &&
                    dest != *hostile_memory_origin_position ) {
                    previous_hostile_sighting_position =
                        *hostile_memory_origin_position;
                } else if( !recent_direct_sighting ||
                           dest == *hostile_memory_origin_position ) {
                    previous_hostile_sighting_position.reset();
                }

                if( recent_direct_sighting &&
                    std::abs( dest.z() - hostile_memory_origin_position->z() ) == 1 &&
                    rl_dist( dest, *hostile_memory_origin_position ) <= 8 ) {
                    witnessed_hostile_transition_origin =
                        *hostile_memory_origin_position;
                    witnessed_hostile_transition_destination = dest;
                    witnessed_hostile_transition_memory_turns =
                        smart_planning ? 60 : 30;
                }

                bool keep_exact_portal = false;
                if( hostile_memory_portal_approach &&
                    hostile_memory_portal_transition ) {
                    const int portal_dz =
                        hostile_memory_portal_transition->z() -
                        hostile_memory_portal_approach->z();
                    const int target_dz = dest.z() - posz();
                    keep_exact_portal =
                        hostile_memory_portal_approach->z() == posz() &&
                        target_dz != 0 && portal_dz * target_dz > 0;
                }

                if( !keep_exact_portal && recent_direct_sighting &&
                    prior_real_sighting &&
                    prior_real_sighting->z() == posz() &&
                    std::abs( dest.z() - posz() ) == 1 &&
                    get_pathfinding_settings().allow_climb_stairs ) {
                    const int wanted_z_direction =
                        dest.z() > posz() ? 1 : -1;
                    const std::optional<monster_z_route_plan> direct_transition =
                        infer_recent_hostile_escape_transition(
                            *this, here, *prior_real_sighting,
                            prior_previous_sighting, hostile_search_lane,
                            wanted_z_direction,
                            smart_planning ? 12 : 6 );
                    if( direct_transition ) {
                        hostile_memory_portal_approach =
                            here.getglobal( direct_transition->approach );
                        hostile_memory_portal_transition =
                            here.getglobal( direct_transition->transition );
                        keep_exact_portal = true;
                        add_msg_debug(
                            debugmode::DF_MONSTER,
                            "%s keeps a directly witnessed hostile stair from %s to %s",
                            name(), direct_transition->approach.to_string(),
                            direct_transition->transition.to_string() );
                    }
                }

                if( !keep_exact_portal ) {
                    hostile_memory_portal_approach.reset();
                    hostile_memory_portal_transition.reset();
                }

                hostile_memory_origin_position = dest;
                hostile_memory_search_origin_position = dest;
                hostile_memory_target_is_avatar = target->is_avatar();
                last_hostile_target_position = dest;
                hostile_target_memory_turns = smart_planning ? 150 : 60;
                hostile_search_turns = 0;
                hostile_search_step = 0;
                hostile_search_waypoint_turns = 0;
                hostile_transition_attempts = 0;
                hostile_search_deadline.reset();
                last_hostile_sighting_turn = calendar::turn;
                // Direct sight is absolute priority.  Cancel stale search and sound
                // state, but keep the current route until move() validates its
                // endpoint.  This mirrors CBN's rule that a target update does
                // not by itself mean the next path step is broken.
                wandf = 0;
                provocative_sound = false;
                failed_pathfinding_target.reset();
                failed_pathfinding_cooldown = 0;
                hostile_search_lane =
                    std::abs( posx() * 31 + posy() * 17 + posz() * 13 ) % 4;
            }
            set_dest( dest );
        } else if( fleeing ) {
            tripoint_abs_ms away = get_location() - dest + get_location();
            away.z() = posz();
            set_dest( away );
        }
        if( ( angers_hostile_weak || fears_hostile_weak || placate_hostile_weak ) &&
            att_to_target != Attitude::FRIENDLY ) {
            int hp_per = target->hp_percentage();
            if( hp_per <= 70 ) {
                if( angers_hostile_weak && anger <= 40 ) {
                    anger += 10 - static_cast<int>( hp_per / 10 );
                    if( x_in_y( anger, 100 ) ) {
                        add_msg_debug( debugmode::DF_MONSTER, "%s's character aggro triggered by %s's weakness", name(),
                                       target->disp_name() );
                        aggro_character = true;
                    }
                } else if( fears_hostile_weak ) {
                    morale -= 10 - static_cast<int>( hp_per / 10 );
                } else if( placate_hostile_weak ) {
                    anger -= 10 - static_cast<int>( hp_per / 10 );
                }
            }
        }
    } else if( improved_pathfinding && friendly == 0 &&
               last_hostile_target_position && hostile_target_memory_turns > 0 ) {
        hostile_pursuit_active = true;
        const tripoint_abs_ms real_memory_origin =
            hostile_memory_origin_position ?
            *hostile_memory_origin_position :
            *last_hostile_target_position;
        const tripoint_abs_ms memory_origin =
            hostile_memory_search_origin_position ?
            *hostile_memory_search_origin_position :
            real_memory_origin;
        const tripoint_abs_ms pursuit_clue = *last_hostile_target_position;
        if( hostile_search_step == 0 &&
            rl_dist( get_location(), pursuit_clue ) > 1 ) {
            // Honour the current clue first. Usually this is the real last
            // sighting; after a confirmed stair hint it can be the landing.
            set_dest( pursuit_clue );
        } else {
            bool followed_transition = false;
            if( hostile_search_step == 0 && hostile_transition_attempts == 0 &&
                get_pathfinding_settings().allow_climb_stairs ) {
                std::optional<monster_z_route_plan> transition;

                if( memory_origin.z() != posz() ) {
                    // We really saw the hostile on another floor.
                    const int wanted_z_direction =
                        memory_origin.z() > posz() ? 1 : -1;
                    transition = infer_hostile_memory_transition(
                        *this, here, memory_origin, hostile_search_lane,
                        wanted_z_direction );
                } else {
                    const bool searching_real_sighting =
                        memory_origin == real_memory_origin;
                    const int disappearance_grace =
                        smart_planning ? 8 : 4;
                    const bool recent_disappearance = last_hostile_sighting_turn &&
                        calendar::turn <= *last_hostile_sighting_turn +
                        time_duration::from_turns( disappearance_grace );

                    // For the avatar, use only the currently observed Z delta as
                    // a one-bit clue. We deliberately do NOT read current X/Y.
                    // This bridges the scheduler gap where the last sampled
                    // sighting can be several tiles before the stair.
                    int witnessed_z_direction = 0;
                    if( recent_disappearance && searching_real_sighting &&
                        hostile_memory_target_is_avatar ) {
                        const int z_delta =
                            player_character.get_location().z() -
                            real_memory_origin.z();
                        if( std::abs( z_delta ) == 1 ) {
                            witnessed_z_direction = z_delta > 0 ? 1 : -1;
                        }
                    }

                    if( witnessed_z_direction != 0 ) {
                        transition = infer_recent_hostile_escape_transition(
                            *this, here, real_memory_origin,
                            previous_hostile_sighting_position,
                            hostile_search_lane, witnessed_z_direction,
                            smart_planning ? 12 : 6 );
                    }

                    if( !transition && recent_disappearance &&
                        searching_real_sighting &&
                        hostile_disappearance_portal_is_plausible(
                            here, real_memory_origin,
                            previous_hostile_sighting_position ) ) {
                        // Keep the stricter generic fallback for NPC/monster
                        // targets or cases where the avatar did not change Z.
                        transition = infer_hostile_memory_transition(
                            *this, here, real_memory_origin,
                            hostile_search_lane, witnessed_z_direction, 1 );
                    }
                }

                if( transition ) {
                    hostile_transition_attempts = 1;
                    const tripoint_abs_ms transition_approach =
                        here.getglobal( transition->approach );
                    const tripoint_abs_ms transition_destination =
                        here.getglobal( transition->transition );

                    // Do not throw away the portal approach. move() can now
                    // route cheaply to the exact stair and execute the exact
                    // paired landing without another cross-Z A* discovery.
                    hostile_memory_portal_approach = transition_approach;
                    hostile_memory_portal_transition =
                        transition_destination;
                    last_hostile_target_position = transition_destination;
                    hostile_memory_search_origin_position =
                        transition_destination;
                    hostile_target_memory_turns = std::max(
                        hostile_target_memory_turns,
                        smart_planning ? 90 : 45 );
                    hostile_search_turns = 0;
                    hostile_search_step = 0;
                    hostile_search_waypoint_turns = 0;
                    hostile_search_deadline.reset();
                    wandf = 0;
                    set_dest( transition_destination );
                    followed_transition = true;

                    add_msg_debug(
                        debugmode::DF_MONSTER,
                        "%s keeps an exact hostile stair handoff from %s to %s",
                        name(), transition_approach.to_string(),
                        transition_destination.to_string() );
                }
            }

            if( !followed_transition ) {
                const int search_count = smart_planning ? 16 : 8;
                const int lane = hostile_search_lane;
                const int waypoint_hold = smart_planning ? 12 : 8;

                if( hostile_search_step == 0 ) {
                    hostile_search_step = 1;
                    hostile_search_turns = smart_planning ? 36 : 18;
                    hostile_search_waypoint_turns = waypoint_hold;
                    hostile_search_deadline = calendar::turn +
                        time_duration::from_turns( hostile_search_turns );
                } else if( hostile_search_waypoint_turns <= 0 ) {
                    ++hostile_search_step;
                    hostile_search_waypoint_turns = waypoint_hold;
                } else {
                    --hostile_search_waypoint_turns;
                }

                if( hostile_search_deadline &&
                    calendar::turn >= *hostile_search_deadline ) {
                    hostile_search_step = search_count + 1;
                }

                std::optional<std::pair<int, tripoint_abs_ms>> waypoint;
                if( hostile_search_step <= search_count ) {
                    waypoint = next_hostile_search_waypoint(
                        here, memory_origin,
                        previous_hostile_sighting_position, lane,
                        hostile_search_step, search_count );
                }

                if( !waypoint ) {
                    // Every safe local clue was exhausted.  End the search now
                    // instead of wrapping around the same offsets indefinitely.
                    hostile_pursuit_active = false;
                    unset_dest();
                    last_hostile_target_position.reset();
                    hostile_memory_origin_position.reset();
                    hostile_memory_search_origin_position.reset();
                    hostile_memory_portal_approach.reset();
                    hostile_memory_portal_transition.reset();
                    hostile_memory_target_is_avatar = false;
                    previous_hostile_sighting_position.reset();
                    hostile_target_memory_turns = 0;
                    hostile_search_turns = 0;
                    hostile_search_step = 0;
                    hostile_search_waypoint_turns = 0;
                    hostile_search_lane = 0;
                    hostile_transition_attempts = 0;
                    hostile_search_deadline.reset();
                } else {
                    hostile_search_step = waypoint->first;
                    const tripoint_abs_ms &search_destination = waypoint->second;
                    if( rl_dist( get_location(), search_destination ) <= 1 ) {
                        ++hostile_search_step;
                        hostile_search_waypoint_turns = waypoint_hold;
                    }
                    set_dest( search_destination );
                }
            }
        }
    } else if( !patrol_route.empty() ) {
        // If we have a patrol route and no target, find the current step on the route
        tripoint_abs_ms next_stop = patrol_route.at( next_patrol_point );

        // if there is more than one patrol point, advance to the next one if we're almost there
        // this handles impassable obstacles but patrollers can still get stuck
        if( ( patrol_route.size() > 1 ) && rl_dist( next_stop, get_location() ) < 2 ) {
            next_patrol_point = ( next_patrol_point + 1 ) % patrol_route.size();
            next_stop = patrol_route.at( next_patrol_point );
        }
        set_dest( next_stop );
    } else if (friendly != 0 && has_effect(effect_led_by_leash) &&
        get_location().z() == get_dest().z()) {
        // visibility doesn't matter, we're getting pulled by a leash
        // To use stairs smoothly, if the destination is on a different Z-level, move there first.
        set_dest(player_character.get_location());
        if( friendly > 0 && one_in( 3 ) ) {
            // Grow restless with no targets
            friendly--;
        }
    } else if( friendly > 0 && one_in( 3 ) ) {
        // Grow restless with no targets
        friendly--;
    } else if( is_pet_follow() && sees( player_character ) &&
               ( get_location().z() == player_character.get_location().z() ||
                 get_location().z() == get_dest().z() ) ) {
        // Dog-type pets follow their owner automatically.
        // To use stairs smoothly, if the destination is on a different Z-level, move there first.
        set_dest( player_character.get_location() );
    }
}

/**
 * Method to make monster movement speed consistent in the face of staggering behavior and
 * differing distance metrics.
 * It works by scaling the cost to take a step by
 * how much that step reduces the distance to your goal.
 * Since it incorporates the current distance metric,
 * it also scales for diagonal vs orthogonal movement.
 **/
static float get_stagger_adjust( const tripoint &source, const tripoint &destination,
                                 const tripoint &next_step )
{
    // TODO: push this down into rl_dist
    const float initial_dist = trig_dist( source, destination );
    const float new_dist = trig_dist( next_step, destination );
    // If we return 0, it wil cancel the action.
    return std::max( 0.01f, initial_dist - new_dist );
}

/**
 * Returns true if the given square presents a possibility of drowning for the monster: it's deep water, it's liquid,
 * the monster can drown, and there is no boardable vehicle part present.
 */
bool monster::is_aquatic_danger( const tripoint &at_pos ) const
{
    map &here = get_map();
    return here.has_flag_ter( ter_furn_flag::TFLAG_DEEP_WATER, at_pos ) &&
           here.has_flag( ter_furn_flag::TFLAG_LIQUID, at_pos ) &&
           can_drown() && !here.veh_at( at_pos ).part_with_feature( "BOARDABLE", false );
}

bool monster::die_if_drowning( const tripoint &at_pos, const int chance )
{
    if( is_aquatic_danger( at_pos ) && one_in( chance ) ) {
        add_msg_if_player_sees( at_pos, _( "The %s drowns!" ), name() );
        die( nullptr );
        return true;
    }
    return false;
}

// General movement.
// Currently, priority goes:
// 1) Special Attack
// 2) Sight-based tracking
// 3) Scent-based tracking
// 4) Sound-based tracking
void monster::move()
{
    // We decrement wandf no matter what.  We'll save our wander_to plans until
    // after we finish out set_dest plans, UNLESS they time out first.
    if( wandf > 0 ) {
        wandf--;
    }

    //Hallucinations have a chance of disappearing each turn
    if( is_hallucination() && one_in( 25 ) ) {
        die( nullptr );
        return;
    }
    map &here = get_map();
    Character &player_character = get_player_character();

    behavior::monster_oracle_t oracle( this );
    behavior::tree goals;
    goals.add( type->get_goals() );
    std::string action = goals.tick( &oracle );
    //The monster can consume objects it stands on. Check if there are any.
    //If there are. Consume them.
    // TODO: Stick this in a map and dispatch to it via the action string.
    // TODO: Create a special attacks whitelist unordered map instead of an if chain.
    std::map<std::string, mtype_special_attack>::const_iterator attack =
        type->special_attacks.find( action );
    if( attack != type->special_attacks.end() && attack->second->call( *this ) ) {
        if( special_attacks.count( action ) != 0 ) {
            reset_special( action );
        }
    }
    // record position before moving to put the player there if we're dragging
    tripoint_abs_ms drag_to = get_location();

    const bool pacified = has_effect( effect_pacified );

    // First, use the special attack, if we can!
    // The attack may change `monster::special_attacks` (e.g. by transforming
    // this into another monster type). Therefore we can not iterate over it
    // directly and instead iterate over the map from the monster type
    // (properties of monster types should never change).
    for( const auto &sp_type : type->special_attacks ) {
        const std::string &special_name = sp_type.first;
        const auto local_iter = special_attacks.find( special_name );
        if( local_iter == special_attacks.end() ) {
            continue;
        }
        mon_special_attack &local_attack_data = local_iter->second;
        if( !local_attack_data.enabled ) {
            continue;
        }

        add_msg_debug( debugmode::DF_MATTACK, "%s attempting a special attack %s, cooldown %d", name(),
                       sp_type.first, local_attack_data.cooldown );

        // Cooldowns are decremented in monster::process_turn

        if( local_attack_data.cooldown == 0 && !pacified && !is_hallucination() ) {
            if( !sp_type.second->call( *this ) ) {
                add_msg_debug( debugmode::DF_MATTACK, "Attack failed" );
                continue;
            }

            // `special_attacks` might have changed at this point. Sadly `reset_special`
            // doesn't check the attack name, so we need to do it here.
            if( special_attacks.count( special_name ) == 0 ) {
                continue;
            }
            reset_special( special_name );
        }
    }

    // Check if they're dragging a foe and find their hapless victim
    Character *dragged_foe = find_dragged_foe();

    // Give nursebots a chance to do surgery.
    nursebot_operate( dragged_foe );

    // The monster can sometimes hang in air due to last fall being blocked
    if( !flies() && !here.has_floor_or_support( pos() ) ) {
        here.creature_on_trap( *this, false );
        if( is_dead() ) {
            return;
        }
    }

    // if the monster is in a deep water tile, it has a chance to drown
    if( die_if_drowning( pos(), 10 ) ) {
        return;
    }

    if( moves < 0 ) {
        return;
    }

    // TODO: Move this to attack_at/move_to/etc. functions
    bool attacking = false;
    if( !move_effects( attacking ) ) {
        moves = 0;
        return;
    }
    if( has_flag( MF_IMMOBILE ) || has_flag( MF_RIDEABLE_MECH ) ) {
        moves = 0;
        return;
    }
    if( has_effect( effect_stunned ) ) {
        stumble();
        moves = 0;
        return;
    }
    if( friendly > 0 ) {
        --friendly;
    }

    // don't move if a passenger in a moving vehicle
    optional_vpart_position vp = here.veh_at( pos() );
    bool harness_part = static_cast<bool>( here.veh_at( pos() ).part_with_feature( "ANIMAL_CTRL",
                                           true ) );
    if( vp && ( ( friendly != 0 && vp->vehicle().is_moving() &&
                  vp->vehicle().get_monster( vp->part_index() ) ) ||
                // Don't move if harnessed, even if vehicle is stationary
                has_effect( effect_harnessed ) ) ) {
        moves = 0;
        return;
        // If harnessed monster finds itself moved from the harness point, the harness probably broke!
    } else if( !harness_part && has_effect( effect_harnessed ) ) {
        remove_effect( effect_harnessed );
    }
    // Set attitude to attitude to our current target
    monster_attitude current_attitude = attitude( nullptr );
    if( !is_wandering() ) {
        if( get_dest() == player_character.get_location() ) {
            current_attitude = attitude( &player_character );
        } else {
            for( const npc &guy : g->all_npcs() ) {
                if( get_dest() == guy.get_location() ) {
                    current_attitude = attitude( &guy );
                }
            }
        }
    }
    bool was_controlled_by_friendly_monster_controller = has_value("was_controlled_by_friendly_monster_controller");
    
    if( is_pet_follow() || ( friendly != 0 && has_effect( effect_led_by_leash ) ) ) {
        const int dist = rl_dist(get_location(), get_dest());
        if ((dist <= 1 || (dist <= 2 && !has_effect(effect_led_by_leash) &&
            sees(player_character))) &&
            (get_dest() == player_character.get_location() &&
                get_location().z() == player_character.get_location().z())) {
            moves = 0;
            stumble();
            return;
        }
    } else if(!was_controlled_by_friendly_monster_controller&&( ( current_attitude == MATT_IGNORE && patrol_route.empty() ) ||
        ( ( current_attitude == MATT_FOLLOW ||
            ( has_flag( MF_KEEP_DISTANCE ) && !( current_attitude == MATT_FLEE ) ) )
          && rl_dist( get_location(), get_dest() ) <= type->tracking_distance ) )) {
        moves = 0;
        stumble();
        return;
    }

    const bool improved_pathfinding =
        get_option<std::string>( "MONSTER_PATHFINDING" ) != "classic";
    const int pathfinding_mode_generation =
        refresh_monster_pathfinding_mode( improved_pathfinding );
    if( pathfinding_mode_generation_seen != pathfinding_mode_generation ) {
        path.clear();
        failed_pathfinding_target.reset();
        failed_pathfinding_cooldown = 0;
        pathfinding_mode_generation_seen = pathfinding_mode_generation;
    }

    if( improved_pathfinding && failed_pathfinding_cooldown > 0 ) {
        --failed_pathfinding_cooldown;
    }

    bool moved = false;
    tripoint destination = pos();
    bool try_to_move = false;
    creature_tracker &creatures = get_creature_tracker();
    const bool can_open_doors = has_flag( MF_CAN_OPEN_DOORS ) && !is_hallucination();
    const int current_bash_estimate = bash_estimate();
    const bool can_bash = improved_pathfinding ? current_bash_estimate > 0 : bash_skill() > 0;

    for( const tripoint &dest :
         here.points_in_radius( pos(), 1, improved_pathfinding ? 1 : 0 ) ) {
        if( dest == pos() ) {
            continue;
        }

        const Creature *occupant = creatures.creature_at( dest, true );
        if( !improved_pathfinding ) {
            if( can_move_to( dest ) && occupant == nullptr ) {
                try_to_move = true;
                break;
            }
            continue;
        }

        const bool can_enter = can_move_to( dest ) && occupant == nullptr;
        const bool can_attack_occupant = occupant != nullptr &&
                                         attitude_to( *occupant ) == Attitude::HOSTILE;
        const bool can_open = can_open_doors &&
                              here.open_door( *this, dest, !here.is_outside( pos() ), true );
        const bool can_break = can_bash && here.bash_rating( current_bash_estimate, dest ) > 0;

        if( can_enter || can_attack_occupant || can_open || can_break ) {
            try_to_move = true;
            break;
        }
    }

    // If true, don't try to greedily avoid locally bad paths
    bool pathed = false;
    bool pathfinding_budget_exhausted = false;
    const tripoint local_dest = here.getlocal( get_dest() );
    // Keep the ultimate goal separate from destination. A successful route
    // stores its immediate next step in destination, while dynamic decongestion
    // needs the original target to steer around a temporary creature blocker.
    tripoint movement_goal = local_dest;

    pathfinding_settings pf_settings = get_pathfinding_settings();
    if( improved_pathfinding ) {
        pf_settings.allow_open_doors = pf_settings.allow_open_doors || can_open_doors;
        pf_settings.bash_strength = std::max( pf_settings.bash_strength, current_bash_estimate );
        if( pf_settings.max_dist <= 0 ) {
            pf_settings.max_dist = fallback_monster_path_radius;
        }
        if( pf_settings.max_length <= 0 ) {
            pf_settings.max_length = pf_settings.max_dist * 5;
        }
    }

    auto same_z_fallback_destination = [&]( const tripoint &goal ) {
        return goal.z == posz() ? goal : tripoint( goal.xy(), posz() );
    };

    auto cached_step_is_dynamically_blocked = [&]( const tripoint &step ) {
        const Creature *blocking = creatures.creature_at( step, true );
        if( blocking == nullptr || blocking == this ) {
            return false;
        }
        if( is_hallucination() != blocking->is_hallucination() &&
            !blocking->is_avatar() ) {
            return true;
        }

        const Attitude blocker_attitude = attitude_to( *blocking );
        if( blocker_attitude == Attitude::HOSTILE ) {
            return false;
        }
        if( blocker_attitude == Attitude::FRIENDLY &&
            ( blocking->is_avatar() || blocking->is_npc() ||
              blocking->has_flag( MF_QUEEN ) ) ) {
            return true;
        }

        // A terrain route is not a reservation. Non-pushy monsters must be
        // allowed to locally steer around temporary same-faction congestion.
        return !has_flag( MF_ATTACKMON ) && !has_flag( MF_PUSH_MON );
    };

    auto cached_step_is_usable = [&]( const tripoint &step ) {
        if( !here.inbounds( step ) || rl_dist( pos(), step ) >= 2 ) {
            return false;
        }
        if( cached_step_is_dynamically_blocked( step ) ) {
            return false;
        }
        if( can_move_to( step ) ) {
            return true;
        }
        if( can_open_doors &&
            here.open_door( *this, step, !here.is_outside( pos() ), true ) ) {
            return true;
        }
        return can_bash && here.bash_rating( current_bash_estimate, step ) > 0;
    };

    auto cross_z_route_is_sane = [&]( const std::vector<tripoint> &candidate_path,
                                     const tripoint &route_dest ) {
        if( route_dest.z == posz() ) {
            return true;
        }
        if( candidate_path.empty() || candidate_path.back() != route_dest ) {
            return false;
        }

        const int wanted_z_direction = route_dest.z > posz() ? 1 : -1;
        int previous_z = posz();
        bool reached_target_z = false;

        for( const tripoint &step : candidate_path ) {
            const int dz = step.z - previous_z;
            if( std::abs( dz ) > 1 ||
                ( dz != 0 && dz * wanted_z_direction < 0 ) ) {
                return false;
            }

            if( step.z == route_dest.z ) {
                reached_target_z = true;
            }
            if( reached_target_z ) {
                // Once a route reaches the target floor, do not accept a plan
                // that immediately leaves it again.  This rejects the common
                // "climb another house, then jump back down" false route.
                if( step.z != route_dest.z ) {
                    return false;
                }
                // A cross-Z route should not deliberately traverse unsupported
                // target-floor tiles.  Same-Z hostile fallback can still make
                // ordinary zombies crowd edges and fall naturally.
                if( !flies() && step != route_dest &&
                    !here.has_floor_or_support( step ) ) {
                    return false;
                }
            }
            previous_z = step.z;
        }

        return reached_target_z;
    };

    auto try_route_to = [&]( const tripoint &route_dest ) {
        if( !improved_pathfinding || route_dest == pos() ) {
            return false;
        }

        const bool cross_z = route_dest.z != posz();
        const tripoint_abs_ms absolute_route_dest = here.getglobal( route_dest );
        const bool fresh_visual_route = last_hostile_sighting_turn &&
            calendar::turn <= *last_hostile_sighting_turn +
            time_duration::from_turns( 1 ) &&
            absolute_route_dest == get_dest();

        // CBN keeps a confirmed pursuit on its real path instead of dropping
        // back to free-form stumbling.  Breeze already owns a bounded 64-tile
        // shared reverse field, so use that existing budget for fresh sight.
        const int same_z_route_radius = fresh_visual_route ?
                                        std::max( pf_settings.max_dist,
                                                  reverse_field_radius ) :
                                        pf_settings.max_dist;
        const int route_radius = cross_z ?
                                 std::max( same_z_route_radius,
                                           cross_z_monster_path_radius ) :
                                 same_z_route_radius;
        const int route_distance = rl_dist( pos(), route_dest );
        if( route_distance > route_radius ) {
            return false;
        }
        if( failed_pathfinding_target && *failed_pathfinding_target != absolute_route_dest ) {
            failed_pathfinding_target.reset();
            failed_pathfinding_cooldown = 0;
        }

        while( !path.empty() && path.front() == pos() ) {
            path.erase( path.begin() );
        }
        if( !path.empty() && !cached_step_is_usable( path.front() ) ) {
            path.clear();
        }

        refresh_monster_path_caches();
        const monster_route_cache_key cache_key {
            type->id,
            route_dest,
            quantize_monster_bash_strength( pf_settings.bash_strength ),
            pf_settings.allow_open_doors
        };
        const monster_z_route_cache_key z_cache_key { cache_key, posz() };

        std::optional<monster_z_route_plan> z_plan;
        if( cross_z ) {
            if( hostile_pursuit_active &&
                hostile_memory_portal_approach &&
                hostile_memory_portal_transition &&
                *hostile_memory_portal_transition == absolute_route_dest ) {
                const tripoint remembered_approach =
                    here.getlocal( *hostile_memory_portal_approach );
                const tripoint remembered_transition =
                    here.getlocal( *hostile_memory_portal_transition );
                if( here.inbounds( remembered_approach ) &&
                    here.inbounds( remembered_transition ) &&
                    remembered_approach.z == posz() &&
                    remembered_transition.z == route_dest.z &&
                    is_monster_stair_transition_usable(
                        *this,
                        here, remembered_approach,
                        remembered_transition ) ) {
                    z_plan = monster_z_route_plan {
                        remembered_approach, remembered_transition
                    };
                }
            }

            const auto z_plan_iter = monster_z_route_cache.find( z_cache_key );
            if( !z_plan && z_plan_iter != monster_z_route_cache.end() ) {
                z_plan = z_plan_iter->second;
            }
            if( !z_plan && hostile_pursuit_active &&
                witnessed_hostile_transition_origin &&
                witnessed_hostile_transition_destination &&
                witnessed_hostile_transition_memory_turns > 0 &&
                witnessed_hostile_transition_origin->z() == posz() ) {
                const int wanted_z_direction = route_dest.z > posz() ? 1 : -1;
                const int witnessed_z_direction =
                    witnessed_hostile_transition_destination->z() -
                    witnessed_hostile_transition_origin->z();
                if( witnessed_z_direction * wanted_z_direction > 0 ) {
                    const int hint_radius = has_flag( MF_PRIORITIZE_TARGETS ) ?
                                            witnessed_hostile_transition_hint_radius : 4;
                    z_plan = infer_hostile_memory_transition(
                                 *this, here,
                                 *witnessed_hostile_transition_origin,
                                 hostile_search_lane, wanted_z_direction,
                                 hint_radius );
                    if( z_plan ) {
                        monster_z_route_cache[z_cache_key] = *z_plan;
                        remember_monster_stair_route( here, absolute_route_dest,
                                                      z_plan->approach,
                                                      z_plan->transition );
                    }
                }
            }
            if( !z_plan ) {
                z_plan = get_remembered_monster_stair_route( *this, here,
                         absolute_route_dest, posz() );
                if( z_plan ) {
                    // Promote the remembered portal to the exact target key
                    // for the rest of this turn.  This is a cheap map lookup
                    // for the rest of the horde, not another A* search.
                    monster_z_route_cache[z_cache_key] = *z_plan;
                }
            }
            if( z_plan && ( z_plan->approach.z != posz() ||
                            z_plan->transition.z != route_dest.z ||
                            !here.inbounds( z_plan->approach ) ||
                            !here.inbounds( z_plan->transition ) ||
                            !is_monster_stair_transition_usable(
                                *this, here, z_plan->approach, z_plan->transition ) ) ) {
                z_plan.reset();
            }
        }
        const bool has_z_plan = z_plan.has_value();
        const tripoint segment_dest = has_z_plan && pos() != z_plan->approach ?
                                      z_plan->approach : route_dest;

        // A remembered stair is a portal, not a normal same-level path step.
        // Execute it directly after validating the tile; the movement code
        // below still performs the final can_reach_to()/can_move_to() checks.
        if( has_z_plan && pos() == z_plan->approach &&
            is_monster_stair_transition_usable( *this, here, z_plan->approach,
                                                z_plan->transition ) ) {
            path.clear();
            destination = z_plan->transition;
            moved = true;
            pathed = true;
            if( hostile_memory_portal_transition &&
                here.getlocal( *hostile_memory_portal_transition ) ==
                z_plan->transition ) {
                hostile_memory_portal_approach.reset();
                hostile_memory_portal_transition.reset();
            }
            return true;
        }

        const bool existing_path_matches = !path.empty() &&
                                           ( path.back() == route_dest ||
                                             path.back() == segment_dest );
        const bool need_new_path = path.empty() || rl_dist( pos(), path.front() ) >= 2 ||
                                   !existing_path_matches;
        if( need_new_path ) {
            path.clear();
            if( failed_pathfinding_target &&
                *failed_pathfinding_target == absolute_route_dest &&
                failed_pathfinding_cooldown > 0 &&
                !( has_z_plan && segment_dest != route_dest ) ) {
                return false;
            }

            // A complete route stores every suffix.  Monsters that already stand
            // on that route, including a stair approach tile, can join it for free.
            const auto cache_iter = monster_route_cache.find( cache_key );
            if( cache_iter != monster_route_cache.end() ) {
                std::vector<tripoint> cached_path;
                std::set<tripoint> visited;
                tripoint cursor = pos();
                const std::size_t max_cached_steps = static_cast<std::size_t>(
                            std::max( 1, pf_settings.max_length ) );

                while( cached_path.size() < max_cached_steps && cursor != route_dest &&
                       visited.insert( cursor ).second ) {
                    const auto next_iter = cache_iter->second.next_steps.find( cursor );
                    if( next_iter == cache_iter->second.next_steps.end() ||
                        next_iter->second == cursor ) {
                        break;
                    }
                    cached_path.push_back( next_iter->second );
                    cursor = next_iter->second;
                }

                if( !cached_path.empty() && cached_path.back() == route_dest &&
                    cached_step_is_usable( cached_path.front() ) ) {
                    path = std::move( cached_path );
                }
            }

            auto build_reverse_field_path = [&]( const tripoint &field_target,
                                                  int field_max_radius ) {
                std::vector<tripoint> result;
                if( field_target.z != posz() || pos() == field_target ||
                    !here.inbounds( field_target ) ||
                    rl_dist( pos(), field_target ) > field_max_radius ) {
                    return result;
                }

                const monster_route_cache_key field_key {
                    type->id,
                    field_target,
                    quantize_monster_bash_strength( pf_settings.bash_strength ),
                    pf_settings.allow_open_doors
                };
                monster_reverse_field_entry *field = get_monster_reverse_field( field_key );
                if( field == nullptr ) {
                    return result;
                }

                constexpr int impassable_cost = std::numeric_limits<int>::max() / 8;
                const auto transition_cost = [&]( const tripoint &from, const tripoint &to ) {
                    if( !here.inbounds( from ) || !here.inbounds( to ) ||
                        from.z != to.z ) {
                        return impassable_cost;
                    }
                    if( from.x != to.x && from.y != to.y ) {
                        const tripoint side_a( from.x, to.y, from.z );
                        const tripoint side_b( to.x, from.y, from.z );
                        if( here.impassable( side_a ) && here.impassable( side_b ) ) {
                            return impassable_cost;
                        }
                    }

                    constexpr int cbn_path_cost_scale = 50;
                    int result_cost = impassable_cost;
                    if( will_move_to( to ) ) {
                        const int base_move_cost =
                            std::max( 1, calc_movecost( from, to ) );
                        const bool diagonal_step =
                            from.x != to.x && from.y != to.y;

                        // Flat Breeze movement is normally about 100 moves,
                        // corresponding to CBN's ordinary path cost 2 at this
                        // scale. Preserve real terrain speed and diagonal
                        // geometry rather than flattening walkable tiles.
                        result_cost = diagonal_step ?
                                      ( base_move_cost * 3 + 1 ) / 2 :
                                      base_move_cost;
                    } else if( can_open_doors &&
                               here.open_door( *this, to, !here.is_outside( from ), true ) ) {
                        result_cost = here.veh_at( to ) ?
                                      10 * cbn_path_cost_scale :
                                      4 * cbn_path_cost_scale;
                    } else if( can_bash ) {
                        const int rating =
                            here.bash_rating( current_bash_estimate, to );
                        if( rating == 1 ) {
                            result_cost = 500 * cbn_path_cost_scale;
                        } else if( rating > 1 ) {
                            result_cost =
                                ( ( 20 / rating ) + 2 + 10 ) *
                                cbn_path_cost_scale;
                        }
                    }

                    // Dynamic creature occupancy does not belong in the
                    // shared terrain field. Immediate movement validates the
                    // current blocker and locally steers around it.
                    return result_cost;
                };

                std::size_t expanded_this_request = 0;
                while( !field->is_settled( pos() ) && !field->frontier.empty() &&
                       expanded_this_request < monster_reverse_field_request_budget &&
                       monster_reverse_field_nodes < monster_reverse_field_turn_budget ) {
                    const monster_reverse_field_node current = field->frontier.top();
                    field->frontier.pop();
                    if( !field->settle_node( current.position, current.cost ) ) {
                        continue;
                    }

                    ++expanded_this_request;
                    ++monster_reverse_field_nodes;
                    if( current.position == pos() ) {
                        break;
                    }

                    for( int dx = -1; dx <= 1; ++dx ) {
                        for( int dy = -1; dy <= 1; ++dy ) {
                            if( dx == 0 && dy == 0 ) {
                                continue;
                            }
                            const tripoint previous( current.position.x + dx,
                                                     current.position.y + dy,
                                                     current.position.z );
                            if( !here.inbounds( previous ) ||
                                rl_dist( previous, field_target ) > field_max_radius ) {
                                continue;
                            }
                            const int edge_cost = transition_cost( previous, current.position );
                            if( edge_cost >= impassable_cost ||
                                current.cost > impassable_cost - edge_cost ) {
                                continue;
                            }
                            const int new_cost = current.cost + edge_cost;
                            if( field->relax( previous, new_cost, current.position ) ) {
                                field->frontier.push( { new_cost, previous } );
                            }
                        }
                    }
                }

                if( !field->is_settled( pos() ) ) {
                    return result;
                }

                tripoint cursor = pos();
                const std::size_t max_steps = static_cast<std::size_t>(
                            std::max( 1, pf_settings.max_length ) );
                while( cursor != field_target && result.size() < max_steps ) {
                    tripoint next;
                    if( !field->next_step( cursor, next ) ) {
                        result.clear();
                        return result;
                    }
                    result.push_back( next );
                    cursor = next;
                }
                if( result.empty() || result.back() != field_target ||
                    !cached_step_is_usable( result.front() ) ) {
                    result.clear();
                }
                return result;
            };

            if( path.empty() && segment_dest != route_dest ) {
                path = build_reverse_field_path( segment_dest, reverse_field_cross_z_radius );
            }
            if( path.empty() && !cross_z ) {
                path = build_reverse_field_path( route_dest,
                                                 std::min( route_radius, reverse_field_radius ) );
            }

            if( path.empty() ) {
                pathfinding_settings route_settings = pf_settings;
                if( cross_z ) {
                    route_settings.max_dist = route_radius;
                    route_settings.max_length = std::max( route_settings.max_length,
                                                         route_radius * 8 );
                }
                if( cross_z ) {
                    if( monster_cross_z_route_searches >=
                        monster_cross_z_route_search_turn_budget ) {
                        const bool priority_hostile_cross_z =
                            hostile_pursuit_active &&
                            has_flag( MF_PRIORITIZE_TARGETS ) &&
                            monster_priority_cross_z_route_searches <
                            monster_priority_cross_z_route_default_turn_budget;
                        if( !priority_hostile_cross_z ) {
                            pathfinding_budget_exhausted = true;
                            failed_pathfinding_target = absolute_route_dest;
                            failed_pathfinding_cooldown = 2 +
                                ( std::abs( posx() + posy() + posz() ) % 3 );
                            return false;
                        }
                        ++monster_priority_cross_z_route_searches;
                    } else {
                        ++monster_cross_z_route_searches;
                    }
                } else {
                    if( monster_route_searches >= monster_route_search_turn_budget ) {
                        pathfinding_budget_exhausted = true;
                        failed_pathfinding_target = absolute_route_dest;
                        failed_pathfinding_cooldown = 2 +
                                                       ( std::abs( posx() + posy() + posz() ) % 3 );
                        return false;
                    }
                    ++monster_route_searches;
                }
                path = here.route( pos(), route_dest, route_settings, get_path_avoid() );
            }

            // A path ending at segment_dest is only the same-Z approach to a
            // remembered stair; validate the complete cross-Z route only.
            if( cross_z && !path.empty() && path.back() == route_dest &&
                !cross_z_route_is_sane( path, route_dest ) ) {
                path.clear();
            }

            if( !path.empty() && path.back() == route_dest ) {
                failed_pathfinding_target.reset();
                failed_pathfinding_cooldown = 0;

                if( cross_z ) {
                    tripoint previous = pos();
                    const int wanted_z_direction = route_dest.z > posz() ? 1 : -1;
                    for( const tripoint &step : path ) {
                        if( step.z != previous.z ) {
                            if( ( step.z - previous.z ) * wanted_z_direction > 0 ) {
                                monster_z_route_cache[z_cache_key] = { previous, step };
                                remember_monster_stair_route( here, absolute_route_dest,
                                                              previous, step );
                            }
                            break;
                        }
                        previous = step;
                    }
                }

                if( monster_route_cache_edges < monster_route_cache_max_edges ) {
                    monster_route_cache_entry &entry = monster_route_cache[cache_key];
                    tripoint from = pos();
                    for( const tripoint &step : path ) {
                        if( monster_route_cache_edges >= monster_route_cache_max_edges ) {
                            break;
                        }
                        const auto inserted = entry.next_steps.emplace( from, step );
                        if( inserted.second ) {
                            ++monster_route_cache_edges;
                        }
                        from = step;
                    }
                }
            } else if( !path.empty() && path.back() == segment_dest ) {
                failed_pathfinding_target.reset();
                failed_pathfinding_cooldown = 0;
            } else {
                path.clear();
                failed_pathfinding_target = absolute_route_dest;
                failed_pathfinding_cooldown = 2 +
                                               ( std::abs( posx() + posy() + posz() ) % 3 );
                return false;
            }
        }

        if( !path.empty() &&
            ( path.back() == route_dest || path.back() == segment_dest ) ) {
            destination = path.front();
            moved = true;
            pathed = true;
            return true;
        }
        return false;
    };

    if( try_to_move && !is_wandering() ) {
        if( improved_pathfinding ) {
            const int target_distance = rl_dist( pos(), local_dest );
            const int target_path_radius = local_dest.z != posz() ?
                                           std::max( pf_settings.max_dist,
                                                     cross_z_monster_path_radius ) :
                                           pf_settings.max_dist;
            movement_goal = local_dest;
            if( !try_route_to( local_dest ) &&
                ( hostile_pursuit_active ||
                  target_distance > target_path_radius ||
                  ( pathfinding_budget_exhausted && local_dest.z == posz() ) ) ) {
                // Exact route failure must not turn a confirmed chase into a
                // 1-in-10 stumble. Cross-Z movement itself still requires a
                // validated stair/ramp route; the fallback also keeps the
                // later direct stair handoff alive when the XY projection is
                // exactly our current tile.
                destination = same_z_fallback_destination( local_dest );
                moved = destination != pos() || movement_goal.z != posz();
            }
        } else {
            while( !path.empty() && path.front() == pos() ) {
                path.erase( path.begin() );
            }
            if( pf_settings.max_dist >= rl_dist( get_location(), get_dest() ) &&
                ( path.empty() || rl_dist( pos(), path.front() ) >= 2 ||
                  path.back() != local_dest ) ) {
                path = here.route( pos(), local_dest, pf_settings, get_path_avoid() );
            }
            if( !path.empty() && path.back() == local_dest ) {
                destination = path.front();
                moved = true;
                pathed = true;
            } else {
                destination = local_dest;
                moved = true;
            }
        }
    }
    if( !moved && has_flag( MF_SMELLS ) &&
        !( is_pet() && !is_pet_follow() &&
           !has_effect( effect_led_by_leash ) ) ) {
        // No sight... or our plans are invalid (e.g. moving through a transparent, but
        //  solid, square of terrain).  Fall back to smell if we have it.

        if (was_controlled_by_friendly_monster_controller) {
            destination = get_map().getlocal(get_dest());
        }
        else {
            unset_dest();
            tripoint tmp = scent_move();
            if (tmp.x != -1) {
                destination = tmp;
                moved = true;
            }
        }
    }
    const bool pursuing_confirmed_hostile =
        improved_pathfinding && hostile_pursuit_active;
    if( wandf > 0 && !moved && friendly == 0 &&
        !pursuing_confirmed_hostile ) { // Sound is below confirmed hostile memory
        if( improved_pathfinding ) {
            if( wander_pos != get_location() ) {
                const tripoint sound_dest = here.getlocal( wander_pos );
                const int sound_distance = rl_dist( pos(), sound_dest );
                const int sound_path_radius = sound_dest.z != posz() ?
                                              std::max( pf_settings.max_dist,
                                                        cross_z_monster_path_radius ) :
                                              pf_settings.max_dist;
                unset_dest();
                movement_goal = sound_dest;

                if( !try_route_to( sound_dest ) &&
                    ( provocative_sound ||
                      sound_distance > sound_path_radius ||
                      ( pathfinding_budget_exhausted && sound_dest.z == posz() ) ) ) {
                    // Only provocative sounds gain the aggressive in-radius
                    // fallback. Ordinary noises retain the previous semantics.
                    destination = same_z_fallback_destination( sound_dest );
                    moved = destination != pos() || movement_goal.z != posz();
                }
            }
        } else {
            unset_dest();
            if( wander_pos != get_location() ) {
                destination = here.getlocal( wander_pos );
                moved = true;
            }
        }
    }
    // Current CBN treats a valid terrain path step as authoritative, but
    // creature occupancy is temporary. Do not let a cached immediate step
    // become a reservation that serializes an entire horde behind one monster.
    bool path_step_is_authoritative = improved_pathfinding && pathed;
    if( path_step_is_authoritative && destination.z == posz() &&
        cached_step_is_dynamically_blocked( destination ) ) {
        path_step_is_authoritative = false;
        pathed = false;
        path.clear();
        destination = same_z_fallback_destination( movement_goal );
        moved = destination != pos() || movement_goal.z != posz();
    }

    point new_d( destination.xy() - pos().xy() );

    // toggle facing direction for sdl flip
    if( !g->is_tileset_isometric() ) {
        if( new_d.x < 0 ) {
            facing = FacingDirection::LEFT;
        } else if( new_d.x > 0 ) {
            facing = FacingDirection::RIGHT;
        }
    } else {
        if( new_d.y <= 0 && new_d.x <= 0 ) {
            facing = FacingDirection::LEFT;
        }
        if( new_d.x >= 0 && new_d.y >= 0 ) {
            facing = FacingDirection::RIGHT;
        }
    }

    tripoint_abs_ms next_step;
    const bool staggers = has_flag( MF_STUMBLES );
    if( moved ) {
        // Implement both avoiding obstacles and staggering.
        moved = false;
        float switch_chance = 0.0f;
        // This is a float and using trig_dist() because that Does the Right Thing(tm)
        // in both circular and roguelike distance modes.
        const float distance_to_target = trig_dist( pos(), destination );
        std::vector<tripoint> movement_candidates;
        std::optional<tripoint> forced_stair_destination;
        if( path_step_is_authoritative ) {
            // Match CBN's pathed_to_goal behavior first. A valid route's
            // immediate next step must win whenever it is usable; retain the
            // old local candidates only as a fallback when that step is
            // dynamically rejected below.
            movement_candidates.push_back( destination );
            const std::vector<tripoint> fallback_candidates =
                squares_closer_to( pos(), destination );
            movement_candidates.insert( movement_candidates.end(),
                                        fallback_candidates.begin(), fallback_candidates.end() );
        } else {
            movement_candidates = squares_closer_to( pos(), destination );

            const int wanted_z_direction =
                movement_goal.z == posz() ? 0 :
                ( movement_goal.z > posz() ? 1 : -1 );
            if( wanted_z_direction != 0 &&
                get_pathfinding_settings().allow_climb_stairs ) {
                const ter_furn_flag wanted_portal =
                    wanted_z_direction > 0 ?
                    ter_furn_flag::TFLAG_GOES_UP :
                    ter_furn_flag::TFLAG_GOES_DOWN;

                if( here.has_flag( wanted_portal, pos() ) ) {
                    bool rope_ladder = false;
                    const std::optional<tripoint> stair_destination =
                        g->find_or_make_stairs(
                            here, posz() + wanted_z_direction,
                            rope_ladder, false, pos() );
                    if( stair_destination &&
                        here.inbounds( *stair_destination ) ) {
                        const ter_furn_flag paired_portal =
                            wanted_z_direction > 0 ?
                            ter_furn_flag::TFLAG_GOES_DOWN :
                            ter_furn_flag::TFLAG_GOES_UP;
                        if( here.has_flag(
                                paired_portal, *stair_destination ) &&
                            is_monster_stair_transition_usable(
                                *this, here, pos(), *stair_destination ) ) {
                            forced_stair_destination = *stair_destination;
                            movement_candidates.insert(
                                movement_candidates.begin(),
                                *stair_destination );
                        }
                    }
                }
            }

            if( movement_goal.z < posz() ) {
                const tripoint directly_below(
                    posx(), posy(), posz() - 1 );
                movement_candidates.insert(
                    forced_stair_destination ? movement_candidates.begin() + 1 :
                    movement_candidates.begin(), directly_below );
            }
        }

        for( tripoint &candidate : movement_candidates ) {
            const bool forced_z_candidate = forced_stair_destination.has_value() &&
                                            candidate == *forced_stair_destination;
            // rare scenario when monster is on the border of the map and it's goal is outside of the map
            if( !here.inbounds( candidate ) ) {
                continue;
            }

            bool via_ramp = false;
            if( here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, candidate ) ) {
                via_ramp = true;
                candidate.z += 1;
            } else if( here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, candidate ) ) {
                via_ramp = true;
                candidate.z -= 1;
            }
            const tripoint_abs_ms candidate_abs = get_map().getglobal( candidate );

            if( candidate.z != posz() ) {
                bool can_z_move = true;
                if( !here.valid_move( pos(), candidate, false, true, via_ramp ) ) {
                    // Can't phase through floor
                    can_z_move = false;
                }

                // CBN/CCB allow ordinary paired stairs without a climbing
                // ability.  Reserve the wall-climbing guard for difficult
                // vertical moves and unsupported jumps.
                const bool ordinary_stair =
                    here.has_flag( ter_furn_flag::TFLAG_GOES_UP, pos() ) &&
                    !here.has_flag( ter_furn_flag::TFLAG_DIFFICULT_Z, pos() );
                if( candidate.z > posz() && !( via_ramp || flies() ) &&
                    !ordinary_stair ) {
                    if( !can_climb() || !here.has_floor_or_support( candidate ) ) {
                        // Can't "jump" up a whole z-level
                        can_z_move = false;
                    }
                }

                // Last chance - we can still do the z-level stair teleport bullshit that isn't removed yet
                // TODO: Remove z-level stair bullshit teleport after aligning all stairs
                if( !can_z_move &&
                    posx() / ( SEEX * 2 ) == candidate.x / ( SEEX * 2 ) &&
                    posy() / ( SEEY * 2 ) == candidate.y / ( SEEY * 2 ) ) {
                    if( is_monster_stair_transition_usable( *this, here, pos(),
                            candidate ) ) {
                        can_z_move = true;
                    }
                }

                if( !can_z_move ) {
                    continue;
                }
            }

            // A flag to allow non-stumbling critters to stumble when the most direct choice is bad.
            bool bad_choice = false;

            const Creature *target = creatures.creature_at( candidate, is_hallucination() );
            if( target != nullptr ) {
                if( is_hallucination() != target->is_hallucination() && !target->is_avatar() ) {
                    // Hallucinations should only be capable of targeting the player or other hallucinations.
                    continue;
                }
                const Attitude att = attitude_to( *target );
                if( att == Attitude::HOSTILE ) {
                    // When attacking an adjacent enemy, we're direct.
                    moved = true;
                    next_step = candidate_abs;
                    break;
                }
                if( att == Attitude::FRIENDLY && ( target->is_avatar() || target->is_npc() ||
                                                   target->has_flag( MF_QUEEN ) ) ) {
                    // Friendly firing the player or an NPC is illegal for gameplay reasons.
                    // Monsters should instinctively avoid attacking queens that regenerate their own population.
                    continue;
                }
                if( !has_flag( MF_ATTACKMON ) && !has_flag( MF_PUSH_MON ) ) {
                    // Bail out if there's a non-hostile monster in the way and we're not pushy.
                    continue;
                }
                // Friendly fire and pushing are always bad choices - they take a lot of time
                bad_choice = true;
                if( forced_z_candidate ) {
                    // Do not reserve a stair landing occupied by a friendly
                    // monster; let ordinary local pushing/queueing handle it.
                    continue;
                }
            }

            // is there an openable door?
            if( can_open_doors &&
                here.open_door( *this, candidate, !here.is_outside( pos() ), true ) ) {
                moved = true;
                next_step = candidate_abs;
                if( path_step_is_authoritative ) {
                    break;
                }
                continue;
            }

            // Try to shove vehicle out of the way
            shove_vehicle( destination, candidate );
            // Bail out if we can't move there and we can't bash.
            if( ( !pathed || path_step_is_authoritative ) && !can_move_to( candidate ) ) {
                if( !can_bash ) {
                    continue;
                }
                // Don't bash if we're just tracking a noise.
                if( forced_z_candidate ) {
                    continue;
                }
                if( !provocative_sound && is_wandering() && destination == here.getlocal( wander_pos ) ) {
                    continue;
                }
                const int estimate = here.bash_rating( bash_estimate(), candidate );
                if( estimate <= 0 ) {
                    continue;
                }

                if( estimate < 5 ) {
                    bad_choice = true;
                }
            }

            // A validated paired stair is an authoritative cross-Z action.  A
            // same-level fallback destination can have zero or negative XY
            // progress even though this is exactly the move needed to reach
            // the remembered target floor, so the ordinary progress gate must
            // not discard it.
            if( forced_z_candidate ) {
                moved = true;
                next_step = candidate_abs;
                break;
            }

            const float progress = distance_to_target - trig_dist( candidate, destination );
            // The x2 makes the first (and most direct) path twice as likely,
            // since the chance of switching is 1/1, 1/4, 1/6, 1/8
            switch_chance += progress * 2;
            // Randomly pick one of the viable squares to move to weighted by distance.
            if( progress > 0 && ( !moved || x_in_y( progress, switch_chance ) ) ) {
                moved = true;
                next_step = candidate_abs;
                // If we stumble, pick a random square, otherwise take the first one,
                // which is the most direct path.
                // Except if the direct path is bad, then check others
                // Or if the path is given by pathfinder
                if( path_step_is_authoritative ||
                    ( !staggers && ( !bad_choice || pathed ) ) ) {
                    break;
                }
            }
        }
    }
    // Finished logic section.  By this point, we should have chosen a square to
    //  move to (moved = true).
    if( moved ) { // Actual effects of moving to the square we've chosen
        const tripoint local_next_step = here.getlocal( next_step );
        const bool did_something =
            ( !pacified && attack_at( local_next_step ) ) ||
            ( !pacified && can_open_doors &&
              here.open_door( *this, local_next_step, !here.is_outside( pos() ) ) ) ||
            ( !pacified && bash_at( local_next_step ) ) ||
            ( !pacified && push_to( local_next_step, 0, 0 ) ) ||
            move_to( local_next_step, false, false, get_stagger_adjust( pos(), destination, local_next_step ) );

        if( !did_something ) {
            moves -= 100; // If we don't do this, we'll get infinite loops.
        }
        if( has_effect( effect_dragging ) && dragged_foe != nullptr ) {

            if( !dragged_foe->has_effect( effect_grabbed ) ) {
                dragged_foe = nullptr;
                remove_effect( effect_dragging );
            } else if( drag_to != get_location() && creatures.creature_at( drag_to ) == nullptr ) {
                dragged_foe->move_to( drag_to );
            }
        }
    } else {
        moves = 0;
        if (!was_controlled_by_friendly_monster_controller) {
            stumble();
        }
        path.clear();
    }
    if( has_effect( effect_led_by_leash ) ) {
        if( rl_dist( get_location(), player_character.get_location() ) > 2 ) {
            // Either failed to keep up with the player or moved away
            remove_effect( effect_led_by_leash );
            add_msg( m_info, _( "You lose hold of a leash." ) );
        }
    }
}

Character *monster::find_dragged_foe()
{
    // Make sure they're actually dragging someone.
    if( !dragged_foe_id.is_valid() || !has_effect( effect_dragging ) ) {
        dragged_foe_id = character_id();
        return nullptr;
    }

    // Dragged critters may die or otherwise become invalid, which is why we look
    // them up each time. Luckily, monsters dragging critters is relatively rare,
    // so this check should happen infrequently.
    Character *dragged_foe = g->critter_by_id<Character>( dragged_foe_id );

    if( dragged_foe == nullptr ) {
        // Target no longer valid.
        dragged_foe_id = character_id();
        remove_effect( effect_dragging );
    }

    return dragged_foe;
}

// Nursebot surgery code
void monster::nursebot_operate( Character *dragged_foe )
{
    // No dragged foe, nothing to do.
    if( dragged_foe == nullptr || !has_dest() ) {
        return;
    }

    // Nothing to do if they can't operate, or they don't think they're dragging.
    if( !( type->has_special_attack( "OPERATE" ) && has_effect( effect_dragging ) ) ) {
        return;
    }

    creature_tracker &creatures = get_creature_tracker();
    map &here = get_map();
    if( rl_dist( get_location(), get_dest() ) == 1 &&
        !here.has_flag_furn( ter_furn_flag::TFLAG_AUTODOC_COUCH, here.getlocal( get_dest() ) ) &&
        !has_effect( effect_operating ) ) {
        if( dragged_foe->has_effect( effect_grabbed ) && !has_effect( effect_countdown ) &&
            ( creatures.creature_at( get_dest() ) == nullptr ||
              creatures.creature_at( get_dest() ) == dragged_foe ) ) {
            add_msg( m_bad, _( "The %1$s slowly but firmly puts %2$s down onto the Autodoc couch." ), name(),
                     dragged_foe->disp_name() );

            dragged_foe->move_to( get_dest() );

            // There's still time to get away
            add_effect( effect_countdown, 2_turns );
            add_msg( m_bad, _( "The %s produces a syringe full of some translucent liquid." ), name() );
        } else if( creatures.creature_at( get_dest() ) != nullptr && has_effect( effect_dragging ) ) {
            sounds::sound( pos(), 8, sounds::sound_t::electronic_speech,
                           string_format(
                               _( "a soft robotic voice say, \"Please step away from the Autodoc, this patient needs immediate care.\"" ) ) );
            // TODO: Make it able to push NPC/player
            push_to( here.getlocal( get_dest() ), 4, 0 );
        }
    }
    if( get_effect_dur( effect_countdown ) == 1_turns && !has_effect( effect_operating ) ) {
        if( dragged_foe->has_effect( effect_grabbed ) ) {

            const bionic_collection &collec = *dragged_foe->my_bionics;
            cata_assert( !collec.empty() );
            const int index = rng( 0, collec.size() - 1 );
            const bionic *const target_cbm = &collec[index];
            const bionic &real_target =
                target_cbm->is_included()
                ? **dragged_foe->find_bionic_by_uid( target_cbm->get_parent_uid() )
                : *target_cbm;

            //8 intelligence*4 + 8 first aid*4 + 3 computer *3 + 4 electronic*1 = 77
            const float adjusted_skill = static_cast<float>( 77 ) - std::min( static_cast<float>( 40 ),
                                         static_cast<float>( 77 ) - static_cast<float>( 77 ) / static_cast<float>( 10.0 ) );

            dragged_foe->cancel_activity();
            get_player_character().uninstall_bionic( real_target, *this, *dragged_foe, adjusted_skill );

            dragged_foe->remove_effect( effect_grabbed );
            remove_effect( effect_dragging );
            dragged_foe_id = character_id();

        }
    }
}

// footsteps will determine how loud a monster's normal movement is
// and create a sound in the monsters location when they move
void monster::footsteps( const tripoint &p )
{
    if( is_hallucination() ) {
        return;
    }

    if( made_footstep ) {
        return;
    }
    made_footstep = true;
    int volume = 6; // same as player's footsteps
    if( flies() ) {
        volume = 0;    // Flying monsters don't have footsteps!
    }
    if( digging() ) {
        volume = 10;
    }
    switch( type->size ) {
        case creature_size::tiny:
            volume = 0; // No sound for the tinies
            break;
        case creature_size::small:
            volume /= 3;
            break;
        case creature_size::medium:
            break;
        case creature_size::large:
            volume *= 1.5;
            break;
        case creature_size::huge:
            volume *= 2;
            break;
        default:
            break;
    }
    if( has_flag( MF_LOUDMOVES ) ) {
        volume += 6;
    }
    if( volume == 0 ) {
        return;
    }
    int dist = rl_dist( p, get_player_character().pos() );
    sounds::add_footstep( p, volume, dist, this, type->get_footsteps() );
}

tripoint monster::scent_move()
{
    // TODO: Remove when scentmap is 3D
    if( std::abs( posz() - get_map().get_abs_sub().z() ) > SCENT_MAP_Z_REACH ) {
        return { -1, -1, INT_MIN };
    }
    scent_map &scents = get_scent();
    bool in_range = scents.inbounds( pos() );
    if( !in_range ) {
        return { -1, -1, INT_MIN };
    }

    const std::set<scenttype_id> &tracked_scents = type->scents_tracked;
    const std::set<scenttype_id> &ignored_scents = type->scents_ignored;

    std::vector<tripoint> smoves;

    int bestsmell = 10; // Squares with smell 0 are not eligible targets.
    int smell_threshold = 200; // Squares at or above this level are ineligible.
    if( has_flag( MF_KEENNOSE ) ) {
        bestsmell = 1;
        smell_threshold = 400;
    }

    Character &player_character = get_player_character();
    const bool fleeing = is_fleeing( player_character );
    int scent_here = scents.get_unsafe( pos() );
    if( fleeing ) {
        bestsmell = scent_here;
    }

    tripoint next( -1, -1, posz() );
    // When the scent is *either* too strong or too weak, can't follow it.
    if( ( !fleeing && scent_here > smell_threshold ) ||
        ( scent_here == 0 ) ) {
        return next;
    }
    // Check for the scent type being compatible.
    const scenttype_id &type_scent = scents.get_type();
    bool right_scent = false;
    // is the monster tracking this scent
    if( !tracked_scents.empty() ) {
        right_scent = tracked_scents.find( type_scent ) != tracked_scents.end();
    }
    //is this scent recognised by the monster species
    if( !type_scent.is_empty() ) {
        const std::set<species_id> &receptive_species = type_scent->receptive_species;
        const std::set<species_id> &monster_species = type->species;
        std::vector<species_id> v_intersection;
        std::set_intersection( receptive_species.begin(), receptive_species.end(), monster_species.begin(),
                               monster_species.end(), std::back_inserter( v_intersection ) );
        if( !v_intersection.empty() ) {
            right_scent = true;
        }
    }
    // is the monster actually ignoring this scent
    if( !ignored_scents.empty() && ( ignored_scents.find( type_scent ) != ignored_scents.end() ) ) {
        right_scent = false;
    }
    if( !right_scent ) {
        return { -1, -1, INT_MIN };
    }

    const bool can_bash = bash_skill() > 0;
    map &here = get_map();
    for( const tripoint &dest : here.points_in_radius( pos(), 1, SCENT_MAP_Z_REACH ) ) {
        int smell = scents.get( dest );

        if( ( !fleeing && smell < bestsmell ) || ( fleeing && smell > bestsmell ) ) {
            continue;
        }
        if( here.valid_move( pos(), dest, can_bash, true ) &&
            ( can_move_to( dest ) || ( dest == player_character.pos() ) ||
              ( can_bash && here.bash_rating( bash_estimate(), dest ) > 0 ) ) ) {
            if( ( !fleeing && smell > bestsmell ) || ( fleeing && smell < bestsmell ) ) {
                smoves.clear();
                smoves.push_back( dest );
                bestsmell = smell;
            } else if( ( !fleeing && smell == bestsmell ) || ( fleeing && smell == bestsmell ) ) {
                smoves.push_back( dest );
            }
        }
    }

    return random_entry( smoves, next );
}

int monster::calc_movecost( const tripoint &f, const tripoint &t ) const
{
    int movecost = 0;

    map &here = get_map();
    const int source_cost = here.move_cost( f );
    const int dest_cost = here.move_cost( t );
    // Digging and flying monsters ignore terrain cost
    if( flies() || ( digging() && here.has_flag( ter_furn_flag::TFLAG_DIGGABLE, t ) ) ) {
        movecost = 100;
        // Swimming monsters move super fast in water
    } else if( swims() ) {
        if( here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, f ) ) {
            movecost += 25;
        } else {
            movecost += 50 * here.move_cost( f );
        }
        if( here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, t ) ) {
            movecost += 25;
        } else {
            movecost += 50 * here.move_cost( t );
        }
    } else if( can_submerge() ) {
        // No-breathe monsters have to walk underwater slowly
        if( here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, f ) ) {
            movecost += 250;
        } else {
            movecost += 50 * here.move_cost( f );
        }
        if( here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, t ) ) {
            movecost += 250;
        } else {
            movecost += 50 * here.move_cost( t );
        }
        movecost /= 2;
    } else if( climbs() ) {
        if( here.has_flag( ter_furn_flag::TFLAG_CLIMBABLE, f ) ) {
            movecost += 150;
        } else {
            movecost += 50 * here.move_cost( f );
        }
        if( here.has_flag( ter_furn_flag::TFLAG_CLIMBABLE, t ) ) {
            movecost += 150;
        } else {
            movecost += 50 * here.move_cost( t );
        }
        movecost /= 2;
    } else {
        movecost = ( ( 50 * source_cost ) + ( 50 * dest_cost ) ) / 2.0;
    }

    return movecost;
}

int monster::calc_climb_cost( const tripoint &f, const tripoint &t ) const
{
    if( flies() ) {
        return 100;
    }

    map &here = get_map();
    if( climbs() && !here.has_flag( ter_furn_flag::TFLAG_NO_FLOOR, t ) ) {
        const int diff = here.climb_difficulty( f );
        if( diff <= 10 ) {
            return 150;
        }
    }

    return 0;
}

/*
 * Return points of an area extending 1 tile to either side and
 * (maxdepth) tiles behind basher.
 */
static std::vector<tripoint> get_bashing_zone( const tripoint &bashee, const tripoint &basher,
        int maxdepth )
{
    std::vector<tripoint> direction;
    direction.push_back( bashee );
    direction.push_back( basher );
    // Draw a line from the target through the attacker.
    std::vector<tripoint> path = continue_line( direction, maxdepth );
    // Remove the target.
    path.insert( path.begin(), basher );
    std::vector<tripoint> zone;
    // Go ahead and reserve enough room for all the points since
    // we know how many it will be.
    zone.reserve( 3 * maxdepth );
    tripoint previous = bashee;
    for( const tripoint &p : path ) {
        std::vector<point> swath = squares_in_direction( previous.xy(), p.xy() );
        for( const point &q : swath ) {
            zone.emplace_back( q, bashee.z );
        }

        previous = p;
    }
    return zone;
}

bool monster::bash_at( const tripoint &p )
{
    if( p.z != posz() ) {
        // TODO: Remove this
        return false;
    }

    //Hallucinations can't bash stuff.
    if( is_hallucination() ) {
        return false;
    }

    // Don't bash if a friendly monster is standing there
    monster *target = get_creature_tracker().creature_at<monster>( p );
    if( target != nullptr && attitude_to( *target ) == Attitude::FRIENDLY ) {
        return false;
    }

    bool try_bash = !can_move_to( p ) || one_in( 3 );
    if( !try_bash ) {
        return false;
    }

    if( bash_skill() <= 0 ) {
        return false;
    }

    map &here = get_map();
    if( !( here.is_bashable_furn( p ) || here.veh_at( p ).obstacle_at_part() ) ) {
        // if the only thing here is road or flat, rarely bash it
        bool flat_ground = here.has_flag( ter_furn_flag::TFLAG_ROAD, p ) ||
                           here.has_flag( ter_furn_flag::TFLAG_FLAT, p );
        if( !here.is_bashable_ter( p ) || ( flat_ground && !one_in( 50 ) ) ) {
            return false;
        }
    }

    int bashskill = group_bash_skill( p );
    here.bash( p, bashskill );
    sounds::tag_recent_sound_from_monster( p, this,
            sounds::sound_t::destructive_activity );
    moves -= 100;
    return true;
}

int monster::bash_estimate() const
{
    int estimate = bash_skill();
    if( has_flag( MF_GROUP_BASH ) ) {
        // Right now just give them a boost so they try to bash a lot of stuff.
        // TODO: base it on number of nearby friendlies.
        estimate *= 2;
    }
    return estimate;
}

int monster::bash_skill() const
{
    return type->bash_skill;
}

int monster::group_bash_skill( const tripoint &target )
{
    if( !has_flag( MF_GROUP_BASH ) ) {
        return bash_skill();
    }
    int bashskill = 0;

    // pileup = more bash skill, but only help bashing mob directly in front of target
    const int max_helper_depth = 5;
    const std::vector<tripoint> bzone = get_bashing_zone( target, pos(), max_helper_depth );

    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint &candidate : bzone ) {
        // Drawing this line backwards excludes the target and includes the candidate.
        std::vector<tripoint> path_to_target = line_to( target, candidate, 0, 0 );
        bool connected = true;
        monster *mon = nullptr;
        for( const tripoint &in_path : path_to_target ) {
            // If any point in the line from zombie to target is not a cooperating zombie,
            // it can't contribute.
            mon = creatures.creature_at<monster>( in_path );
            if( !mon ) {
                connected = false;
                break;
            }
            monster &helpermon = *mon;
            if( !helpermon.has_flag( MF_GROUP_BASH ) || helpermon.is_hallucination() ) {
                connected = false;
                break;
            }
        }
        if( !connected || !mon ) {
            continue;
        }
        // If we made it here, the last monster checked was the candidate.
        monster &helpermon = *mon;
        // Contribution falls off rapidly with distance from target.
        bashskill += helpermon.bash_skill() / rl_dist( candidate, target );
    }

    return bashskill;
}

bool monster::attack_at( const tripoint &p )
{
    if( has_flag( MF_PACIFIST ) ) {
        return false;
    }

    Character &player_character = get_player_character();
    if( p == player_character.pos() && sees( player_character ) ) {
        return melee_attack( player_character );
    }

    creature_tracker &creatures = get_creature_tracker();
    if( monster *mon_ = creatures.creature_at<monster>( p, is_hallucination() ) ) {
        monster &mon = *mon_;

        // Don't attack yourself.
        if( &mon == this ) {
            return false;
        }

        // With no melee dice, we can't attack, but we had to process until here
        // because hallucinations require no melee dice to destroy.
        if( type->melee_dice <= 0 ) {
            return false;
        }

        Creature::Attitude attitude = attitude_to( mon );
        // MF_ATTACKMON == hulk behavior, whack everything in your way
        if( attitude == Attitude::HOSTILE || has_flag( MF_ATTACKMON ) ) {
            return melee_attack( mon );
        }

        return false;
    }

    npc *const guy = creatures.creature_at<npc>( p, is_hallucination() );
    if( guy && type->melee_dice > 0 ) {
        // For now we're always attacking NPCs that are getting into our
        // way. This is consistent with how it worked previously, but
        // later on not hitting allied NPCs would be cool.
        guy->on_attacked( *this ); // allow NPC hallucination to be one shot by monsters
        return melee_attack( *guy );
    }

    // Nothing to attack.
    return false;
}

static tripoint find_closest_stair( const tripoint &near_this, const ter_furn_flag stair_type )
{
    map &here = get_map();
    for( const tripoint &candidate : closest_points_first( near_this, 10 ) ) {
        if( here.has_flag( stair_type, candidate ) ) {
            return candidate;
        }
    }
    // we didn't find it
    return near_this;
}

bool monster::move_to( const tripoint &p, bool force, bool step_on_critter,
                       const float stagger_adjustment )
{
    const bool on_ground = !digging() && !flies();

    const bool z_move = p.z != pos().z;
    const bool going_up = p.z > pos().z;

    tripoint destination = p;
    map &here = get_map();

    // This is stair teleportation hackery.
    // TODO: Remove this in favor of stair alignment
    if( going_up ) {
        if( here.has_flag( ter_furn_flag::TFLAG_GOES_UP, pos() ) ) {
            destination = find_closest_stair( p, ter_furn_flag::TFLAG_GOES_DOWN );
        }
    } else if( z_move ) {
        if( here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, pos() ) ) {
            destination = find_closest_stair( p, ter_furn_flag::TFLAG_GOES_UP );
        }
    }

    // Allows climbing monsters to move on terrain with movecost <= 0
    Creature *critter = get_creature_tracker().creature_at( destination, is_hallucination() );
    if( here.has_flag( ter_furn_flag::TFLAG_CLIMBABLE, destination ) ) {
        if( here.impassable( destination ) && critter == nullptr ) {
            if( flies() ) {
                moves -= 100;
                force = true;
                add_msg_if_player_sees( *this, _( "The %1$s flies over the %2$s." ), name(),
                                        here.has_flag_furn( ter_furn_flag::TFLAG_CLIMBABLE, p ) ? here.furnname( p ) :
                                        here.tername( p ) );
            } else if( climbs() ) {
                moves -= 150;
                force = true;
                add_msg_if_player_sees( *this, _( "The %1$s climbs over the %2$s." ), name(),
                                        here.has_flag_furn( ter_furn_flag::TFLAG_CLIMBABLE, p ) ? here.furnname( p ) :
                                        here.tername( p ) );
            }
        }
    }

    if( critter != nullptr && !step_on_critter ) {
        return false;
    }

    // Make sure that we can move there, unless force is true.
    if( !force && !can_move_to( destination ) ) {
        return false;
    }

    if( !force ) {
        // This adjustment is to make it so that monster movement speed relative to the player
        // is consistent even if the monster stumbles,
        // and the same regardless of the distance measurement mode.
        // Note: Keep this as float here or else it will cancel valid moves
        const float cost = stagger_adjustment *
                           static_cast<float>( climbs() &&
                                               here.has_flag( ter_furn_flag::TFLAG_NO_FLOOR, p ) ? calc_climb_cost( pos(),
                                                       destination ) : calc_movecost( pos(),
                                                               destination ) );
        if( cost > 0.0f ) {
            moves -= static_cast<int>( std::ceil( cost ) );
        } else {
            return false;
        }
    }

    //Check for moving into/out of water
    bool was_water = underwater;
    bool will_be_water =
        on_ground && (
            // AQUATIC monsters always "swim under" the vehicles, while other swimming monsters are forced to surface
            has_flag( MF_AQUATIC ) || ( can_submerge() && !here.veh_at( destination ) )
        ) && here.is_divable( destination );

    //Birds and other flying creatures flying over the deep water terrain
    if( was_water && flies() ) {
        if( one_in( 4 ) ) {
            add_msg_if_player_sees( *this, m_warning, _( "A %1$s flies over the %2$s!" ),
                                    name(), here.tername( pos() ) );
        }
    } else if( was_water && !will_be_water ) {
        // Use more dramatic messages for swimming monsters
        add_msg_if_player_sees( *this, m_warning,
                                //~ Message when a monster emerges from water
                                //~ %1$s: monster name, %2$s: leaps/emerges, %3$s: terrain name
                                pgettext( "monster movement", "A %1$s %2$s from the %3$s!" ),
                                name(), swims() || has_flag( MF_AQUATIC ) ? _( "leaps" ) : _( "emerges" ), here.tername( pos() ) );
    } else if( !was_water && will_be_water ) {
        add_msg_if_player_sees( *this, m_warning, pgettext( "monster movement",
                                //~ Message when a monster enters water
                                //~ %1$s: monster name, %2$s: dives/sinks, %3$s: terrain name
                                "A %1$s %2$s into the %3$s!" ),
                                name(), swims() ||
                                has_flag( MF_AQUATIC ) ? _( "dives" ) : _( "sinks" ), here.tername( destination ) );
    }

    setpos( destination );
    footsteps( destination );
    underwater = will_be_water;
    if( is_hallucination() ) {
        //Hallucinations don't do any of the stuff after this point
        return true;
    }

    if( type->size != creature_size::tiny && on_ground ) {
        const int sharp_damage = rng( 1, 10 );
        const int rough_damage = rng( 1, 2 );
        if( here.has_flag( ter_furn_flag::TFLAG_SHARP, pos() ) && !one_in( 4 ) &&
            get_armor_cut( bodypart_id( "torso" ) ) < sharp_damage && get_hp() > sharp_damage ) {
            apply_damage( nullptr, bodypart_id( "torso" ), sharp_damage );
        }
        if( here.has_flag( ter_furn_flag::TFLAG_ROUGH, pos() ) && one_in( 6 ) &&
            get_armor_cut( bodypart_id( "torso" ) ) < rough_damage && get_hp() > rough_damage ) {
            apply_damage( nullptr, bodypart_id( "torso" ), rough_damage );
        }
    }

    if( here.has_flag( ter_furn_flag::TFLAG_UNSTABLE, destination ) && on_ground ) {
        add_effect( effect_bouldering, 1_turns, true );
    } else if( has_effect( effect_bouldering ) ) {
        remove_effect( effect_bouldering );
    }

    if( here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_NO_SIGHT, destination ) && on_ground ) {
        add_effect( effect_no_sight, 1_turns, true );
    } else if( has_effect( effect_no_sight ) ) {
        remove_effect( effect_no_sight );
    }

    here.creature_on_trap( *this );
    if( is_dead() ) {
        return true;
    }
    if( !will_be_water && ( digs() || can_dig() ) ) {
        underwater = here.has_flag( ter_furn_flag::TFLAG_DIGGABLE, pos() );
    }

    // Digging creatures leave a trail of churned earth
    // They always leave some on their tile, and larger creatures emit some around themselves as well
    if( digging() && here.has_flag( ter_furn_flag::TFLAG_DIGGABLE, pos() ) ) {
        int factor = 0;
        switch( type->size ) {
            case creature_size::medium:
                factor = 4;
                break;
            case creature_size::large:
                factor = 3;
                break;
            case creature_size::huge:
                factor = 2;
                break;
            case creature_size::num_sizes:
                debugmsg( "ERROR: Invalid Creature size class." );
                break;
            default:
                factor = 4;
                break;
        }
        here.add_field( pos(), fd_churned_earth, 2 );
        for( const tripoint &dest : here.points_in_radius( pos(), 1, 0 ) ) {
            if( here.has_flag( ter_furn_flag::TFLAG_DIGGABLE, dest ) && one_in( factor ) ) {
                here.add_field( dest, fd_churned_earth, 2 );
            }
        }
    }

    // Acid trail monsters leave... a trail of acid
    if( has_flag( MF_ACIDTRAIL ) ) {
        here.add_field( pos(), fd_acid, 3 );
    }

    // Not all acid trail monsters leave as much acid. Every time this monster takes a step, there is a 1/5 chance it will drop a puddle.
    if( has_flag( MF_SHORTACIDTRAIL ) ) {
        if( one_in( 5 ) ) {
            here.add_field( pos(), fd_acid, 3 );
        }
    }

    if( has_flag( MF_SLUDGETRAIL ) ) {
        for( const tripoint &sludge_p : here.points_in_radius( pos(), 1 ) ) {
            const int fstr = 3 - ( std::abs( sludge_p.x - posx() ) + std::abs( sludge_p.y - posy() ) );
            if( fstr >= 2 ) {
                here.add_field( sludge_p, fd_sludge, fstr );
            }
        }
    }

    if( has_flag( MF_SMALLSLUDGETRAIL ) ) {
        if( one_in( 2 ) ) {
            here.add_field( pos(), fd_sludge, 1 );
        }
    }

    // Don't leave any kind of liquids on water tiles
    if( !here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, destination ) ) {
        if( has_flag( MF_DRIPS_NAPALM ) ) {
            if( one_in( 10 ) ) {
                // if it has more napalm, drop some and reduce ammo in tank
                if( ammo[itype_pressurized_tank] > 0 ) {
                    here.add_item_or_charges( pos(), item( "napalm", calendar::turn, 50 ) );
                    ammo[itype_pressurized_tank] -= 50;
                } else {
                    // TODO: remove MF_DRIPS_NAPALM flag since no more napalm in tank
                    // Not possible for now since flag check is done on type, not individual monster
                }
            }
        }
        if( has_flag( MF_DRIPS_GASOLINE ) ) {
            if( one_in( 5 ) ) {
                // TODO: use same idea that limits napalm dripping
                here.add_item_or_charges( pos(), item( "gasoline" ) );
            }
        }
    }
    return true;
}

bool monster::push_to( const tripoint &p, const int boost, const size_t depth )
{
    if( is_hallucination() ) {
        // Don't let hallucinations push, not even other hallucinations
        return false;
    }

    if( !has_flag( MF_PUSH_MON ) || depth > 2 || has_effect( effect_pushed ) ) {
        return false;
    }

    creature_tracker &creatures = get_creature_tracker();
    // TODO: Generalize this to Creature
    monster *const critter = creatures.creature_at<monster>( p );
    if( critter == nullptr || critter == this ||
        p == pos() || critter->movement_impaired() ) {
        return false;
    }

    if( !can_move_to( p ) ) {
        return false;
    }

    if( critter->is_hallucination() ) {
        // Kill the hallu, but return false so that the regular move_to is uses instead
        critter->die( nullptr );
        return false;
    }

    // Stability roll of the pushed critter
    const int defend = critter->stability_roll();
    // Stability roll of the pushing zed
    const int attack = stability_roll() + boost;
    if( defend > attack ) {
        return false;
    }

    map &here = get_map();
    const int movecost_from = 50 * here.move_cost( p );
    const int movecost_attacker = std::max( movecost_from, 200 - 10 * ( attack - defend ) );
    const tripoint dir = p - pos();

    // Mark self as pushed to simplify recursive pushing
    add_effect( effect_pushed, 1_turns );

    for( size_t i = 0; i < 6; i++ ) {
        const point d( rng( -1, 1 ), rng( -1, 1 ) );
        if( d.x == 0 && d.y == 0 ) {
            continue;
        }

        // Pushing forward is easier than pushing aside
        const int direction_penalty = std::abs( d.x - dir.x ) + std::abs( d.y - dir.y );
        if( direction_penalty > 2 ) {
            continue;
        }

        tripoint dest( p + d );
        const int dest_movecost_from = 50 * here.move_cost( dest );

        // Pushing into cars/windows etc. is harder
        const int movecost_penalty = here.move_cost( dest ) - 2;
        if( movecost_penalty <= -2 ) {
            // Can't push into unpassable terrain
            continue;
        }

        int roll = attack - ( defend + direction_penalty + movecost_penalty );
        if( roll < 0 ) {
            continue;
        }

        Creature *critter_recur = creatures.creature_at( dest );
        if( !( critter_recur == nullptr || critter_recur->is_hallucination() ) ) {
            // Try to push recursively
            monster *mon_recur = dynamic_cast< monster * >( critter_recur );
            if( mon_recur == nullptr ) {
                continue;
            }

            if( critter->push_to( dest, roll, depth + 1 ) ) {
                // The tile isn't necessarily free, need to check
                if( !creatures.creature_at( p ) ) {
                    move_to( p );
                }

                moves -= movecost_attacker;

                // Don't knock down a creature that successfully
                // pushed another creature, just reduce moves
                critter->moves -= dest_movecost_from;
                return true;
            } else {
                return false;
            }
        }

        critter_recur = creatures.creature_at( dest );
        if( critter_recur != nullptr ) {
            if( critter_recur->is_hallucination() ) {
                critter_recur->die( nullptr );
            }
        } else if( !critter->has_flag( MF_IMMOBILE ) ) {
            critter->setpos( dest );
            move_to( p );
            moves -= movecost_attacker;
            critter->add_effect( effect_downed, time_duration::from_turns( movecost_from / 100 + 1 ) );
        }
        return true;
    }

    // Try to trample over a much weaker zed (or one with worse rolls)
    // Don't allow trampling with boost
    if( boost > 0 || attack < 2 * defend ) {
        return false;
    }

    g->swap_critters( *critter, *this );
    critter->add_effect( effect_stunned, rng( 0_turns, 2_turns ) );
    Character &player_character = get_player_character();
    // Only print the message when near player or it can get spammy
    if( rl_dist( player_character.pos(), pos() ) < 4 ) {
        add_msg_if_player_sees( *critter, m_warning, _( "The %1$s tramples %2$s." ),
                                name(), critter->disp_name() );
    }

    moves -= movecost_attacker;
    if( movecost_from > 100 ) {
        critter->add_effect( effect_downed, time_duration::from_turns( movecost_from / 100 + 1 ) );
    } else {
        critter->moves -= movecost_from;
    }

    return true;
}

/**
 * Stumble in a random direction, but with some caveats.
 */
void monster::stumble()
{
    // Only move every 10 turns.
    if( !one_in( 10 ) ) {
        return;
    }

    map &here = get_map();
    std::vector<tripoint> valid_stumbles;
    valid_stumbles.reserve( 11 );
    const bool avoid_water = has_flag( MF_NO_BREATHE ) && !swims() && !has_flag( MF_AQUATIC );
    for( const tripoint &dest : here.points_in_radius( pos(), 1 ) ) {
        if( dest != pos() ) {
            if( here.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, dest ) ) {
                valid_stumbles.emplace_back( dest.xy(), dest.z - 1 );
            } else  if( here.has_flag( ter_furn_flag::TFLAG_RAMP_UP, dest ) ) {
                valid_stumbles.emplace_back( dest.xy(), dest.z + 1 );
            } else {
                valid_stumbles.push_back( dest );
            }
        }
    }
    const tripoint below( posx(), posy(), posz() - 1 );
    if( here.valid_move( pos(), below, false, true ) ) {
        valid_stumbles.push_back( below );
    }

    creature_tracker &creatures = get_creature_tracker();
    while( !valid_stumbles.empty() && !is_dead() ) {
        const tripoint dest = random_entry_removed( valid_stumbles );
        if( can_move_to( dest ) &&
            //Stop zombies and other non-breathing monsters wandering INTO water
            //(Unless they can swim/are aquatic)
            //But let them wander OUT of water if they are there.
            !( avoid_water &&
               here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, dest ) &&
               !here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, pos() ) ) &&
            ( creatures.creature_at( dest, is_hallucination() ) == nullptr ) ) {
            if( move_to( dest, true, false ) ) {
                break;
            }
        }
    }
}

void monster::knock_back_to( const tripoint &to )
{
    if( to == pos() ) {
        return; // No effect
    }

    if( is_hallucination() ) {
        die( nullptr );
        return;
    }

    bool u_see = get_player_view().sees( to );

    creature_tracker &creatures = get_creature_tracker();
    // First, see if we hit another monster
    if( monster *const z = creatures.creature_at<monster>( to ) ) {
        apply_damage( z, bodypart_id( "torso" ), static_cast<float>( z->type->size ) );
        add_effect( effect_stunned, 1_turns );
        if( type->size > 1 + z->type->size ) {
            z->knock_back_from( pos() ); // Chain reaction!
            z->apply_damage( this, bodypart_id( "torso" ), static_cast<float>( type->size ) );
            z->add_effect( effect_stunned, 1_turns );
        } else if( type->size > z->type->size ) {
            z->apply_damage( this, bodypart_id( "torso" ), static_cast<float>( type->size ) );
            z->add_effect( effect_stunned, 1_turns );
        }
        z->check_dead_state();

        if( u_see ) {
            add_msg( _( "The %1$s bounces off a %2$s!" ), name(), z->name() );
        }

        return;
    }

    if( npc *const p = creatures.creature_at<npc>( to ) ) {
        apply_damage( p, bodypart_id( "torso" ), 3 );
        add_effect( effect_stunned, 1_turns );
        p->deal_damage( this, bodypart_id( "torso" ),
                        damage_instance( damage_type::BASH, static_cast<float>( type->size ) ) );
        if( u_see ) {
            add_msg( _( "The %1$s bounces off %2$s!" ), name(), p->get_name() );
        }

        p->check_dead_state();
        return;
    }

    // If we're still in the function at this point, we're actually moving a tile!
    // die_if_drowning will kill the monster if necessary, but if the deep water
    // tile is on a vehicle, we should check for swimmers out of water
    if( !die_if_drowning( to ) && has_flag( MF_AQUATIC ) ) {
        die( nullptr );
        if( u_see ) {
            add_msg( _( "The %s flops around and dies!" ), name() );
        }
    }

    map &here = get_map();
    // It's some kind of wall.
    if( here.impassable( to ) ) {
        const int dam = static_cast<int>( type->size );
        apply_damage( nullptr, bodypart_id( "torso" ), dam );
        add_effect( effect_stunned, 2_turns );
        if( u_see ) {
            add_msg( _( "The %1$s bounces off a %2$s and takes %3$d damage." ), name(),
                     here.obstacle_name( to ), dam );
        }

    } else { // It's no wall
        setpos( to );
        here.creature_on_trap(*this);
    }
    check_dead_state();
}

/* will_reach() is used for determining whether we'll get to stairs (and
 * potentially other locations of interest).  It is generally permissive.
 * TODO: Pathfinding;
         Make sure that non-smashing monsters won't "teleport" through windows
         Injure monsters if they're gonna be walking through pits or whatever
 */
bool monster::will_reach( const point &p )
{
    monster_attitude att = attitude( &get_player_character() );
    if( att != MATT_FOLLOW && att != MATT_ATTACK && att != MATT_FRIEND ) {
        return false;
    }

    if( digs() || has_flag( MF_AQUATIC ) ) {
        return false;
    }

    if( ( has_flag( MF_IMMOBILE ) || has_flag( MF_RIDEABLE_MECH ) ) && ( pos().xy() != p ) ) {
        return false;
    }

    auto path = get_map().route( pos(), tripoint( p, posz() ), get_pathfinding_settings() );
    if( path.empty() ) {
        return false;
    }

    if( has_flag( MF_SMELLS ) && get_scent().get( pos() ) > 0 &&
        get_scent().get( { p, posz() } ) > get_scent().get( pos() ) ) {
        return true;
    }

    if( can_hear() && wandf > 0 && rl_dist( get_map().getlocal( wander_pos ).xy(), p ) <= 2 &&
        rl_dist( get_location().xy(), wander_pos.xy() ) <= wandf ) {
        return true;
    }

    if( can_see() && sees( tripoint( p, posz() ) ) ) {
        return true;
    }

    return false;
}

int monster::turns_to_reach( const point &p )
{
    map &here = get_map();
    // HACK: This function is a(n old) temporary hack that should soon be removed
    auto path = here.route( pos(), tripoint( p, posz() ), get_pathfinding_settings() );
    if( path.empty() ) {
        return 999;
    }

    double turns = 0.;
    for( size_t i = 0; i < path.size(); i++ ) {
        const tripoint &next = path[i];
        if( here.impassable( next ) ) {
            // No bashing through, it looks stupid when you go back and find
            // the doors intact.
            return 999;
        } else if( i == 0 ) {
            turns += static_cast<double>( calc_movecost( pos(), next ) ) / get_speed();
        } else {
            turns += static_cast<double>( calc_movecost( path[i - 1], next ) ) / get_speed();
        }
    }

    return static_cast<int>( turns + .9 ); // Halve (to get turns) and round up
}

void monster::shove_vehicle( const tripoint &remote_destination,
                             const tripoint &nearby_destination )
{
    map &here = get_map();
    if( this->has_flag( MF_PUSH_VEH ) && !is_hallucination() ) {
        optional_vpart_position vp = here.veh_at( nearby_destination );
        if( vp ) {
            vehicle &veh = vp->vehicle();
            const units::mass veh_mass = veh.total_mass();
            int shove_moves_minimal = 0;
            int shove_veh_mass_moves_factor = 0;
            int shove_velocity = 0;
            float shove_damage_min = 0.00F;
            float shove_damage_max = 0.00F;
            switch( this->get_size() ) {
                case creature_size::tiny:
                case creature_size::small:
                    break;
                case creature_size::medium:
                    if( veh_mass < 500_kilogram ) {
                        shove_moves_minimal = 150;
                        shove_veh_mass_moves_factor = 20;
                        shove_velocity = 500;
                        shove_damage_min = 0.00F;
                        shove_damage_max = 0.01F;
                    }
                    break;
                case creature_size::large:
                    if( veh_mass < 1000_kilogram ) {
                        shove_moves_minimal = 100;
                        shove_veh_mass_moves_factor = 8;
                        shove_velocity = 1000;
                        shove_damage_min = 0.00F;
                        shove_damage_max = 0.03F;
                    }
                    break;
                case creature_size::huge:
                    if( veh_mass < 2000_kilogram ) {
                        shove_moves_minimal = 50;
                        shove_veh_mass_moves_factor = 4;
                        shove_velocity = 1500;
                        shove_damage_min = 0.00F;
                        shove_damage_max = 0.05F;
                    }
                    break;
                default:
                    break;
            }
            if( shove_velocity > 0 ) {
                //~ %1$s - monster name, %2$s - vehicle name
                add_msg_if_player_sees( this->pos(), m_bad, _( "%1$s shoves %2$s out of their way!" ),
                                        this->disp_name(),
                                        veh.disp_name() );
                int shove_moves = shove_veh_mass_moves_factor * veh_mass / 10_kilogram;
                shove_moves = std::max( shove_moves, shove_moves_minimal );
                this->mod_moves( -shove_moves );
                const tripoint destination_delta( -nearby_destination + remote_destination );
                const tripoint shove_destination( clamp( destination_delta.x, -1, 1 ),
                                                  clamp( destination_delta.y, -1, 1 ),
                                                  clamp( destination_delta.z, -1, 1 ) );
                veh.skidding = true;
                veh.velocity = shove_velocity;
                if( shove_destination != tripoint_zero ) {
                    if( shove_destination.z != 0 ) {
                        veh.vertical_velocity = shove_destination.z < 0 ? -shove_velocity : +shove_velocity;
                    }
                    here.move_vehicle( veh, shove_destination, veh.face );
                }
                veh.move = tileray( destination_delta.xy() );
                veh.smash( here, shove_damage_min, shove_damage_max, 0.10F );
            }
        }
    }
}
