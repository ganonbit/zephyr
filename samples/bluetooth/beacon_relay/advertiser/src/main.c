#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define MAX_EXT_ADV_DATA_LEN 191
#define MAX_ADV_SETS         2
#define MAX_BEACONS          100
#define MAX_BEACONS_PER_SET  24
#define BEACON_BATCH_SIZE    3
#define BEACON_DATA_SIZE     7
#define ADV_DURATION_MS      2000
#define MAX_WAIT_TIME_MS     3000
#define RECOVERY_TIMEOUT_MS  5000
#define TEST_DEVICE_ADDR     {0xF6, 0xE5, 0xD4, 0xC3, 0xB2, 0xA1}
#define TIME_THRESHOLD       5000

// Data Structures
struct beacon_info {
	bt_addr_le_t addr;
	int8_t rssi;
	uint32_t last_seen;
	bool is_valid;
};

// Static Variables
static struct bt_le_ext_adv *adv_sets[MAX_ADV_SETS];
static struct beacon_info beacon_queue[MAX_BEACONS];
static atomic_t beacon_count = ATOMIC_INIT(0);
static atomic_t active_adv_sets = ATOMIC_INIT(0);
static ATOMIC_DEFINE(adv_set_active_bitfield, MAX_ADV_SETS);
static atomic_t beacons_since_last_check = ATOMIC_INIT(0);
static atomic_t last_successful_operation = ATOMIC_INIT(0);
static struct k_work_delayable adv_work;
static uint32_t last_send_time = 0;
static uint8_t ad_data[MAX_EXT_ADV_DATA_LEN];

// Function Declarations
static int find_or_update_beacon(const bt_addr_le_t *addr, int8_t rssi);
static void add_beacon(const bt_addr_le_t *addr, int8_t rssi);
static int send_adv_data(void);
static void check_and_send(void);
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad);
static void adv_work_handler(struct k_work *work);
static int create_adv_param(struct bt_le_ext_adv **adv);
static int observer_start(void);
static void bt_ready(int err);
static void recover_from_hang(void);

// Beacon Management
static int find_or_update_beacon(const bt_addr_le_t *addr, int8_t rssi)
{
	uint32_t current_time = k_uptime_get_32();
	int empty_slot = -1;

	for (int i = 0; i < MAX_BEACONS; i++) {
		if (!beacon_queue[i].is_valid) {
			if (empty_slot == -1) {
				empty_slot = i;
			}
			continue;
		}

		if (bt_addr_le_cmp(&beacon_queue[i].addr, addr) == 0) {
			// Update existing beacon with highest RSSI and latest timestamp
			if (rssi > beacon_queue[i].rssi) {
				beacon_queue[i].rssi = rssi;
			}
			beacon_queue[i].last_seen = current_time;
			return i;
		}
	}

	// Add new beacon if space available
	if (empty_slot != -1) {
		memcpy(&beacon_queue[empty_slot].addr, addr, sizeof(bt_addr_le_t));
		beacon_queue[empty_slot].rssi = rssi;
		beacon_queue[empty_slot].last_seen = current_time;
		beacon_queue[empty_slot].is_valid = true;
		atomic_inc(&beacon_count);
		return empty_slot;
	}

	return -1; // Queue is full
}

static void add_beacon(const bt_addr_le_t *addr, int8_t rssi)
{
	int index = find_or_update_beacon(addr, rssi);
	if (index >= 0) {
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		printk("Beacon updated/added: Address: %s, RSSI: %d, Index: %d\n", addr_str, rssi,
		       index);
		printk("Total unique beacons: %ld\n", (long)atomic_get(&beacon_count));

		if (IS_ENABLED(CONFIG_DEBUG)) {
			printk("Beacon queue status:\n");
			for (int i = 0, count = 0; i < MAX_BEACONS && count < 5; i++) {
				if (beacon_queue[i].is_valid) {
					bt_addr_le_to_str(&beacon_queue[i].addr, addr_str,
							  sizeof(addr_str));
					printk("  [%d] Address: %s, RSSI: %d, Last seen: %u\n", i,
					       addr_str, beacon_queue[i].rssi,
					       beacon_queue[i].last_seen);
					count++;
				}
			}
			if (atomic_get(&beacon_count) > 5) {
				printk("  ... (showing only first 5 entries)\n");
			}
		}
	}
}

