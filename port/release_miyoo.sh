#!/bin/sh
# Monta o zip de lancamento (roda NO WSL, na raiz do repositorio):
#   sh port/release_miyoo.sh            -> versao 1.0.0
#   sh port/release_miyoo.sh 1.0.1
#
# O zip nao leva nenhum dado do jogo: o executavel vai com os dados da ROM
# zerados e, no lugar de dc_data/, vai a receita mk64.rcp, que so diz de onde
# tirar cada pedaco da ROM do jogador (port/romkit/romkit.h). Na primeira vez que
# o jogo abre, ele refaz dc_data/ a partir da ROM e apaga a ROM
# (port/setup_miyoo.c).
#
# Resultado no Desktop do Windows: MarioKart64-Miyoo-v<versao>.zip, para extrair
# na raiz do cartao:
#   Roms/PORTS/Games/MarioKart64/        mk64, mk64.rcp, saves/
#   Roms/PORTS/Shortcuts/Mario Kart 64.port
#   Roms/PORTS/Imgs/Mario Kart 64.png    (copiada do Desktop)
cd "$(dirname "$0")/.." || exit 1
VER="${1:-1.0.0}"
DESK="/mnt/c/Users/Régis/Desktop"
IMG="$DESK/Mario Kart 64.png"
ZIP="$DESK/MarioKart64-Miyoo-v$VER.zip"
ROM=baserom.us.z64
MAP=build/us/mario-kart.elf.map
T=build/release

[ -f "$IMG" ] || { echo "ERRO: falta a imagem $IMG"; exit 1; }

# --- compilacao normal (gera build/mk64-miyoo e dc_data/) -------------------------
sh port/build_miyoo.sh || exit 1
for f in "$ROM" "$MAP" build/mk64-miyoo dc_data/common_data.bin; do
    [ -f "$f" ] || { echo "ERRO: falta $f"; exit 1; }
done

rm -rf $T && mkdir -p $T || exit 1
gcc -O2 -o $T/romrecipe port/romkit/romrecipe.c port/romkit/romkit.c || { echo "ERRO: gcc (romrecipe)"; exit 1; }

# --- 1. os dados do jogo saem do executavel ---------------------------------------
# os objetos que levam dados da ROM para dentro do executavel (veja port/romkit/analisar.sh)
OBJS="test.o data_segment2.o build/us/data/textures_0a.o build/us/data/other_textures.o build/us/data/texture_tkmk00.o build/us/assets/code/startup_logo/startup_logo.mio0.o build/us/assets/code/ceremony_data/ceremony_data.mio0.o"
python3 port/romkit/romdata.py strip "$MAP" build/mk64-miyoo $T/mk64 $T/romdata.bin $OBJS || exit 1
python3 port/romkit/romdata.py apply $T/mk64 $T/romdata.bin $T/mk64-refeito || exit 1
cmp -s $T/mk64-refeito build/mk64-miyoo || { echo "ERRO: o executavel refeito nao bate com o original"; exit 1; }

