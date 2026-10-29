#include "pathfinding.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <memory>
#include <queue>
#include <set>
#include <utility>
#include <vector>

#include "cata_utility.h"
#include "coordinates.h"
#include "debug.h"
#include "game.h"
#include "line.h"
#include "map.h"
#include "mapdata.h"
#include <optional>
#include "point.h"
#include "submap.h"
#include "trap.h"
#include "type_id.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

enum astar_state {
    ASL_NONE,
    ASL_OPEN,
    ASL_CLOSED
};

// state 数组按 (generation << 2) | astar_state 打包。每次寻路换一个
// generation，格子里存的 generation 对不上就当作 ASL_NONE，于是不必在每次
// 寻路前把整层 state 清零（原本是一次 memset，独占约 6% 采样）。新分配的
// 层数值初始化为 0，对应 generation 0，与从 1 开始的 generation 永远不等，
// 连首次清零都省掉了。
static constexpr uint32_t astar_state_bits = 2;
static constexpr uint32_t astar_state_mask = ( 1u << astar_state_bits ) - 1;
static constexpr uint32_t astar_generation_max = ( 1u << 30 ) - 1;

// Turns two indexed to a 2D array into an index to equivalent 1D array
static constexpr int flat_index( const point &p )
{
    return ( p.x * MAPSIZE_Y ) + p.y;
}

// Flattened 2D array representing a single z-level worth of pathfinding data
struct path_data_layer {
    // State is accessed way more often than all other values here
    std::array< uint32_t, MAPSIZE_X *MAPSIZE_Y > state;
    std::array< int, MAPSIZE_X *MAPSIZE_Y > score;
    std::array< int, MAPSIZE_X *MAPSIZE_Y > gscore;
    std::array< tripoint, MAPSIZE_X *MAPSIZE_Y > parent;

    astar_state get_state( const int index, const uint32_t generation ) const {
        const uint32_t packed = state[index];
        return ( packed >> astar_state_bits ) == generation ?
               static_cast<astar_state>( packed & astar_state_mask ) : ASL_NONE;
    }

    void set_state( const int index, const uint32_t generation, const astar_state value ) {
        state[index] = ( generation << astar_state_bits ) | static_cast<uint32_t>( value );
    }

    // 只有 generation 溢出时才需要真正清零，一局游戏内几乎不会走到
    void clear_states() {
        state.fill( 0 );
    }
};

// 各层的寻路数据在整个进程内复用：每次 map::route 只换一个 generation，
// 而不是重新分配并清零整层，省掉每层的 operator new 和 memset。
// map::route 里唯一的递归是越界裁剪后的 `return route( ... )`，外层在递归
// 返回后立即退出、不再读写自己的状态，因此共用同一份数组是安全的。
static std::array< std::unique_ptr< path_data_layer >, OVERMAP_LAYERS > shared_path_data;
static uint32_t astar_generation = 0;

static uint32_t next_astar_generation()
{
    if( astar_generation >= astar_generation_max ) {
        for( std::unique_ptr< path_data_layer > &layer : shared_path_data ) {
            if( layer ) {
                layer->clear_states();
            }
        }
        astar_generation = 0;
    }
    return ++astar_generation;
}

// open 优先队列的条目被压进一个 uint64_t：
//   [ score : 43 位 ][ z+偏移 : 5 位 ][ y : 8 位 ][ x : 8 位 ]
// score 放高位，于是默认的整数比较等价于"先比 score"，与原先
// pair_greater_cmp_first 只比较 first 的行为一致。坐标全部落在
// [0, MAPSIZE_X/Y) 与 [-OVERMAP_DEPTH, OVERMAP_HEIGHT] 内，各字段都够放。
// 这样每次入堆/出堆搬移的是 8 字节整数，而不是 16 字节的 pair<int, tripoint>，
// 省去堆调整时对 tripoint 的搬移和一次自定义比较器调用。
static constexpr int open_step_x_bits = 8;
static constexpr int open_step_y_bits = 8;
static constexpr int open_step_z_bits = 5;
static constexpr int open_step_coord_bits = open_step_x_bits + open_step_y_bits + open_step_z_bits;
static constexpr uint64_t open_step_x_mask = ( uint64_t( 1 ) << open_step_x_bits ) - 1;
static constexpr uint64_t open_step_y_mask = ( uint64_t( 1 ) << open_step_y_bits ) - 1;
static constexpr uint64_t open_step_z_mask = ( uint64_t( 1 ) << open_step_z_bits ) - 1;

