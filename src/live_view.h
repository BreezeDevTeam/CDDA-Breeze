#pragma once
#ifndef CATA_SRC_LIVE_VIEW_H
#define CATA_SRC_LIVE_VIEW_H

class live_view
{
    public:
        live_view();
        ~live_view();

        void init();
        bool is_enabled();
        void hide();
};

#endif // CATA_SRC_LIVE_VIEW_H
