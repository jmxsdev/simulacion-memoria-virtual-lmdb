# LMDB en este proyecto

Este documento aterriza la teoría de
[02-que-es-lmdb-y-memoria-virtual.md](02-que-es-lmdb-y-memoria-virtual.md) en
las decisiones concretas del código: cómo se estructura el entorno, cómo se
codifican las claves, cómo se abren las sub-bases, cómo se fuerza la caché fría,
cómo se miden los fallos de página y qué demuestran los benchmarks.

## 1. Estructura del entorno: un archivo, cinco sub-bases

El generador abre el entorno con el flag **`MDB_NOSUBDIR`**
(`src/generator.cpp`, función `open_env`):

```cpp
LMDB_CHECK(mdb_env_open(db.env, p.out.c_str(), MDB_NOSUBDIR, 0664));
```

`MDB_NOSUBDIR` significa que LMDB usa **un solo archivo** en vez de crear un
directorio con `data.mdb` y `lock.mdb`. Así, `data/smoke.lmdb` es un único
archivo, acompañado de un `data/smoke.lmdb-lock` para el bloqueo entre procesos.
Dentro de ese único archivo viven **cinco sub-bases con nombre**, abiertas con
`mdb_dbi_open` (`src/env.h`, `abrir_subbases`, y `open_env` en el generador).

La base `bets` es el registro maestro; las otras cuatro son índices y sorteos.
El tope de sub-bases se declara con `mdb_env_set_maxdbs(env, kMaxSubDbs)` con
`kMaxSubDbs = 8` (`src/types.h`).

| Base | Propósito | Clave | Valor |
| --- | --- | --- | --- |
| `bets` | registro maestro de la jugada (claves ascendentes → `MDB_APPEND`) | 13 B: lotería u8, ts u64, taquilla u16, seq u16 | 16 B: tipo, selecciones, monto |
| `i_animalito` | jugadas donde aparece cada animalito (frecuencia y rachas) | 16 B: lotería u8, animalito u8, ts u64, taquilla u16, seq u16 | 8 B: monto |
| `i_terminal` | frecuencia de terminales (00–99) | 16 B: lotería u8, terminal u8, ts u64, taquilla u16, seq u16 | 8 B: monto |
| `i_taquilla` | replay/sincronización por punto de venta | 16 B: taquilla u16, ts u64, lotería u8, seq u16 | 8 B: monto |
| `draws` | ganadores por lotería/día/sorteo | 6 B: lotería u8, día u32, sorteo u8 | 2 B: animalito, número |

Dos matices de tamaño que conviene saber (están en `src/keys.h`):

- Las claves de índice miden 16 B pero sus campos suman menos: en
  `encode_index_key` los campos ocupan 14 B y los 2 últimos quedan en cero
  (relleno); en `encode_taquilla_key` ocupan 13 B y quedan 3 en cero. El relleno
  en cero garantiza que entradas iguales produzcan claves iguales.
- El valor de `bets` mide 16 B: tipo (1) + selecciones (3) + monto en
  little-endian (8) + 4 bytes reservados en cero.

## 2. El esquema de claves, byte a byte

Regla crítica de `src/keys.h`: **todo entero multi-byte dentro de una CLAVE va
en big-endian**, porque LMDB compara claves con `memcmp`. Los **valores** van en
little-endian (nunca se comparan).

Tomemos la jugada de ejemplo del README: `lottery 0`, `ts 1577838370`,
`taquilla 33`, `seq 0`. Las primitivas relevantes son `put_be64` y `put_be16`.
La clave de `bets` que produce `encode_bets_key` se ve así:

```text
campo        valor         bytes (hex)
lotería      0             00
ts           1577838370    00 00 00 00 5e 0b e7 22
taquilla     33            00 21
seq          0             00 00
------------------------------------------------
clave bets (13 bytes):  00 00 00 00 00 5e 0b e7 22 00 21 00 00
```

Detalles: `1577838370` en hexadecimal es `0x5E0BE722`; como es big-endian, ese
es también el orden de sus bytes. La taquilla 33 es `0x0021`. Ese `ts`
corresponde a 1.570 segundos después de la época (2020-01-01 00:26:10 UTC).

