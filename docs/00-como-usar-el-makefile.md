# Cómo usar el Makefile

Este documento explica qué es `make`, cómo está escrito el `Makefile` del
proyecto (que vive en la raíz, `Makefile`) y cómo se usa para compilar, generar
el dataset de prueba y limpiar los artefactos sin tocar los datos.

## 1. Qué es `make` y qué es un Makefile

Compilar a mano un proyecto de C++ no es difícil, pero sí repetitivo:

```sh
g++ -std=c++17 -O2 -Wall -Wextra src/query.cpp -o bin/query /usr/lib/liblmdb.so
```

Cada vez que tocas un archivo tienes que recordar qué compilación le
corresponde, en qué orden, con qué flags y con qué bibliotecas. `make` es una
herramienta que automatiza eso: lee un archivo llamado `Makefile`, donde están
descritas las **reglas** de construcción, y reconstruye **solo lo que cambió**.

La idea central es simple: para cada objetivo (por ejemplo `bin/query`) el
Makefile declara de qué archivos depende (sus **dependencias**). `make` compara
la fecha de modificación del objetivo contra la de sus dependencias:

- Si alguna dependencia es **más nueva** que el objetivo, el objetivo está
  desactualizado y se reconstruye.
- Si el objetivo es más nuevo que todas sus dependencias, no se hace nada.

Es una comparación de marcas de tiempo del sistema de archivos, nada más. Por
eso, si ejecutas `make` dos veces seguidas, la segunda no compila nada: dirá
`make: Nothing to be done for 'all'`.

## 2. Estructura del Makefile del proyecto

El Makefile completo (`Makefile`) es corto, 47 líneas:

```sh
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
	$(CXX) $(CXXFLAGS) $(BENCH_SRCS) -o $@ $(LDLIBS) -lpthread

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
```

Un Makefile tiene tres ingredientes:

1. **Variables**: nombres que guardan texto (`CXX`, `BIN`, `GEN`...).
2. **Objetivos** (targets): lo que se puede construir o ejecutar (`all`,
   `bin/query`, `smoke`, `clean`).
3. **Reglas**: cada objetivo, seguido de `:`, sus dependencias, y en las líneas
   siguientes (con Tabulador, no espacios) los comandos que lo construyen.

### Las variables, una por una

| Variable | Valor | Para qué |
| --- | --- | --- |
| `CXX` | `g++` | El compilador de C++ que se usa. |
| `CXXFLAGS` | `-std=c++17 -O2 -Wall -Wextra` | Flags que se le pasan al compilador. |
| `LDLIBS` | `/usr/lib/liblmdb.so` | Biblioteca(s) con las que se enlaza el binario. |
| `BIN` | `bin` | Carpeta donde quedan los ejecutables. |
| `SRC` | `src` | Carpeta donde está el código fuente. |
| `GEN`, `QUERY`, `BENCH` | `bin/generator`, `bin/query`, `bin/bench` | Rutas de los tres binarios. |
| `GEN_SRCS`, `QUERY_SRCS`, `BENCH_SRCS` | `src/*.cpp` | El `.cpp` de cada binario. |
| `COMMON_HDRS` | `src/types.h src/keys.h src/env.h` | Headers de los que dependen `query` y `bench`. |

Dos detalles de sintaxis importantes:

- `?=` significa "asigna solo si no estaba definida antes". Gracias a esto
  puedes compilar con otro compilador sin editar el Makefile:

  ```sh
  make CXX=clang++
  ```

  `:=` (usado en `LDLIBS`, `BIN`, `SRC`...) significa "asigna ya, con el valor
  que tenga la variable en este instante" (asignación inmediata).

- `$(NOMBRE)` expande una variable. Por eso `$(BIN)/generator` se convierte en
  `bin/generator`.

### Anatomía de una regla: la de `bin/query`

```sh
$(QUERY): $(QUERY_SRCS) $(COMMON_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(QUERY_SRCS) -o $@ $(LDLIBS)
```

Traducido a español:

> El objetivo `bin/query` depende de `src/query.cpp`, `src/types.h`,
> `src/keys.h` y `src/env.h`, y además **necesita que exista** la carpeta `bin`.
> Para construirlo, ejecuta
> `g++ -std=c++17 -O2 -Wall -Wextra src/query.cpp -o bin/query /usr/lib/liblmdb.so`.

Aquí aparecen dos variables automáticas de `make`:

- `$@` es **el nombre del objetivo** de esta regla, es decir `bin/query`. Es la
  salida del `-o`.
