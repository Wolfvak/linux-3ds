// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * codec/tsc.c
 *
 * Copyright (C) 2016 Sergi Granell
 * Copyright (C) 2017 Paul LaMendola
 * Copyright (C) 2020-2021 Santiago Herrera
 * based on ad7879-spi.c
 */

#define DRIVER_NAME	"3dscodec-tsc"
#define pr_fmt(fmt)	DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>

#include <linux/font.h>
#include <asm/io.h>

#define HIGHLIGHT_COLOR 0xFF0000
#define COLOR_BLACK		0x000000
#define COLOR_WHITE		0xFFFFFF
void nintendo3ds_bottom_lcd_clear_screen(unsigned int color);
int nintendo3ds_bottom_lcd_draw_text(const struct font_desc *font, int x, int y, unsigned int fgcolor, unsigned int bgcolor, const char *text);

#define POLL_INTERVAL_DEFAULT		33 /* ~30fps */
#define MAX_12BIT			((1 << 12) - 1)
#define CIRCLE_PAD_THRESHOLD		150
#define CIRCLE_PAD_FACTOR		150

#define VKB_ROWS (6)
#define VKB_COLS (17)


#define LEFT_SHIFTED  BIT(0)
#define RIGHT_SHIFTED BIT(1)

#define CODEC_REG(bank, reg)	(((bank) << 7) | (reg))

struct vkb_ctx_t {
	const struct font_desc *font;
	unsigned int key_locked[VKB_ROWS][VKB_COLS / sizeof(int) + 1];
	unsigned int x_offsets[VKB_ROWS][VKB_COLS];
	unsigned char x_sizes[VKB_ROWS][VKB_COLS];
	unsigned char last_key;
	bool locked_key;
	int held_row;
	int held_col;
	char shifted;
};

struct tsc_touch_hid {
	struct device *dev;
	struct regmap *map;
	struct input_dev *input_dev;

	struct vkb_ctx_t vkb;
	unsigned long touch_jiffies;
	bool pendown;
};

