#pragma once

void menu_host_init(void);              /* after video_init / pad_init / settings_load */
void menu_host_frame(float dt);         /* once per displayed frame (also while paused) */
int  menu_host_input_blocked(void);     /* 1 while the game must not see controller input */
void menu_host_apply(void);            /* g_settings changed: apply and save them */
int  menu_host_capturing(void);         /* 1 while waiting for a button to remap */

extern int g_volume;                    /* 0-100 */
extern int g_test_pulse;
