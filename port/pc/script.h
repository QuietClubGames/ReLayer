#pragma once
#include "../core/s12sound.h"

int         script_load(const char *path);
void        script_inputs(long frame, S12Inputs *in);   /* OR the scripted presses for this frame into `in` */
const char *script_shot(long frame);
const char *script_set(long frame, int index);         /* index-th "set name=value" due at this frame, or NULL */                    /* screenshot name due at this frame, or NULL */
