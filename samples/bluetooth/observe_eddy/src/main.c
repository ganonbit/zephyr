/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>

#define MAX_EXT_ADV_DATA_LEN  1650
K_SEM_DEFINE(bt_init_ok, 0, 1);

struct eddystone_tlm_frame {
	uint16_t battery_voltage;
	int16_t beacon_temperature;
};

struct device_info {
	bt_addr_le_t addr;
	int8_t rssi;
	uint8_t type;
	struct eddystone_tlm_frame tlm;
};

static bool parse_eddystone_tlm(struct net_buf_simple *ad, struct eddystone_tlm_frame *tlm)
{
	uint8_t len, data_type;
	const uint8_t *data;

	while (ad->len > 1) {
		len = net_buf_simple_pull_u8(ad);
		if (len == 0) {
			break;
		}
		data_type = net_buf_simple_pull_u8(ad);
		data = net_buf_simple_pull_mem(ad, len - 1);

		if (data_type == BT_DATA_SVC_DATA16 && len >= 14 && data[0] == 0xAA &&
		    data[1] == 0xFE && data[2] == 0x20) {
			printk("Debug: TLM Frame: ");
			for (int i = 0; i < 14; i++) {
				printk("%02x ", data[i]);
			}
			printk("\n");

			if (tlm != NULL) {
				tlm->battery_voltage = (data[4] << 8) | data[5];
				tlm->beacon_temperature = ((int16_t)data[6] << 8) | data[7];
				printk("Debug: TLM Data - Battery: %u mV, Temp: %d.%02d°C\n",
				       tlm->battery_voltage, tlm->beacon_temperature / 256,
				       (tlm->beacon_temperature % 256) * 100 / 256);
			}
			return true;
		}
	}
	return false;
}

static bool is_eddystone_tlm(struct net_buf_simple *ad)
{
	struct net_buf_simple ad_copy;
	net_buf_simple_clone(ad, &ad_copy);

	uint8_t len, type;
	const uint8_t *data;

	while (ad_copy.len > 1) {
		len = net_buf_simple_pull_u8(&ad_copy);
		if (len == 0) {
			break;
		}
		type = net_buf_simple_pull_u8(&ad_copy);
		data = net_buf_simple_pull_mem(&ad_copy, len - 1);

		if (type == BT_DATA_SVC_DATA16 && len >= 14 && data[0] == 0xAA && data[1] == 0xFE &&
		    data[2] == 0x20) {
			printk("Debug: Eddystone TLM frame received in is_eddystone_tlm\n");
			return true;
		}
	}
	return false;
}

static struct bt_le_ext_adv *adv;

static int create_adv_param(void)
{
	int err;

	struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_USE_NAME,
				     BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

	err = bt_le_ext_adv_create(&param, NULL, &adv);
	if (err) {
		printk("Failed to create advertising set line 133 (err %d)\n", err);
		return err;
	}

	printk("Advertising set created successfully\n");

	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_PARAM(1000, 0));
	if (err) {
		printk("Failed to start advertising (err %d)\n", err);
		return err;
	}

	printk("Advertising started for 5 seconds\n");
	return 0;
}

