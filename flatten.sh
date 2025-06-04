#!/bin/bash
set -euo pipefail

# Step 1: Deinit and clean out submodule metadata
echo "[*] Deinitializing submodules..."
git submodule deinit -f --all || true
rm -rf .git/modules
rm -f .gitmodules

# Step 2: Remove submodule entries from index
echo "[*] Removing submodule entries from index..."
git ls-files --stage | grep '^160000' | cut -f2- | while read path; do
    git rm --cached "$path"
done

# Step 3: Re-clone all submodule repos directly into their old paths
echo "[*] Re-cloning submodules into working tree..."
git clone https://github.com/grblHAL/Plugin_EEPROM.git      main/eeprom
git clone https://github.com/grblHAL/Plugin_embroidery.git  main/embroidery
git clone https://github.com/grblHAL/core.git               main/grbl
git clone https://github.com/grblHAL/Plugin_I2C_keypad.git  main/keypad
git clone https://github.com/grblHAL/Plugins_laser.git      main/laser
git clone https://github.com/grblHAL/Plugins_motor.git      main/motors
git clone https://github.com/grblHAL/Plugin_networking.git  main/networking
git clone https://github.com/grblHAL/Plugin_OpenPNP.git     main/openpnp
git clone https://github.com/grblHAL/Plugins_misc.git       main/plugins
git clone https://github.com/grblHAL/Plugin_SD_card.git     main/sdcard
git clone https://github.com/grblHAL/Plugins_spindle.git    main/spindle
git clone https://github.com/terjeio/Trinamic-library.git   main/trinamic
git clone https://github.com/grblHAL/Plugin_WebUI.git       main/webui

# Step 4: Stage everything
echo "[*] Staging all new files..."
git add .

# Step 5: Commit
echo "[*] Committing changes..."
git commit -m "Flatten all submodules into normal directories"

echo "[✓] Done: Submodules flattened and committed into master"