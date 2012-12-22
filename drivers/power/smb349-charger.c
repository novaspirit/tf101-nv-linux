/*
 * drivers/power/smb349-charger.c
 *
 * Battery charger driver for smb349 from summit microelectronics
 *
 * Copyright (c) 2012, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/smb349-charger.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/usb/otg.h>

#define SMB349_CHARGE		0x00
#define SMB349_CHRG_CRNTS	0x01
#define SMB349_VRS_FUNC		0x02
#define SMB349_FLOAT_VLTG	0x03
#define SMB349_CHRG_CTRL	0x04
#define SMB349_STAT_TIME_CTRL	0x05
#define SMB349_PIN_CTRL		0x06
#define SMB349_THERM_CTRL	0x07
#define SMB349_CTRL_REG		0x09

#define SMB349_OTG_TLIM_REG	0x0A
#define SMB349_HRD_SFT_TEMP	0x0B
#define SMB349_FAULT_INTR	0x0C
#define SMB349_STS_INTR_1	0x0D
#define SMB349_SYSOK_USB3	0x0E
#define SMB349_IN_CLTG_DET	0x10
#define SMB349_STS_INTR_2	0x11

#define SMB349_CMD_REG		0x30
#define SMB349_CMD_REG_B	0x31
#define SMB349_CMD_REG_c	0x33

#define SMB349_INTR_STS_A	0x35
#define SMB349_INTR_STS_B	0x36
#define SMB349_INTR_STS_C	0x37
#define SMB349_INTR_STS_D	0x38
#define SMB349_INTR_STS_E	0x39
#define SMB349_INTR_STS_F	0x3A

#define SMB349_STS_REG_A	0x3B
#define SMB349_STS_REG_B	0x3C
#define SMB349_STS_REG_C	0x3D
#define SMB349_STS_REG_D	0x3E
#define SMB349_STS_REG_E	0x3F

#define SMB349_ENABLE_WRITE	1
#define SMB349_DISABLE_WRITE	0
#define ENABLE_WRT_ACCESS	0x80
#define THERM_CTRL		0x10
#define BATTERY_MISSING		0x10
#define CHARGING		0x06
#define DEDICATED_CHARGER	0x04
#define CHRG_DOWNSTRM_PORT	0x08
#define ENABLE_CHARGE		0x02

static struct smb349_charger *charger;
static int smb349_configure_charger(struct i2c_client *client, int value);
static int smb349_configure_interrupts(struct i2c_client *client);

static int smb349_read(struct i2c_client *client, int reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, reg);

	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	return ret;
}

static int smb349_write(struct i2c_client *client, int reg, u8 value)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, reg, value);

	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	return ret;
}

static int smb349_update_reg(struct i2c_client *client, int reg, u8 value)
{
	int ret, retval;

	retval = smb349_read(client, reg);
	if (retval < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, retval);
		return retval;
	}

	ret = smb349_write(client, reg, retval | value);
	if (ret < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		return ret;
	}

	return ret;
}

int smb349_volatile_writes(struct i2c_client *client, uint8_t value)
{
	int ret = 0;

	if (value == SMB349_ENABLE_WRITE) {
		/* Enable volatile write to config registers */
		ret = smb349_update_reg(client, SMB349_CMD_REG,
						ENABLE_WRT_ACCESS);
		if (ret < 0) {
			dev_err(&client->dev, "%s(): Failed in writing"
				"register 0x%02x\n", __func__, SMB349_CMD_REG);
			return ret;
		}
	} else {
		ret = smb349_read(client, SMB349_CMD_REG);
		if (ret < 0) {
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
			return ret;
		}

		ret = smb349_write(client, SMB349_CMD_REG, ret & (~(1<<7)));
		if (ret < 0) {
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
			return ret;
		}
	}
	return ret;
}

static void smb349_clear_interrupts(struct i2c_client *client)
{
	uint8_t val, buf[6];

	val = i2c_smbus_read_i2c_block_data(client, SMB349_INTR_STS_A, 6, buf);
	if (val < 0)
		dev_err(&client->dev, "%s(): Failed in clearing interrupts\n",
								__func__);
}