- `$<` es **la primera dependencia** de la regla (`src/query.cpp`). En este
  proyecto los comandos escriben `$(QUERY_SRCS)` de forma explícita en lugar de
  `$<`, pero como solo hay un `.cpp`, ambos producen lo mismo. Conviene
  conocerlas porque son las que se usan en Makefiles más compactos.

Y la parte `| $(BIN)` es una **dependencia de orden** (order-only prerequisite).
Significa: "asegúrate de que `bin/` exista antes de construir `query`, pero no
recompiles `query` solo porque `bin/` cambió de fecha". Sin esta distinción,
cada vez que se tocara la carpeta `bin` se reconstruiría todo. La regla que
materializa la carpeta es:

```sh
$(BIN):
	mkdir -p $(BIN)
```

Que dice: "si `bin` no existe, créala con `mkdir -p`".

### La regla de `bin/generator`, con una ausencia deliberada

```sh
$(GEN): $(GEN_SRCS) $(SRC)/types.h $(SRC)/keys.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(GEN_SRCS) -o $@ $(LDLIBS)
```

Compara esta regla con la de `query` y `bench`: a la de `generator` **no** le
aparece `src/env.h`. No es un error. `src/generator.cpp` no incluye `env.h`
(define sus propias utilidades LMDB y su propia clase `SplitMix64`), así que
declarar `env.h` como dependencia sería mentir y provocaría recompilaciones
innecesarias. Esta es una lección de Makefiles: las dependencias deben
**reflejar los `#include` reales**, ni más ni menos.

### Los tres flags de compilación

- `-std=c++17`: el proyecto usa C++17 (`inline constexpr`, `std::filesystem`,
  `enum class` con tipo base, etc.).
- `-O2`: optimización. Es **imprescindible** aquí: los benchmarks miden
  rendimiento y fallos de página, y compilar sin optimizar falsearía los
  resultados. Un binario de depuración no sirve para medir prestaciones.
- `-Wall -Wextra`: activan la mayoría de las advertencias. No cambian el
  binario, pero avisan de posibles errores (variables sin usar, conversiones
  peligrosas, firmas que no coinciden). Compilar sin advertencias es parte de
  tener el código sano.

## 3. Cómo se usa

Desde la raíz del proyecto (donde está el `Makefile`):

```sh
make            # compila las tres herramientas en bin/
make all        # exactamente lo mismo que make (all es el primer objetivo)
make smoke      # compila generator (si hace falta) y genera 1M de jugadas
make smoke-clean # borra solo los artefactos de la prueba de humo
make clean      # borra solo la carpeta bin/ — jamás los datasets
```

Qué hace cada uno y cuándo usarlo:

| Comando | Qué hace | Cuándo usarlo |
| --- | --- | --- |
| `make` (o `make all`) | Construye `bin/generator`, `bin/query` y `bin/bench`. | Tras clonar el repo o tras cambiar código. |
| `make smoke` | Compila `generator` y corre `./bin/generator --bets 1000000 --out data/smoke.lmdb`. | Verificación rápida de que el proyecto funciona de punta a punta. |
| `make smoke-clean` | Borra `data/smoke.lmdb`, `data/smoke.lmdb-lock`, `data/smoke.manifest.json` y `data/manifest.json`. | Cuando ya no necesitas el dataset de humo. |
| `make clean` | Borra `bin/` completa. | Para forzar una compilación limpia; por ejemplo tras cambiar flags. |

El objetivo `all` es el primero del archivo, por eso `make` a secas equivale a
`make all`. `make` siempre construye como máximo el **primer** objetivo del
Makefile si no le indicas otro.

El `smoke` es determinista (misma semilla, mismo archivo). El generador se
niega a pisar un archivo existente, así que si `data/smoke.lmdb` ya existe,
`make smoke` fallará con un mensaje pidiendo borrarlo: ahí es donde entra
`make smoke-clean`.

## 4. Compilación selectiva: por qué tocar un solo archivo no recompila todo

Supón que solo cambias `src/query.cpp` (un ajuste en un comando). Ejecutas:

```sh
touch src/query.cpp
make
```

`make` revisa las reglas y razona así:

- `bin/query` depende de `src/query.cpp`, que ahora es más nuevo → **recompila
  `bin/query`**.
- `bin/generator` depende de `src/generator.cpp`, `src/types.h` y
  `src/keys.h`; ninguno es más nuevo que el binario → **no lo toca**.
- `bin/bench` depende de `src/bench.cpp` y de los headers comunes; ninguno
  cambió → **no lo toca**.

Resultado: recompila solo lo necesario. Ahora prueba con un header:

```sh
touch src/keys.h
make
```