/* VKB stuff */
static const char *vkb_map_normal[VKB_ROWS][VKB_COLS] = {
	{"Es", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "PSc", "SLk", "Bk"},
	{"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "BSp", "Ins", "Hom", "PU"},
	{"<>", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", " \\", "Del", "End", "PD"},
	{"Cap", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "Ent", NULL, NULL, NULL, NULL},
	{"LShf", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "RShf", NULL, NULL, NULL, NULL, NULL},
	{"Ctl", "M", "Alt", "Space", "Alt", "M", "Mnu", "Ctl", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};
static const char *vkb_map_shift[VKB_ROWS][VKB_COLS] = {
	{"Es", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "PSc", "SLk", "Bk"},
	{"`", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "+", "BSp", "Ins", "Hom", "PU"},
	{"<>", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}", " |", "Del", "End", "PD"},
	{"Cap", "A", "S", "D", "F", "G", "H", "J", "K", "L", ":", "\"", "Ent", NULL, NULL, NULL, NULL},
	{"LShf", "Z", "X", "C", "V", "B", "N", "M", "<", ">", "?", "RShf", NULL, NULL, NULL, NULL, NULL},
	{"Ctl", "M", "Alt", "Space", "Alt", "M", "Mnu", "Ctl", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};
static const char vkb_map_keys[VKB_ROWS][VKB_COLS] = {
	{KEY_ESC, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_SYSRQ, KEY_SCROLLLOCK, KEY_PAUSE},
	{KEY_GRAVE, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_INSERT, KEY_HOME, KEY_PAGEUP},
	{KEY_TAB, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, KEY_DELETE, KEY_END, KEY_PAGEDOWN},
	{KEY_CAPSLOCK, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_COMMA, KEY_ENTER, 0, 0, 0, 0},
	{KEY_LEFTSHIFT, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RIGHTSHIFT, 0, 0, 0, 0, 0},
	{KEY_LEFTCTRL, KEY_LEFTMETA, KEY_LEFTALT, KEY_SPACE, KEY_RIGHTALT, KEY_RIGHTMETA, KEY_MENU, KEY_RIGHTCTRL, 0, 0, 0, 0, 0, 0, 0, 0, 0}
};

static void vkb_draw_key(const struct vkb_ctx_t *vkb, int row, int col) {
	unsigned int color;

	if(vkb->key_locked[row][col / sizeof(int)] & BIT(col % sizeof(int)))
		color = HIGHLIGHT_COLOR;
	else
		color = COLOR_WHITE;

	if(vkb->shifted) {
		if(vkb_map_shift[row][col]) {
			if (row == 0 || row == 5 || vkb_map_normal[row][col][1] != '\0')
				nintendo3ds_bottom_lcd_draw_text(vkb->font, vkb->x_offsets[row][col], row * vkb->font->height * 2, COLOR_BLACK, color,
						                               vkb_map_shift[row][col]);
			else
				nintendo3ds_bottom_lcd_draw_text(vkb->font, vkb->x_offsets[row][col], row * vkb->font->height * 2, color, COLOR_BLACK, 
						                               vkb_map_shift[row][col]);
		}
	} else {
		if(vkb_map_normal[row][col]) {
			if (row == 0 || row == 5 || vkb_map_normal[row][col][1] != '\0')
				nintendo3ds_bottom_lcd_draw_text(vkb->font, vkb->x_offsets[row][col], row * vkb->font->height * 2, COLOR_BLACK, color,
						                               vkb_map_normal[row][col]);
			else
				nintendo3ds_bottom_lcd_draw_text(vkb->font, vkb->x_offsets[row][col], row * vkb->font->height * 2, color, COLOR_BLACK, 
						                               vkb_map_normal[row][col]);
		}
	}
}

static void vkb_draw_bottom_lcd(const struct vkb_ctx_t *vkb)
{
	int i, j;

	for (j = 0; j < VKB_ROWS; j++) {
		for (i = 0; i < VKB_COLS; i++) {
			vkb_draw_key(vkb, j, i);
		}
	}
}

static int vkb_init(struct vkb_ctx_t *vkb)
{
	int x, i, j;

	vkb->font = get_default_font(320, 240, -1, -1);
	vkb->last_key = 0;
	vkb->locked_key = false;
	vkb->shifted = false;

	for(j = 0; j < VKB_ROWS; j++) {
		x = 0;
		for(i = 0; i < VKB_COLS; i++) {
			if (!vkb_map_normal[j][i]) {
				vkb->x_sizes[j][i] = 0;
				vkb->x_offsets[j][i] = 0;
				continue;
			}
			vkb->x_offsets[j][i] = x;
			vkb->x_sizes[j][i] = (strlen(vkb_map_normal[j][i]) + 1) * vkb->font->width;
			x += vkb->x_sizes[j][i];
		}
	}

	for(j = 0; j < VKB_ROWS; j++) {
		for(i = 0; i < VKB_COLS / sizeof(int) + 1; i++) {
			vkb->key_locked[j][i] = 0;
		}
	}

	nintendo3ds_bottom_lcd_clear_screen(COLOR_BLACK);
	vkb_draw_bottom_lcd(vkb);

	return 0;
}
/* End VKB stuff */

static const struct reg_sequence tsc_init_seq[] = {
	REG_SEQ(CODEC_REG(0x67, 0x24), 0x98, 10),
	REG_SEQ(CODEC_REG(0x67, 0x26), 0x00, 10),
	REG_SEQ(CODEC_REG(0x67, 0x25), 0x43, 10),
	REG_SEQ(CODEC_REG(0x67, 0x24), 0x18, 10),
	REG_SEQ(CODEC_REG(0x67, 0x17), 0x43, 10),
	REG_SEQ(CODEC_REG(0x67, 0x19), 0x69, 10),
	REG_SEQ(CODEC_REG(0x67, 0x1B), 0x80, 10),
	REG_SEQ(CODEC_REG(0x67, 0x27), 0x11, 10),
	REG_SEQ(CODEC_REG(0x67, 0x26), 0xEC, 10),
	REG_SEQ(CODEC_REG(0x67, 0x24), 0x18, 10),
	REG_SEQ(CODEC_REG(0x67, 0x25), 0x53, 10)
};

static int tsc_touch_initialize(struct regmap *map)
{
	int err;
	unsigned int reg;

	err = regmap_multi_reg_write(map, tsc_init_seq, ARRAY_SIZE(tsc_init_seq));
	if (err) return err;

	err = regmap_update_bits(map, CODEC_REG(0x67, 0x26), 0x80, 0x80);
	if (err) return err;

	err = regmap_update_bits(map, CODEC_REG(0x67, 0x24), 0x80, 0x00);
	if (err) return err;

	return regmap_update_bits(map, CODEC_REG(0x67, 0x25), 0x3C, 0x10);
}

static int tsc_touch_request_data(struct regmap *map, u8 *buffer)
{
	int err;
	unsigned int reg;

	/* acknowledge tsc? */
	err = regmap_read(map, CODEC_REG(0x67, 0x26), &reg);
	if (err) return err;

	return regmap_bulk_read(map, CODEC_REG(0xFB, 0x01), buffer, 0x34);
}
/* End SPI stuff */

static void tsc_touch_input_poll(struct input_dev *input)
{
	struct tsc_touch_hid *codec_hid = input_get_drvdata(input);
	struct vkb_ctx_t *vkb = &codec_hid->vkb;

	u8 raw_data[0x40] __attribute__((aligned(sizeof(u32))));
	bool pendown;
	u16 raw_touch_x;
	u16 raw_touch_y;
	u16 screen_touch_x;
	u16 screen_touch_y;
	s16 raw_circlepad_x;
	s16 raw_circlepad_y;
	bool sync = false;
	int i, j, err;

	err = tsc_touch_request_data(codec_hid->map, raw_data);
	if (err)
		return;

	raw_circlepad_x =
		(s16)le16_to_cpu(((raw_data[0x24] << 8) | raw_data[0x25]) & 0xFFF) - 2048;
	raw_circlepad_y =
		(s16)le16_to_cpu(((raw_data[0x14] << 8) | raw_data[0x15]) & 0xFFF) - 2048;

	if (abs(raw_circlepad_x) > CIRCLE_PAD_THRESHOLD) {
		input_report_rel(input, REL_X,
				 -raw_circlepad_x / CIRCLE_PAD_FACTOR);
		sync = true;
	}

	if (abs(raw_circlepad_y) > CIRCLE_PAD_THRESHOLD) {
		input_report_rel(input, REL_Y,
				 -raw_circlepad_y / CIRCLE_PAD_FACTOR);
		sync = true;
	}

	pendown = !(raw_data[0] & BIT(4));

	if (pendown) {
		if(!codec_hid->pendown) {
			raw_touch_x = le16_to_cpu((raw_data[0]  << 8) | raw_data[1]);
			raw_touch_y = le16_to_cpu((raw_data[10] << 8) | raw_data[11]);

			screen_touch_x = (u16)((u32)raw_touch_x * 320 / MAX_12BIT);
			screen_touch_y = (u16)((u32)raw_touch_y * 240 / MAX_12BIT);

			for(j = 0; j < VKB_ROWS; j++) {
				for(i = 0; i < VKB_COLS; i++) {
					if(vkb->x_sizes[j][i] > 0 &&
					   screen_touch_x >= vkb->x_offsets[j][i] &&
					   screen_touch_x < vkb->x_offsets[j][i] + vkb->x_sizes[j][i] &&
					   screen_touch_y >= j * vkb->font->height * 2 &&
					   screen_touch_y < (j + 1) * vkb->font->height * 2) {
						codec_hid->pendown = true;

						codec_hid->touch_jiffies = jiffies;

						vkb->last_key = vkb_map_keys[j][i];
						if(vkb->key_locked[j][i / sizeof(int)] & BIT(i % sizeof(int))) {
							vkb->key_locked[j][i / sizeof(int)] &= ~BIT(i % sizeof(int));
							input_report_key(input, vkb->last_key, 0);
							if(vkb->last_key == KEY_LEFTSHIFT)
								vkb->shifted &= ~LEFT_SHIFTED;
							else if(vkb->last_key == KEY_RIGHTSHIFT)
								vkb->shifted &= ~RIGHT_SHIFTED;

							if(vkb->shifted == 0)
								vkb_draw_bottom_lcd(vkb);

							vkb->locked_key = true;

							vkb_draw_key(vkb, j, i);
						} else {
							input_report_key(input, vkb->last_key, 1);
						}

						vkb->held_row = j;
						vkb->held_col = i;

						sync = true;
						i = VKB_COLS;
						j = VKB_ROWS;
					}
				}
			}
		} else {
			if(!vkb->locked_key && time_is_before_jiffies(codec_hid->touch_jiffies + msecs_to_jiffies(500))) {
				vkb->key_locked[vkb->held_row][vkb->held_col / sizeof(int)] |= BIT(vkb->held_col % sizeof(int));
				vkb->locked_key = true;

				if(vkb_map_keys[vkb->held_row][vkb->held_col] == KEY_LEFTSHIFT)
					vkb->shifted |= LEFT_SHIFTED;
				else if(vkb_map_keys[vkb->held_row][vkb->held_col] == KEY_RIGHTSHIFT)
					vkb->shifted |= RIGHT_SHIFTED;

				if(vkb->shifted != 0)
					vkb_draw_bottom_lcd(vkb);

				vkb_draw_key(vkb, vkb->held_row, vkb->held_col);
			}
		}
	} else {
		codec_hid->pendown = false;

		if(vkb->locked_key) {
			vkb->locked_key = false;
		} else {
			if(vkb->last_key) {
				input_report_key(input, vkb->last_key, 0);
				sync = true;
			}
		}

		vkb->last_key = 0;
	}

	if(sync)
		input_sync(input);
}

static int tsc_touch_hid_probe(struct platform_device *pdev)
{
	int err;
	int i, j;
	struct device *dev;
	struct input_dev *input;
	struct regmap *map;
	struct tsc_touch_hid *codec_hid;

	dev = &pdev->dev;

	map = dev_get_regmap(dev->parent, NULL);
	if (!map)
		return -ENODEV;

	codec_hid = devm_kzalloc(dev, sizeof(*codec_hid), GFP_KERNEL);
	if (!codec_hid) {
		pr_err("failed to allocate memory for driver");
		return -ENOMEM;
	}

	input = devm_input_allocate_device(dev);
	if (!input){
		pr_err("failed to allocate input device");
		return -ENOMEM;
	}

	input_set_drvdata(input, codec_hid);
	input->name = "Nintendo 3DS CODEC HID";
	input->phys = DRIVER_NAME "/input0";

	input->dev.parent = dev;
	input->id.bustype = BUS_HOST;

	/* circle pad/mouse stuff */
	set_bit(EV_REL, input->evbit);
	set_bit(REL_X, input->relbit);
	set_bit(REL_Y, input->relbit);
	set_bit(REL_WHEEL, input->relbit);

	/* Enable VKB keys */
	set_bit(EV_KEY, input->evbit);
	input_set_capability(input, EV_MSC, MSC_SCAN);

	for (i = 0; i < VKB_ROWS; i++) {
		for (j = 0; j < VKB_COLS; j++) {
			if (vkb_map_keys[i][j])
				set_bit(vkb_map_keys[i][j], input->keybit);
		}
	}

	codec_hid->map = map;
	codec_hid->input_dev = input;
	platform_set_drvdata(pdev, codec_hid);

	err = tsc_touch_initialize(codec_hid->map);
	if (err) {
		pr_err("failed to initialize hardware (%d)\n", err);
		return err;
	}

	err = input_setup_polling(input, tsc_touch_input_poll);
	if (err) {
		pr_err("failed to setup polling (%d)\n", err);
		return err;
	}

	input_set_poll_interval(input, POLL_INTERVAL_DEFAULT);

	err = input_register_device(input);
	if (err) {
		pr_err("failed to register input device (%d)\n", err);
		return err;
	}

	vkb_init(&codec_hid->vkb);

	return 0;
}

static const struct of_device_id tsc_touch_hid_dt_ids[] = {
	{ .compatible = "nintendo," DRIVER_NAME, },
	{ }
};
MODULE_DEVICE_TABLE(of, tsc_touch_hid_dt_ids);

static struct platform_driver tsc_touch_hid_driver = {
	.probe = tsc_touch_hid_probe,

	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(tsc_touch_hid_dt_ids),
	},
};
module_platform_driver(tsc_touch_hid_driver);

MODULE_AUTHOR("Sergi Granell <xerpi.g.12@gmail.com>, Santiago Herrera");
MODULE_DESCRIPTION("Nintendo 3DS touchscreen/circlepad driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
