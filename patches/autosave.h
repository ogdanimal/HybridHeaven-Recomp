#ifndef AUTOSAVE_H
#define AUTOSAVE_H

/* Per-frame poll for the autosave feature. Safe to call more than once per
 * frame -- everything in it is either edge-triggered or measured against
 * recomp_time_us rather than a call count, for the reason autosave.c's header
 * gives. Called from patches/camera.c's func_80119F9C_4F96AC patch, which is
 * the port's only per-frame foothold inside the gameplay engine. */
void update_autosave(void);

#endif /* AUTOSAVE_H */
