# Guía de lectura del código

El proyecto son 6 archivos fuente y unas 2.537 líneas. No conviene abrirlos en
orden alfabético: el código está diseñado para leerse de abajo hacia arriba, del
vocabulario al rendimiento. Esta guía explica primero cómo está organizado el
código en C++ y después propone un orden de lectura con el porqué de cada paso.

## 1. Qué es un archivo `.h` y por qué existen

Si vienes de C, ya conoces los headers. En C++ la idea es la misma, pero hay
matices que conviene tener claros.

Un archivo `.h` (header) **declara** cosas para que otros archivos puedan
usarlas. Un archivo `.cpp` **define** el comportamiento. El compilador compila
cada `.cpp` por separado y después el enlazador une todo. Para que `query.cpp`
pueda llamar a una función definida en otro sitio, necesita **ver su
declaración**: eso es lo que le da el `#include`.

Tres piezas clave:

- `#pragma once`: al inicio de cada header. Le dice al preprocesador que, si el
  archivo ya fue incluido en esta unidad de compilación, lo ignore. Es la forma
  moderna y portable de evitar la doble inclusión (el clásico
  `#ifndef ... #define ... #endif`). En este proyecto lo usan los tres headers:
  `types.h`, `keys.h` y `env.h`.

- `inline` en los headers: las funciones de `keys.h` y `env.h` están marcadas
  `inline`. La razón es la **regla de definición única** (ODR, *One Definition
  Rule*): en C++ una función no puede definirse dos veces en un programa. Si
  pusieras una función normal con cuerpo en un header incluido por varios
  `.cpp`, al enlazar tendrías múltiples definiciones y el enlazador daría error.
  `inline` permite que la misma definición aparezca en varias unidades de
  compilación sin violar la ODR.

- `#include <...>` con ángulos incluye headers del sistema (`<cstdint>`,
  `<cstring>`, `<lmdb.h>`); `#include "..."` con comillas incluye archivos del
  proyecto (`"types.h"`, `"keys.h"`, `"env.h"`).

Diferencia entre **declaración** y **definición**: una declaración dice "esto
existe y tiene esta forma" (por ejemplo, el prototipo de una función o la firma
de un struct); una definición dice "esto es, ocupa memoria, se ejecuta así".
`types.h` solo declara tipos y constantes: no produce código por sí mismo.

Por eso `types.h`, `keys.h` y `env.h` son **solo headers**: contienen
definiciones `inline` y tipos, y no se compilan a un binario. Los tres `.cpp`
(`generator.cpp`, `query.cpp`, `bench.cpp`) sí contienen una función `main()`
cada uno y producen tres ejecutables distintos. Un header no "se compila"; se
incluye dentro de un `.cpp`.

## 2. El `namespace lotto`

Todo el código del dominio está dentro de `namespace lotto` (y las utilidades de
claves, además, en `namespace lotto::keys`). Un `namespace` es un apellido
compartido que evita choques de nombres: `lotto::Bet` no colisiona con un
`Bet` de otra biblioteca. Como los `.cpp` escriben `using namespace lotto;`,
dentro de ellos basta con `Bet`, `kAnimalitoCount`, etc.

Un detalle de C++ moderno que verás en `types.h`: las constantes son
`inline constexpr` (por ejemplo `kAnimalitoCount`). `constexpr` = valor de
compilación; `inline` = puede definirse en un header incluido por varios `.cpp`
sin romper la ODR. Es la manera correcta de poner constantes en un header.

## 3. Orden de lectura recomendado

Este es el orden, con la razón de cada paso:

### Paso 1. `src/types.h` (91 líneas) — el vocabulario

Aquí se define **qué existe** en el dominio, sin hablar todavía de bytes ni de
LMDB. Antes de entender cómo se guarda una jugada hay que saber qué es una
jugada.

Qué mirar: `enum class Lottery` (6 loterías), `enum class BetType` (Animalito,
Terminal, Tripleta), `struct Bet` (la jugada en memoria) y `struct Draw` (el
resultado de un sorteo), más las constantes del dominio.

- `kAnimalitoCount = 37`, `kTerminalCount = 100`: cuántos animalitos y
  terminales hay.
- `kTsEpoch = 1577836800`: la época del dataset (2020-01-01 UTC). Las marcas de
  tiempo se guardan como segundos desde ahí.
- `kBetsValueSize = 16`, `kIndexValueSize = 8`, `kDrawsValueSize = 2`: los
  tamaños **en disco** de los valores (no confundir con el tamaño en memoria).
- `Bet` y `Draw` son la representación **en memoria**; el comentario del código
  lo dice explícitamente: `struct Bet` no es el formato en disco.

### Paso 2. `src/keys.h` (350 líneas) — el corazón del proyecto

Aquí se traduce el vocabulario a bytes. Es la parte más importante de entender
para la defensa, porque concentra la regla crítica: LMDB compara claves con
`memcmp` (byte a byte), así que todo entero multi-byte en una **clave** va en
**big-endian**, y los **valores** van en little-endian.

Qué mirar:

