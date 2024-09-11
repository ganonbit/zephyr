#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>

#define MAX_RETRIES          3
#define RETRY_DELAY_MS       100
#define ADV_DURATION_MS      1000
#define MAX_EXT_ADV_DATA_LEN 254
#define MAX_BEACONS_PER_ADV  5 // Reduced from 10 to 5

static struct bt_le_ext_adv *adv;
static struct k_work_delayable shutdown_work;
static bool shutdown_requested = false;

enum device_type {
	DEVICE_TYPE_IBEACON,
	DEVICE_TYPE_EDDYSTONE
};

struct device_info {
	bt_addr_le_t addr;
	int8_t rssi;
	uint8_t beacon_data[10]; // Adjust size as needed
	uint32_t timestamp;
	enum device_type type;
};

static struct device_info beacon_queue[MAX_BEACONS_PER_ADV];
static int beacon_count = 0;

static bool parse_beacon_data(struct net_buf_simple *ad, struct device_info *info)
{
	uint8_t len, data_type;
	const uint8_t *data;

	printk("Debug: Parsing beacon data\n");

	while (ad->len > 1) {
		len = net_buf_simple_pull_u8(ad);
		if (len == 0) {
			break;
		}
		data_type = net_buf_simple_pull_u8(ad);
		data = net_buf_simple_pull_mem(ad, len - 1);

		printk("Debug: AD type: 0x%02x, length: %d\n", data_type, len);

		if (data_type == BT_DATA_SVC_DATA16 && len >= 14 && data[0] == 0xAA &&
		    data[1] == 0xFE && data[2] == 0x20) {
			info->type = DEVICE_TYPE_EDDYSTONE;
			memcpy(info->beacon_data, data, MIN(len - 1, sizeof(info->beacon_data)));
			printk("Debug: Eddystone beacon found\n");
			return true;
		} else if (data_type == BT_DATA_MANUFACTURER_DATA && len >= 25 && data[0] == 0x4C &&
			   data[1] == 0x00) {
			info->type = DEVICE_TYPE_IBEACON;
			memcpy(info->beacon_data, data, MIN(len - 1, sizeof(info->beacon_data)));
			printk("Debug: iBeacon found\n");
			return true;
		}
	}
	printk("Debug: No valid beacon data found\n");
	return false;
}

static int create_adv_param(void)
{
	printk("Debug: Creating advertising parameters\n");

	struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_USE_NAME,
				     BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

	int err = bt_le_ext_adv_create(&param, NULL, &adv);
	if (err) {
		printk("Failed to create advertising set (err %d)\n", err);
		return err;
	}

	printk("Advertising set created successfully\n");
	return 0;
}

