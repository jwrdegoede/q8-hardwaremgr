/*
 * Allwinner q8 formfactor tablet hardware manager
 *
 * Copyright (C) 2016 Hans de Goede <hdegoede@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h> /* For constaints hack */
#include <linux/regulator/machine.h> /* For constaints hack */
#include <linux/slab.h>
#include "of-changeset-helpers.h"

/*
 * We can detect which touchscreen controller is used automatically,
 * but some controllers can be wired up differently depending on the
 * q8 PCB variant used, so they need different firmware files / settings.
 *
 * We allow the user to specify a firmware_variant to select a config
 * from a list of known configs. We also allow overriding each setting
 * individually.
 */

static int touchscreen_variant = -1;
module_param(touchscreen_variant, int, 0444);
MODULE_PARM_DESC(touchscreen_variant, "Touchscreen variant 0-x, -1 for auto");

static int touchscreen_width = -1;
module_param(touchscreen_width, int, 0444);
MODULE_PARM_DESC(touchscreen_width, "Touchscreen width, -1 for auto");

static int touchscreen_height = -1;
module_param(touchscreen_height, int, 0444);
MODULE_PARM_DESC(touchscreen_height, "Touchscreen height, -1 for auto");

static int touchscreen_invert_x = -1;
module_param(touchscreen_invert_x, int, 0444);
MODULE_PARM_DESC(touchscreen_invert_x, "Touchscreen invert x, -1 for auto");

static int touchscreen_invert_y = -1;
module_param(touchscreen_invert_y, int, 0444);
MODULE_PARM_DESC(touchscreen_invert_y, "Touchscreen invert y, -1 for auto");

static int touchscreen_swap_x_y = -1;
module_param(touchscreen_swap_x_y, int, 0444);
MODULE_PARM_DESC(touchscreen_swap_x_y, "Touchscreen swap x y, -1 for auto");

static char *touchscreen_fw_name;
module_param(touchscreen_fw_name, charp, 0444);
MODULE_PARM_DESC(touchscreen_fw_name, "Touchscreen firmware filename");

enum soc {
	a13,
	a23,
	a33,
};

#define TOUCHSCREEN_POWER_ON_DELAY	20
#define SILEAD_REG_ID			0xFC
#define EKTF2127_RESPONSE		0x52
#define EKTF2127_REQUEST		0x53
#define EKTF2127_WIDTH			0x63

enum {
	touchscreen_unknown,
	gsl1680_a082,
	gsl1680_b482,
	ektf2127,
	zet6251,
};

#define DA280_REG_CHIP_ID		0x01
#define DA280_REG_ACC_Z_LSB		0x06
#define DA280_REG_MODE_BW		0x11
#define DA280_CHIP_ID			0x13
#define DA280_MODE_ENABLE		0x1e
#define DA280_MODE_DISABLE		0x9e

#define DA311_REG_CHIP_ID		0x0f
#define DA311_CHIP_ID			0x13

#define DMARD06_CHIP_ID_REG		0x0f
#define DMARD05_CHIP_ID			0x05
#define DMARD06_CHIP_ID			0x06
#define DMARD07_CHIP_ID			0x07
#define DMARD09_REG_CHIPID		0x18
#define DMARD09_CHIPID			0x95
#define DMARD10_REG_STADR		0x12
#define DMARD10_REG_STAINT		0x1c
#define DMARD10_VALUE_STADR		0x55
#define DMARD10_VALUE_STAINT		0xaa

#define MC3230_REG_CHIP_ID		0x18
#define MC3230_CHIP_ID			0x01
#define MMA7660_CHIP_ID			0x00 /* Factory reserved on MMA7660 */
#define MC3230_REG_PRODUCT_CODE		0x3b
#define MMA7660_PRODUCT_CODE		0x00 /* Factory reserved on MMA7660 */
#define MC3210_PRODUCT_CODE		0x90
#define MC3230_PRODUCT_CODE		0x19

#define MXC6225_REG_CHIP_ID		0x08
#define MXC6225_CHIP_ID			0x05

enum {
	accel_unknown,
	da226,
	da280,
	da311,
	dmard05,
	dmard06,
	dmard07,
	dmard09,
	dmard10,
	mc3210,
	mc3230,
	mma7660,
	mxc6225,
};

struct q8_hardwaremgr_device {
	int model;
	int addr;
	const char *compatible;
	bool delete_regulator;
};