// Bluetooth Device Discovery Callback
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	if (IS_ENABLED(CONFIG_DEBUG)) {
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		printk("Device found: %s, RSSI: %d\n", addr_str, rssi);
	}

	add_beacon(addr, rssi);

	if (atomic_inc(&beacons_since_last_check) >= BEACON_BATCH_SIZE) {
		printk("Calling check_and_send after adding beacon batch\n");
		check_and_send();
		atomic_set(&beacons_since_last_check, 0);
	}
}

// Advertising Data Preparation and Sending
static int send_adv_data(void)
{
	printk("Entering send_adv_data\n");
	int err;
	uint8_t *ptr = ad_data;
	uint8_t *end = ad_data + MAX_EXT_ADV_DATA_LEN;

	*ptr++ = 0x59;                                     // Company ID (LSB)
	*ptr++ = 0x00;                                     // Company ID (MSB)
	*ptr++ = 0x08;                                     // First byte of manufacturer data
	*ptr++ = (uint8_t)atomic_get(&beacon_count);       // Sequence number

	printk("Company ID bytes: 0x%02X 0x%02X\n", *(ptr - 4), *(ptr - 3));
	printk("First byte of manufacturer data: 0x%02X\n", *(ptr - 2));
	printk("Sequence number: 0x%02X\n", *(ptr - 1));

	printk("First 10 bytes of ad_data: ");
	for (int i = 0; i < 10 && i < (ptr - ad_data); i++) {
		printk("0x%02X ", ad_data[i]);
	}
	printk("\n");

	int beacons_sent = 0;
	bool test_device_added = false;
	uint32_t current_time = k_uptime_get_32();

	int set_to_use = -1;
	for (int i = 0; i < MAX_ADV_SETS; i++) {
		if (!(atomic_get(adv_set_active_bitfield) & BIT(i))) {
			set_to_use = i;
			break;
		}
	}

	if (set_to_use == -1) {
		printk("No inactive advertising sets available\n");
		return -EBUSY;
	}

	// Add test device
	if ((end - ptr) >= BEACON_DATA_SIZE) {
		uint8_t test_addr[] = TEST_DEVICE_ADDR;
		memcpy(ptr, test_addr, 6);
		ptr += 6;
		*ptr++ = (int8_t)(sys_rand32_get() % 51 - 50); // Random RSSI between -50 and 0
		test_device_added = true;
	}

	// Send beacons from the queue
	for (int i = 0; i < MAX_BEACONS && beacons_sent < MAX_BEACONS_PER_SET &&
			(end - ptr) >= BEACON_DATA_SIZE;
	     i++) {
		if (beacon_queue[i].is_valid &&
		    (current_time - beacon_queue[i].last_seen) >= TIME_THRESHOLD) {
			memcpy(ptr, beacon_queue[i].addr.a.val, 6);
			ptr += 6;
			*ptr++ = beacon_queue[i].rssi;
			beacons_sent++;

			// Mark the beacon as invalid (effectively removing it from the queue)
			beacon_queue[i].is_valid = false;
			atomic_dec(&beacon_count);
		}
	}

	struct bt_data ad = {
		.type = BT_DATA_MANUFACTURER_DATA, .data_len = ptr - ad_data, .data = ad_data};

	printk("Advertisement data length: %u\n", ad.data_len);
	printk("First 10 bytes of final ad data: ");
	for (int i = 0; i < 10 && i < ad.data_len; i++) {
		printk("0x%02X ", ad.data[i]);
	}
	printk("\n");

	err = bt_le_ext_adv_set_data(adv_sets[set_to_use], &ad, 1, NULL, 0);
	if (err) {
		printk("Failed to set advertising data for set %d (err %d)\n", set_to_use, err);
		return err;
	}

    err = bt_le_ext_adv_start(adv_sets[set_to_use], BT_LE_EXT_ADV_START_PARAM(ADV_DURATION_MS, 0));
    if (err) {
	    printk("Failed to start extended advertising for set %d (err %d)\n", set_to_use, err);
	    return err;
    }

    printk("Extended advertising started successfully for set %d\n", set_to_use);
    atomic_or(adv_set_active_bitfield, BIT(set_to_use));
    atomic_inc(&active_adv_sets);

    printk("Beacons sent: %d, Test device added: %s, Remaining: %ld\n", beacons_sent,
	   test_device_added ? "Yes" : "No", (long)atomic_get(&beacon_count));
    printk("Exiting send_adv_data\n");

    return 0;
}

