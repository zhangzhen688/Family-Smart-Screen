/**
 * @file ui_main.h
 * Main home screen page — clock, sensor data, quick-access icons.
 */
#ifndef UI_MAIN_H
#define UI_MAIN_H

#include "lvgl.h"

void ui_main_page_create(void);

/* Access to main page label for updating sensor data */
extern lv_obj_t *ui_main_label_humidity;
extern lv_obj_t *ui_main_label_temp;
extern lv_obj_t *ui_main_label_clock;

#endif /* UI_MAIN_H */