static void send_adv_data(void)
{
	int err;
	uint8_t ad_data[191]; // Set to the maximum allowed size
	struct bt_data ad;
	uint8_t *ptr = ad_data;

	printk("Debug: Preparing extended advertising data\n");

	// Add a custom header to identify our data
	*ptr++ = 0xFF;         // Manufacturer Specific Data
	*ptr++ = 0x59;         // Nordic Semiconductor's Company ID (LSB)
	*ptr++ = 0x00;         // Nordic Semiconductor's Company ID (MSB)
	*ptr++ = beacon_count; // Number of beacons in this packet

	int beacons_to_send = MIN(beacon_count, MAX_BEACONS_PER_ADV);
	for (int i = 0; i < beacons_to_send; i++) {
		struct device_info *device = &beacon_queue[i];

		// Check if we have enough space for this beacon's data
		if ((ptr - ad_data) + 22 >
		    sizeof(ad_data)) { // 6 (MAC) + 1 (RSSI) + 10 (data) + 4 (timestamp) + 1 (type)
			printk("Debug: Not enough space for beacon %d, stopping\n", i);
			beacons_to_send = i;
			break;
		}

		// Add MAC address (6 bytes)
		memcpy(ptr, device->addr.a.val, 6);
		ptr += 6;

		// Add RSSI (1 byte)
		*ptr++ = device->rssi;

		// Add beacon data (10 bytes)
		memcpy(ptr, device->beacon_data, 10);
		ptr += 10;

		// Add timestamp (4 bytes)
		memcpy(ptr, &device->timestamp, sizeof(uint32_t));
		ptr += sizeof(uint32_t);

		// Add beacon type (1 byte)
		*ptr++ = device->type;
	}

	ad.type = BT_DATA_MANUFACTURER_DATA;
	ad.data_len = ptr - ad_data;
	ad.data = ad_data;

	printk("Debug: Setting extended advertising data, length: %d\n", ad.data_len);
	err = bt_le_ext_adv_set_data(adv, &ad, 1, NULL, 0);
	if (err) {
		printk("Failed to set advertising data (err %d)\n", err);
		return;
	}

	int retries = 0;
	while (retries < MAX_RETRIES) {
		printk("Debug: Starting extended advertising, attempt %d\n", retries + 1);
		err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_PARAM(ADV_DURATION_MS, 0));
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
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("Debug: Device found: %s, RSSI: %d, type: %u\n", addr_str, rssi, type);

	struct device_info device;
	struct net_buf_simple ad_copy;

	net_buf_simple_clone(ad, &ad_copy);
	if (parse_beacon_data(&ad_copy, &device)) {
		printk("Debug: Valid beacon found: %s\n", addr_str);

		memcpy(&device.addr, addr, sizeof(bt_addr_le_t));
		device.rssi = rssi;
		device.timestamp = k_uptime_get_32();

		if (beacon_count < MAX_BEACONS_PER_ADV) {
			memcpy(&beacon_queue[beacon_count], &device, sizeof(struct device_info));
			beacon_count++;
			printk("Debug: Added beacon to queue. Total beacons: %d\n", beacon_count);

			if (beacon_count == MAX_BEACONS_PER_ADV) {
				send_adv_data();
				beacon_count = 0;
			}
		} else {
			printk("Debug: Beacon queue full, sending data\n");
			send_adv_data();
			// Move remaining beacons to the start of the queue
			memmove(beacon_queue, &beacon_queue[MAX_BEACONS_PER_ADV],
				(beacon_count - MAX_BEACONS_PER_ADV) * sizeof(struct device_info));
			beacon_count -= MAX_BEACONS_PER_ADV;
			memcpy(&beacon_queue[beacon_count], &device, sizeof(struct device_info));
			beacon_count++;
		}
	} else {
		printk("Debug: Not a valid beacon: %s\n", addr_str);
	}
}

static int observer_start(void)
{
	printk("Debug: Starting observer\n");

	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	int err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		printk("Start scanning failed (err %d)\n", err);
		return err;
	}
	printk("Started scanning...\n");

	return 0;
}

static void cleanup_advertising(void)
{
	printk("Debug: Cleaning up advertising\n");
	if (adv) {
		bt_le_ext_adv_stop(adv);
		bt_le_ext_adv_delete(adv);
		adv = NULL;
	}
}

static void shutdown_work_handler(struct k_work *work)
{
	printk("Shutting down...\n");
	bt_le_scan_stop();
	cleanup_advertising();
	bt_disable();
	printk("Shutdown complete.\n");
}

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	err = create_adv_param();
	if (err) {
		printk("Failed to create advertising parameters (err %d)\n", err);
		return;
	}

	err = observer_start();
	if (err) {
		printk("Observer start failed (err %d)\n", err);
	}
}

int main(void)
{
	printk("Starting Observer Demo\n");

	k_work_init_delayable(&shutdown_work, shutdown_work_handler);

	int err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth stack initialized\n");

	while (!shutdown_requested) {
		k_sleep(K_SECONDS(1));
		printk("Debug: Main loop running\n");
	}

	k_work_schedule(&shutdown_work, K_NO_WAIT);
	k_sleep(K_SECONDS(2)); // Give some time for the shutdown work to complete

	return 0;
}
