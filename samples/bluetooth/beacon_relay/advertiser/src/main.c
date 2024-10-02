#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define MAX_EXT_ADV_DATA_LEN 191
#define MAX_ADV_SETS          2
#define MAX_BEACONS          100
#define MAX_BEACONS_PER_SET  24
#define BEACON_BATCH_SIZE    3
#define BEACON_DATA_SIZE      7 // 6 bytes for address + 1 byte for RSSI
#define ADV_DURATION_MS      2000
#define MAX_WAIT_TIME_MS     3000
#define RECOVERY_TIMEOUT_MS  5000
#define TEST_DEVICE_ADDR      {0xF6, 0xE5, 0xD4, 0xC3, 0xB2, 0xA1}
#define TIME_THRESHOLD       5000
#define INITIAL_TTL           3
#define SEQUENCE_HISTORY_SIZE 10

// Data Structures
struct beacon_info {
	bt_addr_le_t addr;
	int8_t rssi;
	uint32_t last_seen;
	bool is_valid;
	uint8_t ttl;
	uint8_t last_sequence;
	uint8_t sequence_history[SEQUENCE_HISTORY_SIZE];
	uint8_t history_index;
	int16_t temperature;
	uint16_t voltage;
};

// Static Variables
static struct bt_le_ext_adv *adv_sets[MAX_ADV_SETS];
static struct beacon_info beacon_queue[MAX_BEACONS];
static atomic_t active_adv_sets = ATOMIC_INIT(0);
static ATOMIC_DEFINE(adv_set_active_bitfield, MAX_ADV_SETS);
static atomic_t beacons_since_last_check = ATOMIC_INIT(0);
static atomic_t last_successful_operation = ATOMIC_INIT(0);
static struct k_work_delayable adv_work;
static uint32_t last_send_time = 0;
static uint8_t ad_data[MAX_EXT_ADV_DATA_LEN];

// Function Declarations
static int find_or_update_beacon(const bt_addr_le_t *addr, int8_t rssi, uint8_t ttl,
				 uint8_t sequence, int16_t temperature, uint16_t voltage);
static void add_beacon(const bt_addr_le_t *addr, int8_t rssi, uint8_t ttl, uint8_t sequence,
		       int16_t temperature, uint16_t voltage, bool is_test_device);
static int send_adv_data(void);
static void check_and_send(void);
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad);
static void adv_work_handler(struct k_work *work);
static int create_adv_param(struct bt_le_ext_adv **adv);
static int observer_start(void);
static void bt_ready(int err);
static void recover_from_hang(void);
static bool is_duplicate_sequence(struct beacon_info *beacon, uint8_t sequence);
static void update_sequence_history(struct beacon_info *beacon, uint8_t sequence);
static void cleanup_old_beacons(void);

// Beacon Management
static int find_or_update_beacon(const bt_addr_le_t *addr, int8_t rssi, uint8_t ttl,
				 uint8_t sequence, int16_t temperature, uint16_t voltage)
{
	uint32_t current_time = k_uptime_get_32();
	int empty_slot = -1;
	int existing_slot = -1;

	for (int i = 0; i < MAX_BEACONS; i++) {
		if (!beacon_queue[i].is_valid) {
			if (empty_slot == -1) {
				empty_slot = i;
			}
			continue;
		}

		if (bt_addr_le_cmp(&beacon_queue[i].addr, addr) == 0) {
			existing_slot = i;
			break;
		}
	}

	if (existing_slot != -1) {
		// Check for duplicate sequence
		if (is_duplicate_sequence(&beacon_queue[existing_slot], sequence)) {
			return existing_slot; // Return existing slot if it's a duplicate
		}

		// Update existing beacon
		beacon_queue[existing_slot].last_seen = current_time;
		beacon_queue[existing_slot].ttl = ttl;
		beacon_queue[existing_slot].temperature = temperature;
		beacon_queue[existing_slot].voltage = voltage;
		update_sequence_history(&beacon_queue[existing_slot], sequence);
		return existing_slot;
	}

	// Add new beacon if space available
	if (empty_slot != -1) {
		memcpy(&beacon_queue[empty_slot].addr, addr, sizeof(bt_addr_le_t));
		beacon_queue[empty_slot].rssi = rssi;
		beacon_queue[empty_slot].temperature = temperature;
		beacon_queue[empty_slot].voltage = voltage;
		beacon_queue[empty_slot].last_seen = current_time;
		beacon_queue[empty_slot].is_valid = true;
		beacon_queue[empty_slot].ttl = ttl;
		beacon_queue[empty_slot].last_sequence = sequence;
		memset(beacon_queue[empty_slot].sequence_history, 0, SEQUENCE_HISTORY_SIZE);
		beacon_queue[empty_slot].sequence_history[0] = sequence;
		beacon_queue[empty_slot].history_index = 1;
		return empty_slot;
	}

	return -1; // Queue is full
}

