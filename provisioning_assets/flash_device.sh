#!/bin/bash

# =============================================================================
# flash_device.sh — ESP32-S3-N16R8 (Domo99 / VendorID fff1_8000)
#
# ⚠️  PROCEDURA OBBLIGATORIA PER OGNI COMANDO ESPTOOL:
#     1. Tieni premuto BOOT sul dispositivo
#     2. Lancia il comando (o approvalo in Antigravity)
#     3. Quando vedi "Connecting..." → premi e rilascia EN/RESET
#     4. Rilascia BOOT
#
# Porta: /dev/cu.usbmodem1101
# Chip:  ESP32-S3 (QFN56) rev 0.2 — 16MB Flash, 8MB PSRAM Octal
# MAC:   14:c1:9f:c1:24:28
# =============================================================================

set -e

PORT="/dev/cu.usbmodem1101"
BAUD="460800"
ESPTOOL="esptool.py --chip esp32s3 --port $PORT --baud $BAUD --before no_reset --after hard_reset --connect-attempts 30"
FLASH_ARGS="--flash_mode dio --flash_freq 80m --flash_size 16MB"

# Caricamento ambiente ESP-IDF
source /Users/matteo/.espressif/v5.5.3/esp-idf/export.sh

# -----------------------------------------------------------------------------
# OPZIONE A — Flash completo + erase NVS (nuovo dispositivo o re-flash totale)
#   ✅ USA QUESTO per un dispositivo nuovo o dopo un erase totale.
#   Scrive: bootloader, partition table, ota_data, firmware, fctry.
#   Poi cancella l'NVS → il device riparte in modalità commissioning.
# -----------------------------------------------------------------------------
flash_completo() {
  echo "=== [A] Erase totale + Flash completo ==="
  $ESPTOOL write_flash --erase-all $FLASH_ARGS \
    0x0      build/bootloader/bootloader.bin \
    0xc000   build/partition_table/partition-table.bin \
    0x51000  build/ota_data_initial.bin \
    0x60000  build/multiple_on_off_plugin_units.bin \
    0x3C0000 provisioning_assets/fff1_8000_domo99_partition.bin
  echo "=== Flash completo OK ==="

  echo "=== Erase NVS ==="
  $ESPTOOL erase_region 0x10000 0x40000
  echo "=== Device pronto per il commissioning (BLE attivo) ==="
}

# -----------------------------------------------------------------------------
# OPZIONE B — Erase NVS solo (reset commissioning, fctry intatta)
#   ✅ USA QUESTO quando Apple Home non riesce ad aggiungere il dispositivo
#      ma il firmware e i certificati sono già corretti.
#   ⚠️  Cancella SOLO la partizione nvs (0x10000, 256KB):
#       rimuove fabric, WiFi, ACL — i certificati DAC/PAI restano intatti.
# -----------------------------------------------------------------------------
erase_nvs() {
  echo "=== [B] Erase NVS (solo) ==="
  $ESPTOOL erase_region 0x10000 0x40000
  echo "=== NVS cancellato. Device in modalità commissioning (BLE attivo). ==="
}

# -----------------------------------------------------------------------------
# OPZIONE C — Reflash FCTRY + erase NVS
#   ✅ USA QUESTO se la partizione fctry è corrotta o dopo un erase totale
#      accidentale di 0x3C0000.
#   ⚠️  Se il device crasha con "ERROR setting up transport: 201" o
#       "Failed to init server instance", la fctry è corrotta → usa questa.
# -----------------------------------------------------------------------------
reflash_fctry_e_nvs() {
  echo "=== [C] Reflash fctry + erase NVS ==="
  $ESPTOOL write_flash $FLASH_ARGS \
    0x3C0000 provisioning_assets/fff1_8000_domo99_partition.bin
  echo "=== Fctry OK ==="

  $ESPTOOL erase_region 0x10000 0x40000
  echo "=== NVS cancellato. Device in modalità commissioning (BLE attivo). ==="
}

# -----------------------------------------------------------------------------
# OPZIONE E — Genera immagine unica (merged bin) da flashare a 0x0
#   Contiene: bootloader, partition table, ota_data, firmware, fctry.
#   NVS resta a 0xFF → il device parte in modalità commissioning.
#   Output: provisioning_assets/domo99_full.bin
#   Flash:  esptool.py --chip esp32s3 write_flash --erase-all 0x0 domo99_full.bin
# -----------------------------------------------------------------------------
genera_merged() {
  echo "=== [E] Generazione immagine unica ==="
  esptool.py --chip esp32s3 merge_bin -o provisioning_assets/domo99_full.bin \
    $FLASH_ARGS \
    0x0      build/bootloader/bootloader.bin \
    0xc000   build/partition_table/partition-table.bin \
    0x51000  build/ota_data_initial.bin \
    0x60000  build/multiple_on_off_plugin_units.bin \
    0x3C0000 provisioning_assets/fff1_8000_domo99_partition.bin
  ls -l provisioning_assets/domo99_full.bin
}

# -----------------------------------------------------------------------------
# OPZIONE D — Monitor seriale
# -----------------------------------------------------------------------------
monitor() {
  echo "=== [D] Avvio Monitor ==="
  idf.py -p $PORT monitor
}

# --- Selezione operazione ---
echo ""
echo "Seleziona operazione:"
echo "  A) Flash completo + erase NVS  (nuovo dispositivo)"
echo "  B) Erase NVS solo              (reset commissioning)"
echo "  C) Reflash fctry + erase NVS   (fctry corrotta)"
echo "  D) Monitor seriale"
echo "  E) Genera immagine unica (merged bin)"
echo ""
read -rp "Scelta [A/B/C/D/E]: " scelta

case "${scelta^^}" in
  A) flash_completo ;;
  B) erase_nvs ;;
  C) reflash_fctry_e_nvs ;;
  D) monitor ;;
  E) genera_merged ;;
  *) echo "Scelta non valida." ; exit 1 ;;
esac
