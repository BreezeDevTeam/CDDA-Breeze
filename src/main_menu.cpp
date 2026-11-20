#include "main_menu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <istream>
#include <memory>
#include <string>

#include "auto_pickup.h"
#include "avatar.h"
#include "cata_scope_helpers.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "character_id.h"
#include "color.h"
#include "debug.h"
#include "enums.h"
#include "filesystem.h"
#include "game.h"
#include "gamemode.h"
#include "get_version.h"
#include "help.h"
#include "loading_ui.h"
#include "localized_comparator.h"
#include "mapbuffer.h"
#include "mapsharing.h"
#include "messages.h"
#include "music.h"
#include <optional>
#include "options.h"
#include "output.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "popup.h"
#include "safemode_ui.h"
#include "scenario.h"
#include "sdlsound.h"
#include "sounds.h"
#include "string_formatter.h"
#include "text_snippets.h"
#include "translations.h"
#include "ui_manager.h"
#include "wcwidth.h"
#include "worldfactory.h"

enum class main_menu_opts : int {
    NEWCHAR = 0,
    LOADCHAR = 1,
    WORLD = 2,
    SPECIAL = 3,
    SETTINGS = 4,
    HELP = 5,
    CREDITS = 6,
    QUIT = 7
};

static constexpr int max_menu_opts = 7;

std::string main_menu::queued_world_to_load;
std::string main_menu::queued_save_id_to_load;

static int getopt( main_menu_opts o )
{
    return static_cast<int>( o );
}

void main_menu::on_move() const
{
    sfx::play_variant_sound( "menu_move", "default", 100 );
}

void main_menu::on_error()
{
    sfx::play_variant_sound( "menu_error", "default", 100 );
}

