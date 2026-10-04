#ifndef __HH_SOUND_H__
#define __HH_SOUND_H__

// One volume, applied by the port rather than by the game: main.cpp's
// queue_samples scales every frame it hands SDL by get_main_volume() / 100.
//
// Goemon64Recomp also had separate BGM and SE sliders. Those were only possible
// because a patch split the two inside the game's own audio code, so they are
// not carried over -- see the note in src/ui/ui_config.cpp.
namespace hybridheaven {
    void reset_sound_settings();
    void set_main_volume(int volume);
    int get_main_volume();
}

#endif