static int smb349_configure_otg(struct i2c_client *client, int enable)
{
	int ret = 0;

	/*Enable volatile writes to registers*/
	ret = smb349_volatile_writes(client, SMB349_ENABLE_WRITE);
	if (ret < 0) {
		dev_err(&client->dev, "%s error in configuring otg..\n",
								__func__);
		goto error;
	}

	if (enable) {
		/* Enable OTG */
	       ret = smb349_update_reg(client, SMB349_CMD_REG, 0x10);
	       if (ret < 0) {
		       dev_err(&client->dev, "%s: Failed in writing register"
				"0x%02x\n", __func__, SMB349_CMD_REG);
			goto error;
	       }

	} else {
	       /* Disable OTG */
	       ret = smb349_read(client, SMB349_CMD_REG);
	       if (ret < 0) {
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
			goto error;
	       }

	       ret = smb349_write(client, SMB349_CMD_REG, (ret & (~(1<<4))));
	       if (ret < 0) {
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
			goto error;
	       }
	}

	/* Disable volatile writes to registers */
	ret = smb349_volatile_writes(client, SMB349_DISABLE_WRITE);
	if (ret < 0) {
		dev_err(&client->dev, "%s error in configuring OTG..\n",
								__func__);
	       goto error;
	}
error:
	return ret;
}

static int smb349_configure_charger(struct i2c_client *client, int value)
{
	int ret = 0;

	/* Enable volatile writes to registers */
	ret = smb349_volatile_writes(client, SMB349_ENABLE_WRITE);
	if (ret < 0) {
		dev_err(&client->dev, "%s() error in configuring charger..\n",
								__func__);
		goto error;
	}

	if (value) {
		 /* Enable charging */
		ret = smb349_update_reg(client, SMB349_CMD_REG, ENABLE_CHARGE);
		if (ret < 0) {
			dev_err(&client->dev, "%s(): Failed in writing register"
					"0x%02x\n", __func__, SMB349_CMD_REG);
			goto error;
		}

		/* Configure THERM ctrl */
		ret = smb349_update_reg(client, SMB349_THERM_CTRL, THERM_CTRL);
		if (ret < 0) {
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
			goto error;
		}
	} else {
		ret = smb349_read(client, SMB349_CMD_REG);
		if (ret < 0) {
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
			goto error;
		}

		ret = smb349_write(client, SMB349_CMD_REG, (ret & (~(1<<1))));
		if (ret < 0) {
			dev_err(&client->dev, "%s: err %d\n", __func__, ret);
			goto error;
		}
	}
	/* Disable volatile writes to registers */
	ret = smb349_volatile_writes(client, SMB349_DISABLE_WRITE);
	if (ret < 0) {
		dev_err(&client->dev, "%s() error in configuring charger..\n",
								__func__);
		goto error;
	}
error:
	return ret;
}

static irqreturn_t smb349_status_isr(int irq, void *dev_id)
{
	struct i2c_client *client = charger->client;
	int ret, val;

	val =  smb349_read(client, SMB349_STS_REG_D);
	if (val < 0) {
		dev_err(&client->dev, "%s(): Failed in reading register"
				"0x%02x\n", __func__, SMB349_STS_REG_D);
		goto irq_error;
	} else if (val != 0) {
		if (val & DEDICATED_CHARGER)
			charger->chrg_type = AC;
		else if (val & CHRG_DOWNSTRM_PORT)
			charger->chrg_type = USB;

		/* configure charger */
		ret = smb349_configure_charger(client, 1);
		if (ret < 0) {
			dev_err(&client->dev, "%s() error in configuring"
				"charger..\n", __func__);
			goto irq_error;
		}

		charger->state = progress;
	} else {
		charger->state = stopped;

		/* Disable charger */
		ret = smb349_configure_charger(client, 0);
		if (ret < 0) {
			dev_err(&client->dev, "%s() error in configuring"
				"charger..\n", __func__);
			goto irq_error;
		}

		ret = smb349_configure_interrupts(client);
		if (ret < 0) {
			dev_err(&client->dev, "%s() error in configuring"
				"charger..\n", __func__);
			goto irq_error;
		}

	}

	if (charger->charger_cb)
		charger->charger_cb(charger->state, charger->chrg_type,
						charger->charger_cb_data);
irq_error:
	smb349_clear_interrupts(client);
	return IRQ_HANDLED;
}

int register_callback(charging_callback_t cb, void *args)
{
	struct smb349_charger *charger_data = charger;
	if (!charger_data)
		return -ENODEV;

	charger_data->charger_cb = cb;
	charger_data->charger_cb_data = args;
	return 0;
}
EXPORT_SYMBOL_GPL(register_callback);

int smb349_battery_online(void)
{
	int val;
	struct i2c_client *client = charger->client;

	val = smb349_read(charger->client, SMB349_INTR_STS_B);
	if (val < 0) {
		dev_err(&client->dev, "%s(): Failed in reading register"
				"0x%02x\n", __func__, SMB349_INTR_STS_B);
		return val;
	}
	if (val & BATTERY_MISSING)
		return 0;
	else
		return 1;
}

