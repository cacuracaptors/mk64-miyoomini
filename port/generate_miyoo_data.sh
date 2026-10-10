#!/bin/sh
# Collects the game data the Miyoo binary loads at run time into dc_data/
# (same files as the Dreamcast build's generate_dc_data.sh, extracted with the
# ARM objcopy; no AICA ADPCM pool). Run from the repository root.
set -e
OBJCOPY=${OBJCOPY:-arm-linux-gnueabihf-objcopy}
COURSES="mario_raceway choco_mountain bowsers_castle banshee_boardwalk yoshi_valley frappe_snowland koopa_troopa_beach royal_raceway luigi_raceway moo_moo_farm toads_turnpike kalimari_desert sherbet_land rainbow_road wario_stadium block_fort skyscraper double_deck dks_jungle_parkway big_donut"

mkdir -p dc_data
cp build/us/assets/code/common_data/common_data.bin dc_data/
cp build/us/assets/code/ceremony_data/ceremony_data.bin dc_data/
cp bin/audiobanks.us.bin dc_data/audiobanks.bin
cp bin/audiotables.bin dc_data/

$OBJCOPY -O binary --only-section=.data build/us/data/sound_data/instrument_sets.o dc_data/instrument_sets.bin
$OBJCOPY -O binary --only-section=.data build/us/data/sound_data/sequences.o dc_data/sequences.bin

for i in $COURSES; do
    cp build/us/courses/"$i"/course_data.bin dc_data/"$i"_data.bin
    $OBJCOPY -O binary --only-section=.data build/us/courses/"$i"/course_textures.linkonly.o dc_data/"$i"_tex.bin
    $OBJCOPY -O binary --only-section=.data build/us/courses/"$i"/course_geography.mio0.o dc_data/"$i"_geography.bin
done
echo "dc_data pronto: $(ls dc_data | wc -l) arquivos"