namespace
{

constexpr int bc_frame_ms = 33;
constexpr int bc_max_box_subs = 5;

std::chrono::steady_clock::time_point bc_epoch = std::chrono::steady_clock::now();

double bc_now()
{
    return std::chrono::duration<double>( std::chrono::steady_clock::now() - bc_epoch ).count();
}

bool bc_on_parent = true;
int bc_sub_scroll = 0;
int bc_visible_subs = bc_max_box_subs;
int bc_box_w = 40;
int bc_box_h = 15;
double bc_move_at = -100.0;

struct bc_sub_entry {
    std::string label;
    std::string key;
    nc_color color;
};

std::vector<bc_sub_entry> bc_subs;

void bc_put( const catacurses::window &w, const point &p, const std::string &glyph, nc_color col )
{
    if( p.x < 0 || p.y < 0 || p.x >= getmaxx( w ) || p.y >= getmaxy( w ) ) {
        return;
    }
    mvwputch( w, p, col, glyph );
}

void bc_text( const catacurses::window &w, const point &p, const std::string &text, nc_color col )
{
    if( text.empty() || p.x < 0 || p.y < 0 || p.x >= getmaxx( w ) || p.y >= getmaxy( w ) ) {
        return;
    }
    mvwprintz( w, p, col, text );
}

int bc_text_end( const catacurses::window &w, const point &p, const std::string &text,
                 nc_color col )
{
    bc_text( w, p, text, col );
    return p.x + utf8_width( text );
}

void bc_text_band( const catacurses::window &w, const point &p, const std::string &text,
                   nc_color col, nc_color band_col, int band_lo, int band_hi )
{
    std::string runs[3];
    int x = p.x;
    const char *ptr = text.c_str();
    int len = static_cast<int>( text.size() );
    while( len > 0 ) {
        const char *start = ptr;
        const uint32_t ch = UTF8_getch( &ptr, &len );
        if( ch == UNKNOWN_UNICODE ) {
            continue;
        }
        const int cw = mk_wcwidth( ch );
        if( cw < 0 ) {
            continue;
        }
        const int bucket = x + cw <= band_lo ? 0 : ( x >= band_hi ? 2 : 1 );
        runs[bucket].append( start, ptr - start );
        x += cw;
    }
    int cx = p.x;
    for( int i = 0; i < 3; i++ ) {
        if( runs[i].empty() ) {
            continue;
        }
        bc_text( w, point( cx, p.y ), runs[i], i == 1 ? band_col : col );
        cx += utf8_width( runs[i] );
    }
}

std::string bc_upper( const std::string &s )
{
    std::string res = s;
    for( char &c : res ) {
        if( c >= 'a' && c <= 'z' ) {
            c = static_cast<char>( c - 'a' + 'A' );
        }
    }
    return res;
}

std::string bc_hotkey( const std::vector< std::vector<std::string> > &hotkeys, size_t idx )
{
    if( idx >= hotkeys.size() || hotkeys[idx].empty() ) {
        return "";
    }
    return bc_upper( hotkeys[idx].front() );
}

void bc_draw_wind( const catacurses::window &w, double t )
{
    const int width = getmaxx( w );
    const int height = getmaxy( w );
    const int count = std::max( 12, width * height / 95 );
    for( int i = 0; i < count; i++ ) {
        const uint32_t h = ( static_cast<uint32_t>( i ) * 2654435761u + 12345u ) & 0xffffffu;
        const int layer = static_cast<int>( ( h >> 11 ) % 3 );
        const double speed = layer == 0 ? 3.0 : ( layer == 1 ? 6.5 : 11.0 );
        const int len = 2 + static_cast<int>( ( h >> 8 ) % 5 );
        const int base_y = static_cast<int>( ( h >> 3 ) % static_cast<uint32_t>( height ) );
        const double phase = static_cast<double>( ( h >> 5 ) % 1000 ) / 1000.0 * 6.2831853;
        const double x0 = std::fmod( static_cast<double>( ( h >> 13 ) % static_cast<uint32_t>( width ) )
                                     + t * speed, width + len ) - len;
        const double amp = layer == 0 ? 0.5 : ( layer == 1 ? 1.1 : 1.8 );
        const int y = base_y + static_cast<int>( std::lround(
                          amp * std::sin( t * 0.5 + phase + x0 * 0.05 ) ) );
        if( y < 0 || y >= height ) {
            continue;
        }
        const nc_color col = layer == 0 ? c_dark_gray : ( layer == 1 ? c_cyan : c_light_cyan );
        for( int k = 0; k < len; k++ ) {
            const int x = static_cast<int>( x0 ) + k;
            if( x < 0 || x >= width ) {
                continue;
            }
            bc_put( w, point( x, y ), k + 1 < len ? "─" : "·", col );
        }
    }
}

const std::vector<std::string> &bc_logo_art( bool big )
{
    static const std::vector<std::string> big_art = {
        "██████╗ ██████╗ ███████╗███████╗███████╗███████╗",
        "██╔══██╗██╔══██╗██╔════╝██╔════╝╚══███╔╝██╔════╝",
        "██████╔╝██████╔╝█████╗  █████╗    ███╔╝ █████╗  ",
        "██╔══██╗██╔══██╗██╔══╝  ██╔══╝   ███╔╝  ██╔══╝  ",
        "██████╔╝██║  ██║███████╗███████╗███████╗███████╗",
        "╚═════╝ ╚═╝  ╚═╝╚══════╝╚══════╝╚══════╝╚══════╝"
    };
    static const std::vector<std::string> small_art = {
        "█▀▀▀▄ █▀▀▀▄ █▀▀▀▀ █▀▀▀▀ ▀▀▀▀█ █▀▀▀▀",
        "█▄▄▄▀ █▄▄▄▀ █▄▄▄  █▄▄▄    ▄▀  █▄▄▄ ",
        "█   █ █ █   █     █     ▄▀    █    ",
        "█▄▄▄▀ █  ▀▄ █▄▄▄▄ █▄▄▄▄ █▄▄▄▄ █▄▄▄▄"
    };
    return big ? big_art : small_art;
}

nc_color bc_gradient( double v, double glow )
{
    static const nc_color ramp[] = { c_white, c_light_cyan, c_light_cyan, c_cyan, c_cyan, c_light_blue };
    if( glow > 0.70 ) {
        return c_white;
    }
    if( glow > 0.40 ) {
        return c_light_cyan;
    }
    const int idx = clamp( static_cast<int>( v * 6.0 ), 0, 5 );
    return ramp[idx];
}

void bc_draw_logo( const catacurses::window &w, const point &origin,
                   const std::vector<std::string> &art, double t )
{
    const int rows = static_cast<int>( art.size() );
    int cols = 0;
    for( const std::string &line : art ) {
        cols = std::max( cols, utf8_width( line ) );
    }
    const double sweep = std::fmod( t * 0.20, 1.45 ) - 0.25;
    const double sigma = std::max( 3.0, cols * 0.14 );
    for( int j = 0; j < rows; j++ ) {
        int x = 0;
        const char *ptr = art[j].c_str();
        int len = static_cast<int>( art[j].size() );
        while( len > 0 ) {
            const uint32_t ch = UTF8_getch( &ptr, &len );
            if( ch == UNKNOWN_UNICODE ) {
                continue;
            }
            const int cw = mk_wcwidth( ch );
            if( cw <= 0 ) {
                continue;
            }
            if( ch != ' ' ) {
                const double glow = std::exp( -( ( x - sweep * cols ) * ( x - sweep * cols ) )
                                              / ( 2.0 * sigma * sigma ) );
                const double v = 0.62 * ( rows > 1 ? static_cast<double>( j ) / ( rows - 1 ) : 0.0 )
                                 + 0.38 * ( cols > 1 ? static_cast<double>( x ) / ( cols - 1 ) : 0.0 );
                bc_put( w, origin + point( x, j ), utf32_to_utf8( ch ), bc_gradient( v, glow ) );
            }
            x += cw;
        }
    }
}

void bc_draw_ribbon( const catacurses::window &w, const point &origin, int width, double t )
{
    for( int i = 0; i < width; i++ ) {
        const double ph = i * 0.42 - t * 2.6;
        const double v = 0.5 + 0.5 * std::sin( ph ) * std::sin( ph * 0.33 + t * 0.9 );
        const int level = clamp( static_cast<int>( v * 8.0 ), 0, 7 );
        bc_put( w, origin + point( i, 0 ), utf32_to_utf8( 0x2581 + level ),
                ( i / 3 ) % 2 == 0 ? c_cyan : c_light_cyan );
    }
}

void bc_frame( const catacurses::window &w, const point &o, int box_w, int box_h,
               const std::string &title, const std::string &right_title )
{
    for( int y = 1; y < box_h - 1; y++ ) {
        for( int x = 1; x < box_w - 1; x++ ) {
            bc_put( w, o + point( x, y ), " ", c_black );
        }
    }
    for( int x = 1; x < box_w - 1; x++ ) {
        bc_put( w, o + point( x, 0 ), "─", c_dark_gray );
        bc_put( w, o + point( x, box_h - 1 ), "─", c_dark_gray );
    }
    for( int y = 1; y < box_h - 1; y++ ) {
        bc_put( w, o + point( 0, y ), "│", c_dark_gray );
        bc_put( w, o + point( box_w - 1, y ), "│", c_dark_gray );
    }
    bc_put( w, o, "╭", c_dark_gray );
    bc_put( w, o + point( box_w - 1, 0 ), "╮", c_dark_gray );
    bc_put( w, o + point( 0, box_h - 1 ), "╰", c_dark_gray );
    bc_put( w, o + point( box_w - 1, box_h - 1 ), "╯", c_dark_gray );

    if( !title.empty() ) {
        const int tw = utf8_width( title );
        for( int i = 0; i < tw + 2; i++ ) {
            bc_put( w, o + point( 2 + i, 0 ), " ", c_black );
        }
        bc_text( w, o + point( 3, 0 ), title, c_light_cyan );
    }
    if( !right_title.empty() ) {
        const int rw = utf8_width( right_title );
        const int rx = box_w - 3 - rw;
        if( rx > 2 + ( title.empty() ? 0 : utf8_width( title ) + 2 ) ) {
            for( int i = 0; i < rw + 2; i++ ) {
                bc_put( w, o + point( rx - 1 + i, 0 ), " ", c_black );
            }
            bc_text( w, o + point( rx, 0 ), right_title, c_dark_gray );
        }
    }
}

struct bc_move_band {
    int lo;
    int hi;
};

bc_move_band bc_band_for( double since, int box_w )
{
    if( since < 0.0 || since >= 0.30 ) {
        return { -1000, -1000 };
    }
    const int head = -5 + static_cast<int>( since / 0.30 * ( box_w + 10 ) );
    return { head - 5, head };
}

struct bc_stats {
    int worlds = 0;
    int saves = 0;
};

bc_stats bc_world_stats()
{
    static bc_stats cached;
    static double cached_at = -1000.0;
    const double now = bc_now();
    if( now - cached_at < 1.0 ) {
        return cached;
    }
    cached_at = now;
    cached = bc_stats();
    const std::vector<std::string> names = world_generator->all_worldnames();
    cached.worlds = static_cast<int>( names.size() );
    for( const std::string &name : names ) {
        if( const WORLD *wrld = world_generator->get_world( name ) ) {
            cached.saves += static_cast<int>( wrld->world_saves.size() );
        }
    }
    return cached;
}

std::string bc_clock_text()
{
    std::time_t stamp = std::time( nullptr );
    std::tm local_time = std::tm();
#if defined(_WIN32)
    if( localtime_s( &local_time, &stamp ) != 0 ) {
        return "";
    }
#else
    if( localtime_r( &stamp, &local_time ) == nullptr ) {
        return "";
    }
#endif
    return string_format( "%02d:%02d", local_time.tm_hour, local_time.tm_min );
}

} // namespace

