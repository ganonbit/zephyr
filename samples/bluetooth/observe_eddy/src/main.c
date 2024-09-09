#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>

#define MAX_EXT_ADV_DATA_LEN 1650
#define MAX_RETRIES          3
#define RETRY_DELAY_MS       100
K_SEM_DEFINE(bt_init_ok, 0, 1);

enum device_type {
	DEVICE_TYPE_IBEACON,
	DEVICE_TYPE_EDDYSTONE
};

struct device_info {
	bt_addr_le_t addr;
	int8_t rssi;
	uint8_t type;
	enum device_type beacon_type;
	union {
		struct {
			uint16_t battery_voltage;
			int16_t beacon_temperature;
		} eddystone;
		struct {
			uint16_t major;
			uint16_t minor;
			int8_t measured_power;
		} ibeacon;
	} data;
};

static struct bt_le_ext_adv *adv;

static bool parse_beacon_data(struct net_buf_simple *ad, struct device_info *info)
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
			info->beacon_type = DEVICE_TYPE_EDDYSTONE;
			info->data.eddystone.battery_voltage = (data[4] << 8) | data[5];
			info->data.eddystone.beacon_temperature = ((int16_t)data[6] << 8) | data[7];
			return true;
		} else if (data_type == BT_DATA_MANUFACTURER_DATA && len >= 25 && data[0] == 0x4C &&
			   data[1] == 0x00) {
			info->beacon_type = DEVICE_TYPE_IBEACON;
			info->data.ibeacon.major = (data[20] << 8) | data[21];
			info->data.ibeacon.minor = (data[22] << 8) | data[23];
			info->data.ibeacon.measured_power = (int8_t)data[24];
			return true;
		}
	}
	return false;
}

static bool is_valid_beacon(struct net_buf_simple *ad)
{
	struct net_buf_simple ad_copy;
	net_buf_simple_clone(ad, &ad_copy);

	struct device_info temp_info;
	return parse_beacon_data(&ad_copy, &temp_info);
}

static int create_adv_param(void)
{
	int err;

	struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_USE_NAME,
				     BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

	err = bt_le_ext_adv_create(&param, NULL, &adv);
	if (err) {
		printk("Failed to create advertising set (err %d)\n", err);
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
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(&device->addr, addr_str, sizeof(addr_str));

	printk("Debug: Sending advertising data for device: %s\n", addr_str);

	if (!adv) {
		printk("Error: Advertising set not created. Creating now.\n");
		err = create_adv_param();
		if (err) {
			printk("Failed to create advertising set (err %d)\n", err);
			return;
		}
	}

	if (!bt_is_ready()) {
		printk("Error: Bluetooth stack not ready. Reinitializing.\n");
		err = bt_enable(NULL);
		if (err) {
			printk("Bluetooth init failed (err %d)\n", err);
			return;
		}
		k_sem_take(&bt_init_ok, K_FOREVER);
	}

	uint8_t ad_data[31];
	struct bt_data ad;

	if (device->beacon_type == DEVICE_TYPE_EDDYSTONE) {
		uint8_t *eddystone_data = ad_data;
		eddystone_data[0] = 0xAA;
		eddystone_data[1] = 0xFE;
		eddystone_data[2] = 0x20;
		eddystone_data[3] = 0x00;
		eddystone_data[4] = device->data.eddystone.battery_voltage >> 8;
		eddystone_data[5] = device->data.eddystone.battery_voltage & 0xFF;
		eddystone_data[6] = device->data.eddystone.beacon_temperature >> 8;
		eddystone_data[7] = device->data.eddystone.beacon_temperature & 0xFF;
		memset(&eddystone_data[8], 0, 6);

		ad.type = BT_DATA_SVC_DATA16;
		ad.data_len = 14;
		ad.data = eddystone_data;
	} else if (device->beacon_type == DEVICE_TYPE_IBEACON) {
		ad_data[0] = 0x1A; // Length of iBeacon data
		ad_data[1] = BT_DATA_MANUFACTURER_DATA;
		ad_data[2] = 0x4C; // Apple company ID
		ad_data[3] = 0x00;
		ad_data[4] = 0x02;         // iBeacon type
		ad_data[5] = 0x15;         // iBeacon length
		memset(&ad_data[6], 0, 4); // Shortened UUID (4 bytes instead of 16)
		ad_data[10] = device->data.ibeacon.major >> 8;
		ad_data[11] = device->data.ibeacon.major & 0xFF;
		ad_data[12] = device->data.ibeacon.minor >> 8;
		ad_data[13] = device->data.ibeacon.minor & 0xFF;
		ad_data[14] = device->data.ibeacon.measured_power;
		ad_data[15] = device->rssi; // Include RSSI

		ad.type = BT_DATA_MANUFACTURER_DATA;
		ad.data_len = 14;
		ad.data = ad_data;
	} else {
		printk("Invalid beacon type, skipping advertisement\n");
		return;
	}

	printk("Debug: Setting advertising data\n");
	err = bt_le_ext_adv_set_data(adv, &ad, 1, NULL, 0);
	if (err) {
		printk("Failed to set advertising data (err %d)\n", err);
		return;
	}

	printk("Debug: Attempting to start extended advertising\n");
	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_PARAM(1000, 0));
	if (err) {
		printk("Failed to start extended advertising (err %d)\n", err);
		return;
	}
	printk("Extended advertising started successfully\n");

	int retries = 0;
	while (retries < MAX_RETRIES) {
		err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_PARAM(1000, 0));
		if (err) {
			if (err == -ENOBUFS) {
				printk("Debug: No buffer space available (err %d), retrying...\n",
				       err);
				retries++;
				k_msleep(RETRY_DELAY_MS);
			} else {
				printk("Failed to start extended advertising (err %d)\n", err);
				return;
			}
		} else {
			printk("Extended advertising started successfully\n");
			return;
		}
	}

	if (retries == MAX_RETRIES) {
		printk("Failed to start extended advertising after %d retries\n", MAX_RETRIES);
	}
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	if (is_valid_beacon(ad)) {
		struct device_info device;
		char addr_str[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		printk("Debug: Found new beacon device: %s\n", addr_str);

		memcpy(&device.addr, addr, sizeof(bt_addr_le_t));
		device.rssi = rssi;
		device.type = type;

		struct net_buf_simple ad_copy;
		net_buf_simple_clone(ad, &ad_copy);
		parse_beacon_data(&ad_copy, &device);

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
		printk("Failed to create advertising set (err %d)\n", err);
		return err;
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