struct q8_hardwaremgr_data {
	struct device *dev;
	enum soc soc;
	struct q8_hardwaremgr_device touchscreen;
	struct q8_hardwaremgr_device accelerometer;
	int touchscreen_variant;
	int touchscreen_width;
	int touchscreen_height;
	int touchscreen_invert_x;
	int touchscreen_invert_y;
	int touchscreen_swap_x_y;
	const char *touchscreen_fw_name;
	bool has_rda599x;
};

/*
 * HACK HACK HACK this is a regulator-core private struct,
 * this must be in sync with the kernel's drivers/regulator/internal.h
 * definition.
 */
struct regulator {
	struct device *dev;
	struct list_head list;
	unsigned int always_on:1;
	unsigned int bypass:1;
	int uA_load;
	int min_uV;
	int max_uV;
	char *supply_name;
	struct device_attribute dev_attr;
	struct regulator_dev *rdev;
	/* Real regulator struct has more entires here */
};

typedef int (*bus_probe_func)(struct q8_hardwaremgr_data *data,
			      struct i2c_adapter *adap);
typedef int (*client_probe_func)(struct q8_hardwaremgr_data *data,
				 struct i2c_client *client);

static struct device_node *q8_hardware_mgr_apply_common(
	struct q8_hardwaremgr_device *dev, struct of_changeset *cset,
	const char *prefix)
{
	struct device_node *np;

	np = of_find_node_by_name(of_root, prefix);
	/* Never happens already checked in q8_hardwaremgr_do_probe() */
	if (WARN_ON(!np))
		return NULL;

	of_changeset_init(cset);
	of_changeset_add_property_u32(cset, np, "reg", dev->addr);
	of_changeset_add_property_string(cset, np, "compatible",
					 dev->compatible);
	of_changeset_update_property_string(cset, np, "status", "okay");

	if (dev->delete_regulator) {
		struct property *p;

		p = of_find_property(np, "vddio-supply", NULL);
		/* Never happens already checked in q8_hardwaremgr_do_probe() */
		if (WARN_ON(!p))
			return np;

		of_changeset_remove_property(cset, np, p);
	}

	return np; /* Allow the caller to make further changes */
}

static int q8_hardwaremgr_probe_client(struct q8_hardwaremgr_data *data,
				       struct q8_hardwaremgr_device *dev,
				       struct i2c_adapter *adap, u16 addr,
				       client_probe_func client_probe)
{
	struct i2c_client *client;
	int ret;

	client = i2c_new_dummy(adap, addr);
	if (!client)
		return -ENOMEM;

	/* ret will be one of 0: Success, -ETIMEDOUT: Bus stuck or -ENODEV */
	ret = client_probe(data, client);
	if (ret == 0)
		dev->addr = addr;

	i2c_unregister_device(client);

	return ret;
}

#define PROBE_CLIENT(dev, addr, probe) \
{ \
	int ret = q8_hardwaremgr_probe_client(data, dev, adap, addr, probe); \
	if (ret != -ENODEV) \
		return ret; \
}

static int q8_hardwaremgr_probe_silead(struct q8_hardwaremgr_data *data,
				       struct i2c_client *client)
{
	__le32 chip_id;
	int ret;

	ret = i2c_smbus_read_i2c_block_data(client, SILEAD_REG_ID,
					    sizeof(chip_id), (u8 *)&chip_id);
	if (ret != sizeof(chip_id))
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	switch (le32_to_cpu(chip_id)) {
	case 0xa0820000:
		data->touchscreen.compatible = "silead,gsl1680";
		data->touchscreen.model = gsl1680_a082;
		dev_info(data->dev, "Silead touchscreen ID: 0xa0820000\n");
		return 0;
	case 0xb4820000:
		data->touchscreen.compatible = "silead,gsl1680";
		data->touchscreen.model = gsl1680_b482;
		dev_info(data->dev, "Silead touchscreen ID: 0xb4820000\n");
		return 0;
	default:
		dev_warn(data->dev, "Silead? touchscreen with unknown ID: 0x%08x\n",
			 le32_to_cpu(chip_id));
	}

	return -ENODEV;
}

static int q8_hardwaremgr_probe_ektf2127(struct q8_hardwaremgr_data *data,
					 struct i2c_client *client)
{
	unsigned char buff[4];
	int ret;

	/* Read hello, ignore data, depends on initial power state */
	ret = i2c_master_recv(client, buff, 4);
	if (ret != 4)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	/* Request width */
	buff[0] = EKTF2127_REQUEST;
	buff[1] = EKTF2127_WIDTH;
	buff[2] = 0x00;
	buff[3] = 0x00;
	ret = i2c_master_send(client, buff, 4);
	if (ret != 4)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	msleep(20);

