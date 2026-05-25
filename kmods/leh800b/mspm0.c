// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/swab.h>

#define DRIVER_NAME "mspm0"

/*
 * The formula of 12-bit value (N) from ADC reading is
 * N = (2^n-1) * (V_in-0.5LSB) / V_ref
 * where LSB = V_ref / (2^n), n = 12
 * V_ref is based on the SMB schematic, 3.3 * 10 / 13.2 = 2.5V
 * Use V_in(N) = 2500 * N / 2^n approximate for SAR core
 */
#define ADC_RESOLUTION_BIT 12U
#define ADC_V_REF 2500U
#define ADC_VIN(N) (ADC_V_REF * (N) / (1U << ADC_RESOLUTION_BIT))

/*
 * Number of ADC channels (Registers 0x00 to 0x15)
 * CH4(A0_4) is not exposed on the register design.
 * 0x0a <-> CH11(A0_11) is occupied for other usage.
 * 0x15 <-> CH29 is a temp sensor that is not used in this driver.
 */
#define MSPM0_NUM_CHANNELS 22
#define MSPM0_ADC_SKIPPED_CH 4
#define MSPM0_ADC_UNUSED_CH 12

static const int mspm0_adc_reg_map[] = {
	0x00, 0x01, 0x02, 0x03,
	  -1, 0x04, 0x05, 0x06,
	0x07, 0x08, 0x09, 0x0a,
	0x0b, 0x0c, 0x0d, 0x0e,
	0x0f, 0x10, 0x11, 0x12,
	0x13, 0x14,
};

static int mspm0_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct i2c_client *client = dev_get_drvdata(dev);
	int ret;
	int reg;
	u16 val_raw;

	if (type != hwmon_in || attr != hwmon_in_input)
		return -EOPNOTSUPP;

	/*
	 * A0_4 and A0_12 should not be used.
	 */
	switch (channel) {
	case MSPM0_ADC_SKIPPED_CH:
	case MSPM0_ADC_UNUSED_CH:
		return -EINVAL;
	}

	if (channel < 0 || channel > MSPM0_NUM_CHANNELS)
		return -EINVAL;

	reg = mspm0_adc_reg_map[channel];

	/*
	 * PEC is handled automatically by the I2C adapter since we
	 * set I2C_CLIENT_PEC in the probe function.
	 */
	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		return ret;

	/* Swap bytes to change endianess */
	val_raw = swab16((u16)ret);
	/* Filter out hardware glitches */
	if (unlikely(val_raw > (1<<ADC_RESOLUTION_BIT) - 1))
		val_raw = (1<<ADC_RESOLUTION_BIT) - 1;

	/* Apply the requested formula and change to voltage value. */
	*val = ADC_VIN((long)val_raw);

	return 0;
}

static umode_t mspm0_is_visible(const void *data,
				enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	switch (channel) {
	case MSPM0_ADC_SKIPPED_CH:
	case MSPM0_ADC_UNUSED_CH:
		return 0;
	}

	if (type == hwmon_in && attr == hwmon_in_input)
		return 0444; /* Read-only permissions */

	return 0;
}

static const struct hwmon_ops mspm0_hwmon_ops = {
	.is_visible = mspm0_is_visible,
	.read = mspm0_read,
};

/*
 * Declare 22 input channels.
 * This creates sysfs entries from in0_input to in21_input
 * and skips in4 and in12.
 */
static const struct hwmon_channel_info *mspm0_info[] = {
	HWMON_CHANNEL_INFO(in,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT,
		HWMON_I_INPUT, HWMON_I_INPUT),
	NULL
};

static const struct hwmon_chip_info mspm0_chip_info = {
	.ops = &mspm0_hwmon_ops,
	.info = mspm0_info,
};

static int mspm0_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device *hwmon_dev;

	/* * Check if the I2C adapter supports SMBus Word Data reads and PEC.
	 */
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_PEC)) {
		dev_err(dev, "I2C adapter doesn't support SMBus Word Read with PEC\n");
		return -ENODEV;
	}

	/* Force Packet Error Checking (PEC) for all SMBus transactions */
	client->flags |= I2C_CLIENT_PEC;

	/* * Register the hwmon device. We pass 'client' as drvdata so we can
	 * retrieve it easily in the mspm0_read() callback.
	 */
	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
							 client,
							 &mspm0_chip_info,
							 NULL);

	if (IS_ERR(hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(hwmon_dev),
				     "Failed to register hwmon device\n");

	return 0;
}

/* Standard I2C Matching */
static const struct i2c_device_id mspm0_id[] = {
	{ DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mspm0_id);

static struct i2c_driver mspm0_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name = DRIVER_NAME,
	},
	.probe		= mspm0_probe,
	.id_table	= mspm0_id,
};

module_i2c_driver(mspm0_driver);

MODULE_AUTHOR("Evan Zong <ezong@celestica.com>");
MODULE_DESCRIPTION("Hwmon driver for TI MSPM0 voltage monitor");
MODULE_LICENSE("GPL");