void main_menu::display_sub_menu( int sel, const point &bottom_left, int sel_line )
{
    (void)bottom_left;
    (void)sel_line;
    bc_subs.clear();
    main_menu_sub_button_map.clear();

    switch( static_cast<main_menu_opts>( sel ) ) {
        case main_menu_opts::SPECIAL:
            for( int i = 1; i < static_cast<int>( special_game_type::NUM_SPECIAL_GAME_TYPES ); i++ ) {
                bc_subs.push_back( { special_game_name( static_cast<special_game_type>( i ) ), "",
                                     c_light_gray } );
            }
            break;
        case main_menu_opts::SETTINGS:
            for( size_t i = 0; i < vSettingsSubItems.size(); ++i ) {
                bc_subs.push_back( { remove_color_tags( shortcut_text( c_white, vSettingsSubItems[i] ) ),
                                     bc_hotkey( vSettingsHotkeys, i ), c_light_gray } );
            }
            break;
        case main_menu_opts::NEWCHAR:
            for( size_t i = 0; i < vNewGameSubItems.size(); ++i ) {
                bc_subs.push_back( { remove_color_tags( shortcut_text( c_white, vNewGameSubItems[i] ) ),
                                     bc_hotkey( vNewGameHotkeys, i ), c_light_gray } );
            }
            break;
        case main_menu_opts::LOADCHAR:
        case main_menu_opts::WORLD: {
            if( sel == getopt( main_menu_opts::WORLD ) ) {
                bc_subs.push_back( { _( "Create World" ), "", c_light_gray } );
            }
            const std::vector<std::string> all_worldnames = world_generator->all_worldnames();
            for( const std::string &world_name : all_worldnames ) {
                const WORLD *wrld = world_generator->get_world( world_name );
                const int saves = wrld ? static_cast<int>( wrld->world_saves.size() ) : 0;
                const bool tutorial = world_name == "TUTORIAL" || world_name == "DEFENSE";
                std::string shown_name = world_name;
                if( utf8_width( shown_name ) > 22 ) {
                    shown_name = utf8_truncate( shown_name, 21 ) + "…";
                }
                bc_subs.push_back( { string_format( "%s (%d)", shown_name, saves ), "",
                                     tutorial ? c_light_cyan : c_light_gray } );
            }
            break;
        }
        case main_menu_opts::CREDITS:
        case main_menu_opts::HELP:
        case main_menu_opts::QUIT:
        default:
            break;
    }

    if( sel2 >= static_cast<int>( bc_subs.size() ) ) {
        sel2 = std::max( 0, static_cast<int>( bc_subs.size() ) - 1 );
    }
}