Si la jugada fuera un animalito con `term = 5`, su clave en `i_animalito`
(16 B, con relleno final en cero) sería:

```text
lotería  animalito  ts (BE)                  taquilla  seq   relleno
00       05         00 00 00 00 5e 0b e7 22  00 21     00 00 00 00
```

Y en `i_taquilla`, la clave empieza por la taquilla:

```text
taquilla  ts (BE)                  lotería  seq   relleno
00 21     00 00 00 00 5e 0b e7 22  00       00 00 00 00 00
```

### Por qué el big-endian hace que un `SET_RANGE` funcione

La consulta `range` (`src/query.cpp`, `cmd_range`) construye la clave de
búsqueda así:

```cpp
auto lo = keys::encode_bets_key(lottery, from, 0, 0);
```

Es decir, la clave más pequeña posible con `ts >= from`: `lotería`, luego
`from` en big-endian, luego taquilla y seq en cero. El cursor hace
`MDB_SET_RANGE` (primera clave mayor o igual) y avanza mientras
`bk.lottery == lottery` y `bk.ts <= to`.

Como el byte más significativo de `ts` va primero, el orden lexicográfico de los
bytes coincide con el orden numérico de las fechas. Por eso el cursor aterriza
**exactamente** en la primera jugada con `ts >= from`, y el barrido devuelve
todas las jugadas de esa lotería dentro del rango. Como `bets` guarda **todas**
las jugadas (animalitos, terminales y tripletas), ese rango cubre a todos los
animalitos de esas fechas sin distinción.

Si el `ts` fuera little-endian, el byte menos significativo iría primero y la
comparación de bytes no respetaría el orden de las fechas: el `SET_RANGE`
saltaría a un punto equivocado. El big-endian es lo que evita ese fallo
silencioso. La auto-prueba `self_test_key_order` del generador verifica
precisamente que las marcas de tiempo no desciendan en el orden de claves.

## 3. `abrir_subbases` y el gotcha de las transacciones de lectura

`src/env.h` documenta un comportamiento de esta build de LMDB:

> los manejadores de sub-base abiertos en una transacción de lectura dejan de
> ser válidos cuando esa transacción se aborta (verificado empíricamente:
> `mdb_stat` devuelve `EINVAL`).

Esto obliga a un patrón: **no** abrir las sub-bases una vez al principio del
programa y reutilizarlas después. En su lugar, cada comando abre las sub-bases
**dentro de la misma transacción que las usa**, y esa transacción vive todo lo
que dure la consulta. El patrón exacto, repetido en todos los comandos de
`query.cpp`, es:

```cpp
MDB_txn* txn = nullptr;
LMDB_CHECK(mdb_txn_begin(db.env, nullptr, MDB_RDONLY, &txn));
abrir_subbases(txn, db);   // los handles solo valen mientras viva esta txn
// ... uso de db.bets, db.i_animalito, etc. ...
mdb_txn_abort(txn);        // abort al terminar: los handles quedan inválidos
```

Y en `bench.cpp` el mismo patrón se encapsula en `preparar_lectura`, que hace
`open_readonly` + `mdb_txn_begin` + `abrir_subbases` en una sola llamada.

Nota: las transacciones de **lectura** se cierran con `mdb_txn_abort`, no con
`mdb_txn_commit` (no hay nada que confirmar; el código lo comenta en
`write_manifest`: "las transacciones de lectura se cierran con abort").

El generador es la excepción: tiene su propio `open_env` y su propio
`DbHandles`, y como abre un entorno **de escritura** no sufre este problema. Por
eso `generator.cpp` no incluye `env.h`.

## 4. Los tres binarios

El proyecto produce tres ejecutables (`bin/generator`, `bin/query`, `bin/bench`)
a partir de tres `.cpp`. Comparten los headers `types.h`, `keys.h` y `env.h`.