static void send_adv_data(struct device_info *device)
{
	int err;
	struct bt_le_ext_adv_info info;
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(&device->addr, addr_str, sizeof(addr_str));

	printk("Debug: Sending advertising data for device: %s\n", addr_str);
	printk("Debug: RSSI: %d, Battery Voltage: %u mV, Temperature: %d.%02d°C\n", device->rssi,
	       device->tlm.battery_voltage, device->tlm.beacon_temperature / 256,
	       (device->tlm.beacon_temperature % 256) * 100 / 256);

	// Step 2: Verify advertising set creation
	if (!adv) {
		printk("Error: Advertising set not created. Creating now.\n");
		err = create_adv_param();
		if (err) {
			printk("Failed to create advertising set line 159 (err %d)\n", err);
			return;
		}
	}

	// Step 5: Verify Bluetooth stack state
	if (!bt_is_ready()) {
		printk("Error: Bluetooth stack not ready. Reinitializing.\n");
		err = bt_enable(NULL);
		if (err) {
			printk("Bluetooth init failed (err %d)\n", err);
			return;
		}
		k_sem_take(&bt_init_ok, K_FOREVER);
	}

	uint8_t eddystone_data[14] = {0xAA,
				      0xFE,
				      0x20,
				      0x00,
				      device->tlm.battery_voltage >> 8,
				      device->tlm.battery_voltage & 0xFF,
				      device->tlm.beacon_temperature >> 8,
				      device->tlm.beacon_temperature & 0xFF,
				      0x00,
				      0x00,
				      0x00,
				      0x00,
				      0x00,
				      0x00};

	// Check if Eddystone data is valid
	if (device->tlm.battery_voltage == 0 && device->tlm.beacon_temperature == 0) {
		printk("Invalid Eddystone data, skipping advertisement\n");
		return;
	}

	struct bt_data ad[] = {
		BT_DATA(BT_DATA_SVC_DATA16, eddystone_data, sizeof(eddystone_data)),
	};

	// Check advertising data format
	printk("Debug: Checking advertising data format\n");
	for (int i = 0; i < ARRAY_SIZE(ad); i++) {
		printk("  AD[%d]: type=%u, data_len=%u\n", i, ad[i].type, ad[i].data_len);
		printk("  Data: ");
		for (int j = 0; j < ad[i].data_len; j++) {
			printk("%02x ", ((uint8_t *)ad[i].data)[j]);
		}
		printk("\n");
		if (ad[i].data_len > 31) {
			printk("Error: AD data length exceeds 31 bytes\n");
		}
	}

	// Verify advertising set state

	err = bt_le_ext_adv_get_info(adv, &info);
	if (err) {
		printk("Error: Failed to get advertising set info line 212 (err %d)\n", err);
	} else {
		printk("Debug: Advertising set info - TX power: %d\n", info.tx_power);
	}

	// Detailed logging for bt_le_ext_adv_set_data call
	printk("Debug: Calling bt_le_ext_adv_set_data\n");
	printk("  adv: %p\n", (void *)adv);
	printk("  ad: %p\n", (void *)ad);
	printk("  ad_len: %zu\n", ARRAY_SIZE(ad));
	printk("  sd: NULL\n");
	printk("  sd_len: 0\n");

	err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err) {
		printk("Error: Failed to set advertising data line 228 (err %d)\n", err);
		printk("Debug: Failed advertising data:\n");
		for (int i = 0; i < ARRAY_SIZE(ad); i++) {
			printk("  AD[%d]: type=%u, data_len=%u\n", i, ad[i].type, ad[i].data_len);
			printk("  Data: ");
			for (int j = 0; j < ad[i].data_len; j++) {
				printk("%02x ", ((uint8_t *)ad[i].data)[j]);
			}
			printk("\n");
		}

		// Retry setting advertising data
		printk("Debug: Retrying to set advertising data\n");
		err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);
		if (err) {
			printk("Error: Failed to set advertising data on retry (err %d)\n", err);
		} else {
			printk("Debug: Advertising data set successfully on retry\n");
		}
	} else {
		printk("Debug: Advertising data set successfully\n");
	}

	printk("Debug: Starting extended advertising\n");
	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_PARAM(3000, 0));
	if (err) {
		printk("Failed to start extended advertising (err %d)\n", err);
		return;
	}
	printk("Extended advertising started successfully\n");

	// Step 7: Check for resource exhaustion
	err = bt_le_ext_adv_get_info(adv, &info);
	if (err) {
		printk("Failed to get advertising set info line 264 (err %d)\n", err);
	} else {
		printk("Debug: Advertising set info - TX power: %d\n", info.tx_power);
		printk("Extended advertising active\n");
		for (int i = 0; i < ARRAY_SIZE(ad); i++) {
			printk("  Data: ");
			for (int j = 0; j < ad[i].data_len; j++) {
				printk("%02x ", ((uint8_t *)ad[i].data)[j]);
			}
			printk("\n");
		}
	}
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	if (is_eddystone_tlm(ad)) {

		struct device_info device;
		char addr_str[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		printk("Debug: Found new Eddystone device: %s\n", addr_str);

		memcpy(&device.addr, addr, sizeof(bt_addr_le_t));
		device.rssi = rssi;
		device.type = type;

		struct net_buf_simple ad_copy;
		net_buf_simple_clone(ad, &ad_copy);
		parse_eddystone_tlm(&ad_copy, &device.tlm);

		send_adv_data(&device);
	}
}

static int observer_start(void)
{
	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err;

	err = create_adv_param();
	if (err) {
		return err;
		printk("Failed to create advertising set line 305 (err %d)\n", err);
	}

	err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		printk("Start scanning failed (err %d)\n", err);
		return err;
	}
	printk("Started scanning...\n");

	return 0;
}
static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	err = observer_start();
	if (err) {
		printk("Observer start failed (err %d)\n", err);
	}
}

static void cleanup_advertising(void)
{
	if (adv) {
		bt_le_ext_adv_stop(adv);
		bt_le_ext_adv_delete(adv);
		adv = NULL;
	}
}

int main(void)
{

	// Rest of your main function code...
	printk("Starting Observer Demo\n");

	int err;

	/* Initialize the Bluetooth Subsystem */
	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	/* Wait for the Bluetooth stack to be ready */
	k_sem_take(&bt_init_ok, K_FOREVER);

	printk("Bluetooth stack initialized\n");

	cleanup_advertising();

	printk("Exiting %s thread.\n", __func__);
	return 0;
}
