#pragma once
#ifndef CATA_SRC_D20_ROLL_H
#define CATA_SRC_D20_ROLL_H

#include <algorithm>

#include "rng.h"

class Character;
class Creature;

/**
 * 二十面骰的投掷状态。
 * 优势投两次取较高的一次，劣势投两次取较低的一次。
 * 摔投/抓取与弓箭隐蔽射击共用这套状态与投骰实现。
 */
enum class d20_roll_state {
    normal,
    advantage,
    disadvantage
};

// 普通投一次 d20；优势取两次中较高的一次，劣势取较低的一次。
inline int roll_d20( const d20_roll_state state )
{
    const int first = rng( 1, 20 );
    if( state == d20_roll_state::normal ) {
        return first;
    }
    const int second = rng( 1, 20 );
    return state == d20_roll_state::advantage ? std::max( first, second ) :
           std::min( first, second );
}

// 依据攻击者与目标的状态判定本次投骰是普通、优势还是劣势（定义见 creature.cpp）。
d20_roll_state get_d20_roll_state( const Character &you, const Creature &target );

#endif // CATA_SRC_D20_ROLL_H