| Binario | Fase | Qué produce / hace |
| --- | --- | --- |
| `bin/generator` | 1 | Crea el dataset LMDB determinista y su `<dataset>.manifest.json`. |
| `bin/query` | 3 | 8 comandos de solo lectura y la validación contra el manifiesto (149/149). |
| `bin/bench` | 2, 4, 5 | Benchmarks de escritura, lectura y escritura concurrente; mide fallos de página. |

### `bin/generator`

Parámetros (definidos en `parse_args`): `--bets`, `--years`, `--taquillas`,
`--seed`, `--out`, `--map-gb`, `--batch-txn`, `--help`.

```sh
./bin/generator --bets 1000000 --out data/smoke.lmdb
```

Se niega a pisar un `--out` existente ("me niego a mezclar datasets"). Escribe
junto al dataset `<nombre>.manifest.json` con los conteos exactos, las
frecuencias por animalito y terminal, las estadísticas del B+tree de cada
sub-base y el tamaño del archivo.

### `bin/query`

Todos los comandos requieren `--db RUTA`. Si no se indica comando, usa `info`.

| Comando | Flags | Qué hace |
| --- | --- | --- |
| `info` | — | Estadísticas del entorno y de cada sub-base. |
| `lookup` | `--lottery --ts --taquilla [--seq]` | Búsqueda puntual exacta en `bets`. |
| `range` | `--lottery --from --to` | Barrido por rango de fechas en `bets`. |
| `animalito-stats` | `--lottery [--animal]` | Frecuencia y monto por animalito (índice). |
| `terminal-stats` | `--lottery` | Frecuencia y monto por terminal (índice). |
| `racha` | `--lottery --animal` | Cuántos días lleva el animalito sin salir (`draws`). |
| `taquilla` | `--taquilla` | Replay de un punto de venta (índice). |
| `validate` | `[--manifest]` | Valida todos los agregados contra el manifiesto del dataset. |

Ejemplos:

```sh
./bin/query --db data/smoke.lmdb info
./bin/query --db data/smoke.lmdb lookup --lottery 0 --ts 1577838370 --taquilla 33 --seq 0
./bin/query --db data/smoke.lmdb range --lottery 0 --from 1577836800 --to 1578441600
./bin/query --db data/smoke.lmdb animalito-stats --lottery 0
./bin/query --db data/smoke.lmdb terminal-stats --lottery 0
./bin/query --db data/smoke.lmdb racha --lottery 0 --animal 5
./bin/query --db data/smoke.lmdb taquilla --taquilla 7
./bin/query --db data/smoke.lmdb validate
```

`validate` busca el manifiesto en `<mismo directorio>/<nombre>.manifest.json`
salvo que se indique `--manifest`. La validación recorre las sub-bases y
compara cada agregado (totales por lotería, por tipo, por animalito, por
terminal) contra el manifiesto. El resultado medido es **149/149**.

### `bin/bench`

Modos de escritura: `write-append`, `write-random`, `write-concurrent`. Modos de
lectura: `read-lookup`, `read-scan`, `read-index`. Los de lectura requieren
`--db`; los de escritura requieren `--out` (y `--force` si el archivo existe).

```sh
# Escritura
./bin/bench write-append     --records 1000000 --batch 50000 --out data/bench_append.lmdb
./bin/bench write-random     --records 1000000 --batch 50000 --out data/bench_random.lmdb
./bin/bench write-concurrent --taquillas 16 --records 500000 --out data/bench_16t.lmdb --force

# Lectura (tibio y frío)
./bin/bench read-lookup --db data/smoke.lmdb --n 200000
./bin/bench read-lookup --db data/smoke.lmdb --n 200000 --cold
./bin/bench read-scan   --db data/smoke.lmdb --lottery 0
./bin/bench read-scan   --db data/smoke.lmdb --lottery 0 --cold
./bin/bench read-index  --db data/smoke.lmdb
./bin/bench read-index  --db data/smoke.lmdb --cold
```

Otras opciones de escritura: `--map-gb`, `--seed`. De lectura: `--n`
(lookups, por defecto 200000), `--lottery` (para `read-scan`, por defecto 0) y
`--cold`. `--help` imprime el uso completo.

## 5. La técnica de caché fría del proyecto

