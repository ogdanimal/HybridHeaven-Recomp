#ifndef __HH_WIDESCREEN_H__
#define __HH_WIDESCREEN_H__

#include <cstdint>

namespace hybridheaven {
    namespace widescreen {
        // Widen the game's own 4:3 safe-rect commands in rdram, so the 16:9 frame
        // RT64 renders carries live geometry out to its edges instead of stopping
        // at the inset the game clips itself to. Called from RT64Context::send_dl,
        // on the gfx thread, immediately before RT64 walks the list.
        //
        // Reads the aspect setting itself and reverts its own edits when that
        // setting goes back to Original, so nothing here needs a host export or a
        // restart. See the file comment in src/main/widescreen.cpp.
        // `data_ptr` is the task's display-list pointer -- the buffer the game is
        // submitting THIS frame. It is what separates a cached site that is still
        // live from one left behind in a buffer the game has stopped rebuilding.
        void widen_display_lists(uint8_t* rdram, uint32_t data_ptr);
    }
}

#endif