void main_menu::print_menu( const catacurses::window &w_open, int iSel, const point &offset,
                            int sel_line )
{
    (void)offset;
    (void)sel_line;
    main_menu_button_map.clear();
    main_menu_sub_button_map.clear();

    werase( w_open );

    const int width = getmaxx( w_open );
    const int height = getmaxy( w_open );
    const double t = bc_now();

    bc_draw_wind( w_open, t );

    const int n_items = static_cast<int>( vMenuItems.size() );
    display_sub_menu( iSel, point_zero, 0 );
    const int n_subs = static_cast<int>( bc_subs.size() );
    const bc_stats stats = bc_world_stats();

    const bool show_logo = height >= 22;
    const bool show_big_logo = height >= 30;
    const bool show_ribbon = height >= 26;
    const std::vector<std::string> &logo = bc_logo_art( show_big_logo );
    const int logo_h = show_logo ? static_cast<int>( logo.size() ) : 0;
    const int ribbon_h = show_ribbon ? 1 : 0;
    const int hero_h = logo_h > 0 ? logo_h + ribbon_h + 1 : 0;

    bc_box_w = std::min( 40, std::max( 24, width - 6 ) );
    bc_box_w = std::min( bc_box_w, width - 2 );
    bc_box_h = std::min( n_items + bc_max_box_subs + 2,
                         std::max( n_items + 2, height - 3 - hero_h ) );
    bc_visible_subs = std::max( 1, bc_box_h - n_items - 2 );

    const int content_h = hero_h + bc_box_h + 1;
    const int top = std::max( 1, 1 + ( height - 2 - content_h ) / 2 );

    const std::string stats_txt = string_format( "● %d 世界 · %d 存档", stats.worlds,
                                  stats.saves );
    std::string bars;
    for( int i = 0; i < 4; i++ ) {
        const double ph = i * 1.7 - t * 2.6;
        const double v = 0.5 + 0.5 * std::sin( ph ) * std::sin( ph * 0.33 + t * 0.9 );
        bars += utf32_to_utf8( 0x2581 + clamp( static_cast<int>( v * 8.0 ), 0, 7 ) );
    }
    const std::string clock_txt = bc_clock_text();
    const std::string tail = ( clock_txt.empty() ? "" : clock_txt + "  ▏  " ) + bars + " ";
    const int tail_w = utf8_width( tail );
    const bool show_tail = width - tail_w - 2 > 40;

    std::string world_path = PATH_INFO::savedir();
    while( !world_path.empty() && world_path.back() == '/' ) {
        world_path.pop_back();
    }
    if( world_path.empty() ) {
        world_path = "save";
    }
    if( !world_generator->last_world_name.empty() ) {
        world_path += "/" + world_generator->last_world_name;
    }
    const int path_budget = std::max( 6, width - 14 - utf8_width( stats_txt )
                                      - ( show_tail ? tail_w + 2 : 0 ) );
    bool path_trimmed = false;
    while( utf8_width( world_path ) > path_budget ) {
        const size_t slash = world_path.find( '/' );
        if( slash == std::string::npos ) {
            break;
        }
        world_path.erase( 0, slash + 1 );
        path_trimmed = true;
    }
    if( path_trimmed ) {
        world_path = "…/" + world_path;
    }

    for( int x = 0; x < width; x++ ) {
        bc_put( w_open, point( x, 0 ), " ", c_black );
    }
    bc_put( w_open, point( 0, 0 ), "▌", c_light_cyan );
    int head_x = bc_text_end( w_open, point( 1, 0 ), " BREEZE ", c_white );
    bc_put( w_open, point( head_x, 0 ), "▏", c_dark_gray );
    head_x = bc_text_end( w_open, point( head_x + 2, 0 ), world_path, c_light_gray );
    bc_put( w_open, point( head_x + 1, 0 ), "▏", c_dark_gray );
    head_x = bc_text_end( w_open, point( head_x + 3, 0 ), stats_txt, c_cyan );
    if( show_tail && head_x + 2 < width - tail_w ) {
        bc_text( w_open, point( width - tail_w - 1, 0 ), tail, c_light_cyan );
    }

    if( show_logo ) {
        int logo_w = 0;
        for( const std::string &line : logo ) {
            logo_w = std::max( logo_w, utf8_width( line ) );
        }
        const point logo_at( std::max( 0, ( width - logo_w ) / 2 ), top );
        for( int j = 0; j < logo_h; j++ ) {
            for( int x = 0; x < logo_w; x++ ) {
                bc_put( w_open, logo_at + point( x, j ), " ", c_black );
            }
        }
        bc_draw_logo( w_open, logo_at, logo, t );
        if( show_ribbon ) {
            bc_draw_ribbon( w_open, logo_at + point( 0, logo_h ), logo_w, t );
        }
    }

    const point box_at( ( width - bc_box_w ) / 2, top + hero_h );
    const std::string right_title = string_format(
                                        "%d/%d  %s", iSel + 1, n_items,
                                        remove_color_tags( shortcut_text( c_white, vMenuItems[iSel] ) ) );
    bc_frame( w_open, box_at, bc_box_w, bc_box_h, "菜单", right_title );

    const auto item_has_subs = [&]( int idx ) {
        switch( static_cast<main_menu_opts>( idx ) ) {
            case main_menu_opts::CREDITS:
            case main_menu_opts::HELP:
            case main_menu_opts::QUIT:
                return false;
            case main_menu_opts::LOADCHAR:
                return stats.worlds > 0;
            default:
                return true;
        }
    };

    const double since_move = t - bc_move_at;
    int row = 1;
    for( int i = 0; i < n_items && row < bc_box_h - 1; i++ ) {
        const bool active = i == iSel;
        const bool cursor = active && bc_on_parent;
        const bool has_sub = item_has_subs( i );
        const std::string key = bc_hotkey( vMenuHotkeys, i );
        const int key_w = utf8_width( key );
        const int key_x = bc_box_w - 2 - key_w;
        std::string label = remove_color_tags( shortcut_text( c_white, vMenuItems[i] ) );
        const int label_cap = std::max( 4, key_x - 3 - 2 );
        if( utf8_width( label ) > label_cap ) {
            label = utf8_truncate( label, label_cap );
        }

        const point line_at = box_at + point( 0, row );
        if( cursor ) {
            for( int x = 1; x < bc_box_w - 1; x++ ) {
                bc_put( w_open, line_at + point( x, 0 ), " ", c_black_cyan );
            }
            bc_put( w_open, line_at + point( 1, 0 ), "▍", c_white_cyan );
            const bc_move_band band = bc_band_for( since_move, bc_box_w );
            bc_text_band( w_open, line_at + point( 3, 0 ), label, c_black_cyan, c_white_cyan,
                          box_at.x + band.lo, box_at.x + band.hi );
            if( has_sub ) {
                bc_put( w_open, line_at + point( 3 + utf8_width( label ) + 1, 0 ), "▾",
                        c_black_cyan );
            }
            if( key_w > 0 ) {
                bc_text( w_open, line_at + point( key_x, 0 ), key, c_black_cyan );
            }
        } else {
            if( active ) {
                bc_put( w_open, line_at + point( 1, 0 ), "▍", c_cyan );
            }
            bc_text( w_open, line_at + point( 3, 0 ), label, active ? c_light_cyan : c_light_gray );
            if( has_sub ) {
                bc_put( w_open, line_at + point( 3 + utf8_width( label ) + 1, 0 ),
                        active && n_subs > 0 ? "▾" : "▸", active ? c_cyan : c_dark_gray );
            }
            if( key_w > 0 ) {
                bc_text( w_open, line_at + point( key_x, 0 ), key, active ? c_cyan : c_dark_gray );
            }
        }
        row++;

        if( !active || n_subs == 0 ) {
            continue;
        }
        const int max_scroll = std::max( 0, n_subs - bc_visible_subs );
        bc_sub_scroll = clamp( bc_sub_scroll, 0, max_scroll );
        if( sel2 < bc_sub_scroll ) {
            bc_sub_scroll = sel2;
        } else if( sel2 >= bc_sub_scroll + bc_visible_subs ) {
            bc_sub_scroll = sel2 - bc_visible_subs + 1;
        }
        for( int k = 0; k < bc_visible_subs && row < bc_box_h - 1; k++ ) {
            const int idx = bc_sub_scroll + k;
            if( idx >= n_subs ) {
                break;
            }
            const bc_sub_entry &sub = bc_subs[idx];
            const bool sub_cursor = !bc_on_parent && idx == sel2;
            const point sub_at = box_at + point( 0, row );
            const int sub_key_w = utf8_width( sub.key );
            const int sub_key_x = bc_box_w - 2 - sub_key_w;
            std::string sub_label = sub.label;
            const int sub_cap = std::max( 4, sub_key_x - 5 - 2 );
            if( utf8_width( sub_label ) > sub_cap ) {
                sub_label = utf8_truncate( sub_label, sub_cap );
            }

            bc_put( w_open, sub_at + point( 2, 0 ),
                    idx + 1 == n_subs ? "└" : "├", c_dark_gray );
            bc_put( w_open, sub_at + point( 3, 0 ), "─", c_dark_gray );

            if( sub_cursor ) {
                for( int x = 4; x < bc_box_w - 1; x++ ) {
                    bc_put( w_open, sub_at + point( x, 0 ), " ", c_black_cyan );
                }
                bc_put( w_open, sub_at + point( 4, 0 ), "▍", c_white_cyan );
                const bc_move_band sub_band = bc_band_for( since_move, bc_box_w );
                bc_text_band( w_open, sub_at + point( 5, 0 ), sub_label, c_black_cyan,
                              c_white_cyan, box_at.x + sub_band.lo, box_at.x + sub_band.hi );
                if( sub_key_w > 0 ) {
                    bc_text( w_open, sub_at + point( sub_key_x, 0 ), sub.key, c_black_cyan );
                }
            } else {
                const bool prearmed = bc_on_parent && idx == sel2;
                bc_text( w_open, sub_at + point( 5, 0 ), sub_label,
                         prearmed ? c_cyan : sub.color );
                if( sub_key_w > 0 ) {
                    bc_text( w_open, sub_at + point( sub_key_x, 0 ), sub.key, c_dark_gray );
                }
            }
            row++;
        }
    }

    const int hint_y = top + hero_h + bc_box_h;
    if( hint_y < height - 1 ) {
        const std::string hint = "↑↓ 移动    ←→ 切换    ↵ 确认";
        bc_text( w_open, point( std::max( 0, ( width - utf8_width( hint ) ) / 2 ), hint_y ),
                 hint, c_dark_gray );
    }

    if( width >= 96 && height >= 28 ) {
        const int tip_w = std::min( 40, box_at.x - 6 );
        if( tip_w >= 16 ) {
            std::string blurb = vdaytip;
            std::string blurb_title = "提示";
            if( iSel == getopt( main_menu_opts::NEWCHAR ) && sel2 >= 0 &&
                static_cast<size_t>( sel2 ) < vNewGameHints.size() ) {
                blurb = vNewGameHints[sel2];
                blurb_title = "说明";
            }
            const std::vector<std::string> lines = foldstring( blurb, tip_w );
            const int shown = std::min<int>( 3, lines.size() );
            const int tip_y = hint_y - shown;
            if( tip_y > 2 && box_at.x > 12 ) {
                bc_text( w_open, point( 2, tip_y - 1 ), blurb_title, c_cyan );
                for( int k = 0; k < shown; k++ ) {
                    bc_put( w_open, point( 2, tip_y + k ), "│", c_dark_gray );
                    bc_text( w_open, point( 4, tip_y + k ), lines[k], c_dark_gray );
                }
            }
        }
    }

    const int sy = height - 1;
    for( int x = 0; x < width; x++ ) {
        bc_put( w_open, point( x, sy ), " ", c_black );
    }
    bc_text( w_open, point( 1, sy ), " NORMAL ", c_black_cyan );
    int sx = 9;
    bc_put( w_open, point( sx, sy ), "▏", c_dark_gray );
    sx = bc_text_end( w_open, point( sx + 2, sy ),
                      string_format( " %s ", getVersionString() ), c_white );
    bc_put( w_open, point( sx + 1, sy ), "▏", c_dark_gray );
    sx = bc_text_end( w_open, point( sx + 2, sy ), " 主菜单 ", c_light_gray );

    const std::string right_info = string_format( "UTF-8  ▏  世界 %d  ▏  存档 %d ",
                                  stats.worlds, stats.saves );
    const int right_w = utf8_width( right_info );
    if( width - right_w - 1 > sx ) {
        bc_text( w_open, point( width - right_w - 1, sy ), right_info, c_cyan );
    }

    wnoutrefresh( w_open );
}