static int smb349_configure_interrupts(struct i2c_client *client)
{
	int ret = 0;

	/* Enable volatile writes to registers */
	ret = smb349_volatile_writes(client, SMB349_ENABLE_WRITE);
	if (ret < 0) {
		dev_err(&client->dev, "%s() error in configuring charger..\n",
								__func__);
		goto error;
	}

	ret = smb349_update_reg(client, SMB349_FAULT_INTR, 0xff);
	if (ret < 0) {
		dev_err(&client->dev, "%s(): Failed in writing register"
				"0x%02x\n", __func__, SMB349_CMD_REG);
		goto error;
	}

	ret = smb349_update_reg(client, SMB349_STS_INTR_1, 0xff);
	if (ret < 0) {
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);
		goto error;
	}

	 /* Disable volatile writes to registers */
	ret = smb349_volatile_writes(client, SMB349_DISABLE_WRITE);
	if (ret < 0) {
		dev_err(&client->dev, "%s() error in configuring charger..\n",
								__func__);
		goto error;
	}

error:
	return ret;
}

static void smb349_otg_status(enum usb_otg_state otg_state, void *data)
{
	struct i2c_client *client = charger->client;
	int ret;

	if (otg_state == OTG_STATE_A_HOST) {

		/* configure charger */
		ret = smb349_configure_charger(client, 0);
		if (ret < 0)
			dev_err(&client->dev, "%s() error in configuring"
				"otg..\n", __func__);

		/* ENABLE OTG */
		ret = smb349_configure_otg(client, 1);
		if (ret < 0)
			dev_err(&client->dev, "%s() error in configuring"
				"otg..\n", __func__);

	} else if (otg_state == OTG_STATE_A_SUSPEND) {

		/* Disable OTG */
		ret = smb349_configure_otg(client, 0);
		if (ret < 0)
			dev_err(&client->dev, "%s() error in configuring"
				"otg..\n", __func__);

		/* configure charger */
		ret = smb349_configure_charger(client, 1);
		if (ret < 0)
			dev_err(&client->dev, "%s() error in configuring"
				"otg..\n", __func__);

		ret = smb349_configure_interrupts(client);
		if (ret < 0)
			dev_err(&client->dev, "%s() error in configuring"
						"otg..\n", __func__);
	}
}

static int __devinit smb349_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	int ret, irq_num;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	charger = kzalloc(sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	charger->client = client;
	charger->dev = &client->dev;
	i2c_set_clientdata(client, charger);

	/* Check battery presence */
	if (!smb349_battery_online()) {
		dev_err(&client->dev, "%s() No Battery present, exiting..\n",
					__func__);
		goto error;
	}

	ret = register_otg_callback(smb349_otg_status, charger);
	if (ret < 0)
		goto error;

	ret = smb349_configure_charger(client, 1);
	if (ret < 0)
		return ret;

	ret = smb349_configure_interrupts(client);
	if (ret < 0) {
		dev_err(&client->dev, "%s() error in configuring charger..\n",
								__func__);
		goto error;
	}

	irq_num = gpio_to_irq(client->irq);
	ret = request_threaded_irq(irq_num,
			NULL, smb349_status_isr, IRQ_TYPE_EDGE_FALLING,
				"smb349", charger);
	if (ret) {
		dev_err(&client->dev, "%s(): Failed in requesting isr\n",
				__func__);
		goto error;
	}
	return 0;
error:
	kfree(charger);
	return ret;
}

static int __devexit smb349_remove(struct i2c_client *client)
{
	struct smb349_charger *charger = i2c_get_clientdata(client);

	kfree(charger);
	return 0;
}

static const struct i2c_device_id smb349_id[] = {
	{ "smb349", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, smb349_id);

static struct i2c_driver smb349_i2c_driver = {
	.driver	= {
		.name	= "smb349",
	},
	.probe		= smb349_probe,
	.remove		= __devexit_p(smb349_remove),
	.id_table	= smb349_id,
};

static int __init smb349_init(void)
{
	return i2c_add_driver(&smb349_i2c_driver);
}
module_init(smb349_init);

static void __exit smb349_exit(void)
{
	i2c_del_driver(&smb349_i2c_driver);
}
module_exit(smb349_exit);

MODULE_AUTHOR("Syed Rafiuddin <srafiuddin@nvidia.com>");
MODULE_DESCRIPTION("SMB349 Battery-Charger");
MODULE_LICENSE("GPL");