	/* Read response */
	ret = i2c_master_recv(client, buff, 4);
	if (ret != 4)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	if (buff[0] == EKTF2127_RESPONSE && buff[1] == EKTF2127_WIDTH) {
		data->touchscreen.compatible = "elan,ektf2127";
		data->touchscreen.model = ektf2127;
		return 0;
	}

	return -ENODEV;
}

static int q8_hardwaremgr_probe_zet6251(struct q8_hardwaremgr_data *data,
					struct i2c_client *client)
{
	unsigned char buff[4];
	int ret;

	/*
	 * We only do a simple read finger data packet test, because some
	 * versions require firmware to be loaded. If no firmware is loaded
	 * the buffer will be filed with 0xff, so we ignore the contents.
	 */
	ret = i2c_master_recv(client, buff, 24);
	if (ret != 24)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	data->touchscreen.compatible = "zeitec,zet6251";
	data->touchscreen.model = zet6251;
	return 0;
}

static int q8_hardwaremgr_probe_touchscreen(struct q8_hardwaremgr_data *data,
					    struct i2c_adapter *adap)
{
	msleep(TOUCHSCREEN_POWER_ON_DELAY);

	PROBE_CLIENT(&data->touchscreen, 0x40, q8_hardwaremgr_probe_silead);
	PROBE_CLIENT(&data->touchscreen, 0x15, q8_hardwaremgr_probe_ektf2127);
	PROBE_CLIENT(&data->touchscreen, 0x76, q8_hardwaremgr_probe_zet6251);

	return -ENODEV;
}

static void q8_hardwaremgr_apply_gsl1680_a082_variant(
	struct q8_hardwaremgr_data *data)
{
	if (touchscreen_variant != -1) {
		data->touchscreen_variant = touchscreen_variant;
	} else {
		/*
		 * These accelerometer based heuristics select the best
		 * default based on known q8 tablets.
		 */
		switch (data->accelerometer.model) {
		case mc3230:
			data->touchscreen_invert_x = 1;
			break;
		case dmard10:
		case mxc6225:
			data->touchscreen_variant = 1;
			break;
		}
	}

	switch (data->touchscreen_variant) {
	default:
		dev_warn(data->dev, "Error unknown touchscreen_variant %d using 0\n",
			 touchscreen_variant);
		/* Fall through */
	case 0:
		data->touchscreen_width = 1024;
		data->touchscreen_height = 600;
		data->touchscreen_fw_name = "gsl1680-a082-q8-700.fw";
		break;
	case 1:
		data->touchscreen_width = 480;
		data->touchscreen_height = 800;
		data->touchscreen_swap_x_y = 1;
		data->touchscreen_fw_name = "gsl1680-a082-q8-a70.fw";
		break;
	}
}

static void q8_hardwaremgr_apply_gsl1680_b482_variant(
	struct q8_hardwaremgr_data *data)
{
	if (touchscreen_variant != -1) {
		data->touchscreen_variant = touchscreen_variant;
	} else {
		/*
		 * These accelerometer based heuristics select the best
		 * default based on known q8 tablets.
		 */
		switch (data->accelerometer.model) {
		case da280:
			if (data->accelerometer.addr == 0x27)
				; /* No-op */
			else if (data->has_rda599x)
				data->touchscreen_invert_x = 1;
			else
				data->touchscreen_invert_y = 1;
			break;
		case dmard09:
			data->touchscreen_invert_x = 1;
			break;
		case mxc6225:
			data->touchscreen_variant = 1;
			break;
		}
	}

	switch (data->touchscreen_variant) {
	default:
		dev_warn(data->dev, "Error unknown touchscreen_variant %d using 0\n",
			 touchscreen_variant);
		/* Fall through */
	case 0:
		data->touchscreen_width = 960;
		data->touchscreen_height = 640;
		data->touchscreen_fw_name = "gsl1680-b482-q8-d702.fw";
		break;
	case 1:
		data->touchscreen_width = 960;
		data->touchscreen_height = 640;
		data->touchscreen_fw_name = "gsl1680-b482-q8-a70.fw";
		break;
	}
}

static void q8_hardwaremgr_issue_gsl1680_warning(
	struct q8_hardwaremgr_data *data)
{
	dev_warn(data->dev, "gsl1680 touchscreen may require kernel cmdline parameters to function properly\n");
	dev_warn(data->dev, "Try q8_hardwaremgr.touchscreen_invert_x=%d if x coordinates are inverted\n",
		 !data->touchscreen_invert_x);
	dev_warn(data->dev, "Try q8_hardwaremgr.touchscreen_variant=%d if coordinates are all over the place\n",
		 !data->touchscreen_variant);

#define	show(x) \
	dev_info(data->dev, #x " %d (%s)\n", data->x, \
		 (x == -1) ? "auto" : "user supplied")