void main_menu::init_windows()
{
    if( LAST_TERM == point( TERMX, TERMY ) ) {
        return;
    }

    w_open = catacurses::newwin( TERMY, TERMX, point_zero );
    menu_offset = point_zero;
    extra_w = 0;

    LAST_TERM = point( TERMX, TERMY );
}

void main_menu::init_strings()
{
    // Credits
    mmenu_credits.clear();
    read_from_file_optional( PATH_INFO::credits(), [&]( std::istream & stream ) {
        std::string line;
        while( std::getline( stream, line ) ) {
            if( line[0] != '#' ) {
                mmenu_credits += ( line.empty() ? " " : line ) + "\n";
            }
        }
    } );

    if( mmenu_credits.empty() ) {
        mmenu_credits = _( "No credits information found." );
    }
    mmenu_credits_len = foldstring( mmenu_credits, FULL_SCREEN_WIDTH - 2 ).size();

    // fill menu with translated menu items
    vMenuItems.clear();
    vMenuItems.emplace_back( pgettext( "Main Menu", "<N|n>ew Game" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "Lo<a|A>d" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "<W|w>orld" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "<S|s>pecial" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "Se<t|T>tings" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "H<e|E|?>lp" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "<C|c>redits" ) );
    vMenuItems.emplace_back( pgettext( "Main Menu", "<Q|q>uit" ) );

    // new game menu items
    vNewGameSubItems.clear();
    vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game", "C<u|U>stom Character" ) );
    vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game", "<P|p>reset Character" ) );
    vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game", "<R|r>andom Character" ) );
    if( !MAP_SHARING::isSharing() ) { // "Play Now" function doesn't play well together with shared maps
        vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game",
                                       "Play Now!  (<D|d>efault Scenario)" ) );
        vNewGameSubItems.emplace_back( pgettext( "Main Menu|New Game", "Play N<o|O>w!" ) );
    }
    vNewGameHints.clear();
    vNewGameHints.emplace_back(
        _( "Allows you to fully customize points pool, scenario, and character's profession, stats, traits, skills and other parameters." ) );
    vNewGameHints.emplace_back( _( "Select from one of previously created character templates." ) );
    vNewGameHints.emplace_back(
        _( "Creates random character, but lets you preview the generated character and the scenario and change character and/or scenario if needed." ) );
    vNewGameHints.emplace_back(
        _( "Puts you right in the game, randomly choosing character's traits, profession, skills and other parameters.  Scenario is fixed to Evacuee." ) );
    vNewGameHints.emplace_back(
        _( "Puts you right in the game, randomly choosing scenario and character's traits, profession, skills and other parameters." ) );
    vNewGameHotkeys.clear();
    vNewGameHotkeys.reserve( vNewGameSubItems.size() );
    for( const std::string &item : vNewGameSubItems ) {
        vNewGameHotkeys.push_back( get_hotkeys( item ) );
    }

    // determine hotkeys from translated menu item text
    vMenuHotkeys.clear();
    for( const std::string &item : vMenuItems ) {
        vMenuHotkeys.push_back( get_hotkeys( item ) );
    }

    vWorldSubItems.clear();
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "显示世界模组" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "编辑世界模组"));
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "复制世界设置" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "角色转为模板" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "删除世界" ) );
    vWorldSubItems.emplace_back( pgettext( "Main Menu|World", "重置世界" ) );

    vWorldHotkeys.clear();
    for( const std::string &item : vWorldSubItems ) {
        vWorldHotkeys.push_back( get_hotkeys( item ) );
    }

    vSettingsSubItems.clear();
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "<O|o>ptions" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "Ke<y|Y>bindings" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "A<u|U>topickup" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "Sa<f|F>emode" ) );
    vSettingsSubItems.emplace_back( pgettext( "Main Menu|Settings", "Colo<r|R>s" ) );

    vSettingsHotkeys.clear();
    for( const std::string &item : vSettingsSubItems ) {
        vSettingsHotkeys.push_back( get_hotkeys( item ) );
    }

    loading_ui ui( false );
    try {
        g->load_core_data( ui );
    } catch( const std::exception &err ) {
        debugmsg( err.what() );
        std::exit( 1 );
    }
    vdaytip = SNIPPET.random_from_category( "tip" ).value_or( translation() ).translated();
}

void main_menu::display_text( const std::string &text, const std::string &title, int &selected )
{
    const int w_open_height = getmaxy( w_open );
    const int b_height = FULL_SCREEN_HEIGHT - clamp( ( FULL_SCREEN_HEIGHT - w_open_height ) + 4, 0, 4 );
    const int vert_off = clamp( ( w_open_height - FULL_SCREEN_HEIGHT ) / 2, getbegy( w_open ), TERMY );

    catacurses::window w_border = catacurses::newwin( b_height, FULL_SCREEN_WIDTH,
                                  point( clamp( ( TERMX - FULL_SCREEN_WIDTH ) / 2, 0, TERMX ), vert_off ) );

    catacurses::window w_text = catacurses::newwin( b_height - 2, FULL_SCREEN_WIDTH - 2,
                                point( 1 + clamp( ( TERMX - FULL_SCREEN_WIDTH ) / 2, 0, TERMX ), 1 + vert_off ) );

    draw_border( w_border, BORDER_COLOR, title );

    int width = FULL_SCREEN_WIDTH - 2;
    int height = b_height - 2;
    const auto vFolded = foldstring( text, width );
    int iLines = vFolded.size();
    selected = clamp( selected, 0, std::max( 0, iLines - height ) );

    fold_and_print_from( w_text, point_zero, width, selected, c_light_gray, text );

    draw_scrollbar( w_border, selected, height, iLines, point_south, BORDER_COLOR, true );
    wnoutrefresh( w_border );
    wnoutrefresh( w_text );
}

void main_menu::load_char_templates()
{
    templates.clear();

    for( std::string path : get_files_from_path( ".template", PATH_INFO::templatedir(), false,
            true ) ) {
        path.erase( path.find( ".template" ), std::string::npos );
        path.erase( 0, path.find_last_of( "\\/" ) + 1 );
        templates.push_back( path );
    }
    std::sort( templates.begin(), templates.end(), localized_compare );
}

