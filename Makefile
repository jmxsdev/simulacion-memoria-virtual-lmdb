# Makefile — Proyecto académico de memoria virtual con LMDB (Fase 1: generador).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
# NOTA: el -lmdb plano falla en este enlazador aunque /usr/lib/liblmdb.so
# exista; enlazar la ruta completa funciona. Ajusta si compilas en otra máquina.
LDLIBS   := /usr/lib/liblmdb.so

BIN := bin
SRC := src

GEN := $(BIN)/generator
QUERY := $(BIN)/query
BENCH := $(BIN)/bench
GEN_SRCS := $(SRC)/generator.cpp
QUERY_SRCS := $(SRC)/query.cpp
BENCH_SRCS := $(SRC)/bench.cpp
COMMON_HDRS := $(SRC)/types.h $(SRC)/keys.h $(SRC)/env.h

.PHONY: all smoke smoke-clean clean

all: $(GEN) $(QUERY) $(BENCH)

$(BIN):
	mkdir -p $(BIN)

$(GEN): $(GEN_SRCS) $(SRC)/types.h $(SRC)/keys.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(GEN_SRCS) -o $@ $(LDLIBS)

$(QUERY): $(QUERY_SRCS) $(COMMON_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(QUERY_SRCS) -o $@ $(LDLIBS)

$(BENCH): $(BENCH_SRCS) $(COMMON_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(BENCH_SRCS) -o $@ $(LDLIBS)

# Prueba de humo: 1M de jugadas, lo demás por defecto; imprime el resumen
# del manifiesto. Determinista: la misma corrida produce el mismo archivo.
smoke: $(GEN)
	./$(GEN) --bets 1000000 --out data/smoke.lmdb

# Elimina SOLO los artefactos de la prueba de humo (nunca los datasets finales).
smoke-clean:
	rm -f data/smoke.lmdb data/smoke.lmdb-lock data/smoke.manifest.json \
	      data/manifest.json

# clean borra solo artefactos de compilación — jamás los datasets generados.
clean:
	rm -rf $(BIN)