	show(touchscreen_variant);
	show(touchscreen_width);
	show(touchscreen_height);
	show(touchscreen_invert_x);
	show(touchscreen_invert_y);
	show(touchscreen_swap_x_y);
	dev_info(data->dev, "touchscreen_fw_name %s (%s)\n",
		 data->touchscreen_fw_name,
		 (touchscreen_fw_name == NULL) ? "auto" : "user supplied");
#undef show
}

static void q8_hardwaremgr_apply_touchscreen(struct q8_hardwaremgr_data *data)
{
	struct of_changeset cset;
	struct device_node *np;

	switch (data->touchscreen.model) {
	case touchscreen_unknown:
		return;
	case gsl1680_a082:
		q8_hardwaremgr_apply_gsl1680_a082_variant(data);
		break;
	case gsl1680_b482:
		q8_hardwaremgr_apply_gsl1680_b482_variant(data);
		break;
	case ektf2127:
	case zet6251:
		/* These have only 1 variant */
		break;
	}

	if (touchscreen_width != -1)
		data->touchscreen_width = touchscreen_width;

	if (touchscreen_height != -1)
		data->touchscreen_height = touchscreen_height;

	if (touchscreen_invert_x != -1)
		data->touchscreen_invert_x = touchscreen_invert_x;

	if (touchscreen_invert_y != -1)
		data->touchscreen_invert_y = touchscreen_invert_y;

	if (touchscreen_swap_x_y != -1)
		data->touchscreen_swap_x_y = touchscreen_swap_x_y;

	if (touchscreen_fw_name)
		data->touchscreen_fw_name = touchscreen_fw_name;

	if (data->touchscreen.model == gsl1680_a082 ||
	    data->touchscreen.model == gsl1680_b482)
		q8_hardwaremgr_issue_gsl1680_warning(data);

	np = q8_hardware_mgr_apply_common(&data->touchscreen, &cset,
					  "touchscreen");
	if (!np)
		return;

	if (data->touchscreen_width)
		of_changeset_add_property_u32(&cset, np, "touchscreen-size-x",
					      data->touchscreen_width);
	if (data->touchscreen_height)
		of_changeset_add_property_u32(&cset, np, "touchscreen-size-y",
					      data->touchscreen_height);
	if (data->touchscreen_invert_x)
		of_changeset_add_property_bool(&cset, np,
					       "touchscreen-inverted-x");
	if (data->touchscreen_invert_y)
		of_changeset_add_property_bool(&cset, np,
					       "touchscreen-inverted-y");
	if (data->touchscreen_swap_x_y)
		of_changeset_add_property_bool(&cset, np,
					       "touchscreen-swapped-x-y");
	if (data->touchscreen_fw_name)
		of_changeset_add_property_string(&cset, np, "firmware-name",
						 data->touchscreen_fw_name);

	of_changeset_apply(&cset);
	of_changeset_destroy(&cset);
	of_node_put(np);
}

static int q8_hardwaremgr_probe_rda599x(struct q8_hardwaremgr_data *data,
					struct i2c_client *client)
{
	int id;

	/*
	 * Check for the (integrated) rda580x / rda5820 fm receiver at 0x11
	 *
	 * Alternatively we could check for the wifi_rf i2c interface at
	 * address 0x14, by selecting page/bank 1 through:
	 * smbus_write_word_swapped(0x3f, 0x01)
	 * and then doing a smbus_read_word_swapped(0x20) which will
	 * return 0x5990 for a rda5990. We prefer the fm detect method since
	 * we want to avoid doing any smbus_writes while probing.
	 */
	id = i2c_smbus_read_word_swapped(client, 0x0c);
	if (id == 0x5802 || id == 0x5803 || id == 0x5805 || id == 0x5820)
		data->has_rda599x = true;

	return id == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;
}

static int q8_hardwaremgr_probe_mxc6225(struct q8_hardwaremgr_data *data,
					struct i2c_client *client)
{
	int id;

	id = i2c_smbus_read_byte_data(client, MXC6225_REG_CHIP_ID);
	/* Bits 7 - 5 of the chip-id register are undefined */
	if (id >= 0 && (id & 0x1f) == MXC6225_CHIP_ID) {
		data->accelerometer.compatible = "memsic,mxc6225";
		data->accelerometer.model = mxc6225;
		return 0;
	}

	return id == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;
}