# --- 2. a receita: romdata.bin e todo o dc_data/ ------------------------------------
ARGS="dc_data/romdata.bin=$T/romdata.bin"
for f in dc_data/*; do
    [ -f "$f" ] && ARGS="$ARGS $f=$f"
done
$T/romrecipe make "$ROM" $T/mk64.rcp $ARGS > $T/receita.txt 2>&1 || { cat $T/receita.txt; echo "ERRO: receita"; exit 1; }
tail -1 $T/receita.txt

# --- 3. conferencia: tudo refeito a partir da ROM, byte a byte -----------------------
$T/romrecipe apply "$ROM" $T/mk64.rcp $T/refeito > $T/conferencia.txt 2>&1 || { cat $T/conferencia.txt; echo "ERRO: apply"; exit 1; }
cmp -s $T/refeito/dc_data/romdata.bin $T/romdata.bin || { echo "ERRO: romdata.bin refeito diferente"; exit 1; }
N=0
for f in dc_data/*; do
    [ -f "$f" ] || continue
    cmp -s "$T/refeito/$f" "$f" || { echo "ERRO: $f refeito diferente"; exit 1; }
    N=$((N + 1))
done
echo "conferencia: romdata.bin e $N arquivos de dc_data refeitos iguais a partir da ROM"

# --- 4. o pacote -----------------------------------------------------------------
P=$T/pacote
GAME="$P/Roms/PORTS/Games/MarioKart64"
mkdir -p "$GAME/saves" "$P/Roms/PORTS/Shortcuts" "$P/Roms/PORTS/Imgs" || exit 1
cp $T/mk64 "$GAME/mk64" && chmod 755 "$GAME/mk64" || exit 1
cp $T/mk64.rcp "$GAME/" || exit 1
cp "$IMG" "$P/Roms/PORTS/Imgs/Mario Kart 64.png" || exit 1

cat > "$P/Roms/PORTS/Shortcuts/Mario Kart 64.port" <<'PORT'
#!/bin/sh
GameName="Mario Kart 64"
GameDir="MarioKart64"
GameExecutable="mk64"
GameDataFile="mk64"

KillAudioserver=1
PerformanceMode=1

FullGamePath="/mnt/SDCARD/Roms/PORTS/Games/$GameDir"
GameFolder="$FullGamePath"
export HOME="$FullGamePath"

# Options (add or remove them in Arguments below):
#   -hires    render at 640x480 (without it: 320x240, faster and blurrier)
#   -60fps    single-player races at up to 60 fps (without it: 30 fps, as on the N64)
#   -resdin   dynamic resolution: 640x480 frames drop to as low as 448x336 only while
#             a frame doesn't fit in 1/60 s
#   -filtroleve  texture filter on the road and other 3D surfaces, worked out in
#             advance (looks like the N64's filter, costs about half as much)
#   -filtro   the N64's texture filter itself, computed as it draws (slower)
#   -mudo     no sound
#   -perf     timing lines in log.txt every 2 s
Arguments="-hires -60fps -resdin -filtroleve"

cd "$GameFolder"
/mnt/SDCARD/Emu/PORTS/launch_standalone.sh "$GameName" "$GameDir" "$GameExecutable" "$Arguments" "$GameDataFile" "$KillAudioserver" "$PerformanceMode" > "$GameFolder/log.txt" 2>&1
PORT
chmod 755 "$P/Roms/PORTS/Shortcuts/Mario Kart 64.port"

# nada de dados do jogo no pacote
if find "$P" -iname "*.z64" -o -iname "*.n64" -o -iname "*.v64" -o -name "dc_data" -o -name "romdata.bin" | grep -q .; then
    echo "ERRO: o pacote tem ROM ou dados do jogo"; exit 1
fi

rm -f "$ZIP"
python3 - "$P" "$ZIP" <<'PY' || { echo "ERRO: zip"; exit 1; }
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, dirs, files in sorted(os.walk(root)):
        dirs.sort()
        rel = os.path.relpath(d, root)
        if rel != "." and not files and not dirs:
            info = zipfile.ZipInfo(rel + "/")   # saves/
            info.external_attr = (0o40755 << 16) | 0x10
            z.writestr(info, "")
        for f in sorted(files):
            p = os.path.join(d, f)
            info = zipfile.ZipInfo.from_file(p, os.path.relpath(p, root))
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(p, "rb") as fh:
                z.writestr(info, fh.read())
PY

echo
echo "=== ZIP PRONTO: $ZIP ==="
python3 -c "import zipfile,sys; [print('  %9d  %s' % (i.file_size, i.filename)) for i in zipfile.ZipFile(sys.argv[1]).infolist()]" "$ZIP"
ls -l "$ZIP" | awk '{print "tamanho do zip: " $5 " bytes"}'
md5sum "$GAME/mk64" "$GAME/mk64.rcp"