static uint64_t pack_open_step( const int score, const tripoint &p )
{
    const uint64_t coord = ( static_cast<uint64_t>( p.x ) & open_step_x_mask ) |
                           ( ( static_cast<uint64_t>( p.y ) & open_step_y_mask ) << open_step_x_bits ) |
                           ( ( static_cast<uint64_t>( p.z + OVERMAP_DEPTH ) & open_step_z_mask )
                             << ( open_step_x_bits + open_step_y_bits ) );
    return ( static_cast<uint64_t>( score ) << open_step_coord_bits ) | coord;
}

static tripoint unpack_open_step( const uint64_t packed )
{
    const uint64_t coord = packed & ( ( uint64_t( 1 ) << open_step_coord_bits ) - 1 );
    const int x = static_cast<int>( coord & open_step_x_mask );
    const int y = static_cast<int>( ( coord >> open_step_x_bits ) & open_step_y_mask );
    const int z = static_cast<int>(
                      ( coord >> ( open_step_x_bits + open_step_y_bits ) ) & open_step_z_mask ) - OVERMAP_DEPTH;
    return tripoint( x, y, z );
}

struct pathfinder {
    uint32_t generation;

    // 最小堆：greater 使 score 最小者先出，等价于原先 pair_greater_cmp_first 的效果。
    std::priority_queue< uint64_t, std::vector<uint64_t>, std::greater<uint64_t> > open;

    pathfinder() : generation( next_astar_generation() ) {
    }

    path_data_layer &get_layer( const int z ) {
        std::unique_ptr< path_data_layer > &ptr = shared_path_data[z + OVERMAP_DEPTH];
        if( ptr == nullptr ) {
            ptr = std::make_unique<path_data_layer>();
        }
        return *ptr;
    }

    bool empty() const {
        return open.empty();
    }

    tripoint get_next() {
        const uint64_t packed = open.top();
        open.pop();
        return unpack_open_step( packed );
    }

    void add_point( const int gscore, const int score, const tripoint &from, const tripoint &to ) {
        path_data_layer &layer = get_layer( to.z );
        const int index = flat_index( to.xy() );
        const astar_state state = layer.get_state( index, generation );
        if( ( state == ASL_OPEN && gscore >= layer.gscore[index] ) ||
            state == ASL_CLOSED ) {
            return;
        }

        layer.set_state( index, generation, ASL_OPEN );
        layer.gscore[index] = gscore;
        layer.parent[index] = from;
        layer.score [index] = score;
        open.push( pack_open_step( score, to ) );
    }

    void close_point( const tripoint &p ) {
        path_data_layer &layer = get_layer( p.z );
        const int index = flat_index( p.xy() );
        layer.set_state( index, generation, ASL_CLOSED );
    }

    void unclose_point( const tripoint &p ) {
        path_data_layer &layer = get_layer( p.z );
        const int index = flat_index( p.xy() );
        layer.set_state( index, generation, ASL_NONE );
    }
};

// Modifies `t` to point to a tile with `flag` in a 1-submap radius of `t`'s original value,
// searching nearest points first (starting with `t` itself).
// return false if it could not find a suitable point
static bool vertical_move_destination( const map &m, ter_furn_flag flag, tripoint &t )
{
    const pathfinding_cache &pf_cache = m.get_pathfinding_cache_ref( t.z );
    for( const point &p : closest_points_first( t.xy(), SEEX ) ) {
        if( pf_cache.special[p.x][p.y] & PF_UPDOWN ) {
            const tripoint t2( p, t.z );
            if( m.has_flag( flag, t2 ) ) {
                t = t2;
                return true;
            }
        }
    }
    return false;
}

template<class Set1, class Set2>
static bool is_disjoint( const Set1 &set1, const Set2 &set2 )
{
    if( set1.empty() || set2.empty() ) {
        return true;
    }

    typename Set1::const_iterator it1 = set1.begin();
    typename Set1::const_iterator it1_end = set1.end();

    typename Set2::const_iterator it2 = set2.begin();
    typename Set2::const_iterator it2_end = set2.end();

    if( *set2.rbegin() < *it1 || *set1.rbegin() < *it2 ) {
        return true;
    }

    while( it1 != it1_end && it2 != it2_end ) {
        if( *it1 == *it2 ) {
            return false;
        }
        if( *it1 < *it2 ) {
            it1++;
        } else {
            it2++;
        }
    }

    return true;
}

