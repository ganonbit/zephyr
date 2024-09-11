.. _bluetooth-observer-sample:

Bluetooth: Observer with Extended Advertising
#############################################

Overview
********

This application demonstrates Bluetooth Low Energy Observer role functionality
with extended advertising support. It periodically scans for nearby beacons,
specifically iBeacon and Eddystone devices. Upon detection, it captures the
device address, RSSI, and beacon-specific data. The application then uses
extended advertising to relay this information, allowing a nearby gateway to
collect the aggregated beacon data.

Requirements
************

* A board with Bluetooth Low Energy support, specifically one that supports
  extended advertising (e.g., nRF52840 USB dongle)
* One or more Bluetooth Low Energy beacons (iBeacon or Eddystone)

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/observe_eddy` in the
Zephyr tree.

See :ref:`Bluetooth samples section <bluetooth-samples>` for details on how to build and run the sample.

Testing
*******

After building and flashing the sample to your board, it will start scanning for
nearby beacons. When a beacon is detected, the application will log its information
and attempt to relay this data using extended advertising.

Expected Output
===============

You should see output similar to this on the console:

.. code-block:: console

   Starting Observer Demo
   Bluetooth initialized
   Started scanning...
   Debug: Found new beacon device: XX:XX:XX:XX:XX:XX (random)
   Debug: Sending extended advertising data for device: XX:XX:XX:XX:XX:XX (random)
   Debug: Extended advertising data details:
     - Total ad elements: 2
     - Name length: 12
     - Manufacturer data length: 14
     - Total data length: 28
   Extended advertising started successfully

Note: Replace XX:XX:XX:XX:XX:XX with actual Bluetooth addresses you observe.