Como `keys.h` es dependencia declarada de los **tres** binarios, los tres se
recompilan. Y este es el caso interesante:

```sh
touch src/env.h
make
```

Recompila `query` y `bench`, pero **no** `generator`, porque `env.h` no está
entre las dependencias de `bin/generator` (y el código lo confirma: `generator`
no lo incluye). Ese comportamiento es exactamente lo que queremos: si el
Makefile declarara de más, se perdería tiempo; si declarara de menos, un cambio
de header no se propagaría y tendrías binarios inconsistentes.

## 5. Los tres gotchas del Makefile

### 5.1 La ruta completa de la biblioteca LMDB

```sh
LDLIBS   := /usr/lib/liblmdb.so
```

En un proyecto típico en Linux uno escribiría `-llmdb` (enlazar `liblmdb.so`
buscándola en las rutas estándar). En esta máquina ese `-lmdb` plano **falla**
aunque el archivo `/usr/lib/liblmdb.so` exista: el enlazador no encuentra un
nombre que pueda resolver. La solución documentada en el propio Makefile es
pasar la **ruta completa** del archivo `.so`, que sí funciona. Si compilas en
otra máquina, revisa dónde está `liblmdb.so` y ajusta la ruta.

### 5.2 `-lpthread` solo para `bench`

```sh
$(BENCH): $(BENCH_SRCS) $(COMMON_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(BENCH_SRCS) -o $@ $(LDLIBS) -lpthread
```

`src/bench.cpp` usa `std::thread` (el modo `write-concurrent` lanza un hilo por
taquilla) y un `std::mutex`. Eso necesita la biblioteca de hilos POSIX, que se
enlaza con `-lpthread`. `generator` y `query` no usan hilos, por eso no la
llevan. Cada binario enlaza lo que realmente necesita.

### 5.3 `clean` nunca borra los datasets

```sh
clean:
	rm -rf $(BIN)
```

`clean` ejecuta `rm -rf bin` y **nada más**. Los datasets viven en `data/`, no
en `bin/`, así que un `make clean` jamás puede borrar un dataset generado
(puede costar horas regenerarlos). La limpieza de artefactos de prueba se
mantiene separada y explícita en `smoke-clean`, que borra nombres concretos de
archivos, no un directorio completo. Es una decisión de diseño: separar
"artefactos de compilación" (desechables) de "datos" (valiosos).

## 6. Preguntas que te pueden hacer en la defensa

**¿Por qué no usar un script `.sh` en lugar de un Makefile?**
Un script recompilaría todo siempre; `make` compara fechas y recompila solo lo
que cambió. Además el Makefile declara las dependencias de forma explícita, así
que el orden y las bibliotecas de cada binario quedan documentados y no
dependen de que alguien recuerde la línea exacta de `g++`.

**¿Qué pasa si ejecuto `make` dos veces seguidas?**
La segunda no compila nada: todos los binarios son más nuevos que sus fuentes y
`make` responde `make: Nothing to be done for 'all'`.

**¿Qué es `.PHONY` y por qué importa aquí?**
`.PHONY` marca objetivos que **no** corresponden a archivos reales (`all`,
`smoke`, `smoke-clean`, `clean`). Sin esa marca, si existiera por accidente un
archivo llamado `clean`, `make clean` no ejecutaría el comando: creería que el
objetivo ya está construido. Con `.PHONY` se le dice a `make` que siempre los
ejecute.

**¿Por qué `-O2` y no compilar sin optimizar?**
Porque el proyecto mide rendimiento (registros/s, latencias, fallos de página).
Un binario sin optimizar no representa el costo real del programa y falsearía
las conclusiones sobre localidad y paginación.

**¿Por qué `-Wall -Wextra`?**
Activan las advertencias del compilador. No generan código distinto, pero
detectan errores latentes. Compilar limpio, sin advertencias, es señal de un
código cuidado.

**¿Para qué sirve la dependencia de orden `| $(BIN)`?**
Para garantizar que la carpeta `bin/` exista antes de compilar, sin que el
timestamp de la carpeta fuerce recompilaciones innecesarias.

**¿Qué significan `$@` y `$<` en una regla?**
`$@` es el nombre del objetivo (la salida, lo que va tras `-o`) y `$<` es la
primera dependencia (normalmente el `.cpp` de entrada).

**¿Por qué la regla de `generator` no depende de `env.h`?**
Porque `src/generator.cpp` no incluye `env.h`: define sus propias utilidades
LMDB. Declarar una dependencia que no existe en los `#include` solo provoca
recompilaciones de más.
