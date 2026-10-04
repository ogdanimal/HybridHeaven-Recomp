#ifndef __HH_SAVE_ROLLBACK_H__
#define __HH_SAVE_ROLLBACK_H__

namespace hybridheaven {

// Installs the librecomp save observation hooks that maintain the `.manual.bak`
// rollback point. Call once during startup, before ultramodern::init_saving().
void init_save_rollback();

// Bracket for the autosave's own pak traffic. hh_save_now() sets this for the
// whole of its body -- see patches/autosave.c -- so the observer can tell
// autosave writes from deliberate, player-initiated ones.
void set_autosave_in_progress(bool in_progress);

} // namespace hybridheaven

#endif