static int q8_hardwaremgr_probe_mc3230(struct q8_hardwaremgr_data *data,
				       struct i2c_client *client)
{
	int id, pc = 0;

	/* First check chip-id, then product-id */
	id = i2c_smbus_read_byte_data(client, MC3230_REG_CHIP_ID);
	if (id == MC3230_CHIP_ID || id == MMA7660_CHIP_ID) {
		pc = i2c_smbus_read_byte_data(client, MC3230_REG_PRODUCT_CODE);
		switch (pc) {
		case MMA7660_PRODUCT_CODE:
			data->accelerometer.compatible = "fsl,mma7660";
			data->accelerometer.model = mma7660;
			return 0;
		case MC3210_PRODUCT_CODE:
			data->accelerometer.compatible = "mcube,mc3210";
			data->accelerometer.model = mc3210;
			return 0;
		case MC3230_PRODUCT_CODE:
			data->accelerometer.compatible = "mcube,mc3230";
			data->accelerometer.model = mc3230;
			return 0;
		case -ETIMEDOUT:
			return -ETIMEDOUT; /* Bus stuck bail immediately */
		}
	}

	return id == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;
}

static int q8_hardwaremgr_probe_dmard06(struct q8_hardwaremgr_data *data,
					struct i2c_client *client)
{
	int id;

	id = i2c_smbus_read_byte_data(client, DMARD06_CHIP_ID_REG);
	switch (id) {
	case DMARD05_CHIP_ID:
		data->accelerometer.compatible = "domintech,dmard05";
		data->accelerometer.model = dmard05;
		return 0;
	case DMARD06_CHIP_ID:
		data->accelerometer.compatible = "domintech,dmard06";
		data->accelerometer.model = dmard06;
		return 0;
	case DMARD07_CHIP_ID:
		data->accelerometer.compatible = "domintech,dmard07";
		data->accelerometer.model = dmard07;
		return 0;
	}

	return id == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;
}

static int q8_hardwaremgr_probe_dmard09(struct q8_hardwaremgr_data *data,
					struct i2c_client *client)
{
	int id;

	id = i2c_smbus_read_byte_data(client, DMARD09_REG_CHIPID);
	if (id == DMARD09_CHIPID) {
		data->accelerometer.compatible = "domintech,dmard09";
		data->accelerometer.model = dmard09;
		return 0;
	}

	return id == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;
}

static int q8_hardwaremgr_probe_dmard10(struct q8_hardwaremgr_data *data,
					struct i2c_client *client)
{
	int stadr, staint = 0;

	/* These 2 registers have special POR reset values used for id */
	stadr = i2c_smbus_read_byte_data(client, DMARD10_REG_STADR);
	if (stadr == DMARD10_VALUE_STADR) {
		staint = i2c_smbus_read_byte_data(client, DMARD10_REG_STAINT);
		switch (staint) {
		case DMARD10_VALUE_STAINT:
			data->accelerometer.compatible = "domintech,dmard10";
			data->accelerometer.model = dmard10;
			return 0;
		case -ETIMEDOUT:
			return -ETIMEDOUT; /* Bus stuck bail immediately */
		}
	}

	return stadr == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;
}

static int q8_hardwaremgr_probe_da280(struct q8_hardwaremgr_data *data,
				      struct i2c_client *client)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, DA280_REG_CHIP_ID);
	if (ret != DA280_CHIP_ID)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	/* da226 (2-axis) or da280 (3-axis) ? measure once to detect */
	ret = i2c_smbus_write_byte_data(client, DA280_REG_MODE_BW,
					DA280_MODE_ENABLE);
	if (ret)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	msleep(10);

	ret = i2c_smbus_read_word_data(client, DA280_REG_ACC_Z_LSB);
	if (ret < 0)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	/* If not present Z reports max pos value (14 bits, 2 low bits 0) */
	if (ret == 32764) {
		data->accelerometer.compatible = "miramems,da226";
		data->accelerometer.model = da226;
	} else {
		data->accelerometer.compatible = "miramems,da280";
		data->accelerometer.model = da280;
	}

	ret = i2c_smbus_write_byte_data(client, DA280_REG_MODE_BW,
					DA280_MODE_DISABLE);
	if (ret)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	return 0;
}

static int q8_hardwaremgr_probe_da311(struct q8_hardwaremgr_data *data,
				      struct i2c_client *client)
{
	int id;

	id = i2c_smbus_read_byte_data(client, DA311_REG_CHIP_ID);
	if (id == DA311_CHIP_ID) {
		data->accelerometer.compatible = "miramems,da311";
		data->accelerometer.model = da311;
		return 0;
	}

