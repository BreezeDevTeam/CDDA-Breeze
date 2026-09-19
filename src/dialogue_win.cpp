#include "dialogue_win.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "catacharset.h"
#if defined( TILES )
#include "character_preview.h"
#endif
#include "input.h"
#include "messages.h"
#include "output.h"
#include "point.h"
#include "sdltiles.h"
#include "string_formatter.h"
#include "translations.h"
#include "ui_manager.h"
#include "uistate.h"

// Height of the response section
static const int RESPONSES_LINES = 15;

multiline_list_entry talk_data::get_entry() const
{
    multiline_list_entry entry;
    entry.entry_text = colorize( text, color );
    entry.prefix = formatted_hotkey( hotkey_desc, color );
    return entry;
}

dialogue_window::dialogue_window()
{
    responses_list = std::make_unique<multiline_list>( resp_win );
    history_view = std::make_unique<scrolling_text_view>( history_win );
}

dialogue_window::~dialogue_window()
{
    if( temporary_image != nullptr ) {
        SDL_DestroyTexture( temporary_image );
    }
}

bool dialogue_window::wants_character_sidebar() const
{
    return !is_computer && !is_not_conversation;
}

bool dialogue_window::has_character_sidebar() const
{
    return sidebar_enabled;
}

void dialogue_window::resize( ui_adaptor &ui )
{
    const int win_beginy = TERMY > FULL_SCREEN_HEIGHT ? ( TERMY - FULL_SCREEN_HEIGHT ) / 4 : 0;
    const int win_beginx = TERMX > FULL_SCREEN_WIDTH ? ( TERMX - FULL_SCREEN_WIDTH ) / 4 : 0;
    const int maxy = win_beginy ? TERMY - 2 * win_beginy : FULL_SCREEN_HEIGHT;
    const int maxx = win_beginx ? TERMX - 2 * win_beginx : FULL_SCREEN_WIDTH;
    d_win = catacurses::newwin( maxy, maxx, point( win_beginx, win_beginy ) );
    ui.position_from_window( d_win );

    sidebar_enabled = false;
    sidebar_width = 0;
    content_left = 0;
    content_width = std::max( 1, maxx - 2 );
    portrait_height_cells = 0;
    portrait_win = catacurses::window();

    // A character card needs at least 20 columns of its own and 20 columns for dialogue.
    if( wants_character_sidebar() && maxx >= 60 ) {
        sidebar_enabled = true;
        // The character card owns roughly one fifth of the dialogue width.  Keep enough room on
        // small terminals for the actual conversation to remain readable.
        sidebar_width = std::max( 20, maxx * 3 / 10 );
        sidebar_width = std::min( sidebar_width, std::max( 0, maxx - 40 ) );
        content_left = sidebar_width;
        content_width = std::max( 1, maxx - content_left - 2 );

        const int portrait_cols = std::max( 8, sidebar_width - 2 );
        const int inner_width_px = std::max( 1, ( portrait_cols - 2 ) * fontwidth );
        const int desired_inner_height_px = inner_width_px * 4 / 3;
        const int max_portrait_rows = std::max( 6, maxy - 12 );
        portrait_height_cells = std::clamp(
                                    ( desired_inner_height_px + fontheight - 1 ) / fontheight + 2,
                                    6, max_portrait_rows );

        const point portrait_begin( win_beginx + 1, win_beginy + 2 );
        const int inner_height_px = std::max( 1, ( portrait_height_cells - 2 ) * fontheight );
        portrait_inner_rect = {
            ( portrait_begin.x + 1 ) * fontwidth,
            ( portrait_begin.y + 1 ) * fontheight,
            inner_width_px,
            inner_height_px
        };
        portrait_win = catacurses::newwin( portrait_height_cells, portrait_cols, portrait_begin );
    }

    response_width = has_character_sidebar() ? std::max( 1, content_width ) :
                     std::max( 1, maxx / 2 );

    if( has_character_sidebar() ) {
        const int usable_rows = std::max( 10, maxy - 3 );
        const int response_rows = std::clamp( ( usable_rows * 3 + 5 ) / 10, 7,
                                  std::max( 7, usable_rows - 6 ) );
        separator_y = maxy - response_rows - 2;

        // scrolling_text_view owns its first column as a scrollbar / vertical edge.  Put that
        // column directly on the character-card divider instead of one cell to its right, so the
        // dialogue no longer renders a doubled vertical line.
        history_win = catacurses::newwin( std::max( 1, separator_y - 2 ), content_width + 1,
                                          point( win_beginx + content_left, win_beginy + 2 ) );
        // multiline_list also owns its first column for its left edge / scrollbar.  Overlay
        // that column on the character-card divider just like history_win, otherwise the
        // response area gets a second vertical line one cell to the right of the divider.
        resp_win = catacurses::newwin( std::max( 1, maxy - separator_y - 3 ),
                                       response_width + 1,
                                       point( win_beginx + content_left,
                                              win_beginy + separator_y + 2 ) );
    } else {
        separator_y = maxy - 1 - RESPONSES_LINES - 1;
        history_win = catacurses::newwin( maxy - 1 - RESPONSES_LINES - 2 - 1,
                                          content_width,
                                          point( win_beginx + content_left + 1, win_beginy + 2 ) );
        resp_win = catacurses::newwin( RESPONSES_LINES - 1, response_width,
                                       point( win_beginx + content_left + 1,
                                              win_beginy + maxy - RESPONSES_LINES ) );
    }

    // Reset size-dependant state
    update_history_view = true;
    responses_list->fold_entries();
}