- Primitivas de serialización: `put_be16`, `put_be32`, `put_be64` y sus lectores
  `get_be16`, `get_be32`, `get_be64` (big-endian, para claves); `put_le64` y
  `get_le64` (little-endian, para valores).
- `encode_bets_key` / `decode_bets_key`: clave de 13 bytes.
- `encode_index_key` / `decode_index_key`: clave de 16 bytes para
  `i_animalito` e `i_terminal`.
- `encode_taquilla_key` / `decode_taquilla_key`: clave de 16 bytes para
  `i_taquilla`.
- `encode_draws_key` / `decode_draws_key`: clave de 6 bytes.
- `encode_bets_value` / `decode_bets_value`: valor de 16 bytes.
- `encode_index_value`: valor de 8 bytes; `encode_draws_value`: valor de 2.

`keys.h` no incluye `lmdb.h` ni toca LMDB: es una **capa pura** de conversión
bytes ↔ campos. Que esté aislada es lo que permite razonar y probar la
codificación sin base de datos.

### Paso 3. `src/env.h` (94 líneas) — abrir LMDB y utilidades comunes

Aquí están las piezas que comparten `query` y `bench` (y que `generator`
reimplementa por su cuenta).

Qué mirar:

- La macro `LMDB_CHECK(call)`: envuelve cada llamada a LMDB y lanza una
  excepción con `mdb_strerror` si el código de retorno no es `MDB_SUCCESS`. Es
  la razón por la que todo el código usa `try/catch` en `main`.
- `struct DbHandles`: el entorno (`MDB_env*`) más los 5 manejadores `MDB_dbi`.
- `open_readonly()`: crea y abre el entorno en modo archivo único
  (`MDB_NOSUBDIR`) y solo lectura.
- `abrir_subbases(txn, db)`: abre las 5 sub-bases **dentro** de una transacción.
- `close_env()`.
- `class SplitMix64`: el generador pseudoaleatorio determinista.

Importante: el comentario de `env.h` documenta un **gotcha** de esta build de
LMDB (los manejadores de sub-base dejan de ser válidos al abortar la
transacción de lectura). Eso obliga a un patrón que verás repetido en cada
comando. Está explicado en detalle en
[03-lmdb-en-este-proyecto.md](03-lmdb-en-este-proyecto.md).

### Paso 4. `src/generator.cpp` (690 líneas) — el primer binario

Es el binario que **crea** el dataset. Se lee antes que `query` porque define el
formato que luego se consulta.

Qué mirar, en este orden dentro del archivo:

- `struct Params` y `parse_args`: los parámetros (`--bets`, `--years`,
  `--taquillas`, `--seed`, `--out`, `--map-gb`, `--batch-txn`).
- `open_env`: abre el entorno escribible y crea las 5 sub-bases.
- `write_bet`: escribe una jugada en `bets` y en los índices que le
  corresponden. Es el punto donde el dominio se convierte en bytes.
- `make_bet`: construye una jugada determinista a partir del PRNG.
- `self_test_key_order`: comprueba con un cursor que las marcas de tiempo no
  descienden; si el big-endian estuviera roto, falla aquí.
- `write_manifest`: escribe `<dataset>.manifest.json` con los conteos exactos
  (la "verdad absoluta" que valida `query`).
- `main`: el flujo completo.

Además verás clases auxiliares en el espacio anónimo: `SplitMix64` (copia local,
con `next_unit`), `WeightedPicker` (selección por pesos), `LotteryConfig`
(horarios de sorteo) y `Counters` (contadores de validación).

### Paso 5. `src/query.cpp` (548 líneas) — cómo se lee el dataset

Implementa 8 comandos de solo lectura y la validación contra el manifiesto.

Qué mirar:

- `class Scannner` (sí, con tres enes en el código): envuelve el patrón de
  cursor "posicionarse en un prefijo y avanzar mientras se mantenga".
- Los 8 comandos: `cmd_info`, `cmd_lookup`, `cmd_range`,
  `cmd_animalito_stats`, `cmd_terminal_stats`, `cmd_racha`, `cmd_taquilla`,
  `cmd_validate`.
- `main`: parser de argumentos (`--db`, comando, flags) y despacho. Si no se
  da comando, usa `info` por defecto.
- `cmd_validate`: la prueba de corrección. Recorre las sub-bases y compara cada
  agregado contra el manifiesto. El resultado esperado es 149/149.

Fíjate en el patrón: **cada** comando hace `mdb_txn_begin` → `abrir_subbases` →
consulta → `mdb_txn_abort`. Ese es el gotcha de `env.h` en acción.

### Paso 6. `src/bench.cpp` (764 líneas) — cómo se mide el rendimiento

Es el más avanzado (hilos, `madvise`, lectura de `/proc`), por eso va de último.
Cuando llegues aquí ya debes entender claves, índices y el formato del dataset.

Qué mirar:

- `run_write_bench` / `run_write_concurrent` y `taquilla_writer`: benchmarks de
  escritura (ordenada, aleatoria y con N taquillas).
