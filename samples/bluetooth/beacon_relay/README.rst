.. _bluetooth-beacon-relay-sample:

Bluetooth: Beacon Relay with Extended Advertising
#################################################

Overview
********

This application demonstrates a Bluetooth Low Energy system that observes nearby
beacons and relays their information using extended advertising. It consists of
two parts: a scanner and an advertiser.

Components
**********

Scanner (scanner/src/main.c)
============================

The scanner's main.c file implements the following functionality:

* Initializes Bluetooth and sets up scanning parameters
* Implements a callback function to handle discovered devices
* Filters discovered devices to only process beacons
* Sends beacon information to the advertiser via UART
* Continuously scans for nearby Bluetooth beacons

Key functions:
- device_found: Callback for discovered devices
- scan_start: Starts the scanning process
- bt_ready: Callback when Bluetooth is initialized

Advertiser (advertiser/src/main.c)
==================================

The advertiser's main.c file implements the following functionality:

* Initializes Bluetooth and sets up extended advertising
* Receives beacon data from the scanner via UART
* Maintains a queue of detected beacons
* Dynamically creates and manages advertising sets
* Packages multiple beacon entries into each advertising packet
* Uses manufacturer-specific data format for advertising
* Implements a periodic check-and-send mechanism

Key functions:
- add_beacon: Adds a beacon to the queue
- send_adv_data: Prepares and sends advertising data
- check_and_send: Periodically checks conditions and sends data
- adv_work_handler: Manages advertising sets
- create_adv_param: Creates advertising parameters
- bt_ready: Callback when Bluetooth is initialized

The advertiser allows a nearby gateway to collect the aggregated beacon data.

Requirements
************

* Two boards with Bluetooth Low Energy support, specifically ones that support
  extended advertising (e.g., nRF52840 USB dongle)
* One or more Bluetooth Low Energy beacons (iBeacon or Eddystone)

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/beacon_relay` in the
Zephyr tree.

See :ref:`Bluetooth samples section <bluetooth-samples>` for details on how to build and run the sample.

.. note::
   You can only generate and flash one hex file at a time. Follow these steps for each board separately.

Building the Scanner
====================

1. Build the hex file for the scanner application:

   .. code-block:: console

      west build -p always -b nrf52840dongle samples/bluetooth/beacon_relay/scanner

2. Flash the built hex file to the scanner board.

Building the Advertiser
=======================

1. Build the hex file for the advertiser application:

   .. code-block:: console

      west build -p always -b nrf52840dongle samples/bluetooth/beacon_relay/advertiser

2. Flash the built hex file to the advertiser board.

Viewing Console Output
======================

To view the console output:

1. Find the appropriate device paths:

   .. tabs::

      .. group-tab:: macOS

         .. code-block:: console

            ls /dev/tty.*

      .. group-tab:: Linux

         .. code-block:: console

            ls /dev/ttyACM*

      .. group-tab:: Windows

         Open Device Manager and look under "Ports (COM & LPT)" for COM ports.

2. Connect to the scanner:

   .. tabs::

      .. group-tab:: macOS/Linux

         .. code-block:: console

            minicom -D /dev/ttyACM0 -b 115200

         Replace `/dev/ttyACM0` with the appropriate device path.

      .. group-tab:: Windows

         Use PuTTY or a similar terminal program:
         - Select "Serial" connection type
         - Choose the appropriate COM port
         - Set the baud rate to 115200

3. In a separate terminal or window, connect to the advertiser using the same method.

.. note::
   The exact device names may vary. On Linux, you may need to use `sudo` to access the serial ports.

Testing
*******

After building and flashing the samples to your boards:

1. The scanner will start scanning for nearby Bluetooth beacons.
2. When beacons are detected, the scanner will pass this information to the advertiser.
3. The advertiser will queue the beacon information and start extended advertising.

Expected Output
===============

Scanner console:

.. code-block:: console

   Starting Beacon Relay Scanner
   Bluetooth initialized
   Started scanning...
   Beacon found: XX:XX:XX:XX:XX:XX (random), RSSI: -70

Advertiser console:

.. code-block:: console

   Starting Beacon Relay Advertiser
   Bluetooth initialized
   Advertising set created successfully
   Extended advertising started successfully for set 0

.. note::
   Replace XX:XX:XX:XX:XX:XX with actual Bluetooth addresses you observe.

A nearby Bluetooth gateway should be able to receive the extended advertisements
containing the aggregated beacon data.