bool main_menu::opening_screen()
{
    if( music::get_music_id() != music::music_id::title ) {
        music::deactivate_music_id_all();
    } else {
        play_music( music::get_music_id_string() );
    }

    world_generator->set_active_world( nullptr );
    world_generator->init();

    init_strings();

    load_char_templates();

    ctxt.register_cardinal();
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );

    ctxt.register_action( "SCROLL_UP" );
    ctxt.register_action( "SCROLL_DOWN" );

    // for the menu shortcuts
    ctxt.register_action( "ANY_INPUT" );
    bool start = false;
    bool load_game = false;

    avatar &player_character = get_avatar();
    player_character = avatar();

    int sel_line = 0;
    size_t last_world_pos = 0;
    bc_on_parent = true;
    bc_sub_scroll = 0;
    bc_move_at = -100.0;

    // Make [Load Game] the default cursor position if there's game save available
    if( !world_generator->all_worldnames().empty() ) {
        std::vector<std::string> worlds = world_generator->all_worldnames();
        last_world_pos = std::find( worlds.begin(), worlds.end(),
                                    world_generator->last_world_name ) - worlds.begin();
        if( last_world_pos >= worlds.size() ) {
            last_world_pos = 0;
        }
        sel1 = getopt( main_menu_opts::LOADCHAR );
        sel2 = last_world_pos;
    }

    background_pane background;

    ui_adaptor ui;
    ui.on_redraw( [&]( const ui_adaptor & ) {
        print_menu( w_open, sel1, menu_offset, sel_line );
    } );
    ui.on_screen_resize( [this]( ui_adaptor & ui ) {
        init_windows();
        ui.position_from_window( w_open );
    } );
    ui.mark_resize();

    if( !queued_world_to_load.empty() ) {
        save_t const &save_to_load = queued_save_id_to_load.empty() ? world_generator->get_world(
                                         queued_world_to_load )->world_saves.front() : save_t::from_save_id( queued_save_id_to_load );
        start = main_menu::load_game( queued_world_to_load, save_to_load );
        queued_world_to_load.clear();
        queued_save_id_to_load.clear();
        if( start ) {
            load_game = true;
        }
    }

    while( !start ) {
        ui_manager::redraw();
        const bool animate_menu = get_option<bool>( "ANIMATIONS" );
        std::string action = ctxt.handle_input( animate_menu ? bc_frame_ms : -1 );
        if( action == "TIMEOUT" ) {
            continue;
        }
        input_event sInput = ctxt.get_raw_input();
        const int sel1_at_input = sel1;

        // check automatic menu shortcuts
        for( int i = 0; static_cast<size_t>( i ) < vMenuHotkeys.size(); ++i ) {
            for( const std::string &hotkey : vMenuHotkeys[i] ) {
                if( sInput.text == hotkey && sel1 != i ) {
                    sel1 = i;
                    sel2 = i == getopt( main_menu_opts::LOADCHAR ) ? last_world_pos : 0;
                    sel_line = 0;
                    bc_on_parent = true;
                    bc_sub_scroll = 0;
                    bc_move_at = bc_now();
                    if( i == getopt( main_menu_opts::HELP ) ) {
                        action = "CONFIRM";
                    } else if( i == getopt( main_menu_opts::QUIT ) ) {
                        action = "QUIT";
                    }
                }
            }
        }
        if( sel1 == getopt( main_menu_opts::SETTINGS ) ) {
            for( int i = 0; static_cast<size_t>( i ) < vSettingsSubItems.size(); ++i ) {
                for( const std::string &hotkey : vSettingsHotkeys[i] ) {
                    if( sInput.text == hotkey ) {
                        sel2 = i;
                        bc_on_parent = false;
                        action = "CONFIRM";
                    }
                }
            }
        }
        if( sel1 == getopt( main_menu_opts::NEWCHAR ) ) {
            for( int i = 0; static_cast<size_t>( i ) < vNewGameSubItems.size(); ++i ) {
                for( const std::string &hotkey : vNewGameHotkeys[i] ) {
                    if( sInput.text == hotkey ) {
                        sel2 = i;
                        bc_on_parent = false;
                        action = "CONFIRM";
                    }
                }
            }
        }

        if( sel1 != sel1_at_input && ( action == "LEFT" || action == "RIGHT" ||
                                       action == "UP" || action == "DOWN" ||
                                       action == "PREV_TAB" || action == "NEXT_TAB" ||
                                       action == "PAGE_UP" || action == "PAGE_DOWN" ||
                                       action == "SCROLL_UP" || action == "SCROLL_DOWN" ) ) {
            action.clear();
        }

        // also check special keys
        if( action == "QUIT" ) {
            if( query_yn( _( "Really quit?" ) ) ) {
                return false;
            }
        } else if( action == "LEFT" || action == "PREV_TAB" ) {
            sel_line = 0;
            sel1 = sel1 > 0 ? sel1 - 1 : max_menu_opts;
            sel2 = sel1 == getopt( main_menu_opts::LOADCHAR ) ? last_world_pos : 0;
            bc_on_parent = true;
            bc_sub_scroll = 0;
            bc_move_at = bc_now();
            on_move();
        } else if( action == "RIGHT" || action == "NEXT_TAB" ) {
            sel_line = 0;
            sel1 = sel1 < max_menu_opts ? sel1 + 1 : 0;
            sel2 = sel1 == getopt( main_menu_opts::LOADCHAR ) ? last_world_pos : 0;
            bc_on_parent = true;
            bc_sub_scroll = 0;
            bc_move_at = bc_now();
            on_move();
        } else if( action == "UP" || action == "DOWN" ) {
            const int sub_count = static_cast<int>( bc_subs.size() );
            if( action == "DOWN" ) {
                if( bc_on_parent && sub_count > 0 ) {
                    bc_on_parent = false;
                    sel2 = 0;
                } else if( !bc_on_parent && sel2 + 1 < sub_count ) {
                    sel2++;
                } else {
                    sel1 = sel1 < max_menu_opts ? sel1 + 1 : 0;
                    sel2 = sel1 == getopt( main_menu_opts::LOADCHAR ) ? last_world_pos : 0;
                    bc_on_parent = true;
                    bc_sub_scroll = 0;
                }
            } else {
                if( !bc_on_parent && sel2 > 0 ) {
                    sel2--;
                } else if( !bc_on_parent ) {
                    bc_on_parent = true;
                } else {
                    sel1 = sel1 > 0 ? sel1 - 1 : max_menu_opts;
                    sel2 = sel1 == getopt( main_menu_opts::LOADCHAR ) ? last_world_pos : 0;
                    bc_sub_scroll = 0;
                }
            }
            bc_move_at = bc_now();
            on_move();
        } else if( action == "PAGE_UP" || action == "PAGE_DOWN" ||
                   action == "SCROLL_UP" || action == "SCROLL_DOWN" ) {
            const int sub_count = static_cast<int>( bc_subs.size() );
            if( sub_count > 0 ) {
                const bool up = action == "PAGE_UP" || action == "SCROLL_UP";
                const bool page = action == "PAGE_UP" || action == "PAGE_DOWN";
                const int step = page ? std::max( 1, bc_visible_subs ) : 1;
                sel2 = clamp( up ? sel2 - step : sel2 + step, 0, sub_count - 1 );
                bc_on_parent = false;
                bc_move_at = bc_now();
                on_move();
            }
        } else if( action == "CONFIRM" ) {
            switch( static_cast<main_menu_opts>( sel1 ) ) {
                case main_menu_opts::HELP:
                    get_help().display_help();
                    break;
                case main_menu_opts::QUIT:
                    return false;
                case main_menu_opts::SPECIAL:
                    if( MAP_SHARING::isSharing() ) {
                        on_error();
                        popup( _( "Special games don't work with shared maps." ) );
                    } else if( sel2 >= 0 && sel2 < static_cast<int>( special_game_type::NUM_SPECIAL_GAME_TYPES ) - 1 ) {
                        on_out_of_scope cleanup( [&player_character]() {
                            g->gamemode.reset();
                            player_character = avatar();
                            world_generator->set_active_world( nullptr );
                        } );
                        g->gamemode = get_special_game( static_cast<special_game_type>( sel2 + 1 ) );
                        // check world
                        WORLD *world = world_generator->make_new_world( static_cast<special_game_type>( sel2 + 1 ) );
                        if( world == nullptr ) {
                            break;
                        }
                        world_generator->set_active_world( world );
                        try {
                            g->setup();
                        } catch( const std::exception &err ) {
                            debugmsg( "Error: %s", err.what() );
                            break;
                        }
                        if( !g->gamemode->init() ) {
                            break;
                        }
                        cleanup.cancel();
                        start = true;
                        if( g->gametype() == special_game_type::TUTORIAL ) {
                            load_game = true;
                        }
                    }
                    break;
                case main_menu_opts::SETTINGS:
                    if( sel2 == 0 ) {        /// Options
                        get_options().show( false );
                        // The language may have changed- gracefully handle this.
                        init_strings();
                    } else if( sel2 == 1 ) { /// Keybindings
                        input_context ctxt_default = get_default_mode_input_context();
                        ctxt_default.display_menu();
                    } else if( sel2 == 2 ) { /// Autopickup
                        get_auto_pickup().show();
                    } else if( sel2 == 3 ) { /// Safemode
                        get_safemode().show();
                    } else if( sel2 == 4 ) { /// Colors
                        all_colors.show_gui();
                    }
                    break;
                case main_menu_opts::WORLD:
                    sel2 = std::min<int>( sel2, world_generator->all_worldnames().size() );
                    world_tab( sel2 > 0 ? world_generator->all_worldnames().at( sel2 - 1 ) : "" );
                    break;
                case main_menu_opts::LOADCHAR:
                    if( static_cast<std::size_t>( sel2 ) < world_generator->all_worldnames().size() ) {
                        start = load_character_tab( world_generator->all_worldnames().at( sel2 ) );
                        if( start ) {
                            load_game = true;
                        }
                    } else {
                        popup( _( "No world to load." ) );
                    }
                    break;
                case main_menu_opts::NEWCHAR:
                    start = new_character_tab();
                    break;
                case main_menu_opts::CREDITS: {
                    int pos = 0;
                    ui_adaptor credits_ui;
                    credits_ui.on_redraw( [&]( const ui_adaptor & ) {
                        display_text( mmenu_credits, _( "Credits" ), pos );
                    } );
                    while( true ) {
                        ui_manager::redraw();
                        const std::string credits_action = ctxt.handle_input();
                        if( credits_action == "UP" || credits_action == "SCROLL_UP" ) {
                            pos--;
                        } else if( credits_action == "DOWN" || credits_action == "SCROLL_DOWN" ) {
                            pos++;
                        } else if( credits_action == "PAGE_UP" ) {
                            pos -= 10;
                        } else if( credits_action == "PAGE_DOWN" ) {
                            pos += 10;
                        } else if( credits_action == "CONFIRM" || credits_action == "QUIT" ||
                                   credits_action == "LEFT" || credits_action == "RIGHT" ) {
                            break;
                        }
                        pos = clamp( pos, 0, std::max( 0, mmenu_credits_len - 1 ) );
                    }
                    break;
                }
            }
        }
    }
    if( start && !load_game && get_scenario() ) {
        add_msg( get_scenario()->description( player_character.male ) );

        if( get_option<std::string>( "ETERNAL_WEATHER" ) != "normal" ) {
            if( player_character.posz() >= 0 ) {
                add_msg( _( "You feel as if this %1$s will last forever…" ),
                         get_options().get_option( "ETERNAL_WEATHER" ).getValueName() );
            }
        }
    }
    return true;
}

