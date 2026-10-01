#ifndef _OPLUS_BL_IC_KTZ8869_H_
#define _OPLUS_BL_IC_KTZ8869_H_

int bl_ic_ktz8869_hw_en(bool on);
int bl_ic_ktz8869_set_brightness(int value);
int bl_ic_ktz8869_set_lcd_bias_by_gpio(bool enable);
int bl_ic_ktz8869_set_lcd_bias_by_reg(bool enable);
int __init bl_ic_ktz8869_init(void);
void __exit bl_ic_ktz8869_exit(void);

#endif