static void add_beacon(const bt_addr_le_t *addr, int8_t rssi, uint8_t ttl, uint8_t sequence,
		       int16_t temperature, uint16_t voltage, bool is_test_device)
{
	int index = find_or_update_beacon(addr, rssi, ttl, sequence, temperature, voltage);
	if (index >= 0) {
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		// printk("Beacon updated/added: Address: %s (0x%02x), RSSI: %d,"
		//        "Temperature: %d, Voltage: %d,"
		//        "Index: %d\n",
		//        addr_str, addr->a.val[5], rssi, temperature, voltage, index);

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

	uint8_t ttl = INITIAL_TTL;
	uint8_t sequence = 0;
	int16_t temperature = 0;
	uint16_t voltage = 0;

	// Check if this is a relay packet by looking for our custom data
	struct net_buf_simple_state state;
	net_buf_simple_save(ad, &state);
	printk("Debug: Saved net_buf_simple state. Offset: %u, Length: %u\n", state.offset,
	       state.len);

	// Debug: Print entire advertisement data
	printk("Debug: Advertisement data (%d bytes):", ad->len);
	for (int i = 0; i < ad->len; i++) {
		printk(" %02X", ad->data[i]);
	}
	printk("\n");

	while (ad->len > 1) {
		uint8_t len = net_buf_simple_pull_u8(ad);
		uint8_t type = net_buf_simple_pull_u8(ad);

		printk("Debug: AD type: 0x%02X, length: %d\n", type, len);

		if (type == BT_DATA_MANUFACTURER_DATA && len >= 3) {
			uint8_t *data = net_buf_simple_pull_mem(ad, len - 1);
			printk("Debug: Manufacturer data: %02X %02X %02X\n", data[0], data[1],
			       data[2]);
			if (data[0] == 0x59 && data[1] == 0x00 && data[2] == 0x08) {
				printk("Debug: Found custom identifier\n");
				sequence = data[3];
				ttl = data[4];
				if (ttl > 0) {
					ttl--;
				}
				printk("Debug: Sequence: %d, TTL: %d\n", sequence, ttl);
			}
		} else if (type == BT_DATA_SVC_DATA16 && len >= 11) {
			uint8_t *data = net_buf_simple_pull_mem(ad, len - 1);
			if (data[0] == 0xAA && data[1] == 0xFE && data[2] == 0x20) {
				char addr_str[BT_ADDR_LE_STR_LEN];
				bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
				printk("Debug: Found Eddystone TLM frame from device %s\n",
				       addr_str);
				printk("Debug: TLM frame data for %s:", addr_str);
				for (int i = 0; i < len - 1; i++) {
					printk(" %02X", data[i]);
				}
				printk("\n");
				voltage = (data[4] << 8) | data[5];
				temperature = (data[6] << 8) | data[7];

				printk("Debug: Parsed TLM data for %s - Temperature: %.2d C, "
				       "Voltage: %d mV\n",
				       addr_str, temperature, voltage);
			}
		} else {
			net_buf_simple_pull_mem(ad, len - 1);
		}
	}

	net_buf_simple_restore(ad, &state);

	printk("Debug: Adding beacon in device_found - Address: %02X:%02X:%02X:%02X:%02X:%02X, "
	       "RSSI: %d, TTL: %d, Sequence: %d, Temperature: %d, Voltage: %d\n",
	       addr->a.val[5], addr->a.val[4], addr->a.val[3], addr->a.val[2], addr->a.val[1],
	       addr->a.val[0], rssi, ttl, sequence, temperature, voltage);
	add_beacon(addr, rssi, ttl, sequence, temperature, voltage, false);
	printk("Debug: Beacon added successfully\n");

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

	// Check if any advertising sets are active before sending new beacons
	if (atomic_get(&active_adv_sets) > 0) { // Pass the address of active_adv_sets
		printk("Currently active advertising sets. Skipping new beacon advertisement.\n");
		return -EBUSY; // Return busy if advertising is ongoing
	}

	static uint8_t global_sequence = 0;
	global_sequence++;

	*ptr++ = 0x59;            // Company ID (LSB)
	*ptr++ = 0x00;            // Company ID (MSB)
	*ptr++ = 0x08;            // Custom first byte of manufacturer data
	*ptr++ = global_sequence; // Sequence number
	*ptr++ = INITIAL_TTL;     // Add initial TTL to the packet

	printk("Company ID bytes: 0x%02X 0x%02X\n", *(ptr - 5), *(ptr - 4));
	printk("Custom first byte of manufacturer data: 0x%02X\n", *(ptr - 3));
	printk("Sequence number: 0x%02X\n", *(ptr - 2));
	printk("Initial TTL: 0x%02X\n", *(ptr - 1));

	uint32_t current_time = k_uptime_get_32();
	bool test_device_added = false;
	int beacons_sent = 0;

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
	if ((end - ptr) >= (BEACON_DATA_SIZE + 4)) { // +4 for temperature and voltage bytes
		printk("Debug: Adding test device\n");
		uint8_t test_addr[] = TEST_DEVICE_ADDR;
		memcpy(ptr, test_addr, 6);
		ptr += 6;
		*ptr++ = -20;                   // Static RSSI of -20
		uint8_t test_ttl = INITIAL_TTL;
		uint16_t test_temperature = 17664;
		uint16_t test_voltage = 5000;
		*ptr++ = test_ttl;

		// When adding to the packet:
		*ptr++ = test_temperature & 0xFF;        // temperature low byte
		*ptr++ = (test_temperature >> 8) & 0xFF; // temperature high byte
		*ptr++ = test_voltage & 0xFF;            // voltage low byte
		*ptr++ = (test_voltage >> 8) & 0xFF;     // voltage high byte

		printk("Debug: Test device data - RSSI: %d, TTL: %d, Temperature: %d, Voltage: "
		       "%d\n",
		       -20, test_ttl, test_temperature, test_voltage);

		add_beacon((bt_addr_le_t *)test_addr, *(ptr - 4), test_ttl, global_sequence,
			   test_temperature, test_voltage, true); // Pass true for test device
		test_device_added = true;
		printk("Debug: Test device added successfully\n");
	} else {
		printk("Debug: Not enough space to add test device\n");
	}

	// Send beacons from the queue
	for (int i = 0; i < MAX_BEACONS && beacons_sent < MAX_BEACONS_PER_SET &&
			(end - ptr) >= BEACON_DATA_SIZE;
	     i++) {
		if (beacon_queue[i].is_valid &&
		    (current_time - beacon_queue[i].last_seen) >= TIME_THRESHOLD &&
		    beacon_queue[i].ttl > 0) { // Only relay if TTL > 0
			memcpy(ptr, beacon_queue[i].addr.a.val, 6);
			ptr += 6;
			*ptr++ = beacon_queue[i].rssi;
			*ptr++ = beacon_queue[i].ttl;
			*ptr++ = beacon_queue[i].temperature & 0xFF;
			*ptr++ = (beacon_queue[i].temperature >> 8) & 0xFF;
			*ptr++ = beacon_queue[i].voltage & 0xFF;
			*ptr++ = (beacon_queue[i].voltage >> 8) & 0xFF;
			beacons_sent++;

			// Mark the beacon as invalid (effectively removing it from the queue)
			beacon_queue[i].is_valid = false;
			printk("Beacon sent: Address: %02X:%02X:%02X:%02X:%02X:%02X, RSSI: %d, "
			       "TTL: %d, Temperature: %d, Voltage: %d, Last seen: %u\n",
			       beacon_queue[i].addr.a.val[5], beacon_queue[i].addr.a.val[4],
			       beacon_queue[i].addr.a.val[3], beacon_queue[i].addr.a.val[2],
			       beacon_queue[i].addr.a.val[1], beacon_queue[i].addr.a.val[0],
			       beacon_queue[i].rssi, beacon_queue[i].ttl,
			       beacon_queue[i].temperature, beacon_queue[i].voltage,
			       beacon_queue[i].last_seen);
		}
	}

	struct bt_data ad = {
		.type = BT_DATA_MANUFACTURER_DATA, .data_len = ptr - ad_data, .data = ad_data};

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

    printk("Beacons sent: %d, Test device added: %s\n", beacons_sent,
	   test_device_added ? "Yes" : "No");
    printk("Exiting send_adv_data\n");

    return 0;
}

// Periodic Check and Send Mechanism
static void check_and_send(void)
{
	uint32_t current_time = k_uptime_get_32();

	if (IS_ENABLED(CONFIG_DEBUG)) {
		printk("check_and_send: current_time=%u, last_send_time=%u\n", current_time,
		       last_send_time);
	}

	// Call cleanup_old_beacons to remove old entries
	cleanup_old_beacons();

	if ((current_time - last_send_time) >= MAX_WAIT_TIME_MS) {
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
		printk("Not sending: waiting for timeout\n");
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

	if (should_send) {
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
	atomic_set(&beacons_since_last_check, 0);
	last_send_time = 0;

	// Restart scanning
	observer_start();

	printk("Recovery attempt completed\n");
}

// Add a function to check if a sequence number has been seen recently
static bool is_duplicate_sequence(struct beacon_info *beacon, uint8_t sequence)
{
	if (sequence == beacon->last_sequence) {
		return true;
	}
	for (int i = 0; i < SEQUENCE_HISTORY_SIZE; i++) {
		if (beacon->sequence_history[i] == sequence) {
			return true;
		}
	}
	return false;
}

// Add a function to update the sequence history
static void update_sequence_history(struct beacon_info *beacon, uint8_t sequence)
{
	beacon->last_sequence = sequence;
	beacon->sequence_history[beacon->history_index] = sequence;
	beacon->history_index = (beacon->history_index + 1) % SEQUENCE_HISTORY_SIZE;
}

// Add a function to periodically clean up old beacons
static void cleanup_old_beacons(void)
{
	uint32_t current_time = k_uptime_get_32();
	for (int i = 0; i < MAX_BEACONS; i++) {
		if (beacon_queue[i].is_valid &&
		    (current_time - beacon_queue[i].last_seen) >= (TIME_THRESHOLD * 2)) {
			beacon_queue[i].is_valid = false;
			printk("Removed old beacon: Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
			       beacon_queue[i].addr.a.val[5], beacon_queue[i].addr.a.val[4],
			       beacon_queue[i].addr.a.val[3], beacon_queue[i].addr.a.val[2],
			       beacon_queue[i].addr.a.val[1], beacon_queue[i].addr.a.val[0]);
		}
	}
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
