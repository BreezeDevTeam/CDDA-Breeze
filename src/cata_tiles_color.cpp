#if defined(TILES)
#include "cata_tiles.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "hsv_color.h"
#include "options.h"
#include "sdl_utils.h"
#include "sdl_wrappers.h"
#include "vehicle.h"

namespace
{
class render_target_guard
{
    public:
        explicit render_target_guard( const SDL_Renderer_Ptr &renderer )
            : renderer_( renderer.get() ), old_target_( SDL_GetRenderTarget( renderer.get() ) ) {
            SDL_GetRenderDrawColor( renderer_, &old_r_, &old_g_, &old_b_, &old_a_ );
        }
        ~render_target_guard() {
            SDL_SetRenderTarget( renderer_, old_target_ );
            SDL_SetRenderDrawColor( renderer_, old_r_, old_g_, old_b_, old_a_ );
        }
    private:
        SDL_Renderer *renderer_;
        SDL_Texture *old_target_;
        Uint8 old_r_ = 0;
        Uint8 old_g_ = 0;
        Uint8 old_b_ = 0;
        Uint8 old_a_ = 0;
};
} // namespace

const texture *tileset::get_tinted_tile( const SDL_Renderer_Ptr &renderer, const size_t index,
        const RGBColor &color, const texture *source ) const
{
    if( source == nullptr ) {
        source = get_tile( index );
    }
    if( source == nullptr ) {
        return nullptr;
    }

    const uint64_t color_key = ( static_cast<uint64_t>( color.r ) << 24 ) |
                               ( static_cast<uint64_t>( color.g ) << 16 ) |
                               ( static_cast<uint64_t>( color.b ) << 8 ) |
                               static_cast<uint64_t>( color.a );
    const uint64_t key = reinterpret_cast<uintptr_t>( source ) ^
                         ( color_key * 0x9E3779B97F4A7C15ULL );
    const auto cached = tinted_tile_values.find( key );
    if( cached != tinted_tile_values.end() ) {
        return &cached->second;
    }

    const auto dim = source->dimension();
    const int w = dim.first;
    const int h = dim.second;
    if( w <= 0 || h <= 0 ) {
        return nullptr;
    }

    SDL_Texture *raw_target = SDL_CreateTexture( renderer.get(), SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_TARGET, w, h );
    if( raw_target == nullptr ) {
        return nullptr;
    }
    SDL_Texture_Ptr target( raw_target );
    SDL_SetTextureBlendMode( target.get(), SDL_BLENDMODE_BLEND );

    SDL_Surface_Ptr surf = create_surface_32( w, h );
    if( !surf ) {
        return nullptr;
    }

    {
        render_target_guard guard( renderer );
        if( SDL_SetRenderTarget( renderer.get(), target.get() ) != 0 ) {
            return nullptr;
        }
        SDL_SetRenderDrawColor( renderer.get(), 0, 0, 0, 0 );
        SDL_RenderClear( renderer.get() );
        const SDL_Rect dst{ 0, 0, w, h };
        source->render_copy_ex( renderer, &dst, 0.0, nullptr, SDL_FLIP_NONE );
        if( SDL_RenderReadPixels( renderer.get(), &dst, surf->format->format,
                                 surf->pixels, surf->pitch ) != 0 ) {
            return nullptr;
        }
    }

    for( int y = 0; y < surf->h; ++y ) {
        auto *row = reinterpret_cast<Uint32 *>( static_cast<Uint8 *>( surf->pixels ) + y * surf->pitch );
        for( int x = 0; x < surf->w; ++x ) {
            Uint8 r = 0;
            Uint8 g = 0;
            Uint8 b = 0;
            Uint8 a = 0;
            SDL_GetRGBA( row[x], surf->format, &r, &g, &b, &a );
            if( a == 0 ) {
                continue;
            }
            const RGBColor recolored = tint_blend( RGBColor{ r, g, b, a }, color );
            row[x] = SDL_MapRGBA( surf->format, recolored.r, recolored.g, recolored.b, recolored.a );
        }
    }

    SDL_Texture *raw = SDL_CreateTextureFromSurface( renderer.get(), surf.get() );
    if( raw == nullptr ) {
        return nullptr;
    }
    std::shared_ptr<SDL_Texture> tex_ptr( raw, SDL_DestroyTexture );
    SDL_SetTextureBlendMode( tex_ptr.get(), SDL_BLENDMODE_BLEND );

    texture baked( std::move( tex_ptr ), SDL_Rect{ 0, 0, w, h } );
    const auto inserted = tinted_tile_values.emplace( key, std::move( baked ) );
    return &inserted.first->second;
}

std::optional<RGBColor> cata_tiles::get_vpart_tint( const vehicle &veh, const point &mount,
        const bool roof, const int z ) const
{
    if( !get_option<bool>( "VEHICLE_PART_COLOR" ) ) {
        return std::nullopt;
    }
    const int part_idx = veh.part_displayed_at( mount, true, !roof, roof, z );
    if( part_idx < 0 ) {
        return std::nullopt;
    }
    const vehicle_part &vp = veh.part( part_idx );
    if( !vp.has_custom_color() ) {
        return std::nullopt;
    }
    return vp.get_color( true ).fg;
}

#endif // TILES
