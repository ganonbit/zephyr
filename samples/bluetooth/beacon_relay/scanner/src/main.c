#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>

#define BEACON_DATA_SIZE      20
#define MAX_BEACONS           5
#define RELAY_IDENTIFIER_SIZE 2

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

	if (type == BT_GAP_ADV_TYPE_EXT_ADV) {
		printk("Extended Advertisement found: %s (RSSI %d)\n", addr_str, rssi);
	} else {
		printk("Legacy Advertisement found: %s (RSSI %d)\n", addr_str, rssi);
	}

	if (ad->len > 31) {
		printk("Extended AD data: %d bytes\n", ad->len);
	} else {
		printk("AD data: %d bytes\n", ad->len);
	}

	// Parse relay packets
	uint8_t *data = ad->data;
	uint8_t data_len = ad->len;

	while (data_len > 1) {
		uint8_t len = data[0];
		uint8_t type = data[1];

		if (type == BT_DATA_MANUFACTURER_DATA && len >= RELAY_IDENTIFIER_SIZE + 2) {
			if (data[2] == 0x43 && data[3] == 0x52) { // 'C' 'R' for Croxel Relay
				printk("Relay packet found\n");
				uint8_t *relay_data = &data[4];
				uint8_t relay_data_len = len - RELAY_IDENTIFIER_SIZE;

				for (int i = 0; i < relay_data_len;
				     i += (6 + 1 + BEACON_DATA_SIZE)) {
					if (i + 6 + 1 + BEACON_DATA_SIZE > relay_data_len) {
						break;
					}

					bt_addr_le_t beacon_addr;
					memcpy(beacon_addr.a.val, &relay_data[i], 6);
					beacon_addr.type =
						BT_ADDR_LE_RANDOM; // Assume random address type

					int8_t beacon_rssi = relay_data[i + 6];
					uint8_t *beacon_data = &relay_data[i + 7];

					char beacon_addr_str[BT_ADDR_LE_STR_LEN];
					bt_addr_le_to_str(&beacon_addr, beacon_addr_str,
							  sizeof(beacon_addr_str));

					printk("Relayed beacon: %s (RSSI %d)\n", beacon_addr_str,
					       beacon_rssi);

					// Print beacon data as hexdump
					printk("Beacon data: ");
					for (int j = 0; j < BEACON_DATA_SIZE; j++) {
						printk("%02X ", beacon_data[j]);
					}
					printk("\n");

					// Additional parsing for iBeacon and Eddystone
					if (beacon_data[0] == 0x02 && beacon_data[1] == 0x15) {
						// This is likely an iBeacon
						printk("iBeacon data:\n");
						printk("  UUID: ");
						for (int j = 2; j < 18; j++) {
							printk("%02X", beacon_data[j]);
						}
						printk("\n");
						uint16_t major =
							(beacon_data[18] << 8) | beacon_data[19];
						uint16_t minor =
							(beacon_data[20] << 8) | beacon_data[21];
						int8_t measured_power = (int8_t)beacon_data[22];
						printk("  Major: %u\n", major);
						printk("  Minor: %u\n", minor);
						printk("  Measured Power: %d\n", measured_power);
					} else if (beacon_data[0] == 0xAA &&
						   beacon_data[1] == 0xFE) {
						// This is likely an Eddystone beacon
						printk("Eddystone data:\n");
						printk("  Frame type: %02X\n", beacon_data[2]);
						// Further parsing depends on the frame type
					}
				}
			}
		}

		data += len + 1;
		data_len -= len + 1;
	}
}

static void scan_start(void)
{
	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err;

	err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		printk("Starting scanning failed (err %d)\n", err);
		return;
	}

	printk("Scanning successfully started\n");
}

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	scan_start();
}

int main(void)
{
	int err;

	printk("Starting Extended Advertising Scanner\n");

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	return 0;
}