bool main_menu::new_character_tab()
{
    avatar &pc = get_avatar();
    // Preset character templates
    if( sel2 == 1 ) {
        if( templates.empty() ) {
            on_error();
            popup( _( "No templates found!" ) );
            return false;
        }
        while( true ) {
            uilist mmenu( _( "Choose a preset character template" ), {} );
            mmenu.border_color = c_white;
            int opt_val = 0;
            for( const std::string &tmpl : templates ) {
                mmenu.entries.emplace_back( opt_val++, true, MENU_AUTOASSIGN, tmpl );
            }
            mmenu.entries.emplace_back( opt_val, true, 'q', _( "<- Back to Main Menu" ), c_yellow, c_yellow );
            mmenu.query();
            opt_val = mmenu.ret;
            if( opt_val < 0 || static_cast<size_t>( opt_val ) >= templates.size() ) {
                return false;
            }

            std::string res = query_popup()
                              .context( "LOAD_DELETE_CANCEL" ).default_color( c_white )
                              .message( _( "What to do with template \"%s\"?" ), templates[opt_val] )
                              .option( "LOAD" ).option( "DELETE" ).option( "CANCEL" ).cursor( 0 )
                              .query().action;
            if( res == "DELETE" &&
                query_yn( _( "Are you sure you want to delete %s?" ), templates[opt_val] ) ) {
                const auto path = PATH_INFO::templatedir() + templates[opt_val] + ".template";
                if( !remove_file( path ) ) {
                    popup( _( "Sorry, something went wrong." ) );
                } else {
                    templates.erase( templates.begin() + opt_val );
                }
            } else if( res == "LOAD" ) {
                on_out_of_scope cleanup( [&pc]() {
                    pc = avatar();
                    world_generator->set_active_world( nullptr );
                } );
                g->gamemode = nullptr;
                WORLD *world = world_generator->pick_world();
                if( world == nullptr ) {
                    continue;
                }
                world_generator->set_active_world( world );
                try {
                    g->setup();
                } catch( const std::exception &err ) {
                    debugmsg( "Error: %s", err.what() );
                    continue;
                }
                if( !pc.create( character_type::TEMPLATE, templates[opt_val] ) ) {
                    load_char_templates();
                    MAPBUFFER.clear();
                    overmap_buffer.clear();
                    return false;
                }
                if( !g->start_game() ) {
                    return false;
                }
                cleanup.cancel();
                return true;
            }

            if( templates.empty() ) {
                return false;
            }
        }
    } else { ///Non-template options
        on_out_of_scope cleanup( [&pc]() {
            pc = avatar();
            world_generator->set_active_world( nullptr );
        } );
        g->gamemode = nullptr;
        // First load the mods, this is done by
        // loading the world.
        // Pick a world, suppressing prompts if it's "play now" mode.
        WORLD *world = world_generator->pick_world( true, sel2 == 3 || sel2 == 4 );
        if( world == nullptr ) {
            return false;
        }
        world_generator->set_active_world( world );
        try {
            g->setup();
        } catch( const std::exception &err ) {
            debugmsg( "Error: %s", err.what() );
            return false;
        }
        character_type play_type = character_type::CUSTOM;
        switch( sel2 ) {
            case 0:
                play_type = character_type::CUSTOM;
                break;
            case 2:
                play_type = character_type::RANDOM;
                break;
            case 3:
                play_type = character_type::NOW;
                break;
            case 4:
                play_type = character_type::FULL_RANDOM;
                break;
        }
        if( !pc.create( play_type ) ) {
            load_char_templates();
            MAPBUFFER.clear();
            overmap_buffer.clear();
            return false;
        }

        if( !g->start_game() ) {
            return false;
        }
        cleanup.cancel();
        return true;
    }
    return false;
}

