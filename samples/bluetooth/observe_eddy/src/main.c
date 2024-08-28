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

#define MAX_EDDYSTONE_DEVICES 100
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

static struct device_info eddystone_devices[MAX_EDDYSTONE_DEVICES];
static int eddystone_count = 0;

static uint16_t calculate_ad_size(const struct bt_data *ad, size_t ad_len)
{
	uint16_t total_size = 0;
	for (size_t i = 0; i < ad_len; i++) {
		total_size += ad[i].data_len + 2; // +2 for type and length fields
	}
	return total_size;
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
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY,
				     BT_GAP_ADV_FAST_INT_MIN_1, BT_GAP_ADV_FAST_INT_MAX_1, NULL);

	err = bt_le_ext_adv_create(&param, NULL, &adv);
	if (err) {
		printk("Failed to create advertising set (err %d)\n", err);
		return err;
	}

	return 0;
}

static void send_adv_data(struct device_info *device)
{
	int err;
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(&device->addr, addr_str, sizeof(addr_str));

	printk("Debug: Sending advertising data for device: %s\n", addr_str);
	printk("Debug: device->rssi = %d\n", device->rssi);
	printk("Debug: device->tlm.battery_voltage = %u\n", device->tlm.battery_voltage);
	printk("Debug: device->tlm.beacon_temperature = %d\n", device->tlm.beacon_temperature);

	uint8_t eddystone_data[14] = {
		0xAA,
		0xFE, // Eddystone UUID
		0x20, // TLM frame type
		0x00, // TLM version
		device->tlm.battery_voltage >> 8,
		device->tlm.battery_voltage & 0xFF,
		device->tlm.beacon_temperature >> 8,
		device->tlm.beacon_temperature & 0xFF,
		0x00,
		0x00,
		0x00,
		0x00, // Advertising PDU count (not used)
		0x00,
		0x00 // Time since power-on or reboot (not used)
	};

	struct bt_data ad[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
			sizeof(CONFIG_BT_DEVICE_NAME) - 1),
		BT_DATA(BT_DATA_SVC_DATA16, eddystone_data, sizeof(eddystone_data)),
	};

	uint16_t ad_size = calculate_ad_size(ad, ARRAY_SIZE(ad));

	if (ad_size > MAX_EXT_ADV_DATA_LEN) {
		printk("Advertising data size exceeds maximum allowed size\n");
		return;
	}

	printk("Total advertising data size: %u\n", ad_size);

	printk("Debug: Advertising data:\n");
	for (int i = 0; i < ARRAY_SIZE(ad); i++) {
		printk("  Type: 0x%02x, Length: %u\n", ad[i].type, ad[i].data_len);
		printk("  Data: ");
		for (int j = 0; j < ad[i].data_len; j++) {
			printk("%02x ", ((uint8_t *)ad[i].data)[j]);
		}
		printk("\n");
	}

	err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printk("Failed to set advertising data (err %d)\n", err);
		return;
	}

	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err) {
		printk("Failed to start extended advertising (err %d)\n", err);
		return;
	}
	printk("Extended advertising started successfully\n");

	// k_sleep(K_SECONDS(10));

	// Add a periodic check to confirm advertising is still running
	for (int i = 0; i < 5; i++) {
		k_sleep(K_SECONDS(2));
		printk("Extended advertising still active - iteration %d\n", i + 1);
	}

	err = bt_le_ext_adv_stop(adv);
	if (err) {
		printk("Failed to stop extended advertising (err %d)\n", err);
	} else {
		printk("Extended advertising stopped successfully\n");
	}
}

static bool device_exists(const bt_addr_le_t *addr)
{
	for (int i = 0; i < eddystone_count; i++) {
		if (bt_addr_le_cmp(addr, &eddystone_devices[i].addr) == 0) {
			return true;
		}
	}
	return false;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	if (is_eddystone_tlm(ad)) {
		if (!device_exists(addr) && eddystone_count < MAX_EDDYSTONE_DEVICES) {
			if (ad->len == 0) {
				printk("Warning: Empty advertising data received\n");
				return;
			}
			memcpy(&eddystone_devices[eddystone_count].addr, addr,
			       sizeof(bt_addr_le_t));

			eddystone_devices[eddystone_count].rssi = rssi;
			eddystone_devices[eddystone_count].type = type;

			// Extract TLM frame data
			uint8_t len, data_type;
			const uint8_t *data;

			while (ad->len > 1) {
				len = net_buf_simple_pull_u8(ad);
				if (len == 0) {
					break;
				}
				data_type = net_buf_simple_pull_u8(ad);
				data = net_buf_simple_pull_mem(ad, len - 1);

				if (data_type == BT_DATA_SVC_DATA16 && len >= 14 &&
				    data[0] == 0xAA && data[1] == 0xFE && data[2] == 0x20) {
					struct eddystone_tlm_frame *tlm =
						&eddystone_devices[eddystone_count].tlm;
					tlm->battery_voltage = (data[4] << 8) | data[5];
					tlm->beacon_temperature = ((int16_t)data[6] << 8) | data[7];

					printk("TLM Frame:\n");
					printk("  Battery Voltage: %u mV\n", tlm->battery_voltage);
					printk("  Beacon Temperature: %d.%02d°C\n",
					       tlm->beacon_temperature / 256,
					       (tlm->beacon_temperature % 256) * 100 / 256);
				}
			}

			eddystone_count++;
			send_adv_data(&eddystone_devices[eddystone_count - 1]);
		} else {
			printk("Duplicate or max devices reached. Not adding device.\n");
		}
	}
}

static void process_devices(void)
{
	for (int i = 0; i < eddystone_count; i++) {
		send_adv_data(&eddystone_devices[i]);
	}
	eddystone_count = 0;
}

static int observer_start(void)
{
	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err;

	err = create_adv_param();
	if (err) {
		return err;
	}

	err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		printk("Start scanning failed (err %d)\n", err);
		return err;
	}
	printk("Started scanning...\n");

	while (1) {
		k_sleep(K_SECONDS(1));
		process_devices();
	}

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

int main(void)
{
	int err;

	printk("Starting Observer Demo\n");

	/* Initialize the Bluetooth Subsystem */
	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	/* Wait for the Bluetooth stack to be ready */
	k_sem_take(&bt_init_ok, K_FOREVER);

	printk("Bluetooth stack initialized\n");

	printk("Exiting %s thread.\n", __func__);
	return 0;
}
