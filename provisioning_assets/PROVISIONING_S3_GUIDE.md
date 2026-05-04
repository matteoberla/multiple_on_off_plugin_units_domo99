# Guida al Provisioning Matter: ESP32-S3 vs ESP32-C6

Questa guida documenta la procedura di migrazione e ottimizzazione del firmware Matter per il passaggio dall'hardware ESP32-C6 all'ESP32-S3 (modello N16R8), con supporto a 100 endpoint.

## 1. Differenze Architetturali

| Caratteristica | ESP32-C6 | ESP32-S3 (N16R8) |
| :--- | :--- | :--- |
| **Flash** | Tipicamente 4MB | 16MB |
| **PSRAM** | Assente / Limitata | 8MB Octal (Esterna) |
| **Offset Partizioni** | Standard Matter | Personalizzati (fctry a 0x3C0000) |
| **Capacità Endpoint** | ~20-30 (limite RAM) | 100+ (grazie a PSRAM) |

## 2. Configurazione Hardware (ESP32-S3-N16R8)

Per supportare 100 endpoint, è stato necessario configurare il file `sdkconfig` con parametri specifici per la gestione della memoria esterna:

- **Flash 16MB**: Abilitata tramite `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`.
- **Octal SPIRAM**: Attivata la modalità `CONFIG_SPIRAM_MODE_OCT=y` per massime prestazioni con il modulo R8.
- **Memory Mapping**: Forzata l'allocazione del Data Model Matter e di mbedTLS (sicurezza) in PSRAM per evitare l'errore `BIGNUM - Memory allocation failed`.

## 3. Risoluzione Identità e QR Code

Il problema del QR Code che non veniva riconosciuto è stato risolto tramite:
1. **Disattivazione Test Params**: Disabilitato `CONFIG_ENABLE_TEST_SETUP_PARAMS` che forzava il passcode standard `20202021`.
2. **Factory Provider**: Reindirizzato il firmware a leggere dalla partizione `fctry` invece di `nvs` tramite la label `CONFIG_CHIP_FACTORY_NAMESPACE_PARTITION_LABEL="fctry"`.
3. **NVS Erase**: Esecuzione obbligatoria di `erase_flash` per rimuovere vecchie credenziali memorizzate nella partizione NVS che sovrascrivevano quelle nuove.

## 4. Procedura Operativa da Terminale

Seguire questi passaggi in ordine per caricare l'ambiente e flashare il dispositivo.

### Passaggio A: Caricamento Ambiente (Export)
Aprire il terminale nella cartella del progetto ed eseguire:
```bash
. /Users/matteo/.espressif/v5.5.3/esp-idf/export.sh
. /Users/matteo/esp/esp-matter/export.sh
```

### Passaggio B: Compilazione
```bash
idf.py build
```

### Passaggio C: Flash Manuale (Consigliato)
A causa di possibili conflitti di porta con i processi in background, si consiglia l'uso diretto di `esptool`. Sostituire `/dev/tty.usbmodem11101` con la propria porta seriale se diversa.

1. **Pulizia totale della memoria**:
   ```bash
   python3 -m esptool -p /dev/tty.usbmodem11101 erase_flash
   ```

2. **Scrittura Firmware e Dati Factory**:
   ```bash
   python3 -m esptool -p /dev/tty.usbmodem11101 write_flash \
     0x0 build/bootloader/bootloader.bin \
     0xc000 build/partition_table/partition-table.bin \
     0x51000 build/ota_data_initial.bin \
     0x60000 build/multiple_on_off_plugin_units.bin \
     0x3C0000 mfg_s3_final/fff1_8000/db91e535-aca6-4ff1-8563-af75090f5e22/db91e535-aca6-4ff1-8563-af75090f5e22-partition.bin
   ```

## 5. Verifica Metadati Endpoint
Dopo il flash, il monitor seriale (`idf.py monitor`) deve mostrare i log di inizializzazione per ogni endpoint:
- `app_main: Endpoint X (Plug Y) configurato (Label: Bxx, Serial: D99-Bxx)`
- `chip[DIS]: Advertise commission parameter ... discriminator=1679`

Se il discriminator visualizzato nel log è `1679`, il QR code personalizzato è attivo e funzionante.