bool main_menu::load_game( std::string const &worldname, save_t const &savegame )
{
    avatar &pc = get_avatar();
    on_out_of_scope cleanup( [&pc]() {
        pc = avatar();
        world_generator->set_active_world( nullptr );
    } );

    g->gamemode = nullptr;
    WORLD *world = world_generator->get_world( worldname );
    world_generator->last_world_name = world->world_name;
    world_generator->last_character_name = savegame.decoded_name();
    world_generator->save_last_world_info();
    world_generator->set_active_world( world );

    try {
        g->setup();
    } catch( const std::exception &err ) {
        debugmsg( "Error: %s", err.what() );
        return false;
    }

    if( g->load( savegame ) ) {
        cleanup.cancel();
        return true;
    }

    return false;
}

static std::optional<std::chrono::seconds> get_playtime_from_save(const WORLD* world,
    const save_t& save)
{
    cata_path playtime_file = world->folder_path_path() / (save.base_path() + ".pt");
    std::optional<std::chrono::seconds> pt_seconds;
    if (file_exist(playtime_file)) {
        read_from_file(playtime_file, [&pt_seconds](std::istream& fin) {
            if (fin.eof()) {
                return;
            }
            std::chrono::seconds::rep dur_seconds = 0;
            fin.imbue(std::locale::classic());
            fin >> dur_seconds;
            pt_seconds = std::chrono::seconds(dur_seconds);
            });
    }
    return pt_seconds;
}

bool main_menu::load_character_tab( const std::string &worldname )
{
    WORLD* cur_world = world_generator->get_world(worldname);
    savegames = cur_world->world_saves;
    if( MAP_SHARING::isSharing() ) {
        auto new_end = std::remove_if( savegames.begin(), savegames.end(), []( const save_t &str ) {
            return str.decoded_name() != MAP_SHARING::getUsername();
        } );
        savegames.erase( new_end, savegames.end() );
    }

    if( savegames.empty() ) {
        on_error();
        //~ %s = world name
        popup( _( "%s has no characters to load!" ), worldname );
        return false;
    }

    uilist mmenu( string_format( _( "Load character from \"%s\"" ), worldname ), {} );
    mmenu.border_color = c_white;
    int opt_val = 0;
    for( const save_t &s : savegames ) {
        std::optional<std::chrono::seconds> playtime = get_playtime_from_save(cur_world, s);
        std::string save_str = s.decoded_name();
        if (playtime) {
            int padding = std::max(16 - utf8_width(save_str), 0) + 2;
            std::chrono::seconds::rep tmp_sec = playtime->count();
            int pt_sec = static_cast<int>(tmp_sec % 60);
            int pt_min = static_cast<int>(tmp_sec % 3600) / 60;
            int pt_hrs = static_cast<int>(tmp_sec / 3600);
            save_str = string_format("%s%s<color_c_light_blue>[%02d:%02d:%02d]</color>",
                save_str, std::string(padding, ' '), pt_hrs, pt_min,
                static_cast<int>(pt_sec));
        }
        mmenu.entries.emplace_back(opt_val++, true, MENU_AUTOASSIGN, save_str);
    }
    mmenu.entries.emplace_back( opt_val, true, 'q', _( "<- Back to Main Menu" ), c_yellow, c_yellow );
    mmenu.query();
    opt_val = mmenu.ret;
    if( opt_val < 0 || static_cast<size_t>( opt_val ) >= savegames.size() ) {
        return false;
    }

    return main_menu::load_game( worldname, savegames[opt_val] );
}

void main_menu::world_tab( const std::string &worldname )
{
    // Create world
    if( sel2 == 0 ) {
        world_generator->make_new_world();
        return;
    }

    uilist mmenu( string_format( _( "Manage world \"%s\"" ), worldname ), {} );
    mmenu.border_color = c_white;
    int opt_val = 0;
    std::array<char, 6> hotkeys = { 'm', 'e', 's', 't', 'd', 'r'};
    for( const std::string &it : vWorldSubItems ) {
        mmenu.entries.emplace_back( opt_val, true, hotkeys[opt_val],
                                    remove_color_tags( shortcut_text( c_white, it ) ) );
        ++opt_val;
    }
    mmenu.entries.emplace_back( opt_val, true, 'q', _( "<- Back to Main Menu" ), c_yellow, c_yellow );
    mmenu.query();
    opt_val = mmenu.ret;
    if( opt_val < 0 || static_cast<size_t>( opt_val ) >= vWorldSubItems.size() ) {
        return;
    }

    auto clear_world = [this, &worldname]( bool do_delete ) {
        world_generator->delete_world( worldname, do_delete );
        savegames.clear();
        MAPBUFFER.clear();
        overmap_buffer.clear();
        if( do_delete ) {
            sel2 = 0; // reset to create world selection
        }
    };

    switch( opt_val ) {
        
        case 0: // Active World Mods
            world_generator->show_active_world_mods(
                world_generator->get_world( worldname )->active_mod_order );
            break;
        case 1: {


            WORLD  *the_world = world_generator->get_world(worldname);

            ui_adaptor ui;
            catacurses::window new_window;

            int win_height = 0;
            bool recalc_startpos = false;
            const auto init_windows = [&](ui_adaptor& ui) {
                recalc_startpos = true;
                const int iMinScreenWidth = std::max(FULL_SCREEN_WIDTH, TERMX / 2);
                const int iOffsetX = TERMX > FULL_SCREEN_WIDTH ? (TERMX - iMinScreenWidth) / 2 : 0;
                new_window = catacurses::newwin(TERMY, iMinScreenWidth, point(iOffsetX, 0));
                win_height = getmaxy(new_window);
                ui.position_from_window(new_window);
            };

            init_windows(ui);
            ui.on_screen_resize(init_windows);
            
            world_generator->show_worldgen_tab_modselection(new_window, the_world, false,true);

            break;
        }
        case 2: // Copy World settings
            world_generator->make_new_world( true, worldname );
            break;
        case 3: // Character to Template
            if( load_character_tab( worldname ) ) {
                avatar &pc = get_avatar();
                pc.setID( character_id(), true );
                pc.reset_all_missions();
                pc.character_to_template( pc.name );
                pc = avatar();
                MAPBUFFER.clear();
                overmap_buffer.clear();
                load_char_templates();
            }
            break;
        case 4: // Delete World
            if( query_yn( _( "Delete the world and all saves within?" ) ) ) {
                clear_world( true );
            }
            break;
        case 5: // Reset World
            if( query_yn( _( "Remove all saves and regenerate world?" ) ) ) {
                clear_world( false );
            }
            break;
        default:
            break;
    }
}