Para medir la diferencia entre "todo en RAM" y "hay que ir a disco", el proyecto
necesita **vaciar el caché de páginas** antes de una corrida fría. La forma
clásica en Linux escribe en `/proc/sys/vm/drop_caches`, pero eso **requiere
privilegios de root**, y una demo de proyecto no debería depender de sudo.

La alternativa implementada es `soltar_cache`
(`src/bench.cpp`): sin privilegios, combinando dos mecanismos.

```cpp
void soltar_cache(MDB_env* env, const std::string& db_path) {
  // 1. Buscar la región mmap del archivo en /proc/self/maps y aplicar madvise.
  //    ... sscanf(line.c_str(), "%lx-%lx", &start, &end) ...
  //    madvise(reinterpret_cast<void*>(start), len, MADV_DONTNEED)
  // 2. posix_fadvise como refuerzo.
  //    mdb_env_get_fd(env, &fd); posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}
```

Cómo funciona, paso a paso:

1. Abre `/proc/self/maps`, que lista las regiones de memoria del propio proceso,
   y busca la línea que corresponde al archivo del dataset (descartando el
   archivo `-lock`). De esa línea extrae la dirección inicial y final del mapeo.
2. Aplica `madvise(addr, len, MADV_DONTNEED)` sobre esa región. Esto le dice al
   kernel que las páginas de esa región ya no son necesarias: se invalidan las
   entradas del proceso, de modo que el próximo acceso **vuelva a fallar** y
   tenga que traer la página de nuevo.
3. Como refuerzo, obtiene el descriptor de archivo del entorno con
   `mdb_env_get_fd` y llama a `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)`,
   que aconseja al kernel descartar del caché de páginas las páginas de ese
   archivo en todo su rango (`0, 0`).

Por qué **`posix_fadvise` solo no basta**: el `fadvise` actúa sobre la caché de
páginas del kernel, pero no destruye el mapeo del propio proceso. El proceso
podría seguir accediendo a páginas que ya tiene mapeadas en su tabla de páginas.
Hace falta el `madvise(MADV_DONTNEED)` para invalidar el mapeo del proceso y
forzar que el siguiente acceso sea un fallo de página real. Los dos juntos
vacían tanto la caché del sistema como las traducciones del proceso.

Después de soltar el caché, cada modo de lectura aplica además un hint de
`madvise` según su patrón de acceso (`aplicar_hint_madvise`):

- `read-lookup`, patrón aleatorio → `MADV_RANDOM`, que desactiva la relectura
  anticipada (no tiene sentido traer la página siguiente si vas a saltar).
- `read-scan` y `read-index`, patrón secuencial → `MADV_SEQUENTIAL`, que amplía
  la relectura anticipada.

## 6. Cómo se miden los fallos de página

Cada corrida de lectura mide los fallos de página del proceso leyendo
`/proc/self/stat`, en la función `leer_fallos`. Ese archivo tiene los campos del
proceso separados por espacios; el segundo campo (el nombre del comando) va
entre paréntesis y puede contener espacios, así que el código busca el **último**
`)` y tokeniza desde ahí.

Los campos relevantes, contando tokens desde esa posición (índice 0):

- token 7 = **minflt**: fallos de página **menores** (la página ya estaba en
  memoria).
- token 9 = **majflt**: fallos de página **mayores** (hubo que leer del
  dispositivo).

```cpp
while (iss >> tok) {
  if (idx == 7) f.minflt = std::stoull(tok);
  if (idx == 9) f.majflt = std::stoull(tok);
  ++idx;
}
```

La medición es una **diferencia**: se llama a `leer_fallos()` antes (`antes`) y
después (`despues`) de la corrida, y se reporta
`despues.minflt - antes.minflt` y `despues.majflt - antes.majflt`. Así se aísla
lo ocurrido durante la medición y no lo que el proceso hizo al arrancar. La
diferencia tibio/frío de los fallos mayores es, literalmente, el costo de la
paginación por demanda.

## 7. Modos de benchmark y qué demuestra cada uno

### 7.1 `write-append` vs. `write-random` (Fase 2)