	return id == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;
}

static int q8_hardwaremgr_probe_accelerometer(struct q8_hardwaremgr_data *data,
					      struct i2c_adapter *adap)
{
	/* The rda599x wifi/bt/fm shares the i2c bus with the accelerometer */
	PROBE_CLIENT(NULL,		   0x11, q8_hardwaremgr_probe_rda599x);
	PROBE_CLIENT(&data->accelerometer, 0x15, q8_hardwaremgr_probe_mxc6225);
	PROBE_CLIENT(&data->accelerometer, 0x4c, q8_hardwaremgr_probe_mc3230);
	PROBE_CLIENT(&data->accelerometer, 0x1c, q8_hardwaremgr_probe_dmard06);
	PROBE_CLIENT(&data->accelerometer, 0x1d, q8_hardwaremgr_probe_dmard09);
	PROBE_CLIENT(&data->accelerometer, 0x18, q8_hardwaremgr_probe_dmard10);
	PROBE_CLIENT(&data->accelerometer, 0x26, q8_hardwaremgr_probe_da280);
	PROBE_CLIENT(&data->accelerometer, 0x27, q8_hardwaremgr_probe_da280);
	PROBE_CLIENT(&data->accelerometer, 0x27, q8_hardwaremgr_probe_da311);

	return -ENODEV;
}

static void q8_hardwaremgr_apply_accelerometer(struct q8_hardwaremgr_data *data)
{
	struct of_changeset cset;
	struct device_node *np;

	if (data->accelerometer.model == accel_unknown)
		return;

	np = q8_hardware_mgr_apply_common(&data->accelerometer, &cset,
					  "accelerometer");
	if (!np)
		return;

	of_changeset_apply(&cset);
	of_changeset_destroy(&cset);
	of_node_put(np);
}

static void q8_hardwaremgr_apply_quirks(struct q8_hardwaremgr_data *data)
{
	struct device_node *np;
	struct of_changeset cset;

	/* This A33 tzx-723q4 PCB tablet with esp8089 needs crystal_26M_en=1 */
	if (data->soc == a33 && data->touchscreen.model == gsl1680_b482 &&
	    data->accelerometer.model == dmard09 && !data->has_rda599x) {
		dev_info(data->dev, "Applying crystal_26M_en=1 sdio_wifi quirk\n");
		np = of_find_node_by_name(of_root, "sdio_wifi");
		if (!np) {
			dev_warn(data->dev, "Could not find sdio_wifi dt node\n");
			return;
		}
		of_changeset_init(&cset);
		of_changeset_add_property_u32(&cset, np,
					      "esp,crystal-26M-en", 1);
		of_changeset_apply(&cset);
		of_changeset_destroy(&cset);
		of_node_put(np);
	}
}

static int q8_hardwaremgr_do_probe(struct q8_hardwaremgr_data *data,
				   struct q8_hardwaremgr_device *dev,
				   const char *prefix, bus_probe_func func)
{
	struct device_node *np;
	struct pinctrl *pinctrl;
	struct i2c_adapter *adap;
	struct regulator *reg;
	struct gpio_desc *gpio;
	
	int ret = 0;

	np = of_find_node_by_name(of_root, prefix);
	if (!np) {
		dev_err(data->dev, "Error %s node is missing\n", prefix);
		return -EINVAL;
	}

	/*
	 * Patch the dt_node into our device since there is no device for
	 * the probed hw yet (status = disabled) .
	 */
	data->dev->of_node = np;

	pinctrl = pinctrl_get(data->dev);
	if (IS_ERR(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		if (ret == -EPROBE_DEFER)
			goto put_node;
		pinctrl = NULL;
	}

	if (pinctrl) {	
		struct pinctrl_state *state = 
			pinctrl_lookup_state(pinctrl, PINCTRL_STATE_DEFAULT);
		if (!IS_ERR(state)) {
			ret = pinctrl_select_state(pinctrl, state);
			if (ret == -EPROBE_DEFER)
				goto put_pinctrl;
		}
	}

	adap = of_get_i2c_adapter_by_node(np->parent);
	if (!adap) {
		ret = -EPROBE_DEFER;
		goto put_pinctrl;
	}

	reg = regulator_get_optional(data->dev, "vddio");
	if (IS_ERR(reg)) {
		ret = PTR_ERR(reg);
		if (ret == -EPROBE_DEFER)
			goto put_adapter;
		reg = NULL;
	}

	gpio = fwnode_get_named_gpiod(&np->fwnode, "power-gpios");
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		if (ret == -EPROBE_DEFER)
			goto put_reg;
		gpio = NULL;
	}

	/* First try with only the power gpio driven high */
	if (gpio) {
		ret = gpiod_direction_output(gpio, 1);
		if (ret)
			goto put_gpio;
	}

	dev_info(data->dev, "Probing %s without a regulator\n", prefix);
	ret = func(data, adap);
	if (ret != 0 && reg) {
		/* Second try, also enable the regulator */
		ret = regulator_enable(reg);
		if (ret)
			goto restore_gpio;

		dev_info(data->dev, "Probing %s with a regulator\n", prefix);
		ret = func(data, adap);

#if 0 /* 4.9 silead driver lacks regulator support, leave it enabled */
		regulator_disable(reg);
#endif
	} else if (reg)
		dev->delete_regulator = true; /* Regulator not needed */

	if (ret == 0)
		dev_info(data->dev, "Found %s at 0x%02x\n",
			 dev->compatible, dev->addr);
	else
		ret = 0; /* Not finding a device is not an error */

restore_gpio:
	if (gpio)
		gpiod_direction_output(gpio, 0);
put_gpio:
	if (gpio)
		gpiod_put(gpio);
put_reg:
	if (reg)
		regulator_put(reg);
put_adapter:
	i2c_put_adapter(adap);

put_pinctrl:
	if (pinctrl)
		pinctrl_put(pinctrl);
put_node:
	data->dev->of_node = NULL;
	of_node_put(np);

	return ret;
}