// Periodic Check and Send Mechanism
static void check_and_send(void)
{
	uint32_t current_time = k_uptime_get_32();
	uint32_t current_beacon_count = atomic_get(&beacon_count);

	if (IS_ENABLED(CONFIG_DEBUG)) {
		printk("check_and_send: current_time=%u, last_send_time=%u, beacon_count=%u\n",
		       current_time, last_send_time, current_beacon_count);
	}

	if (current_beacon_count >= MAX_BEACONS_PER_SET ||
	    (current_time - last_send_time) >= MAX_WAIT_TIME_MS) {

		int err = send_adv_data();
		if (err == 0) {
			atomic_set(&last_successful_operation, current_time);
			last_send_time = current_time;
		} else if (err == -EBUSY) {
			printk("All advertising sets are busy. Waiting for sets to become "
			       "available.\n");
		} else {
			printk("Failed to send advertising data (err %d)\n", err);
		}
	} else {
		printk("Not sending: waiting for more beacons or timeout\n");
	}

	// Check for long-running issues and attempt recovery
	if (current_time - atomic_get(&last_successful_operation) > RECOVERY_TIMEOUT_MS) {
		printk("No successful operations in a while. Attempting recovery...\n");
		recover_from_hang();
	}
}

// Advertising Work Handler
static void adv_work_handler(struct k_work *work)
{
	bool should_send = false;

	for (int i = 0; i < MAX_ADV_SETS; i++) {
		if (adv_sets[i] && (atomic_get(adv_set_active_bitfield) & BIT(i))) {
			int err = bt_le_ext_adv_stop(adv_sets[i]);
			if (err) {
				printk("Failed to stop advertising set %d (err %d)\n", i, err);
			} else {
				printk("Advertising set %d stopped\n", i);
				atomic_and(adv_set_active_bitfield, ~BIT(i));
				atomic_dec(&active_adv_sets);
				should_send = true;
			}
		}
	}

	if (should_send || atomic_get(&beacon_count) > 0) {
		check_and_send();
	}

	k_work_schedule(&adv_work, K_MSEC(ADV_DURATION_MS));
}

// Advertising Set Creation
static int create_adv_param(struct bt_le_ext_adv **adv)
{
	struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_USE_IDENTITY,
				     BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

	int err = bt_le_ext_adv_create(&param, NULL, adv);
	if (err) {
		printk("Failed to create advertising set (err %d)\n", err);
		return err;
	}

	printk("Advertising set created successfully\n");
	return 0;
}

// Bluetooth Observer Start
static int observer_start(void)
{
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
	printk("Started scanning successfully\n");

	return 0;
}

// Bluetooth Ready Callback
static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	for (int i = 0; i < MAX_ADV_SETS; i++) {
		err = create_adv_param(&adv_sets[i]);
		if (err) {
			printk("Failed to create advertising set %d (err %d)\n", i, err);
			return;
		}
	}

	k_work_init_delayable(&adv_work, adv_work_handler);
	k_work_schedule(&adv_work, K_MSEC(ADV_DURATION_MS));

	err = observer_start();
	if (err) {
		printk("Observer start failed (err %d)\n", err);
	}

	atomic_set(&last_successful_operation, k_uptime_get_32());
}

static void recover_from_hang(void)
{
	printk("Attempting to recover from hang...\n");

	// Stop all advertising sets
	for (int i = 0; i < MAX_ADV_SETS; i++) {
		if (adv_sets[i]) {
			bt_le_ext_adv_stop(adv_sets[i]);
			atomic_and(adv_set_active_bitfield, ~BIT(i));
		}
	}
	atomic_set(&active_adv_sets, 0);

	// Restart Bluetooth
	bt_disable();
	k_sleep(K_MSEC(1000));
	int err = bt_enable(bt_ready);
	if (err) {
		printk("Failed to re-enable Bluetooth (err %d)\n", err);
	} else {
		printk("Bluetooth re-enabled successfully\n");
	}

	// Reset other variables
	atomic_set(&beacon_count, 0);
	atomic_set(&beacons_since_last_check, 0);
	last_send_time = 0;

	// Restart scanning
	observer_start();

	printk("Recovery attempt completed\n");
}

// Main Function
int main(void)
{
	int err;

	printk("Starting Beacon Relay\n");

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