Ambos escriben 1 M de registros con lotes de 50 k. `write-append` usa claves
ascendentes y `MDB_APPEND`; `write-random` usa claves aleatorias y puts normales.

| Métrica | Append (ordenado) | Aleatorio |
| --- | --- | --- |
| Rendimiento | 1,94 M registros/s · 192 MB/s | 226 k registros/s · 22,4 MB/s |
| Commit p50 | 11,6 ms | 101 ms |
| Tamaño final | 118 MB | 398 MB |

Ordenar las claves vale **×8,6 de rendimiento** y **×3,4 de espacio**. El modo
aleatorio hace que casi cada inserción descienda por el B+tree y provoque
páginas sucias dispersas, lo que fragmenta el archivo. Demuestra por qué el
generador ordena las claves antes de escribir.

### 7.2 `write-concurrent` (Fase 5)

500 K registros por taquilla, lotes de 50 K, N taquillas en paralelo.

| Taquillas | Total | Throughput | Commit p50 | Degradación |
| --- | --- | --- | --- | --- |
| 1 | 500K | 1.342.000 reg/s | 11,2 ms | — |
| 4 | 2M | 1.104.000 reg/s | 21,8 ms | ×0,82 |
| 8 | 4M | 1.004.000 reg/s | 24,3 ms | ×0,75 |
| 16 | 8M | 943.000 reg/s | 26,3 ms | ×0,70 |

Demuestra el **único escritor** de LMDB: añadir taquillas no multiplica el
throughput porque todos los hilos compiten por el mismo escritor (el código usa
un `std::mutex` compartido, coherente con la restricción de LMDB).

### 7.3 Lecturas (Fase 4)

Dataset de 1 M de jugadas (≈ 180 MB), tibio vs. frío con `madvise`.

| Modo | Tibio ops/s | Frío ops/s | Factor | Frío majflt | Frío p99 |
| --- | --- | --- | --- | --- | --- |
| `read-lookup` (200k) | 1.156.332 | 314.544 | ×3,7 | 68 | 4,3 µs |
| `read-scan` (167k) | 8.428.337 | 1.150.678 | ×7,3 | 11 | 0,078 µs |
| `read-index` (900k) | 1.866.943 | 1.499.141 | ×1,2 | 76 | 0,072 µs |

Lectura sobre el dataset de **10 M (1,57 GB)**: el lookup en frío cae a
**6.068 ops/s** con **83.423 fallos mayores** (unas **×151** respecto del
tibio), mientras que el barrido apenas cae ×1,8. La lección: cuando el dataset
**no cabe en RAM**, el patrón de acceso aleatorio es demoledor y el secuencial
sobrevive gracias a la localidad.

### 7.4 Prueba de humo y validación

El dataset de humo (1 M de jugadas) da: 1.000.000 jugadas exactas, 37.230
sorteos (6 loterías × 3 años), 180 MB en disco (≈ **180 B por jugada**,
incluyendo el B+tree), auto-prueba de orden de claves **APROBADA**,
determinismo verificado (mismo archivo byte a byte al regenerar) y validación
contra el manifiesto **149/149**. Para dimensionar: 10 M ≈ 1,8 GB, 50 M ≈ 9 GB,
**100 M ≈ 18 GB**, 500 M ≈ 90 GB.

## 8. Aplicación al negocio real (taquillas)

El benchmark `write-concurrent` no es un ejercicio abstracto: modela el caso
real de varias taquillas registrando jugadas. Y confirma una restricción dura de
LMDB: **un solo escritor**. Con 16 taquillas el throughput cae a ×0,70 y el
commit p50 casi se duplica. En un negocio con muchas taquillas, esto importa.

Estrategias recomendadas para escalar (todas se apoyan en lo medido):

- **Particionar por tiempo o por taquilla.** Como LMDB permite un escritor
  **por entorno**, se pueden usar varios entornos independientes (por ejemplo,
  uno por taquilla o uno por ventana de tiempo). Cada partición tiene su propio
  escritor y dejan de competir entre sí. Es la forma de recuperar el
  paralelismo que un solo entorno no da.