/*
 * sun5i-a13-q8-tablet.dts on kernel 4.8 is missing the touchscreen
 * template node, add it.
 */
static int q8_hardwaremgr_add_touchscreen_node(struct q8_hardwaremgr_data *data)
{
	/* FIXME */
	return 0;
}

/*
 * Originally the q8-hwmgr design was for the dt to already have a template
 * touchscreen node on the right i2c bus, but since q8-hwmgr is not (yet?)
 * upstream that node may be missing or incomplete
 */
static int q8_hardwaremgr_fixup_touchscreen_node(
	struct q8_hardwaremgr_data *data)
{
	struct device_node *ts_np, *reg_np = NULL;
	struct property *prop;
	struct of_changeset cset;
	struct regulator *reg;
	int ret = 0;

	if (data->soc == a13)
		return q8_hardwaremgr_add_touchscreen_node(data);

	ts_np = of_find_node_by_name(of_root, "touchscreen");
	reg_np = of_find_node_by_name(of_root, "ldo_io1");
	if (!ts_np || !reg_np) {
		dev_err(data->dev,
			"Error dt-nodes missing touchscreen %p, ldo_io1 %p\n",
			ts_np, reg_np);
		ret = -EINVAL;
		goto out_put_np;
	}

	reg = regulator_get_optional(NULL, "ldo_io1");
	if (IS_ERR(reg)) {
		ret = PTR_ERR(reg);
		if (ret != -EPROBE_DEFER)
			dev_err(data->dev, "Error could not get ldo_io1 regulator %d\n",
				ret);
		goto out_put_np;
	}

	if (!reg->rdev->dev.of_node) {
		reg->rdev->dev.of_node = reg_np;
	} else if (reg->rdev->dev.of_node != reg_np) {
		dev_err(data->dev, "Error ldo_io1 dt-node mismatch\n");
		goto out_put_reg;
	}

	/* See if the constraints are already set */
	prop = of_find_property(reg_np, "regulator-min-microvolt", NULL);
	if (prop)
		goto add_touchscreen_supply_prop;

	of_changeset_init(&cset);
	of_changeset_add_property_u32(&cset, reg_np, "regulator-min-microvolt",
				      3300000);
	of_changeset_add_property_u32(&cset, reg_np, "regulator-max-microvolt",
				      3300000);
	of_changeset_update_property_string(&cset, reg_np, "regulator-name", "vcc-touchscreen");
	of_changeset_update_property_string(&cset, reg_np, "status", "okay");
	of_changeset_apply(&cset);
	of_changeset_destroy(&cset);

	/*
	 * HACK HACK HACK update the constraints after the regulator core
	 * has already set them from the incomplete dt.
	 */
	reg->rdev->constraints->min_uV = 3300000;
	reg->rdev->constraints->max_uV = 3300000;
	reg->rdev->constraints->name = kstrdup("vcc-touchscreen", GFP_KERNEL);
	reg->rdev->constraints->valid_ops_mask |=
		REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_STATUS;

