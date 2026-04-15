# Guida Tecnica Provisioning Matter (ESP32-C6)

Questa guida documenta i passaggi tecnici eseguiti per personalizzare l'identità del dispositivo Matter (Manufacturer Name, Product Name, Certificazioni) e risolvere l'errore "Test Vendor".

## Overview del Sistema
L'ecosistema Matter richiede che il dispositivo presenti certificati di attestazione (DAC e PAI) validi durante l'onboarding (PASE). Per evitare di usare i dati di test predefiniti (Test Vendor), il sistema è stato configurato per leggere queste informazioni da una partizione dedicata chiamata `fctry`.

### Indirizzi di Memoria
*   **Partizione Factory (`fctry`)**: `0x3E0000` (Dimensione: 0x6000)
*   **Partition Table**: `0xc000`
*   **App Firmware**: `0x20000`

---

## 1. Configurazione `sdkconfig`
Per abilitare il recupero dell'identità dalla partizione di fabbrica, sono stati modificati i seguenti parametri nel file `sdkconfig`:

### Abilitazione Factory Data Provider
Istruisce il firmware a cercare una partizione NVS chiamata `fctry`.
```conf
CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER=y
CONFIG_CHIP_FACTORY_NAMESPACE_PARTITION_LABEL="fctry"
```

### DAC & Info Providers (Cruciale per i Nomi)
Disabilitiamo i provider di esempio e attiviamo quelli che leggono dalla partizione factory:
```conf
# DAC Provider (Certificati DAC/PAI)
# CONFIG_EXAMPLE_DAC_PROVIDER is not set
CONFIG_FACTORY_PARTITION_DAC_PROVIDER=y

# Device Info Provider (Manufacturer/Product Names)
CONFIG_ENABLE_ESP32_DEVICE_INSTANCE_INFO_PROVIDER=y
# CONFIG_EXAMPLE_DEVICE_INSTANCE_INFO_PROVIDER is not set
CONFIG_FACTORY_DEVICE_INSTANCE_INFO_PROVIDER=y
```

---

## 2. Personalizzazione dei Nomi (Vendor & Product Name)
Per cambiare il nome che appare nell'app Casa (es: "Elettronica Mimmo" invece di "Test Vendor"), devi agire su due fronti:

### A. Parametri del mfg_tool
I nomi vengono inseriti nel binario factory tramite questi due flag del comando di generazione:
*   `--vendor-name "Tuo Nome Produttore"`
*   `--product-name "Tuo Nome Prodotto"`
*   `--product-label "Versione Prodotto"` (Opzionale)

### B. Collegamento nel Firmware
Affinché il firmware non usi i nomi di default, in `sdkconfig` deve essere attivo:
```conf
CONFIG_FACTORY_DEVICE_INSTANCE_INFO_PROVIDER=y
```
Questo dice all'applicazione: "Invece di restituire la stringa statica 'Test Vendor', vai a leggerla nella partizione NVS `fctry` sotto la chiave opportuna".

---

## 3. Generazione Factory Data (`mfg_tool.py`)
È stato utilizzato il tool di produzione di ESP-Matter per generare il file binario della partizione `fctry`. Per soddisfare i requisiti di Apple Home, abbiamo incluso i certificati di test ufficiali di Matter (Chain PAI -> DAC).

### Parametri Utilizzati
*   **Vendor ID (VID)**: `0xFFF1` (65521)
*   **Product ID (PID)**: `0x8000` (32768)
*   **Discriminator**: `1679`
*   **Passcode**: `92226036`

### Comando di Generazione Completa
Assicurati di usare il percorso dei certificati all'interno della tua installazione `esp-matter`:

```bash
python3 mfg_tool.py \
  --vendor-id 0xFFF1 \
  --product-id 0x8000 \
  --vendor-name "Elettronica Mimmo" \
  --product-name "Domo99" \
  --product-label "Domo99" \
  --discriminator 1679 \
  --passcode 92226036 \
  --outdir mfg_output \
  --dac-cert /Users/matteo/esp/esp-matter/connectedhomeip/connectedhomeip/credentials/development/attestation/Matter-Development-DAC-FFF1-8000-Cert.der \
  --dac-key /Users/matteo/esp/esp-matter/connectedhomeip/connectedhomeip/credentials/development/attestation/Matter-Development-DAC-FFF1-8000-Key.der \
  --cert /Users/matteo/esp/esp-matter/connectedhomeip/connectedhomeip/credentials/development/attestation/Matter-Development-PAI-FFF1-noPID-Cert.der \
  --pai \
  --cert-dclrn /Users/matteo/esp/esp-matter/examples/light/main/certification_declaration/certification_declaration.der
```

---

## 3. Flashing del Dispositivo
Una volta generato il file binario (es: `partition.bin`), deve essere caricato all'offset corretto.

### Comando esptool
Per semplicità su Mac, abbiamo usato il device `/dev/tty.usbmodem11101`:

```bash
esptool.py -p /dev/tty.usbmodem11101 write-flash 0x3E0000 mfg_output/.../partition.bin
```

---

## Troubleshooting Comuni (Problemi e Soluzioni)

### 1. Errore PASE (Error 38 / 0x26)
**Problema**: L'onboarding fallisce immediatamente durante la verifica del Passcode (PAKE).
**Soluzione**: 
*   **Reset NVS**: Lo stato del Fabric precedente potrebbe essere ancora in memoria. Esegui `esptool.py erase_flash` e riflasha tutto.
*   **Verifica QR/Passcode**: Assicurati che lo `summary-xxx.csv` della partizione generata riporti lo stesso `passcode` che stai inserendo nell'app Casa.
*   **Re-pairing**: Se avevi già aggiunto il dispositivo, rimuovilo esplicitamente dalle impostazioni dell'App Casa e riavvia il Bluetooth del telefono.

### 2. Failed CertificateChainRequest (201)
**Problema**: Apple Home vede il dispositivo ma fallisce l'aggiunta perché non trova i certificati DAC o PAI.
**Soluzione**: 
*   **Generazione Completa**: Assicurati di aver generato la partizione usando i flag `--dac-cert`, `--dac-key`, `--cert` (PAI) e `--cert-dclrn` (CD). Una generazione "base" senza questi certificati non funzionerà con controller rigorosi come Apple o Google.
*   **DAC Provider**: Verifica in `sdkconfig` che sia attivo `CONFIG_FACTORY_PARTITION_DAC_PROVIDER=y`.

### 3. Port Busy (Errno 35 / 16)
**Problema**: `esptool` non riesce ad aprire la porta seriale.
**Soluzione**: 
*   **Chiudi il Monitor**: Esci da `idf.py monitor` o chiudi il plugin del Serial Monitor in VS Code.
*   **Usa TTY**: Su macOS, prova a usare `/dev/tty.usbmodemXXX` invece di `/dev/cu.usbmodemXXX` se la porta risulta bloccata.
*   **Processi appesi**: Usa `killall python` se un processo di monitor precedente è rimasto appeso in background.

### 4. Nomi "Test Vendor" persistenti
**Problema**: Il dispositivo è aggiunto ma vedi ancora i nomi di default.
**Soluzione**: 
*   **Device Info Provider**: Verifica in `sdkconfig` che `CONFIG_FACTORY_DEVICE_INSTANCE_INFO_PROVIDER=y` sia attivo.
*   **Re-build**: Ricordati che dopo aver cambiato queste opzioni in `sdkconfig`, è obbligatorio eseguire `idf.py build` perché il cambio è a livello di codice firmware.