- **Agrupar escrituras en lotes.** Cada `commit` paga un `fsync` (durabilidad).
  Lotes más grandes (`--batch`) amortizan ese costo fijo entre más registros, y
  reducen el número de commits.
- **Separar lecturas de escrituras.** Gracias a MVCC, los lectores no bloquean al
  escritor ni al revés. Una taquilla puede consultar mientras otra escribe; lo
  que conviene es no mezclar cargas pesadas de escritura con barridos largos en
  el mismo proceso.
- **Precalcular agregados.** En lugar de recorrer `bets` para responder
  "frecuencia por animalito", el proyecto ya mantiene índices (`i_animalito`,
  `i_terminal`) con el monto por entrada. Materializar los agregados que se
  consultan mucho convierte scans O(N) en recorridos de índice acotados.

## 9. Preguntas de defensa

**¿Por qué las claves de índice miden 16 bytes si sus campos suman menos?**
Porque el diseño reserva bytes de relleno en cero para que las claves sean
deterministas y de tamaño fijo. En `encode_index_key` sobran 2 bytes y en
`encode_taquilla_key` sobran 3; todos se dejan en cero.

**¿Por qué la base `bets` usa `MDB_APPEND` y los índices no?**
Porque el generador produce las claves de `bets` en orden estrictamente
ascendente (recorre las loterías y, dentro de cada una, el tiempo), lo que
permite insertar en el borde derecho del B+tree. Las claves de los índices se
agrupan por animalito/terminal/taquilla y **no** quedan globalmente ordenadas,
así que se escriben con puts normales.

**¿Qué pasa si se aborta la transacción de lectura donde abriste las sub-bases?**
En esta build de LMDB los manejadores dejan de ser válidos (por ejemplo,
`mdb_stat` devuelve `EINVAL`). Por eso cada comando abre las sub-bases dentro de
la misma transacción que las usa y no las conserva después del abort.

**¿Por qué la caché fría no usa `drop_caches`?**
Porque `echo 3 > /proc/sys/vm/drop_caches` requiere root. El proyecto usa
`madvise(MADV_DONTNEED)` sobre la región `mmap` del archivo más
`posix_fadvise(POSIX_FADV_DONTNEED)` sobre el descriptor, que no necesita
privilegios.

**¿Por qué no basta `posix_fadvise` para enfriar la caché?**
Porque `fadvise` actúa sobre la caché de páginas del kernel, pero no invalida el
mapeo del proceso. Hace falta `madvise(MADV_DONTNEED)` para que el proceso deje
de tener esas páginas mapeadas y el próximo acceso sea un fallo de página real.

**¿Cómo se miden los fallos de página y qué diferencia hay entre menores y
mayores?** Se leen los tokens 7 (`minflt`) y 9 (`majflt`) de `/proc/self/stat`
antes y después de cada corrida. Los menores son páginas que ya estaban en
memoria; los mayores implican E/S al dispositivo y son los caros.

**¿Por qué el `write-concurrent` se degrada si hay más hilos?**
Porque LMDB permite un único escritor por entorno. Los hilos compiten por ese
escritor (el benchmark los serializa con un mutex) y el throughput total cae a
×0,70 con 16 taquillas en lugar de crecer.

**¿Por qué el barrido frío aguanta mejor que el lookup frío?**
Por localidad espacial: un fallo de página trae 4 KB (una página) llena de
jugadas contiguas, así que el barrido amortiza un fallo entre muchas lecturas.
El lookup aleatorio salta entre páginas distintas y casi cada consulta paga su
propio fallo (y desciende por varios niveles del B+tree).

**¿`read-index` sale casi igual en frío que en tibio; no contradice la teoría?**
No: el índice `i_animalito` con 1 M de jugadas ocupa mucho menos que `bets`, así
que cabe mejor en RAM y se expulsa menos. El factor medido es ×1,2, coherente
con que el conjunto de trabajo es más pequeño.

**¿Qué demuestra la validación 149/149?**
Que las consultas de agregados del proyecto (totales por lotería, tipo,
animalito y terminal) coinciden exactamente con los contadores que el generador
guardó en el manifiesto. Es la prueba de corrección de la capa de consultas.
