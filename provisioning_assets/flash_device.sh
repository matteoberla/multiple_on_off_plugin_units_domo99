#!/bin/bash

# 1. Caricamento ambiente
. /Users/matteo/.espressif/v5.5.3/esp-idf/export.sh
. /Users/matteo/esp/esp-matter/export.sh

# 2. Erase totale
echo "Esecuzione Erase Flash..."
idf.py -p /dev/cu.usbmodem11101 erase-flash

# 3. Flash completo (Bootloader + App + Factory Data)
echo "Esecuzione Flash..."
python3 -m esptool -p /dev/cu.usbmodem11101 write_flash \
  0x0 build/bootloader/bootloader.bin \
  0xc000 build/partition_table/partition-table.bin \
  0x51000 build/ota_data_initial.bin \
  0x60000 build/multiple_on_off_plugin_units.bin \
  0x3C0000 mfg_s3_final/fff1_8000/db91e535-aca6-4ff1-8563-af75090f5e22/db91e535-aca6-4ff1-8563-af75090f5e22-partition.bin

# 4. Monitor
echo "Avvio Monitor..."
idf.py -p /dev/cu.usbmodem11101 monitor