void dialogue_window::draw_character_sidebar( const std::string &npc_name )
{
    if( !has_character_sidebar() || sidebar_width <= 0 ) {
        return;
    }

    if( portrait_win ) {
        werase( portrait_win );
        draw_border( portrait_win );
    }

    int text_y = 2 + portrait_height_cells + 1;
    const int text_width = std::max( 1, sidebar_width - 4 );
    if( text_y < getmaxy( d_win ) - 1 ) {
        trim_and_print( d_win, point( 2, text_y ), text_width, c_white, npc_name );
        ++text_y;
    }
    if( has_affection_score && text_y < getmaxy( d_win ) - 1 ) {
        std::string relationship = pgettext( "NPC relationship", "Indifferent" );
        nc_color relationship_color = c_light_gray;
        if( affection_score_value <= -60 ) {
            relationship = pgettext( "NPC relationship", "憎恨" );
            relationship_color = c_red;
        } else if( affection_score_value <= -40 ) {
            relationship = pgettext( "NPC relationship", "厌恶" );
            relationship_color = c_light_red;
        } else if( affection_score_value <= -20 ) {
            relationship = pgettext( "NPC relationship", "反感" );
            relationship_color = c_yellow;
        } else if( affection_score_value >= 60 ) {
            relationship = pgettext( "NPC relationship", "充满爱意" );
            relationship_color = c_pink;
        } else if( affection_score_value >= 40 ) {
            relationship = pgettext( "NPC relationship", "信任" );
            relationship_color = c_cyan;
        } else if( affection_score_value >= 20 ) {
            relationship = pgettext( "NPC relationship", "友好" );
            relationship_color = c_light_green;
        }
        trim_and_print( d_win, point( 2, text_y ), text_width, relationship_color,
                        string_format("关系: %s", relationship ) );
        ++text_y;
    }

    // Keep the fixed dialogue utilities with the character card instead of consuming
    // dialogue-choice width on the right.
    if( text_y < getmaxy( d_win ) - 1 ) {
        ++text_y;
    }

    input_context ctxt( "DIALOGUE_CHOOSE_RESPONSE" );
    nc_color cur_color = c_magenta;
    std::string formatted_text;

    if( text_y < getmaxy( d_win ) - 1 ) {
        formatted_text = formatted_hotkey( ctxt.get_desc( "LOOK_AT", 1 ), cur_color )
                         .append( _( "Look at" ) );
        print_colored_text( d_win, point( 2, text_y ), cur_color, c_magenta, formatted_text );
        ++text_y;
    }
    if( text_y < getmaxy( d_win ) - 1 ) {
        formatted_text = formatted_hotkey( ctxt.get_desc( "SIZE_UP_STATS", 1 ), cur_color )
                         .append( _( "Size up stats" ) );
        print_colored_text( d_win, point( 2, text_y ), cur_color, c_magenta, formatted_text );
        ++text_y;
    }
    if( text_y < getmaxy( d_win ) - 1 ) {
        formatted_text = formatted_hotkey( ctxt.get_desc( "CHECK_OPINION", 1 ), cur_color )
                         .append( _( "Check opinion" ) );
        print_colored_text( d_win, point( 2, text_y ), cur_color, c_magenta, formatted_text );
        ++text_y;
    }
    if( available_display_modes() > 1 && text_y < getmaxy( d_win ) - 1 ) {
        formatted_text = formatted_hotkey( ctxt.get_desc( "CYCLE_NPC_DISPLAY", 1 ), cur_color )
                         .append( _( "Switch display" ) );
        print_colored_text( d_win, point( 2, text_y ), cur_color, c_magenta, formatted_text );
    }
}