std::vector<tripoint> map::route( const tripoint &f, const tripoint &t,
                                  const pathfinding_settings &settings,
                                  const std::set<tripoint> &pre_closed ) const
{
    /* TODO: If the origin or destination is out of bound, figure out the closest
     * in-bounds point and go to that, then to the real origin/destination.
     */
    std::vector<tripoint> ret;

    if( f == t || !inbounds( f ) ) {
        return ret;
    }

    if( !inbounds( t ) ) {
        tripoint clipped = t;
        clip_to_bounds( clipped );
        return route( f, clipped, settings, pre_closed );
    }
    // First, check for a simple straight line on flat ground
    // Except when the line contains a pre-closed tile - we need to do regular pathing then
    static const pf_special non_normal = PF_SLOW | PF_WALL | PF_VEHICLE | PF_TRAP | PF_SHARP;
    if( f.z == t.z ) {
        auto line_path = line_to( f, t );
        const pathfinding_cache &pf_cache = get_pathfinding_cache_ref( f.z );
        // Check all points for any special case (including just hard terrain)
        if( std::all_of( line_path.begin(), line_path.end(), [&pf_cache]( const tripoint & p ) {
        return !( pf_cache.special[p.x][p.y] & non_normal );
        } ) ) {
            const std::set<tripoint> sorted_line( line_path.begin(), line_path.end() );

            if( is_disjoint( sorted_line, pre_closed ) ) {
                return line_path;
            }
        }
    }

    // If expected path length is greater than max distance, allow only line path, like above
    if( rl_dist( f, t ) > settings.max_dist ) {
        return ret;
    }

    int max_length = settings.max_length;
    int bash = settings.bash_strength;
    int climb_cost = settings.climb_cost;
    bool doors = settings.allow_open_doors;
    bool trapavoid = settings.avoid_traps;
    bool roughavoid = settings.avoid_rough_terrain;
    bool sharpavoid = settings.avoid_sharp;

    const int pad = 16;  // Should be much bigger - low value makes pathfinders dumb!
    tripoint min( std::min( f.x, t.x ) - pad, std::min( f.y, t.y ) - pad, std::min( f.z, t.z ) );
    tripoint max( std::max( f.x, t.x ) + pad, std::max( f.y, t.y ) + pad, std::max( f.z, t.z ) );
    clip_to_bounds( min.x, min.y, min.z );
    clip_to_bounds( max.x, max.y, max.z );

    pathfinder pf;
    // Make NPCs not want to path through player
    // But don't make player pathing stop working
    for( const tripoint &p : pre_closed ) {
        if( p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y ) {
            pf.close_point( p );
        }
    }

    // Start and end must not be closed
    pf.unclose_point( f );
    pf.unclose_point( t );
    pf.add_point( 0, 0, f, f );

    bool done = false;

    do {
        tripoint cur = pf.get_next();

        const int parent_index = flat_index( cur.xy() );
        path_data_layer &layer = pf.get_layer( cur.z );
        if( layer.get_state( parent_index, pf.generation ) == ASL_CLOSED ) {
            continue;
        }

        if( layer.gscore[parent_index] > max_length ) {
            // Shortest path would be too long, return empty vector
            return std::vector<tripoint>();
        }

        if( cur == t ) {
            done = true;
            break;
        }

        layer.set_state( parent_index, pf.generation, ASL_CLOSED );

        const pathfinding_cache &pf_cache = get_pathfinding_cache_ref( cur.z );
        const pf_special cur_special = pf_cache.special[cur.x][cur.y];

        // 7 3 5
        // 1 . 2
        // 6 4 8
        constexpr std::array<int, 8> x_offset{{ -1,  1,  0,  0,  1, -1, -1, 1 }};
        constexpr std::array<int, 8> y_offset{{  0,  0, -1,  1, -1,  1, -1, 1 }};
        for( size_t i = 0; i < 8; i++ ) {
            const tripoint p( cur.x + x_offset[i], cur.y + y_offset[i], cur.z );
            const int index = flat_index( p.xy() );

            // TODO: Remove this and instead have sentinels at the edges
            if( p.x < min.x || p.x >= max.x || p.y < min.y || p.y >= max.y ) {
                continue;
            }

            if( layer.get_state( index, pf.generation ) == ASL_CLOSED ) {
                continue;
            }

            // Penalize for diagonals or the path will look "unnatural"
            int newg = layer.gscore[parent_index] + ( ( cur.x != p.x && cur.y != p.y ) ? 1 : 0 );

            const pf_special p_special = pf_cache.special[p.x][p.y];
            // TODO: De-uglify, de-huge-n
            if( !( p_special & non_normal ) ) {
                // Boring flat dirt - the most common case above the ground
                newg += 2;
            } else {
                if( roughavoid ) {
                    layer.set_state( index, pf.generation, ASL_CLOSED ); // Close all rough terrain tiles
                    continue;
                }

                int part = -1;
                const const_maptile &tile = maptile_at_internal( p );
                const ter_t &terrain = tile.get_ter_t();
                const furn_t &furniture = tile.get_furn_t();
                const field &field = tile.get_field();
                const vehicle *veh = veh_at_internal( p, part );

                const int cost = move_cost_internal( furniture, terrain, field, veh, part );
                const int terrain_cost = move_cost_internal( furniture, terrain, field, nullptr, -1 );
                // Vehicle obstacles and the underlying terrain/furniture are separate layers.
                // Neither layer may make an otherwise impassable tile silently traversable.
                const int terrain_rating = ( bash == 0 || terrain_cost != 0 ) ? -1 :
                                           bash_rating_internal( bash, furniture, terrain, false, nullptr, -1 );

                newg += cost;
                if( cost == 0 ) {
                    // First account for the underlying terrain/furniture.  This prevents a vehicle or
                    // appliance part from masking a wall, closed furniture, or an impassable field.
                    if( terrain_cost == 0 ) {
                        if( climb_cost > 0 && p_special & PF_CLIMBABLE ) {
                            // Climbing fences
                            newg += climb_cost;
                        } else if( doors && ( terrain.open || furniture.open ) &&
                                   ( ( !terrain.has_flag( ter_furn_flag::TFLAG_OPENCLOSE_INSIDE ) &&
                                       !furniture.has_flag( ter_furn_flag::TFLAG_OPENCLOSE_INSIDE ) ) ||
                                     !is_outside( cur ) ) ) {
                            // Only try to open INSIDE doors from the inside
                            // To open and then move onto the tile
                            newg += 4;
                        } else if( terrain_rating > 1 ) {
                            // Expected number of turns to bash the terrain/furniture down.
                            newg += ( 20 / terrain_rating ) + 2 + 10;
                        } else if( terrain_rating == 1 ) {
                            // Desperate measures, avoid whenever possible.
                            newg += 500;
                        } else {
                            layer.set_state( index, pf.generation, ASL_CLOSED );
                            continue;
                        }
                    }

                    // Then account for a real vehicle obstacle, if there is one.  A non-obstacle
                    // vehicle part does not replace the underlying terrain/furniture decision above.
                    if( veh != nullptr ) {
                        const vpart_position vp( const_cast<vehicle &>( *veh ), part );
                        if( vp.cargo_blocks_passage() ) {
                            // Cargo blocking a walkway is not a structural vehicle part.
                            // Route around it instead of treating the cargo rack as bashable.
                            layer.set_state( index, pf.generation, ASL_CLOSED );
                            continue;
                        }
                        const auto vpobst = vp.obstacle_at_part();
                        part = vpobst ? vpobst->part_index() : -1;
                        if( part >= 0 ) {
                            int dummy = -1;
                            if( doors && veh->part_flag( part, VPFLAG_OPENABLE ) &&
                                ( !veh->part_flag( part, "OPENCLOSE_INSIDE" ) ||
                                  veh_at_internal( cur, dummy ) == veh ) ) {
                                // Handle car doors, but don't try to path through curtains
                                newg += 10; // One turn to open, 4 to move there
                            } else if( bash > 0 ) {
                                // Car obstacle that isn't a door
                                // TODO: Account for armor
                                int hp = veh->part( part ).hp();
                                if( hp / 20 > bash ) {
                                    // Threshold damage thing means we just can't bash this down
                                    layer.set_state( index, pf.generation, ASL_CLOSED );
                                    continue;
                                } else if( hp / 10 > bash ) {
                                    // Threshold damage thing means we will fail to deal damage pretty often
                                    hp *= 2;
                                }

                                newg += 2 * hp / bash + 8 + 4;
                            } else {
                                if( !doors || !veh->part_flag( part, VPFLAG_OPENABLE ) ) {
                                    // Won't be openable, don't try from other sides
                                    layer.set_state( index, pf.generation, ASL_CLOSED );
                                }

                                continue;
                            }
                        }
                    }
                }

                if( trapavoid && ( p_special & PF_TRAP ) ) {
                    const trap &ter_trp = terrain.trap.obj();
                    const trap &trp = ter_trp.is_benign() ? tile.get_trap_t() : ter_trp;
                    if( !trp.is_benign() ) {
                        // For now make them detect all traps
                        if( terrain.has_flag( ter_furn_flag::TFLAG_NO_FLOOR ) ) {
                            // Special case - ledge in z-levels
                            // Warning: really expensive, needs a cache
                            if( valid_move( p, tripoint( p.xy(), p.z - 1 ), false, true ) ) {
                                tripoint below( p.xy(), p.z - 1 );
                                if( !has_flag( ter_furn_flag::TFLAG_NO_FLOOR, below ) ) {
                                    // Otherwise this would have been a huge fall
                                    path_data_layer &layer = pf.get_layer( p.z - 1 );
                                    // From cur, not p, because we won't be walking on air
                                    pf.add_point( layer.gscore[parent_index] + 10,
                                                  layer.score[parent_index] + 10 + 2 * rl_dist( below, t ),
                                                  cur, below );
                                }

                                // Close p, because we won't be walking on it
                                layer.set_state( index, pf.generation, ASL_CLOSED );
                                continue;
                            }
                        } else {
                            // Otherwise it's walkable
                            newg += 500;
                        }
                    }
                }

                if( sharpavoid && p_special & PF_SHARP ) {
                    layer.set_state( index, pf.generation, ASL_CLOSED ); // Avoid sharp things
                }

            }

            // If not visited, add as open
            // If visited, add it only if we can do so with better score
            if( layer.get_state( index, pf.generation ) == ASL_NONE || newg < layer.gscore[index] ) {
                pf.add_point( newg, newg + 2 * rl_dist( p, t ), cur, p );
            }
        }

        if( !settings.allow_climb_stairs ) {
            // The part below is only for z-level pathing
            continue;
        }

        // 载具绳梯（飞艇等）的 z-level 转换
        // 载具绳梯存储在 cached_veh_rope 中，不设置 PF_UPDOWN 标志，
        // 因此需要独立于地形楼梯逻辑处理，放在 continue 之前避免被跳过。
        // 绳梯的中间层通常是空气（t_open_air），NPC 踩上去会坠落，
        // 所以路径必须直接连接绳梯两端可站立的位置，而不是逐层经过空气。
        // 向下：cur 在绳梯上，找到第一个可着陆点（非空气地形或有 BOARDABLE 载具）
        if( cur.z > min.z ) {
            tripoint_bub_ms cur_bub( cur );
            for( const auto &entry : cached_veh_rope ) {
                const tripoint_bub_ms &rope_top = entry.first;
                if( rope_top.xy() != cur_bub.xy() ) {
                    continue;
                }
                if( rope_top.z() < cur_bub.z() ) {
                    continue;  // 绳梯顶部在 cur 下方，不覆盖 cur
                }
                const vehicle *veh = entry.second.first;
                const int part = entry.second.second;
                const int ladder_len = veh->part( part ).info().ladder_length();
                if( rope_top.z() - cur_bub.z() + 1 > ladder_len ) {
                    continue;  // 绳梯长度不足，不覆盖 cur
                }
                // 绳梯的垂落线被中间层的载具部件挡住时，够不到 cur
                bool line_blocked = false;
                for( int z = cur.z + 1; z < rope_top.z(); ++z ) {
                    if( veh_at( tripoint( cur.xy(), z ) ) ) {
                        line_blocked = true;
                        break;
                    }
                }
                if( line_blocked ) {
                    continue;
                }
                // 从 cur 正下方开始逐层找可着陆点
                for( int i = 1; i <= ladder_len - 1; i++ ) {
                    tripoint dest( cur.xy(), cur.z - i );
                    if( dest.z < min.z ) {
                        break;
                    }
                    // 该层同格有载具部件：绳梯垂到这里为止，站得上去就连过去，否则没有边
                    if( veh_at( dest ) ) {
                        if( passable( dest ) ) {
                            pf.add_point( layer.gscore[parent_index] + 2 * i,
                                          layer.score[parent_index] + 2 * i + 2 * rl_dist( dest, t ),
                                          cur, dest );
                        }
                        break;
                    }
                    if( !has_flag( ter_furn_flag::TFLAG_NO_FLOOR, dest ) ) {
                        // 非空气地形：可通过才可着陆
                        if( !impassable_ter_furn( dest ) ) {
                            pf.add_point( layer.gscore[parent_index] + 2 * i,
                                          layer.score[parent_index] + 2 * i + 2 * rl_dist( dest, t ),
                                          cur, dest );
                        }
                        break;
                    }
                }
                break;
            }
        }
        // 向上：cur 在绳梯下方，直接连到绳梯顶部（可站立的载具甲板）
        if( cur.z < max.z ) {
            auto [found, rope_pos] = has_rope_at( tripoint_bub_ms( cur ), true );
            if( found && rope_pos.z() > cur.z ) {
                const auto rope_pair = get_rope_at( rope_pos );
                const int ladder_len = rope_pair.first->part( rope_pair.second ).info().ladder_length();
                // 最大上升层数为 ladder_len - 1
                const int dist = std::min( rope_pos.z() - cur.z, ladder_len - 1 );
                if( dist > 0 ) {
                    tripoint dest( cur.xy(), cur.z + dist );
                    if( inbounds( dest ) ) {
                        pf.add_point( layer.gscore[parent_index] + 2 * dist,
                                      layer.score[parent_index] + 2 * dist + 2 * rl_dist( dest, t ),
                                      cur, dest );
                    }
                }
            }
        }

        if( !( cur_special & PF_UPDOWN ) ) {
            // 没有地形楼梯/坡道标志，跳过下方地形 z-level 转换逻辑
            continue;
        }

        bool rope_ladder = false;
        const const_maptile &parent_tile = maptile_at_internal( cur );
        const ter_t &parent_terrain = parent_tile.get_ter_t();
        if( settings.allow_climb_stairs && cur.z > min.z &&
            parent_terrain.has_flag( ter_furn_flag::TFLAG_GOES_DOWN ) ) {
            std::optional<tripoint> opt_dest = g->find_or_make_stairs( get_map(),
                                                cur.z - 1, rope_ladder, false, cur );
            if( !opt_dest ) {
                continue;
            }
            tripoint dest = opt_dest.value();
            if( vertical_move_destination( *this, ter_furn_flag::TFLAG_GOES_UP, dest ) ) {
                if( !inbounds( dest ) ) {
                    continue;
                }
                path_data_layer &layer = pf.get_layer( dest.z );
                pf.add_point( layer.gscore[parent_index] + 2,
                              layer.score[parent_index] + 2 * rl_dist( dest, t ),
                              cur, dest );
            }
        }
        if( settings.allow_climb_stairs && cur.z < max.z &&
            parent_terrain.has_flag( ter_furn_flag::TFLAG_GOES_UP ) ) {
            std::optional<tripoint> opt_dest = g->find_or_make_stairs( get_map(),
                                                cur.z + 1, rope_ladder, false, cur );
            if( !opt_dest ) {
                continue;
            }
            tripoint dest = opt_dest.value();
            if( vertical_move_destination( *this, ter_furn_flag::TFLAG_GOES_DOWN, dest ) ) {
                if( !inbounds( dest ) ) {
                    continue;
                }
                path_data_layer &layer = pf.get_layer( dest.z );
                pf.add_point( layer.gscore[parent_index] + 2,
                              layer.score[parent_index] + 2 * rl_dist( dest, t ),
                              cur, dest );
            }
        }
        if( cur.z < max.z && parent_terrain.has_flag( ter_furn_flag::TFLAG_RAMP ) &&
            valid_move( cur, tripoint( cur.xy(), cur.z + 1 ), false, true ) ) {
            path_data_layer &layer = pf.get_layer( cur.z + 1 );
            for( size_t it = 0; it < 8; it++ ) {
                const tripoint above( cur.x + x_offset[it], cur.y + y_offset[it], cur.z + 1 );
                if( !inbounds( above ) ) {
                    continue;
                }
                pf.add_point( layer.gscore[parent_index] + 4,
                              layer.score[parent_index] + 4 + 2 * rl_dist( above, t ),
                              cur, above );
            }
        }
        if( cur.z < max.z && parent_terrain.has_flag( ter_furn_flag::TFLAG_RAMP_UP ) &&
            valid_move( cur, tripoint( cur.xy(), cur.z + 1 ), false, true, true ) ) {
            path_data_layer &layer = pf.get_layer( cur.z + 1 );
            for( size_t it = 0; it < 8; it++ ) {
                const tripoint above( cur.x + x_offset[it], cur.y + y_offset[it], cur.z + 1 );
                if( !inbounds( above ) ) {
                    continue;
                }
                pf.add_point( layer.gscore[parent_index] + 4,
                              layer.score[parent_index] + 4 + 2 * rl_dist( above, t ),
                              cur, above );
            }
        }
        if( cur.z > min.z && parent_terrain.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN ) &&
            valid_move( cur, tripoint( cur.xy(), cur.z - 1 ), false, true, true ) ) {
            path_data_layer &layer = pf.get_layer( cur.z - 1 );
            for( size_t it = 0; it < 8; it++ ) {
                const tripoint below( cur.x + x_offset[it], cur.y + y_offset[it], cur.z - 1 );
                if( !inbounds( below ) ) {
                    continue;
                }
                pf.add_point( layer.gscore[parent_index] + 4,
                              layer.score[parent_index] + 4 + 2 * rl_dist( below, t ),
                              cur, below );
            }
        }

    } while( !done && !pf.empty() );

    if( done ) {
        ret.reserve( rl_dist( f, t ) * 2 );
        tripoint cur = t;
        // 检查两个同 xy 坐标点之间是否由载具绳梯连接
        auto connected_by_vehicle_rope = [&]( const tripoint & from, const tripoint & to ) -> bool {
            if( from.xy() != to.xy() ) {
                return false;
            }
            const int low_z = std::min( from.z, to.z );
            const int high_z = std::max( from.z, to.z );
            for( const auto &entry : cached_veh_rope ) {
                const tripoint_bub_ms &rope_top = entry.first;
                if( rope_top.xy().raw() != from.xy() ) {
                    continue;
                }
                const vehicle *veh = entry.second.first;
                const int part = entry.second.second;
                const int ladder_len = veh->part( part ).info().ladder_length();
                // 绳梯顶部必须在高点或更高，且长度覆盖从 low_z 到 rope_top.z() 的区间
                if( rope_top.z() >= high_z && rope_top.z() - low_z + 1 <= ladder_len ) {
                    return true;
                }
            }
            return false;
        };

        // Just to limit max distance, in case something weird happens
        for( int fdist = max_length; fdist != 0; fdist-- ) {
            const int cur_index = flat_index( cur.xy() );
            const path_data_layer &layer = pf.get_layer( cur.z );
            const tripoint &par = layer.parent[cur_index];
            if( cur == f ) {
                break;
            }

            ret.push_back( cur );
            // 楼梯等跨层连接可能在上下层使用不同的水平坐标，因此单层跨层
            // 路径即使水平距离较大也可能是有效的。载具绳梯则可能跨越多层，
            // 只有在缓存确认绳梯覆盖整个高度区间时才允许这种跳跃。
            const bool single_z_level_transition = std::abs( cur.z - par.z ) == 1;
            if( rl_dist( cur, par ) > 1 && !single_z_level_transition &&
                !connected_by_vehicle_rope( cur, par ) ) {
                debugmsg( "Jump in our route!  %d:%d:%d->%d:%d:%d",
                          cur.x, cur.y, cur.z, par.x, par.y, par.z );
                return ret;
            }

            cur = par;
        }

        std::reverse( ret.begin(), ret.end() );
    }

    return ret;
}

std::vector<tripoint_bub_ms> map::route( const tripoint_bub_ms &f, const tripoint_bub_ms &t,
        const pathfinding_settings &settings,
        const std::set<tripoint> &pre_closed ) const
{
    std::vector<tripoint> raw_result = route( f.raw(), t.raw(), settings, pre_closed );
    std::vector<tripoint_bub_ms> result;
    std::transform( raw_result.begin(), raw_result.end(), std::back_inserter( result ),
    []( const tripoint & p ) {
        return tripoint_bub_ms( p );
    } );
    return result;
}
