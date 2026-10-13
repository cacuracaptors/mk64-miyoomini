#!/bin/sh
# Compila o Mario Kart 64 para o Miyoo Mini Plus e monta o pacote.
# Roda NO WSL, na raiz do repositorio:   sh port/build_miyoo.sh
#   sh port/build_miyoo.sh clean   -> apaga a compilacao anterior antes
#
# Resultado:
#   ~/fallout-miyoo/mk64-pacote/MarioKart64/   -> /mnt/SDCARD/Roms/PORTS/Games/MarioKart64
#   ~/fallout-miyoo/mk64-pacote/Mario Kart 64.port -> /mnt/SDCARD/Roms/PORTS/Shortcuts
cd "$(dirname "$0")/.." || exit 1

BASE="$HOME/fallout-miyoo"
PKG="$BASE/mk64-pacote"

# --- compilador ARM -------------------------------------------------------------
CROSS=""
# a copia do container pode ter o compilador em bin/ ou em usr/bin/
ACHADO="$(find "$BASE/miyoomini-toolchain" -maxdepth 3 -name arm-linux-gnueabihf-gcc 2>/dev/null | head -1 | sed 's/gcc$//')"
# O compilador ARM do Ubuntu (/usr/bin/arm-linux-gnueabihf-gcc) NAO serve: ele liga o
# programa a uma glibc bem mais nova que a do Miyoo, e o jogo nem abriria no aparelho.
# So o compilador do container (a mesma glibc do OnionOS) e aceito.
for c in "$CROSS_PREFIX" \
         "$ACHADO" \
         "/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-" \
         "/opt/miyoomini-toolchain/bin/arm-linux-gnueabihf-"; do
    if [ -n "$c" ] && [ -x "${c}gcc" ]; then CROSS="$c"; break; fi
done
if [ -z "$CROSS" ]; then
    echo "ERRO: compilador ARM (arm-linux-gnueabihf-gcc) nao encontrado."
    echo "Copie o do container uma vez (Docker Desktop aberto), no WSL:"
    echo "  docker cp miyoomini-toolchain-debug:/opt/miyoomini-toolchain $BASE/miyoomini-toolchain"
    exit 1
fi
echo "compilador: ${CROSS}gcc ($(${CROSS}gcc -dumpversion))"
case "$CROSS" in
    /usr/bin/*) echo "ERRO: ${CROSS}gcc e o compilador do Ubuntu, nao o do Miyoo (veja o comentario acima)."
                echo "Copie o do container: docker cp miyoomini-toolchain-debug:/opt/miyoomini-toolchain $BASE/miyoomini-toolchain"
                exit 1 ;;
esac

[ -f baserom.us.z64 ] || { echo "ERRO: falta baserom.us.z64 na raiz do repositorio"; exit 1; }
command -v python3 >/dev/null || { echo "ERRO: falta python3"; exit 1; }

# trocou de compilador desde a ultima vez? entao recompila tudo
if [ -f build/.compilador ] && [ "$(cat build/.compilador)" != "$CROSS" ]; then
    echo "compilador mudou desde a ultima compilacao: recompilando tudo"
    set -- clean
fi
if [ "$1" = "clean" ] || { [ -d build ] && [ ! -f build/.compilador ]; }; then
    rm -rf build test.o data_segment2.o dc_data
fi
mkdir -p build && echo "$CROSS" > build/.compilador

make -f Makefile.miyoo -j4 CROSS="$CROSS" || {
    echo; echo "=== A COMPILACAO FALHOU (veja as primeiras linhas com 'error') ==="; exit 1; }

[ -f build/mk64-miyoo ] || { echo "ERRO: build/mk64-miyoo nao foi gerado"; exit 1; }
[ -f dc_data/common_data.bin ] || { echo "ERRO: dc_data nao foi gerado"; exit 1; }

# --- pacote ---------------------------------------------------------------------
GAME="$PKG/MarioKart64"
rm -rf "$PKG"
mkdir -p "$GAME/saves" || exit 1
cp build/mk64-miyoo "$GAME/mk64" || exit 1
cp -r dc_data "$GAME/" || exit 1

cat > "$PKG/Mario Kart 64.port" <<'PORT'
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

# -perf: tempos no log.txt a cada 2 s   -hires: renderiza em 640x480 (mais lento)
# -filtroleve: filtro de textura feito de antemao (mais leve)   -filtro: o filtro do N64 calculado na hora
# -semparalelo: desliga o desenho em linha de montagem nos 2 nucleos
# -resdin: baixa a resolucao (ate 448x336) so quando o quadro nao cabe em 60 fps
# -mudo: sem som
Arguments="-perf"

cd "$GameFolder"
/mnt/SDCARD/Emu/PORTS/launch_standalone.sh "$GameName" "$GameDir" "$GameExecutable" "$Arguments" "$GameDataFile" "$KillAudioserver" "$PerformanceMode" > "$GameFolder/log.txt" 2>&1
PORT

echo
echo "=== PACOTE PRONTO em $PKG ==="
du -sh "$GAME"
md5sum "$GAME/mk64"
echo "glibc exigida: $(${CROSS}objdump -T "$GAME/mk64" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