void dialogue_window::draw( const std::string &npc_name )
{
    werase( d_win );

    print_header( npc_name );
    draw_character_sidebar( npc_name );

    wnoutrefresh( d_win );
    if( portrait_win ) {
        wnoutrefresh( portrait_win );
    }

    responses_list->print_entries();

    if( update_history_view ) {
        update_history_view = false;
        const int newindex = history.size() - num_lines_highlighted;
        std::string assembled;
        for( int i = 0; i < static_cast<int>( history.size() ); ++i ) {
            nc_color col = ( i >= newindex ) ? history[i].color : c_light_gray;
            assembled += colorize( history[i].text, col ).append( "\n" );
        }

        history_view->set_text( assembled, false );
    }
    history_view->draw( c_light_gray );

#if defined( TILES )
    if( display_mode == character_display_mode::preview && preview_is_available() ) {
        display_character_preview_in_window( *preview_character, portrait_win );
    } else if( display_mode == character_display_mode::portrait && image ) {
        draw_static_portrait();
    }
#endif
}

void dialogue_window::handle_scrolling( std::string &action, input_context &ctxt )
{
    if( responses_list->handle_navigation( action, ctxt ) ||
        history_view->handle_navigation( action, ctxt ) ) {
        // No further action required
    }
    sel_response = responses_list->get_entry_pos();
}

void dialogue_window::set_up_scrolling( input_context &ctxt ) const
{
    if( !is_computer && !is_not_conversation ) {
        ctxt.register_action( "LOOK_AT" );
        ctxt.register_action( "SIZE_UP_STATS" );
        ctxt.register_action( "YELL" );
        ctxt.register_action( "CHECK_OPINION" );
        if( available_display_modes() > 1 ) {
            ctxt.register_action( "CYCLE_NPC_DISPLAY" );
        }
    }
    history_view->set_up_navigation( ctxt, scrolling_key_scheme::angle_bracket_scroll );
    responses_list->set_up_navigation( ctxt );
}

void dialogue_window::add_to_history( const std::string &text, const std::string &speaker_name,
                                      nc_color speaker_color )
{
    add_to_history( speaker_name, speaker_color );
    add_to_history( text );
}

void dialogue_window::add_to_history( const std::string &text )
{
    add_to_history( text, default_color() );
}

void dialogue_window::add_to_history( const std::string &text, nc_color color )
{
    history.emplace_back( color, text );
    ++num_lines_highlighted;
    update_history_view = true;
}

void dialogue_window::add_history_separator()
{
    if( history.empty() || history.back().text.empty() ) {
        return;
    }
    add_to_history( "", default_color() );
}

void dialogue_window::clear_history_highlights()
{
    num_lines_highlighted = 0;
}

nc_color dialogue_window::default_color() const
{
    return is_computer ? c_green : c_white;
}

void dialogue_window::print_header( const std::string &name ) const
{
    draw_border( d_win );
    const int ymax = getmaxy( d_win );
    if( sidebar_width > 0 ) {
        mvwvline( d_win, point( sidebar_width, 1 ), LINE_XOXO, ymax - 2 );
        // Join the single sidebar divider to the outer frame instead of leaving one-cell gaps.
        mvwputch( d_win, point( sidebar_width, 0 ), BORDER_COLOR, LINE_OXXX );
        mvwputch( d_win, point( sidebar_width, ymax - 1 ), BORDER_COLOR, LINE_XXOX );
    }

    const int header_x = content_left + 2;
    if( is_computer ) {
        mvwprintz( d_win, point( header_x, 1 ), default_color(), _( "Interaction: %s" ), name );
    } else if( !is_not_conversation && !has_character_sidebar() ) {
        mvwprintz( d_win, point( header_x, 1 ), default_color(), _( "Dialogue: %s" ), name );
    }
    const int xmax = getmaxx( d_win );
    const int ybar = separator_y;
    // Horizontal bar dividing history and responses.  The length excludes both the divider cell
    // and the outer right border so neither side can overrun or stop one cell short.
    if( sidebar_width > 0 ) {
        mvwputch( d_win, point( sidebar_width, ybar ), BORDER_COLOR, LINE_XXXX );
        mvwhline( d_win, point( sidebar_width + 1, ybar ), LINE_OXOX,
                   std::max( 0, xmax - sidebar_width - 2 ) );
        mvwputch( d_win, point( xmax - 1, ybar ), BORDER_COLOR, LINE_XOXX );
    } else {
        mvwputch( d_win, point( 0, ybar ), BORDER_COLOR, LINE_XXXO );
        mvwhline( d_win, point( 1, ybar ), LINE_OXOX, std::max( 0, xmax - 2 ) );
        mvwputch( d_win, point( xmax - 1, ybar ), BORDER_COLOR, LINE_XOXX );
    }
    if( is_computer ) {
        // NOLINTNEXTLINE(cata-use-named-point-constants)
        mvwprintz( d_win, point( content_left + 2, ybar + 1 ), default_color(), _( "Your input:" ) );
    } else if( is_not_conversation ) {
        mvwprintz( d_win, point( content_left + 2, ybar + 1 ), default_color(), _( "What do you do?" ) );
    } else {
        // NOLINTNEXTLINE(cata-use-named-point-constants)
        mvwprintz( d_win, point( content_left + 2, ybar + 1 ), default_color(), _( "Your response:" ) );
    }
}