- `percentile`: función de percentiles reutilizada en todos los reportes.
- `correr_read_lookup`, `correr_read_scan`, `correr_read_index`: benchmarks de
  lectura, en modo tibio o frío.
- `soltar_cache`, `encontrar_mmap`, `aplicar_hint_madvise`: la técnica de caché
  fría con `madvise` sobre la región `mmap` y `posix_fadvise`.
- `muestrear_claves`: toma claves reales del dataset (una clave inventada al
  azar casi nunca existe y no ejercitaría la lectura real de páginas).
- `leer_fallos`: lee minflt/majflt de `/proc/self/stat` antes y después de cada
  corrida.
- `main`: parser de modos y flags (`--cold`, `--force`, `--n`, `--taquillas`...).

## 4. Diagramas

### 4.1 Flujo de datos: de struct en memoria a disco

```text
        Programa (C++)                         LMDB (B+tree en mmap)        Disco
  ┌──────────────────────────┐
  │ struct Bet               │
  │  lottery  (uint8_t)      │
  │  ts       (uint64_t)     │
  │  taquilla (uint16_t)     │
  │  seq      (uint16_t)     │
  │  type     (BetType)      │
  │  sel0/1/2 (uint8_t)      │
  │  amount_cents (uint64_t) │
  └────────────┬─────────────┘
               │  write_bet()            keys.h
               ▼
  ┌──────────────────────────┐      ┌─────────────────────────────┐
  │ CLAVE  encode_bets_key   │      │ VALOR  encode_bets_value    │
  │  13 bytes BIG-endian     │      │  16 bytes (monto LE + res.) │
  └────────────┬─────────────┘      └──────────────┬──────────────┘
               └──────────────┬────────────────────┘
                              ▼
                  mdb_put(txn, bets, &k, &v, MDB_APPEND)
                              ▼
  ┌───────────────────────────────────────────────────────────────┐
  │ Páginas de 4 KB del B+tree dentro del mapa de memoria (mmap)   │
  │   — residentes en memoria → acceso rápido (RAM)                │
  │   — no residentes → fallo de página → I/O al disco             │
  └───────────────────────────────┬───────────────────────────────┘
                                  ▼  commit (fsync)
                       archivo único data/*.lmdb en disco
```

La misma jugada se escribe además en los índices (`i_animalito`, `i_terminal`,
`i_taquilla`) con sus propias claves de 16 bytes y valores de 8 bytes. Las
claves de índice empiezan por el campo por el que se indexa, para que todas las
jugadas de un mismo animalito (o terminal, o taquilla) queden contiguas.

### 4.2 Dependencias entre archivos (`#include`)

```text
   generator.cpp            query.cpp              bench.cpp
        │                       │                      │
        ├──► keys.h             ├──► env.h ◄───────────┤
        │                       │      │               │
        └──► types.h            ├──► keys.h            ├──► keys.h
                                │      │               │
                                └──► types.h           └── (types.h vía env.h)

   types.h  ──►  <cstdint>                       (sin dependencias del proyecto)
   keys.h   ──►  <array> <cstdint> <cstring> <stdexcept> <string>
   env.h    ──►  <lmdb.h>, types.h, <cstdint> <stdexcept> <string>
```

Lectura del diagrama: `generator.cpp` no usa `env.h` (define lo suyo), por eso
su regla del Makefile no depende de ese header. `query.cpp` y `bench.cpp` sí
usan `env.h`. `types.h` y `keys.h` son la base que todos comparten.

## 5. Tabla de apoyo: las 5 sub-bases como mapa

Cuando te pierdas durante la lectura, vuelve a esta tabla: te dice qué base
resuelve qué consulta y con qué forma de clave.

| Base | Propósito | Clave | Valor |
| --- | --- | --- | --- |
| `bets` | registro maestro | 13 B: lotería, ts, taquilla, seq | 16 B |
| `i_animalito` | jugadas donde aparece un animalito | 16 B: lotería, animalito, ts, taquilla, seq | 8 B (monto) |
| `i_terminal` | frecuencia de terminales | 16 B: lotería, terminal, ts, taquilla, seq | 8 B (monto) |
| `i_taquilla` | replay por punto de venta | 16 B: taquilla, ts, lotería, seq | 8 B (monto) |
| `draws` | ganadores de los sorteos | 6 B: lotería, día, sorteo | 2 B |

## 6. Consejo de lectura activa

- **Empieza por los `main()`.** Cada `.cpp` tiene un `main()` que cuenta el
  flujo completo en dos pantallas. Léelo primero y luego baja a las funciones.
- **Sigue el flujo, no el orden del archivo.** En `generator.cpp`, por ejemplo,
  el camino útil es `parse_args` → `open_env` → `make_bet` → `write_bet` →
  `self_test_key_order` → `write_manifest`.
- **Ten la tabla de sub-bases a mano.** Cada consulta del proyecto es una
  variación de "posicionarse en un prefijo y avanzar".
- **Pregúntate siempre qué byte va primero.** Si te topas con una clave, piensa
  por qué ese campo va al inicio: es lo que determina qué consultas son baratas
  (contiguas) y cuáles no.
