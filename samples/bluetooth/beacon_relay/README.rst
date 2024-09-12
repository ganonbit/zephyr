.. _bluetooth-beacon-relay-sample:

Bluetooth: Beacon Relay with Extended Advertising
#################################################

Overview
********

This application demonstrates a Bluetooth Low Energy system that observes nearby
beacons and relays their information using extended advertising. It consists of
two parts: a scanner and an advertiser.

Scanner
=======

The scanner continuously scans for nearby Bluetooth beacons. When a beacon is
detected, it captures the device's address and RSSI. This data is then passed
to the advertiser.

Advertiser
==========

The advertiser receives beacon data from the scanner and uses extended advertising
to relay this information. Key features include:

* Maintains a queue of detected beacons
* Dynamically creates advertising sets as needed
* Packages multiple beacon entries into each advertising packet
* Uses manufacturer-specific data format for advertising

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

1. Build and flash the scanner application to one board:

   .. zephyr-app-commands::
      :zephyr-app: samples/bluetooth/beacon_relay/scanner
      :board: nrf52840dongle
      :goals: build flash
      :compact:

2. Build and flash the advertiser application to another board:

   .. zephyr-app-commands::
      :zephyr-app: samples/bluetooth/beacon_relay/advertiser
      :board: nrf52840dongle
      :goals: build flash
      :compact:

Testing
*******

After building and flashing the samples to your boards:

1. The scanner will start scanning for nearby Bluetooth beacons.
2. When beacons are detected, the scanner will pass this information to the advertiser.
3. The advertiser will queue the beacon information and start extended advertising.

Expected Output
===============

On the scanner console, you should see output similar to this:

.. code-block:: console

   Starting Beacon Relay Scanner
   Bluetooth initialized
   Started scanning...
   Beacon found: XX:XX:XX:XX:XX:XX (random), RSSI: -70

On the advertiser console, you should see output similar to this:

.. code-block:: console

   Starting Beacon Relay Advertiser
   Bluetooth initialized
   Advertising set created successfully
   Extended advertising started successfully for set 0

Note: Replace XX:XX:XX:XX:XX:XX with actual Bluetooth addresses you observe.

A nearby Bluetooth gateway should be able to receive the extended advertisements
containing the aggregated beacon data.
