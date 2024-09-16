#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define ADV_DURATION_MS      1000
#define MAX_EXT_ADV_DATA_LEN 191
#define MAX_ADV_SETS         3
#define BEACON_DATA_SIZE     7
#define MAX_BEACONS_PER_SET  25 // must be divisible by BEACON_BATCH_SIZE
#define MAX_BEACONS          120
#define MAX_WAIT_TIME_MS     1500
#define BEACON_BATCH_SIZE    5

// Data Structures
struct beacon_info {
	bt_addr_le_t addr;
	int8_t rssi;
};

// Static Variables
static struct bt_le_ext_adv *adv_sets[MAX_ADV_SETS];
static struct beacon_info beacon_queue[MAX_BEACONS];
static ATOMIC_DEFINE(beacon_count, 32);
static atomic_t beacon_write_index = ATOMIC_INIT(0);
static atomic_t beacon_read_index = ATOMIC_INIT(0);
static atomic_t active_adv_sets = ATOMIC_INIT(0);
static ATOMIC_DEFINE(adv_set_active_bitfield, MAX_ADV_SETS);
static atomic_t beacons_since_last_check = ATOMIC_INIT(0);

static struct k_work_delayable adv_work;
static uint32_t last_send_time = 0;

static uint8_t ad_data[MAX_EXT_ADV_DATA_LEN];

// Function Declarations
static void add_beacon(const bt_addr_le_t *addr, int8_t rssi);
static void send_adv_data(void);
static void check_and_send(void);
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad);
static void adv_work_handler(struct k_work *work);
static int create_adv_param(struct bt_le_ext_adv **adv);
static int observer_start(void);
static void bt_ready(int err);

// Beacon Management
static inline void add_beacon(const bt_addr_le_t *addr, int8_t rssi)
{
	atomic_val_t write_index = atomic_get(&beacon_write_index);

	memcpy(&beacon_queue[write_index].addr, addr, sizeof(bt_addr_le_t));
	beacon_queue[write_index].rssi = rssi;

	atomic_set(&beacon_write_index, (write_index + 1) % MAX_BEACONS);
	atomic_inc(beacon_count);

	if (IS_ENABLED(CONFIG_DEBUG)) {
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		printk("Added new beacon: %s, RSSI: %d, total count: %ld, write index: %ld\n",
		       addr_str, rssi, (long)atomic_get(beacon_count), (long)write_index);
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
static void send_adv_data(void)
{
	printk("Entering send_adv_data\n");
	int err;
	uint8_t *ptr = ad_data;
	uint8_t *end = ad_data + MAX_EXT_ADV_DATA_LEN;

	*ptr++ = 0x62;                                     // Company ID (MOKO TECHNOLOGY Ltd - LSB)
	*ptr++ = 0x0A;                                     // Company ID (MOKO TECHNOLOGY Ltd - MSB)
	*ptr++ = 0x00;                                     // Breakpoint
	*ptr++ = (uint8_t)atomic_get(&beacon_write_index); // Sequence number

	printk("Company ID bytes: 0x%02X 0x%02X\n", *(ptr - 4), *(ptr - 3));
	printk("Sequence number: 0x%02X\n", *(ptr - 1));

	printk("First 10 bytes of ad_data: ");
	for (int i = 0; i < 10 && i < (ptr - ad_data); i++) {
		printk("0x%02X ", ad_data[i]);
	}
	printk("\n");

	int total_beacons = atomic_get(beacon_count);
	atomic_val_t read_index = atomic_get(&beacon_read_index);
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
		return;
	}

	int beacons_to_send = MIN(total_beacons, MAX_BEACONS_PER_SET);

	while (beacons_sent < beacons_to_send && (end - ptr) >= BEACON_DATA_SIZE) {
		struct beacon_info *beacon = &beacon_queue[read_index];

		memcpy(ptr, beacon->addr.a.val, 6);
		ptr += 6;
		*ptr++ = beacon->rssi;
		beacons_sent++;
		read_index = (read_index + 1) % MAX_BEACONS;
	}

	atomic_set(&beacon_read_index, read_index);
	atomic_sub(beacon_count, beacons_sent);

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
		return;
	}

    err = bt_le_ext_adv_start(adv_sets[set_to_use], BT_LE_EXT_ADV_START_PARAM(ADV_DURATION_MS, 0));
    if (err) {
	    printk("Failed to start extended advertising for set %d (err %d)\n", set_to_use, err);
	    return;
    }

    printk("Extended advertising started successfully for set %d\n", set_to_use);
    atomic_or(adv_set_active_bitfield, BIT(set_to_use));
    atomic_inc(&active_adv_sets);

    printk("Beacons sent: %d, Remaining: %ld\n", beacons_sent, atomic_get(beacon_count));
    printk("Exiting send_adv_data\n");
}

// Periodic Check and Send Mechanism
static void check_and_send(void)
{
	uint32_t current_time = k_uptime_get_32();
	uint32_t current_beacon_count = atomic_get(beacon_count);

	if (IS_ENABLED(CONFIG_DEBUG)) {
		printk("check_and_send: current_time=%u, last_send_time=%u, beacon_count=%u\n",
		       current_time, last_send_time, current_beacon_count);
	}

	if (current_beacon_count >= BEACON_BATCH_SIZE ||
	    (current_time - last_send_time) >= MAX_WAIT_TIME_MS) {

		int available_sets = MAX_ADV_SETS - atomic_get(&active_adv_sets);
		int sets_to_use = MIN(available_sets, (current_beacon_count + MAX_BEACONS_PER_SET -
						       1) / MAX_BEACONS_PER_SET);

		for (int i = 0; i < sets_to_use; i++) {
			printk("Sending beacons using set %d\n", i);
			send_adv_data();

			// Break if we've sent all beacons
			if (atomic_get(beacon_count) == 0) {
				break;
			}
		}

		last_send_time = current_time;
	} else {
		printk("Not sending: waiting for more beacons or timeout\n");
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