	ret = regulator_set_voltage(reg, 3300000, 3300000);
	if (ret) {
		dev_err(data->dev, "Error setting ldo_io1 voltage %d\n", ret);
		goto out_put_reg;
	}

add_touchscreen_supply_prop:
	/* See if the supply are already set */
	prop = of_find_property(ts_np, "vddio-supply", NULL);
	if (prop)
		goto out_put_reg;

	/* The reg_np may not have a phandle */
	if (!reg_np->phandle)
		reg_np->phandle = of_gen_phandle();

	of_changeset_init(&cset);
	/* The regulator phandle has no args */
	of_changeset_add_property_u32(&cset, ts_np, "vddio-supply",
				      reg_np->phandle);
	of_changeset_apply(&cset);
	of_changeset_destroy(&cset);

out_put_reg:
	regulator_put(reg);
out_put_np:
	of_node_put(reg_np);
	of_node_put(ts_np);
	return ret;
}

/*
 * Originally the q8-hwmgr design was for the dt to already have an empty
 * accel node on the right i2c bus, but since q8-hwmgr is not (yet?) upstream
 * that node may be missing.
 */
static int q8_hardwaremgr_add_accel_node(struct q8_hardwaremgr_data *data)
{
	struct device_node *np, *parent;
	struct of_changeset cset;
	int ret = 0;

	np = of_find_node_by_name(of_root, "accelerometer");
	if (np) {
		of_node_put(np);
		return 0; /* accelerometer node already exists */
	}

	parent = of_find_node_by_path("/soc@01c00000/i2c@01c2b000");
	if (!parent) {
		dev_err(data->dev, "Error i2c1 node is missing\n");
		return -EINVAL;
	}

	of_changeset_init(&cset);
	np = of_changeset_create_device_node(&cset, parent, "accelerometer");
	if (IS_ERR(np)) {
		ret = PTR_ERR(np);
		goto out;
	}
	of_changeset_add_property_string(&cset, np, "name", "accelerometer");
	of_changeset_add_property_string(&cset, np, "status", "disabled");
	of_changeset_attach_node(&cset, np);
	of_changeset_apply(&cset);
out:
	of_changeset_destroy(&cset);
	of_node_put(parent);
	return ret;
}

static int q8_hardwaremgr_probe(struct platform_device *pdev)
{
	struct q8_hardwaremgr_data *data;
	int ret = 0;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = &pdev->dev;
	data->soc = (long)pdev->dev.platform_data;

	ret = q8_hardwaremgr_fixup_touchscreen_node(data);
	if (ret)
		goto error;

	ret = q8_hardwaremgr_add_accel_node(data);
	if (ret)
		goto error;

	ret = q8_hardwaremgr_do_probe(data, &data->touchscreen, "touchscreen",
				      q8_hardwaremgr_probe_touchscreen);
	if (ret)
		goto error;

	ret = q8_hardwaremgr_do_probe(data, &data->accelerometer,
				      "accelerometer",
				      q8_hardwaremgr_probe_accelerometer);
	if (ret)
		goto error;

	if (data->has_rda599x)
		dev_info(data->dev, "Found a rda599x sdio/i2c wifi/bt/fm combo chip\n");

	q8_hardwaremgr_apply_touchscreen(data);
	q8_hardwaremgr_apply_accelerometer(data);
	q8_hardwaremgr_apply_quirks(data);

error:
	kfree(data);

	return ret;
}

static int q8_hardwaremgr_remove(struct platform_device *pdev)
{
	/* Nothing todo */
	return 0;
}

static struct platform_driver q8_hardwaremgr_driver = {
	.driver = {
		.name	= "q8-hwmgr",
	},
	.probe	= q8_hardwaremgr_probe,
	.remove = q8_hardwaremgr_remove,
};

static int __init q8_hardwaremgr_init(void)
{
	struct platform_device *pdev;
	enum soc soc;
	int ret;

	if (of_machine_is_compatible("allwinner,q8-a13"))
		soc = a13;
	else if (of_machine_is_compatible("allwinner,q8-a23"))
		soc = a23;
	else if (of_machine_is_compatible("allwinner,q8-a33"))
		soc = a33;
	else
		return 0;

	pdev = platform_device_alloc("q8-hwmgr", 0);
	if (!pdev)
		return -ENOMEM;

	pdev->dev.platform_data = (void *)(long)soc;

	ret = platform_device_add(pdev);
	if (ret)
		return ret;

	return platform_driver_register(&q8_hardwaremgr_driver);
}

device_initcall(q8_hardwaremgr_init);

MODULE_DESCRIPTION("Allwinner q8 formfactor tablet hardware manager");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL");
