#!/bin/sh
# Analise para o lancamento (roda NO WSL, na raiz do repositorio, depois de um
# sh port/build_miyoo.sh): mede quanto dos dados do jogo que o port usa (dentro
# do executavel e em dc_data/) pode ser refeito a partir da ROM. So gera numeros
# (nada da ROM sai do seu computador): analise_rom.txt no Desktop do Windows.
cd "$(dirname "$0")/../.." || exit 1
ROM=baserom.us.z64
MAP=build/us/mario-kart.elf.map
BIN=build/mk64-miyoo
OUT="/mnt/c/Users/Régis/Desktop/analise_rom.txt"
T=/tmp/mk64-romkit
for f in "$ROM" "$MAP" "$BIN" dc_data/common_data.bin; do
    [ -f "$f" ] || { echo "ERRO: falta $f (rode antes: sh port/build_miyoo.sh)"; exit 1; }
done
mkdir -p $T
gcc -O2 -o $T/romrecipe port/romkit/romrecipe.c port/romkit/romkit.c || { echo "ERRO: gcc"; exit 1; }

# os objetos que hoje levam dados do jogo para dentro do executavel
OBJS="test.o data_segment2.o build/us/data/textures_0a.o build/us/data/other_textures.o build/us/data/texture_tkmk00.o build/us/assets/code/startup_logo/startup_logo.mio0.o build/us/assets/code/ceremony_data/ceremony_data.mio0.o"

{
    echo "=== 1. secoes do executavel com dados da ROM (todas as secoes de dados, de todos os objetos)"
    python3 port/romkit/romdata.py sections "$MAP" "$BIN" $T/secoes.bin
    $T/romrecipe scan "$ROM" $T/secoes.bin 2>&1
    echo
    echo "=== 2. receita: dados tirados do executavel + dc_data"
    python3 port/romkit/romdata.py strip "$MAP" "$BIN" $T/mk64-publicavel $T/romdata.bin $OBJS
    ARGS="romdata.bin=$T/romdata.bin"
    for f in dc_data/*; do ARGS="$ARGS $f=$f"; done
    $T/romrecipe make "$ROM" $T/receita.bin $ARGS 2>&1
    echo
    echo "=== 3. conferencia: refazendo tudo a partir da ROM"
    rm -rf $T/refeito
    $T/romrecipe apply "$ROM" $T/receita.bin $T/refeito 2>&1 | grep -c " ok$" | sed 's/^/arquivos certos: /'
    $T/romrecipe apply "$ROM" $T/receita.bin $T/refeito 2>&1 | grep "ERRADO" || echo "nenhum errado"
} > "$OUT" 2>&1
echo "pronto: $OUT"