void dialogue_window::set_responses( const std::vector<talk_data> &responses )
{
    responses_list->create_entries( responses );
}

catacurses::window *dialogue_window::get_d_win()
{
    return &d_win;
}

catacurses::window *dialogue_window::get_history_win()
{
    return &history_win;
}

catacurses::window *dialogue_window::get_resp_win()
{
    return &resp_win;
}

bool dialogue_window::preview_is_available() const
{
#if defined( TILES )
    return preview_character != nullptr && character_preview_available();
#else
    return false;
#endif
}

int dialogue_window::available_display_modes() const
{
    int count = 0;
    if( image ) {
        ++count;
    }
    if( preview_is_available() ) {
        ++count;
    }
    return count;
}

void dialogue_window::draw_static_portrait() const
{
#if defined( TILES )
    if( !image || portrait_inner_rect.w <= 0 || portrait_inner_rect.h <= 0 ) {
        return;
    }

    const SDL_Renderer_Ptr &renderer = get_sdl_renderer();
    if( !renderer ) {
        return;
    }

    int texture_width = 0;
    int texture_height = 0;
    if( SDL_QueryTexture( image, nullptr, nullptr, &texture_width, &texture_height ) != 0 ||
        texture_width <= 0 || texture_height <= 0 ) {
        return;
    }

    SDL_Rect source = { 0, 0, texture_width, texture_height };

    const long long lhs = static_cast<long long>( texture_width ) * portrait_inner_rect.h;
    const long long rhs = static_cast<long long>( texture_height ) * portrait_inner_rect.w;
    if( lhs > rhs ) {
        source.w = std::max( 1, texture_height * portrait_inner_rect.w /
                            portrait_inner_rect.h );
        source.x = ( texture_width - source.w ) / 2;
    } else if( lhs < rhs ) {
        source.h = std::max( 1, texture_width * portrait_inner_rect.h /
                            portrait_inner_rect.w );
        source.y = ( texture_height - source.h ) / 3;
    }

    SDL_RenderCopy( renderer.get(), image, &source, &portrait_inner_rect );
#endif
}

void dialogue_window::apply_saved_display_preference()
{
    // "Hidden" remains an internal fallback only.  A saved old hidden preference is migrated to
    // the best visible representation so opening dialogue never requires an extra key press.
    if( uistate.npc_dialogue_display_mode == 1 && preview_is_available() ) {
        display_mode = character_display_mode::preview;
    } else if( image ) {
        display_mode = character_display_mode::portrait;
    } else if( preview_is_available() ) {
        display_mode = character_display_mode::preview;
    } else {
        display_mode = character_display_mode::hidden;
    }
}

void dialogue_window::set_image( SDL_Texture *image )
{
    reset_temporary_image();
    base_image = image;
    this->image = image;
    apply_saved_display_preference();
}

void dialogue_window::set_temporary_image( SDL_Texture *new_image )
{
    if( temporary_image != nullptr && temporary_image != new_image ) {
        SDL_DestroyTexture( temporary_image );
    }
    temporary_image = new_image;
    // A temporary speaker with no portrait must not inherit the original speaker's portrait.
    // Keeping image null lets the alternate speaker's character preview become the fallback.
    image = temporary_image;
    apply_saved_display_preference();
}

void dialogue_window::reset_temporary_image()
{
    if( temporary_image != nullptr ) {
        SDL_DestroyTexture( temporary_image );
        temporary_image = nullptr;
    }
    image = base_image;
    apply_saved_display_preference();
}

void dialogue_window::set_preview_character( const Character *character )
{
    preview_character = character;
    apply_saved_display_preference();
}

bool dialogue_window::cycle_character_display()
{
    // C is useful only when the NPC has both a fixed portrait and a generated character preview.
    // With only one representation available, keep it visible and do not create a no-op cycle.
    if( !image || !preview_is_available() ) {
        return false;
    }

    if( display_mode == character_display_mode::portrait ) {
        display_mode = character_display_mode::preview;
        uistate.npc_dialogue_display_mode = 1;
    } else {
        display_mode = character_display_mode::portrait;
        uistate.npc_dialogue_display_mode = 0;
    }
    return true;
}

void dialogue_window::set_character_profession( const std::string &profession )
{
    character_profession = profession;
}

void dialogue_window::set_affection_score( const int score )
{
    affection_score_value = std::clamp( score, -100, 100 );
    has_affection_score = true;
}
